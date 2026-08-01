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
static const char* NVS_COL = "inscol";
static const char* NVS_NAM = "insnam";

static Insert  s_ins[INSERT_MAX];
static uint8_t s_nIns = 0;
static uint8_t s_led[LED_MAX];                       // chain index -> insert index
static bool    s_any = false;
static Colour  s_col[INSERT_MAX];      // per-insert plastic colour, all-zero = unset
static char    s_nam[INSERT_MAX][NAME_LEN];  // owner's label, empty = use the shipped one

uint8_t       insertCount()       { return s_nIns; }
const Insert* insert(uint8_t i)   { return (i < s_nIns) ? &s_ins[i] : nullptr; }
uint8_t       ledInsert(uint16_t led) { return (led < LED_MAX) ? s_led[led] : UNASSIGNED; }
bool          anyAssigned()       { return s_any; }

// Matches the label the owner sees first, then the shipped one, so /api/latch
// keeps working with either.
int indexOf(const char* name) {
  for (uint8_t i = 0; i < s_nIns; i++)
    if (s_nam[i][0] && strncmp(s_nam[i], name, NAME_LEN) == 0) return i;
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

const char* nameOf(uint8_t ins) {
  if (ins >= s_nIns) return "";
  return s_nam[ins][0] ? s_nam[ins] : s_ins[ins].name;
}
bool setName(uint8_t ins, const char* name) {
  if (ins >= s_nIns) return false;
  strncpy(s_nam[ins], name ? name : "", NAME_LEN - 1);
  s_nam[ins][NAME_LEN - 1] = 0;
  return true;
}
bool saveNames() { return s_prefs.putBytes(NVS_NAM, s_nam, sizeof(s_nam)) == sizeof(s_nam); }

Colour colourOf(uint8_t ins) {
  return (ins < s_nIns) ? s_col[ins] : Colour{ 0, 0, 0, 0 };
}
Colour colourOfLed(uint16_t led) {
  const uint8_t a = ledInsert(led);
  return (a == UNASSIGNED) ? Colour{ 0, 0, 0, 0 } : colourOf(a);
}
bool setColour(uint8_t ins, Colour c) {
  if (ins >= s_nIns) return false;
  s_col[ins] = c;
  return true;
}
void clearColours() { memset(s_col, 0, sizeof(s_col)); }

bool saveColours() {
  return s_prefs.putBytes(NVS_COL, s_col, sizeof(s_col)) == sizeof(s_col);
}
// Only inserts that carry a colour, by the machine's own name: a fresh wall is
// two bytes, and the UI never has to know the table's internal indices.
String coloursJson() {
  String j = "{\"colours\":[";
  bool first = true;
  for (uint8_t i = 0; i < s_nIns; i++) {
    const Colour& c = s_col[i];
    if (!(c.r | c.g | c.b | c.w)) continue;
    if (!first) j += ',';
    first = false;
    j += "{\"n\":\"" + String(s_ins[i].name) + "\",\"i\":" + String(i) +
         ",\"r\":" + String(c.r) + ",\"g\":" + String(c.g) +
         ",\"b\":" + String(c.b) + ",\"w\":" + String(c.w) + "}";
  }
  j += "]}";
  return j;
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
// Built from memory rather than streamed from the file, so the owner's renames
// and colours are part of the one description the UI reads. "d" is the shipped
// label, kept so the UI can show what a rename replaced.
String insertsJson() {
  String j = "{\"inserts\":[";
  for (uint8_t i = 0; i < s_nIns; i++) {
    if (i) j += ',';
    const Colour& c = s_col[i];
    j += "{\"n\":\"" + String(nameOf(i)) + "\",\"d\":\"" + String(s_ins[i].name) +
         "\",\"l\":" + String(s_ins[i].lamp) +
         ",\"k\":\"" + String(s_ins[i].kind) + "\"" +
         ",\"x\":" + String(s_ins[i].x, 4) + ",\"y\":" + String(s_ins[i].y, 4);
    if (c.r | c.g | c.b | c.w)
      j += ",\"c\":[" + String(c.r) + "," + String(c.g) + "," + String(c.b) + "," + String(c.w) + "]";
    j += "}";
  }
  j += "]}";
  return j;
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
  clearColours();
  memset(s_nam, 0, sizeof(s_nam));
  const bool haveTable = loadInserts();
  const bool haveMap   = haveTable && loadAssignment();
  if (s_prefs.getBytesLength(NVS_COL) == sizeof(s_col))
    s_prefs.getBytes(NVS_COL, s_col, sizeof(s_col));
  if (s_prefs.getBytesLength(NVS_NAM) == sizeof(s_nam))
    s_prefs.getBytes(NVS_NAM, s_nam, sizeof(s_nam));
  uint16_t n = 0;
  for (uint16_t i = 0; i < LED_MAX; i++) if (s_led[i] != UNASSIGNED) n++;
  Serial.printf("[pf] %u inserts%s, %u pixels placed\n",
                s_nIns, haveTable ? "" : " (MISSING /arena_pf.json)", n);
  (void)haveMap;
}

}  // namespace arenapf
