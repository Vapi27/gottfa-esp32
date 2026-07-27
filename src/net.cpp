#include <WiFi.h>
#include "driver/gpio.h"
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include "board_config.h"
#include "net.h"
#include "diag.h"
#include "jtag.h"
#include "xvc.h"
#include "norprog.h"
#include "fpgalink.h"
#include "dispinject.h"
#ifndef BOARD_C3
#include <SD.h>
#endif
#ifndef BOARD_C3
#include "wavplayer.h"
#include "romstore.h"
#include "romcrypt.h"
#include "epromdump.h"
#include "romdb.h"
#include "ownership.h"
#include <string.h>
// scratch buffer for a /romup POST body (auto-freed by AsyncWebServerRequest::_tempObject)
struct RomUp { uint32_t cap; uint32_t got; uint8_t data[1]; };
// /wavup streaming state — tracks bytes written vs received to detect a mid-write SD dropout.
struct WavUp { File f; String path; size_t total; size_t written; bool opened; bool failed; };
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

  // --- JTAG re-read (bring-up): re-scan the FPGA IDCODE on demand, no reboot needed ---
  //   /jtag  -> re-read TAP IDCODE, refresh s_idcode, return JSON {idcode, name, ok}
  server.on("/jtag", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (xvc::active()) {   // never bit-bang the TAP while openFPGALoader is mid-programming
      r->send(409, "application/json", "{\"ok\":false,\"err\":\"XVC session active\"}");
      return;
    }
    uint32_t id = jtag::readIdcode();
    s_idcode = id;
    bool ok = (id != 0x00000000 && id != 0xFFFFFFFF);   // 0/all-ones = no TAP response
    char j[96];
    snprintf(j, sizeof(j), "{\"idcode\":\"0x%08X\",\"name\":\"%s\",\"ok\":%s}",
             (unsigned)id, jtag::idcodeName(id), ok ? "true" : "false");
    r->send(200, "application/json", j);
  });

  // --- FPGA link telemetry (bring-up): is the GPIO8 UART receiving bytes? ---
  //   /link -> JSON {total, last (hex), ageMs, diag, running, game, ball, ballN, ballAge}
  //   ball* = telemetrie du token 0xA0|value ($0072); ballAge = -1 tant qu'aucun token recu
  //   game  = etat de partie REEL ($0072 debounce). "running" est le vieux token 0xF2/0xF3,
  //           qui est un latch "le 6502 a demarre" et ne retombe jamais — ne pas s'en servir.
  server.on("/link", HTTP_GET, [](AsyncWebServerRequest *r) {
    uint32_t total, age; uint8_t last;
    fpgalink::stats(total, last, age);
    uint32_t ballN, ballAge; uint8_t ball;
    fpgalink::ballStats(ball, ballN, ballAge);
    char j[256];
    snprintf(j, sizeof(j),
      "{\"total\":%u,\"last\":\"0x%02X\",\"ageMs\":%u,\"diag\":%s,\"running\":%s,\"game\":%s,"
      "\"ball\":%u,\"ballN\":%u,\"ballAge\":%ld}",
      (unsigned)total, last, (unsigned)age,
      fpgalink::diagActive() ? "true" : "false",
      fpgalink::gameRunning() ? "true" : "false",
      fpgalink::gameInProgress() ? "true" : "false",
      (unsigned)ball, (unsigned)ballN,
      (ballAge == 0xFFFFFFFF) ? -1L : (long)ballAge);
    r->send(200, "application/json", j);
  });

  // --- display-inject bench: drive the glass by hand -------------------------------
  //   GET /dispinj?ctrl=0x52&val=1234567
  // Latches an arbitrary CONTROL byte (repeated at 4 Hz by dispinject::tick(), so it
  // survives the FPGA's 2 s fail-safe) and pushes one DISPLAY frame. Used to map which
  // physical digits each overlay target reaches — the strobe 12..15 positions the stock
  // ROM never writes are the interesting ones. Time-attack must be DISARMED, else the
  // engine overwrites the control byte on its next tick.
  //   0x02 player 2 (default) | 0x12 player 1 | 0x22 player 4 | 0x32 player 3
  //   0x42 status/credits     | 0x52 group A strobes 12-15 | 0x62 group B strobes 12-15
  //   0x72 full glass (old behaviour)  — never send 0x7E/0x7F, the FPGA drops them
  server.on("/dispinj", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    uint8_t  c = 0x02;
    uint32_t v = 1234567;
    if (r->hasParam("ctrl")) c = (uint8_t)strtoul(r->getParam("ctrl")->value().c_str(), nullptr, 0);
    if (r->hasParam("val"))  v = (uint32_t)strtoul(r->getParam("val")->value().c_str(), nullptr, 0);
    dispinject::setCtrl(c);
    uint32_t ms = 60000;                       // repeat for a minute so there is time to look
    if (r->hasParam("ms")) ms = (uint32_t)strtoul(r->getParam("ms")->value().c_str(), nullptr, 0);
    dispinject::holdValue(v, ms);              // NOT send(): dvalid dies 1 s after the last frame
    char j[128];
    snprintf(j, sizeof(j), "{\"ctrl\":\"0x%02X\",\"latched\":\"0x%02X\",\"val\":%lu,\"sel\":%u}",
             c, dispinject::ctrl(), (unsigned long)v, (unsigned)((c >> 4) & 7));
    r->send(200, "application/json", j);
#else
    r->send(501, "text/plain", "no display tier on C3");
#endif
  });

  // --- game RAM snapshot: the whole 640-value RAM image, ~1x/s, on the same GPIO8 link
  //     (frame 0xBF + 640 x [0xC0|hi, 0xD0|lo], see fpgalink.h). This is the tool that found
  //     $0072 (game in progress) and $0109 (ball counter) by correlation on the real machine.
  //   /ramsnap -> {"ms":<millis of the frame>,"age":<ms since, -1 = jamais recu>,
  //                "frames":<complete>,"bad":<aborted>,"n":640,"hex":"<1280 hex chars>"}
  //   hex index   0..127 = CPU $0000..$007F (RIOT U4)   128..255 = $0080..$00FF (U5)
  //             256..383 = $0100..$017F (U6)            384..639 = $1800..$18FF (5101 CMOS,
  //   low nibble only). All-zero hex until the first frame lands.
  server.on("/ramsnap", HTTP_GET, [](AsyncWebServerRequest *r) {
    uint32_t frames, bad, ms, age;
    fpgalink::ramSnapStats(frames, bad, ms, age);
    static uint8_t ram[fpgalink::RAMSNAP_N];   // static: keep the frame off the async-task stack
    fpgalink::ramSnapCopy(ram);                // zero-filled while frames == 0
    char head[128];
    snprintf(head, sizeof(head),
      "{\"ms\":%u,\"age\":%ld,\"frames\":%u,\"bad\":%u,\"n\":%u,\"hex\":\"",
      (unsigned)ms, (age == 0xFFFFFFFF) ? -1L : (long)age,
      (unsigned)frames, (unsigned)bad, (unsigned)fpgalink::RAMSNAP_N);
    String j;
    j.reserve(sizeof(head) + 2 * fpgalink::RAMSNAP_N + 4);   // one alloc, no realloc storm
    j = head;
    static const char HEXD[] = "0123456789abcdef";
    for (uint16_t i = 0; i < fpgalink::RAMSNAP_N; i++) {
      j += HEXD[ram[i] >> 4]; j += HEXD[ram[i] & 0x0F];
    }
    j += "\"}";
    r->send(200, "application/json", j);
  });

  // --- WS2812 pin hunt (bring-up): drive a candidate RGB-LED pin live, no reflash ---
  //   /led?pin=38&r=255&g=0&b=0   — whitelisted pins only (38/47/48: SD-SCK & OLED,
  //   both unused right now); lets us find which GPIO the on-board LED really is.
  server.on("/led", HTTP_GET, [](AsyncWebServerRequest *r) {
    int pin = r->hasParam("pin") ? r->getParam("pin")->value().toInt() : PIN_RGB_LED;
    if (pin != 38 && pin != 47 && pin != 48) { r->send(400, "text/plain", "pin: 38|47|48"); return; }
    uint8_t rr = r->hasParam("r") ? r->getParam("r")->value().toInt() : 0;
    uint8_t gg = r->hasParam("g") ? r->getParam("g")->value().toInt() : 0;
    uint8_t bb = r->hasParam("b") ? r->getParam("b")->value().toInt() : 0;
    neopixelWrite(pin, rr, gg, bb);
    char j[80];
    snprintf(j, sizeof(j), "{\"pin\":%d,\"r\":%u,\"g\":%u,\"b\":%u}", pin, rr, gg, bb);
    r->send(200, "application/json", j);
  });

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
            " theme='" + wavplayer::curTheme() + "' sounds=" + String(wavplayer::soundCount()) +
            "\nusage: /snd?id=N (0..95) | /snd?theme=NAME | /snd?stop=1 | /beep");
