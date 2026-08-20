// wifiprov.cpp — see wifiprov.h. SoftAP + captive portal + NVS credentials.
// (C) 2026 Valere Pillet / Pstore. Original implementation.
#include "wifiprov.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include "board_config.h"
#include "wifiprov_page.h"

// ---- tunables ---------------------------------------------------------------------------------
// The SoftAP name gets a per-board suffix (last 3 bytes of the WiFi MAC) so two machines in the
// same house do not collide, and so the owner can tell them apart from a phone.
#ifndef WIFIPROV_AP_BASE
#define WIFIPROV_AP_BASE "GottFA80"
#endif
// Factory AP password. WPA2, so >= 8 chars — an OPEN AP is not an option here: the control surface
// behind it (OTA, NOR reflash, FPGA reconfiguration) is unauthenticated, so an open hotspot would
// hand the street a reprogrammable pinball. This constant is the documented factory default (see
// WIFI_SETUP.md); the customer can change it from the portal, and a builder can override it per
// batch with -DWIFIPROV_AP_PASS_DEFAULT='"..."'.
#ifndef WIFIPROV_AP_PASS_DEFAULT
#define WIFIPROV_AP_PASS_DEFAULT "pinball80"
#endif
// Factory-reset gesture: hold the ESP32-S3 DevKit BOOT button (GPIO0) for 5 s WHILE RUNNING.
// WHY not "hold at power-up", which would be the obvious gesture: GPIO0 is a strapping pin, and a
// low level at reset release puts the chip in ROM download mode — the firmware never runs, so it
// could never see the hold. A runtime hold is the only version that actually works on this chip.
// GPIO0 is free on the S3 map (JTAG TCK is GPIO4 there); on the C3 it IS PIN_JTAG_TCK, so the
// button is compiled out on that target and only the web action remains.
#ifndef BOARD_C3
#define WIFIPROV_BTN_PIN 0
#endif

namespace {

  const uint32_t STA_BOOT_MS   = WIFI_STA_TIMEOUT_MS;  // boot-time connect budget (blocking)
  const uint32_t TRY_MS        = 16000;  // portal-driven attempt budget
  const uint32_t LOST_GRACE_MS = 20000;  // STA down this long -> raise the AP again
  const uint32_t RETRY_MS      = 30000;  // while the link is lost, re-arm WiFi.begin() this often
  const uint32_t AP_LINGER_MS  = 30000;  // keep the AP alive after a success so the page can read
  const uint32_t SCAN_TTL_MS   = 20000;  // cached scan results considered fresh for this long
  const uint32_t BTN_HOLD_MS   = 5000;   // BOOT hold -> factory reset
  const uint8_t  SCAN_MAX      = 24;     // networks published to the portal
  const size_t   SCAN_BUF      = 1600;   // per scan buffer (double-buffered, see below)

  enum Res : uint8_t { RES_NONE = 0, RES_TRYING, RES_OK, RES_FAIL };
  enum Cmd : uint8_t { CMD_NONE = 0, CMD_CONNECT, CMD_APONLY, CMD_APPASS, CMD_FORGET, CMD_SCAN };

  // ---- radio / config state (owned by tick(), read by the handlers) ----------------------------
  bool     g_apUp        = false;
  bool     g_staUp       = false;
  bool     g_apOnly      = false;   // user explicitly chose "hotspot only, forever"
  bool     g_done        = false;   // wizard answered (creds saved OR AP-only chosen)
  bool     g_apPassDflt  = true;    // still the factory AP password -> nag in the portal
  bool     g_fsHasUi     = false;   // LittleFS holds an /index.html (else the portal owns "/")
  bool     g_mdnsUp      = false;   // caller started mDNS; re-announce it on a later STA-up
  uint8_t  g_res         = RES_NONE;
  uint32_t g_tryStart    = 0;
  uint32_t g_lostSince   = 0;
  uint32_t g_lastRetry   = 0;
  uint32_t g_apDropAt    = 0;       // != 0: drop the AP at this millis() (post-success linger)
  uint32_t g_fsCheckAt   = 0;
  uint32_t g_btnDown     = 0;
  volatile int g_reason  = 0;       // last STA disconnect reason (set from the WiFi event task)

