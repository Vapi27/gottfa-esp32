// arena_pf.cpp — see arena_pf.h.
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <string.h>
#include "arena_pf.h"

namespace arenapf {

static const char* PF_PATH   = "/arena_pf.json";     // the playfield (read-only)
static const char* LEDS_PATH = "/arena_leds.json";   // this wall's assignment

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
  File f = LittleFS.open(LEDS_PATH, "w");
  if (!f) return false;
  String j = toJson();
  size_t w = f.print(j);
  f.close();
  return w == j.length();
}

static bool loadAssignment() {
  if (!LittleFS.exists(LEDS_PATH)) return false;
  File f = LittleFS.open(LEDS_PATH, "r");
  if (!f) return false;
  String s = f.readString();
  f.close();
  return fromJson(s.c_str());
}

void begin() {
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
