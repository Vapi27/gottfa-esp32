#include <WiFi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Update.h>
#include "arena_config.h"
#include "arena_net.h"
#include "arenaled.h"
#include "arena_map.h"

namespace arenanet {

static AsyncWebServer s_server(80);
static Preferences    s_prefs;
static String         s_ip   = "0.0.0.0";
static String         s_mode = "init";

const char* ip()   { return s_ip.c_str(); }
const char* mode() { return s_mode.c_str(); }

// Served when LittleFS is empty (nobody ran `pio run -e arenaled -t uploadfs` yet):
// enough UI to prove the chain lights up and to reach the mode buttons.
static const char FALLBACK[] PROGMEM =
  "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Arena LED</title><style>body{background:#111;color:#eee;font:16px system-ui;padding:2em}"
  "a{display:inline-block;margin:.3em;padding:.6em 1em;background:#c60;color:#fff;"
  "text-decoration:none;border-radius:6px}</style><h1>Arena LED</h1>"
  "<p>Web UI not uploaded yet — run <code>pio run -e arenaled -t uploadfs</code>.</p>"
  "<p><a href='/api/set?mode=classic'>classic</a><a href='/api/set?mode=attract'>attract</a>"
  "<a href='/api/set?mode=arena'>arena</a><a href='/api/set?mode=night'>night</a>"
  "<a href='/api/set?mode=rainbow'>rainbow</a><a href='/api/set?mode=test'>test</a>"
  "<a href='/api/set?mode=off'>off</a></p><p><a href='/api/state'>/api/state</a></p>";

static String stateJson() {
  arenaled::Rgbw c = arenaled::color();
  String j = "{";
  j += "\"fw\":\"" ARENA_FW_VERSION "\"";
  j += ",\"mode\":\"" + String(arenaled::modeName(arenaled::mode())) + "\"";
  j += ",\"bright\":" + String(arenaled::brightness());
  j += ",\"speed\":"  + String(arenaled::speed());
  j += ",\"count\":"  + String(arenaled::count());
  j += ",\"max\":"    + String(LED_MAX);
  j += ",\"r\":" + String(c.r) + ",\"g\":" + String(c.g) +
       ",\"b\":" + String(c.b) + ",\"w\":" + String(c.w);
  j += ",\"amps\":"   + String(arenaled::lastAmps(), 2);
  // Also in mA: two decimals of an amp cannot show a bench of one or three LEDs
  // (a single pixel in TEST mode is ~4 mA, which prints as "0.00" and reads like
  // a broken meter). mA is what the multimeter in the +5 V wire shows anyway.
  j += ",\"ma\":"     + String(arenaled::lastAmps() * 1000.0f, 1);
  j += ",\"budget\":" + String(arenaled::budgetMa());
  j += ",\"order\":\""  + String(arenaled::order()) + "\"";
  j += ",\"limited\":" + String(arenaled::limited() ? "true" : "false");
  j += ",\"fps\":"    + String(arenaled::fps());
  j += ",\"ip\":\""   + s_ip + "\",\"net\":\"" + s_mode + "\"";
  j += ",\"up\":"     + String(millis() / 1000);
  j += ",\"heap\":"   + String(ESP.getFreeHeap());
  j += "}";
  return j;
}

static uint8_t param8(AsyncWebServerRequest* r, const char* k, uint8_t cur) {
  if (!r->hasParam(k)) return cur;
  long v = r->getParam(k)->value().toInt();
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return (uint8_t)v;
}

void begin() {
  // --- WiFi: NVS credentials (set from the UI) override the compile-time ones ---
  s_prefs.begin("arenanet", false);
  String ssid = s_prefs.getString("ssid", ARENA_STA_SSID);
  String pass = s_prefs.getString("pass", ARENA_STA_PASS);

  WiFi.persistent(false);
  bool connected = false;
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(ARENA_MDNS_HOST);
    WiFi.setSleep(false);            // keep the REST latency low; the wall is mains-powered
    Serial.printf("[net] STA connecting to '%s' ...\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < ARENA_STA_TIMEOUT_MS) {
      delay(250);
      Serial.print('.');
    }
    Serial.println();
    connected = (WiFi.status() == WL_CONNECTED);
  }
  if (connected) {
    s_mode = "STA";
    s_ip = WiFi.localIP().toString();
    Serial.printf("[net] STA OK ip=%s\n", s_ip.c_str());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ARENA_AP_SSID, ARENA_AP_PASS);
    s_mode = "SoftAP";
    s_ip = WiFi.softAPIP().toString();
    Serial.printf("[net] SoftAP '%s' ip=%s\n", ARENA_AP_SSID, s_ip.c_str());
  }
  if (MDNS.begin(ARENA_MDNS_HOST)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[net] http://%s.local/\n", ARENA_MDNS_HOST);
  }

  // --- UI ---------------------------------------------------------------
  s_server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (LittleFS.exists("/arena.html")) r->send(LittleFS, "/arena.html", "text/html");
    else                                r->send_P(200, "text/html", FALLBACK);
  });

  // --- State / control ----------------------------------------------------
  s_server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", stateJson());
  });

  //  /api/set?mode=arena&bright=180&speed=128&r=255&g=100&b=0&w=0&count=100&budget=9000&order=grbw
  s_server.on("/api/set", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (r->hasParam("mode")) {
      arenaled::Mode m = arenaled::modeFromName(r->getParam("mode")->value().c_str());
      if (m == arenaled::MODE_COUNT) { r->send(400, "text/plain", "bad mode"); return; }
      arenaled::setMode(m);
    }
    if (r->hasParam("bright")) arenaled::setBrightness(param8(r, "bright", arenaled::brightness()));
    if (r->hasParam("speed"))  arenaled::setSpeed(param8(r, "speed", arenaled::speed()));
    if (r->hasParam("r") || r->hasParam("g") || r->hasParam("b") || r->hasParam("w")) {
      arenaled::Rgbw c = arenaled::color();
      c.r = param8(r, "r", c.r);
      c.g = param8(r, "g", c.g);
      c.b = param8(r, "b", c.b);
      c.w = param8(r, "w", c.w);
      arenaled::setColor(c);
    }
    if (r->hasParam("order")) {
      if (!arenaled::setOrder(r->getParam("order")->value().c_str())) {
        r->send(400, "text/plain", "bad order (grbw|rgbw|gbrw|brgw|rbgw|bgrw)");
        return;
      }
    }
    if (r->hasParam("count"))  arenaled::setCount((uint16_t)r->getParam("count")->value().toInt());
    if (r->hasParam("budget")) arenaled::setBudgetMa((uint16_t)r->getParam("budget")->value().toInt());
    r->send(200, "application/json", stateJson());
  });

  s_server.on("/api/save", HTTP_GET, [](AsyncWebServerRequest* r) {
    arenaled::save();
    r->send(200, "text/plain", "saved");
  });

  // --- Mapping wizard -----------------------------------------------------
  //  /api/identify?led=42 | ?zone=3 | ?clear=1   (ms= optional, default 10 s)
  s_server.on("/api/identify", HTTP_GET, [](AsyncWebServerRequest* r) {
    uint32_t ms = r->hasParam("ms") ? (uint32_t)r->getParam("ms")->value().toInt() : 10000;
    if (r->hasParam("clear"))     arenaled::clearIdentify();
    else if (r->hasParam("led"))  arenaled::identifyLed(r->getParam("led")->value().toInt(), ms);
    else if (r->hasParam("zone")) arenaled::identifyZone(r->getParam("zone")->value().toInt(), ms);
    else { r->send(400, "text/plain", "led= | zone= | clear=1"); return; }
    r->send(200, "text/plain", "ok");
  });

  s_server.on("/api/zones", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", arenamap::toJson());
  });

  //  POST /api/zones with the same JSON shape -> replace + persist the insert map.
  s_server.on("/api/zones", HTTP_POST,
    [](AsyncWebServerRequest* r) {
      String* body = (String*)r->_tempObject;
      bool ok = body && arenamap::fromJson(body->c_str());
      if (ok) ok = arenamap::save();
      if (body) { delete body; r->_tempObject = nullptr; }
      r->send(ok ? 200 : 400, "text/plain", ok ? "map saved" : "bad map json");
    },
    nullptr,
    [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
      if (index == 0) {
        if (r->_tempObject) delete (String*)r->_tempObject;
        String* b = new String();
        b->reserve(total + 1);
        r->_tempObject = b;
      }
      String* body = (String*)r->_tempObject;
      if (body) for (size_t i = 0; i < len; i++) body->concat((char)data[i]);
    });

  s_server.on("/api/zones/reset", HTTP_GET, [](AsyncWebServerRequest* r) {
    arenamap::reset();
    arenamap::save();
    r->send(200, "application/json", arenamap::toJson());
  });

  // --- WiFi provisioning: /api/wifi?ssid=...&pass=... then reboot ---------
  s_server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!r->hasParam("ssid")) {
      r->send(400, "text/plain", "ssid= [&pass=]");
      return;
    }
    s_prefs.putString("ssid", r->getParam("ssid")->value());
    s_prefs.putString("pass", r->hasParam("pass") ? r->getParam("pass")->value() : String(""));
    r->send(200, "text/plain", "saved, rebooting");
    delay(200);
    ESP.restart();
  });

  // --- OTA: POST a firmware .bin (fails gracefully if there is no OTA slot) ---
  s_server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest* r) {
      bool ok = !Update.hasError();
      r->send(ok ? 200 : 500, "text/plain", ok ? "OK, rebooting" : "FAIL");
      if (ok) { delay(200); ESP.restart(); }
    },
    [](AsyncWebServerRequest* r, String fn, size_t idx, uint8_t* data, size_t len, bool done) {
      if (!idx) {
        Serial.printf("[ota] start %s\n", fn.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      }
      if (Update.write(data, len) != len) Update.printError(Serial);
      if (done) {
        if (Update.end(true)) Serial.printf("[ota] ok %u bytes\n", (unsigned)(idx + len));
        else Update.printError(Serial);
      }
    });

  s_server.serveStatic("/", LittleFS, "/");
  s_server.onNotFound([](AsyncWebServerRequest* r) { r->send(404, "text/plain", "404"); });
  s_server.begin();
  Serial.println("[net] http server up");
}

void loop() {
  // Nothing periodic: ESPAsyncWebServer runs on its own task and the LED engine
  // is driven from the main loop. Kept so main.cpp reads the same as the others.
}

}  // namespace arenanet