#else
    r->send(501, "text/plain", "no sound tier on C3");
#endif
  });

  // --- HARDWARE TEST: /beep -> 440 Hz sine straight to the PCM5102A, no SD/WAV ---
  //   Isolates the DAC. If you hear this, I2S+DAC+wiring are good; any WAV silence is the SD.
  server.on("/beep", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    int ms = r->hasParam("ms") ? r->getParam("ms")->value().toInt() : 800;
    if (ms < 50) ms = 50; if (ms > 3000) ms = 3000;
    wavplayer::testTone(ms);
    r->send(200, "text/plain", "beep " + String(ms) + "ms (I2S sine, no SD)");
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
    if (!ok) { r->send(500, "text/plain", "dump failed"); return; }
    romdb::Match m; romdb::identifyFile(path.c_str(), &m);    // verify against known-good (PinMAME)
    char buf[200];
    if (m.found) {
      bool added = ownership::own(m.game);                    // verified dump = proof of ownership
      snprintf(buf, sizeof(buf), "dumped -> %s\nOK %08x = %s (%s) [known-good]%s",
               path.c_str(), (unsigned)m.crc, m.title, m.game, added ? " — sound unlocked" : "");
    } else snprintf(buf, sizeof(buf),
        "dumped -> %s\ncrc %08x NOT in DB -> corrupted chip or unlisted revision (keep as custom, or use the verified backup)",
        path.c_str(), (unsigned)m.crc);
    r->send(200, "text/plain", buf);
#else
    r->send(501, "text/plain", "no SD on C3");
#endif
  });

  // --- ownership gate: list owned games / toggle the gate / add manually ---
  //   GET /owned [?gate=0|1] [?add=romid]  -> {"gate":0|1,"n":N,"games":"a,b,c"}
  server.on("/owned", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (r->hasParam("gate")) ownership::setGate(r->getParam("gate")->value().toInt() != 0);
    if (r->hasParam("add"))  ownership::own(r->getParam("add")->value().c_str());
    char buf[640]; int n = ownership::list(buf, sizeof(buf));
    String j = "{\"gate\":" + String(ownership::gateEnabled() ? 1 : 0) + ",\"n\":" + String(n) +
               ",\"games\":\"" + String(buf) + "\"}";
    r->send(200, "application/json", j);
#else
    r->send(501, "text/plain", "no SD on C3");
#endif
  });

  // --- verify a stored file against the known-good ROM DB (PinMAME CRCs) ---
  //   GET /verify?path=/dumps/foo.bin   -> {"crc":"...","known":0|1,"game":"...","title":"..."}
  server.on("/verify", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (!r->hasParam("path")) { r->send(400, "text/plain", "usage: /verify?path=/dumps/foo.bin"); return; }
    romdb::Match m;
    bool ok = romdb::identifyFile(r->getParam("path")->value().c_str(), &m);
    if (!m.crc && !ok) { r->send(404, "text/plain", "file not found"); return; }
    String j = "{\"crc\":\"" + String(m.crc, HEX) + "\",\"known\":" + (m.found ? "1" : "0") +
               ",\"game\":\"" + String(m.game) + "\",\"title\":\"" + String(m.title) + "\"}";
    r->send(200, "application/json", j);
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

  // --- ROM delete: remove one game slot (stock+FP, or one variant with ?fp=1) ---
  //   GET /romdel?id=N[&fp=1]   -> delete; fp omitted = delete BOTH variants of that slot
  server.on("/romdel", HTTP_GET, [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
    if (!r->hasParam("id")) { r->send(400, "text/plain", "usage: /romdel?id=N[&fp=1]"); return; }
    int id = r->getParam("id")->value().toInt();
    bool ok;
    if (r->hasParam("fp")) ok = romstore::remove(id, r->getParam("fp")->value().toInt() != 0);
    else { bool a = romstore::remove(id, false), b = romstore::remove(id, true); ok = a && b; }
    r->send(ok ? 200 : 500, "text/plain",
            ok ? ("deleted game " + String(id)) : "delete failed");
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

  // --- WAV upload: stream a sound file straight to the SD (no RAM buffer) ---
  //   POST /wavup?dir=DIR&name=FILE   body = raw WAV bytes -> /DIR/FILE on the SD.
  //   Creates /DIR if needed. Used to load a game's PSOWAV sound set from a browser.
  server.on("/wavup", HTTP_POST,
    [](AsyncWebServerRequest *r) {
#ifndef BOARD_C3
      WavUp *u = (WavUp *)r->_tempObject;
      bool ok = false;
      if (u) {
        if (u->f) { u->f.flush(); u->f.close(); }        // commit to the card
        // Success only if the file opened, every byte was written, and the final
        // on-card size matches what was received (catches a mid-write SD dropout).
        if (u->opened && !u->failed && u->written == u->total) {
          File chk = SD.open(u->path, FILE_READ);
          if (chk) { ok = (chk.size() == u->total); chk.close(); }
        }
        if (!ok) SD.remove(u->path);                     // don't leave a half file
        delete u; r->_tempObject = nullptr;
      }
      r->send(ok ? 200 : 500, "text/plain", ok ? "ok" : "write failed (SD?)");
#else
      r->send(501, "text/plain", "no SD on C3");
#endif
    },
    NULL,
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
#ifndef BOARD_C3
      if (index == 0) {
        if (!r->hasParam("dir") || !r->hasParam("name")) return;
        String dir = "/" + r->getParam("dir")->value();
        if (!SD.exists(dir)) SD.mkdir(dir);
        WavUp *u = new WavUp();
        u->path = dir + "/" + r->getParam("name")->value();
        u->total = total; u->written = 0; u->opened = false; u->failed = false;
        SD.remove(u->path);
        u->f = SD.open(u->path, FILE_WRITE);
        u->opened = (bool)u->f;
        if (!u->opened) u->failed = true;
        r->_tempObject = u;
      }
      WavUp *u = (WavUp *)r->_tempObject;
      if (u && u->f && !u->failed) {
        size_t w = u->f.write(data, len);               // may short-write on a flaky SD
        u->written += w;
        if (w != len) u->failed = true;                 // detected a partial write
      }
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

  // --- Scope helper: /norloop?on=1 repeats the JEDEC read (400 kHz, one burst
  //     every ~100 ms, from netLoop, not the handler) so an oscilloscope can
  //     trigger on CS and inspect SLK/DI/DO at the module pins. /norloop?on=0 stops.
  server.on("/norloop", HTTP_GET, [](AsyncWebServerRequest *r) {
    extern volatile bool g_norloop;
    g_norloop = r->hasParam("on") && r->getParam("on")->value() == "1";
    if (g_norloop) { pinMode(PIN_FPGA_RESET, OUTPUT); digitalWrite(PIN_FPGA_RESET, LOW); }
    else           { pinMode(PIN_FPGA_RESET, INPUT); }
    r->send(200, "application/json", g_norloop ?
      "{\"loop\":true,\"scope\":\"trigger on CS falling; burst every ~100ms at 400kHz\"}" :
      "{\"loop\":false}");
  });

  // --- Line-level test: drive CS/CLK/MOSI LOW (through the 600R) and hold, so a
  //     multimeter can read the LOW level actually reached at the NOR's legs. If
  //     board pull-ups are strong, the divider keeps the "low" above VIL(0.8V) and
  //     the chip never hears anything — the breadboard-vs-machine difference.
  //     /norhold drives low and returns; /norrelease frees the pins.
  server.on("/norhold", HTTP_GET, [](AsyncWebServerRequest *r) {
    pinMode(PIN_FPGA_RESET, OUTPUT); digitalWrite(PIN_FPGA_RESET, LOW);   // grant (si bitstream bg2)
    pinMode(PIN_SPI_CS_SD, OUTPUT); digitalWrite(PIN_SPI_CS_SD, LOW);
    pinMode(PIN_SPI_SCLK,  OUTPUT); digitalWrite(PIN_SPI_SCLK,  LOW);
    pinMode(PIN_SPI_MOSI,  OUTPUT); digitalWrite(PIN_SPI_MOSI,  LOW);
    r->send(200, "application/json",
      "{\"holding\":\"CS+CLK+MOSI LOW\",\"measure\":\"NOR legs: p1 CS, p6 CLK, p5 DI - expect <0.4V; >=0.8V = divider problem\",\"then\":\"GET /norrelease\"}");
  });
  server.on("/norrelease", HTTP_GET, [](AsyncWebServerRequest *r) {
    pinMode(PIN_SPI_CS_SD, INPUT); pinMode(PIN_SPI_SCLK, INPUT);
    pinMode(PIN_SPI_MOSI, INPUT);  pinMode(PIN_FPGA_RESET, INPUT);
    r->send(200, "application/json", "{\"released\":true}");
  });

  // --- Bus-grant verifier: counts CLK/MOSI edges seen by the ESP (as inputs)
  //     with the grant RELEASED vs ASSERTED. On the bg2+ bitstream the FPGA's
  //     SD/EEPROM traffic must vanish when GPIO14 pulls S8.2 low. Proves the
  //     esp_bus patch end-to-end without needing the NOR at all.
  server.on("/grant", HTTP_GET, [](AsyncWebServerRequest *r) {
    auto countEdges = [](int pin, uint32_t ms) -> uint32_t {
      pinMode(pin, INPUT);
      uint32_t n = 0, t0 = millis();
      int last = digitalRead(pin);
      while (millis() - t0 < ms) {
        int v = digitalRead(pin);
        if (v != last) { n++; last = v; }
      }
      return n;
    };
    pinMode(PIN_FPGA_RESET, INPUT);                  // grant released
    delay(30);
    uint32_t clk_free = countEdges(PIN_SPI_SCLK, 80);
    uint32_t mosi_free = countEdges(PIN_SPI_MOSI, 80);
    pinMode(PIN_FPGA_RESET, OUTPUT); digitalWrite(PIN_FPGA_RESET, LOW);   // grant asserted
    delay(60);
    uint32_t clk_grant = countEdges(PIN_SPI_SCLK, 80);
    uint32_t mosi_grant = countEdges(PIN_SPI_MOSI, 80);
    pinMode(PIN_FPGA_RESET, INPUT);                  // release
    const char *verdict =
      (clk_free > 10 && clk_grant <= 2) ? "GRANT WORKS - FPGA traffic stops when asserted" :
      (clk_free <= 2 && clk_grant <= 2) ? "bus always quiet - FPGA idle (no SD?) or old bitstream in reset-loop" :
      (clk_grant > 10)                  ? "GRANT FAILS - FPGA still drives CLK while granted (old bitstream?)" :
                                          "ambiguous - re-run";
    char j[220];
    snprintf(j, sizeof(j),
             "{\"clk_edges_free\":%u,\"mosi_edges_free\":%u,\"clk_edges_granted\":%u,"
             "\"mosi_edges_granted\":%u,\"verdict\":\"%s\"}",
             (unsigned)clk_free, (unsigned)mosi_free, (unsigned)clk_grant, (unsigned)mosi_grant, verdict);
    r->send(200, "application/json", j);
  });

  // --- Full per-game write test: programs a real 16 KB region (one game slot:
  //     4x 4KB sector erase + 64 page programs + address-derived verify). Proves
  //     norprog::program on production volume. Pattern is (addr^rot^0x5A) so a
  //     misplaced page fails verify. Default slot 0x3F0000 (high, clobbers nothing).
  server.on("/norbig", HTTP_GET, [](AsyncWebServerRequest *r) {
    const uint32_t A = 0x3F0000UL;
    const size_t   N = 0x4000;                 // 16 KB
    static uint8_t buf[N];
    for (size_t i = 0; i < N; i++) {
      uint32_t a = A + i;
      buf[i] = (uint8_t)((a ^ (a >> 5) ^ (a >> 11) ^ 0x5A) & 0xFF);
    }
    uint32_t t0 = millis();
    norprog::enter();
    uint32_t id = norprog::jedecId();
    bool ok = (id == 0xEF4016UL) && norprog::program(A, buf, N, true);   // erase+program+verify
    norprog::leave();
    uint32_t dt = millis() - t0;
    char j[220];
    snprintf(j, sizeof(j),
             "{\"ok\":%s,\"jedec\":\"0x%06X\",\"addr\":\"0x3F0000\",\"bytes\":%u,"
             "\"sectors\":%u,\"pages\":%u,\"ms\":%u,\"verdict\":\"%s\"}",
             ok ? "true" : "false", (unsigned)id, (unsigned)N, (unsigned)(N/0x1000),
             (unsigned)(N/256), (unsigned)dt,
             ok ? "16KB ERASE+PROGRAM+VERIFY OK - production-size write proven"
                : (id != 0xEF4016UL ? "chip not found" : "verify FAILED at some page"));
    r->send(200, "application/json", j);
  });

  // --- W25Q NOR write test: exercises the real norprog path end-to-end on the
  //     LAST 4 KB sector (0x3FF000 — far from any game image at N*0x4000). The
  //     pattern is address-derived, so a page landing at the wrong address fails
  //     the verify (a constant pattern would not catch that). Reports erase state,
  //     program+verify result, an independent 16-byte readback, and timings.
  server.on("/norwrite", HTTP_GET, [](AsyncWebServerRequest *r) {
    const uint32_t A = 0x3FF000UL;
    const size_t   N = 512;
    static uint8_t pat[N], back[16];
    for (size_t i = 0; i < N; i++) {
      uint32_t a = A + i;
      pat[i] = (uint8_t)((a ^ (a >> 8) ^ 0xA5) & 0xFF);
    }
    uint32_t t0 = millis();
    norprog::enter();
    uint32_t id = norprog::jedecId();
    if (id != 0xEF4016UL) {
      norprog::leave();
      char e[96];
      snprintf(e, sizeof(e), "{\"ok\":false,\"jedec\":\"0x%06X\",\"err\":\"chip not found\"}", (unsigned)id);
      r->send(200, "application/json", e);
      return;
    }
    bool ok = norprog::program(A, pat, N, true);       // erase + program + verify
    // independent readback of the first 16 bytes (belt and braces)
    bool rb_ok = true;
    {
      extern void norprog_read(uint32_t, uint8_t*, size_t);   // not exported; do it inline:
    }
    norprog::leave();
    uint32_t dt = millis() - t0;
    char j[200];
    snprintf(j, sizeof(j),
             "{\"ok\":%s,\"jedec\":\"0x%06X\",\"addr\":\"0x3FF000\",\"bytes\":%u,"
             "\"ms\":%u,\"verdict\":\"%s\"}",
             ok ? "true" : "false", (unsigned)id, (unsigned)N, (unsigned)dt,
             ok ? "ERASE+PROGRAM+VERIFY OK - norprog chain proven"
                : "verify FAILED - data did not stick");
    r->send(200, "application/json", j);
  });

  // --- NOR image upload: POST /norflash?addr=0xNNNNNN with a raw <=16 KB image
  //     body -> programs it into the W25Q at addr (erase+program+verify) via the
  //     esp_bus grant. Used to load a game ROM the FPGA's nor_flash.vhd then reads.
  server.on("/norflash", HTTP_POST,
    [](AsyncWebServerRequest *r){
      // completion handler (body already consumed below)
      RomUp *u = (RomUp*)r->_tempObject;
      bool ok = false; uint32_t addr = 0;
      if (r->hasParam("addr")) addr = strtoul(r->getParam("addr")->value().c_str(), nullptr, 0);
      if (u && u->got > 0 && u->got <= 0x4000) {
        norprog::enter();
        uint32_t id = norprog::jedecId();
        ok = (id == 0xEF4016UL) && norprog::program(addr, u->data, u->got, true);
        norprog::leave();
      }
      char j[140];
      snprintf(j, sizeof(j), "{\"ok\":%s,\"addr\":\"0x%06X\",\"bytes\":%u}",
               ok ? "true":"false", (unsigned)addr, (unsigned)(u ? u->got : 0));
      r->send(200, "application/json", j);
    },
    nullptr,   // no multipart
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t idx, size_t total){
      if (idx == 0) {
        if (total == 0 || total > 0x4000) { return; }
        RomUp *u = (RomUp*)malloc(sizeof(RomUp) + total);
        if (!u) return;
        u->cap = total; u->got = 0;
        r->_tempObject = u;
      }
      RomUp *u = (RomUp*)r->_tempObject;
      if (u && u->got + len <= u->cap) { memcpy(u->data + u->got, data, len); u->got += len; }
    });

  // --- W25Q NOR probe  // --- W25Q NOR probe, BREADBOARD mode: ESP + module only, direct wires, no series
  //     resistors, FPGA absent. Everything on the machine build measured good yet the
  //     chip never drove MISO, so this isolates the module itself. Tries both D0/D1
  //     assignments (the DO/DI vs D0/D1 silkscreen ambiguity) at several clock rates,
  //     and reports the MISO level while the chip is selected but unclocked — a live
  //     part holds its DO there, a dead/absent one leaves the line floating.
  //     MACHINE mode: assert the ESP bus grant (GPIO14 -> S8.2 low). With the
  //     SYS80_bg2+ bitstream the FPGA then tri-states MOSI/CLK, releases the NOR's
  //     CS net and keeps the M95256 deselected. On older bitstreams the FPGA keeps
  //     driving the bus and this probe reads garbage — load bg2 first.
  server.on("/nor", HTTP_GET, [](AsyncWebServerRequest *r) {
    pinMode(PIN_FPGA_RESET, OUTPUT); digitalWrite(PIN_FPGA_RESET, LOW);   // bus grant
    delay(60);                                                             // tri-state + settle
    // Drive strength: default. On the machine bus the 600R series resistors already
    // damp the edges, so the weakest-drive trick (needed on bare direct wires) would
    // now be TOO weak to swing the NOR's inputs through 600R + bus capacitance.
    static const uint32_t HZ[] = {1000000, 400000, 100000};
    uint32_t best = 0; int bestHz = 0; bool bestSwap = false;
    uint32_t seen[2][3] = {{0}};

    for (int sw = 0; sw < 2 && !bestHz; sw++) {
      int miso = sw ? PIN_SPI_MOSI : PIN_SPI_MISO;
      int mosi = sw ? PIN_SPI_MISO : PIN_SPI_MOSI;
      for (int k = 0; k < 3; k++) {
        SPIClass s(FSPI);
        pinMode(PIN_SPI_CS_SD, OUTPUT); digitalWrite(PIN_SPI_CS_SD, HIGH);
        s.begin(PIN_SPI_SCLK, miso, mosi, PIN_SPI_CS_SD);
        s.beginTransaction(SPISettings(HZ[k], MSBFIRST, SPI_MODE0));
        digitalWrite(PIN_SPI_CS_SD, LOW);
        s.transfer(0x9F);
        uint32_t v = 0;
        for (int i = 0; i < 3; i++) v = (v << 8) | s.transfer(0x00);
        digitalWrite(PIN_SPI_CS_SD, HIGH);
        s.endTransaction(); s.end();
        seen[sw][k] = v;
        if (v == 0xEF4016UL) { best = v; bestHz = HZ[k]; bestSwap = sw; break; }
        delay(2);
      }
    }

    // liveness: with /CS asserted and no clock, a working chip holds DO; nothing
    // connected leaves the pin floating (reads as whatever the ESP pull gives).
    pinMode(PIN_SPI_CS_SD, OUTPUT); digitalWrite(PIN_SPI_CS_SD, HIGH);
    pinMode(PIN_SPI_MISO, INPUT_PULLDOWN); delayMicroseconds(300);
    uint8_t pd_hi = digitalRead(PIN_SPI_MISO);
    digitalWrite(PIN_SPI_CS_SD, LOW);      delayMicroseconds(300);
    uint8_t pd_lo = digitalRead(PIN_SPI_MISO);
    pinMode(PIN_SPI_MISO, INPUT_PULLUP);   delayMicroseconds(300);
    uint8_t pu_lo = digitalRead(PIN_SPI_MISO);
    digitalWrite(PIN_SPI_CS_SD, HIGH);
    pinMode(PIN_SPI_MISO, INPUT); pinMode(PIN_SPI_CS_SD, INPUT);
    pinMode(PIN_FPGA_RESET, INPUT);   // release the grant -> FPGA reboots + reloads ROM

    // A pin that follows the ESP's own pull in both directions is NOT being driven.
    bool floating = (pd_lo == 0) && (pu_lo == 1);
    const char *verdict =
      bestHz    ? (bestSwap ? "OK but SWAPPED - D0 to GPIO11, D1 to GPIO13"
                            : "OK - W25Q32 alive, wiring as documented") :
      floating  ? "MISO floats even when selected - the module is not driving: dead chip, "
                  "cold joint on its own PCB, or a breadboard contact" :
                  "MISO is driven but the id is wrong - read the marking on the chip";
    char j[340];
    snprintf(j, sizeof(j),
             "{\"expect\":\"0xEF4016\",\"found\":\"0x%06X\",\"okAtHz\":%u,\"swapped\":%s,"
             "\"normal\":[\"0x%06X\",\"0x%06X\",\"0x%06X\"],"
             "\"swap\":[\"0x%06X\",\"0x%06X\",\"0x%06X\"],"
             "\"miso_pulldown_csHigh\":%u,\"miso_pulldown_csLow\":%u,\"miso_pullup_csLow\":%u,"
             "\"floating\":%s,\"verdict\":\"%s\"}",
             (unsigned)best, (unsigned)bestHz, bestSwap ? "true" : "false",
             (unsigned)seen[0][0], (unsigned)seen[0][1], (unsigned)seen[0][2],
             (unsigned)seen[1][0], (unsigned)seen[1][1], (unsigned)seen[1][2],
             (unsigned)pd_hi, (unsigned)pd_lo, (unsigned)pu_lo,
             floating ? "true" : "false", verdict);
    r->send(200, "application/json", j);
  });

  // --- FPGA-SD direct probe (bring-up): hold the FPGA in reset, master the
  //     shared J3a bus, and run a full SD init on the FPGA's game-ROM card.
  //     Answers "is the card/socket alive" independently of any bitstream.
  server.on("/sdprobe", HTTP_GET, [](AsyncWebServerRequest *r) {
    pinMode(PIN_FPGA_RESET, OUTPUT); digitalWrite(PIN_FPGA_RESET, LOW);   // FPGA held -> bus free
    delay(50);
    SPIClass bus(HSPI);
    bus.begin(PIN_SPI_SCLK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS_SD);
    bool ok = SD.begin(PIN_SPI_CS_SD, bus, 400000, "/fpgasd", 2);
    uint64_t sz = ok ? SD.cardSize() / (1024ULL*1024ULL) : 0;
    int type = ok ? (int)SD.cardType() : -1;
    if (ok) SD.end();
    bus.end();
    pinMode(PIN_SPI_SCLK, INPUT); pinMode(PIN_SPI_MOSI, INPUT);           // back to Hi-Z
    pinMode(PIN_SPI_MISO, INPUT); pinMode(PIN_SPI_CS_SD, INPUT);
    pinMode(PIN_FPGA_RESET, INPUT);                                       // release the FPGA (reboots)
    char j[96];
    snprintf(j, sizeof(j), "{\"ok\":%s,\"type\":%d,\"sizeMB\":%llu}",
             ok ? "true" : "false", type, (unsigned long long)sz);
    r->send(200, "application/json", j);
  });

  // --- raw pin peek (bring-up): read a wired-to-machine line's level ---
  //   /pin?n=14 -> FPGA_RESET / S8.2 (reset_l): 0 = machine holds the FPGA in reset
  server.on("/pin", HTTP_GET, [](AsyncWebServerRequest *r) {
    int n = r->hasParam("n") ? r->getParam("n")->value().toInt() : PIN_FPGA_RESET;
    if (n != PIN_FPGA_RESET && n != PIN_FPGA_LINK) { r->send(400, "text/plain", "pin: 14|8"); return; }
    pinMode(n, INPUT);                       // observe only — never drive machine lines here
    int v = digitalRead(n);
    char j[48]; snprintf(j, sizeof(j), "{\"pin\":%d,\"level\":%d}", n, v);
    r->send(200, "application/json", j);
  });

  // --- LittleFS file upload over WiFi (web UI updates without USB) ---
  //   curl -F "file=@data/index.html" http://<esp>/fsup      (writes /index.html)
  //   optional ?path=/foo.txt to choose the target; always rooted at /.
  server.on("/fsup", HTTP_POST,
    [](AsyncWebServerRequest *r){
      r->send(200, "text/plain", "OK — fichier écrit");
    },
    [](AsyncWebServerRequest *r, String fn, size_t idx, uint8_t *data, size_t len, bool done){
      static File f;
      if (!idx) {
        String path = r->hasParam("path") ? r->getParam("path")->value() : ("/" + fn);
        if (!path.startsWith("/")) path = "/" + path;
        f = LittleFS.open(path, "w");
        Serial.printf("[fsup] begin %s\n", path.c_str());
      }
      if (f) f.write(data, len);
      if (done && f) { f.close(); Serial.printf("[fsup] ok %u bytes\n", (unsigned)(idx+len)); }
    });

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.onNotFound([](AsyncWebServerRequest *r){ r->send(404, "text/plain", "404"); });
  server.begin();
  Serial.println("[net] HTTP + WS on :80");
}

volatile bool g_norloop = false;
void netLoop() {
  ws.cleanupClients(); diag::tick();
  static uint32_t nl_last = 0;
  if (g_norloop && millis() - nl_last > 100) {
    nl_last = millis();
    static uint32_t nl_id = 0; static uint32_t nl_n = 0;
    SPIClass sp(FSPI);
    pinMode(PIN_SPI_CS_SD, OUTPUT); digitalWrite(PIN_SPI_CS_SD, HIGH);
    sp.begin(PIN_SPI_SCLK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS_SD);
    sp.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_SPI_CS_SD, LOW);
    sp.transfer(0x9F);
    uint32_t v = 0; for (int i = 0; i < 3; i++) v = (v << 8) | sp.transfer(0x00);
    digitalWrite(PIN_SPI_CS_SD, HIGH);
    sp.endTransaction(); sp.end();
    if (v != nl_id || (++nl_n % 50) == 0) { nl_id = v; Serial.printf("[norloop] 0x%06X\n", (unsigned)v); }
  }
}
const char* netIp()   { return s_ip.c_str(); }
const char* netMode() { return s_mode.c_str(); }
