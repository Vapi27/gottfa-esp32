// wifiprov.h — WiFi provisioning wizard: SoftAP + captive portal + NVS-stored credentials.
//
// WHY this module exists: until now the home-network credentials were compile-time #defines in
// board_config.h, so a customer could not put the machine on their WiFi without a toolchain and a
// USB cable. This module replaces that with the usual appliance flow — the board raises its own
// hotspot, you join it, a setup page pops up, you pick your network. Choosing to STAY on the
// hotspot forever is an equally valid answer: the pinball is fully usable with no home network.
//
// INVARIANT (the whole point): the board is ALWAYS reachable. There is no code path that leaves
// both the STA link and the SoftAP down — every failure raises the AP again.
//
// WHERE the settings live: NVS (Preferences namespace "wifiprov"), NOT LittleFS. The web UI's
// /fsup route rewrites the whole LittleFS image, so credentials stored there would be wiped by a
// routine UI update — and the customer would be locked out of a board they cannot re-flash.
// NVS lives in its own partition and survives both /fsup and an OTA app update.
//
// THREADING: HTTP handlers run in the AsyncTCP task. They NEVER touch the radio — they only queue
// an intent under a spinlock and return. All WiFi work (scan, connect, AP up/down, DNS) happens in
// tick() from loop(). This is deliberate: doing real work inside an async handler has already
// wedged this firmware once (a 30 kHz bit-bang in a handler -> watchdog reboot -> silent OTA
// rollback). begin() is the one exception and it is called from setup(), never from a handler.
//
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include <Arduino.h>
class AsyncWebServer;
class AsyncWebServerRequest;

namespace wifiprov {

  // Load NVS + bring the radio up. Blocks up to WIFI_STA_TIMEOUT_MS on the first STA attempt
  // (same as the old netBegin() did) so that the caller's MDNS.begin() still runs on a live
  // interface. Returns with either the STA link or the SoftAP up — never neither.
  void begin();

  // Call from loop(). Drives everything: captive DNS, async scans, connection attempts,
  // STA-loss fallback, reconnect, and the BOOT-button factory reset. Never blocks.
  void tick();

  // Register the portal + its JSON API. MUST be called BEFORE server.serveStatic("/", ...) so the
  // portal can own "/" while the board is still unprovisioned (handlers match in registration
  // order). Everything it adds is filtered to the SoftAP interface, so it is invisible from the
  // home LAN once the machine is on it.
  void attachRoutes(AsyncWebServer &server);

  // Optional: call first from server.onNotFound(). Returns true if it answered the request
  // (captive-portal redirect for an unknown host on the AP). Improves the "sign in to this
  // network" popup on OSes that probe an unusual URL; everything works without it.
  bool captiveRedirect(AsyncWebServerRequest *r);

  bool        isAP();        // the SoftAP is currently up
  bool        isSTA();       // the home-network link is up
  const char* mode();        // "STA" / "SoftAP" / "STA+AP" — for the OLED and /info
  const char* ip();          // best current address (STA if connected, else the AP's)
  const char* staSsid();     // configured home SSID ("" if none)
  const char* apSsid();      // hotspot name, e.g. "GottFA80-3A7C1E"
  const char* apPass();      // hotspot password (WPA2). NEVER print the STA password anywhere.

  // Wipe the stored network + the "stay in AP mode" choice and go back to the wizard.
  // Reconfigures the radio live — no reboot, so it can never interrupt a NOR/OTA write.
  void forget();

  bool factoryHolding();     // BOOT button currently held (for an optional LED/OLED hint)
}
