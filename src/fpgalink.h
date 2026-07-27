// fpgalink.h — the single 8N1 UART link from the FPGA, tapped on the Debug pin
// (PIN_FPGA_LINK, = FPGA PIN_11 / K2, right next to the FPGA). One self-describing
// byte (see sound_link.vhd): 0xF0|diag (mode token), 0x80|sound[4:0], 0x40|game[5:0],
// 0xA0|value[3:0], emitted when RIOT RAM $0072 changes. That byte is NOT the ball counter
//
// FULL BYTE MAP (authoritative copy; the FPGA side is sound_link.vhd, the rationale and
// the FPGA-side obligations are SOUND_WIRE.md):
//   0x00-0x2F  free / reserved — MUST stay a no-op (a line break or a floating pin during
//              FPGA reconfiguration decodes as 0x00, so 0x00 can never be a token)
//   0x30-0x3F  SOUND META (claimed by SOUND_WIRE.md, decoded below, not yet emitted):
//              0x30 = sound bus RELEASED (no command selected) · 0x31 = sound events LOST
//              (FPGA FIFO overflow) · 0x32-0x3F reserved
//   0x40-0x7F  game number 0x40|game[5:0]            [LEVEL]
//   0x80-0x9F  sound command 0x80|cmd[4:0]           [EVENT]
//   0xA0-0xAF  RIOT $0072 (game in progress)         [LEVEL]
//   0xB0-0xBE  disp_inject deframed-byte count       [LEVEL]
//   0xBF       RAM-snapshot frame marker
//   0xC0-0xDF  RAM-snapshot payload (hi/lo nibble)
//   0xE0-0xEF  disp_inject state                     [LEVEL]
//   0xF0-0xF3  diag mode / game-state                [LEVEL]
//   0xF4-0xFF  free / reserved
//
// (the ball in play is $0109): $0072 is the GAME-IN-PROGRESS flag, 0 = attract / 1 = playing,
// proven twice on hardware. The token name stayed "ball" for wire compatibility.
// Diag and gameplay sound never overlap, so one wire carries both. Compiled on both
// tiers: the mode token drives diag bus arbitration (S3 + C3); the sound/game bytes
// drive the WAV player (S3 only).
//
// The same wire also carries a periodic RAM-snapshot FRAME (~1 Hz, ~67 ms of link
// time at 115200 = ~7% duty):
//     0xBF                      frame marker
//     then 384 values, each as EXACTLY two bytes: 0xC0|(v>>4) then 0xD0|(v&0x0F)
//   = 1 + 768 = 769 bytes. Every payload byte lands in 0xC0..0xDF, a range no token
//   test can claim, so framing is unambiguous and a lost byte self-heals at the next
//   0xBF. See fpgalink.cpp for the byte-by-byte collision proof.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include <stdint.h>
namespace fpgalink {
  void begin();        // open the UART (RX only) on PIN_FPGA_LINK
  void poll();         // drain the FIFO: update diag mode (+ S3: play sounds). Call from loop().
  bool diagActive();   // last diag-mode token (0xF1 -> true, 0xF0 -> false)
  // ⚠ NOT a game-state signal. This is the last 0xF2/0xF3 token, which the FPGA derives from
  // `game_running` — proven on hardware (2026-07-25) to be a "the 6502 has booted" latch: it
  // goes high ~0.25 s after reset and NEVER clears. Kept for the /link bring-up readout only.
  // Anything that needs "is a game being played" must use gameInProgress() below.
  bool gameRunning();
  // THE real game-state signal: RIOT RAM $0072 (0 = attract, 1 = game in progress), proven
  // twice on hardware. Fed by BOTH link sources for this byte — the 0xA0|value change token
  // (sub-100 ms latency) and index 0x72 of every RAM snapshot (~1 Hz re-sync) — then debounced,
  // so a single glitched byte cannot start or end a tournament game.
  bool gameInProgress();
  // Live sound commands (token 0x80|cmd), FIFO, for the time-attack bonus. Returns false when
  // empty. Independent of the WAV player: the caller decides what a command is worth.
  bool popSound(uint8_t& cmd);

