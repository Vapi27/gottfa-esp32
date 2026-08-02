#pragma once
#include <Arduino.h>
#include "arena_config.h"

// ============================================================================
//  Arena's ORIGINAL attract mode, taken from the game ROM.
//
//  Not an imitation. `tools/capture_attract.cpp` runs Arena's own prom1/prom2
//  under PinMAME and writes down the lamp matrix the ROM drives, 50 times a
//  second. The result is `/arena_attract.bin`: one 64-bit lamp mask per frame.
//
//  It lands on the wall because the whole chain of identifiers lines up — the
//  ROM drives lamp n, the playfield plan calls that insert L<n> (both come from
//  the same machine), and the wizard recorded which pixel sits on that insert.
//  Cross-checked before trusting it: all 44 lamps the ROM touches exist on the
//  plan, none orphaned.
//
//  17 KB for 44 s. Loaded to RAM once, since re-reading LittleFS at 50 Hz to
//  save 17 KB on a board with 200 KB free would be a poor trade.
// ============================================================================

// 12288 frames = 10 min at 50 ms = 96 KB - comfortably inside the ~190 KB free
// heap of the non-PSRAM boards, and far beyond any attract loop worth storing.
static const uint16_t ARENA_ATTRACT_MAX_FRAMES = 12288;

namespace arenaattract {

void     begin();                  // load /arena_attract.bin if present
bool     available();              // false -> the generic attract effect stands in
uint16_t frames();
uint16_t stepMs();

// Lamp mask for a point in the sequence; wraps, so callers just hand it a
// free-running millisecond clock.
uint64_t maskAt(uint32_t ms);

}  // namespace arenaattract
