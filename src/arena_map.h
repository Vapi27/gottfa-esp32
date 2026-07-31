#pragma once
#include <Arduino.h>
#include "arena_config.h"

// ============================================================================
//  Arena insert map — which LED lights which part of the playfield.
//
//  The daisy chain is a straight line of anonymous pixels; the *meaning* of an
//  index (this one is under the spinner, those nine are the pop bumpers...) is
//  pure data. It lives in a small table that can be edited from the web UI and
//  is persisted as JSON on LittleFS (ARENA_MAP_PATH), so re-routing the chain or
//  adding LEDs never means recompiling.
//
//  The built-in default is a 100-LED template following the chain order used in
//  ARENA_LED.md (bottom-left -> up the playfield -> back panel). Treat the zone
//  boundaries as a starting point: run the mapping wizard (web UI -> "Map"),
//  which lights one LED at a time so you can assign it to a real Arena insert.
// ============================================================================

namespace arenamap {

static const uint8_t ZONE_MAX = 24;
static const uint8_t ZONE_NAME_LEN = 20;

struct Zone {
  char     name[ZONE_NAME_LEN];
  uint16_t first;      // first LED index in the chain
  uint16_t count;      // number of LEDs (0 = zone disabled)
};

void begin();                       // load ARENA_MAP_PATH, else install the default table
bool load();                        // (re)load from LittleFS  — false if absent/invalid
bool save();                        // write the current table to LittleFS
void reset();                       // back to the compiled-in default template

uint8_t       count();              // number of zones
const Zone*   zone(uint8_t i);      // nullptr if out of range
int           indexOf(const char* name);   // -1 if unknown
int           zoneOfLed(uint16_t led);     // which zone owns this LED, -1 if none

bool setZone(uint8_t i, const char* name, uint16_t first, uint16_t cnt);
bool addZone(const char* name, uint16_t first, uint16_t cnt);
bool removeZone(uint8_t i);

// Serialise the whole table as JSON: {"zones":[{"n":"pop-bumpers","f":30,"c":9},...]}
String toJson();
// Replace the whole table from the same JSON shape. False on parse error (table untouched).
bool fromJson(const char* json);

}  // namespace arenamap
