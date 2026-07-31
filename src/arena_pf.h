#pragma once
#include <Arduino.h>
#include "arena_config.h"

// ============================================================================
//  Arena playfield geometry — where each pixel actually sits on the table.
//
//  Two different things, deliberately kept apart:
//
//   * The **insert table** is the playfield itself: 99 inserts with a Gottlieb
//     lamp name (L26, LS10, F4a) and a normalised 0..1 position. It was pulled
//     out of the Visual Pinball table (`tools/vpx_inserts.py`) and shipped as
//     `/arena_pf.json`. It does not change: it is the machine.
//
//   * The **LED assignment** is which chain index landed on which insert, which
//     is a property of how the wall was wired and is filled in from the web UI
//     one pixel at a time. 150 bytes, persisted to `/arena_leds.json`.
//
//  Splitting them is what makes the geometry cheap: the chain stores an index,
//  not a coordinate, so re-routing a run never invalidates the playfield.
//
//  Effects use xy(): index -> position. Anything spatial (a wave climbing the
//  playfield, a ripple leaving the spinner) is written in those coordinates
//  rather than in chain order, which is arbitrary.
// ============================================================================

namespace arenapf {

static const uint8_t  INSERT_MAX  = 128;
static const uint8_t  NAME_LEN    = 8;
static const uint8_t  UNASSIGNED  = 255;

struct Insert {
  char  name[NAME_LEN];
  char  kind;        // 'i' matrix insert, 'f' flasher
  int8_t lamp;       // matrix lamp number parsed from the name (L26a -> 26), -1 if none
  float x, y;        // 0..1, origin top-left: y grows towards the flippers
};

void begin();                       // load the insert table + the LED assignment

uint8_t        insertCount();
const Insert*  insert(uint8_t i);   // nullptr if out of range
int            indexOf(const char* name);

uint8_t  ledInsert(uint16_t led);              // UNASSIGNED if not placed yet
bool     setLedInsert(uint16_t led, uint8_t ins);
void     clearAssignment();

// Position of a chain index, 0..1. False when that pixel has no insert yet —
// callers must decide what an unplaced pixel does rather than get (0,0).
bool xy(uint16_t led, float& x, float& y);

// Matrix lamp that drives the pixel at this chain index, -1 if it sits on a
// flasher or has not been placed. This is what lets a lamp mask captured from
// the ROM be painted straight onto the wall.
int lampOfLed(uint16_t led);
bool anyAssigned();

String toJson();                    // {"leds":[{"i":0,"a":12},...]}
bool   fromJson(const char* json);
String insertsJson();               // the fixed table, straight from LittleFS
bool   save();

}  // namespace arenapf
