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
  // --- bring-up counters: prove the GPIO8 link is alive (no behaviour change) ---
  uint32_t g_rxCount = 0;             // total bytes ever received on the link
  uint8_t  g_lastByte = 0;            // most recent raw byte
  uint32_t g_lastMs = 0;             // millis() of the most recent byte
}

namespace fpgalink {

void begin() {
  port.begin(115200, SERIAL_8N1, PIN_FPGA_LINK, -1);   // RX only (no TX pin)
}

void poll() {
  while (port.available()) {
    uint8_t b = (uint8_t)port.read();
    g_rxCount++; g_lastByte = b; g_lastMs = millis();   // bring-up telemetry
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

// Bring-up telemetry: total bytes seen, last raw byte, ms since last byte.
void stats(uint32_t& total, uint8_t& last, uint32_t& ageMs) {
  total = g_rxCount; last = g_lastByte;
  ageMs = g_rxCount ? (millis() - g_lastMs) : 0xFFFFFFFF;
}

} // namespace fpgalink
