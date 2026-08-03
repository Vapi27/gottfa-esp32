#include <LittleFS.h>
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
  { "letters",         51,  5 },
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

static void copyName(char* dst, const char* src) {
  strncpy(dst, src ? src : "", ZONE_NAME_LEN - 1);
  dst[ZONE_NAME_LEN - 1] = '\0';
}

void reset() {
  s_n = DEFAULT_N;
  memcpy(s_zones, DEFAULT_ZONES, sizeof(DEFAULT_ZONES));
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
  for (uint8_t i = 0; i < s_n; i++)
    if (led >= s_zones[i].first && led < (uint16_t)(s_zones[i].first + s_zones[i].count)) return i;
  return -1;
}

bool setZone(uint8_t i, const char* name, uint16_t first, uint16_t cnt) {
  if (i >= s_n || first >= LED_MAX) return false;
  if (first + cnt > LED_MAX) cnt = LED_MAX - first;
  copyName(s_zones[i].name, name);
  s_zones[i].first = first;
  s_zones[i].count = cnt;
  return true;
}

bool addZone(const char* name, uint16_t first, uint16_t cnt) {
  if (s_n >= ZONE_MAX || first >= LED_MAX) return false;
  if (first + cnt > LED_MAX) cnt = LED_MAX - first;
  copyName(s_zones[s_n].name, name);
  s_zones[s_n].first = first;
  s_zones[s_n].count = cnt;
  s_n++;
  return true;
}

bool removeZone(uint8_t i) {
  if (i >= s_n) return false;
  for (uint8_t k = i; k + 1 < s_n; k++) s_zones[k] = s_zones[k + 1];
  s_n--;
  return true;
}

String toJson() {
  String j = "{\"zones\":[";
  for (uint8_t i = 0; i < s_n; i++) {
    if (i) j += ',';
    j += "{\"n\":\"";
    j += s_zones[i].name;
    j += "\",\"f\":" + String(s_zones[i].first) + ",\"c\":" + String(s_zones[i].count) + "}";
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

  // Build into a scratch table first: a malformed entry must not corrupt a good map.
  Zone tmp[ZONE_MAX];
  uint8_t n = 0;
  for (JsonObjectConst z : arr) {
    if (n >= ZONE_MAX) break;
    const char* name = z["n"] | "";
    uint32_t f = z["f"] | 0;
    uint32_t c = z["c"] | 0;
    if (f >= LED_MAX) continue;
    if (f + c > LED_MAX) c = LED_MAX - f;
    copyName(tmp[n].name, name);
    tmp[n].first = (uint16_t)f;
    tmp[n].count = (uint16_t)c;
    n++;
  }
  if (n == 0) return false;
  memcpy(s_zones, tmp, sizeof(Zone) * n);
  s_n = n;
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

bool save() {
  File f = LittleFS.open(ARENA_MAP_PATH, "w");
  if (!f) return false;
  String j = toJson();
  size_t w = f.print(j);
  f.close();
  return w == j.length();
}

void begin() {
  reset();
  if (load()) Serial.printf("[map] loaded %s (%u zones)\n", ARENA_MAP_PATH, s_n);
  else        Serial.printf("[map] default template (%u zones)\n", s_n);
}

}  // namespace arenamap
