// test_chef.cpp — rejoue la logique de DECISION du chefTask (gosowav_psorom.cpp) sur Mac, avec le
// VRAI emulateur + la VRAIE ROM Arena. Verifie : one-shot -> stopFx ~0.2 s apres la fin ROM ;
// boucle entretenue -> JAMAIS de stop ; commande suivante -> pas de stop parasite (episodes propres).
// Build: g++ -std=c++17 -Isrc /tmp/test_chef.cpp src/psorom.cpp src/sp0250.cpp src/ym2151w.cpp src/ymfm_opm.cpp /tmp/f6502.o /tmp/emu2149.o -o /tmp/test_chef
#include "psorom.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static std::vector<uint8_t> load(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return {}; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); std::vector<uint8_t> b(n); if(fread(b.data(),1,n,f)!=(size_t)n) b.clear(); fclose(f); return b; }

// ---- copie EXACTE du noyau de decision de chefTask (Gen1) ----
struct Chef {
  int64_t emu=0; uint32_t pDac=0, maskBefore=0, chanEp=0; int64_t epStart=0, dacFreeze=0, tonFreeze=0, gTonFreeze=0;
  bool dacSeen=false, tonSeen=false, firedFx=false, firedTon=false, armed=false, musWatch=false;
  bool stopFx=false, stopMus=false;
  void cmd(int c){
    psorom::command((uint8_t)c);
    uint32_t d,t,s; psorom::activitySplit(&d,&t,&s);
    maskBefore=psorom::toneMask(); chanEp=0; epStart=emu;
    pDac=d; dacFreeze=emu; tonFreeze=emu; gTonFreeze=emu;
    dacSeen=false; tonSeen=false; firedFx=false; firedTon=false;
    stopFx=false; stopMus=false; armed=true;
  }
  void step(){                                  // = 1 lot du chefTask
    emu += psorom::run(4000);
    uint32_t d,t,s; psorom::activitySplit(&d,&t,&s);
    uint32_t mask = psorom::toneMask();
    const int64_t adoptCyc=1000000;             // Gen1
    if (emu-epStart<adoptCyc) chanEp |= (mask & ~maskBefore);
    if (d!=pDac)     { pDac=d; dacFreeze=emu; dacSeen=true; firedFx=false; }
    if (mask&chanEp) {         tonFreeze=emu; tonSeen=true; firedTon=false; }
    if (mask)        {         gTonFreeze=emu; }                // tonal GLOBAL (toutes voix) : la traine DAC d'un
    if (armed){                                                 // ancien son ne doit pas tuer une musique vivante
      const int64_t silCyc=2000000;             // Gen1 ~1.0 s emulee (> pause interne max ~260 ms, cf gaps) — MIROIR du firmware
      bool quiet=(emu-gTonFreeze>silCyc)&&(emu-dacFreeze>silCyc);
      if (quiet&&!firedFx)                      { stopFx=true;  firedFx=true; }   // sans evidence : les codes STOP (31) rendent la ROM muette sans son
      if (musWatch&&quiet&&!firedTon)           { stopMus=true; firedTon=true; }
    }
  }
  double ms() const { return (double)emu/2000.0; }   // Gen1: 2.0 M cyc combines / s = 2000 cyc/ms
};

static int fails=0;
#define CHECK(c,msg) do{ if(c) printf("  OK  %s\n",msg); else {printf("  FAIL %s\n",msg); fails++;} }while(0)

int main(int argc,char**argv){
  const char* yp = (argc>1)?argv[1]:"/tmp/arena_y.snd";
  const char* dp = (argc>2)?argv[2]:0;
  auto y=load(yp), d=load(dp?dp:"/dev/null");
  if(y.empty()||d.empty()){ printf("ROMs manquantes\n"); return 2; }
  if(!psorom::begin(psorom::GTS80B_GEN1, y.data(), y.size(), d.data(), d.size())){ printf("begin KO\n"); return 2; }
  psorom::command(0); psorom::run(20000);                       // amorcage (comme loadGame)
  Chef ch;

  printf("1) cmd 3 (one-shot ROM ~840 ms) : stopFx doit tomber ~840+200 ms\n");
  ch.cmd(3); double fired=-1;
  while (ch.ms()<6000){ ch.step(); if(ch.stopFx){ fired=ch.ms(); break; } }
  printf("     stopFx @ %.0f ms emulees\n", fired);
  CHECK(fired>1700 && fired<3500, "stop dans la fenetre attendue (~1900 ms : 840 son + 1000 silence)");
  CHECK(!ch.stopMus, "pas de stopMus (pas de musique commandee)");

  printf("2) cmd 6 (boucle entretenue par la ROM) : AUCUN stop pendant 12 s\n");
  ch.cmd(6); bool spurious=false; double t0=ch.ms();
  while (ch.ms()-t0<12000){ ch.step(); if(ch.stopFx||ch.stopMus){ spurious=true; break; } }
  CHECK(!spurious, "la boucle vit tant que la ROM l'entretient");

  printf("3) musique surveillee (musWatch) : pas de stopMus tant que la ROM joue\n");
  ch.cmd(6); ch.musWatch=true; spurious=false; t0=ch.ms();
  while (ch.ms()-t0<8000){ ch.step(); if(ch.stopMus){ spurious=true; break; } }
  CHECK(!spurious, "stopMus ne tombe pas pendant que la musique ROM tourne");

  printf("4) episodes successifs : cmd 3 puis cmd 6 -> le one-shot fini ne tue pas la boucle\n");
  ch.musWatch=false; ch.cmd(3); t0=ch.ms();
  while (ch.ms()-t0<400){ ch.step(); }                          // 3 joue encore (one-shot 840 ms)
  ch.cmd(6);                                                    // nouvelle commande pendant que 3 joue
  spurious=false; t0=ch.ms();
  while (ch.ms()-t0<12000){ ch.step(); if(ch.stopFx||ch.stopMus){ spurious=true; break; } }
  CHECK(!spurious, "episode 6 propre : pas de stop herite de l'episode 3");

  printf("5) commande STOP (31) pendant la musique 6 : stopMus/stopFx doivent tomber ~1 s apres\n");
  ch.cmd(6); ch.musWatch=true; t0=ch.ms();
  while (ch.ms()-t0<3000){ ch.step(); }                         // la musique ROM tourne
  ch.cmd(31); ch.musWatch=true; double f31=-1; t0=ch.ms();      // 31 = code stop (la ROM se tait, AUCUN son produit)
  while (ch.ms()-t0<5000){ ch.step(); if(ch.stopMus){ f31=ch.ms()-t0; break; } }
  printf("     stopMus @ +%.0f ms apres le 31\n", f31);
  CHECK(f31>800 && f31<2600, "le 31 coupe la musique (silence ROM ~1 s -> stop, sans evidence)");
  printf(fails ? "\n>>> %d ECHEC(S)\n" : "\n>>> TOUT PASSE\n", fails);
  return fails?1:0;
}
