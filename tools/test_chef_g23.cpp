// test_chef_g23.cpp — noyau chef sur Gen2/Gen3 : (a) une boucle entretenue par la ROM ne recoit JAMAIS
// de stop ; (b) une commande MUETTE ne declenche AUCUN stop fantome (garde "evidence").
// Build: g++ -std=c++17 -Isrc /tmp/test_chef_g23.cpp src/psorom.cpp src/sp0250.cpp src/ym2151w.cpp src/ymfm_opm.cpp /tmp/f6502.o /tmp/emu2149.o -o /tmp/test_chef_g23
#include "psorom.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static std::vector<uint8_t> load(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return {}; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); std::vector<uint8_t> b(n); if(fread(b.data(),1,n,f)!=(size_t)n) b.clear(); fclose(f); return b; }
static int g_gen=2;
struct Chef {  // copie du noyau chefTask (constantes par gen)
  int64_t emu=0; uint32_t pDac=0, maskBefore=0, chanEp=0; int64_t epStart=0, dacFreeze=0, tonFreeze=0, gTonFreeze=0;
  bool dacSeen=false, tonSeen=false, firedFx=false, firedTon=false, armed=false, musWatch=false, stopFx=false, stopMus=false;
  void cmd(int c){ psorom::command((uint8_t)c); uint32_t d,t,s; psorom::activitySplit(&d,&t,&s);
    maskBefore=psorom::toneMask(); chanEp=0; epStart=emu; pDac=d; dacFreeze=tonFreeze=gTonFreeze=emu;
    dacSeen=tonSeen=firedFx=firedTon=stopFx=stopMus=false; armed=true; }
  void step(){ emu+=psorom::run(4000);
    uint32_t d,t,s; psorom::activitySplit(&d,&t,&s); uint32_t mask=psorom::toneMask();
    const int64_t adoptCyc=(g_gen==1)?1000000:2000000;
    if (emu-epStart<adoptCyc) chanEp|=(mask&~maskBefore);
    if (d!=pDac){ pDac=d; dacFreeze=emu; dacSeen=true; firedFx=false; }
    if (mask&chanEp){ tonFreeze=emu; tonSeen=true; firedTon=false; }
    if (mask){ gTonFreeze=emu; }
    if (armed){ const int64_t silCyc=(g_gen==1)?2000000:4000000;
      bool quiet=(emu-gTonFreeze>silCyc)&&(emu-dacFreeze>silCyc);
      if (quiet&&!firedFx){ stopFx=true; firedFx=true; }
      if (musWatch&&quiet&&!firedTon){ stopMus=true; firedTon=true; } } }
  double ms() const { return (double)emu/((g_gen==1)?2000.0:4000.0); }
};
static int fails=0;
#define CHECK(c,msg) do{ if(c) printf("  OK  %s\n",msg); else {printf("  FAIL %s\n",msg); fails++;} }while(0)
int main(int argc,char**argv){
  if(argc<5){ printf("usage: %s <yrom> <drom> <gen 2|3> <cmdLoop> [cmdMuet]\n",argv[0]); return 2; }
  g_gen=atoi(argv[3]); int cl=atoi(argv[4]); int cm=(argc>5)?atoi(argv[5]):16;
  auto y=load(argv[1]), d=load(argv[2]); if(y.empty()||d.empty()){ printf("ROMs KO\n"); return 2; }
  psorom::Board b=(g_gen==2)?psorom::GTS80B_GEN2:psorom::GTS80B_GEN3;
  if(!psorom::begin(b,y.data(),y.size(),d.data(),d.size())){ printf("begin KO\n"); return 2; }
  psorom::command(0); psorom::run(20000);
  Chef ch;
  printf("Gen%d — 1) cmd %d (boucle ROM) : aucun stop pendant 12 s\n", g_gen, cl);
  ch.cmd(cl); bool sp=false; double t0=ch.ms();
  while (ch.ms()-t0<12000){ ch.step(); if(ch.stopFx||ch.stopMus){ sp=true; break; } }
  CHECK(!sp, "boucle vivante (la ROM entretient -> pas de stop)");
  printf("Gen%d — 2) cmd %d (muet/stop) : le stop TIRE (no-op si rien ne joue) ~1 s apres\n", g_gen, cm);
  psorom::reset(); psorom::command(0); psorom::run(20000); ch = Chef{};   // etat ROM propre (le tune du test 1 jouait encore)
  ch.cmd(cm); sp=false; t0=ch.ms();
  while (ch.ms()-t0<6000){ ch.step(); if(ch.stopFx){ sp=true; break; } }
  CHECK(sp, "ROM silencieuse apres la commande -> stopFx tire (les codes stop type 31 marchent)");
  printf(fails ? ">>> %d ECHEC(S)\n" : ">>> TOUT PASSE\n", fails);
  return fails?1:0;
}