  // Fixed buffers instead of String for everything a handler can read: the handlers run in the
  // AsyncTCP task while tick() writes from the loop task, and a String reallocating under a
  // concurrent reader is a use-after-free. A char array can at worst be read mid-update, which
  // yields an odd label for one poll — never a crash.
  char g_ip[16]       = "0.0.0.0";
  char g_mode[10]     = "init";
  char g_msg[112]     = "";
  char g_staSsid[33]  = "";
  char g_staPass[65]  = "";         // NEVER printed, never returned by any route
  char g_trySsid[33]  = "";
  char g_tryPass[65]  = "";         // held only for the duration of one attempt, then wiped
  char g_apSsid[33]   = "";
  char g_apPass[65]   = "";

  // ---- scan results: double-buffered so a reader never sees a half-written list ---------------
  char     g_scan[2][SCAN_BUF] = {{'\0'}, {'\0'}};
  volatile uint8_t g_scanPub   = 0;      // index of the published (readable) buffer
  bool     g_scanBusy          = false;
  bool     g_scanWant          = false;
  uint32_t g_scanAt            = 0;

  // ---- handler -> tick() command queue --------------------------------------------------------
  portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
  volatile uint8_t g_cmd = CMD_NONE;
  char g_cmdSsid[33] = "";
  char g_cmdPass[65] = "";

  DNSServer   g_dns;
  Preferences g_nvs;

  void setStr(char *dst, size_t cap, const char *src) {
    if (!src) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
  }

  // Queue an intent from an HTTP handler. Returns false if one is already pending (tick() runs
  // every couple of ms, so this window is tiny — it is a real "busy", not a lost command).
  bool queueCmd(uint8_t c, const char *ssid = nullptr, const char *pass = nullptr) {
    bool ok = false;
    portENTER_CRITICAL(&g_mux);
    if (g_cmd == CMD_NONE) {
      setStr(g_cmdSsid, sizeof g_cmdSsid, ssid);
      setStr(g_cmdPass, sizeof g_cmdPass, pass);
      g_cmd = c;                     // published last: a reader either sees NONE or a full command
      ok = true;
    }
    portEXIT_CRITICAL(&g_mux);
    return ok;
  }

  uint8_t takeCmd(char *ssid, size_t sl, char *pass, size_t pl) {
    uint8_t c;
    portENTER_CRITICAL(&g_mux);
    c = g_cmd;
    if (c != CMD_NONE) {
      setStr(ssid, sl, g_cmdSsid);
      setStr(pass, pl, g_cmdPass);
      memset(g_cmdPass, 0, sizeof g_cmdPass);   // don't leave a plaintext password lying around
      g_cmd = CMD_NONE;
    }
    portEXIT_CRITICAL(&g_mux);
    return c;
  }

  // ---- NVS ------------------------------------------------------------------------------------
  // Namespace "wifiprov" in the NVS partition. Deliberately NOT LittleFS: /fsup rewrites the whole
  // filesystem image, so a routine web-UI update would otherwise wipe the customer's WiFi setup.
  void loadCfg() {
    char macs[8] = "000000";
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);                    // works before WiFi.mode() (falls back to esp_read_mac)
    snprintf(macs, sizeof macs, "%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(g_apSsid, sizeof g_apSsid, "%s-%s", WIFIPROV_AP_BASE, macs);

    setStr(g_apPass, sizeof g_apPass, WIFIPROV_AP_PASS_DEFAULT);
    g_apPassDflt = true;
    if (g_nvs.begin("wifiprov", true)) {
      String s = g_nvs.getString("ssid", "");
      String p = g_nvs.getString("pass", "");
      String a = g_nvs.getString("appass", "");
      g_apOnly = g_nvs.getBool("aponly", false);
      g_done   = g_nvs.getBool("done", false);
      g_nvs.end();
      setStr(g_staSsid, sizeof g_staSsid, s.c_str());
      setStr(g_staPass, sizeof g_staPass, p.c_str());
      if (a.length() >= 8) { setStr(g_apPass, sizeof g_apPass, a.c_str()); g_apPassDflt = false; }
    }
  }

