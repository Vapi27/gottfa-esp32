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

enum Board { GTS80S = 0, GTS80B_GEN3 = 1, GTS80B_GEN2 = 2, GTS80B_GEN1 = 3 };
// Gen1 = AY-3-8910 + SP0250 speech (e.g. Arena: Y-ROM = yrom1++yrom2 16K, D-ROM = drom1 8K) ;
// Gen2 = AY-3-8912 ; Gen3 = YM2151.

// GTS80S:      rom1 = 6530 system code (6530sy80.bin), rom2 = per-game .snd (4-bit DAC data).
// GTS80B_GEN3: rom1 = Y-CPU ROM (yrom1.snd),           rom2 = D-CPU ROM (drom1.snd).
bool     begin(Board b, const uint8_t* rom1, size_t len1, const uint8_t* rom2, size_t len2);
void     reset();
void     command(uint8_t cmd);                      // inject a sound command (MPU-style, inverted for 80B)
uint32_t run(uint32_t cycles);                      // run ~cycles (per CPU for 80B); returns ticks
uint32_t clockNow();                                // horloge emulee (wallclk, cycles Y-CPU) — LA reference de temps
uint32_t levDrops();                                // evenements live JETES (ring plein) — 0 attendu en mode chef v2
void     setDip(uint8_t d);                         // DIP carte son Gen1 (bit6 : son d'attract) — avant begin()
int      dacDrain(int16_t* out, int maxN);          // drain captured DAC samples

uint16_t pcNow();                                   // debug
uint32_t dacCount();                                // total DAC writes since reset
void     dbgNmi(uint32_t* y, uint32_t* d, uint8_t* en, uint8_t* rate);   // debug chaine NMI
void     dbgPc(uint16_t* ypc, uint16_t* dpc);                              // debug PC des 2 CPU
uint8_t  dbgYRam(uint16_t a);                                              // debug RAM Y
uint32_t insCount();                                // total instructions (both CPUs for 80B)
uint32_t ymWrites();                                // 80B: YM2151 register writes (chip stubbed)
int      ayRender(int16_t* out, int n);             // Gen1/Gen2: rend n echantillons AY (emu2149) @ ayFs()
int      ayFs();                                    // frequence d'echantillonnage AY (Hz)
void     dbgGen1(uint32_t* o);  // DEBUG: [Ypc,Dpc,nmi_en,yNmiCnt,hist0..15]
int      renderMix(int16_t* out, int n);            // 80B: avance l'emu + rend n ech. mixes (DAC + AY) @ ayFs()
uint32_t activity();                                // ROM-chef: total ecritures DAC+puces (detecte sustain/idle)
void     activitySplit(uint32_t* dac, uint32_t* tone, uint32_t* sp); // idem mais SEPARE : DAC(samples) / AY+YM(musique) / SP0250(voix)
uint32_t toneMask();                                // ROM-chef: canaux tonals sonores (6 bits AY + 8 bits YM<<6) — suivi PAR CANAL
uint32_t stateHash();                               // empreinte d'etat (verif coeur rapide)
int      synthBench(int ms);                        // bench: ech/s de la synthese seule (faisabilite live)
struct Ev { uint32_t t; uint8_t ty, a, b; };        // evenement puce horodate (ty: 0=AY0 1=AY1 2=SP 3=DAC)
int      liveDrain(Ev* out, int maxN);              // draine le flux BRUT (analyse hors-ligne / chef v2)
void     liveEvents(bool on);                       // PSOLIVE: pousse les ecritures puces dans un ring horodate
int      liveRender(int16_t* out, int n);           // PSOLIVE (coeur 1): applique les evenements + synthetise
void     setSynth(bool on);                         // on=synthese (renderMix produit l'audio, pour l'extraction hors-ligne) ; off=chef d'orchestre rapide
void     setMixMask(uint8_t m);                     // stems host : bit0=DAC bit1=AY0 bit2=AY1 bit3=SP/YM (defaut 0x0F)

} // namespace psorom
