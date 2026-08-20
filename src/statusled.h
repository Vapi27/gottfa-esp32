// statusled.h — on-board WS2812 RGB LED as a status beacon (ESP32-S3 DevKitC-1).
// Priority (highest first):
//   XVC session (FPGA flash over WiFi)  -> ORANGE, slow breathing
//   OTA update in progress              -> VIOLET, fast blink
//   diag / LISYcontrol mode             -> CYAN, solid dim
//   FPGA link alive (fresh heartbeat)   -> GREEN, solid dim
//   powered but no FPGA link            -> RED, faint slow blink
// DevKitC-1 v1.0 has the LED on GPIO48 (= our OLED SCL) — begin() disables the
// LED if an OLED answered on the bus. v1.1 boards use GPIO38: change PIN_RGB_LED.
// (C) 2026 Valere Pillet / Pstore. Original implementation.
#pragma once
namespace statusled {
  void begin();   // call AFTER oled::begin() (pin-conflict detection)
  void tick();    // call from loop(); cheap (rate-limited internally)
}
