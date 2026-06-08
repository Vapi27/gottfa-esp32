// psorom.h — PSOROM: run the ORIGINAL Gottlieb sound-board 6502(s) + sound ROM on the ESP for
// exact-by-construction sound. DAC writes become audio directly; control chips (YM2151/AY) become
// PSOWAV triggers (logged for now). See PSOROM.md.
//   GTS80S       : 1×6502 + 6530 RIOT + DAC (also the 80B D-CPU core).
//   GTS80B_GEN3  : 2×6502 (Y = YM2151, D = DAC) + cross-NMI — the 80B target.
// CPU core = vendored PUBLIC-DOMAIN Fake6502 (global state; the dual-CPU board context-switches it).
// (C) 2026 Valere Pilpil / Pstore. Original implementation (CPU core = PD Fake6502).
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace psorom {

enum Board { GTS80S = 0, GTS80B_GEN3 = 1 };

// GTS80S:      rom1 = 6530 system code (6530sy80.bin), rom2 = per-game .snd (4-bit DAC data).
// GTS80B_GEN3: rom1 = Y-CPU ROM (yrom1.snd),           rom2 = D-CPU ROM (drom1.snd).
bool     begin(Board b, const uint8_t* rom1, size_t len1, const uint8_t* rom2, size_t len2);
void     reset();
void     command(uint8_t cmd);                      // inject a sound command (MPU-style, inverted for 80B)
uint32_t run(uint32_t cycles);                      // run ~cycles (per CPU for 80B); returns ticks
int      dacDrain(int16_t* out, int maxN);          // drain captured DAC samples

uint16_t pcNow();                                   // debug
uint32_t dacCount();                                // total DAC writes since reset
uint32_t insCount();                                // total instructions (both CPUs for 80B)
uint32_t ymWrites();                                // 80B: YM2151 register writes (chip stubbed)

} // namespace psorom
