// statusled.cpp — see statusled.h. WS2812 status beacon via the core's neopixelWrite.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "statusled.h"
#include "board_config.h"

#ifndef BOARD_C3
#include <Arduino.h>
#include <Update.h>
#include "xvc.h"
#include "fpgalink.h"
#include "oled.h"

namespace {
  bool     enabled = false;
  uint32_t lastMs  = 0;
  uint8_t  lastR = 255, lastG = 255, lastB = 255;   // impossible first value -> forces 1st write

  // soft triangle wave 0..255, period ms (breathing)
  inline uint8_t breathe(uint32_t period) {
    uint32_t t = millis() % period;
    uint32_t half = period / 2;
    return (uint8_t)((t < half ? t : period - t) * 255 / half);
  }

  inline void show(uint8_t r, uint8_t g, uint8_t b) {
    if (r == lastR && g == lastG && b == lastB) return;   // WS2812 write only on change
    lastR = r; lastG = g; lastB = b;
    neopixelWrite(PIN_RGB_LED, r, g, b);
  }
}

namespace statusled {

void begin() {
  // DevKitC-1 v1.0: the WS2812 shares GPIO48 with our OLED SCL. If a panel
  // answered, the pin belongs to I2C — the LED stays off rather than jamming it.
  if (PIN_RGB_LED == PIN_OLED_SCL && oled::found()) {
    Serial.println("[led] GPIO48 owned by the OLED — status LED disabled");
    return;
  }
  enabled = true;
  neopixelWrite(PIN_RGB_LED, 0, 0, 0);
  Serial.printf("[led] WS2812 status beacon on GPIO%d\n", PIN_RGB_LED);
}

void tick() {
  if (!enabled) return;
  uint32_t now = millis(); if (now - lastMs < 33) return; lastMs = now;   // ~30 Hz

  const uint8_t DIM = 28;   // gentle max brightness (it's a beacon, not a torch)

  if (xvc::active()) {                    // FPGA flash over WiFi: ORANGE slow breathing
    uint8_t v = breathe(1600);
    show((uint16_t)v * DIM / 255, (uint16_t)v * (DIM / 3) / 255, 0);
  } else if (Update.isRunning()) {        // ESP OTA: VIOLET fast blink
    bool on = (now / 150) & 1;
    show(on ? DIM : 0, 0, on ? DIM : 0);
  } else if (fpgalink::diagActive()) {    // diag / LISYcontrol: CYAN solid
    show(0, DIM / 2, DIM / 2);
  } else {
    uint32_t total, age; uint8_t last;
    fpgalink::stats(total, last, age);
    if (total > 0 && age < 250) {         // FPGA link alive: GREEN dim solid
      show(0, DIM / 3, 0);
    } else {                              // powered, no FPGA link: faint RED slow blink
      bool on = (now / 1000) & 1;
      show(on ? DIM / 4 : 0, 0, 0);
    }
  }
}

} // namespace statusled

#else  // C3: no WS2812 on the board
namespace statusled { void begin() {} void tick() {} }
#endif
