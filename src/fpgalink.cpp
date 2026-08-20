// fpgalink.cpp — see fpgalink.h. The FPGA->ESP link on the Debug pin.
// (C) 2026 Valere Pillet / Pstore. Original implementation.
#include "fpgalink.h"
#include "board_config.h"
#include <Arduino.h>
#include <string.h>
#include "xvc.h"
#ifndef BOARD_C3
#include "wavplayer.h"
#endif

namespace {
  HardwareSerial& port = Serial1;     // dedicated UART for the FPGA link (Debug pin)
  bool g_diag = false;                // last diag-mode token (touched only from poll())
  bool g_gameRunning = false;         // last game-state token (0xF2/0xF3) — tournament auto-timer
  // --- bring-up counters: prove the GPIO8 link is alive (no behaviour change) ---
  uint32_t g_rxCount = 0;             // total bytes ever received on the link
  uint8_t  g_lastByte = 0;            // most recent raw byte
  uint32_t g_lastMs = 0;             // millis() of the most recent byte
  // --- selected game number (token 0x40|game[5:0]) ---------------------------------
  // CHANGE-DRIVEN, NOT a heartbeat: sound_link.vhd loads game_r from the live `game` input
  // on reset and only queues a token when `game /= game_r` afterwards (the 50 ms heartbeat
  // re-announces the mode and game-state tokens ONLY). So this can legitimately stay -1 for
  // ever on a board whose game_select was already stable when reset was released. Anything
  // keying persistent per-title data off it MUST handle -1 instead of assuming a game 0.
  int8_t   g_gameNo = -1;             // last 0x40|game token, -1 = never received
  uint32_t g_gameMs = 0;              // millis() of that token

  // --- game in progress: the FPGA watches RIOT RAM $0072 and emits 0xA0|value on change ---
  uint8_t  g_ball = 0;                // last $0072 value received (0..15)
  uint32_t g_ballCount = 0;           // how many 0xA0 tokens have been seen
  uint32_t g_ballMs = 0;              // millis() of the most recent 0xA0 token
  // Debounced game-in-progress state. RAM index 0x72 = CPU $0072 (RIOT U4): 0 attract, 1 game.
  // Two independent feeds (the 0xA0 change token, and every RAM snapshot) both call gipObserve();
  // gipSettle() only publishes a value that has held for GIP_DEBOUNCE_MS, so one corrupt byte
  // cannot start a tournament game or cut one short.
  const uint32_t GIP_DEBOUNCE_MS = 300;
  const uint16_t GIP_RAM_IDX = 0x72;  // snapshot index of CPU $0072
  bool     g_gip = false;             // published (debounced) state
  bool     g_gipRaw = false;          // last observed raw state
  uint32_t g_gipRawMs = 0;            // millis() when g_gipRaw last changed
  // --- live sound commands (0x80|cmd) -> FIFO for the time-attack bonus -------------
  const uint8_t SND_Q = 16;           // power of two; overwrite-oldest (a stalled reader
  uint8_t  g_sndQ[SND_Q] = {0};       // must not stall the link)
  uint8_t  g_sndW = 0, g_sndR = 0;
  uint32_t g_relCount = 0, g_relMs = 0;   // 0x30 sound-bus-release events
  uint32_t g_lostCount = 0;               // 0x31 "FPGA sound FIFO overflowed" reports
  // --- RAM snapshot: 0xBF marker then 768 payload bytes (384 values x hi/lo nibble) ---
  const uint16_t SNAP_BYTES = 2 * fpgalink::RAMSNAP_N;   // 768 payload bytes per frame
  // Double buffer: poll() fills [g_snapPub ^ 1] and only flips g_snapPub once the frame
  // is complete, so a reader (the async HTTP task, another FreeRTOS task) never sees a
  // half-written frame. A partial/aborted frame is simply never published.
  uint8_t  g_snapBuf[2][fpgalink::RAMSNAP_N] = {{0}};
  volatile uint8_t g_snapPub = 0;     // index of the buffer holding the last complete frame
  bool     g_snapping = false;        // true while collecting the 768 payload bytes
  uint16_t g_snapIdx = 0;             // payload byte index within the frame (0..767)
  uint8_t  g_snapHi = 0;              // high nibble waiting for its low partner
  uint32_t g_snapFrames = 0;          // complete frames stored
  uint32_t g_snapBad = 0;             // frames aborted mid-capture (nibble-class desync)
  uint32_t g_snapMs = 0;              // millis() of the most recent complete frame

