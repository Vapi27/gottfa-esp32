#pragma once
#include <Arduino.h>
#include "arena_config.h"

// ============================================================================
//  Arena Wall-Art LED — render engine.
//
//  Pipeline, once per frame (LED_FRAME_HZ):
//     effect renders into frame[]  (full-scale RGBW, no brightness applied)
//        -> optional crossfade with the previous mode (mode changes never snap)
//        -> identify/zone overlay (mapping wizard)
//        -> global brightness
//        -> power meter + limiter (never exceed LED_POWER_BUDGET_MA)
//        -> push to the SK6812 chain
//
//  Everything is non-blocking: tick() returns immediately when it is not yet
//  time for the next frame, so the web server keeps its latency.
// ============================================================================

namespace arenaled {

struct Rgbw {
  uint8_t r, g, b, w;
};

enum Mode : uint8_t {
  MODE_OFF = 0,     // all pixels dark (the chain stays powered/refreshed)
  MODE_CLASSIC,     // static warm white with a subtle incandescent flicker
  MODE_ATTRACT,     // slow pulses / waves / chases / random inserts, auto-cycled
  MODE_ARENA,       // animations that follow the playfield layout (zone-driven)
  MODE_NIGHT,       // warm white at ~10 %
  MODE_RAINBOW,     // full RGB hue sweep
  MODE_TEST,        // R / G / B / W channel walk — wiring + colour-order check
  MODE_COUNT
};

void begin();
void tick();                          // call from loop()

void        setMode(Mode m);
Mode        mode();
const char* modeName(Mode m);
Mode        modeFromName(const char* s);   // MODE_COUNT if unknown
void        nextMode();                    // front-panel button: cycle usable modes

void    setBrightness(uint8_t b);
uint8_t brightness();
void    setSpeed(uint8_t s);               // 0..255 -> x0.25 .. x4
uint8_t speed();
void    setColor(Rgbw c);                  // base colour for CLASSIC / ATTRACT / ARENA
Rgbw    color();
void    setCount(uint16_t n);              // live LED count (1..LED_MAX)
uint16_t count();
void    setBudgetMa(uint16_t ma);          // power ceiling for the whole chain
uint16_t budgetMa();

// --- mapping helpers (web UI wizard) ---------------------------------------
void identifyLed(int led, uint32_t ms);    // spotlight one pixel, any mode, then auto-clear
void identifyZone(int zoneIdx, uint32_t ms);
void clearIdentify();
int  identifyingLed();                     // -1 when idle

// --- telemetry --------------------------------------------------------------
float    lastAmps();       // A estimated for the frame just pushed
bool     limited();        // true if the power limiter had to scale the last frame
uint32_t frameCount();
uint16_t fps();

void save();               // persist mode/brightness/speed/colour/count to NVS

}  // namespace arenaled
