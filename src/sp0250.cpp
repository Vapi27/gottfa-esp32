// sp0250.cpp — GI SP0250 LPC speech DSP core. See sp0250.h.
//
// Ported from MAME's src/devices/sound/sp0250.cpp, which carries:
//     // license:BSD-3-Clause
//     // copyright-holders:Olivier Galibert
// Only the integer DSP (6-stage IIR lattice + LFSR/glottal excitation + gc/ga coding tables + the
// 15-byte LPC frame decode) is retained; all MAME device/stream/timer glue is removed. Redistributed
// under the BSD-3-Clause license (copyright Olivier Galibert and the MAME team) — the copyright notice,
// this license reference, and the disclaimer are preserved per BSD-3-Clause.
#pragma GCC optimize("O3")   /* chemin chaud temps-reel : vitesse > taille (PlatformIO compile en -Os) */
#include "sp0250.h"

namespace sp0250 {

struct Filt {
  int16_t F, B, z1, z2;
  inline int16_t apply(int16_t in) {
    int16_t z0 = (int16_t)((int32_t)in + (((int32_t)z1 * F) >> 8) + (((int32_t)z2 * B) >> 9));
    z2 = z1; z1 = z0; return z0;
  }
  inline void rst() { z1 = z2 = 0; }
};

static Filt    filt[6];
static int16_t amp = 0;
static uint16_t lfsr = 0x7fff;
static uint8_t pitch = 0, pcount = 0, repeat = 0, rcount = 0;
static bool    voiced = false;
static uint8_t fifo[15], fifoPos = 0;

static inline uint16_t ga(uint8_t v) { return (uint16_t)((v & 0x1f) << (v >> 5)); }

static int16_t gc(uint8_t v) {            // internal coding ROM (from the SP0250 manual)
  static const uint16_t coefs[128] = {
      0,   9,  17,  25,  33,  41,  49,  57,  65,  73,  81,  89,  97, 105, 113, 121,
    129, 137, 145, 153, 161, 169, 177, 185, 193, 201, 209, 217, 225, 233, 241, 249,
    257, 265, 273, 281, 289, 297, 301, 305, 309, 313, 317, 321, 325, 329, 333, 337,
    341, 345, 349, 353, 357, 361, 365, 369, 373, 377, 381, 385, 389, 393, 397, 401,
    405, 409, 413, 417, 421, 425, 427, 429, 431, 433, 435, 437, 439, 441, 443, 445,
    447, 449, 451, 453, 455, 457, 459, 461, 463, 465, 467, 469, 471, 473, 475, 477,
    479, 481, 482, 483, 484, 485, 486, 487, 488, 489, 490, 491, 492, 493, 494, 495,
    496, 497, 498, 499, 500, 501, 502, 503, 504, 505, 506, 507, 508, 509, 510, 511 };
  int16_t res = (int16_t)coefs[v & 0x7f];
  if (!(v & 0x80)) res = -res;
  return res;
}

static void loadValues() {
  filt[0].B = gc(fifo[0]);  filt[0].F = gc(fifo[1]);  amp     = (int16_t)ga(fifo[2]);
  filt[1].B = gc(fifo[3]);  filt[1].F = gc(fifo[4]);  pitch   = fifo[5];
  filt[2].B = gc(fifo[6]);  filt[2].F = gc(fifo[7]);  repeat  = fifo[8] & 0x3f; voiced = (fifo[8] & 0x40) != 0;
  filt[3].B = gc(fifo[9]);  filt[3].F = gc(fifo[10]);
  filt[4].B = gc(fifo[11]); filt[4].F = gc(fifo[12]);
  filt[5].B = gc(fifo[13]); filt[5].F = gc(fifo[14]);
  fifoPos = 0; pcount = 0; rcount = 0;
  for (int f = 0; f < 6; f++) filt[f].rst();
}

void reset() {
  for (int i = 0; i < 15; i++) fifo[i] = 0;
  fifoPos = 0; lfsr = 0x7fff; amp = 0; pitch = pcount = repeat = rcount = 0; voiced = false;
  loadValues();
}

static unsigned long g_feeds=0;
static uint32_t g_idleN = 0;                          // echantillons depuis le dernier feed : affame = MUET
void feed(uint8_t d) { g_feeds++; g_idleN = 0; if (fifoPos != 15) fifo[fifoPos++] = d; }
unsigned long feeds() { return g_feeds; }
bool ready() { return fifoPos != 15; }

int next() {
  if (++g_idleN > 4480) return 0;                     // plus nourri depuis ~450 ms (vraie parole = flux continu de trames) :
  if (rcount >= repeat) {                             // la puce reelle REPETE la derniere trame a l'infini -> "tut-tut" perpetuel

    if (fifoPos == 15) loadValues();
    else { repeat = 1; pcount = 0; rcount = 0; }     // NOP frame while waiting for input
  }
  lfsr ^= (uint16_t)((lfsr ^ (lfsr >> 1)) << 15);     // 15-bit LFSR, clocks every sample
  lfsr >>= 1;
  int16_t z0 = voiced ? ((pcount == 0) ? amp : (int16_t)0)
                      : ((lfsr & 1) ? amp : (int16_t)-amp);
  for (int f = 0; f < 6; f++) z0 = filt[f].apply(z0);
  int dac = z0 >> 6;                                  // ~13-bit -> 7-bit
  if (dac < -64) dac = -64; if (dac > 63) dac = 63;
  if (pcount++ == pitch) { pcount = 0; rcount++; }
  return dac;
}

} // namespace sp0250
