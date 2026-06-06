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
  bool g_gameRunning = false;         // last game-state token (0xF2/0xF3) — tournament auto-timer
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
    else if ((b & 0xFE) == 0xF2) {            // game-state: 0xF2 over / 0xF3 running (tournament timer)
      g_gameRunning = (b & 0x01) != 0;
    }
#ifndef BOARD_C3
    else if ((b & 0xE0) == 0x80) {            // sound command 0x80..0x9F
      wavplayer::playLive(b & 0x1F);          // hybrid-aware: skips cmds GOSOF80 synthesises
    } else if ((b & 0xC0) == 0x40) {          // game number 0x40..0x7F -> games.txt -> set
      wavplayer::selectGame(b & 0x3F);        // No = GottFA80_PLuS gamelist index
    }
#endif
  }
}

bool diagActive() { return g_diag; }
bool gameRunning() { return g_gameRunning; }

} // namespace fpgalink
