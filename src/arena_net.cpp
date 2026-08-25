#include <WiFi.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>   // le pixel de statut, meme bibliotheque que la chaine
#include <esp_ota_ops.h>   // validation de l'image apres une OTA

// esp_read_mac : dans esp_mac.h depuis IDF 5, dans esp_system.h avant.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include <esp_mac.h>
#else
#include <esp_system.h>
#endif
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>   // ecriture directe de la partition fichiers
#include <esp_flash.h>       // taille physique de la puce, via son identifiant JEDEC
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
#include "arena_peers.h"

namespace arenanet {

static AsyncWebServer s_server(80);
static Preferences    s_prefs;

// Un redemarrage demande par HTTP ne doit PAS partir depuis le gestionnaire :
// la reponse n'est pas encore sur le fil, et l'appelant ne verrait qu'une
// connexion coupee - impossible de distinguer "c'est fait" de "ca a plante".
static uint32_t       s_rebootAt = 0;

// Retour arriere automatique : la MOITIE manquante.
//
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE fait demarrer toute image fraichement
// installee a l'essai. Si personne ne la declare saine, le prochain demarrage
// repart sur l'ANCIENNE - ce qui est exactement le filet qu'on veut... et ce
// qui annulerait CHAQUE mise a jour si cet appel manquait. Les deux moities
// vont ensemble, il n'y a pas de demi-mesure possible.
//
// On ne valide pas au demarrage : une image qui plante trois secondes apres
// avoir demarre serait declaree bonne avant de tomber. On attend d'avoir une
// adresse ET une minute de fonctionnement, ce qui couvre le seul mode de panne
// que le retour arriere sait reparer - une image qui ne tient pas debout.
static bool s_imgValidated = false;

static void validateImageWhenHealthy() {
  if (s_imgValidated) return;
  if (millis() < 60000) return;
  const char* ip = arenanet::ip();
  if (!ip || !strcmp(ip, "0.0.0.0")) return;

  esp_ota_img_states_t st;
  const esp_partition_t* run = esp_ota_get_running_partition();
  if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
      Serial.println("[ota] image declaree saine - plus de retour arriere");
    else
      Serial.println("[ota] ECHEC de la validation - la carte reviendra a l'ancienne image");
  }
  s_imgValidated = true;
}

// Identite de la carte. La MAC est gravee en usine, donc unique sans reglage ni
// numero de serie a gerer : deux murs sortis de la meme image ne peuvent pas se
// confondre. Le proprietaire peut ensuite mettre "Volcano" ou "Arena".
static String         s_name;
static String         s_mac;

const String& wallName() { return s_name; }

// Un nom d'hote mDNS n'accepte ni espace ni accent ni majuscule.
static String hostify(const String& in) {
  String o;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += c;
    else if (o.length() && o[o.length()-1] != '-')       o += '-';
  }
  while (o.length() && o[o.length()-1] == '-') o.remove(o.length()-1);
  return o.length() ? o : String("playfield");
}
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
// Pourquoi la derniere tentative d'association a echoue. Sans ca, un mot de
// passe faux, un reseau absent et un reseau en 5 GHz donnent tous le meme
// resultat visible : la carte repart en point d'acces, sans un mot.
static String        s_staReason  = "";

static volatile bool s_scanWanted = false;
static String        s_scanJson   = "[]";
// Pourquoi un balayage n'a rien rendu. Une liste vide est ambigue - refus du
// pilote, ou vraiment aucun reseau - et les deux se soignent differemment.
static String        s_scanErr    = "";

// Un ecran noir et une carte qui redemarre se ressemblent de l'exterieur. La
// difference se lit ici : le compteur monte a chaque demarrage, et la cause du
// dernier reset dit si c'etait une coupure, un plantage ou un chien de garde.
static uint8_t       s_statPin    = ARENA_STATUS_PIN;

// Le pixel de statut et la chaine du mur se partagent le peripherique RMT, et
// le conflit vient de la CADENCE, pas de la bibliotheque.
//
// L'histoire, parce qu'elle a coute trois tours : neopixelWrite() re-reclame un
// canal RMT a chaque appel, et a 25 Hz - la respiration de ce temoin - il
// rendait la chaine muette ("RMT DRIVER ERR" a 25 Hz, exactement). J'ai d'abord
// eteint le temoin, ce qui reglait le conflit en supprimant l'un des deux ; puis
// je l'ai passe a Adafruit_NeoPixel, et il est sorti BLANC A FOND.
//
// Les timings de la branche heritee d'Adafruit sont ceux du WS2812 (T1H 800 ns);
// un SK6812 attend 600 ns et lit alors des uns partout - du blanc plein, a
// pleine echelle, insensible a la luminosite qu'on lui demande. neopixelWrite()
// rendait les bonnes couleurs sur CETTE carte : c'est un fait mesure, et il
// repasse devant mon raisonnement.
//
// On revient donc a neopixelWrite(), et on retire ce qui obligeait au 25 Hz : la
// respiration. Le temoin est fixe, et n'est reecrit que lorsque sa couleur
// change - plus un rafraichissement lent de securite.
static bool          s_statOn     = true;
// Luminosite du temoin, 0..255. Elle etait ecrite en dur, et un temoin en dur
// est un temoin qui eblouit quelqu'un : la meme LED sert de veilleuse a cote
// d'un lit et de repere en plein jour. 60 par defaut - visible sans agresser.
static uint8_t       s_statBright = 60;
static String        s_reset      = "?";
static uint32_t      s_boots      = 0;

