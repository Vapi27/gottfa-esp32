// ym2151w.h — thin wrapper around the ymfm YM2151 (OPM) core (Gottlieb System-80B Gen3 music).
// ymfm by Aaron Giles, BSD-3-Clause (see ymfm.LICENSE). Clock on the GTS80BS3 board = 3.579545 MHz
// -> native output ~55.93 kHz (clock/64); the caller resamples to the mix rate.
#pragma once
#include <stdint.h>
namespace ym2151w {
void     reset();
void     write(uint8_t offset, uint8_t data);   // offset 0 = address latch, 1 = data
void     generate(int16_t* out, int n);         // n native (~55.93 kHz) mono samples (L+R)/2
uint32_t sampleRate();                           // native rate for the 3.579545 MHz clock
}
