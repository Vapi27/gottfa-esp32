// test_eps.cpp — stop PAR EPISODE : (1) musique 6 + stop 31 PENDANT que la partie continue (effets DAC
// en rafale -> jamais de silence global) => stopTag(6) doit tomber quand SES canaux meurent ;
// (2) tune 9 (pauses tonales internes 4.4 s) ne doit PAS etre coupe a tort pendant 20 s.
#include "psorom.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
static std::vector<uint8_t> load(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return {}; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); std::vector<uint8_t> b(n); if(fread(b.data(),1,n,f)!=(size_t)n) b.clear(); fclose(f); return b; }
struct Ep { int16_t id; uint32_t chan; int64_t tonFreeze; bool seen, fired; };
struct Chef {
  int64_t emu=0; uint32_t maskBefore=0; int64_t epStart=0;
  Ep eps[8]; int cur=-1; int stopTag=-1;                          // stopTag = dernier stop-par-son emis
  Chef(){ memset(eps,0,sizeof(eps)); }
  void cmd(int c){
    psorom::command((uint8_t)c);
    maskBefore=psorom::toneMask(); epStart=emu;
    cur=-1;
    for(int i=0;i<8;i++) if(eps[i].id==c && (eps[i].seen||!eps[i].fired)){ cur=i; break; }
    if(cur<0) for(int i=0;i<8;i++) if(eps[i].fired||eps[i].id==0){ cur=i; break; }
    if(cur<0){ int64_t old=INT64_MAX; for(int i=0;i<8;i++) if(eps[i].tonFreeze<old){ old=eps[i].tonFreeze; cur=i; } }
    eps[cur]=Ep{(int16_t)c,0,emu,false,false};
  }
  void step(){
    emu += psorom::run(4000);
    uint32_t mask = psorom::toneMask();
    const int64_t adoptCyc=1000000;                                // Gen1
    if (emu-epStart<adoptCyc && cur>=0) eps[cur].chan |= (mask & ~maskBefore);
    const int64_t silTon=12000000;                                 // 6 s emulees Gen1
    for(int i=0;i<8;i++){ Ep&e=eps[i]; if(!e.id||e.fired||!e.chan) continue;
      if(mask&e.chan){ e.tonFreeze=emu; e.seen=true; }
      else if(e.seen && emu-e.tonFreeze>silTon){ e.fired=true; stopTag=e.id; } }
  }
  double ms() const { return (double)emu/2000.0; }
};
static int fails=0;
#define CHECK(c,msg) do{ if(c) printf("  OK  %s\n",msg); else {printf("  FAIL %s\n",msg); fails++;} }while(0)
int main(int argc,char**argv){
  auto y=load(argv[1]), d=load(argv[2]);
  psorom::begin(psorom::GTS80B_GEN1,y.data(),y.size(),d.data(),d.size());
  psorom::command(0); psorom::run(20000);
  Chef ch;
  printf("1) musique 6 -> stop 31 -> la PARTIE CONTINUE (effet 3 toutes les 1.5 s)\n");
  ch.cmd(6); double t0=ch.ms();
  while (ch.ms()-t0<3000) ch.step();
  ch.cmd(31); double t31=ch.ms(), lastFx=ch.ms(), fired6=-1;
  while (ch.ms()-t31<14000){
    ch.step();
    if (ch.ms()-lastFx>1500){ lastFx=ch.ms(); ch.cmd(3); }         // le jeu enchaine les bruitages (DAC jamais muet)
    if (ch.stopTag==6 && fired6<0) fired6=ch.ms()-t31;
  }
  printf("     stopTag(6) @ +%.0f ms apres le 31 (partie toujours active)\n", fired6);
  CHECK(fired6>4000 && fired6<10000, "le son 6 est coupe PAR SES CANAUX, sans silence global");
  printf("2) tune 9 (pauses tonales 4.4 s) : PAS de stop a tort pendant 20 s\n");
  psorom::reset(); psorom::command(0); psorom::run(20000);
  Chef c2; c2.cmd(9); t0=c2.ms(); bool sp=false;
  while (c2.ms()-t0<20000){ c2.step(); if(c2.stopTag==9){ sp=true; break; } }
  CHECK(!sp, "tune 9 vivant (silTon 6 s > pause interne max 4.4 s)");
  printf(fails ? "\n>>> %d ECHEC(S)\n" : "\n>>> TOUT PASSE\n", fails);
  return fails?1:0;
}