  void saveSta(const char *ssid, const char *pass) {
    if (!g_nvs.begin("wifiprov", false)) return;
    g_nvs.putString("ssid", ssid);
    g_nvs.putString("pass", pass);           // written, never read back out over HTTP
    g_nvs.putBool("aponly", false);
    g_nvs.putBool("done", true);
    g_nvs.end();
    setStr(g_staSsid, sizeof g_staSsid, ssid);
    setStr(g_staPass, sizeof g_staPass, pass);
    g_apOnly = false;
    g_done   = true;
  }

  void saveFlag(const char *key, bool v) {
    if (!g_nvs.begin("wifiprov", false)) return;
    g_nvs.putBool(key, v);
    g_nvs.end();
  }

  void saveApPass(const char *p) {
    if (!g_nvs.begin("wifiprov", false)) return;
    g_nvs.putString("appass", p);
    g_nvs.end();
    setStr(g_apPass, sizeof g_apPass, p);
    g_apPassDflt = false;
  }

  // ---- radio ----------------------------------------------------------------------------------
  void refreshIdentity() {
    g_staUp = (WiFi.status() == WL_CONNECTED);
    if (g_staUp && g_apUp)      setStr(g_mode, sizeof g_mode, "STA+AP");
    else if (g_staUp)           setStr(g_mode, sizeof g_mode, "STA");
    else if (g_apUp)            setStr(g_mode, sizeof g_mode, "SoftAP");
    else                        setStr(g_mode, sizeof g_mode, "down");
    IPAddress a = g_staUp ? WiFi.localIP() : WiFi.softAPIP();
    setStr(g_ip, sizeof g_ip, a.toString().c_str());
  }

  void apUp() {
    if (g_apUp) return;
    // AP_STA only when a home network is in play at all — in AP-only mode the STA interface would
    // just sit there enabled and never used.
    WiFi.mode((!g_apOnly && ((WiFi.status() == WL_CONNECTED) || g_staSsid[0])) ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(g_apSsid, g_apPass);         // passphrase >= 8 chars => WPA2-PSK, never open
    WiFi.softAPsetHostname(MDNS_HOST);
    g_apUp = true;
    // Captive DNS: answer EVERY name with our own address so the phone's connectivity probe lands
    // on us and pops the "sign in to this network" sheet instead of silently failing.
    g_dns.setErrorReplyCode(DNSReplyCode::NoError);
    g_dns.start(53, "*", WiFi.softAPIP());
    refreshIdentity();
    Serial.printf("[wifiprov] SoftAP '%s' up, ip=%s\n", g_apSsid, g_ip);
  }

  void apDown() {
    if (!g_apUp) return;
    g_dns.stop();
    WiFi.softAPdisconnect(true);
    g_apUp = false;
    g_apDropAt = 0;
    if (g_res == RES_OK) g_res = RES_NONE;   // the success banner has done its job
    refreshIdentity();
    Serial.println("[wifiprov] SoftAP down (home link is up)");
  }

  // Start an STA attempt. Never logs the password — only its length, which is enough to debug a
  // "did the field reach the board" question without leaking anything.
  void staBegin(const char *ssid, const char *pass) {
    Serial.printf("[wifiprov] STA -> '%s' (pass %u chars)\n", ssid, (unsigned)strlen(pass));
    g_reason = 0;
    WiFi.disconnect(false, true);            // drop any half-open association, clear old config
    WiFi.mode(g_apUp ? WIFI_AP_STA : WIFI_STA);
    WiFi.setHostname(MDNS_HOST);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid, pass);
  }

  // Turn an IDF disconnect reason into something a pinball owner can act on (French, like the UI).
  void explainFailure() {
    int r = g_reason;
    if (r == WIFI_REASON_NO_AP_FOUND)
      setStr(g_msg, sizeof g_msg, "reseau introuvable (hors de portee, ou nom mal orthographie)");
    else if (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_HANDSHAKE_TIMEOUT ||
             r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT)
      setStr(g_msg, sizeof g_msg, "mot de passe refuse par la box");
    else if (r == WIFI_REASON_ASSOC_FAIL)
      setStr(g_msg, sizeof g_msg, "association refusee par la box (filtrage MAC ? box pleine ?)");
    else if (r)
      snprintf(g_msg, sizeof g_msg, "la box a coupe la connexion (code %d)", r);
    else
      setStr(g_msg, sizeof g_msg, "delai depasse, aucune reponse du reseau");
  }

