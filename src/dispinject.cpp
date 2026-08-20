// dispinject.cpp — see dispinject.h. ESP->FPGA time-attack display + control UART.
// (C) 2026 Valere Pillet / Pstore. Original implementation.
#include "dispinject.h"
#ifndef BOARD_C3
#include "board_config.h"
#include <Arduino.h>
#include <stdio.h>

namespace {
  bool     g_on   = false;
  uint8_t  g_ctrl = 0;          // latched steady-state flags (bit0/bit1)
  uint32_t g_lastCtrlMs = 0;    // millis() of the last control frame sent
  uint32_t g_killUntil  = 0;    // while > millis(): OR CTRL_GAME_KILL into every frame
  uint32_t g_quietUntil = 0;    // keep repeating flags==0 until here (explicit clear)
  uint32_t g_holdVal    = 0;    // bench: value repeated on the glass until g_holdUntil
  uint32_t g_holdUntil  = 0;
  uint32_t g_lastDispMs = 0;    // millis() of the last DISPLAY frame sent
  const uint32_t REPEAT_MS = 250;   // 4 Hz — the FPGA fail-safe fires at 2 s of silence
  const uint32_t KILL_MS   = 400;   // hold bit2 long enough for the FPGA to see the edge
  const uint32_t QUIET_MS  = 3000;  // after disarming, still push the explicit 0 for 3 s

  // One control frame. The flags byte is masked into 0x00..0x7D so it can NEVER look like a
  // frame marker (0xFE/0xFF) — the whole framing rests on that invariant. We only ever set
  // bits 0-2, so the clamp is belt-and-braces, not a live path.
  void writeCtrl(uint8_t flags) {
    uint8_t f = (uint8_t)(flags & 0x7F);
    if (f > 0x7D) f &= 0x7D;
    uint8_t frame[2] = { 0xFE, f };
    Serial2.write(frame, 2);
    g_lastCtrlMs = millis();
  }
  // Steady flags + the kill one-shot while its window is open.
  uint8_t liveFlags(uint32_t now) {
    uint8_t f = g_ctrl;
    if (g_killUntil && (int32_t)(g_killUntil - now) > 0) f |= dispinject::CTRL_GAME_KILL;
    return f;
  }
}

namespace dispinject {

void begin() {
  // TX only (RX pin = -1). Wired to the FPGA Audio_RX/PIN_2 (freed in a hybrid build).
  Serial2.begin(115200, SERIAL_8N1, -1, PIN_FPGA_DISP_TX);
  g_on = true;
}

void send(uint32_t value) {
  if (!g_on) return;
  if (value > 9999999u) value = 9999999u;           // 7-digit display range
  char d[8];
  // "%7lu" = right-justified in 7 cols with leading SPACES (= leading-zero blanking, last digit
  // kept), exactly matching the FPGA value_to_dispstr rendering it replaces. Digits and blank
  // only — the System-80 glass cannot render letters at all.
  snprintf(d, sizeof d, "%7lu", (unsigned long)value);
  uint8_t frame[8];
  frame[0] = 0xFF;                                   // sync (never a printable char)
  for (int i = 0; i < 7; i++) frame[i + 1] = (uint8_t)d[i];
  Serial2.write(frame, 8);
  g_lastDispMs = millis();
}

void sendCtrl(uint8_t flags) { if (g_on) writeCtrl(flags); }

// Latch the steady flags. Sent immediately on change so arming/disarming is not delayed by up
// to one repeat period, then kept alive by tick(). Dropping to 0 starts a 3 s window in which
// we keep pushing the explicit 0, so the FPGA clears at once instead of waiting out its 2 s
// fail-safe timeout.
void setCtrl(uint8_t flags) {
  // Steady-state bits: 0 auto-restart, 1 overlay, 3 long-slam option, 6..4 overlay TARGET
  // (000 player 2 = default, 010 player 4, 101/110 the strobe-12..15 probes, 111 full glass).
  // Bit 2 (kill) is excluded on purpose: it is a one-shot edge owned by pulseKill().
  uint8_t f = (uint8_t)(flags & 0x7B);
  if (f == g_ctrl) return;
  g_ctrl = f;
  g_quietUntil = f ? 0 : (millis() + QUIET_MS);
  if (g_on) writeCtrl(liveFlags(millis()));
}

// game_kill is edge-triggered in the FPGA and needs no timeout: raise bit2 for KILL_MS (several
// repeats, so one lost frame cannot swallow the edge), then let it fall so a later kill makes a
// fresh 0->1 edge.
void pulseKill() {
  g_killUntil = millis() + KILL_MS;
  if (g_on) writeCtrl(liveFlags(millis()));
}

// Bench helper: keep ONE value on the glass. The FPGA's overlay enable is
// `ta_arm <= ctrl(1) and dvalid`, and dvalid self-clears 1 s after the last DISPLAY frame — so a
// control byte alone shows nothing, and a single display frame shows something for one second and
// then vanishes. Anything driving the glass must therefore repeat the display frame, not just the
// control frame. The time-attack engine already does (4 Hz); this is for /dispinj and manual tests.
void holdValue(uint32_t value, uint32_t ms) {
  g_holdVal   = value;
  g_holdUntil = millis() + ms;
  if (g_on) send(value);                             // show it now, tick() keeps it alive
}

void tick() {
  if (!g_on) return;
  uint32_t now = millis();
  bool killOpen = g_killUntil && (int32_t)(g_killUntil - now) > 0;
  bool quiet    = g_quietUntil && (int32_t)(g_quietUntil - now) > 0;
  bool holding  = g_holdUntil && (int32_t)(g_holdUntil - now) > 0;

  if (holding && (now - g_lastDispMs) >= REPEAT_MS) { send(g_holdVal); }

  if (!g_ctrl && !killOpen && !quiet) return;        // nothing armed: let the FPGA fail-safe hold
  if (now - g_lastCtrlMs < REPEAT_MS) return;
  writeCtrl(liveFlags(now));
}

uint8_t ctrl() { return g_ctrl; }

} // namespace dispinject

#else  // BOARD_C3: no Serial2 / no display tier -> no-ops
namespace dispinject {
  void begin() {} void send(uint32_t) {} void sendCtrl(uint8_t) {}
  void setCtrl(uint8_t) {} void pulseKill() {} void tick() {} uint8_t ctrl() { return 0; }
  void holdValue(uint32_t, uint32_t) {}
}
#endif
