// fpgalink.cpp — see fpgalink.h. The FPGA->ESP link on the Debug pin.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "fpgalink.h"
#include "board_config.h"
#include <Arduino.h>
#ifndef BOARD_C3
#include "wavplayer.h"
#endif

namespace {
  HardwareSerial& port = Serial1;     // dedicated UART for the FPGA link (Debug pin)
  bool g_diag = false;                // last diag-mode token (touched only from poll())
}

namespace fpgalink {

void begin() {
  port.begin(115200, SERIAL_8N1, PIN_FPGA_LINK, -1);   // RX only (no TX pin)
}

void poll() {
  while (port.available()) {
    uint8_t b = (uint8_t)port.read();
    if ((b & 0xFE) == 0xF0) {                 // diag-mode token: 0xF0 normal / 0xF1 diag
      g_diag = (b & 0x01) != 0;
    }
#ifndef BOARD_C3
    else if ((b & 0xE0) == 0x80) {            // sound command 0x80..0x9F
      wavplayer::play(b & 0x1F);
    } else if ((b & 0xC0) == 0x40) {          // game number 0x40..0x7F -> games.txt -> set
      wavplayer::selectGame(b & 0x3F);        // No = GottFA80_PLuS gamelist index
    }
#endif
  }
}

bool diagActive() { return g_diag; }

} // namespace fpgalink
