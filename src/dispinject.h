// dispinject.h — ESP -> FPGA one-wire control link (PIN_FPGA_DISP_TX -> FPGA Audio_RX/PIN_2,
// 115200 8N1, TX only). Three frame types, told apart by the first byte, which is the ONLY byte
// allowed to be >= 0xFD:
//
//   DISPLAY frame:  0xFF + 7 ASCII chars (0x20..0x7E), right-justified, space padded.
//                   The ESP computes the time-attack countdown and FORMATS the 7 chars itself
//                   (so the 7-seg 80/80A and 16-seg 80B differences stay in software); the FPGA
//                   `disp_inject` module just shows them. NOTE the glass renders DIGITS AND
//                   BLANK ONLY — never send letters, hence send() emits "%7lu".
//   CONTROL frame:  0xFE + ONE flags byte kept inside 0x00..0x7D so it can never be mistaken
//                   for a frame marker:
//                     bit0 CTRL_AUTORESTART  auto-restart games at game over (coin+start inject)
//                     bit1 CTRL_TA_DISPLAY   draw the injected countdown on the glass
//                     bit2 CTRL_GAME_KILL    edge 0->1 = end the current game NOW (one-shot)
//                     bits 3-6 reserved, sent as 0
//
//   CONTROL2 frame: 0xFD + ONE flags byte, same 0x00..0x7D clamp:
//                     bit0 CTRL2_DIAG        REQUEST diagnostic mode (LEVEL, not an edge)
//                     bits 1-6 reserved, sent as 0
//                   This is what makes LISY CONNECT / DISCONNECT work without the operator
//                   long-pressing the door test switch. Held high = in diag; dropped = out.
//                   The FPGA releases diag by itself after 5 s of silence, so a crashed ESP
//                   can never leave the 6502 frozen with the machine dead.
//
// FAIL-SAFE: the FPGA clears auto-restart + overlay by itself after 2 s with no valid control
// frame, so a crashed/unplugged ESP can never leave the machine restarting games forever. That
// is why tick() must be called from loop(): it repeats the latched flags at 4 Hz (<< 2 s).
// S3 only (uses Serial2; no-op on C3).
// (C) 2026 Valere Pillet / Pstore. Original implementation.
#pragma once
#include <stdint.h>
namespace dispinject {
  // control-frame flag bits (see the CONTROL frame above)
  constexpr uint8_t CTRL_AUTORESTART = 0x01;
  constexpr uint8_t CTRL_TA_DISPLAY  = 0x02;
  constexpr uint8_t CTRL_GAME_KILL   = 0x04;
  // control2-frame flag bits (see the CONTROL2 frame above)
  constexpr uint8_t CTRL2_DIAG       = 0x01;

  void begin();                 // open the TX-only UART to the FPGA (PIN_FPGA_DISP_TX)
  void send(uint32_t value);    // DISPLAY frame (0xFF + 7 chars), right-justified, leading zeros blanked
  void sendCtrl(uint8_t flags); // CONTROL frame (0xFE + flags), sent right now, once
  void setCtrl(uint8_t flags);  // latch the steady-state flags (bit0/bit1) — sent now + repeated by tick()
  void pulseKill();             // one-shot CTRL_GAME_KILL edge (held ~400 ms, then self-clears)
  void tick();                  // call from loop(): repeats the control frame at 4 Hz
  uint8_t ctrl();               // currently latched flags (for the web UI)
  void setCtrl2(uint8_t flags); // latch the CONTROL2 flags (bit0 = diag) — sent now + repeated by tick()
  uint8_t ctrl2();              // currently latched CONTROL2 flags
  // Bench: hold ONE value on the glass for `ms`, repeating the DISPLAY frame from tick().
  // Required because the FPGA's dvalid self-clears 1 s after the last display frame, so a
  // control byte alone (or a single display frame) leaves the glass blank.
  void holdValue(uint32_t value, uint32_t ms);
}
