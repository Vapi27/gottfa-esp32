#include <WiFi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include "board_config.h"
#include "net.h"
#include "diag.h"

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
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest *r){ r->send(404, "text/plain", "404"); });
  server.begin();
  Serial.println("[net] HTTP + WS on :80");
}

void netLoop() { ws.cleanupClients(); diag::tick(); }
