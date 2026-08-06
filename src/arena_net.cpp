#include <WiFi.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>   // ecriture directe de la partition fichiers
// Empreinte de build exposee dans /api/state. L'en-tete a demenage entre les
// versions de l'IDF : esp_app_desc.h n'existe qu'a partir de la 5, alors que le
// build Arduino/PlatformIO tourne encore sur la 4.4. Meme structure, deux noms.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include <esp_app_desc.h>
#define ARENA_APP_DESC() esp_app_get_description()
#else
#include <esp_ota_ops.h>
#define ARENA_APP_DESC() esp_ota_get_app_description()
#endif

#ifdef ARENA_MATTER
// Definies dans arena_matter.cpp : ce fichier n'inclut aucun en-tete Matter.
extern "C" uint8_t arena_matter_fabrics();
extern "C" void    arena_matter_forget();
extern "C" const char* arena_matter_last_event();
extern "C" void arena_matter_event_log(char* out, size_t n);
#endif   // OTA en mode pull (voir pullOta plus bas)
#include "arena_config.h"
#include "arena_net.h"
#include "arenaled.h"
#include "arena_map.h"
#include "arena_pf.h"
#include "arena_attract.h"
#include "arena_oled.h"

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

// Compteurs exportes par libs/Adafruit_NeoPixel/esp.c : ils disent si la sortie
// RMT part vraiment, ou si rmtInit() echoue (carte hors USB, donc pas de log).
extern "C" {
  extern volatile uint32_t espShowRmtFail, espShowFrames, espShowLockMiss;
  extern volatile int32_t  espShowBusType;
}

// --- OTA en mode "pull" ----------------------------------------------------
// Le POST /update pousse l'image depuis le callback AsyncTCP. Sur S3 ca tue la
// carte : Update.begin() efface la partition, ce qui bloque la tache AsyncTCP
// plusieurs secondes pendant que le client continue d'envoyer - lwIP manque de
// tampons et le chip tombe. Mesure du 2026-08-02 : 250 ko recus sur 1,6 Mo, puis
// un redemarrage qui ressemble a s'y meprendre a une mise a jour reussie.
//
// Ici c'est la carte qui va chercher l'image : elle lit au rythme qu'elle veut,
// donc rien ne s'accumule, et l'effacement se fait pendant que personne ne
// pousse. On passe aussi la taille reelle a esp_ota_begin() au lieu de
// OTA_SIZE_UNKNOWN, qui effacait les 3 Mo entiers de la partition.
static String        s_pullUrl;
static volatile bool s_pullPending = false;
static String        s_pullStatus  = "idle";
static uint32_t      s_pullDone = 0, s_pullTotal = 0;
static bool          s_pullFs = false;   // application ou systeme de fichiers

// Balayage WiFi. Lance depuis la boucle principale, jamais depuis un handler :
// WiFi.scanNetworks() bloque 2 a 5 s, et bloquer la tache AsyncTCP est
// exactement ce qui tuait la carte pendant les mises a jour.
static volatile bool s_scanWanted = false;
static String        s_scanJson   = "[]";
static uint32_t      s_scanAt     = 0;

// Envoi par morceaux depuis le navigateur (/api/fw).
static esp_ota_handle_t     s_fwHandle = 0;
static const esp_partition_t* s_fwPart = nullptr;
static size_t               s_fwTotal = 0, s_fwGot = 0;
static uint32_t             s_fwReboot = 0;   // 0 = pas de redemarrage arme