  // mDNS is started once by the caller (netBegin) on whatever interface was up at boot. If the
  // link changes later — portal connect, or a reconnect after an outage — the responder has to be
  // re-armed on the new interface or http://gottfa.local/ silently stops resolving.
  void remdns() {
    if (!g_mdnsUp) return;
    MDNS.end();
    if (MDNS.begin(MDNS_HOST)) MDNS.addService("http", "tcp", 80);
  }

  // ---- scan -----------------------------------------------------------------------------------
  void jsonEscape(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 7 < cap; i++) {
      unsigned char c = (unsigned char)src[i];
      if (c == '"' || c == '\\')      { dst[o++] = '\\'; dst[o++] = (char)c; }
      else if (c < 0x20)              { o += snprintf(dst + o, cap - o, "\\u%04X", c); }
      else                             { dst[o++] = (char)c; }
    }
    dst[o] = '\0';
  }

  void scanStart() {
    if (g_scanBusy) return;
    // Scanning hops the radio across channels; doing it while an association attempt is in flight
    // makes the attempt fail for no reason. Defer instead.
    if (g_res == RES_TRYING) { g_scanWant = true; return; }
    g_scanWant = false;
    // The station interface MUST be up to scan. On a virgin board there are no stored
    // credentials, so applyMode() leaves the radio in plain WIFI_AP — and scanNetworks()
    // then returns nothing at all, which reads to the operator as "no networks in range"
    // on the one screen whose whole job is to list them. Ground-truthed on hardware
    // 2026-07-30: first boot, empty list. Add STA alongside the AP for the scan; the AP
    // stays up so the phone filling in the form is not dropped.
    if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);
    g_scanBusy = true;
    WiFi.scanNetworks(true /*async*/, true /*show hidden*/);
  }

  void scanCollect(int n) {
    uint8_t back = (uint8_t)(g_scanPub ^ 1);
    char *b = g_scan[back];
    size_t o = 0;
    o += snprintf(b + o, SCAN_BUF - o, "[");
    uint8_t out = 0;
    for (int i = 0; i < n && out < SCAN_MAX; i++) {
      String ss = WiFi.SSID(i);
      if (!ss.length()) continue;                       // hidden: nothing to click, type it instead
      bool dup = false;                                 // same SSID on 2.4 + 5 GHz -> keep the best
      for (int j = 0; j < i; j++) if (WiFi.SSID(j) == ss) { dup = true; break; }
      if (dup) continue;
      char esc[97];
      jsonEscape(esc, sizeof esc, ss.c_str());
      int need = snprintf(nullptr, 0, "%s{\"s\":\"%s\",\"r\":%d,\"e\":%d}",
                          out ? "," : "", esc, (int)WiFi.RSSI(i),
                          WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1);
      if (o + (size_t)need + 2 >= SCAN_BUF) break;
      o += snprintf(b + o, SCAN_BUF - o, "%s{\"s\":\"%s\",\"r\":%d,\"e\":%d}",
                    out ? "," : "", esc, (int)WiFi.RSSI(i),
                    WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1);
      out++;
    }
    snprintf(b + o, SCAN_BUF - o, "]");
    g_scanPub = back;                                   // single volatile store = atomic publish
    g_scanAt  = millis();
    WiFi.scanDelete();
    g_scanBusy = false;
    Serial.printf("[wifiprov] scan: %d found, %u listed\n", n, (unsigned)out);
  }

  // ---- HTTP -----------------------------------------------------------------------------------
  void sendPortal(AsyncWebServerRequest *r) {
    // No _P variant needed (and it is deprecated in 3.11): on the ESP32 the flash holding a
    // PROGMEM literal is memory-mapped, so a plain pointer read is correct.
    AsyncWebServerResponse *res = r->beginResponse(200, "text/html; charset=utf-8", WIFIPROV_PAGE);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  }

  void sendJson(AsyncWebServerRequest *r, int code, const char *body) {
    AsyncWebServerResponse *res = r->beginResponse(code, "application/json", body);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  }

  const char *resName() {
    switch (g_res) {
      case RES_TRYING: return "trying";
      case RES_OK:     return "ok";
      case RES_FAIL:   return "fail";
      default:         return "none";
    }
  }

  // POST body param, falling back to the query string so the routes stay curl-friendly.
  String param(AsyncWebServerRequest *r, const char *n) {
    const AsyncWebParameter *p = r->getParam(n, true);
    if (!p) p = r->getParam(n, false);
    return p ? p->value() : String();
  }
}

