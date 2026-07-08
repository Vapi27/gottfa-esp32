// host_psosig.cpp — V1 "chef d'orchestre hors-ligne" : fait tourner la VRAIE ROM (vitesse illimitee sur Mac)
// et extrait, par commande, le COMPORTEMENT son : loop vs one-shot, duree (one-shot), et trace d'activite.
// = la base des signatures psosig (la ROM dicte le timing, capture d'avance car la WROVER est trop lente live).
// Build: gcc -c src/fake6502.c -o /tmp/f6502.o && gcc -c src/emu2149.c -o /tmp/emu2149.o &&
//        g++ -std=c++17 -Isrc tools/host_psosig.cpp src/psorom.cpp src/sp0250.cpp src/ym2151w.cpp src/ymfm_opm.cpp /tmp/f6502.o /tmp/emu2149.o -o /tmp/sig
// Run:   /tmp/sig <gen 1|2|b> <rom1> <rom2> <cmd> [traceMs]
#include "psorom.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static std::vector<uint8_t> load(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return {}; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); std::vector<uint8_t> b(n); if(fread(b.data(),1,n,f)!=(size_t)n) b.clear(); fclose(f); return b; }
int main(int argc,char**argv){
  if(argc<5){ printf("usage: %s <gen 1|2|b> <rom1> <rom2> <cmd> [traceMs=4000]\n",argv[0]); return 2; }
  psorom::Board board = (argv[1][0]=='1')?psorom::GTS80B_GEN1 : (argv[1][0]=='2')?psorom::GTS80B_GEN2 : psorom::GTS80B_GEN3;
  int gen = (argv[1][0]=='1')?1:(argv[1][0]=='2')?2:3;
  auto r1=load(argv[2]), r2=load(argv[3]); if(r1.empty()){ printf("rom1 KO\n"); return 1; }
  int hdr=0, cmd; { const char* a=argv[4]; const char* dot=strchr(a,'.');
    if(dot){ hdr=(int)strtol(a,0,10); cmd=(int)strtol(dot+1,0,10); } else cmd=(int)strtol(a,0,0); } int traceMs = (argc>=6)?atoi(argv[5]):4000;
  if(!psorom::begin(board,r1.data(),r1.size(),r2.data(),r2.size())){ printf("begin KO\n"); return 1; }
  // amorcage standard (comme le firmware loadGame)
  psorom::command(0x00); psorom::run(40000);
  uint32_t base = psorom::activity();
  if(hdr){ psorom::command((uint8_t)hdr); psorom::run(300000); }
  psorom::command((uint8_t)cmd);
  const int SLICE_MS = 20;                         // resolution 20 ms
  uint32_t cycPerSlice = (uint32_t)((gen==1?2.0:4.0) * 1000.0 * SLICE_MS);  // cycles combines temps-reel / tranche
  int n = traceMs/SLICE_MS;
  uint32_t prev = psorom::activity();
  int flat=0, durMs=-1, lastActiveMs=0;
  printf("cmd %d (gen%d) trace %dms @%dms:\n", cmd, gen, traceMs, SLICE_MS);
  for(int s=0;s<n;s++){
    psorom::run(cycPerSlice);
    uint32_t a = psorom::activity();
    bool act = (a != prev); prev = a;
    if(act){ flat=0; lastActiveMs=(s+1)*SLICE_MS; } else { flat++; if(flat==8 && durMs<0) durMs=(s-7)*SLICE_MS; }
    if(s%5==0 || act) printf("  %4dms act-delta=%u %s\n", s*SLICE_MS, a-base, act?"###":"");
  }
  bool loop = (flat < 8);                           // activite encore vivante en fin de trace = boucle
  printf("=> cmd %d : %s ; dernier son @%dms ; durMs(one-shot)=%d\n", cmd, loop?"LOOP (ROM entretient)":"ONE-SHOT", lastActiveMs, durMs);
  return 0;
}
