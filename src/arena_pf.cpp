// arena_pf.cpp — see arena_pf.h.
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>
#include <ctype.h>
#include "arena_pf.h"

namespace arenapf {

static const char* PF_PATH   = "/arena_pf.json";     // the playfield (read-only)
// This wall's assignment lives in NVS, NOT on LittleFS. It used to be a file
// there, and pushing a new web UI wiped it: `/update?target=fs` rewrites the
// whole filesystem partition, so shipped data and the owner's own work shared a
// blast radius. NVS is a separate partition that neither OTA touches. The old
// file is still read once, so an install that predates this keeps its map.
static const char* LEDS_PATH = "/arena_leds.json";   // legacy, read-only migration
static Preferences s_prefs;
static const char* NVS_NS  = "arenapf";
static const char* NVS_KEY = "ledmap";

static Insert  s_ins[INSERT_MAX];
static uint8_t s_nIns = 0;
static uint8_t s_led[LED_MAX];                       // chain index -> insert index
static bool    s_any = false;

uint8_t       insertCount()       { return s_nIns; }
const Insert* insert(uint8_t i)   { return (i < s_nIns) ? &s_ins[i] : nullptr; }
uint8_t       ledInsert(uint16_t led) { return (led < LED_MAX) ? s_led[led] : UNASSIGNED; }
bool          anyAssigned()       { return s_any; }

int indexOf(const char* name) {
  for (uint8_t i = 0; i < s_nIns; i++)
    if (strncmp(s_ins[i].name, name, NAME_LEN) == 0) return i;
  return -1;
}

static void recountAssigned() {
  s_any = false;
  for (uint16_t i = 0; i < LED_MAX && !s_any; i++) s_any = (s_led[i] != UNASSIGNED);
}

bool setLedInsert(uint16_t led, uint8_t ins) {
  if (led >= LED_MAX) return false;
  if (ins != UNASSIGNED && ins >= s_nIns) return false;
  s_led[led] = ins;
  if (ins != UNASSIGNED) s_any = true; else recountAssigned();
  return true;
}

void clearAssignment() {
  memset(s_led, UNASSIGNED, sizeof(s_led));
  s_any = false;
}

int lampOfLed(uint16_t led) {
  const uint8_t a = ledInsert(led);
  return (a == UNASSIGNED || a >= s_nIns) ? -1 : s_ins[a].lamp;
}

bool xy(uint16_t led, float& x, float& y) {
  const uint8_t a = ledInsert(led);
  if (a == UNASSIGNED || a >= s_nIns) return false;
  x = s_ins[a].x;
  y = s_ins[a].y;
  return true;
}

// --- the fixed playfield table -------------------------------------------
// Streamed back to the browser verbatim rather than re-serialised: it is
// static data, and re-encoding 99 records would cost heap for nothing.
String insertsJson() {
  if (!LittleFS.exists(PF_PATH)) return String("{\"inserts\":[]}");
  File f = LittleFS.open(PF_PATH, "r");
  if (!f) return String("{\"inserts\":[]}");
  String s = f.readString();
  f.close();
  return s;
}

static bool loadInserts() {
  s_nIns = 0;
  if (!LittleFS.exists(PF_PATH)) return false;
  File f = LittleFS.open(PF_PATH, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  JsonArray arr = doc["inserts"].as<JsonArray>();
  if (arr.isNull()) return false;
  for (JsonObject o : arr) {
    if (s_nIns >= INSERT_MAX) break;
    Insert& it = s_ins[s_nIns];
    strncpy(it.name, o["n"] | "", NAME_LEN - 1);
    it.name[NAME_LEN - 1] = 0;
    const char* k = o["k"] | "i";
    it.kind = k[0];
    // Two numbers, and confusing them cost an evening. "l" is PinMAME's internal
    // lamp index, which is what the captured ROM sequence is indexed by. The
    // NAME carries what the machine calls that lamp (PinMAME + 8, per
    // GTS80_lamp2m), because that is what is written in the manual and on the
    // playfield — so it is what the plan must show and what the firmware must
    // never compute with.
    const int l = o["l"] | -1;
    it.lamp = (l >= 0 && l < 64) ? (int8_t)l : -1;
    it.x = o["x"] | 0.0f;
    it.y = o["y"] | 0.0f;
    s_nIns++;
  }
  return s_nIns > 0;
}

// --- this wall's assignment ----------------------------------------------
// Only assigned pixels are written: a fresh wall is a two-byte file, and the
// common early state (six pixels placed out of 150) does not carry 144 nulls.
String toJson() {
  String j = "{\"leds\":[";
  bool first = true;
  for (uint16_t i = 0; i < LED_MAX; i++) {
    if (s_led[i] == UNASSIGNED) continue;
    if (!first) j += ',';
    first = false;
    j += "{\"i\":" + String(i) + ",\"a\":" + String(s_led[i]) + "}";
  }
  j += "]}";
  return j;
}

bool fromJson(const char* json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  JsonArray arr = doc["leds"].as<JsonArray>();
  if (arr.isNull()) return false;
  // Build into a scratch copy: a half-applied assignment after a bad record
  // would leave the wall mapped to something nobody asked for.
  uint8_t tmp[LED_MAX];
  memset(tmp, UNASSIGNED, sizeof(tmp));
  for (JsonObject o : arr) {
    const int i = o["i"] | -1;
    const int a = o["a"] | -1;
    if (i < 0 || i >= LED_MAX) return false;
    if (a < 0 || (a >= s_nIns && a != UNASSIGNED)) return false;
    tmp[i] = (uint8_t)a;
  }
  memcpy(s_led, tmp, sizeof(s_led));
  recountAssigned();
  return true;
}

bool save() {
  return s_prefs.putBytes(NVS_KEY, s_led, sizeof(s_led)) == sizeof(s_led);
}

static bool loadAssignment() {
  if (s_prefs.getBytesLength(NVS_KEY) == sizeof(s_led)) {
    s_prefs.getBytes(NVS_KEY, s_led, sizeof(s_led));
    recountAssigned();
    return true;
  }
  // Migration: an install from before the move still has its map on LittleFS.
  // Read it once and write it where it belongs.
  if (!LittleFS.exists(LEDS_PATH)) return false;
  File f = LittleFS.open(LEDS_PATH, "r");
  if (!f) return false;
  String j = f.readString();
  f.close();
  if (!fromJson(j.c_str())) return false;
  save();
  Serial.println("[pf] migrated the insert map from LittleFS to NVS");
  return true;
}

void begin() {
  s_prefs.begin(NVS_NS, false);
  clearAssignment();
  const bool haveTable = loadInserts();
  const bool haveMap   = haveTable && loadAssignment();
  uint16_t n = 0;
  for (uint16_t i = 0; i < LED_MAX; i++) if (s_led[i] != UNASSIGNED) n++;
  Serial.printf("[pf] %u inserts%s, %u pixels placed\n",
                s_nIns, haveTable ? "" : " (MISSING /arena_pf.json)", n);
  (void)haveMap;
}

}  // namespace arenapf
