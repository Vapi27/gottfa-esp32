// psorom.cpp — see psorom.h. Stage-1 GTS80S sound-board model around the PD Fake6502 core.
// (C) 2026 Valere Pilpil / Pstore. Original implementation (CPU core = PD Fake6502).
#include "psorom.h"
#include "fake6502.h"
#include <string.h>

namespace psorom {

// ---- 64K address space of the sound 6502 -----------------------------------------------------
// GTS80S map (gts80s.c):  0x0000-0x01FF RAM · 0x0200-0x03FF 6530 RIOT (regs+timer, port A=DAC,
// port B=command in) · 0x0400-0x0FFF ROM · 0x1000-0x10FF RAM · 0xF800-0xFFFF ROM (reset vector).
static uint8_t mem[0x10000];
static bool    isRom[0x10000];                 // write-protect ROM regions

// ---- 6530 RIOT (minimal: ports + a loose timer + IRQ on command) ----
static uint8_t  riotB_in   = 0xFF;             // command latch (active-low style; set by command())
static bool     irqPending = false;
// DAC capture ring
static int16_t  dacBuf[1024];
static volatile int dacHead = 0, dacTail = 0;
static uint32_t dacWrites = 0;

static inline void dacPush(uint8_t data) {
  // PinMAME gts80s_riot6530_0a_w:  sample = ((data<<7) - 0x4000) * 2
  int16_t s = (int16_t)((((int16_t)data << 7) - 0x4000) * 2);
  int n = (dacHead + 1) & 1023;
  if (n != dacTail) { dacBuf[dacHead] = s; dacHead = n; }   // drop on overflow
  dacWrites++;
}

// 6530 register access within 0x0200-0x03FF (offset & 0x0F selects the register).
static uint8_t riotRead(uint16_t addr) {
  switch (addr & 0x0F) {
    case 0x02: return riotB_in;                // DRB = command latch (port B in)
    case 0x04: case 0x0C: return 0x00;         // timer read -> "expired" (loose: wait loops fall through)
    default:   return 0x00;
  }
}
static void riotWrite(uint16_t addr, uint8_t val) {
  switch (addr & 0x0F) {
    case 0x00: dacPush(val); break;            // DRA write = DAC sample
    default:   break;                          // DDR / timer / port B writes: ignored (stage 1)
  }
}

// ---- Fake6502 memory hooks (C linkage so the C core links to them) ----
extern "C" uint8_t read6502(uint16_t address) {
  if (address >= 0x0200 && address <= 0x03FF) return riotRead(address);
  return mem[address];
}
extern "C" void write6502(uint16_t address, uint8_t value) {
  if (address >= 0x0200 && address <= 0x03FF) { riotWrite(address, value); return; }
  if (!isRom[address]) mem[address] = value;
}

// ---- public API ----
bool begin(const uint8_t* rom, size_t romLen) {
  if (!rom || !romLen || romLen > 0x10000) return false;
  memset(mem, 0, sizeof(mem));
  memset(isRom, 0, sizeof(isRom));
  // Map the ROM at the top of memory so the reset vector (0xFFFC/D) lands in it. Mirror the image
  // down across the upper half too, so banked/aliased fetches resolve (GTS80S aliases the ROM).
  size_t base = 0x10000 - romLen;
  for (size_t i = 0; i < romLen; i++) { mem[base + i] = rom[i]; isRom[base + i] = true; }
  // mirror into 0x0400.. region as well (covers the 6530-ROM/code area for early fetches)
  for (size_t i = 0; i < romLen && (0x0400 + i) < 0x1000; i++) { mem[0x0400 + i] = rom[i]; isRom[0x0400 + i] = true; }
  reset();
  return true;
}

void reset() {
  dacHead = dacTail = 0; dacWrites = 0; irqPending = false; riotB_in = 0xFF;
  reset6502();
}

void command(uint8_t cmd) {
  riotB_in = cmd;                              // present the command on port B
  irq6502();                                   // pulse IRQ (gts80s_data_w pulses the CPU IRQ)
}

uint32_t run(uint32_t cycles) {
  uint32_t start = clockticks6502;
  exec6502(cycles);
  return clockticks6502 - start;
}

int dacDrain(int16_t* out, int maxN) {
  int n = 0;
  while (n < maxN && dacTail != dacHead) { out[n++] = dacBuf[dacTail]; dacTail = (dacTail + 1) & 1023; }
  return n;
}

uint16_t pcNow()     { return pc; }
uint32_t dacCount()  { return dacWrites; }
uint32_t insCount()  { return instructions; }

} // namespace psorom
