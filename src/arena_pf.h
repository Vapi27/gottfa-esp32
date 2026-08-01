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

static const uint8_t FUNC_LEN = 30;

struct Insert {
  char  name[NAME_LEN];
  char  func[FUNC_LEN];  // what the manual calls this lamp ("W DROP TARGET"), may be empty
  char  kind;            // 'i' matrix insert, 'f' flasher
  int8_t lamp;           // the machine's lamp number driving it, -1 if none
  float x, y;            // 0..1, origin top-left: y grows towards the flippers
};

// Colour of the plastic over an insert, 0,0,0,0 = none set (use the bulb's own
// colour). It belongs to the INSERT, not to the pixel: on a real playfield the
// colour is the moulded plastic, so it must survive re-routing the chain.
struct Colour { uint8_t r, g, b, w; };

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

// The label an insert carries. The shipped one is derived from the VP table
// plus PinMAME's GTS80_lamp2m offset, which is a GUESS: it matched the one
// insert we could check against the real playfield and failed on another. The
// machine's own documentation is the authority and only the owner has it, so
// the label is editable and the override wins. Nothing computes with it — the
// ROM sequence is indexed by `lamp`, never by the name.
const char* nameOf(uint8_t ins);
bool  setName(uint8_t ins, const char* name);   // empty string restores the shipped one
bool  saveNames();

Colour colourOf(uint8_t ins);            // 0,0,0,0 when the insert has no colour
Colour colourOfLed(uint16_t led);
bool   setColour(uint8_t ins, Colour c); // all zero clears it
void   clearColours();
String coloursJson();
bool   saveColours();
bool anyAssigned();

String toJson();                    // {"leds":[{"i":0,"a":12},...]}
bool   fromJson(const char* json);
String insertsJson();               // the fixed table, straight from LittleFS
bool   save();

}  // namespace arenapf
