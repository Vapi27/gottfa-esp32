// psorom.h — PSOROM: run the ORIGINAL Gottlieb sound-board 6502 + sound ROM (exact-by-construction
// sound). The 6502 (vendored PD Fake6502) executes the real ROM; chip writes are intercepted —
// the DAC port becomes audio samples directly (the bulk of 80B sound is DAC), control chips
// (YM2151/AY) become PSOWAV sample triggers. So orchestration (start/stop/replace/game-over) is
// exact because it IS the original program running.
//
// STAGE 1 (this file): the single-6502 board (GTS80S = 6530 RIOT + DAC) — which is also the 80B
// D-CPU (DAC) core. STAGE 2: 80B dual-6502 + YM2151/AY (Y-CPU) + cycle-accurate 6530 timer +
// diff vs PinMAME. STAGE 3: ESP audio-task integration + real-time bench. See PSOROM.md.
// (C) 2026 Valere Pilpil / Pstore. Original implementation (CPU core = PD Fake6502).
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace psorom {

bool     begin(const uint8_t* rom, size_t romLen);  // map ROM at top of 64K, init RIOT, reset
void     reset();
void     command(uint8_t cmd);                      // inject a sound command (RIOT port B + IRQ)
uint32_t run(uint32_t cycles);                      // execute ~cycles 6502 ticks; returns ticks run
int      dacDrain(int16_t* out, int maxN);          // drain captured DAC samples; returns count

uint16_t pcNow();                                   // current 6502 PC (debug)
uint32_t dacCount();                                // total DAC writes since reset (debug)
uint32_t insCount();                                // total instructions executed (debug)

} // namespace psorom