static bool pullOta(const String& url, bool fs, String& err) {
  if (!url.startsWith("http://")) { err = "seul http:// est supporte"; return false; }
  const int slash     = url.indexOf('/', 7);
  const String hostp  = (slash < 0) ? url.substring(7) : url.substring(7, slash);
  const String path   = (slash < 0) ? "/" : url.substring(slash);
  const int colon     = hostp.indexOf(':');
  const String host   = (colon < 0) ? hostp : hostp.substring(0, colon);
  const uint16_t port = (colon < 0) ? 80 : (uint16_t)hostp.substring(colon + 1).toInt();

  WiFiClient c;
  if (!c.connect(host.c_str(), port)) { err = "connexion refusee " + hostp; return false; }
  c.print(String("GET ") + path + " HTTP/1.1\r\nHost: " + hostp +
          "\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n");

  int status = 0; long len = -1;
  const uint32_t tHdr = millis();
  while (true) {
    if (millis() - tHdr > 15000) { err = "timeout en-tetes"; return false; }
    if (!c.available()) {
      if (!c.connected()) { err = "coupe pendant les en-tetes"; return false; }
      delay(5); continue;
    }
    String line = c.readStringUntil('\n'); line.trim();
    if (line.length() == 0) break;                       // fin des en-tetes
    if (!status && line.startsWith("HTTP/")) status = line.substring(9, 12).toInt();
    String low = line; low.toLowerCase();
    if (low.startsWith("content-length:")) len = line.substring(15).toInt();
  }
  if (status != 200) { err = "HTTP " + String(status); return false; }
  if (len <= 0) { err = "Content-Length absent (chunked non gere)"; return false; }

  // Deux destinations : l'application (via esp_ota_*) ou la partition du
  // systeme de fichiers, qui porte l'interface web. La seconde s'ecrit a la
  // main - il n'y a pas d'API "ota" pour elle - et LittleFS doit etre demonte
  // avant, sinon on reecrit sous les pieds du serveur qui vient de servir la page.
  const esp_partition_t* part = fs
      ? esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                 ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL)
      : esp_ota_get_next_update_partition(NULL);
  if (!part) { err = fs ? "partition fichiers introuvable" : "aucune partition OTA libre"; return false; }
  if ((size_t)len > part->size) {
    err = "image trop grande (" + String((uint32_t)len) + " > " + String(part->size) + ")";
    return false;
  }

  esp_ota_handle_t h = 0;
  esp_err_t e = ESP_OK;
  if (fs) {
    LittleFS.end();
    e = esp_partition_erase_range(part, 0, (((size_t)len) + 4095) & ~((size_t)4095));
    if (e != ESP_OK) { err = String("erase: ") + esp_err_to_name(e); return false; }
  } else {
    e = esp_ota_begin(part, (size_t)len, &h);
    if (e != ESP_OK) { err = String("esp_ota_begin: ") + esp_err_to_name(e); return false; }
  }

  s_pullTotal = (uint32_t)len;
  s_pullDone  = 0;
  static uint8_t buf[1460];                              // statique : pas sur la pile
  uint32_t tLast = millis();
  while (s_pullDone < (uint32_t)len) {
    const int n = c.read(buf, sizeof(buf));
    if (n > 0) {
      e = fs ? esp_partition_write(part, s_pullDone, buf, (size_t)n)
             : esp_ota_write(h, buf, (size_t)n);
      if (e != ESP_OK) {
        if (!fs) esp_ota_abort(h);
        err = String(fs ? "partition_write: " : "esp_ota_write: ") + esp_err_to_name(e);
        return false;
      }
      s_pullDone += (uint32_t)n;
      tLast = millis();
      // Respiration obligatoire : sans elle cette boucle monopolise le coeur et
      // le task watchdog tue la tache IDLE. 1 ms par paquet de 1460 octets
      // plafonne a ~1,4 Mo/s, tres au-dessus du debit reseau reel.
      delay(1);
    } else {
      if (!c.connected() && !c.available()) break;
      if (millis() - tLast > 20000) { esp_ota_abort(h); err = "timeout reception"; return false; }
      delay(2);
    }
  }
  if (s_pullDone != (uint32_t)len) {
    if (!fs) esp_ota_abort(h);
    err = "tronque " + String(s_pullDone) + "/" + String((uint32_t)len);
    return false;
  }
  if (fs) return true;                       // rien a valider : le redemarrage remonte LittleFS
  e = esp_ota_end(h);
  if (e != ESP_OK) { err = String("esp_ota_end: ") + esp_err_to_name(e); return false; }
  e = esp_ota_set_boot_partition(part);
  if (e != ESP_OK) { err = String("set_boot: ") + esp_err_to_name(e); return false; }
  return true;
}

