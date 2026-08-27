#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <string.h>
#include "arena_map.h"

namespace arenamap {

// --- Default template: 100 LEDs, chain order = bottom-left -> up -> back panel ---
// Sums to exactly 100 pixels. Zones beyond the configured LED count are simply
// skipped by the effects, so this table is also safe on a 50-LED first build.
static const Zone DEFAULT_ZONES[] = {
  { "slings-outlanes",  0,  6 },
  { "lower-inserts",    6,  8 },
  { "drop-targets",    14,  8 },
  { "left-orbit",      22,  8 },
  { "pop-bumpers",     30,  9 },
  { "spinner",         39,  4 },
  { "right-orbit",     43,  8 },
  { "arena-letters",   51,  5 },
  { "top-lanes",       56,  6 },
  { "bonus-ladder",    62, 10 },
  { "center-star",     72,  6 },
  { "ramp",            78,  8 },
  { "apron",           86,  8 },
  { "wash",            94,  6 },
};
static const uint8_t DEFAULT_N = sizeof(DEFAULT_ZONES) / sizeof(DEFAULT_ZONES[0]);

static Zone    s_zones[ZONE_MAX];
static uint8_t s_n = 0;

// L'appartenance, un octet par pixel. C'est ELLE qui definit les groupes ;
// s_zones[].first/count en sont recalcules et ne servent qu'a l'affichage.
static uint8_t s_ledZone[LED_MAX];

// --- Persistance -----------------------------------------------------------
//
// Les groupes vivaient UNIQUEMENT dans /arena_map.json, sur LittleFS. Or un OTA
// du systeme de fichiers reecrit la partition entiere : chaque mise a jour de
// l'interface effacait donc les groupes crees par le proprietaire, en silence.
// Constate deux fois de suite sur la vraie carte le 2026-08-27 - le groupe
// champignons a disparu aux deux flashs.
//
// Ce n'etait pas une question de fichier livre dans data/ : le retirer n'a rien
// change, puisque c'est la partition qui est effacee, pas le fichier qui est
// ecrase. Le mapping LED, lui, survivait - parce qu'il est en NVS
// (arena_pf.cpp, putBytes) et que la NVS est une AUTRE partition, que l'OTA fs
// ne touche pas. On aligne les groupes sur le meme choix.
//
// LittleFS reste lu au demarrage, en migration : une carte qui tourne
// aujourd'hui a ses groupes la-bas et doit les retrouver une derniere fois.
static Preferences s_prefs;
// D'ou viennent les zones au demarrage. Publie dans /api/state : sans ca, un
// groupe disparu ne dit pas s'il a ete efface ou simplement jamais relu.
static const char* s_src = "?";
static const char* NVS_NS   = "arenamap";
static const char* NVS_ZONE = "zones";     // le tableau s_zones
static const char* NVS_LEDZ = "ledzone";   // l appartenance de chaque pixel

static void recount() {
  for (uint8_t z = 0; z < s_n; z++) { s_zones[z].first = 0; s_zones[z].count = 0; }
  for (uint16_t i = 0; i < LED_MAX; i++) {
    const uint8_t z = s_ledZone[i];
    if (z >= s_n) continue;
    if (s_zones[z].count == 0) s_zones[z].first = i;
    s_zones[z].count++;
  }
}

static void clearMembers() { memset(s_ledZone, ZONE_NONE, sizeof(s_ledZone)); }

// Remplir une plage : la facon commode de decrire un groupe qui SE TROUVE etre
// contigu, sans que le groupe soit defini par cette contiguite.
static void fillRange(uint8_t z, uint16_t first, uint16_t cnt) {
  for (uint16_t i = 0; i < LED_MAX; i++) if (s_ledZone[i] == z) s_ledZone[i] = ZONE_NONE;
  for (uint16_t i = first; i < (uint32_t)first + cnt && i < LED_MAX; i++) s_ledZone[i] = z;
}

static void copyName(char* dst, const char* src) {
  strncpy(dst, src ? src : "", ZONE_NAME_LEN - 1);
  dst[ZONE_NAME_LEN - 1] = '\0';
}

void reset() {
  s_n = DEFAULT_N;
  memcpy(s_zones, DEFAULT_ZONES, sizeof(DEFAULT_ZONES));
  clearMembers();
  for (uint8_t z = 0; z < s_n; z++) fillRange(z, DEFAULT_ZONES[z].first, DEFAULT_ZONES[z].count);
  recount();
}

uint8_t count() { return s_n; }

const Zone* zone(uint8_t i) { return (i < s_n) ? &s_zones[i] : nullptr; }

int indexOf(const char* name) {
  if (!name) return -1;
  for (uint8_t i = 0; i < s_n; i++)
    if (strncmp(s_zones[i].name, name, ZONE_NAME_LEN) == 0) return i;
  return -1;
}

int zoneOfLed(uint16_t led) {
  if (led >= LED_MAX) return -1;
  const uint8_t z = s_ledZone[led];
  return (z < s_n) ? (int)z : -1;
}

bool inZone(uint8_t z, uint16_t led) {
  return led < LED_MAX && z < s_n && s_ledZone[led] == z;
}

int zoneNth(uint8_t z, uint16_t n) {
  if (z >= s_n) return -1;
  for (uint16_t i = 0; i < LED_MAX; i++)
    if (s_ledZone[i] == z && n-- == 0) return (int)i;
  return -1;
}

bool setLedZone(uint16_t led, uint8_t z) {
  if (led >= LED_MAX) return false;
  if (z != ZONE_NONE && z >= s_n) return false;
  s_ledZone[led] = z;
  recount();
  return true;
}

bool renameZone(uint8_t i, const char* name) {
  if (i >= s_n) return false;
  copyName(s_zones[i].name, name);
  return true;
}

bool setZone(uint8_t i, const char* name, uint16_t first, uint16_t cnt) {
  if (i >= s_n || first >= LED_MAX) return false;
  if (first + cnt > LED_MAX) cnt = LED_MAX - first;
  copyName(s_zones[i].name, name);
  fillRange(i, first, cnt);
  recount();
  return true;
}

bool addZone(const char* name, uint16_t first, uint16_t cnt) {
  if (s_n >= ZONE_MAX || first >= LED_MAX) return false;
  if (first + cnt > LED_MAX) cnt = LED_MAX - first;
  copyName(s_zones[s_n].name, name);
  s_zones[s_n].first = 0;
  s_zones[s_n].count = 0;
  s_n++;
  fillRange(s_n - 1, first, cnt);
  recount();
  return true;
}

bool removeZone(uint8_t i) {
  if (i >= s_n) return false;
  // Les membres suivent le decalage du tableau, sinon un retrait renommerait
  // silencieusement l'appartenance de tous les groupes situes apres.
  for (uint16_t k = 0; k < LED_MAX; k++) {
    if (s_ledZone[k] == i)      s_ledZone[k] = ZONE_NONE;
    else if (s_ledZone[k] != ZONE_NONE && s_ledZone[k] > i) s_ledZone[k]--;
  }
  for (uint8_t k = i; k + 1 < s_n; k++) s_zones[k] = s_zones[k + 1];
  s_n--;
  recount();
  return true;
}

String toJson() {
  String j = "{\"zones\":[";
  for (uint8_t i = 0; i < s_n; i++) {
    if (i) j += ',';
    j += "{\"n\":\"";
    j += s_zones[i].name;
    // La liste des membres, toujours. "f"/"c" ne decrivaient une plage que tant
    // qu'un groupe ETAIT une plage ; les ecrire encore ferait relire un groupe
    // disperse comme la tranche qui va de son premier a son dernier membre,
    // c'est-a-dire en avalant tous les pixels d'autres groupes au passage.
    j += "\",\"m\":[";
    bool first = true;
    for (uint16_t k = 0; k < LED_MAX; k++) {
      if (s_ledZone[k] != i) continue;
      if (!first) j += ',';
      j += String(k);
      first = false;
    }
    j += "]}";
  }
  j += "]}";
  return j;
}

bool fromJson(const char* json) {
  if (!json) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  JsonArrayConst arr = doc["zones"].as<JsonArrayConst>();
  if (arr.isNull()) return false;

  // On construit a cote : une entree malformee ne doit pas abimer une carte
  // valide. Les deux formes sont acceptees - "m" (les membres, ce qu'on ecrit
  // aujourd'hui) et "f"/"c" (une plage, ce que porte le fichier livre et tout
  // ce qui a ete enregistre avant). Refuser l'ancienne forme reviendrait a
  // effacer la cartographie de chaque mur deja pose.
  Zone    tmpZ[ZONE_MAX];
  uint8_t tmpL[LED_MAX];
  memset(tmpL, ZONE_NONE, sizeof(tmpL));
  uint8_t n = 0;
  for (JsonObjectConst z : arr) {
    if (n >= ZONE_MAX) break;
    copyName(tmpZ[n].name, z["n"] | "");
    tmpZ[n].first = 0;
    tmpZ[n].count = 0;
    JsonArrayConst m = z["m"].as<JsonArrayConst>();
    if (!m.isNull()) {
      for (JsonVariantConst v : m) {
        const uint32_t led = v.as<uint32_t>();
        if (led < LED_MAX) tmpL[led] = n;
      }
    } else {
      uint32_t f = z["f"] | 0;
      uint32_t c = z["c"] | 0;
      if (f >= LED_MAX) continue;
      if (f + c > LED_MAX) c = LED_MAX - f;
      for (uint32_t k = f; k < f + c; k++) tmpL[k] = n;
    }
    n++;
  }
  if (n == 0) return false;
  memcpy(s_zones, tmpZ, sizeof(Zone) * n);
  memcpy(s_ledZone, tmpL, sizeof(s_ledZone));
  s_n = n;
  recount();
  return true;
}

bool load() {
  if (!LittleFS.exists(ARENA_MAP_PATH)) return false;
  File f = LittleFS.open(ARENA_MAP_PATH, "r");
  if (!f) return false;
  String s = f.readString();
  f.close();
  return fromJson(s.c_str());
}

// La NVS est la reference. Le fichier reste ecrit pour rester lisible depuis un
// navigateur ou un outil, mais plus rien n'en depend.
bool save() {
  const bool okZ = s_prefs.putBytes(NVS_ZONE, s_zones, sizeof(s_zones)) == sizeof(s_zones);
  const bool okL = s_prefs.putBytes(NVS_LEDZ, s_ledZone, sizeof(s_ledZone)) == sizeof(s_ledZone);
  s_prefs.putUChar("n", s_n);

  File f = LittleFS.open(ARENA_MAP_PATH, "w");
  if (f) { String j = toJson(); f.print(j); f.close(); }
  return okZ && okL;
}

static bool loadNvs() {
  if (s_prefs.getBytesLength(NVS_ZONE) != sizeof(s_zones)) return false;
  if (s_prefs.getBytesLength(NVS_LEDZ) != sizeof(s_ledZone)) return false;
  s_prefs.getBytes(NVS_ZONE, s_zones, sizeof(s_zones));
  s_prefs.getBytes(NVS_LEDZ, s_ledZone, sizeof(s_ledZone));
  s_n = s_prefs.getUChar("n", 0);
  if (s_n > ZONE_MAX) s_n = ZONE_MAX;
  // Un nom non termine rendrait toute la table illisible : on borne plutot que
  // de faire confiance a ce qui sort de la flash.
  for (uint8_t z = 0; z < ZONE_MAX; z++) s_zones[z].name[ZONE_NAME_LEN - 1] = 0;
  for (uint16_t i = 0; i < LED_MAX; i++)
    if (s_ledZone[i] != ZONE_NONE && s_ledZone[i] >= s_n) s_ledZone[i] = ZONE_NONE;
  recount();
  return s_n > 0;
}

const char* source() { return s_src; }

void begin() {
  reset();
  s_prefs.begin(NVS_NS, false);
  if (loadNvs()) {
    s_src = "nvs";
    Serial.printf("[map] %u zones depuis la NVS\n", s_n);
  } else if (load()) {
    // Migration : la carte avait ses groupes dans LittleFS. On les remonte en
    // NVS tout de suite, pour que le prochain flash fs ne les emporte pas.
    s_src = "file";
    Serial.printf("[map] migration de %s vers la NVS (%u zones)\n", ARENA_MAP_PATH, s_n);
    save();
  } else {
    s_src = "default";
    Serial.printf("[map] modele par defaut (%u zones)\n", s_n);
  }
}

}  // namespace arenamap