// Un brownout ne laisse aucune trace apres le redemarrage suivant : la cause du
// reset ne parle que du DERNIER. Ce compteur-la, lui, tient le total, et c est
// lui qui distingue "une fois, en debranchant" de "a chaque rafale WiFi".
static uint32_t      s_brownouts  = 0;
static uint8_t       s_txq        = ARENA_WIFI_TXPWR_QDBM;   // quarts de dBm
static bool          s_txDerated  = false;   // baissee d office apres un brownout

// A appeler seulement quand la radio tourne (esp_wifi_start est passe) : sinon
// l IDF repond ESP_ERR_WIFI_NOT_STARTED et la valeur est perdue en silence.
static void applyTxPower() {
  const esp_err_t e = esp_wifi_set_max_tx_power((int8_t)s_txq);
  if (e != ESP_OK) {
    Serial.printf("[net] puissance TX refusee (%d)\n", (int)e);
    return;
  }
  Serial.printf("[net] puissance TX = %.2f dBm%s\n", s_txq / 4.0f,
                s_txDerated ? "  (baissee apres un brownout)" : "");
}

// La taille de flash annoncee par ESP.getFlashChipSize() vient de l en-tete que
// l outil de flashage a ecrit : elle repete ce que le build a SUPPOSE, pas ce que
// la puce porte. L identifiant JEDEC, lui, vient du silicium - octet 3 = capacite
// en puissance de deux. C est la seule source qui puisse contredire le build.
static uint32_t physicalFlashBytes() {
  uint32_t id = 0;
  if (esp_flash_read_id(esp_flash_default_chip, &id) != ESP_OK) return 0;
  const uint8_t cap = (uint8_t)(id & 0xFF);
  if (cap < 0x14 || cap > 0x1A) return 0;      // hors 1 Mo..64 Mo : code inconnu
  return 1UL << cap;
}

// Fin de la derniere partition de la table. Si elle depasse la flash reelle, les
// partitions du haut - OTA et fichiers, justement - pointent dans le vide : une
// OTA s ecrit par-dessus elle-meme et LittleFS se corrompt au premier montage.
// Rien de tout cela ne se voit au demarrage, ce qui est exactement le probleme.
static uint32_t partitionEndBytes() {
  uint32_t end = 0;
  esp_partition_iterator_t it =
      esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it) {
    const esp_partition_t* p = esp_partition_get(it);
    if (p && p->address + p->size > end) end = p->address + p->size;
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  return end;
}

static uint32_t s_flashPhys = 0;
static uint32_t s_partEnd   = 0;
static bool     s_flashBad  = false;

static const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "mise sous tension";
    case ESP_RST_EXT:       return "reset externe (bouton)";
    case ESP_RST_SW:        return "redemarrage logiciel";
    case ESP_RST_PANIC:     return "PLANTAGE (panic)";
    case ESP_RST_INT_WDT:   return "chien de garde d interruption";
    case ESP_RST_TASK_WDT:  return "chien de garde de tache";
    case ESP_RST_WDT:       return "chien de garde";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (alimentation)";
    case ESP_RST_DEEPSLEEP: return "reveil de sommeil profond";
    default:                return "inconnue";
  }
}
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

void resetNetwork() {
  s_prefs.clear();
  Serial.println("[net] nom et identifiants WiFi effaces");
}

