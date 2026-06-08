// ym2151w.h — wrapper around the ymfm YM2151 (OPM) core (Gottlieb System-80B Gen3 music).
// ymfm by Aaron Giles, BSD-3-Clause (see ymfm.LICENSE). Clock on the GTS80BS3 board = 3.579545 MHz
// -> native output ~55.93 kHz (clock/64).
//
// On the ESP32 the heavy FM synthesis runs in a DEDICATED FreeRTOS task on CORE 0 (next to WiFi), so
// it does not steal cycles from the 6502 emulation on core 1. Register writes (core 1) are pushed to
// a lock-free queue; samples are produced into a lock-free ring that renderMix() drains via
// nextSample(). On the host (no FreeRTOS) everything is synchronous.
#pragma once
#include <stdint.h>
namespace ym2151w {
void     begin();                       // ESP: start the core-0 synth task (idempotent); host: no-op
void     reset();                        // queue a chip reset
void     write(uint8_t offset, uint8_t data);   // offset 0 = address latch, 1 = data (core 1 -> queue)
int16_t  nextSample();                   // one native (~55.93 kHz) mono sample (ring pop / synchronous on host)
uint32_t sampleRate();                   // native rate for the 3.579545 MHz clock
}