namespace wifiprov {

void begin() {
  WiFi.persistent(false);           // we own persistence (our NVS namespace), not the WiFi driver
  loadCfg();

  // Capture the disconnect reason so the portal can say "wrong password" instead of "it failed".
  // Runs in the WiFi event task: store an int, nothing else.
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    g_reason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  g_fsHasUi = LittleFS.exists("/index.html");

  if (g_apOnly) {                   // explicit customer choice: hotspot only, do not nag
    Serial.println("[wifiprov] AP-only mode (saved choice)");
    apUp();
    refreshIdentity();
    return;
  }

  if (g_staSsid[0]) {
    staBegin(g_staSsid, g_staPass);
    // The ONE blocking wait in this module, and it is in setup(): the caller starts mDNS right
    // after begin() returns, and mDNS needs a live interface to bind to.
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < STA_BOOT_MS) { delay(50); }
    if (WiFi.status() == WL_CONNECTED) {
      // Deliberately NOT RES_OK: "ok" is feedback for a user-initiated attempt, and would
      // otherwise show a stale "connected, the AP is shutting down" banner months later.
      refreshIdentity();
      Serial.printf("[wifiprov] STA OK ip=%s\n", g_ip);
      return;
    }
    explainFailure();
    g_res = RES_FAIL;    // so the portal can say WHY (e.g. the owner changed their box password)
    Serial.printf("[wifiprov] STA failed: %s -> portal\n", g_msg);
  } else {
    Serial.println("[wifiprov] no saved network -> setup portal");
  }

  // Every failure path lands here. The board is ALWAYS reachable.
  apUp();
  refreshIdentity();
}

