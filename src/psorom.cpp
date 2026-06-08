// psorom.cpp — see psorom.h. Stage-2 GTS80S model around the PD Fake6502 core, with a
// cycle-accurate 6530 RIOT (timer + prescaler + IRQ + ports), per PinMAME 6530riot.c / gts80s.c.
// (C) 2026 Valere Pilpil / Pstore. Original implementation (CPU core = PD Fake6502).
#include "psorom.h"
#include "fake6502.h"
#include <string.h>

namespace psorom {

// ---- memory: 64B RAM mirrored over 0x0000-0x01FF, RIOT 0x0200-0x03FF, ROM 0x0400-0x0FFF +
// 0xF800-0xFFFF, 256B RAM at 0x1000-0x10FF (gts80s.c). ----
static uint8_t  ram64[0x40];          // 0x0000-0x01FF (mirrored every 64 bytes)
static uint8_t  ram2[0x100];          // 0x1000-0x10FF
static uint8_t  rom[0x10000];         // ROM image, placed at its addresses
static bool     isRom[0x10000];

// ---- 6530 RIOT ----
struct Riot {
  uint8_t  out_a, ddr_a, out_b, ddr_b, in_a, in_b;
  uint8_t  timer_start; uint16_t divider; uint8_t timer_irq_en;
  uint8_t  irq_state;                  // bit7 = RIOT_TIMERIRQ
  uint32_t timer_set_clk;              // clockticks6502 when timer last written
  uint32_t underflow_clk;             // clk at which the timer underflows
  bool     timerFired;
} static rt;
static const uint8_t TIMERIRQ = 0x80;

// DAC capture ring
static int16_t  dacBuf[2048];
static volatile int dacHead = 0, dacTail = 0;
static uint32_t dacWrites = 0;

static inline void dacPush(uint8_t portA) {
  int16_t s = (int16_t)((((int16_t)portA << 7) - 0x4000) * 2);   // gts80s_riot6530_0a_w
  int n = (dacHead + 1) & 2047;
  if (n != dacTail) { dacBuf[dacHead] = s; dacHead = n; }
  dacWrites++;
}

static uint8_t riotRead(uint16_t addr) {
  int off = addr & 0x0f;
  if (!(off & 0x04)) {                                            // I/O
    switch (off & 0x03) {
      case 0x00: return (uint8_t)((rt.out_a & rt.ddr_a) | (rt.in_a & ~rt.ddr_a));   // PORTA
      case 0x01: return rt.ddr_a;
      case 0x02: return (uint8_t)((rt.out_b & rt.ddr_b) | (rt.in_b & ~rt.ddr_b));   // PORTB (command)
      case 0x03: return rt.ddr_b;
    }
  } else {
    if (!(off & 0x01)) {                                          // TIMER read
      int val;
      uint32_t elapsed = clockticks6502 - rt.timer_set_clk;
      if (rt.timerFired) val = 0xff - (int)((clockticks6502 - rt.underflow_clk) & 0xff);
      else { int diff = (int)((elapsed + rt.divider - 1) / rt.divider); val = rt.timer_start - diff; }
      if (val < 0) val = 0;
      rt.irq_state &= ~TIMERIRQ;                                  // reading the timer clears its IRQ
      rt.timer_irq_en = off & 0x08;
      return (uint8_t)val;
    } else {                                                      // IRF (interrupt flags)
      uint8_t v = rt.irq_state;
      return v;
    }
  }
  return 0;
}

static void riotWrite(uint16_t addr, uint8_t data) {
  int off = addr & 0x0f;
  if (!(off & 0x04)) {                                            // I/O
    switch (off & 0x03) {
      case 0x00: rt.out_a = data; if (rt.ddr_a) dacPush(rt.out_a & rt.ddr_a); break;  // PORTA -> DAC
      case 0x01: rt.ddr_a = data; break;
      case 0x02: rt.out_b = data; break;
      case 0x03: rt.ddr_b = data; break;
    }
  } else {                                                        // TIMER write
    rt.timer_irq_en = off & 0x08;
    rt.timer_start  = data;
    switch (off & 0x03) { case 0: rt.divider=1; break; case 1: rt.divider=8; break;
                          case 2: rt.divider=64; break; case 3: rt.divider=1024; break; }
    rt.irq_state &= ~TIMERIRQ;
    rt.timer_set_clk = clockticks6502;
    rt.underflow_clk = clockticks6502 + (uint32_t)rt.divider * rt.timer_start + 1;
    rt.timerFired = false;
  }
}

extern "C" uint8_t read6502(uint16_t address) {
  if (address < 0x0200)               return ram64[address & 0x3f];     // 64B RAM mirrored
  if (address <= 0x03FF)              return riotRead(address);
  if (address >= 0x1000 && address <= 0x10FF) return ram2[address & 0xff];
  return rom[address];
}
extern "C" void write6502(uint16_t address, uint8_t value) {
  if (address < 0x0200)              { ram64[address & 0x3f] = value; return; }
  if (address <= 0x03FF)             { riotWrite(address, value); return; }
  if (address >= 0x1000 && address <= 0x10FF) { ram2[address & 0xff] = value; return; }
  if (!isRom[address]) rom[address] = value;
}

bool begin(const uint8_t* code, size_t codeLen, const uint8_t* data, size_t dataLen) {
  if (!code || !codeLen || codeLen > 0x10000) return false;
  memset(rom, 0, sizeof(rom)); memset(isRom, 0, sizeof(isRom));
  memset(ram64, 0, sizeof(ram64)); memset(ram2, 0, sizeof(ram2));
  // 6530 SYSTEM code ROM (6530sy80.bin, 1 KB) maps at 0x0C00-0x0FFF AND mirrored at the top
  // (0xFC00-0xFFFF) for the 6502 vectors (reset vector points into the 0x0C00 image).
  for (size_t i = 0; i < codeLen && (0x0C00 + i) <= 0x0FFF; i++) { rom[0x0C00 + i] = code[i]; isRom[0x0C00 + i] = true; }
  size_t base = 0x10000 - codeLen;
  for (size_t i = 0; i < codeLen; i++) { rom[base + i] = code[i]; isRom[base + i] = true; }
  // per-game SOUND DATA (.snd) at 0x0400-0x0BFF = 4-bit DAC nibbles (upper nibble masked off).
  if (data && dataLen)
    for (size_t i = 0; i < dataLen && (0x0400 + i) <= 0x0BFF; i++) { rom[0x0400 + i] = data[i] & 0x0f; isRom[0x0400 + i] = true; }
  for (int i = 0; i < 0x100; i++) ram2[i] = rom[0x0700 + i];      // gts80s_init: 0x1000 seeded from 0x0700
  reset();
  return true;
}

void reset() {
  dacHead = dacTail = 0; dacWrites = 0;
  memset(&rt, 0, sizeof(rt)); rt.divider = 1024; rt.in_b = 0x20;  // dips=0, bit5 set, no cmd
  reset6502();
}

void command(uint8_t cmd) { rt.in_b = 0x20 | (cmd & 0x0f); }      // gts80s_data_w (System 80 path)

uint32_t run(uint32_t cycles) {
  uint32_t start = clockticks6502, target = start + cycles;
  while (clockticks6502 < target) {
    step6502();
    // timer underflow -> set IRQ flag (edge); fire CPU IRQ if enabled and not masked
    if (!rt.timerFired && clockticks6502 >= rt.underflow_clk) {
      rt.timerFired = true; rt.irq_state |= TIMERIRQ;
      if (rt.timer_irq_en && !(status & 0x04)) irq6502();
    }
  }
  return clockticks6502 - start;
}

int dacDrain(int16_t* out, int maxN) {
  int n = 0;
  while (n < maxN && dacTail != dacHead) { out[n++] = dacBuf[dacTail]; dacTail = (dacTail + 1) & 2047; }
  return n;
}

uint16_t pcNow()    { return pc; }
uint32_t dacCount() { return dacWrites; }
uint32_t insCount() { return instructions; }

} // namespace psorom
