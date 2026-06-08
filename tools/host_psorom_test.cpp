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
  if (argc < 4) { printf("usage: host_psorom_test <s|b> <rom1> <rom2> [cmd]\n"
                         "  s = GTS80S  : rom1=6530sy80.bin rom2=game.snd\n"
                         "  b = 80B Gen3: rom1=yrom1.snd    rom2=drom1.snd\n"); return 1; }
  psorom::Board board = (argv[1][0]=='b') ? psorom::GTS80B_GEN3 : psorom::GTS80S;
  std::vector<uint8_t> r1 = load(argv[2]), r2 = load(argv[3]);
  if (r1.empty()) { printf("cannot read rom1 %s\n", argv[2]); return 1; }
  printf("board %s   rom1: %s (%zu B)   rom2: %s (%zu B)\n",
         board==psorom::GTS80B_GEN3?"80B-Gen3":"GTS80S", argv[2], r1.size(), argv[3], r2.size());

  if (!psorom::begin(board, r1.data(), r1.size(), r2.data(), r2.size())) { printf("psorom::begin failed\n"); return 1; }
  printf("reset vector -> PC=%04X\n", psorom::pcNow());

  psorom::run(100000);                                   // boot/idle
  printf("after boot : PC=%04X  ins=%u  dac=%u\n", psorom::pcNow(), psorom::insCount(), psorom::dacCount());

  uint8_t cmd = (argc >= 5) ? (uint8_t)strtol(argv[4], 0, 0) : 1;
  psorom::command(0x00); psorom::run(20000);             // prime (80B ignores the first byte after reset)
  psorom::command(cmd);
  for (int i = 0; i < 40; i++) psorom::run(50000);       // let the command play
  printf("after cmd %02X: ins=%u  dac=%u  ym=%u\n", cmd, psorom::insCount(), psorom::dacCount(), psorom::ymWrites());

  int16_t buf[64]; int d = psorom::dacDrain(buf, 64);
  printf("DAC drained %d samples; first:", d);
  for (int i = 0; i < d && i < 10; i++) printf(" %d", buf[i]);
  printf("\n%s\n", (psorom::dacCount() > 100) ? "PASS: real ROM runs + emits per-command DAC audio"
                  : (psorom::insCount() > 1000 ? "PARTIAL: executes but no DAC yet" : "FAIL: stalled"));
  return 0;
}
