// dispinject.h — ESP -> FPGA display injection for tournament time-attack (option B).
//
// The ESP computes the time-attack countdown and FORMATS the 7 display chars itself (so the
// 7-seg 80/80A and 16-seg 80B differences are handled in software), then streams them to the
// FPGA over a TX-only UART (PIN_FPGA_DISP_TX -> FPGA Audio_RX/PIN_2 in a hybrid build). The FPGA
// `disp_inject` module receives [0xFF + 7 chars], shows it on the machine display, and times out
// (drops the overlay) when we stop sending at game over. S3 only (uses Serial2; no-op on C3).
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include <stdint.h>
namespace dispinject {
  void begin();             // open the TX-only UART to the FPGA (PIN_FPGA_DISP_TX)
  void send(uint32_t value); // send one frame (0xFF + 7 chars), right-justified, leading zeros blanked
}
