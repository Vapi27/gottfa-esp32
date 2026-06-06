// oled.h — optional SSD1306 128x32 I2C status screen for the GottFA80+ ESP companion.
// Shows mode / game / last sound / WiFi-IP / heap. Gracefully skipped if no panel is found.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
namespace oled {
  void begin();   // init I2C + the panel (no-op if absent or on C3)
  void tick();    // refresh ~2 Hz; call from loop()
}