void forgetHomes() {
#ifdef ARENA_MATTER
  arena_matter_forget();
#else
  Serial.println("[net] pas de Matter dans cette image - rien a oublier");
#endif
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
  // Le repeteur decide si le 1er pixel physique est tenu eteint. C'est le
  // reglage qui explique "les LED ne s'allument pas" une fois sur deux, et il
  // n'etait expose NULLE PART : ni la page ni /api/state ne disaient sa valeur,
  // donc personne ne pouvait verifier ce qu'il venait de changer.
  j += ",\"repeater\":" + String(arenaled::repeater() ? 1 : 0);
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
  // Memoire. C'est ce qui decide si la PSRAM est necessaire : ce qui compte
  // n'est pas le total mais le plus BAS jamais atteint, car le manque de tas ne
  // se manifeste qu'a la pointe - typiquement la poignee de main d'appairage.
  j += ",\"heap\":"    + String((uint32_t)ESP.getFreeHeap());
  j += ",\"heapmin\":" + String((uint32_t)ESP.getMinFreeHeap());
  j += ",\"heapblk\":" + String((uint32_t)ESP.getMaxAllocHeap());
  j += ",\"psram\":"   + String((uint32_t)ESP.getFreePsram());

  // Retour arriere OTA : l'etat de l'image qui tourne. C'est ce qui dit si le
  // filet est reellement arme, et un cable serie n'est pas une facon durable de
  // le verifier - une OTA reenumere l'USB natif et le port disparait.
  //   "trial"   installee, pas encore declaree saine : un redemarrage revient
  //             a l'ancienne image
  //   "valid"   declaree saine, plus de retour en arriere
  //   "nofw"    le bootloader n'a pas le retour arriere (option absente)
  {
    esp_ota_img_states_t ost;
    const esp_partition_t* rp = esp_ota_get_running_partition();
    const char* v = "nofw";
    if (rp && esp_ota_get_state_partition(rp, &ost) == ESP_OK) {
      if      (ost == ESP_OTA_IMG_PENDING_VERIFY) v = "trial";
      else if (ost == ESP_OTA_IMG_VALID)          v = "valid";
      else if (ost == ESP_OTA_IMG_UNDEFINED)      v = "nofw";
      else                                        v = "other";
    }
    j += ",\"otavalid\":\"" + String(v) + "\"";
    j += ",\"slot\":\"" + String(rp ? rp->label : "?") + "\"";
  }

  // Les autres murs vus sur le reseau, et le comportement choisi face a eux.
  j += ",\"link\":\"" + String(arenapeers::linkName(arenapeers::link())) + "\"";
  j += ",\"rank\":"  + String((int)arenapeers::rank());
  j += ",\"shared\":" + String(arenapeers::sharedPower() ? 1 : 0);
  j += ",\"share\":"  + String((int)arenaled::budgetShare());
  j += ",\"peers\":" + arenapeers::json();

  // Defaut du limiteur de sortie : le seul signal materiel qui dit qu'il se
  // passe quelque chose AU BOUT du cable, la ou personne ne regarde.
  j += ",\"ledfault\":"  + String(arenaled::ledFault() ? 1 : 0);
  j += ",\"ledfaultn\":" + String((int)arenaled::ledFaultCount());

  // Qui est ce mur. Indispensable des qu'il y en a plusieurs : c'est ce qui
  // permet de balayer le reseau et de dire lequel est lequel.
  j += ",\"name\":\"" + s_name + "\"";
  j += ",\"mac\":\""  + s_mac  + "\"";

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
  j += ",\"staFail\":\"" + s_staReason + "\"";
  j += ",\"reset\":\"" + s_reset + "\",\"boots\":" + String(s_boots);
  j += ",\"brownouts\":" + String(s_brownouts);
  j += ",\"txpwr\":" + String(s_txq / 4.0f, 2);
  j += ",\"txDerated\":" + String(s_txDerated ? 1 : 0);
  j += ",\"flashMb\":" + String(s_flashPhys >> 20);
  j += ",\"partMb\":"  + String(s_partEnd >> 20);
  j += ",\"flashBad\":" + String(s_flashBad ? 1 : 0);
  j += ",\"statusLed\":" + String(s_statOn ? 1 : 0);
  j += ",\"statusBright\":" + String(s_statBright);
  // La page doit savoir QUI possede le reseau. Sous Matter, l'appairage apporte
  // les identifiants et il n'y a rien a saisir ; sans Matter, la saisie est le
  // SEUL moyen de rejoindre un reseau. Afficher le discours Matter dans une
  // image qui n'en a pas, c'est dire au proprietaire qu'il n'a rien a faire
  // pendant qu'on lui retire le seul geste qui marche.
#ifdef ARENA_MATTER
  j += ",\"matter\":1";
#else
  j += ",\"matter\":0";
#endif
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
    if (r->hasParam("repeater")) arenaled::setRepeater(r->getParam("repeater")->value().toInt() != 0);
    if (r->hasParam("pin"))      arenaled::setPin((uint8_t)r->getParam("pin")->value().toInt());
    if (r->hasParam("statuspin")) {
      const uint8_t p = (uint8_t)r->getParam("statuspin")->value().toInt();
      if (p <= 48) {
        s_statPin = p;
        s_prefs.putUChar("statpin", p);
      }
    }
    // /api/set?btnup=7&btndown=15&btnok=17
    // Les trois ensemble : un remappage partiel laisserait deux roles sur la
    // meme broche, donc un poussoir muet et un autre qui fait deux choses.
    if (r->hasParam("btnup") && r->hasParam("btndown") && r->hasParam("btnok")) {
      arenaoled::setButtons((uint8_t)r->getParam("btnup")->value().toInt(),
                            (uint8_t)r->getParam("btndown")->value().toInt(),
                            (uint8_t)r->getParam("btnok")->value().toInt());
    }
    // /api/set?btnrot=1 : faire tourner les roles d'un cran, a l'aveugle.
    if (r->hasParam("btnrot")) arenaoled::rotateButtons();
    // /api/set?statusbright=0..255 - le temoin de la carte, pas le mur.
    if (r->hasParam("statusbright")) {
      long v = r->getParam("statusbright")->value().toInt();
      if (v < 0)   v = 0;
      if (v > 255) v = 255;
      s_statBright = (uint8_t)v;
      s_prefs.putUChar("statbr", s_statBright);
    }
    if (r->hasParam("statusled")) {
      s_statOn = r->getParam("statusled")->value().toInt() != 0;
      s_prefs.putUChar("staten", s_statOn ? 1 : 0);
      Serial.printf("[net] pixel de statut : %s\n", s_statOn ? "allume (il peut couper la chaine)" : "eteint");
    }
    // /api/set?txpwr=13   puissance d emission en dBm (2..20). Baisser echange
    // de la portee contre des rafales de courant plus petites : c est le seul
    // levier logiciel sur un brownout, et il ne repare pas une alimentation.
    if (r->hasParam("txpwr")) {
      float d = r->getParam("txpwr")->value().toFloat();
      if (d < 2)  d = 2;
      if (d > 20) d = 20;
      s_txq = (uint8_t)lroundf(d * 4.0f);
      s_txDerated = false;              // choix explicite du proprietaire
      s_prefs.putUChar("txq", s_txq);
      applyTxPower();
    }
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

  // --- Remise a zero : /api/reset?what=look|network|homes|all -----------
  // Quatre niveaux, du plus anodin au plus definitif. Ils sont separes parce
  // qu'ils ne repondent pas a la meme question : "je n'aime pas ce reglage"
  // n'appelle pas le meme geste que "je donne ce mur a quelqu'un".
  s_server.on("/api/reset", HTTP_GET, [](AsyncWebServerRequest* r) {
    const String w = r->hasParam("what") ? r->getParam("what")->value() : String("");
    if (w == "look") {
      arenaled::resetLook();
      r->send(200, "application/json", "{\"ok\":true,\"did\":\"look\"}");
      return;
    }
    if (w == "network") {
      resetNetwork();
      r->send(200, "application/json", "{\"ok\":true,\"did\":\"network\",\"reboot\":true}");
      s_rebootAt = millis() + 400;      // laisser la reponse partir
      return;
    }
    if (w == "homes") {
      r->send(200, "application/json", "{\"ok\":true,\"did\":\"homes\",\"reboot\":true}");
      forgetHomes();
      return;
    }
    if (w == "all") {
      arenaled::resetAll();
      resetNetwork();
      arenapeers::resetAll();
      r->send(200, "application/json", "{\"ok\":true,\"did\":\"all\",\"reboot\":true}");
      forgetHomes();
      s_rebootAt = millis() + 800;
      return;
    }
    if (w == "reboot") {
      r->send(200, "application/json", "{\"ok\":true,\"did\":\"reboot\"}");
      s_rebootAt = millis() + 400;
      return;
    }
    r->send(400, "application/json",
            "{\"ok\":false,\"t\":\"what= look | network | homes | all | reboot\"}");
  });

  // --- Voisinage : /api/link?v=off|mirror|relay --------------------------
  // Comment ce mur se comporte quand il en voit d'autres. La detection, elle,
  // tourne toujours : savoir qui est la ne change rien a l'affichage.
  s_server.on("/api/link", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (r->hasParam("v"))
      arenapeers::setLink(arenapeers::linkFromName(r->getParam("v")->value().c_str()));
    // Alimentation partagee : chainage USB-C. Separe du mode de liaison, parce
    // qu'on peut vouloir des murs synchronises sur des chargeurs distincts, ou
    // des murs independants sur le meme chargeur.
    if (r->hasParam("shared"))
      arenapeers::setSharedPower(r->getParam("shared")->value() != "0");
    r->send(200, "application/json",
            String("{\"link\":\"") + arenapeers::linkName(arenapeers::link()) +
            "\",\"rank\":" + String((int)arenapeers::rank()) +
            ",\"peers\":" + arenapeers::json() + "}");
  });

  // --- Nom du mur : /api/name?v=Volcano ----------------------------------
  // C'est ce qui rend quatre murs utilisables. Sans nom, un balayage du reseau
  // ne rend que des adresses IP interchangeables, et l'app Maison affiche
  // quatre accessoires au libelle identique.
  s_server.on("/api/name", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (r->hasParam("v")) {
      String v = r->getParam("v")->value();
      v.trim();
      if (v.length() > 24) v = v.substring(0, 24);
      if (v.length()) { s_name = v; s_prefs.putString("name", v); }
      else            { s_prefs.remove("name"); }   // vide = retour au nom d'usine
      arenaoled::poke();
    }
    r->send(200, "application/json",
            String("{\"name\":\"") + s_name + "\",\"mac\":\"" + s_mac +
            "\",\"note\":\"nom mDNS et point d acces au prochain demarrage\"}");
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
  // Reveiller l'ecran a distance. Il s'eteint apres ARENA_OLED_SLEEP_MS, ce qui
  // est voulu - un menu statique se grave dans le verre d'un OLED - mais rend
  // toute verification impossible depuis l'autre bout de la maison : on flashe,
  // on va voir, il dort deja. ?s=1 le garde eveille en repoussant l'extinction
  // tant qu'on interroge.
  s_server.on("/api/oled", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!arenaoled::found()) { r->send(404, "text/plain", "aucun panneau detecte"); return; }
    if (r->hasParam("qr")) {
      arenaoled::showQr();
      r->send(200, "text/plain", "code d'appairage affiche");
      return;
    }
    arenaoled::poke();
    r->send(200, "text/plain", "ecran reveille");
  });

  // --- "Pourquoi ca ne fait pas ce que j'attends" ---------------------------
  // Une page en texte brut, lisible sur un telephone, qui repond aux trois
  // questions que le banc pose sans arret. Le port serie repond deja a tout
  // cela - mais il faut un cable, un moniteur et un reflash pour le lire,
  // alors que le navigateur est deja ouvert sur la page. Une preuve qu'on ne
  // peut pas obtenir est une preuve qui n'existe pas.
  s_server.on("/api/why", HTTP_GET, [](AsyncWebServerRequest* r) {
    String t = "=== POURQUOI ===\n\n";
    t += "-- La carte redemarre-t-elle ? --\n";
    t += "demarrages   : " + String(s_boots) + "\n";
    t += "dernier reset: " + s_reset + "\n";
    t += "en marche    : " + String(millis() / 1000) + " s\n";
    t += "  Note le compteur, provoque le probleme, recharge cette page.\n";
    t += "  Le compteur a monte  -> la carte a redemarre (la cause est ci-dessus).\n";
    t += "  Le compteur inchange -> elle n a PAS redemarre, c est autre chose.\n\n";

    // Un ecran qui s eteint et une carte qui redemarre se ressemblent trop pour
    // qu on devine. Quand le compteur ci-dessous n est pas a zero, la question
    // n est plus logicielle : le 3,3 V est descendu sous le seuil du detecteur,
    // et aucune correction de firmware ne remonte une tension.
    t += "-- L alimentation --\n";
    t += "brownouts    : " + String(s_brownouts) + "\n";
    t += "puissance TX : " + String(s_txq / 4.0f, 2) + " dBm";
    t += s_txDerated ? "  (baissee d office apres le brownout)\n" : "\n";
    if (s_brownouts) {
      t += "  La carte s est deja ETEINTE faute de tension, puis rerentree.\n";
      t += "  De l exterieur : ecran noir, reglages qui reviennent au depart.\n";
      t += "  Ce n est ni l ecran, ni les boutons, ni la veille.\n";
      t += "  Dans l ordre, du plus frequent au plus rare :\n";
      t += "   1. Le cable USB. Un cable de charge fin chute d un demi-volt sous\n";
      t += "      charge. Un cable court et epais, marque DATA, regle le cas le\n";
      t += "      plus courant a lui seul.\n";
      t += "   2. Le port. Un hub non alimente ou un port clavier ne tient pas\n";
      t += "      les pointes. Essayer un port arriere du PC, ou un chargeur\n";
      t += "      mural de 2 A et plus.\n";
      t += "   3. Les rafales WiFi. ~350 mA pendant quelques centaines de us.\n";
      t += "      Pour tester sans rien debrancher : /api/set?txpwr=8\n";
      t += "      Plus de brownouts a 8 dBm mais toujours a 20 -> c est la marge\n";
      t += "      d alimentation, pas le firmware.\n";
      t += "   4. Un condensateur de 470 a 1000 uF aux bornes du 5 V absorbe les\n";
      t += "      pointes que le cable ne sait pas fournir.\n";
      t += "  Si les LED sont cablees, les alimenter par leur propre 5 V, jamais\n";
      t += "  a travers l USB de la carte.\n\n";
    } else {
      t += "  Aucun effondrement d alimentation enregistre.\n";
      t += "  Un ecran noir ou des reglages perdus viennent d ailleurs.\n\n";
    }

    // Une OTA qui echoue a moitie laisse une carte muette, et le proprietaire
    // n a alors plus de page pour lire pourquoi. Donc on le dit AVANT.
    // Un defaut materiel CONNU, ecrit dans le BOM depuis la conception. Sans
    // cette section, chaque proprietaire le redecouvre a ses frais - et le
    // cherche dans le firmware, ou il n'est pas.
    t += "-- Le temoin de statut (D2) --\n";
    t += "allume       : " + String(s_statOn ? "oui" : "non") +
         "   luminosite " + String(s_statBright) + "/255\n";
    t += "  D2 est un WS2812B-2020. La carte l alimente en 3,3 V alors que sa\n";
    t += "  fiche demande 3,5 a 5,3 V : il tourne SOUS son minimum. C est ecrit\n";
    t += "  dans hardware/BOM_PCB.csv depuis la conception, avec la raison - le\n";
    t += "  couloir vers la zone +5 V etait sature au routage, et rouvrir une\n";
    t += "  region qui marche pour un temoin ne valait pas le risque.\n";
    t += "  Un exemplaire hors spec ne s eteint pas : il lit mal sa donnee et\n";
    t += "  sort du BLANC PLEIN, insensible a la couleur ET a la luminosite\n";
    t += "  qu on lui envoie. Aucun reglage ici n y changera quoi que ce soit.\n";
    t += "  Le rattrapage est prevu, dix minutes au fer : un fil de quelques mm\n";
    t += "  entre +5 V et la broche 4 de D2, avec une 1N4148 en l air (cathode\n";
    t += "  vers D2). VDD passe a ~4,3 V, dans la plage.\n";
    t += "  En attendant : /api/set?statusled=0 l eteint plutot que de laisser\n";
    t += "  un temoin qui ment.\n\n";

    t += "-- La flash --\n";
    t += "puce         : " + String(s_flashPhys >> 20) + " Mo (identifiant JEDEC)\n";
    t += "table        : jusqu a " + String(s_partEnd >> 20) + " Mo\n";
    if (s_flashBad) {
      t += "  ATTENTION : la table depasse la puce. Les partitions du haut -\n";
      t += "  OTA et fichiers - pointent dans le vide. Une OTA s ecrirait\n";
      t += "  par-dessus elle-meme. Corriger board_build.partitions dans\n";
      t += "  platformio.ini et reflasher par cable AVANT toute OTA.\n\n";
    } else if (!s_flashPhys) {
      t += "  Taille physique illisible : verifier a la main avant une OTA.\n\n";
    } else {
      t += "  La table tient dans la puce.\n\n";
    }

    t += "-- Les LED --\n";
    t += "broche data  : GPIO" + String(arenaled::pin()) +
         "   (essayer une autre sans reflasher : /api/set?pin=N)\n";
    t += "pixels pilotes: " + String(arenaled::count());
    if (arenaled::repeater()) {
      t += " visibles + 1 repeteur = " + String(arenaled::count() + 1) + " a cabler\n";
      t += "  ATTENTION : le 1er pixel physique est tenu ETEINT expres (repeteur).\n";
      t += "  Avec une seule LED cablee, elle ne s allumera JAMAIS. Il en faut 2,\n";
      t += "  Pas de repeteur sur ta chaine ? -> /api/set?repeater=0\n";
    } else {
      t += " (pas de repeteur)\n";
    }
    t += "mode         : " + String(arenaled::modeLabel(arenaled::mode())) + "\n";
    t += "luminosite   : " + String(arenaled::brightness()) + "/255\n";
    t += "courant      : " + String(arenaled::lastAmps(), 2) + " A\n";
    t += "ordre couleur: " + String(arenaled::order()) + "\n\n";

    t += "-- Temoin de la carte --\n";
    t += "pixel statut : GPIO" + String(s_statPin) +
         "   (autre broche : /api/set?statuspin=N)\n";
    t += "  ambre=demarrage  bleu=point d acces  vert=reseau  rouge=defaut\n";
    t += "  Toujours eteint ? La broche n est pas la bonne : essaie 48, 38, 8, 2.\n\n";

    t += "-- Boutons (au repos, les trois doivent lire 'haut') --\n";
    {
      bool bu = false, bo = false, bd = false;
      uint32_t nu = 0, no = 0, nd = 0;
      arenaoled::btnRaw(bu, bo, bd, nu, no, nd);
      uint8_t pu, pd, po;
      arenaoled::buttons(pu, pd, po);
      t += "gauche GPIO" + String(pu) + " : " + (bu ? "BAS (enfonce !)" : "haut") + "   declenche " + String(nu) + "x\n";
      t += "OK     GPIO" + String(po) + " : " + (bo ? "BAS (enfonce !)" : "haut") + "   declenche " + String(no) + "x\n";
      t += "droite GPIO" + String(pd) + " : " + (bd ? "BAS (enfonce !)" : "haut") + "   declenche " + String(nd) + "x\n";
      t += "  Un poussoir qui repond mais dans le mauvais sens n'est pas un bug :\n";
      t += "  la netlist nomme des nets, pas des positions. Remapper sans\n";
      t += "  reflasher : /api/set?btnup=<gauche>&btndown=<droite>&btnok=<ok>\n";
      t += "  Sans lire un seul numero de broche : /api/set?btnrot=1 decale les\n";
      t += "  trois roles d'un cran. Au pire deux fois, et c'est dans le bon sens.\n";
      if (bu || bo || bd)
        t += "  !! Une entree est BASSE sans que tu touches rien. Cette broche n est\n"
             "     pas cablee a ce poussoir, ou il est colle. Un OK bloque bas part en\n"
             "     appui long : ecran noir, reveil, rebelote - exactement le symptome.\n";
      t += "\n";
    }

    t += "-- Reseau --\n";
    t += "mode         : " + s_mode + "  ip " + s_ip + "\n";
    t += "dernier echec: " + (s_staReason.length() ? s_staReason : String("(aucun essai)")) + "\n";
    t += "erreur scan  : " + (s_scanErr.length() ? s_scanErr : String("(aucune)")) + "\n";
    r->send(200, "text/plain; charset=utf-8", t);
  });

  s_server.on("/api/wifiscan", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (r->hasParam("results")) {
      wifi_ap_record_t apInfo = {};
      const bool apInfoOk = (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK);
      String j = "{\"fail\":\"" + s_staReason + "\"" +
                 ",\"scanErr\":\"" + s_scanErr + "\"" +
                 ",\"age\":" + String(s_scanAt ? (millis() - s_scanAt) / 1000 : 9999) +
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

// Codes de deconnexion de l'IDF, traduits. Les trois premiers couvrent
// l'immense majorite des cas sur une installation domestique.
static const char* staReasonName(uint8_t r) {
  switch (r) {
    case WIFI_REASON_NO_AP_FOUND:        return "reseau introuvable (SSID exact ? 2,4 GHz ?)";
    case WIFI_REASON_AUTH_FAIL:          return "authentification refusee (mot de passe)";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:  return "handshake expire (mot de passe, ou signal trop faible)";
    case WIFI_REASON_ASSOC_FAIL:         return "association refusee par le routeur";
    case WIFI_REASON_AUTH_EXPIRE:        return "authentification expiree";
    case WIFI_REASON_BEACON_TIMEOUT:     return "balise perdue (hors de portee)";
    case WIFI_REASON_CONNECTION_FAIL:    return "connexion refusee";
    default:                             return "echec";
  }
}

void begin() {
  // --- WiFi: NVS credentials (set from the UI) override the compile-time ones ---
  s_prefs.begin("arenanet", false);
  s_statPin = s_prefs.getUChar("statpin", ARENA_STATUS_PIN);
  s_statOn  = s_prefs.getUChar("staten", 1) != 0;
  s_statBright = s_prefs.getUChar("statbr", 60);
  s_reset = resetReasonName(esp_reset_reason());
  s_boots = s_prefs.getUInt("boots", 0) + 1;
  s_prefs.putUInt("boots", s_boots);
  s_brownouts = s_prefs.getUInt("brown", 0);
  s_txq = s_prefs.getUChar("txq", ARENA_WIFI_TXPWR_QDBM);
  if (esp_reset_reason() == ESP_RST_BROWNOUT) {
    s_brownouts++;
    s_prefs.putUInt("brown", s_brownouts);
    // Revenir a pleine puissance dans une alimentation qui vient de ceder, c est
    // rejouer la meme scene. On baisse pour CETTE session seulement : un blip
    // isole ne doit pas amputer la portee du mur pour toujours, et un reflash
    // ou une coupure propre rend la valeur enregistree.
    if (s_txq > ARENA_WIFI_TXPWR_SAFE_QDBM) {
      s_txq = ARENA_WIFI_TXPWR_SAFE_QDBM;
      s_txDerated = true;
    }
  }
  Serial.printf("[dev] demarrage #%lu - cause du dernier reset : %s\n",
                (unsigned long)s_boots, s_reset.c_str());
  if (s_brownouts) {
    Serial.printf("[dev] %lu brownout(s) au total : le 3,3 V s effondre. Ce n est\n"
                  "      PAS un bug logiciel - cable USB, port ou regulateur.\n",
                  (unsigned long)s_brownouts);
  }
  s_flashPhys = physicalFlashBytes();
  s_partEnd   = partitionEndBytes();
  s_flashBad  = (s_flashPhys && s_partEnd > s_flashPhys);
  Serial.printf("[dev] flash %lu Mo (puce) / table de partitions jusqu a %lu Mo\n",
                (unsigned long)(s_flashPhys >> 20), (unsigned long)(s_partEnd >> 20));
  if (s_flashBad) {
    Serial.printf("[dev] ATTENTION : la table depasse la flash de %lu Ko. Les\n"
                  "      partitions du haut - OTA et fichiers - pointent dans le\n"
                  "      vide. Corriger board_build.partitions dans platformio.ini\n"
                  "      AVANT toute OTA.\n",
                  (unsigned long)((s_partEnd - s_flashPhys) >> 10));
  }
  {
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    char sfx[16]; snprintf(sfx, sizeof(sfx), "%02X%02X", m[4], m[5]);
    char mac[20]; snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                           m[0], m[1], m[2], m[3], m[4], m[5]);
    s_mac  = mac;
    s_name = s_prefs.getString("name", "");
    if (!s_name.length()) s_name = String("Playfield-") + sfx;
    Serial.printf("[net] mur '%s' (%s)\n", s_name.c_str(), s_mac.c_str());
  }
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
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    const uint8_t r = info.wifi_sta_disconnected.reason;
    s_staReason = String(staReasonName(r)) + " [" + String((int)r) + "]";
    Serial.printf("[net] STA echec : %s\n", s_staReason.c_str());
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  bool connected = false;
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostify(s_name).c_str());
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
    if (connected) s_staReason = "";
    else if (!s_staReason.length()) s_staReason = "delai depasse (aucune reponse)";
  }
  if (connected) {
    s_mode = "STA";
    s_ip = WiFi.localIP().toString();
    Serial.printf("[net] STA OK ip=%s\n", s_ip.c_str());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(s_name.c_str(), ARENA_AP_PASS);
    s_mode = "SoftAP";
    s_ip = WiFi.softAPIP().toString();
    Serial.printf("[net] SoftAP '%s' ip=%s\n", s_name.c_str(), s_ip.c_str());
  }
  applyTxPower();
  // Nom d'hote propre a la carte : sans ca, quatre murs se disputent arena.local
  // et mDNS en renomme trois en arena-2, arena-3... au petit bonheur.
  {
    String h = hostify(s_name);
    if (MDNS.begin(h.c_str())) {
      MDNS.addService("http", "tcp", 80);
      MDNS.addServiceTxt("http", "tcp", "wall", s_name.c_str());
      Serial.printf("[net] http://%s.local/\n", h.c_str());
    }
  }

