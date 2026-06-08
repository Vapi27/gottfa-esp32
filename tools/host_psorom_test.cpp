// host_psorom_test.cpp — Stage-1 PSOROM check: load a real Gottlieb sound ROM, run the vendored
// 6502 + GTS80S sound-board model, and confirm it EXECUTES the ROM (PC advances, instructions
// retire, DAC writes appear after a command). Not exact-audio yet — proves the core + map + ROM.
// Build & run on the container:
//   gcc -c src/fake6502.c -o /tmp/f6502.o
//   g++ -std=c++17 -Isrc tools/host_psorom_test.cpp src/psorom.cpp /tmp/f6502.o -o /tmp/pt && /tmp/pt <rom> [cmd]
#include "psorom.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: host_psorom_test <sound.rom> [cmd]\n"); return 1; }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> rom(n);
  if ((long)fread(rom.data(), 1, n, f) != n) { printf("read fail\n"); fclose(f); return 1; }
  fclose(f);
  printf("ROM: %s (%ld bytes)\n", argv[1], n);

  if (!psorom::begin(rom.data(), n)) { printf("psorom::begin failed\n"); return 1; }
  printf("reset vector -> PC=%04X\n", psorom::pcNow());

  psorom::run(100000);                                   // boot/idle
  printf("after boot : PC=%04X  ins=%u  dac=%u\n", psorom::pcNow(), psorom::insCount(), psorom::dacCount());

  uint8_t cmd = (argc >= 3) ? (uint8_t)strtol(argv[2], 0, 0) : 1;
  psorom::command(cmd);
  for (int i = 0; i < 40; i++) psorom::run(50000);       // let the command play
  printf("after cmd %02X: PC=%04X  ins=%u  dac=%u\n", cmd, psorom::pcNow(), psorom::insCount(), psorom::dacCount());

  int16_t buf[64]; int d = psorom::dacDrain(buf, 64);
  printf("DAC drained %d samples; first:", d);
  for (int i = 0; i < d && i < 10; i++) printf(" %d", buf[i]);
  printf("\n%s\n", (psorom::insCount() > 1000) ? "PASS: 6502 executes the real ROM" : "WEAK: few instructions retired");
  return 0;
}
