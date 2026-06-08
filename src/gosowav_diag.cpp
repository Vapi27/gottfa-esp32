// gosowav_diag.cpp — bare WiFi+WebServer bisect for the GOSOWAV WROVER. NO SD, NO emulator, NO DAC.
// Proves the board's SoftAP + HTTP path works in isolation, to separate "firmware blocked" from
// "board/client" faults. Build: pio run -e gosowav_diag -t upload   (env in platformio.ini).
// Join SSID 'GOSOWAV-DIAG' then open http://192.168.4.1/  (or `curl -v http://192.168.4.1/`).
//   page LOADS  -> WROVER web stack is fine; the real-firmware fault was the blocking SD/bench.
//   page DEAD   -> lower-level (board/antenna/power/brownout or client captive-portal); not our code.
// (C) 2026 Valere Pilpil / Pstore.
#ifdef GOSOWAV_DIAG
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

static WebServer server(80);
static uint32_t g_hits = 0;

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n=== GOSOWAV DIAG (bare SoftAP + WebServer, no SD/emu) ===");
  WiFi.mode(WIFI_AP); WiFi.softAP("GOSOWAV-DIAG");
  Serial.printf("join WiFi 'GOSOWAV-DIAG' -> http://%s/\n", WiFi.softAPIP().toString().c_str());
  server.on("/", []() { g_hits++; server.send(200, "text/html", "<h1>diag ok</h1><p>WROVER web stack works.</p>"); });
  server.onNotFound([]() { g_hits++; server.send(200, "text/html", "<h1>diag ok</h1>"); });   // catch captive-portal probes too
  server.begin();
}

void loop() {
  server.handleClient();
  static uint32_t t = 0;
  if (millis() - t > 2000) { t = millis(); Serial.printf("alive: AP stations=%d  http hits=%u\n", WiFi.softAPgetStationNum(), (unsigned)g_hits); }
}
#endif // GOSOWAV_DIAG