#endif

  startServer();
  Serial.println("[net] http server up");
}

// Pixel de temoin : une pulsation lente dont la COULEUR porte l'etat. Le but
// n'est pas la decoration, c'est de repondre a "est-ce que la carte tourne ?"
// sans cable, sans page web et sans ruban - la question qu'on se pose en
// premier et a laquelle rien ne repondait.
//   ambre  = en cours de demarrage       bleu = point d'acces, personne connecte
//   vert   = associe a un reseau         rouge = defaut signale par le limiteur
static void statusTick() {
#if ARENA_STATUS_LED_ENABLE
  if (!s_statOn) return;
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 200) return;                // 5 Hz : on ne fait que comparer
  last = now;

  uint8_t r = 255, g = 128, b = 0;             // ambre par defaut, pleine echelle
  if (arenaled::ledFault())      { r = 255; g = 0;   b = 0;   }
  else if (s_mode == "STA")      { r = 0;   g = 255; b = 0;   }
  else if (s_mode == "SoftAP")   { r = 0;   g = 70;  b = 255; }

  // La couleur porte l'etat, la luminosite est un reglage : les teintes restent
  // a pleine echelle et l'echelle s'applique A LA FIN, sinon baisser le temoin
  // le ferait deraper vers son canal le plus fort au lieu de l'assombrir.
  const float k = s_statBright / 255.0f;
  const uint8_t rr = (uint8_t)(r * k), gg = (uint8_t)(g * k), bb = (uint8_t)(b * k);

  // Ecrire seulement quand ca change : quelques trames par minute au lieu de
  // vingt-cinq par seconde, et le partage du RMT redevient sans consequence.
  static uint8_t  lr = 1, lg = 1, lb = 1;
  static uint32_t lastPush = 0;
  if (rr == lr && gg == lg && bb == lb && now - lastPush < 5000) return;
  lr = rr; lg = gg; lb = bb; lastPush = now;
  neopixelWrite(s_statPin, rr, gg, bb);
