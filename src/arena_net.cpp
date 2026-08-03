#include <WiFi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_system.h>
#include "arena_config.h"
#include "arena_net.h"
#include "arenaled.h"
#include "arena_map.h"

namespace arenanet {

static AsyncWebServer s_server(80);
static Preferences    s_prefs;
static String         s_ip   = "0.0.0.0";
static String         s_mode = "init";
static String         s_ap;              // SoftAP SSID, suffixed per device
static String         s_name;            // customer-visible device name
static String         s_host;            // mDNS host derived from the name
static uint8_t        s_radioPhase = 0;  // 0 idle · 1 arming · 2 radio off
static uint32_t       s_radioAt    = 0;
static uint16_t       s_radioSec   = 0;
static const char*    s_reset      = "?";   // why the last boot happened
static uint32_t       s_boots      = 0;     // boots since the last factory reset

// A brief white flash across the WHOLE string is also what a controller reset
// looks like: the data pin goes high-impedance while the chain is listening, the
// pixels latch noise, and the firmware then repaints. So the reset cause and a
// boot counter are first-class diagnostics here, not housekeeping. If the counter
// climbs every time the wall flashes, nothing about the LED signal is wrong — the
// controller is dropping out (brownout on a weak USB feed, watchdog, panic).
static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external reset";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "PANIC (crash)";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply dipped)";
    case ESP_RST_DEEPSLEEP:return "deep sleep wake";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "unknown";
  }
}

const char* ip()   { return s_ip.c_str(); }
const char* mode() { return s_mode.c_str(); }

// Last two bytes of the eFuse MAC. Stable for the life of the board, and the
// only thing distinguishing two units out of the same batch.
static String deviceSuffix() {
  char b[5];
  snprintf(b, sizeof(b), "%04X", (uint16_t)(ESP.getEfuseMac() >> 32));
  return String(b);
}

static String deviceId() {
  uint64_t m = ESP.getEfuseMac();
  char b[13];
  snprintf(b, sizeof(b), "%04X%08X", (uint16_t)(m >> 32), (uint32_t)m);
  return String(b);
}

// A customer types "Arena Wall" as the device name; mDNS needs "arena-wall".
static String hostFromName(const String& n) {
  String h;
  for (size_t i = 0; i < n.length() && h.length() < 24; i++) {
    char c = n[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) h += c;
    else if ((c == ' ' || c == '-' || c == '_') && h.length() && h[h.length()-1] != '-') h += '-';
  }
  while (h.length() && h[h.length()-1] == '-') h.remove(h.length() - 1);
  return h.length() ? h : String(ARENA_MDNS_HOST);
}

// Served when LittleFS is empty (nobody ran `pio run -e arenaled -t uploadfs` yet):
// enough UI to prove the chain lights up and to reach the mode buttons.
static const char FALLBACK[] PROGMEM =
  "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>" PRODUCT_BRAND " " PRODUCT_NAME "</title>"
  "<style>body{background:#0d0b09;color:#efe6da;font:16px system-ui;padding:2em}"
  "h1{color:#ffc046;font-size:20px;letter-spacing:.12em;text-transform:uppercase}"
  "a{display:inline-block;margin:.3em;padding:.6em 1em;background:#ff9b21;color:#150f08;"
  "font-weight:600;text-decoration:none;border-radius:6px}</style>"
  "<h1>" PRODUCT_BRAND " &mdash; " PRODUCT_NAME "</h1>"
  "<p>Web interface not installed. Run <code>pio run -t uploadfs</code>, "
  "or use the controls below.</p>"
  "<p><a href='/api/set?mode=classic'>Classic</a><a href='/api/set?mode=attract'>Attract</a>"
  "<a href='/api/set?mode=playfield'>Playfield</a><a href='/api/set?mode=night'>Night</a>"
  "<a href='/api/set?mode=rainbow'>Rainbow</a><a href='/api/set?mode=test'>Wiring test</a>"
  "<a href='/api/set?mode=off'>Off</a></p><p><a href='/api/state'>/api/state</a></p>";