  // --- SOUND TRACE RING (the instrument, see SOUND_WIRE.md) -------------------------
  // Every byte the link delivers, timestamped, so one playfield target can be pressed and
  // the EXACT bus traffic it produced read back over HTTP (GET /sndtrace). This is the same
  // method that ground-truthed $0072, $0109 and the Arena command map: capture first, decide
  // after. Filling costs one compare + one 8-byte store per byte; nothing is decoded here.
  //
  // Two capture modes:
  //   FILTERED (default) — records every byte EXCEPT (a) the 0xC0..0xDF RAM-snapshot payload
  //     (769 bytes/s of bulk that would overwrite the whole ring in 0.6 s) and (b) a LEVEL
  //     token repeating a value already recorded for its class. Level tokens carry the current
  //     value by construction (sound_link.vhd calls them LEVEL and coalesces them freely), so
  //     a repeat carries zero information — but the 50 ms heartbeat re-sends mode+state+dinj
  //     ~60 times/s, which would also flush the ring. EVENT bytes (sound) and unknown bytes
  //     are NEVER elided.
  //   RAW — records literally every byte, snapshot payload included. ~830 bytes/s, so the ring
  //     holds ~0.6 s: arm it, do the one action, read it back immediately.
  constexpr uint16_t TRACE_N = 512;                    // ring entries (4 KB static)
  struct TraceEv { uint32_t ms; uint8_t b; };
  void traceMode(bool raw);                            // true = record everything
  bool traceRaw();
  void traceClear();
  // Copy up to `max` entries, OLDEST FIRST, into out[]. Returns how many were written.
  // Safe from the async HTTP task (taken under a spinlock; poll() writes under the same one).
  uint16_t traceCopy(TraceEv* out, uint16_t max);
  // kept = entries stored ever; elided = level repeats dropped; payload = snapshot bytes dropped.
  void traceStats(uint32_t& kept, uint32_t& elided, uint32_t& payload);
  // Decode a raw byte for display: returns the token-class name and, via `value`, the decoded
  // payload (-1 when the token has none). Lives here so the byte map has ONE owner.
  const char* traceDecode(uint8_t b, int& value);
  // Sound-meta counters: 0x30 bus-release events and 0x31 "FPGA dropped a cue" reports.
  // Both stay 0 until the FPGA implements SOUND_WIRE.md — which is itself the signal that
  // the wire is still the old lossy one.
  void soundMetaStats(uint32_t& rel, uint32_t& lost);
  void stats(uint32_t& total, uint8_t& last, uint32_t& ageMs);  // bring-up telemetry
  // ball-in-play telemetry (token 0xA0|value): last value, how many tokens seen,
  // ms since the last one (0xFFFFFFFF = jamais recu). Compiled on both tiers.
  void ballStats(uint8_t& value, uint32_t& count, uint32_t& ageMs);

  // --- Game RAM snapshot (frame 0xBF + 640 x 2 nibble bytes) -----------------------
  // Gottlieb System 80 game RAM, in this exact order:
  //   index   0..127 = CPU $0000..$007F (RIOT U4)
  //   index 128..255 = CPU $0080..$00FF (RIOT U5)
  //   index 256..383 = CPU $0100..$017F (RIOT U6)
  //   index 384..639 = CPU $1800..$18FF (5101 CMOS, LOW NIBBLE only -> 0..15)
  // Point: find which byte is REALLY the ball-in-play counter. $0072 (from PinMAME)
  // was disproven on hardware, and no RIOT byte tracked the ball either — hence the
  // 5101, which is where System 80 keeps its persistent game state.
  constexpr uint16_t RAMSNAP_N = 640;   // values per frame
  // Last COMPLETE frame stats: frames stored, frames aborted mid-capture (desync),
  // millis() of the last complete frame, ms since it (0xFFFFFFFF = jamais recu).
  void ramSnapStats(uint32_t& frames, uint32_t& bad, uint32_t& ms, uint32_t& ageMs);
  // Copy the last complete frame into out[RAMSNAP_N]. Returns false (and zero-fills)
  // if no frame has completed yet. Safe to call from the async HTTP task.
  bool ramSnapCopy(uint8_t* out);
}