  // --- sound trace ring (see fpgalink.h + SOUND_WIRE.md) -----------------------------
  // Written by poll() (loop task), read by traceCopy() (async HTTP task). A spinlock, not a
  // double buffer: entries are 8 bytes and the writer holds the lock for a single store, so
  // the copy can never see a torn entry and the 128-byte UART hardware FIFO (11 ms at 115200)
  // absorbs the ~40 us the 4 KB copy holds it.
  portMUX_TYPE g_trMux = portMUX_INITIALIZER_UNLOCKED;
  fpgalink::TraceEv g_tr[fpgalink::TRACE_N];
  uint16_t g_trW = 0;                 // next write slot
  uint16_t g_trN = 0;                 // entries currently held (<= TRACE_N)
  bool     g_trRaw = false;           // false = filtered (default), true = every byte
  uint32_t g_trKept = 0, g_trElided = 0, g_trPayload = 0;
  // token classes, for the level-repeat filter and for traceDecode()
  enum TrCls : uint8_t { CL_UNK = 0, CL_SMETA, CL_GAME, CL_SND, CL_BALL, CL_RXC,
                         CL_SNAP, CL_SNAPD, CL_DINJ, CL_MODE, CL_STATE, CL_N };
  uint8_t g_trLast[CL_N] = {0};       // last byte recorded per class
  bool    g_trSeen[CL_N] = {false};

  // Classify a raw byte. Order matters exactly as on the wire: the snapshot marker 0xBF is
  // an EXACT byte and must be tested before (b & 0xF0) == 0xB0 (rx count), else rx_cnt 15
  // would shadow it — which is why disp_inject wraps its counter at 14 (sound_link.vhd).
  inline TrCls trClass(uint8_t b) {
    if (b == 0xBF)              return CL_SNAP;
    if ((b & 0xE0) == 0xC0)     return CL_SNAPD;      // 0xC0..0xDF payload
    if ((b & 0xFE) == 0xF0)     return CL_MODE;
    if ((b & 0xFE) == 0xF2)     return CL_STATE;
    if ((b & 0xF0) == 0xE0)     return CL_DINJ;
    if ((b & 0xF0) == 0xB0)     return CL_RXC;        // 0xB0..0xBE (0xBF taken above)
    if ((b & 0xF0) == 0xA0)     return CL_BALL;
    if ((b & 0xE0) == 0x80)     return CL_SND;
    if ((b & 0xC0) == 0x40)     return CL_GAME;
    if ((b & 0xF0) == 0x30)     return CL_SMETA;      // 0x30..0x3F sound meta
    return CL_UNK;
  }
  // Is this class a LEVEL report (carries its current value, so a repeat says nothing new)?
  inline bool trIsLevel(TrCls c) {
    return c == CL_MODE || c == CL_STATE || c == CL_DINJ || c == CL_RXC ||
           c == CL_BALL || c == CL_GAME;
  }

  inline void traceByte(uint8_t b, uint32_t now) {
    TrCls c = trClass(b);
    if (!g_trRaw) {
      if (c == CL_SNAPD) { g_trPayload++; return; }           // 769 bytes/s of bulk
      if (trIsLevel(c) && g_trSeen[c] && g_trLast[c] == b) { g_trElided++; return; }
    }
    g_trLast[c] = b; g_trSeen[c] = true;
    portENTER_CRITICAL(&g_trMux);
    g_tr[g_trW].ms = now; g_tr[g_trW].b = b;
    g_trW = (uint16_t)((g_trW + 1) % fpgalink::TRACE_N);
    if (g_trN < fpgalink::TRACE_N) g_trN++;
    portEXIT_CRITICAL(&g_trMux);
    g_trKept++;
  }

  inline void gipObserve(bool raw, uint32_t now) {      // a fresh $0072 reading
    if (raw != g_gipRaw) { g_gipRaw = raw; g_gipRawMs = now; }
  }
  inline void gipSettle(uint32_t now) {                 // publish once it has held long enough
    if (g_gip != g_gipRaw && (now - g_gipRawMs) >= GIP_DEBOUNCE_MS) g_gip = g_gipRaw;
  }
  inline void sndPush(uint8_t cmd) {
    g_sndQ[g_sndW] = cmd; g_sndW = (uint8_t)((g_sndW + 1) & (SND_Q - 1));
    if (g_sndW == g_sndR) g_sndR = (uint8_t)((g_sndR + 1) & (SND_Q - 1));   // full: drop the oldest
  }
}

