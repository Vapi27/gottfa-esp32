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
      bool ok = (id >= 0 && id <= 31) && wavplayer::play(id);
      r->send(ok ? 200 : 400, "text/plain", ok ? ("play " + String(id)) : "bad id (0..31) or not ready");
      return;
    }
    if (r->hasParam("theme")) {
      wavplayer::setTheme(r->getParam("theme")->value().c_str());
      r->send(200, "text/plain", "theme -> " + r->getParam("theme")->value());
      return;
    }
    if (r->hasParam("stop")) { wavplayer::stopAll(); r->send(200, "text/plain", "stopped"); return; }
    r->send(200, "text/plain", String("PSOWAV ") + (wavplayer::ready() ? "ready" : "NOT ready") +
            "\nusage: /snd?id=N (0..31) | /snd?theme=NAME | /snd?stop=1");
#else
    r->send(501, "text/plain", "no sound tier on C3");
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
