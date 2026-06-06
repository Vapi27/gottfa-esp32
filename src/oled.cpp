// oled.cpp — see oled.h. SSD1306 128x32 status screen (S3 sound-tier board).
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "oled.h"
#include "board_config.h"

#ifndef BOARD_C3
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "net.h"
#include "wavplayer.h"
#include "fpgalink.h"

namespace {
  Adafruit_SSD1306 disp(OLED_W, OLED_H, &Wire, -1);
  bool     present = false;
  uint32_t lastMs  = 0;
}

namespace oled {

void begin() {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  present = disp.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!present) { Serial.println("[oled] no SSD1306 (skipped)"); return; }
  disp.clearDisplay();
  disp.setTextColor(SSD1306_WHITE); disp.setTextSize(1); disp.setCursor(0, 0);
  disp.println("GottFA80+ PSOWAV"); disp.println(FW_VERSION);
  disp.display();
  Serial.println("[oled] SSD1306 ready");
}

void tick() {
  if (!present) return;
  uint32_t now = millis(); if (now - lastMs < 500) return; lastMs = now;
  disp.clearDisplay(); disp.setCursor(0, 0);
  disp.print("GottFA80+ ");                              // line 1: mode
  disp.println(fpgalink::diagActive() ? "DIAG" : "play");
  if (wavplayer::ready()) {                              // line 2: game + last sound
    disp.print(wavplayer::curTheme());
    int s = wavplayer::lastSound(); if (s >= 0) { disp.print(" s"); disp.print(s); }
    disp.println();
  } else disp.println("no SD");
  disp.print(netMode()); disp.print(' '); disp.println(netIp());   // line 3: wifi/ip
  disp.print("heap "); disp.print(ESP.getFreeHeap() / 1024); disp.println("k");  // line 4
  disp.display();
}

} // namespace oled

#else  // C3: no OLED (no sound-tier pins)
namespace oled { void begin() {} void tick() {} }
#endif
