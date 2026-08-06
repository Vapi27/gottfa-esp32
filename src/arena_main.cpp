#include <Arduino.h>
#include <WiFi.h>
#include <esp_netif.h>

// Under Matter, CHIP owns the WiFi - Arduino never started it, so
// WiFi.localIP() reports 0.0.0.0 forever even while CHIP happily routes
// Matter traffic (Siri worked while the web server never rose: this is why).
// Read the address at the esp_netif level, where it actually lives.
static bool netHasIp() {
  esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t info;
  return n && esp_netif_get_ip_info(n, &info) == ESP_OK && info.ip.addr != 0;
}
#include <LittleFS.h>
#include "arena_config.h"
#include "arenaled.h"
#include "arena_map.h"
#include "arena_pf.h"
#include "arena_attract.h"
#include "arena_oled.h"
#include "arena_net.h"
#include "arena_peers.h"
#ifdef ARENA_MATTER
extern void arena_matter_init();
extern void arena_matter_sync();   // reflechit le mode reel vers Maison/Siri
#endif

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
  if (down) arenaoled::poke();
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
#ifndef ARENA_MATTER
  arenaattract::begin();  // Arena's own attract sequence, captured from the ROM
#endif                     // under Matter: deferred until provisioned - its 19 KB
                           // belong to the PASE crypto during commissioning
  arenaled::begin();      // NVS settings + chain init; pixels go dark-then-live
#ifdef ARENA_MATTER
  arena_matter_init();    // Matter d abord: c est lui qui possede le WiFi
#endif
  arenanet::begin();      // WiFi + http://arena.local/
  arenaoled::begin();     // ecran de controle, inerte si aucun panneau
  arenapeers::begin();    // voisinage : les murs se voient et se repondent

  Serial.printf("[boot] ready — %u LEDs, mode=%s, %s %s\n",
                arenaled::count(), arenaled::modeName(arenaled::mode()),
                arenanet::mode(), arenanet::ip());
}

void loop() {
#ifdef ARENA_MATTER
  // Grace period: the IP arrives BEFORE commissioning is finished - the phone
  // still has the operational CASE handshake to run over that fresh network.
  // Spending RAM on the web server or the attract capture at that instant is
  // the same starvation bug as the PASE abort, one step later. Eight seconds
  // of stable IP means commissioning is done (or this is a normal reboot).
  static uint32_t ipSinceMs = 0;
  if (!netHasIp()) {
    ipSinceMs = 0;
  } else {
    if (!ipSinceMs) ipSinceMs = millis();
    if (millis() - ipSinceMs > 8000) {
      arenanet::matterTick();
      arena_matter_sync();   // page web -> Maison, pas seulement l'inverse
      static bool atrLoaded = false;
      if (!atrLoaded) { atrLoaded = true; arenaattract::begin(); }
    }
  }
#endif
#if ARENA_BUTTON_ENABLE
  buttonPoll();
#endif
  arenaled::tick();
  arenaoled::tick();       // self-rate-limited to LED_FRAME_HZ
  arenanet::loop();
  arenapeers::tick();      // balise UDP + adoption de l'etat des voisins
}
