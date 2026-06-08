#include <WiFi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include "board_config.h"
#include "net.h"
#include "diag.h"
#ifndef BOARD_C3
#include "wavplayer.h"
#include "romstore.h"
#include "romcrypt.h"
#include "epromdump.h"
#include <string.h>
// scratch buffer for a /romup POST body (auto-freed by AsyncWebServerRequest::_tempObject)
struct RomUp { uint32_t cap; uint32_t got; uint8_t data[1]; };
#endif

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static uint32_t s_idcode = 0;
static String   s_mode = "init";
static String   s_ip   = "0.0.0.0";

void netSetFpgaIdcode(uint32_t id) { s_idcode = id; }

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:    Serial.printf("[ws] #%u connected\n", client->id()); diag::onConnect(client); break;
    case WS_EVT_DISCONNECT: Serial.printf("[ws] #%u left\n", client->id()); break;
    case WS_EVT_DATA: {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
        diag::onText(client, (const char *)data, len);
      break;
    }
    default: break;
  }
}

void netBegin() {
  WiFi.persistent(false);
  bool connected = false;
  if (strlen(WIFI_STA_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(MDNS_HOST);
    Serial.printf("[net] STA connecting to '%s' ...\n", WIFI_STA_SSID);
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_STA_TIMEOUT_MS) { delay(250); Serial.print('.'); }
    Serial.println();
    connected = (WiFi.status() == WL_CONNECTED);
  }
  if (connected) { s_mode = "STA"; s_ip = WiFi.localIP().toString(); Serial.printf("[net] STA OK ip=%s\n", s_ip.c_str()); }
  else { WiFi.mode(WIFI_AP); WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS); s_mode = "SoftAP"; s_ip = WiFi.softAPIP().toString();
         Serial.printf("[net] SoftAP '%s' ip=%s\n", WIFI_AP_SSID, s_ip.c_str()); }

  if (MDNS.begin(MDNS_HOST)) { MDNS.addService("http", "tcp", 80); Serial.printf("[net] http://%s.local/\n", MDNS_HOST); }

  diag::begin();
  diag::setInfo(FW_VERSION, s_idcode, s_mode.c_str(), s_ip.c_str());
  diag::attach(&ws);

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // --- PSOWAV sound test (bring-up): play any sound / set theme from a browser, no FPGA ---
  //   /snd?id=N      play PSOWAV sound N (0..31)      /snd?theme=arena   load a game set
  //   /snd?stop=1    stop all voices                  /snd               usage + status
  server.on("/snd", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (r->hasParam("id")) {
      int id = r->getParam("id")->value().toInt();
      bool ok = (id >= 0 && id <= 95) && wavplayer::play(id);
      r->send(ok ? 200 : 400, "text/plain", ok ? ("play " + String(id)) : "bad id (0..95) or not ready");
      return;
    }
    if (r->hasParam("theme")) {
      wavplayer::setTheme(r->getParam("theme")->value().c_str());
      r->send(200, "text/plain", "theme -> " + r->getParam("theme")->value());
      return;
    }
    if (r->hasParam("stop")) { wavplayer::stopAll(); r->send(200, "text/plain", "stopped"); return; }
    r->send(200, "text/plain", String("PSOWAV ") + (wavplayer::ready() ? "ready" : "NOT ready") +
            "\nusage: /snd?id=N (0..95) | /snd?theme=NAME | /snd?stop=1");
#else
    r->send(501, "text/plain", "no sound tier on C3");
#endif
  });

  // --- bench game-select: load a game's PSOWAV sound set without an FPGA token ---
  //   GET /game?id=N   (N = FPGA game No 0..62 = DIP S1 = games.txt index)
  server.on("/game", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (r->hasParam("id")) {
      int id = r->getParam("id")->value().toInt();
      wavplayer::selectGame(id);
      r->send(200, "text/plain", "game -> " + String(id));
    } else r->send(400, "text/plain", "usage: /game?id=N (0..62)");
#else
    r->send(501, "text/plain", "no sound tier on C3");
#endif
  });

  // --- EPROM reader (optional daughterboard): dump the user's own chip to /dumps/<name>.bin ---
  //   GET /dump?type=2716|2732|2764[&name=foo]   (see EPROM_READER.md)
  server.on("/dump", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (!epromdump::available()) { r->send(503, "text/plain", "EPROM reader disabled (set EPROM_READER_ENABLE=1 + fit the board)"); return; }
    epromdump::Type t = epromdump::T2764;
    if (r->hasParam("type")) { String s = r->getParam("type")->value();
      t = (s == "2716") ? epromdump::T2716 : (s == "2732") ? epromdump::T2732 :
          (s == "u2")   ? epromdump::T2332_U2 : (s == "u3") ? epromdump::T2332_U3 : epromdump::T2764; }
    String name = r->hasParam("name") ? r->getParam("name")->value() : String("dump");
    String path = "/dumps/" + name + ".bin";
    bool ok = epromdump::dumpToSD(t, path.c_str());
    r->send(ok ? 200 : 500, "text/plain", ok ? ("dumped -> " + path) : "dump failed");
#else
    r->send(501, "text/plain", "no SD on C3");
#endif
  });

  // --- ROM store: list per-game variants + device key fingerprint + global Free-Play setting ---
  //   GET /roms -> {"key":"<hex>","fp":0|1,"games":[{"n":N,"s":stock,"se":stockEnc,"f":fp,"fe":fpEnc}]}
  server.on("/roms", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    String j = "{\"key\":\"" + String(romcrypt::keyId(), HEX) + "\",\"fp\":" +
               (romstore::freePlay() ? "1" : "0") + ",\"games\":[";
    bool first = true;
    for (int i = 0; i < romstore::MAX_GAME; i++) {
      bool s = romstore::has(i, false), f = romstore::has(i, true);
      if (!s && !f) continue;
      if (!first) j += ',';
      first = false;
      j += "{\"n\":" + String(i) +
           ",\"s\":"  + (s ? "1" : "0") + ",\"se\":" + (romstore::encrypted(i, false) ? "1" : "0") +
           ",\"f\":"  + (f ? "1" : "0") + ",\"fe\":" + (romstore::encrypted(i, true)  ? "1" : "0") + "}";
    }
    j += "]}";
    r->send(200, "application/json", j);
#else
    r->send(501, "text/plain", "no store on C3");
#endif
  });

  // --- Free-Play device setting: which ROM variant is served to the FPGA ---
  //   GET /fp -> {"fp":0|1}   ;   GET /fp?set=0|1 -> set then return the new state
  server.on("/fp", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (r->hasParam("set")) romstore::setFreePlay(r->getParam("set")->value().toInt() != 0);
    r->send(200, "application/json", String("{\"fp\":") + (romstore::freePlay() ? "1" : "0") + "}");
#else
    r->send(501, "text/plain", "no store on C3");
#endif
  });

  // --- ROM upload: POST a raw 16384-byte GottFA game image -> ENCRYPTED into /roms/<NN>.img ---
  //   POST /romup?id=N[&fp=1]   body = exactly 16384 bytes (the user supplies their own ROM).
  //   Encryption is device-bound (romcrypt) anti-extraction; it does NOT change legality.
  server.on("/romup", HTTP_POST,
    [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
      RomUp *u = (RomUp *)r->_tempObject;
      if (!u || u->got != u->cap) { r->send(400, "text/plain", "need exactly 16384 bytes"); return; }
      if (!r->hasParam("id")) { r->send(400, "text/plain", "missing ?id=N"); return; }
      int id = r->getParam("id")->value().toInt();
      bool fp = (r->hasParam("fp") && r->getParam("fp")->value().toInt() != 0);
      bool ok = romstore::store(id, fp, u->data);
      r->send(ok ? 200 : 500, "text/plain",
              ok ? ("stored game " + String(id) + (fp ? " (Free Play)" : " (stock)")) : "store failed (key? SD?)");
#else
      r->send(501, "text/plain", "no store on C3");
#endif
    },
    NULL,
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
#ifndef BOARD_C3
      if (index == 0) {
        if (total != (size_t)romstore::IMG_SIZE) return;     // wrong size -> reject in completion
        RomUp *u = (RomUp *)malloc(sizeof(RomUp) - 1 + total);
        if (!u) return;
        u->cap = (uint32_t)total; u->got = 0;
        r->_tempObject = u;
      }
      RomUp *u = (RomUp *)r->_tempObject;
      if (u && index + len <= u->cap) { memcpy(u->data + index, data, len); u->got += len; }
#endif
    });

  // --- Déploiement: OTA firmware update (POST a firmware .bin). Fails gracefully if the
  //     partition scheme has no OTA slot (Update.begin returns false) -> never bricks. To
  //     enable real OTA: set board_build.partitions to an OTA scheme + one USB flash first.
  server.on("/ota", HTTP_POST,
    [](AsyncWebServerRequest *r){
      bool ok = !Update.hasError();
      AsyncWebServerResponse *res = r->beginResponse(200, "text/plain", ok ? "OK — redémarrage…" : "ÉCHEC OTA (partition ?)");
      res->addHeader("Connection","close"); r->send(res);
      if (ok) { delay(150); ESP.restart(); }
    },
    [](AsyncWebServerRequest *r, String fn, size_t idx, uint8_t *data, size_t len, bool done){
      if (!idx) { Serial.printf("[ota] begin %s\n", fn.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial); }
      if (Update.write(data, len) != len) Update.printError(Serial);
      if (done) { if (Update.end(true)) Serial.printf("[ota] ok %u bytes\n", (unsigned)(idx+len));
                  else Update.printError(Serial); }
    });

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest *r){ r->send(404, "text/plain", "404"); });
  server.begin();
  Serial.println("[net] HTTP + WS on :80");
}

void netLoop() { ws.cleanupClients(); diag::tick(); }
const char* netIp()   { return s_ip.c_str(); }
const char* netMode() { return s_mode.c_str(); }
