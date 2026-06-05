// soundlink.cpp — see soundlink.h. ESP32-S3 sound tier. Not built on C3.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#ifndef BOARD_C3
#include "soundlink.h"
#include "wavplayer.h"
#include "board_config.h"
#include <Arduino.h>

namespace {
  HardwareSerial& port = Serial1;            // dedicated UART for the FPGA sound link
}

namespace soundlink {

void begin() {
  port.begin(115200, SERIAL_8N1, PIN_SOUNDLINK_RX, -1);   // RX only (no TX pin)
}

void poll() {
  while (port.available()) {
    uint8_t b = (uint8_t)port.read();
    if (b & 0x80) {                          // sound command
      wavplayer::play(b & 0x1F);
    } else if (b & 0x40) {                    // game number -> theme folder "<n>"
      char t[8]; snprintf(t, sizeof(t), "%d", b & 0x3F);
      wavplayer::setTheme(t);
    }
  }
}

} // namespace soundlink
#endif // !BOARD_C3