static String stateJson() {
  arenaled::Rgbw c = arenaled::color();
  String j = "{";
  j += "\"fw\":\"" ARENA_FW_VERSION "\"";
  j += ",\"mode\":\"" + String(arenaled::modeName(arenaled::mode())) + "\"";
  j += ",\"bright\":" + String(arenaled::brightness());
  j += ",\"speed\":"  + String(arenaled::speed());
  j += ",\"gi\":"     + String(arenaled::gi());
  j += ",\"density\":" + String(arenaled::density());
  j += ",\"warm\":"   + String(arenaled::warm());
  j += ",\"inc\":"    + String(arenaled::incandescent() ? 1 : 0);
  j += ",\"boot\":"   + String(arenaled::bootOn() ? 1 : 0);
  // What the wall is running: seconds of ROM attract (0 = generic fallback)
  // and how many inserts the plan carries. The Game panel reads these.
  j += ",\"atr\":"    + String(arenaattract::available()
                              ? arenaattract::frames() * arenaattract::stepMs() / 1000 : 0);
  j += ",\"ins\":"    + String(arenapf::insertCount());
  j += ",\"fsu\":"    + String(LittleFS.usedBytes());
  j += ",\"fst\":"    + String(LittleFS.totalBytes());
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
  // Diagnostic sortie LED : pin reellement compilee, trames emises, echecs.
  j += ",\"pin\":"      + String(PIN_LED_DATA);
  j += ",\"rmtframes\":" + String((uint32_t)espShowFrames);
  j += ",\"rmtfail\":"  + String((uint32_t)espShowRmtFail);
  j += ",\"lockmiss\":" + String((uint32_t)espShowLockMiss);
  // Rendu gele pendant un appairage BLE. Sans ce champ, fps/ma/rmtframes figes
  // ressemblent a s'y meprendre a un rendu normal.
  j += ",\"paused\":" + String(arenaled::paused() ? "true" : "false");
  // L'ecran a-t-il repondu sur l'I2C au demarrage ? Sans ce temoin, un panneau
  // muet est indiscernable d'un panneau absent, d'une mauvaise adresse ou d'un
  // fil inverse - et il n'y a pas de port serie sur un mur accroche.
  j += ",\"oled\":" + String(arenaoled::found() ? "true" : "false");
  j += ",\"bustype\":"  + String((int32_t)espShowBusType);
  // OTA en mode pull : ou en est le telechargement declenche par /api/otapull.
  j += ",\"otast\":\""  + s_pullStatus + "\"";
  j += ",\"otadone\":"  + String(s_pullDone);
  j += ",\"otatot\":"   + String(s_pullTotal);
  j += ",\"fwgot\":"   + String((uint32_t)s_fwGot);
  j += ",\"fwtot\":"   + String((uint32_t)s_fwTotal);
#ifdef ARENA_MATTER
  // A combien de maisons la carte appartient. Si Maison dit "deja dans une autre
  // maison", c'est ce nombre qu'il faut regarder avant de conclure quoi que ce soit.
  j += ",\"fabrics\":" + String((int)arena_matter_fabrics());
  j += ",\"mev\":\"" + String(arena_matter_last_event()) + "\"";
  { char ev[560]; arena_matter_event_log(ev, sizeof(ev));
    j += ",\"mevlog\":\"" + String(ev) + "\""; }
#endif
  // Empreinte de build : les 8 premiers octets du SHA256 de l'ELF. C'est le SEUL
  // champ qui prouve qu'une OTA a bien remplace l'image - l'uptime ne prouve
  // rien (un envoi qui plante redemarre la carte exactement pareil), et un
  // compteur remis a zero au boot non plus.
  {
    const esp_app_desc_t* d = ARENA_APP_DESC();
    char sha[17];
    for (int i = 0; i < 8; i++) sprintf(sha + i * 2, "%02x", d->app_elf_sha256[i]);
    sha[16] = 0;
    j += ",\"build\":\"" + String(sha) + "\"";
  }
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

static void startServer() {
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
    if (r->hasParam("gi"))     arenaled::setGi(param8(r, "gi", arenaled::gi()));
    if (r->hasParam("density")) arenaled::setDensity(param8(r, "density", arenaled::density()));
    if (r->hasParam("warm"))   arenaled::setWarm(param8(r, "warm", arenaled::warm()));
    if (r->hasParam("inc"))    arenaled::setIncandescent(r->getParam("inc")->value().toInt() != 0);
    if (r->hasParam("boot"))   arenaled::setBootOn(r->getParam("boot")->value().toInt() != 0);
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

  // /api/latch?n=L9,L48   lamps held lit through attract, by the MACHINE's name
  // /api/latch?clear=1    release them all
  // Named, not numbered: the owner reads L9 off the playfield, and the mask
  // underneath is in PinMAME's numbering, which nobody should have to know.
  s_server.on("/api/latch", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (r->hasParam("clear")) { arenaled::setLatched(0); arenaled::save(); }
    else if (r->hasParam("n")) {
      uint64_t m = 0;
      String list = r->getParam("n")->value();
      list.trim();
      int start = 0;
      while (start < (int)list.length()) {
        int comma = list.indexOf(',', start);
        if (comma < 0) comma = list.length();
        String one = list.substring(start, comma);
        one.trim();
        start = comma + 1;
        if (!one.length()) continue;
        const int idx = arenapf::indexOf(one.c_str());
        const arenapf::Insert* ins = (idx >= 0) ? arenapf::insert((uint8_t)idx) : nullptr;
        if (!ins || ins->lamp < 0) {
          r->send(400, "text/plain", "unknown insert: " + one);
          return;
        }
        m |= (1ULL << ins->lamp);
      }
      arenaled::setLatched(m);
      arenaled::save();
    }
    // Answer in the owner's names, not in the internal mask.
    String out = "{\"latched\":[";
    bool first = true;
    const uint64_t m = arenaled::latched();
    for (uint8_t i = 0; i < arenapf::insertCount(); i++) {
      const arenapf::Insert* ins = arenapf::insert(i);
      if (!ins || ins->lamp < 0 || !((m >> ins->lamp) & 1ULL)) continue;
      if (!first) out += ',';
      first = false;
      out += "\"" + String(ins->name) + "\"";
    }
    out += "]}";
    r->send(200, "application/json", out);
  });

  //  /api/music?e=..&b=..&t=..   0..255 — drive the music mode from anything
  //  that can hit REST at ~20 Hz. Overrides the on-board mic for 2 s per push.
  s_server.on("/api/music", HTTP_GET, [](AsyncWebServerRequest* r) {
    arenaled::musicPush(param8(r, "e", 0), param8(r, "b", 0), param8(r, "t", 0));
    r->send(200, "text/plain", "ok");
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

  // ---- Sub-paths must be registered BEFORE their parent --------------------
  // This server matches a handler when the request URL equals its URI *or*
  // starts with it followed by '/'. So "/api/zones" also answers
  // "/api/zones/reset", and whichever was registered first wins. Registered the
  // other way round, /api/zones/reset silently returned the zone list and the
  // UI's "Reset to template" button did nothing at all - which is exactly what
  // it had been doing, unnoticed, since there was no hardware to try it on.
  s_server.on("/api/zones/reset", HTTP_GET, [](AsyncWebServerRequest* r) {
    arenamap::reset();
    arenamap::save();
    r->send(200, "application/json", arenamap::toJson());
  });

  s_server.on("/api/ledmap/reset", HTTP_GET, [](AsyncWebServerRequest* r) {
    arenapf::clearAssignment();
    arenapf::save();
    r->send(200, "text/plain", "cleared");
  });

  s_server.on("/api/zones", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", arenamap::toJson());
  });

  //  POST /api/zones with the same JSON shape -> replace + persist the insert map.
  s_server.on("/api/zones", HTTP_POST,
    [](AsyncWebServerRequest* r) {
      String* body = (String*)r->_tempObject;
      // No body at all is almost never malformed JSON — it is the wrong content
      // type. ESPAsyncWebServer swallows application/x-www-form-urlencoded and
      // multipart into request params and never calls the body handler, so
      // `curl -d` silently arrives here empty while the browser's fetch (which
      // sends text/plain) works. Say which of the two failures this is.
      if (!body) {
        r->send(400, "text/plain",
                "empty body - send the JSON raw, not form-encoded "
                "(curl: --data-binary + -H 'Content-Type: application/json')");
        return;
      }
      bool ok = arenamap::fromJson(body->c_str());
      if (ok) ok = arenamap::save();
      delete body; r->_tempObject = nullptr;
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

  // --- Playfield geometry ---------------------------------------------------
  //  /api/pf                      the 99 inserts and where they are (fixed)
  //  /api/ledmap                  which chain index sits on which insert
  //  /api/assign?led=N&ins=M      place one pixel (ins=none to unplace it)
  //  /api/ledmap/reset            forget every placement
  s_server.on("/api/pf", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", arenapf::insertsJson());
  });

  // /api/insert?ins=N[&name=L48][&r=&g=&b=&w=][&clear=1]
  // Edit one insert: its label (the machine's own, from the manual — the shipped
  // one is a guess) and the colour of its plastic. clear=1 drops both back to
  // the shipped label and no colour.
  s_server.on("/api/insert", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!r->hasParam("ins")) { r->send(400, "text/plain", "ins= required"); return; }
    const int ins = r->getParam("ins")->value().toInt();
    if (ins < 0 || ins >= arenapf::insertCount()) { r->send(400, "text/plain", "ins out of range"); return; }
    if (r->hasParam("clear")) {
      arenapf::setName((uint8_t)ins, "");
      arenapf::setColour((uint8_t)ins, { 0, 0, 0, 0 });
      arenapf::setHidden((uint8_t)ins, false);   // "clear" rend la pastille au plan
    } else {
      if (r->hasParam("hide"))
        arenapf::setHidden((uint8_t)ins, r->getParam("hide")->value().toInt() != 0);
      if (r->hasParam("name")) arenapf::setName((uint8_t)ins, r->getParam("name")->value().c_str());
      if (r->hasParam("r") || r->hasParam("g") || r->hasParam("b") || r->hasParam("w"))
        arenapf::setColour((uint8_t)ins, { param8(r, "r", 0), param8(r, "g", 0),
                                           param8(r, "b", 0), param8(r, "w", 0) });
    }
    arenapf::saveNames();
    arenapf::saveColours();
    arenapf::saveHidden();
    r->send(200, "application/json", arenapf::insertsJson());
  });

  s_server.on("/api/ledmap", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", arenapf::toJson());
  });

  // Saved on every click. Placing 99 inserts is a long session under a
  // playfield; losing it to a power cut at pixel 80 is not acceptable, and the
  // write is a few hundred bytes onto a partition that is otherwise idle.
  s_server.on("/api/assign", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!r->hasParam("led")) { r->send(400, "text/plain", "led= [&ins=N|none]"); return; }
    const int led = r->getParam("led")->value().toInt();
    uint8_t ins = arenapf::UNASSIGNED;
    if (r->hasParam("ins")) {
      const String v = r->getParam("ins")->value();
      if (v != "none" && v.length()) ins = (uint8_t)v.toInt();
    }
    if (!arenapf::setLedInsert((uint16_t)led, ins)) {
      r->send(400, "text/plain", "led or ins out of range");
      return;
    }
    arenapf::save();
    r->send(200, "application/json", arenapf::toJson());
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

  // --- Game bundle upload ---------------------------------------------------
  //  POST /api/game?target=pf       multipart file -> /arena_pf.json
  //  POST /api/game?target=attract  multipart file -> /arena_attract.bin
  //
  // This is what makes the wall a product instead of a developer project: a
  // different machine is two files, uploaded from the browser — no PlatformIO,
  // no littlefs rebuild, no toolchain. The file lands in a .new temp first and
  // is VALIDATED before it replaces anything: a truncated upload or a wrong
  // file must never cost the working setup. Then a clean reboot rather than a
  // live swap — the render task reads these structures at 63 fps and a reboot
  // is 3 s of dark wall, which is cheaper than a use-after-free is expensive.
  // The pixel mapping lives in NVS and survives; a NEW table's inserts differ,
  // so stale assignments are dropped at load when they point past the table.
  s_server.on("/api/game", HTTP_POST,
    [](AsyncWebServerRequest* r) {
      const bool pf = r->hasParam("target") && r->getParam("target")->value() == "pf";
      const char* tmp = pf ? "/arena_pf.new" : "/arena_attract.new";
      const char* dst = pf ? "/arena_pf.json" : "/arena_attract.bin";
      bool ok = false;
      File f = LittleFS.open(tmp, "r");
      if (f) {
        if (pf) {
          JsonDocument doc;
          ok = !deserializeJson(doc, f) && doc["inserts"].is<JsonArray>() &&
               doc["inserts"].as<JsonArray>().size() > 0;
        } else {
          uint16_t hdr[2] = { 0, 0 };
          f.read((uint8_t*)hdr, 4);
          ok = hdr[0] > 0 && hdr[1] > 0 && hdr[1] <= ARENA_ATTRACT_MAX_FRAMES &&
               f.size() == (size_t)4 + (size_t)hdr[1] * 8;
        }
        f.close();
      }
      if (!ok) {
        LittleFS.remove(tmp);
        r->send(400, "text/plain", pf ? "not a valid pf.json (needs a non-empty inserts array)"
                                      : "not a valid attract.bin (u16 step, u16 frames <= 12288, frames x u64)");
        return;
      }
      LittleFS.remove(dst);
      LittleFS.rename(tmp, dst);
      r->send(200, "text/plain", "OK, rebooting");
      delay(200);
      ESP.restart();
    },
    [](AsyncWebServerRequest* r, String fn, size_t idx, uint8_t* data, size_t len, bool done) {
      const bool pf = r->hasParam("target") && r->getParam("target")->value() == "pf";
      const char* tmp = pf ? "/arena_pf.new" : "/arena_attract.new";
      if (!idx) { LittleFS.remove(tmp); }
      File f = LittleFS.open(tmp, idx ? "a" : "w");
      if (f) { f.write(data, len); f.close(); }
      (void)fn; (void)done;
    });

  // --- OTA: POST a firmware .bin, or the web UI with ?target=fs -------------
  //  /update             -> application partition   (firmware.bin)
  //  /update?target=fs   -> filesystem partition    (littlefs.bin)
  //
  // The second one matters more than it looks: without it the only way to change
  // the web UI is a USB cable, and this board spends its life behind a playfield.
  // The filesystem has to be unmounted before it is overwritten, so everything
  // served from LittleFS is gone until the reboot at the end — expected, not a
  // failure. Note the reply below often never reaches the client: the restart
  // beats the TCP flush, so curl reports HTTP 000 on a *successful* update.
  // Verify by uptime, not by the response (ARENA_LED.md §4 B).
  // Demande a la carte d'aller chercher elle-meme une image (voir pullOta).
  // C'est le chemin fiable sur S3 ; /update reste pour le WROOM et l'interface.