void tick() {
  uint32_t now = millis();

  if (g_apUp) g_dns.processNextRequest();     // cheap: one non-blocking UDP read

  // ---- drain one queued intent ----------------------------------------------------------------
  char ssid[33] = "", pass[65] = "";
  switch (takeCmd(ssid, sizeof ssid, pass, sizeof pass)) {
    case CMD_CONNECT:
      apUp();                                  // keep the client attached while we try
      setStr(g_trySsid, sizeof g_trySsid, ssid);
      setStr(g_tryPass, sizeof g_tryPass, pass);
      setStr(g_msg, sizeof g_msg, "");
      g_res = RES_TRYING;
      g_tryStart = now;
      g_apDropAt = 0;
      staBegin(ssid, pass);
      break;
    case CMD_APONLY:
      g_apOnly = true; g_done = true;
      if (g_nvs.begin("wifiprov", false)) {
        g_nvs.putBool("aponly", true); g_nvs.putBool("done", true); g_nvs.end();
      }
      g_res = RES_NONE;
      apUp();
      WiFi.disconnect(false, true);
      WiFi.mode(WIFI_AP);
      g_staUp = false; g_lostSince = 0;
      refreshIdentity();
      Serial.println("[wifiprov] AP-only mode chosen by the user");
      break;
    case CMD_APPASS:
      if (strlen(pass) >= 8) {
        saveApPass(pass);
        if (g_apUp) { g_apUp = false; g_dns.stop(); apUp(); }   // restart the AP with the new key
        Serial.println("[wifiprov] AP password changed");
      }
      break;
    case CMD_FORGET:
      forget();
      break;
    case CMD_SCAN:
      scanStart();
      break;
    default: break;
  }
  memset(pass, 0, sizeof pass);                // the copy on our stack has served its purpose

  // ---- async scan ----------------------------------------------------------------------------
  if (g_scanBusy) {
    int n = WiFi.scanComplete();
    if (n >= 0)               scanCollect(n);
    else if (n == WIFI_SCAN_FAILED) { WiFi.scanDelete(); g_scanBusy = false; }
  } else if (g_scanWant && g_res != RES_TRYING) {
    scanStart();
  }

  // ---- connection attempt in flight ------------------------------------------------------------
  if (g_res == RES_TRYING) {
    if (WiFi.status() == WL_CONNECTED) {
      saveSta(g_trySsid, g_tryPass);           // only NOW is it worth persisting: it actually works
      memset(g_tryPass, 0, sizeof g_tryPass);
      g_res = RES_OK;
      g_lostSince = 0;
      refreshIdentity();
      remdns();
      // Linger before dropping the AP so the phone still on the hotspot can read "connected, the
      // new address is X" — dropping it instantly would leave the customer staring at a dead page.
      g_apDropAt = now + AP_LINGER_MS;
      Serial.printf("[wifiprov] connected, ip=%s (AP drops in %us)\n", g_ip, (unsigned)(AP_LINGER_MS / 1000));
    } else if (now - g_tryStart > TRY_MS) {
      explainFailure();
      memset(g_tryPass, 0, sizeof g_tryPass);  // a rejected password is not kept anywhere
      g_res = RES_FAIL;
      Serial.printf("[wifiprov] attempt failed: %s\n", g_msg);
      WiFi.disconnect(false, true);
      // Fall back to the previously working network, if any: a failed experiment must not cost the
      // owner the network the machine was already on.
      if (g_staSsid[0]) staBegin(g_staSsid, g_staPass);
      else              WiFi.mode(WIFI_AP);
      g_lastRetry = now;
      refreshIdentity();
    }
    return;                                    // no fallback bookkeeping while an attempt is live
  }

  if (g_apDropAt && (int32_t)(now - g_apDropAt) >= 0 && !g_apOnly && WiFi.status() == WL_CONNECTED)
    apDown();

  // ---- link supervision ------------------------------------------------------------------------
  bool up = (WiFi.status() == WL_CONNECTED);
  if (up != g_staUp) {
    if (up) {
      Serial.printf("[wifiprov] home link back, ip=%s\n", WiFi.localIP().toString().c_str());
      remdns();
      g_lostSince = 0;
      if (g_res == RES_FAIL) g_res = RES_NONE;   // it works now; stop showing the old failure
      // Do not yank the AP out from under someone who is using it right now.
      if (g_apUp && !g_apOnly) g_apDropAt = now + (WiFi.softAPgetStationNum() ? AP_LINGER_MS : 3000);
    } else if (!g_apOnly && g_staSsid[0]) {
      g_lostSince = now;
      Serial.println("[wifiprov] home link lost");
    }
    g_staUp = up;
    refreshIdentity();
  }

  if (!up && !g_apOnly) {
    // Grace period, then the AP comes back: a customer whose box is rebooting must never end up
    // with an unreachable machine.
    if (g_lostSince && now - g_lostSince > LOST_GRACE_MS && !g_apUp) {
      Serial.println("[wifiprov] link down too long -> raising the SoftAP");
      apUp();
    }
    if (g_staSsid[0] && now - g_lastRetry > RETRY_MS) {
      g_lastRetry = now;
      staBegin(g_staSsid, g_staPass);          // auto-rejoin when the home network comes back
    }
  }

  // ---- housekeeping ---------------------------------------------------------------------------
  if (now - g_fsCheckAt > 10000) {             // a fresh /fsup can add the UI at any time
    g_fsCheckAt = now;
    g_fsHasUi = LittleFS.exists("/index.html");
  }

#ifdef WIFIPROV_BTN_PIN
  // Factory reset: BOOT held 5 s while running. No reboot — reconfiguring the radio in place can
  // never interrupt a NOR write or an OTA the way ESP.restart() could.
  static bool btnInit = false;
  if (!btnInit) { pinMode(WIFIPROV_BTN_PIN, INPUT_PULLUP); btnInit = true; }
  if (digitalRead(WIFIPROV_BTN_PIN) == LOW) {
    if (!g_btnDown) { g_btnDown = now ? now : 1; Serial.println("[wifiprov] BOOT held..."); }
    else if (now - g_btnDown > BTN_HOLD_MS) {
      g_btnDown = 0;
      Serial.println("[wifiprov] BOOT held 5 s -> factory reset");
      forget();
    }
  } else {
    g_btnDown = 0;
  }
#endif
}

