// dispinject.cpp — see dispinject.h. ESP->FPGA time-attack display UART (option B).
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "dispinject.h"
#ifndef BOARD_C3
#include "board_config.h"
#include <Arduino.h>
#include <stdio.h>

namespace { bool g_on = false; }

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
  // "%7lu" = right-justified in 7 cols with leading SPACES (= leading-zero blanking, last digit kept),
  // exactly matching the FPGA value_to_dispstr rendering it replaces.
  snprintf(d, sizeof d, "%7lu", (unsigned long)value);
  uint8_t frame[8];
  frame[0] = 0xFF;                                   // sync (never a printable char)
  for (int i = 0; i < 7; i++) frame[i + 1] = (uint8_t)d[i];
  Serial2.write(frame, 8);
}

} // namespace dispinject

#else  // BOARD_C3: no Serial2 / no display tier -> no-ops
namespace dispinject { void begin() {} void send(uint32_t) {} }
#endif