#ifdef ARENA_MATTER
  // Fait oublier toutes les maisons, le WiFi compris. La carte quitte le reseau
  // et ne revient que par un appairage Bluetooth : d'ou le mot de passe dans
  // l'URL, pour qu'un clic distrait ne mette pas le mur hors ligne.
  s_server.on("/api/matter/forget", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!r->hasParam("confirm") || r->getParam("confirm")->value() != "oui") {
      r->send(400, "text/plain",
              "Efface les maisons Matter ET le WiFi. La carte quittera le reseau\n"
              "jusqu'a un nouvel appairage au telephone. Pour confirmer :\n"
              "  /api/matter/forget?confirm=oui\n");
      return;
    }
    r->send(200, "text/plain", "oubli en cours, la carte redemarre en appairage");
    delay(300);
    arena_matter_forget();
  });
#endif

  // --- Mise a jour par le navigateur, en morceaux ---------------------------
  // Le client telecharge le .bin depuis pinballs.store et le depose ici. C'est
  // le chemin destine au proprietaire : il ne demande aucun serveur chez lui et
  // il marche depuis un telephone.
  //
  // Pourquoi en morceaux, et pas un seul POST : un envoi d'un bloc tue le S3.
  // Mesure du 2026-08-02 - 250 ko recus sur 1,6 Mo puis chute. La cause n'est
  // pas le volume, c'est le RECOUVREMENT : esp_ota_begin() efface la partition
  // en bloquant plusieurs secondes, le navigateur continue d'emettre pendant ce
  // temps, lwIP manque de tampons et la puce tombe.
  //
  // On separe donc les phases. "begin" fait l'effacement dans SA propre requete,
  // et le navigateur attend la reponse : rien n'est en vol pendant l'effacement.
  // Ensuite chaque morceau est acquitte avant que le suivant parte, donc le
  // debit est plafonne par la carte elle-meme et ne peut plus la submerger.
  // Balayage WiFi - en LECTURE seule. Sous Matter c'est l'application Maison qui
  // fournit le reseau ; la carte ne peut pas en changer elle-meme (begin() lit
  // les identifiants puis sort avant de s'en servir). Ce que ce balayage sert
  // vraiment : savoir quelle puissance de signal le mur recoit LA OU IL EST
  // ACCROCHE, ce qu'aucun telephone tenu a la main ne dit.
  s_server.on("/api/wifiscan", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (r->hasParam("results")) {
      wifi_ap_record_t apInfo = {};
      const bool apInfoOk = (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK);
      String j = "{\"age\":" + String(s_scanAt ? (millis() - s_scanAt) / 1000 : 9999) +
                 ",\"busy\":" + String(s_scanWanted ? "true" : "false") +
                 ",\"rssi\":" + String(apInfoOk ? (int)apInfo.rssi : 0) +
                 ",\"ssid\":\"" + String(apInfoOk ? (const char*)apInfo.ssid : "") + "\"" +
                 ",\"nets\":" + s_scanJson + "}";
      r->send(200, "application/json", j);
      return;
    }
    s_scanWanted = true;
    r->send(200, "text/plain", "scan lance");
  });

  s_server.on("/api/fw", HTTP_POST,
    [](AsyncWebServerRequest* r) {
      // --- begin : reserve et efface, une bonne fois ---
      if (r->hasParam("begin")) {
        const size_t sz = (size_t)r->getParam("begin")->value().toInt();
        if (sz < 65536) { r->send(400, "text/plain", "taille invalide"); return; }
        if (s_fwHandle) { esp_ota_abort(s_fwHandle); s_fwHandle = 0; }
        s_fwPart = esp_ota_get_next_update_partition(NULL);
        if (!s_fwPart) { r->send(500, "text/plain", "aucune partition OTA"); return; }
        if (sz > s_fwPart->size) { r->send(400, "text/plain", "image trop grande"); return; }
        // Taille reelle, pas OTA_SIZE_UNKNOWN : on efface ce qu'on ecrit, pas
        // les 3 Mo de la partition.
        const esp_err_t e = esp_ota_begin(s_fwPart, sz, &s_fwHandle);
        if (e != ESP_OK) {
          s_fwHandle = 0;
          r->send(500, "text/plain", String("esp_ota_begin: ") + esp_err_to_name(e));
          return;
        }
        s_fwTotal = sz; s_fwGot = 0;
        r->send(200, "text/plain", "pret");
        return;
      }
      // --- end : valide et redemarre ---
      if (r->hasParam("end")) {
        if (!s_fwHandle) { r->send(409, "text/plain", "aucun envoi en cours"); return; }
        if (s_fwGot != s_fwTotal) {
          esp_ota_abort(s_fwHandle); s_fwHandle = 0;
          r->send(400, "text/plain", "tronque " + String(s_fwGot) + "/" + String(s_fwTotal));
          return;
        }
        esp_err_t e = esp_ota_end(s_fwHandle);
        s_fwHandle = 0;
        if (e != ESP_OK) { r->send(400, "text/plain", String("image refusee: ") + esp_err_to_name(e)); return; }
        e = esp_ota_set_boot_partition(s_fwPart);
        if (e != ESP_OK) { r->send(500, "text/plain", String("set_boot: ") + esp_err_to_name(e)); return; }
        r->send(200, "text/plain", "ok, redemarrage");
        s_fwReboot = millis();          // laisse la reponse partir avant de couper
        return;
      }
      // --- abandon explicite ---
      if (r->hasParam("abort")) {
        if (s_fwHandle) { esp_ota_abort(s_fwHandle); s_fwHandle = 0; }
        s_fwGot = s_fwTotal = 0;
        r->send(200, "text/plain", "abandonne");
        return;
      }
      // --- un morceau : la reponse part ICI, apres l'ecriture ---
      if (!s_fwHandle) { r->send(409, "text/plain", "appelle ?begin= d'abord"); return; }
      r->send(200, "text/plain", String(s_fwGot));
    },
    NULL,
    // Corps brut du morceau. Ecrit au fil de l'eau : un morceau de 32 ko tient
    // dans les tampons, et l'acquittement ci-dessus ne part qu'une fois ecrit.
    [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
      if (!s_fwHandle || !len) return;
      if (esp_ota_write(s_fwHandle, data, len) != ESP_OK) {
        esp_ota_abort(s_fwHandle); s_fwHandle = 0;
        return;
      }
      s_fwGot += len;
    });

  s_server.on("/api/otapull", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!r->hasParam("url")) { r->send(400, "text/plain", "url= manquant"); return; }
    if (s_pullPending) { r->send(409, "text/plain", "un telechargement est deja en cours"); return; }
    s_pullUrl     = r->getParam("url")->value();
    s_pullDone    = 0;
    s_pullTotal   = 0;
    s_pullFs      = r->hasParam("target") && r->getParam("target")->value() == "fs";
    s_pullStatus  = "demarre";
    s_pullPending = true;   // la boucle principale prend le relais
    r->send(200, "text/plain", "telechargement lance depuis " + s_pullUrl +
                               "\nsuivre otast / otadone / otatot dans /api/state");
  });

  s_server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest* r) {
      bool ok = !Update.hasError();
      r->send(ok ? 200 : 500, "text/plain", ok ? "OK, rebooting" : "FAIL");
      if (ok) { delay(200); ESP.restart(); }
    },
    [](AsyncWebServerRequest* r, String fn, size_t idx, uint8_t* data, size_t len, bool done) {
      if (!idx) {
        const bool fs = r->hasParam("target") && r->getParam("target")->value() == "fs";
        Serial.printf("[ota] start %s -> %s\n", fn.c_str(), fs ? "filesystem" : "app");
        if (fs) LittleFS.end();                      // cannot be mounted while it is rewritten
        // Taille reelle plutot que UPDATE_SIZE_UNKNOWN : ce dernier efface la
        // partition entiere (3 Mo) d'un bloc et gele AsyncTCP le temps que ca
        // dure, ce qui est la cause du plantage decrit devant pullOta().
        const size_t sz = r->contentLength() ? r->contentLength() : UPDATE_SIZE_UNKNOWN;
        if (!Update.begin(sz, fs ? U_SPIFFS : U_FLASH)) Update.printError(Serial);
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
}

void begin() {
  // --- WiFi: NVS credentials (set from the UI) override the compile-time ones ---
  s_prefs.begin("arenanet", false);
  String ssid = s_prefs.getString("ssid", ARENA_STA_SSID);
  String pass = s_prefs.getString("pass", ARENA_STA_PASS);

#ifdef ARENA_MATTER
  // Matter owns WiFi: commissioning stores the credentials and CHIP drives
  // esp_wifi. We only wait for an address. No SoftAP (an AP interface fights
  // the CHIP station state machine) and no ESPmDNS (CHIP minimal mDNS holds
  // port 5353) - reach the board by IP; proper mdns unification is P3 work.
  // No blocking wait and NO web server yet: the PASE handshake at pairing
  // time needs every byte of heap this chip has (measured: abort() on the BLE
  // connect with the server up), and a web server without an address serves
  // nobody. matterTick() below brings it up the moment WiFi is provisioned.
  Serial.println("[net] Matter owns WiFi - server deferred until an address");
  s_mode = "Matter";
  s_ip = "0.0.0.0";
  return;
#else
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

#endif

  startServer();
  Serial.println("[net] http server up");
}

void loop() {
  // Le balayage tourne ICI : il bloque plusieurs secondes et n'a rien a faire
  // dans un handler HTTP.
  if (s_scanWanted) {
    s_scanWanted = false;
    // API IDF, pas l'objet Arduino WiFi : sous Matter c'est CHIP qui possede
    // esp_wifi et arenanet::begin() sort avant tout WiFi.mode(), donc le
    // wrapper Arduino n'a aucun etat - il rend 0 reseau, RSSI 0, SSID vide.
    // Meme lecon que netHasIp(), qui lit deja au niveau esp_netif.
    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    String j = "[";
    if (esp_wifi_scan_start(&cfg, true) == ESP_OK) {
      uint16_t n = 0;
      esp_wifi_scan_get_ap_num(&n);
      if (n > 20) n = 20;
      if (n) {
        wifi_ap_record_t* ap = (wifi_ap_record_t*)calloc(n, sizeof(wifi_ap_record_t));
        if (ap && esp_wifi_scan_get_ap_records(&n, ap) == ESP_OK) {
          for (uint16_t i = 0; i < n; i++) {
            if (i) j += ',';
            j += "{\"s\":\"" + String((const char*)ap[i].ssid) +
                 "\",\"r\":" + String((int)ap[i].rssi) +
                 ",\"c\":" + String((int)ap[i].primary) +
                 ",\"e\":" + String(ap[i].authmode == WIFI_AUTH_OPEN ? 0 : 1) + "}";
          }
        }
        free(ap);
      }
    }
    j += "]";
    s_scanJson = j;
    s_scanAt = millis();
  }

  // Redemarrage differe apres un envoi par morceaux : couper dans le handler
  // tuerait la reponse HTTP avant qu'elle parte, et le navigateur afficherait
  // un echec sur une mise a jour reussie.
  if (s_fwReboot && millis() - s_fwReboot > 600) ESP.restart();

  // Le telechargement OTA tourne ICI, dans la tache principale - surtout pas
  // dans le callback AsyncTCP, ou bloquer sur la flash fait tomber la pile WiFi.
  if (s_pullPending) {
    s_pullPending = false;
    s_pullStatus  = "en cours";
    String err;
    if (pullOta(s_pullUrl, s_pullFs, err)) {
      s_pullStatus = "ok, redemarrage";
      delay(200);
      ESP.restart();
    } else {
      s_pullStatus = "echec: " + err;
    }
  }
}

#ifdef ARENA_MATTER
// Called from loop(): once commissioning hands us a network, raise the HTTP
// server. Until then the wall renders and Matter owns the radio.
void matterTick() {
  static bool up = false;
  esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t info;
  if (up || !n || esp_netif_get_ip_info(n, &info) != ESP_OK || info.ip.addr == 0) return;
  up = true;
  s_ip = IPAddress(info.ip.addr).toString();
  Serial.printf("[net] ip=%s - starting the web server\n", s_ip.c_str());
  startServer();
}
#endif

}  // namespace arenanet