// ---- public accessors -------------------------------------------------------------------------
bool        isAP()    { return g_apUp; }
bool        isSTA()   { return g_staUp; }
const char* mode()    { return g_mode; }
const char* ip()      { return g_ip; }
const char* staSsid() { return g_staSsid; }
const char* apSsid()  { return g_apSsid; }
const char* apPass()  { return g_apPass; }
// Still the documented factory hotspot password? /sysinfo reports this so a fleet check can see
// at a glance which boards are still shipping the password that is printed in WIFI_SETUP.md.
bool        apPassIsDefault() { return g_apPassDflt; }
// True only after a deliberate half-second of hold, so a stray touch never lights a warning.
bool        factoryHolding() { return g_btnDown && (millis() - g_btnDown) > 500; }

void forget() {
  if (g_nvs.begin("wifiprov", false)) { g_nvs.clear(); g_nvs.end(); }
  g_staSsid[0] = '\0';
  g_staPass[0] = '\0';
  g_apOnly = false;
  g_done   = false;
  g_res    = RES_NONE;
  g_msg[0] = '\0';
  g_lostSince = 0;
  g_apDropAt  = 0;
  setStr(g_apPass, sizeof g_apPass, WIFIPROV_AP_PASS_DEFAULT);
  g_apPassDflt = true;
  WiFi.disconnect(false, true);
  g_staUp = false;
  if (g_apUp) { g_apUp = false; g_dns.stop(); }   // force a restart: the AP key may have changed
  apUp();
  refreshIdentity();
  Serial.println("[wifiprov] credentials cleared, wizard armed");
}

bool captiveRedirect(AsyncWebServerRequest *r) {
  if (!g_apUp || !ON_AP_FILTER(r)) return false;
  String h = r->host();
  int c = h.indexOf(':');
  if (c > 0) h = h.substring(0, c);
  if (h == g_ip || h == MDNS_HOST ".local" || h == MDNS_HOST) return false;  // really us: 404 is honest
  r->redirect(String("http://") + g_ip + "/wifi");
  return true;
}

