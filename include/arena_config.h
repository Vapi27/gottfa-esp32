#pragma once
#include <Arduino.h>

// ============================================================================
//  Arena Wall-Art LED — configuration
//  Gottlieb "Arena" playfield turned into an illuminated wall decoration.
//  Target MCU: ESP32-S3 (DevKitC-1, USB-C) driving up to 150 SK6812MINI-RGBW.
//  Decorative only: no gameplay electronics, no FPGA, no SPI bridge.
//  Full hardware/build notes: ../ARENA_LED.md
// ============================================================================

#define ARENA_FW_NAME    "Arena Wall-Art LED"
#define ARENA_FW_VERSION "1.0.0"
#define ARENA_MDNS_HOST  "arena"            // -> http://arena.local/

// ---- WiFi -------------------------------------------------------------------
// Leave the STA fields empty to boot straight into the SoftAP (join 'Arena-LED',
// password below, then open http://192.168.4.1/). Credentials set from the web UI
// are stored in NVS and win over these compile-time defaults.
#define ARENA_STA_SSID       ""
#define ARENA_STA_PASS       ""
#define ARENA_STA_TIMEOUT_MS 12000
#define ARENA_AP_SSID        "Arena-LED"
#define ARENA_AP_PASS        "pinball87"    // >= 8 chars

// ---- LED chain --------------------------------------------------------------
// One single data chain: ESP32 -> LED1 -> LED2 -> ... -> LEDn (data only; +5V and
// GND come from the two thick bus wires under the playfield, never through the
// small data connectors). LED_MAX sizes the frame buffers at compile time, so the
// firmware is built for the full 150-LED target from day one; LED_COUNT_DEFAULT is
// just the boot value and is changed live from the web UI (persisted in NVS).
#define LED_MAX              150
#define LED_COUNT_DEFAULT    100
#define PIN_LED_DATA           5   // GPIO5 -> 330 R -> DATA IN of LED1
#define LED_FRAME_HZ          60   // render/refresh rate (150 px RGBW = 4.8 ms/frame on the wire)

// Second (optional) chain — split the playfield in two halves if one long run
// picks up too much noise. 0 = disabled (single chain, the documented build).
#define LED_CHAIN2_ENABLE      0
#define PIN_LED_DATA2          6

// ---- 3.3 V logic into a 5 V chain, without a 74AHCT125 ----------------------
// The SK6812 wants VIH >= 0.7 x VDD; at VDD = 5.0 V that is 3.5 V and an ESP32
// GPIO only reaches 3.3 V. Two hardware fixes need no logic IC (ARENA_LED.md §9):
//   A. run the whole chain at 4.3-4.5 V (trim the PSU) -> VIH = 3.0-3.15 V. Nothing to set here.
//   B. "repeater pixel": feed ONLY the first LED through 2 series Schottky diodes
//      (~4.4 V) so it accepts 3.3 V data, and let its DATA OUT — a clean, full
//      VDD-swing regenerated signal — drive the rest of the chain at 5 V.
// Set this to 1 for option B: pixel 0 is then a hidden repeater, kept dark and
// excluded from the map, so LED numbering in the UI still starts at the first
// visible insert.
#define LED_REPEATER_PIXEL     0

// ---- Front-panel button (optional) -----------------------------------------
// The DevKitC-1 BOOT button is on GPIO0 and is free once the board is running:
// short press = next lighting mode, long press (>1 s) = night mode toggle.
// Wire any NO push button to GPIO0/GND to bring it out to the frame edge.
#define ARENA_BUTTON_ENABLE    1
#define PIN_ARENA_BUTTON       0   // active LOW (internal pull-up)

// ---- Power model / safety ---------------------------------------------------
// SK6812MINI-RGBW: 4 dice, ~17.5 mA each at full drive => ~70 mA per LED all-on,
// plus ~1 mA quiescent for the controller. 150 LEDs all-white-all-colours would be
// 150 x 70 mA = 10.5 A, hence the 5 V / 15 A supply (20 % margin) in ARENA_LED.md.
// The firmware never trusts that headroom blindly: every frame is metered and, if
// the estimate exceeds LED_POWER_BUDGET_MA, the whole frame is scaled down before
// it is pushed out. That keeps the PSU, the bus wires and the injection points
// inside their ratings whatever effect is running.
#define LED_MA_PER_CHANNEL    17.5f
#define LED_MA_QUIESCENT       1.0f
#define LED_POWER_BUDGET_MA   9000   // mA ceiling for the whole chain (9 A of a 15 A PSU)

// ---- Look & feel defaults ---------------------------------------------------
// Vintage incandescent reference values (see ARENA_LED.md §10).
#define ARENA_AMBER_R 255
#define ARENA_AMBER_G 100
#define ARENA_AMBER_B   0
#define ARENA_AMBER_W   0

#define ARENA_GOLD_R  255
#define ARENA_GOLD_G  140
#define ARENA_GOLD_B    0
#define ARENA_GOLD_W   10

#define ARENA_WARM_R    0
#define ARENA_WARM_G    0
#define ARENA_WARM_B    0
#define ARENA_WARM_W  255

#define ARENA_BRIGHT_DEFAULT 180   // 0..255 global brightness
#define ARENA_SPEED_DEFAULT  128   // 0..255 -> x0.25 .. x4 animation speed
#define ARENA_NIGHT_BRIGHT    26   // ~10 % of 255 (night mode)

// ---- Filesystem -------------------------------------------------------------
#define ARENA_MAP_PATH "/arena_map.json"   // insert map (editable from the web UI)
