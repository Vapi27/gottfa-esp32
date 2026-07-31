#include <Arduino.h>
#include <LittleFS.h>
#include "arena_config.h"
#include "arenaled.h"
#include "arena_map.h"
#include "arena_pf.h"
#include "arena_attract.h"
#include "arena_net.h"

// ============================================================================
//  Arena Wall-Art LED — firmware entry point (env: arenaled).
//
//  Standalone build: this firmware shares the repo but not the GottFA80 app —
//  no FPGA, no SPI bridge, no sound. Just the SK6812 chain + WiFi + web UI.
//    pio run -e arenaled -t uploadfs     (web UI -> LittleFS, do this first)
//    pio run -e arenaled -t upload       (firmware)
//    pio device monitor -e arenaled
// ============================================================================

#if ARENA_BUTTON_ENABLE
static uint32_t s_btnDown = 0;
static bool     s_btnHandled = false;
static const uint32_t BTN_LONG_MS = 1000;

static void buttonPoll() {
  bool down = (digitalRead(PIN_ARENA_BUTTON) == LOW);
  uint32_t now = millis();

  if (down && !s_btnDown) {                       // press
    s_btnDown = now;
    s_btnHandled = false;
  } else if (down && !s_btnHandled && now - s_btnDown > BTN_LONG_MS) {
    // Long press: toggle night mode (and back to the previous look on release-free hold)
    arenaled::setMode(arenaled::mode() == arenaled::MODE_NIGHT ? arenaled::MODE_CLASSIC
                                                               : arenaled::MODE_NIGHT);
    s_btnHandled = true;
  } else if (!down && s_btnDown) {                // release
    if (!s_btnHandled && now - s_btnDown > 30) arenaled::nextMode();   // short press (debounced)
    s_btnDown = 0;
  }
}
#endif

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n" ARENA_FW_NAME " v" ARENA_FW_VERSION);

#if ARENA_BUTTON_ENABLE
  pinMode(PIN_ARENA_BUTTON, INPUT_PULLUP);
#endif

  if (!LittleFS.begin(true))
    Serial.println("[fs] LittleFS mount failed (run: pio run -e arenaled -t uploadfs)");

  arenamap::begin();      // named zones: /arena_map.json, else the built-in template
  arenapf::begin();       // playfield geometry: inserts + which pixel sits on which
  arenaattract::begin();  // Arena's own attract sequence, captured from the ROM
  arenaled::begin();      // NVS settings + chain init; pixels go dark-then-live
  arenanet::begin();      // WiFi + http://arena.local/

  Serial.printf("[boot] ready — %u LEDs, mode=%s, %s %s\n",
                arenaled::count(), arenaled::modeName(arenaled::mode()),
                arenanet::mode(), arenanet::ip());
}

void loop() {
#if ARENA_BUTTON_ENABLE
  buttonPoll();
#endif
  arenaled::tick();       // self-rate-limited to LED_FRAME_HZ
  arenanet::loop();
}