static String stateJson() {
  arenaled::Rgbw c = arenaled::color();
  String j = "{";
  j += "\"fw\":\"" ARENA_FW_VERSION "\"";
  j += ",\"mode\":\""  + String(arenaled::modeName(arenaled::mode()))  + "\"";
  j += ",\"modeLabel\":\"" + String(arenaled::modeLabel(arenaled::mode())) + "\"";
  j += ",\"name\":\""  + s_name + "\"";
  j += ",\"bright\":" + String(arenaled::brightness());
  j += ",\"speed\":"  + String(arenaled::speed());
  j += ",\"count\":"  + String(arenaled::count());
  j += ",\"max\":"    + String(LED_MAX);
  j += ",\"r\":" + String(c.r) + ",\"g\":" + String(c.g) +
       ",\"b\":" + String(c.b) + ",\"w\":" + String(c.w);
  j += ",\"amps\":"   + String(arenaled::lastAmps(), 2);
  j += ",\"budget\":" + String(arenaled::budgetMa());
  j += ",\"hz\":"     + String(arenaled::hz());
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
  s_name = s_prefs.getString("name", PRODUCT_NAME);
  s_reset = resetReasonName(esp_reset_reason());
  s_boots = s_prefs.getUInt("boots", 0) + 1;
  s_prefs.putUInt("boots", s_boots);
  Serial.printf("[dev] boot #%lu — last reset: %s\n", (unsigned long)s_boots, s_reset);
  s_ap   = String(ARENA_AP_PREFIX) + "-" + deviceSuffix();
  s_host = hostFromName(s_prefs.getString("name", ""));
  Serial.printf("[dev] %s %s v%s  id=%s  name='%s'\n", PRODUCT_BRAND, PRODUCT_MODEL,
                ARENA_FW_VERSION, deviceId().c_str(), s_name.c_str());

  WiFi.persistent(false);
  bool connected = false;
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(s_host.c_str());
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
    WiFi.softAP(s_ap.c_str(), ARENA_AP_PASS);
    s_mode = "SoftAP";
    s_ip = WiFi.softAPIP().toString();
    Serial.printf("[net] SoftAP '%s' ip=%s\n", s_ap.c_str(), s_ip.c_str());
  }
  if (MDNS.begin(s_host.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "brand", PRODUCT_BRAND);
    MDNS.addServiceTxt("http", "tcp", "model", PRODUCT_MODEL);
    MDNS.addServiceTxt("http", "tcp", "fw", ARENA_FW_VERSION);
    Serial.printf("[net] http://%s.local/\n", s_host.c_str());
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
    if (r->hasParam("hz"))     arenaled::setHz((uint8_t)r->getParam("hz")->value().toInt());
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

  // --- Identity: everything a support request needs in one call ------------
  s_server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest* r) {
    String j = "{\"brand\":\"" PRODUCT_BRAND "\",\"product\":\"" PRODUCT_NAME "\"";
    j += ",\"model\":\"" PRODUCT_MODEL "\",\"fw\":\"" ARENA_FW_VERSION "\"";
    j += ",\"built\":\"" __DATE__ " " __TIME__ "\"";
    j += ",\"id\":\""   + deviceId() + "\"";
    j += ",\"name\":\"" + s_name + "\"";
    j += ",\"host\":\"" + s_host + ".local\"";
    j += ",\"ap\":\""   + s_ap + "\"";
    j += ",\"net\":\""  + s_mode + "\",\"ip\":\"" + s_ip + "\"";
    j += ",\"chip\":\"" + String(ESP.getChipModel()) + "\"";
    j += ",\"cores\":"   + String(ESP.getChipCores());
    j += ",\"flash\":"   + String(ESP.getFlashChipSize());
    j += ",\"heap\":"    + String(ESP.getFreeHeap());
    j += ",\"maxLeds\":" + String(LED_MAX);
    j += ",\"pin\":"     + String(PIN_LED_DATA);
    j += ",\"reset\":\"" + String(s_reset) + "\"";
    j += ",\"boots\":"    + String(s_boots);
    j += ",\"up\":"      + String(millis() / 1000) + "}";
    r->send(200, "application/json", j);
  });

  // --- Device name: shown in the UI and used for mDNS (reboot to re-announce) ---
  s_server.on("/api/name", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!r->hasParam("v")) { r->send(400, "text/plain", "v=<name>"); return; }
    String v = r->getParam("v")->value();
    v.trim();
    if (v.length() > 31) v = v.substring(0, 31);
    s_prefs.putString("name", v);
    s_name = v.length() ? v : String(PRODUCT_NAME);
    r->send(200, "application/json",
            String("{\"name\":\"") + s_name + "\",\"host\":\"" + hostFromName(v) +
            ".local\",\"note\":\"reboot to re-announce mDNS\"}");
  });

  // --- Factory reset: wipe every stored setting and the insert map ----------
  s_server.on("/api/factory", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!r->hasParam("confirm")) {
      r->send(400, "text/plain", "add ?confirm=1 — this erases all settings, WiFi and the insert map");
      return;
    }
    s_prefs.clear();                       // WiFi + device name + boot counter
    Preferences p;
    if (p.begin("arena", false)) { p.clear(); p.end(); }   // mode/brightness/colour/count
    LittleFS.remove(ARENA_MAP_PATH);
    r->send(200, "text/plain", "factory reset — rebooting");
    delay(300);
    ESP.restart();
  });

  // --- Signal health: the counters that decide timing vs electrical ---------
  //   /api/health          read      /api/health?reset=1   zero the counters
  s_server.on("/api/health", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (r->hasParam("reset")) arenaled::resetHealth();
    arenaled::Health h = arenaled::health();
    String j = "{\"showUs\":"     + String(h.showUs);
    j += ",\"showMaxUs\":"        + String(h.showMaxUs);
    j += ",\"showExpUs\":"        + String(h.showExpUs);
    j += ",\"lateShow\":"         + String(h.lateShow);
    j += ",\"lateFrame\":"        + String(h.lateFrame);
    j += ",\"maxGapMs\":"         + String(h.maxGapMs);
    j += ",\"sinceLateMs\":"      + String(h.sinceLateMs);
    j += ",\"frames\":"           + String(h.frames);
    j += ",\"up\":"               + String(millis() / 1000) + "}";
    r->send(200, "application/json", j);
  });

  // --- Radio-off test: the one experiment that separates the two causes of a
  //     whole-chain colour glitch. If the flashes STOP while the radio is off,
  //     the refresh was being starved by the WiFi/TCP stack; if they continue,
  //     the fault is electrical (data level margin, wiring, grounding).
  //     The unit reboots at the end of the window to bring the radio back cleanly.
  s_server.on("/api/radiotest", HTTP_GET, [](AsyncWebServerRequest* r) {
    uint32_t sec = r->hasParam("sec") ? (uint32_t)r->getParam("sec")->value().toInt() : 30;
    if (sec < 5)   sec = 5;
    if (sec > 300) sec = 300;
    s_radioSec = (uint16_t)sec;
    s_radioAt  = millis() + 400;          // let this response leave first
    s_radioPhase = 1;
    r->send(200, "text/plain",
            String("WiFi off for ") + sec + " s — watch the LEDs now.\n"
            "Flashes stop  -> the radio was starving the refresh (firmware/timing).\n"
            "Flashes stay  -> electrical: data level margin, wiring or grounding.\n"
            "The unit reboots when the window ends.");
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
  // Only the radio-off diagnostic window needs servicing here: the web server has
  // its own task, and (on dual-core parts) so does the renderer.
  if (!s_radioPhase) return;
  if ((int32_t)(millis() - s_radioAt) < 0) return;

  if (s_radioPhase == 1) {
    Serial.printf("[net] radio OFF for %u s (glitch test)\n", s_radioSec);
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    s_radioPhase = 2;
    s_radioAt = millis() + (uint32_t)s_radioSec * 1000;
  } else {
    Serial.println("[net] test window over — rebooting to restore the radio");
    delay(50);
    ESP.restart();
  }
}

}  // namespace arenanet
