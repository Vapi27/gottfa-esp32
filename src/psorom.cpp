// psorom.cpp — see psorom.h. Two boards around the PD Fake6502 core:
//   GTS80S      : 1×6502 + cycle-accurate 6530 RIOT + DAC (per gts80s.c / 6530riot.c).
//   GTS80B_GEN3 : 2×6502 (Y = YM2151, D = DAC) + cross-NMI; the global core is context-switched
//                 between the two CPUs at quantum boundaries (save/restore pc/sp/a/x/y/status).
// (C) 2026 Valere Pilpil / Pstore. Original implementation (CPU core = PD Fake6502).
#include "psorom.h"
#include "fake6502.h"
#include "emu2149.h"      // AY-3-8910/8913 (MIT, M. Okazaki) pour Gen1/Gen2
#include "sp0250.h"       // voix SP0250 (BSD-3, O. Galibert / MAME) pour Gen1
#include <string.h>
#include <stdlib.h>

namespace psorom {

static Board g_board = GTS80S;

// ---- shared DAC capture ring ----
static int16_t  dacBuf[4096];
static volatile int dacHead = 0, dacTail = 0;
static uint32_t dacWrites = 0;
static void dacPush(int16_t s) { int n=(dacHead+1)&4095; if(n!=dacTail){dacBuf[dacHead]=s;dacHead=n;} dacWrites++; }

// =====================================================================================
// GTS80S board (1×6502 + 6530 RIOT + DAC)
// =====================================================================================
static uint8_t  sRam0[0x40];          // 0x0000-0x01FF, 64B mirrored
static uint8_t  sRam1[0x100];         // 0x1000-0x10FF
static uint8_t* sRom = nullptr;       // 64K, ROM regions only
struct Riot { uint8_t out_a,ddr_a,out_b,ddr_b,in_a,in_b,timer_start,timer_irq_en,irq_state;
              uint16_t divider; uint32_t set_clk,uflow_clk; bool fired; } static rt;
static const uint8_t TIMERIRQ=0x80;

static void s_dac(uint8_t portA){ dacPush((int16_t)((((int16_t)portA<<7)-0x4000)*2)); }
static uint8_t riotRead(uint16_t a){ int o=a&0x0f;
  if(!(o&0x04)){ switch(o&3){case 0:return (rt.out_a&rt.ddr_a)|(rt.in_a&~rt.ddr_a);case 1:return rt.ddr_a;
                 case 2:return (rt.out_b&rt.ddr_b)|(rt.in_b&~rt.ddr_b);case 3:return rt.ddr_b;} }
  else if(!(o&1)){ int v; uint32_t el=clockticks6502-rt.set_clk;
    if(rt.fired) v=0xff-(int)((clockticks6502-rt.uflow_clk)&0xff);
    else { int d=(int)((el+rt.divider-1)/rt.divider); v=rt.timer_start-d; }
    if(v<0)v=0; rt.irq_state&=~TIMERIRQ; rt.timer_irq_en=o&0x08; return (uint8_t)v; }
  else return rt.irq_state;
  return 0; }
static void riotWrite(uint16_t a,uint8_t d){ int o=a&0x0f;
  if(!(o&0x04)){ switch(o&3){case 0:rt.out_a=d; if(rt.ddr_a)s_dac(rt.out_a&rt.ddr_a); break;
                 case 1:rt.ddr_a=d;break;case 2:rt.out_b=d;break;case 3:rt.ddr_b=d;break;} }
  else { rt.timer_irq_en=o&0x08; rt.timer_start=d;
    switch(o&3){case 0:rt.divider=1;break;case 1:rt.divider=8;break;case 2:rt.divider=64;break;case 3:rt.divider=1024;break;}
    rt.irq_state&=~TIMERIRQ; rt.set_clk=clockticks6502; rt.uflow_clk=clockticks6502+(uint32_t)rt.divider*rt.timer_start+1; rt.fired=false; } }
static uint8_t s_read(uint16_t a){ if(a<0x0200)return sRam0[a&0x3f]; if(a<=0x03ff)return riotRead(a);
  if(a>=0x1000&&a<=0x10ff)return sRam1[a&0xff]; return sRom[a]; }
static void s_write(uint16_t a,uint8_t v){ if(a<0x0200){sRam0[a&0x3f]=v;return;} if(a<=0x03ff){riotWrite(a,v);return;}
  if(a>=0x1000&&a<=0x10ff){sRam1[a&0xff]=v;return;}
  if((a>=0x0400&&a<=0x0fff)||(a>=0xfc00)) return;   // ROM (write-protected)
  sRom[a]=v; }

// =====================================================================================
// GTS80B Gen3 board (2×6502 + YM2151 + DAC)
// =====================================================================================
static uint8_t  yRam[0x800], dRam[0x800];
static uint8_t* yRom=nullptr;          // 0x8000-0xFFFF
static uint8_t* dRom=nullptr;
static uint8_t  soundlatch=0, nmi_rate=0, nmi_enable=0, dac_vol=0, dac_data=0, ym_port=0;
static uint32_t ymW=0, firstCmd=0, wallclk=0, nextYnmi=0xffffffff;
static bool     yIrq=false,dIrq=false,yNmi=false,dNmi=false;
static int      g_cpu=0;               // 0=Y, 1=D
struct Cpu { uint16_t pc; uint8_t sp,a,x,y,st; } static cY,cD;
static void saveCpu(Cpu&c){ c.pc=pc;c.sp=sp;c.a=a;c.x=x;c.y=y;c.st=status; }
static void loadCpu(Cpu&c){ pc=c.pc;sp=c.sp;a=c.a;x=c.x;y=c.y;status=c.st; }

static int g_gen=3;                                    // 1=AY+SP0250 (Gen1), 2=AY (Gen2), 3=YM2151 (Gen3)
static int16_t g_dacHeld=0;                            // dernier echantillon DAC (sample&hold) pour le mixeur
static inline void dacFromVolData(){ g_dacHeld=(int16_t)((int)dac_vol*(int)dac_data - 0x4000); dacPush(g_dacHeld); }

// ---- AY-3-8910/8913 via emu2149 (MIT). Gen1 = 2 puces (bit3 selectionne), Gen2 = 1. Horloge AY = 2 MHz. ----
static const uint32_t AY_CLK=2000000, AY_FS=32000;
static PSG*    g_psg[2] = {nullptr,nullptr};
static uint8_t ayLatch=0, ayAddr[2]={0,0}, ayCtlLast=0, spLatch=0;
static void ayControl(uint8_t d){          // s80bs1_sound_control_w : AY (Gen1 0x4000 / Gen2 0xa000) + SP0250 (Gen1)
  if((ayCtlLast & 0x04) && !(d & 0x04)){    // bit2 = BDIR, front descendant
    int chip = (d & 0x08) ? 0 : 1;          // bit3 : Gen1 a 2 puces, Gen2 = chip0
    if(d & 0x10) ayAddr[chip] = ayLatch & 0x0f;                          // bit4 = BC1=1 -> latch adresse registre
    else if(g_psg[chip]) PSG_writeReg(g_psg[chip], ayAddr[chip], ayLatch); // BC1=0 -> ecrit la data
  }
  if(g_gen==1 && (ayCtlLast & 0x40) && !(d & 0x40)) sp0250::feed(spLatch);  // bit6 front desc -> SP0250 data present
  ayCtlLast = d;
}
static uint8_t y_read(uint16_t a){ if(a<0x0800)return yRam[a];
  if(g_gen==1){ if(a==0xa800)return soundlatch; if(a==0xb000){dNmi=true;return 0;}   // GTS80BS1
                if(a==0x6000) return (uint8_t)(0x40 | (sp0250::ready()?0x80:0));     // sound_input: DIP(~d4&1)<<6 + bit7=DRQ SP0250
                if(a>=0xc000)return yRom[a]; return 0; }
  if(a==0x6800)return soundlatch;
  if(a==0x7000){ dNmi=true; return 0; }
  if(g_gen==2 && a==0x4000) return 0;                  // Gen2 sound_input (dips/test) -> 0
  if(a>=0x8000)return yRom[a]; return 0; }
static uint32_t g_yNmiCnt=0, g_ywHist[16]={0};   // DEBUG Gen1
static void y_write(uint16_t a,uint8_t d){ g_ywHist[(a>>12)&15]++; if(a<0x0800){yRam[a]=d;return;}
  if(g_gen==1){ if(a==0x2000){spLatch=d; ymW++; return;}           // Gen1: latch octet pour le SP0250
                if(a==0x4000){nmi_enable=d&1; ayControl(d); return;} // sound_control: nmi_en + strobe AY (+SP0250)
                if(a==0x8000){ayLatch=d; ymW++; return;}           // latch AY (data sur le bus)
                if(a==0xa000){nmi_rate=d;return;}
                if(a==0xb000){dNmi=true;return;} return; }
  if(g_gen==3 && a==0x4000){ ymW++; return; }          // Gen3 YM2151 (stubbed -> PSOWAV trigger)
  if(g_gen==2 && a==0x8000){ ayLatch=d; ymW++; return; }           // Gen2 AY latch (data sur le bus)
  if(a==0x6000){ nmi_rate=d; return; }
  if(a==0x7000){ dNmi=true; return; }
  if(a==0xa000){ nmi_enable=d&1; ym_port=(d&0x80)?1:0; if(g_gen==2) ayControl(d); return; }  // sound_control
}
static uint8_t d_read(uint16_t a){ if(a<0x0800)return dRam[a];
  if(g_gen==1){ if(a==0x8000)return soundlatch; if(a>=0xe000)return dRom[a]; return 0; }   // GTS80BS1
  if(a==0x4000)return soundlatch; if(a>=0x8000)return dRom[a]; return 0; }
static void d_write(uint16_t a,uint8_t d){ if(a<0x0800){dRam[a]=d;return;}
  if(g_gen==1){ if(a==0x4000){dac_vol=d;return;} if(a==0x4001){dac_data=d;dacFromVolData();return;} return; } // Gen1 DAC @4000/4001
  if(a==0x8000){ dac_vol=d; return; }                  // Gen2/3 DAC volume
  if(a==0x8001){ dac_data=d; dacFromVolData(); return; } // Gen2/3 DAC data -> sample
}
static uint32_t ynmiPeriod(){ int cl1=16-(nmi_rate&0x0f), cl2=16-((nmi_rate>>4)&0x0f);
  if(cl1<1)cl1=1; if(cl2<1)cl2=1; double hz=976.5625/((double)cl1*cl2); if(hz<1)hz=1;
  uint32_t p=(uint32_t)(1000000.0/hz); return p?p:1; }

// ---- Fake6502 hooks (dispatch on board + current CPU) ----
extern "C" uint8_t read6502(uint16_t a){ return (g_board==GTS80S)?s_read(a):(g_cpu==0?y_read(a):d_read(a)); }
extern "C" void write6502(uint16_t a,uint8_t v){ if(g_board==GTS80S)s_write(a,v); else if(g_cpu==0)y_write(a,v); else d_write(a,v); }

// =====================================================================================
// public API
// =====================================================================================
static size_t s_len=0;
bool begin(Board b, const uint8_t* rom1, size_t len1, const uint8_t* rom2, size_t len2) {
  g_board=b;
  if (b==GTS80S) {
    if(!rom1||!len1||len1>0x10000) return false;
    if(!sRom) sRom=(uint8_t*)malloc(0x10000); if(!sRom) return false;
    memset(sRom,0,0x10000); memset(sRam0,0,sizeof(sRam0)); memset(sRam1,0,sizeof(sRam1));
    for(size_t i=0;i<len1&&(0x0C00+i)<=0x0FFF;i++) sRom[0x0C00+i]=rom1[i];   // 6530 code @0x0C00
    size_t base=0x10000-len1; for(size_t i=0;i<len1;i++) sRom[base+i]=rom1[i];  // + top mirror (vectors)
    if(rom2&&len2) for(size_t i=0;i<len2&&(0x0400+i)<=0x0BFF;i++) sRom[0x0400+i]=rom2[i]&0x0f; // .snd 4-bit data
    for(int i=0;i<0x100;i++) sRam1[i]=sRom[0x0700+i];
  } else {
    g_gen = (b==GTS80B_GEN1) ? 1 : (b==GTS80B_GEN2) ? 2 : 3;
    int nay = (g_gen==1)?2:(g_gen==2)?1:0;             // Gen1=2 AY, Gen2=1 AY, Gen3=0 (YM2151)
    for(int i=0;i<2;i++){
      if(i<nay){ if(!g_psg[i]) g_psg[i]=PSG_new(AY_CLK, AY_FS);
                 PSG_setVolumeMode(g_psg[i],2); PSG_setQuality(g_psg[i],1); PSG_reset(g_psg[i]); }
      else if(g_psg[i]){ PSG_delete(g_psg[i]); g_psg[i]=nullptr; }
    }
    if(!rom1||!rom2||!len1||!len2) return false;
    if(!yRom) yRom=(uint8_t*)malloc(0x10000); if(!dRom) dRom=(uint8_t*)malloc(0x10000);
    if(!yRom||!dRom) return false;
    memset(yRom,0,0x10000); memset(dRom,0,0x10000); memset(yRam,0,sizeof(yRam)); memset(dRam,0,sizeof(dRam));
    size_t yb=0x10000-len1, db=0x10000-len2;            // map to top (vectors at 0xFFFx)
    for(size_t i=0;i<len1;i++) yRom[yb+i]=rom1[i];
    for(size_t i=0;i<len2;i++) dRom[db+i]=rom2[i];
  }
  reset();
  return true;
}

void reset() {
  dacHead=dacTail=0; dacWrites=0;
  if (g_board==GTS80S) { memset(&rt,0,sizeof(rt)); rt.divider=1024; rt.in_b=0x20; reset6502(); }
  else {
    soundlatch=0; nmi_rate=0; nmi_enable=0; dac_vol=0; dac_data=0; ymW=0; firstCmd=0;
    ayLatch=0; ayAddr[0]=ayAddr[1]=0; ayCtlLast=0; spLatch=0; g_dacHeld=0;
    for(int i=0;i<2;i++) if(g_psg[i]) PSG_reset(g_psg[i]);
    if(g_gen==1) sp0250::reset();
    wallclk=0; nextYnmi=0xffffffff; yIrq=dIrq=yNmi=dNmi=false;
    g_cpu=0; reset6502(); saveCpu(cY);
    g_cpu=1; reset6502(); saveCpu(cD);
  }
}

void command(uint8_t cmd) {
  if (g_board==GTS80S) { rt.in_b = 0x20 | (cmd & 0x0f); return; }
  if (!firstCmd) { firstCmd=1; return; }                // gts80b_data_w ignores the first byte
  uint8_t d = cmd ^ 0xff;                                // inverted from the MPU
  if (d != 0xff) { soundlatch=d; yIrq=true; dIrq=true; } // s80bs_sh_w (test1 path): latch + IRQ both
}

uint32_t run(uint32_t cycles) {
  uint32_t start=clockticks6502;
  if (g_board==GTS80S) {
    uint32_t target=start+cycles;
    while(clockticks6502<target){ step6502();
      if(!rt.fired && clockticks6502>=rt.uflow_clk){ rt.fired=true; rt.irq_state|=TIMERIRQ;
        if(rt.timer_irq_en && !(status&0x04)) irq6502(); } }
    return clockticks6502-start;
  }
  // 80B: alternate Y and D in quanta
  const uint32_t Q=64; uint32_t rounds=cycles/Q+1;
  for(uint32_t r=0;r<rounds;r++){
    g_cpu=0; loadCpu(cY);
    if(yNmi){ nmi6502(); yNmi=false; }
    if(yIrq && !(status&0x04)){ irq6502(); yIrq=false; }
    { uint32_t c0=clockticks6502; while(clockticks6502-c0<Q) step6502(); }
    saveCpu(cY);
    wallclk+=Q;
    if(nmi_enable){ if(nextYnmi==0xffffffff) nextYnmi=wallclk+ynmiPeriod();
                    if(wallclk>=nextYnmi){ yNmi=true; g_yNmiCnt++; nextYnmi=wallclk+ynmiPeriod(); } }
    g_cpu=1; loadCpu(cD);
    if(dNmi){ nmi6502(); dNmi=false; }
    if(dIrq && !(status&0x04)){ irq6502(); dIrq=false; }
    { uint32_t c0=clockticks6502; while(clockticks6502-c0<Q) step6502(); }
    saveCpu(cD);
  }
  return clockticks6502-start;
}

int dacDrain(int16_t* out,int maxN){ int n=0; while(n<maxN&&dacTail!=dacHead){out[n++]=dacBuf[dacTail];dacTail=(dacTail+1)&4095;} return n; }
uint16_t pcNow(){ return pc; }
uint32_t dacCount(){ return dacWrites; }
uint32_t insCount(){ return instructions; }
uint32_t ymWrites(){ return ymW; }
void dbgGen1(uint32_t* o){ o[0]=cY.pc; o[1]=cD.pc; o[2]=nmi_enable; o[3]=g_yNmiCnt; for(int i=0;i<16;i++)o[4+i]=g_ywHist[i]; } // DEBUG
// Rend n echantillons @ AY_FS = melange des AY actifs (etat courant). Pour validation host + futur mixeur.
int ayRender(int16_t* out, int n){
  for(int i=0;i<n;i++){ int32_t s=0;
    if(g_psg[0]) s+=PSG_calc(g_psg[0]);
    if(g_psg[1]) s+=PSG_calc(g_psg[1]);
    if(s>32767)s=32767; else if(s<-32768)s=-32768; out[i]=(int16_t)s; }
  return n;
}
int ayFs(){ return (int)AY_FS; }

// Mixeur a cadence fixe AY_FS : fait avancer l'emulateur (~1MHz wallclk) et produit n echantillons
// = DAC maintenu (sample&hold) + AY. Appele a la cadence AY_FS par la boucle -> temps-reel + mix.
int renderMix(int16_t* out, int n){
  static double cyc=0, spAcc=0; static int16_t spHeld=0;
  const double per = 1000000.0/(double)AY_FS;        // ~31.25 wallclk-units/echantillon
  const double spPer = 10000.0/(double)AY_FS;        // SP0250 : trame ~10 kHz -> sur-echantillonne a AY_FS
  for(int i=0;i<n;i++){
    cyc += per;
    while(cyc >= 64.0){ run(1); cyc -= 64.0; }       // run(1) = 1 quantum -> wallclk += 64
    int32_t s = g_dacHeld;
    if(g_psg[0]) s += PSG_calc(g_psg[0]);
    if(g_psg[1]) s += PSG_calc(g_psg[1]);
    if(g_gen==1){ spAcc += spPer; if(spAcc>=1.0){ spAcc-=1.0; spHeld=(int16_t)(sp0250::next()<<7); } s += spHeld; }
    s = (s * 3) >> 2;                                  // ~0.75 : headroom anti-saturation du mix DAC+AY+voix
    if(s>32767)s=32767; else if(s<-32768)s=-32768; out[i]=(int16_t)s;
  }
  return n;
}

} // namespace psorom