#endif
}

void loop() {
  statusTick();
  validateImageWhenHealthy();

  // Redemarrage differe demande par /api/reset : la reponse a eu le temps de
  // partir, l'appelant sait donc que l'ordre a ete accepte.
  if (s_rebootAt && (int32_t)(millis() - s_rebootAt) >= 0) {
    Serial.println("[net] redemarrage demande");
    delay(50);
    ESP.restart();
  }

  // Le balayage tourne ICI : il bloque plusieurs secondes et n'a rien a faire
  // dans un handler HTTP.
  if (s_scanWanted) {
    s_scanWanted = false;
    // API IDF, pas l'objet Arduino WiFi : sous Matter c'est CHIP qui possede
    // esp_wifi et arenanet::begin() sort avant tout WiFi.mode(), donc le
    // wrapper Arduino n'a aucun etat - il rend 0 reseau, RSSI 0, SSID vide.
    // Meme lecon que netHasIp(), qui lit deja au niveau esp_netif.
    // Un balayage exige que l'interface STA EXISTE. Une carte non provisionnee
    // tourne en WIFI_AP seul - exactement l'instant ou l'on a besoin de la liste
    // des reseaux - et esp_wifi_scan_start() y rend ESP_ERR_WIFI_MODE. Le code
    // ne testait que ESP_OK, donc l'echec sortait une liste VIDE, sans un mot :
    // "le scan ne marche pas". On passe en AP+STA, ce qui ajoute la station
    // SANS couper le point d'acces - couper l'AP deconnecterait le telephone
    // qui est en train de regarder la page.
    wifi_mode_t wm = WIFI_MODE_NULL;
    esp_wifi_get_mode(&wm);
    if (wm == WIFI_MODE_AP) {
#ifdef ARENA_MATTER
      // Sous Matter, CHIP possede esp_wifi et le wrapper Arduino n'a aucun etat :
      // il faut passer par l'IDF.
      if (esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK) esp_wifi_start();
#else
      // Hors Matter, c'est WiFi.softAP() qui a monte la radio, donc c'est le
      // wrapper Arduino qui tient les interfaces. Changer le mode par l'IDF
      // seul ne cree PAS le netif station et le wrapper reimpose son propre
      // mode a l'evenement suivant - le balayage repart alors sans station.
      // Passer par WiFi.mode() fait les deux et ne coupe pas le point d'acces.
      WiFi.mode(WIFI_AP_STA);
#endif
      delay(100);                       // laisser l'interface station demarrer
      esp_wifi_get_mode(&wm);
      Serial.printf("[net] balayage : AP -> mode %d (interface station requise)\n", (int)wm);
    }
    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    String j = "[";
    const esp_err_t scanErr = esp_wifi_scan_start(&cfg, true);
    s_scanErr = "";
    if (scanErr != ESP_OK) {
      s_scanErr = String(esp_err_to_name(scanErr)) + " (mode " + String((int)wm) + ")";
      Serial.printf("[net] balayage refuse : %s\n", s_scanErr.c_str());
    }
    if (scanErr == ESP_OK) {
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
    if (scanErr == ESP_OK) {
      uint16_t seen = 0;
      esp_wifi_scan_get_ap_num(&seen);
      // "Nothing found" sur la page et rien du tout au journal, c'est une panne
      // sans temoin : impossible de distinguer un balayage refuse d'un balayage
      // qui a bien tourne dans un endroit vide.
      Serial.printf("[net] balayage : %u reseau(x) vus\n", (unsigned)seen);
    }
    if (scanErr != ESP_OK) s_scanJson = "[]";
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