void attachRoutes(AsyncWebServer &server) {
  g_mdnsUp = true;   // the caller starts mDNS right after us; tick() re-announces on link changes

  // NOTE ON ORDER: with this library a plain "/wifi" also matches "/wifi/...", so the sub-routes
  // must be registered FIRST or they would never be reached.

  server.on("/wifi/status", HTTP_GET, [](AsyncWebServerRequest *r) {
    char j[512];
    snprintf(j, sizeof j,
      "{\"mode\":\"%s\",\"ip\":\"%s\",\"ap\":%s,\"sta\":%s,\"ssid\":\"%s\",\"apssid\":\"%s\","
      "\"appass\":\"%s\",\"apdefault\":%s,\"aponly\":%s,\"done\":%s,\"busy\":%s,"
      "\"trying\":\"%s\",\"result\":\"%s\",\"msg\":\"%s\",\"rssi\":%d}",
      g_mode, g_ip, g_apUp ? "true" : "false", g_staUp ? "true" : "false",
      g_staSsid, g_apSsid, g_apPass,
      g_apPassDflt ? "true" : "false", g_apOnly ? "true" : "false", g_done ? "true" : "false",
      g_res == RES_TRYING ? "true" : "false",
      g_trySsid, resName(), g_msg, g_staUp ? (int)WiFi.RSSI() : 0);
    sendJson(r, 200, j);
  });

  // The handler NEVER scans — it publishes the cached list and asks tick() for a refresh. A
  // synchronous WiFi.scanNetworks() here would block the AsyncTCP task for seconds and trip the
  // watchdog, which is exactly the failure this project has already lived through once.
  server.on("/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *r) {
    uint32_t age   = millis() - g_scanAt;
    bool     stale = (g_scanAt == 0) || (age > SCAN_TTL_MS) || r->hasParam("force");
    if (stale && !g_scanBusy) queueCmd(CMD_SCAN);
    const char *nets = g_scan[g_scanPub];
    // Streamed, not snprintf'd into a local: the list is ~1.5 kB and the AsyncTCP task stack is
    // not the place to put it.
    AsyncResponseStream *res = r->beginResponseStream("application/json");
    res->addHeader("Cache-Control", "no-store");
    res->printf("{\"scanning\":%s,\"age\":%u,\"nets\":",
                (g_scanBusy || g_scanWant || stale) ? "true" : "false",
                (unsigned)(g_scanAt ? age : 0));
    res->print(nets[0] ? nets : "[]");
    res->print("}");
    r->send(res);
  });

  server.on("/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *r) {
    String s = param(r, "ssid");
    String p = param(r, "pass");
    s.trim();
    if (!s.length() || s.length() > 32) { sendJson(r, 400, "{\"ok\":false,\"err\":\"SSID invalide\"}"); return; }
    if (p.length() > 63)                { sendJson(r, 400, "{\"ok\":false,\"err\":\"mot de passe trop long\"}"); return; }
    if (g_res == RES_TRYING)            { sendJson(r, 409, "{\"ok\":false,\"err\":\"tentative deja en cours\"}"); return; }
    if (!queueCmd(CMD_CONNECT, s.c_str(), p.c_str())) { sendJson(r, 409, "{\"ok\":false,\"err\":\"occupe\"}"); return; }
    sendJson(r, 202, "{\"ok\":true}");   // 202: queued. The outcome arrives via /wifi/status.
  });

  server.on("/wifi/aponly", HTTP_POST, [](AsyncWebServerRequest *r) {
    queueCmd(CMD_APONLY);
    sendJson(r, 200, "{\"ok\":true}");
  });

  server.on("/wifi/appass", HTTP_POST, [](AsyncWebServerRequest *r) {
    String p = param(r, "pass");
    if (p.length() < 8 || p.length() > 63) { sendJson(r, 400, "{\"ok\":false,\"err\":\"8 a 63 caracteres\"}"); return; }
    if (!queueCmd(CMD_APPASS, nullptr, p.c_str())) { sendJson(r, 409, "{\"ok\":false,\"err\":\"occupe\"}"); return; }
    sendJson(r, 200, "{\"ok\":true}");
  });

  server.on("/wifi/forget", HTTP_POST, [](AsyncWebServerRequest *r) {
    queueCmd(CMD_FORGET);
    sendJson(r, 200, "{\"ok\":true}");
  });

  // The portal itself. No interface filter: being able to change the WiFi from the home LAN is the
  // whole point of not needing a USB cable.
  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *r) { sendPortal(r); });

  // "/" belongs to the portal only while the wizard is unanswered, or when LittleFS holds no UI at
  // all (fresh board / botched /fsup). Once the customer has chosen, "/" is the normal control UI
  // again — the filter returning false lets the request fall through to serveStatic().
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) { sendPortal(r); })
        .setFilter([](AsyncWebServerRequest *r) { return g_apUp && (!g_done || !g_fsHasUi); });

  // ---- captive-portal probes ---------------------------------------------------------------
  // Unanswered wizard -> redirect, which is what makes the "sign in to this network" sheet open by
  // itself. Once the customer has chosen (typically AP-only, permanently), answer exactly what each
  // OS expects instead, so joining the hotspot stops nagging them forever.
  struct Probe { const char *path; const char *type; const char *body; int code; };
  static const Probe probes[] = {
    { "/generate_204",             nullptr,      nullptr,                            204 },  // Android
    { "/gen_204",                  nullptr,      nullptr,                            204 },
    { "/hotspot-detect.html",      "text/html",  "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
                                                 "<BODY>Success</BODY></HTML>",      200 },  // iOS/macOS
    { "/library/test/success.html","text/html",  "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
                                                 "<BODY>Success</BODY></HTML>",      200 },
    { "/connecttest.txt",          "text/plain", "Microsoft Connect Test",           200 },  // Windows
    { "/ncsi.txt",                 "text/plain", "Microsoft NCSI",                   200 },
    { "/redirect",                 "text/html",  "<HTML></HTML>",                    200 },
    { "/fwlink",                   "text/html",  "<HTML></HTML>",                    200 },
    { "/success.txt",              "text/plain", "success\n",                        200 },  // Firefox
    { "/canonical.html",           "text/html",  "<HTML></HTML>",                    200 },
  };
  for (const Probe &p : probes) {
    const Probe *pp = &p;                      // static storage: safe to capture by pointer
    server.on(p.path, HTTP_GET, [pp](AsyncWebServerRequest *r) {
      if (!g_done) { r->redirect(String("http://") + g_ip + "/wifi"); return; }
      if (pp->code == 204) r->send(204);
      else                 r->send(200, pp->type, pp->body);
    }).setFilter(ON_AP_FILTER);
  }
}

} // namespace wifiprov
