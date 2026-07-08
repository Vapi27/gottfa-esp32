// host_render.cpp — rend l'audio de la VRAIE ROM (synthese AY/DAC/SP0250) dans un WAV, hors-ligne sur Mac.
// Sert a REGENERER des boucles musicales PROPRES (capture longue -> vraie periode -> decoupe seamless en Python).
// Build: gcc -c src/fake6502.c -o /tmp/f6502.o && gcc -c src/emu2149.c -o /tmp/emu2149.o &&
//        g++ -std=c++17 -Isrc tools/host_render.cpp src/psorom.cpp src/sp0250.cpp src/ym2151w.cpp src/ymfm_opm.cpp /tmp/f6502.o /tmp/emu2149.o -o /tmp/render
// Run:   /tmp/render <gen 1|2|b> <rom1> <rom2> <cmd> <secondes> <sortie.wav>
#include "psorom.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
static std::vector<uint8_t> load(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return {}; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); std::vector<uint8_t> b(n); if(fread(b.data(),1,n,f)!=(size_t)n) b.clear(); fclose(f); return b; }
static void wr32(FILE*f,uint32_t v){ fputc(v&255,f);fputc((v>>8)&255,f);fputc((v>>16)&255,f);fputc((v>>24)&255,f); }
static void wr16(FILE*f,uint16_t v){ fputc(v&255,f);fputc((v>>8)&255,f); }
int main(int argc,char**argv){
  if(argc<7){ printf("usage: %s <gen 1|2|b> <rom1> <rom2> <cmd> <sec> <out.wav>\n",argv[0]); return 2; }
  psorom::Board board=(argv[1][0]=='1')?psorom::GTS80B_GEN1:(argv[1][0]=='2')?psorom::GTS80B_GEN2:psorom::GTS80B_GEN3;
  auto r1=load(argv[2]), r2=load(argv[3]); if(r1.empty()){ printf("rom1 KO\n"); return 1; }
  int hdr=0, cmd; { const char* a=argv[4]; const char* dot=strchr(a,'.');
    if(dot){ hdr=(int)strtol(a,0,10); cmd=(int)strtol(dot+1,0,10); } else cmd=(int)strtol(a,0,0); } double sec=atof(argv[5]); const char* out=argv[6];
  if(!psorom::begin(board,r1.data(),r1.size(),r2.data(),r2.size())){ printf("begin KO\n"); return 1; }
  psorom::setSynth(true);                                    // synthese ON -> renderMix produit l'audio reel
  if(argc>7) psorom::setMixMask((uint8_t)strtol(argv[7],0,0));   // stems : bit0=DAC bit1=AY0 bit2=AY1 bit3=SP
  int fs=psorom::ayFs();                                     // 44100
  psorom::command(0x00); psorom::run(40000);                 // amorcage
  if(hdr){ psorom::command((uint8_t)hdr); psorom::run(300000); }
  psorom::command((uint8_t)cmd);
  uint32_t total=(uint32_t)(sec*fs), dataBytes=total*2;
  FILE* o=fopen(out,"wb"); if(!o){ printf("sortie KO\n"); return 1; }
  fwrite("RIFF",1,4,o); wr32(o,36+dataBytes); fwrite("WAVE",1,4,o);
  fwrite("fmt ",1,4,o); wr32(o,16); wr16(o,1); wr16(o,1); wr32(o,fs); wr32(o,fs*2); wr16(o,2); wr16(o,16);
  fwrite("data",1,4,o); wr32(o,dataBytes);
  int16_t buf[256]; uint32_t done=0;
  while(done<total){ int n=psorom::renderMix(buf,256); if(n<=0)break; if(done+n>total)n=total-done; fwrite(buf,2,n,o); done+=n; }
  fclose(o);
  printf("rendu cmd %d : %.1fs -> %s (%u ech)\n",cmd,sec,out,done);
  return 0;
}
