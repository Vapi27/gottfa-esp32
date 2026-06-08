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

static std::vector<uint8_t> load(const char* p) {
  std::vector<uint8_t> v; FILE* f = fopen(p, "rb"); if (!f) return v;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  v.resize(n); if ((long)fread(v.data(), 1, n, f) != n) v.clear(); fclose(f); return v;
}

int main(int argc, char** argv) {
  if (argc < 3) { printf("usage: host_psorom_test <6530sy80.bin> <game.snd> [cmd]\n"); return 1; }
  std::vector<uint8_t> code = load(argv[1]), data = load(argv[2]);
  if (code.empty()) { printf("cannot read code %s\n", argv[1]); return 1; }
  printf("code: %s (%zu B)   data: %s (%zu B)\n", argv[1], code.size(), argv[2], data.size());

  if (!psorom::begin(code.data(), code.size(), data.data(), data.size())) { printf("psorom::begin failed\n"); return 1; }
  printf("reset vector -> PC=%04X\n", psorom::pcNow());

  psorom::run(100000);                                   // boot/idle
  printf("after boot : PC=%04X  ins=%u  dac=%u\n", psorom::pcNow(), psorom::insCount(), psorom::dacCount());

  uint8_t cmd = (argc >= 4) ? (uint8_t)strtol(argv[3], 0, 0) : 1;
  psorom::command(cmd);
  for (int i = 0; i < 40; i++) psorom::run(50000);       // let the command play
  printf("after cmd %02X: PC=%04X  ins=%u  dac=%u\n", cmd, psorom::pcNow(), psorom::insCount(), psorom::dacCount());

  int16_t buf[64]; int d = psorom::dacDrain(buf, 64);
  printf("DAC drained %d samples; first:", d);
  for (int i = 0; i < d && i < 10; i++) printf(" %d", buf[i]);
  printf("\n%s\n", (psorom::dacCount() > 100) ? "PASS: real ROM runs + emits per-command DAC audio"
                  : (psorom::insCount() > 1000 ? "PARTIAL: executes but no DAC yet" : "FAIL: stalled"));
  return 0;
}