namespace fpgalink {

void begin() {
  port.begin(115200, SERIAL_8N1, PIN_FPGA_LINK, -1);   // RX only (no TX pin)
}

// --- Collision proof: the snapshot bytes 0xBF / 0xC0..0xCF / 0xD0..0xDF match NONE of
//     the five existing token tests, so adding the frame cannot shadow (or be shadowed
//     by) diag mode, game state, ball, sound or game number.
//
//   byte        &0xFE       ==0xF0? ==0xF2? | &0xF0  ==0xA0? | &0xE0  ==0x80? | &0xC0  ==0x40?
//   0xBF        0xBE         no      no     | 0xB0    no     | 0xA0    no     | 0x80    no
//   0xC0..0xCF  0xC0..0xCE   no      no     | 0xC0    no     | 0xC0    no     | 0xC0    no
//   0xD0..0xDF  0xD0..0xDE   no      no     | 0xD0    no     | 0xC0    no     | 0xC0    no
//
//   For 0xC0..0xDF every test result depends only on the high nibble (masks 0xF0/0xE0/
//   0xC0 preserve it, and it is constant across each 16-byte class), so one representative
//   per class proves the whole class. The (b & 0xFE) tests can only yield 0xF0/0xF2 when
//   the high nibble is 0xF — ours are 0xB/0xC/0xD, so they are out by construction.
//   Conversely no existing token can be mistaken for snapshot data: the tokens occupy
//   0x40..0xAF and 0xF0..0xF3, none of which falls in 0xBF..0xDF.
//   Byte-space map after this change: 0x40-0x7F game | 0x80-0x9F sound | 0xA0-0xAF ball |
//   0xBF marker | 0xC0-0xCF hi nibble | 0xD0-0xDF lo nibble | 0xF0-0xF3 diag/state.
static_assert((0xBF & 0xFE) != 0xF0 && (0xBF & 0xFE) != 0xF2 && (0xBF & 0xF0) != 0xA0 &&
              (0xBF & 0xE0) != 0x80 && (0xBF & 0xC0) != 0x40, "0xBF collides with a token");
static_assert((0xC0 >> 4) != 0xF && (0xC0 & 0xF0) != 0xA0 &&
              (0xC0 & 0xE0) != 0x80 && (0xC0 & 0xC0) != 0x40, "0xC0..0xCF collides with a token");
static_assert((0xD0 >> 4) != 0xF && (0xD0 & 0xF0) != 0xA0 &&
              (0xD0 & 0xE0) != 0x80 && (0xD0 & 0xC0) != 0x40, "0xD0..0xDF collides with a token");

void poll() {
  // During an XVC session the FPGA is being reconfigured: the Debug pin floats
  // and the UART sees garbage. A stray byte decoding as 0xF1 would set g_diag
  // -> diag::tick() grabs the shared SPI bus -> the freshly-configured FPGA
  // can't read its SD game ROM and never boots (ground-truthed 2026-07-09).
  // Discard everything and force diag off until the session ends.
  static uint32_t xvcGraceMs = 0;
  if (xvc::active()) xvcGraceMs = millis();
  if (xvcGraceMs && (millis() - xvcGraceMs) < 1000) {   // active + 1 s grace (FIFO leftovers)
    while (port.available()) port.read();
    g_diag = false;
    g_snapping = false;                                 // drop any half-captured frame too
    g_gip = g_gipRaw = false;                           // FPGA being reconfigured: no game
    return;
  }
  gipSettle(millis());                                  // let a pending $0072 change mature
  while (port.available()) {
    uint8_t b = (uint8_t)port.read();
    g_rxCount++; g_lastByte = b; g_lastMs = millis();   // bring-up telemetry
    traceByte(b, g_lastMs);                             // instrument: one compare + one store

    // --- RAM snapshot capture ------------------------------------------------------
    // Payload bytes (0xC0..0xDF) are consumed HERE and never reach the token decoder
    // below, so a 769-byte frame cannot fire a sound, change the game or flip diag mode.
    // They alternate strictly high (0xC0|hi) / low (0xD0|lo).
    //
    // Status tokens DO interleave mid-frame and that is normal, not an error: the FPGA
    // arbiter serves them ahead of the snapshot stream and the 50 ms heartbeat lands
    // 2-3 of them inside every 73 ms frame. So a non-payload byte is decoded normally
    // and the capture continues — aborting on it would drop 100% of frames.
    if (g_snapping) {
      if ((b & 0xE0) == 0xC0) {                         // a payload byte
        bool okByte;
        if ((g_snapIdx & 1) == 0) {                     // even index: expect 0xC0|hi
          okByte = ((b & 0xF0) == 0xC0);
          if (okByte) g_snapHi = (uint8_t)((b & 0x0F) << 4);
        } else {                                        // odd index: expect 0xD0|lo
          okByte = ((b & 0xF0) == 0xD0);
          if (okByte) g_snapBuf[g_snapPub ^ 1][g_snapIdx >> 1] = (uint8_t)(g_snapHi | (b & 0x0F));
        }
        if (okByte && ++g_snapIdx >= SNAP_BYTES) {      // 768 payload bytes -> publish
          g_snapPub ^= 1;
          g_snapFrames++; g_snapMs = millis(); g_snapping = false;
          // ~1 Hz re-sync of the game-state flag straight from RAM: even if a 0xA0 change
          // token were lost, the snapshot puts $0072 back in agreement within a second.
          gipObserve(g_snapBuf[g_snapPub][GIP_RAM_IDX] != 0, g_snapMs);
        } else if (!okByte) {
          g_snapping = false; g_snapBad++;              // wrong nibble class = real desync
        }
        continue;                                       // payload never reaches the decoder
      }
      if (b == 0xBF) { g_snapBad++; g_snapIdx = 0; continue; }   // marker mid-frame: restart
      // anything else: an interleaved status token -> fall through, keep capturing
    }
    if (b == 0xBF) { g_snapping = true; g_snapIdx = 0; continue; }   // snapshot frame marker

    if ((b & 0xFE) == 0xF0) {                 // diag-mode token: 0xF0 normal / 0xF1 diag
      g_diag = (b & 0x01) != 0;
    }
    else if ((b & 0xFE) == 0xF2) {            // game-state: 0xF2 over / 0xF3 running (tournament timer)
      g_gameRunning = (b & 0x01) != 0;
    }
    // ball in play 0xA0..0xAF — disjoint from the sound test below ((0xA0 & 0xE0) == 0xA0,
    // not 0x80) and from the game-number test ((0xA0 & 0xC0) == 0x80, not 0x40), so this
    // branch shadows nothing and nothing shadows it. Kept outside the BOARD_C3 guard: it
    // is pure telemetry, no wavplayer involved, and /link is served on both tiers.
    else if ((b & 0xF0) == 0xA0) {            // 0xA0|value: RIOT RAM $0072 changed
      g_ball = b & 0x0F; g_ballCount++; g_ballMs = millis();
      gipObserve(g_ball != 0, g_ballMs);      // $0072: 0 = attract, 1 = game in progress
    }
    else if ((b & 0xE0) == 0x80) {            // sound command 0x80..0x9F
      uint8_t c = (uint8_t)(b & 0x1F);
      sndPush(c);                             // time-attack bonus feed (both tiers)
#ifndef BOARD_C3
      wavplayer::playLive(c);                 // hybrid-aware: skips cmds GOSOF80 synthesises
#endif
    }
    // --- sound META 0x30..0x3F (SOUND_WIRE.md). Not emitted by today's sound_link.vhd; decoded
    // now so the FPGA side can be upgraded without a firmware change, and so a stray 0x3x can
    // never be mistaken for a playable command. Disjoint from every test above: (0x3x & 0xC0)
    // = 0x00, so it reaches this branch only.
    else if ((b & 0xF0) == 0x30) {
      if (b == 0x30) {                        // sound bus released: no command selected
        g_relCount++; g_relMs = millis();
#ifndef BOARD_C3
        wavplayer::soundRelease();            // sound.map decides: ignore (80B) or stop (System 80)
#endif
      } else if (b == 0x31) {                 // the FPGA sound FIFO overflowed: >=1 cue lost
        g_lostCount++;
      }
    }
    else if ((b & 0xC0) == 0x40) {            // game number 0x40..0x7F -> games.txt -> set
      g_gameNo = (int8_t)(b & 0x3F);          // both tiers: keyed per-title data needs it too
      g_gameMs = millis();
#ifndef BOARD_C3
      wavplayer::selectGame(b & 0x3F);        // No = GottFA80_PLuS gamelist index
#endif
    }
  }
  gipSettle(millis());                        // publish a change seen in this drain
}

bool diagActive() { return g_diag; }
bool gameRunning() { return g_gameRunning; }   // "CPU booted" latch — see fpgalink.h
bool gameInProgress() { return g_gip; }        // RAM $0072, debounced — the real game state

// Pop one live sound command (0x80|cmd -> 0..31). FIFO, so a burst of cues between two callers'
// polls is not collapsed into one; empty -> false.
bool popSound(uint8_t& cmd) {
  if (g_sndR == g_sndW) return false;
  cmd = g_sndQ[g_sndR]; g_sndR = (uint8_t)((g_sndR + 1) & (SND_Q - 1));
  return true;
}

// Bring-up telemetry: total bytes seen, last raw byte, ms since last byte.
void stats(uint32_t& total, uint8_t& last, uint32_t& ageMs) {
  total = g_rxCount; last = g_lastByte;
  ageMs = g_rxCount ? (millis() - g_lastMs) : 0xFFFFFFFF;
}

// Selected game number, or -1 if the FPGA has never announced one (see g_gameNo).
int  gameNo()      { return g_gameNo; }
uint32_t gameAgeMs(){ return (g_gameNo < 0) ? 0xFFFFFFFFu : (millis() - g_gameMs); }

// Ball in play: last value, number of 0xA0|value tokens seen, ms since the last one.
void ballStats(uint8_t& value, uint32_t& count, uint32_t& ageMs) {
  value = g_ball; count = g_ballCount;
  ageMs = g_ballCount ? (millis() - g_ballMs) : 0xFFFFFFFF;
}

// --- sound trace ring -------------------------------------------------------------------
void traceMode(bool raw) { g_trRaw = raw; }
bool traceRaw()          { return g_trRaw; }

void traceClear() {
  portENTER_CRITICAL(&g_trMux);
  g_trW = 0; g_trN = 0;
  portEXIT_CRITICAL(&g_trMux);
  g_trKept = g_trElided = g_trPayload = 0;
  for (int i = 0; i < CL_N; i++) g_trSeen[i] = false;
}

// Oldest-first copy. The ring is (g_trW - g_trN) .. (g_trW - 1) modulo TRACE_N.
uint16_t traceCopy(TraceEv* out, uint16_t max) {
  if (!out || !max) return 0;
  portENTER_CRITICAL(&g_trMux);
  uint16_t n = g_trN, w = g_trW;
  if (n > max) n = max;                                   // keep the NEWEST `max` entries
  uint16_t start = (uint16_t)((w + TRACE_N - n) % TRACE_N);
  for (uint16_t i = 0; i < n; i++) out[i] = g_tr[(start + i) % TRACE_N];
  portEXIT_CRITICAL(&g_trMux);
  return n;
}

void traceStats(uint32_t& kept, uint32_t& elided, uint32_t& payload) {
  kept = g_trKept; elided = g_trElided; payload = g_trPayload;
}

// Sound-meta telemetry: 0x30 releases seen, 0x31 "cue lost" reports seen.
void soundMetaStats(uint32_t& rel, uint32_t& lost) { rel = g_relCount; lost = g_lostCount; }

// Human-readable decode of one raw byte, for the trace dump. `value` = the token payload,
// or -1 when the token carries none.
const char* traceDecode(uint8_t b, int& value) {
  value = -1;
  switch (trClass(b)) {
    case CL_SNAP:  return "snap";                                     // 0xBF frame marker
    case CL_SNAPD: value = b & 0x0F; return (b & 0x10) ? "snapLo" : "snapHi";
    case CL_MODE:  value = b & 0x01; return "mode";                   // 1 = diag
    case CL_STATE: value = b & 0x01; return "state";                  // 1 = 6502 booted
    case CL_DINJ:  value = b & 0x0F; return "dinj";
    case CL_RXC:   value = b & 0x0F; return "rxc";
    case CL_BALL:  value = b & 0x0F; return "gip";                    // RIOT $0072
    case CL_SND:   value = b & 0x1F; return "snd";                    // THE sound command
    case CL_GAME:  value = b & 0x3F; return "game";
    case CL_SMETA: value = b & 0x0F;
                   return (b == 0x30) ? "rel" : (b == 0x31) ? "lost" : "smeta?";
    default:       value = b; return "?";                             // unclaimed byte space
  }
}

// RAM snapshot: frames stored, frames aborted, millis() of the last one, ms since it.
void ramSnapStats(uint32_t& frames, uint32_t& bad, uint32_t& ms, uint32_t& ageMs) {
  frames = g_snapFrames; bad = g_snapBad; ms = g_snapMs;
  ageMs = g_snapFrames ? (millis() - g_snapMs) : 0xFFFFFFFF;
}

// Copy the last complete frame out. Reads g_snapPub once: poll() only ever writes the
// OTHER buffer, and the next flip is a full frame period (~1 s) away, so this 384-byte
// memcpy cannot race a capture in progress.
bool ramSnapCopy(uint8_t* out) {
  if (!out) return false;
  if (!g_snapFrames) { memset(out, 0, RAMSNAP_N); return false; }
  memcpy(out, g_snapBuf[g_snapPub], RAMSNAP_N);
  return true;
}

} // namespace fpgalink
