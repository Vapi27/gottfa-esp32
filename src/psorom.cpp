// psorom.cpp — see psorom.h. Two boards around the PD Fake6502 core:
//   GTS80S      : 1×6502 + cycle-accurate 6530 RIOT + DAC (per gts80s.c / 6530riot.c).
//   GTS80B_GEN3 : 2×6502 (Y = YM2151, D = DAC) + cross-NMI; the global core is context-switched
//                 between the two CPUs at quantum boundaries (save/restore pc/sp/a/x/y/status).
// (C) 2026 Valere Pilpil / Pstore. Original implementation (CPU core = PD Fake6502).
#include "psorom.h"
#include "fake6502.h"
#include "emu2149.h"      // AY-3-8910/8913 (MIT, M. Okazaki) pour Gen1/Gen2
#include "sp0250.h"       // voix SP0250 (BSD-3, O. Galibert / MAME) pour Gen1
#include "ym2151w.h"      // YM2151 (BSD-3, ymfm / A. Giles) pour Gen3
#include <string.h>
#include <stdlib.h>
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#endif

#pragma GCC optimize("O3")   /* chemin chaud temps-reel : vitesse > taille (PlatformIO compile en -Os) */

namespace psorom {

static Board g_board = GTS80S;

// ---- shared DAC capture ring ----
static int16_t  dacBuf[4096];
static volatile int dacHead = 0, dacTail = 0;
static uint32_t dacWrites = 0;
static int16_t  dacPrev = 0; static uint32_t dacChanges = 0;     // variation reelle du DAC (= son digitalise, pas silence fige)
static IRAM_ATTR void dacPush(int16_t s) { int n=(dacHead+1)&4095; if(n!=dacTail){dacBuf[dacHead]=s;dacHead=n;} dacWrites++;
  int d = (int)s - (int)dacPrev; if (d < 0) d = -d; if (d > 256) dacChanges++; dacPrev = s; }

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
static uint8_t g_dip = 0x40;                           // DIP carte son (Gen1 bit6 : son d'attract !) — defaut historique
// ===== PSOLIVE : emulation COMPLETE en direct (Gen1). Le coeur 0 fait tourner les 6502 et pousse
// chaque ecriture de puce dans un ring HORODATE (wallclk) ; le coeur 1 (liveRender) applique les
// evenements a l'echantillon pres et synthetise. Mixage exact par construction — zero heuristique. =====
struct LiveEv { uint32_t t; uint8_t ty, a, b; };     // ty: 0=AY0 1=AY1 2=SP0250 3=DAC(val16=a|b<<8)
static uint32_t s_levDrop = 0;
static LiveEv  s_levBuf[1024];                       // DRAM : la PSRAM creait une CONTENTION inter-coeurs aux debits DAC eleves
static LiveEv* s_lev = s_levBuf;                     // 1024 evts = ~45 ms a 22 kHz ; chef v2 draine toutes les ~4 ms (meme coeur),
#define LEVMASK 1023                                 // @live (experimental) toutes les ~3 ms : marge x10
static volatile uint16_t s_levH = 0, s_levT = 0;     // SPSC : producteur coeur 0 (run), consommateur coeur 1
static volatile bool s_liveEv = false, s_liveRst = false;
static inline IRAM_ATTR void levPush(uint8_t ty, uint8_t a, uint8_t b) {
  uint16_t n = (uint16_t)((s_levH + 1) & LEVMASK);
  if (n == s_levT) { s_levDrop++; return; }          // plein -> on jette (ne JAMAIS bloquer le CPU) mais on COMPTE
  s_lev[s_levH] = LiveEv{ wallclk, ty, a, b }; s_levH = n;
}
void liveEvents(bool on) { s_liveEv = on; s_levH = s_levT = 0; s_liveRst = true; s_levDrop = 0; }   // metrique propre (le bench du boot inonde le ring)
uint32_t levDrops() { return s_levDrop; }
uint32_t clockNow() { return wallclk; }
void setDip(uint8_t d) { g_dip = d; }
int liveDrain(Ev* out, int maxN) {                   // consommation BRUTE (exclusif avec liveRender)
  int n = 0;
  while (n < maxN && s_levT != s_levH) {
    const LiveEv& e = s_lev[s_levT];
    out[n].t = e.t; out[n].ty = e.ty; out[n].a = e.a; out[n].b = e.b; n++;
    s_levT = (uint16_t)((s_levT + 1) & LEVMASK);
  }
  return n;
}
static bool     g_noSynth = true;            // true = chef d'orchestre (suivi seul, RAPIDE) ; false = synthese (renderMix)
static int16_t g_dacHeld=0;                            // dernier echantillon DAC (sample&hold) pour le mixeur
static uint32_t g_lastDacEv=0;
static inline IRAM_ATTR void dacFromVolData(){ g_dacHeld=(int16_t)((int)dac_vol*(int)dac_data - 0x4000);
  if(!g_noSynth) dacPush(g_dacHeld); else dacWrites++;   // conducteur : compteur seul (l'anneau ne sert qu'a renderMix)
  if(s_liveEv){ if(g_noSynth){                           // chef v2 : le DAC n'est qu'une ACTIVITE -> 1 evenement/ms suffit
                  if((uint32_t)(wallclk-g_lastDacEv)>=1000){ g_lastDacEv=wallclk; levPush(3,(uint8_t)(g_dacHeld&0xFF),(uint8_t)((uint16_t)g_dacHeld>>8)); } }
                else levPush(3,(uint8_t)(g_dacHeld&0xFF),(uint8_t)((uint16_t)g_dacHeld>>8)); } }   // @live : tout (synthese exacte)

// ---- AY-3-8910/8913 via emu2149 (MIT). Gen1 = 2 puces (bit3 selectionne), Gen2 = 1. Horloge AY = 2 MHz. ----
static const uint32_t AY_CLK=2000000, AY_FS=44100;   // 44.1kHz : aligne le mix sur les WAV PSOWAV (pitch inchange: per=1e6/Fs)
static PSG*    g_psg[2] = {nullptr,nullptr};
static uint8_t ayLatch=0, ayAddr[2]={0,0}, ayCtlLast=0, spLatch=0;
// --- CHEF D'ORCHESTRE : run() SUIT les ecritures registres (pour detecter l'activite) SANS synthetiser.
//     La synthese (PSG_writeReg/sp0250/ym) est le gros poids ; on la coupe -> run() retrouve sa vitesse. ---
static uint8_t  g_ayReg[2][16] = {{0}};      // registres AY suivis (volumes reg8..10, mixer reg7) — lecture pure
static uint32_t g_spFeeds = 0;               // nb d'octets nourris au SP0250 (Gen1 parole) — change tant que ca parle
static uint8_t  g_ymKeyMask = 0, g_ymPendAddr = 0;  // YM2151 (Gen3) : masque des voix key-on courantes

static IRAM_ATTR void ayControl(uint8_t d){          // s80bs1_sound_control_w : AY (Gen1 0x4000 / Gen2 0xa000) + SP0250 (Gen1)
  if((ayCtlLast & 0x04) && !(d & 0x04)){    // bit2 = BDIR, front descendant
    int chip = (d & 0x08) ? 0 : 1;          // bit3 : Gen1 a 2 puces, Gen2 = chip0
    if(d & 0x10) ayAddr[chip] = ayLatch & 0x0f;                          // bit4 = BC1=1 -> latch adresse registre
    else { g_ayReg[chip][ayAddr[chip]] = ayLatch;                        // suivi registre (pas cher)
           if(s_liveEv) levPush(chip ? 1 : 0, ayAddr[chip], ayLatch);        // live : evenement horodate
           if(!g_noSynth && g_psg[chip]) PSG_writeReg(g_psg[chip], ayAddr[chip], ayLatch); } // synthese seulement si demandee
  }
  if(g_gen==1 && (ayCtlLast & 0x40) && !(d & 0x40)){ g_spFeeds++;
    if(s_liveEv) levPush(2, spLatch, 0);
    if(!g_noSynth) sp0250::feed(spLatch); } // bit6 front desc -> SP0250
  ayCtlLast = d;
}
static IRAM_ATTR uint8_t y_read(uint16_t a){ if(a<0x0800)return yRam[a];
  if(g_gen==1){ if(a==0xa800)return soundlatch; if(a==0xb000){dNmi=true;return 0;}   // GTS80BS1
                if(a==0x6000) return (uint8_t)(g_dip | (sp0250::ready()?0x80:0));    // sound_input: DIP (reglable !) + bit7=DRQ SP0250
                if(a>=0xc000)return yRom[a]; return 0; }
  if(a==0x6800)return soundlatch;
  if(a==0x7000){ dNmi=true; return 0; }
  if(g_gen==2 && a==0x4000) return 0;                  // Gen2 sound_input (dips/test) -> 0
  if(a>=0x8000)return yRom[a]; return 0xFF; }          // non mappe = bus OUVERT (0xFF, comme MAME) :
                                                       // hotshots Y poll un statut non mappe avant de pomper la parole
static uint32_t g_yNmiCnt=0, g_dNmiCnt=0, g_ywHist[16]={0};   // DEBUG
static IRAM_ATTR void y_write(uint16_t a,uint8_t d){ g_ywHist[(a>>12)&15]++; if(a<0x0800){yRam[a]=d;return;}
  if(g_gen==1){ if(a==0x2000){spLatch=d; ymW++; return;}           // Gen1: latch octet pour le SP0250
                if(a==0x4000){nmi_enable=d&1; ayControl(d); return;} // sound_control: nmi_en + strobe AY (+SP0250)
                if(a==0x8000){ayLatch=d; ymW++; return;}           // latch AY (data sur le bus)
                if(a==0xa000){nmi_rate=d;return;}
                if(a==0xb000){dNmi=true;return;} return; }
  if(g_gen==3 && a==0x4000){                                       // Gen3 YM2151 (port 0=addr,1=data via sound_control bit7)
    if(ym_port==0) g_ymPendAddr=d;                                 // suivi key-on : registre 0x08 = KON (voix + slots)
    else if(g_ymPendAddr==0x08){ uint8_t ch=d&7; if(d&0x78) g_ymKeyMask|=(1<<ch); else g_ymKeyMask&=~(1<<ch);
                                 if(s_liveEv) levPush(4, 0x08, d); }   // evenement chef v2 : key-on/off EXACT (l'equivalent YM du vol AY)
    if(!g_noSynth) ym2151w::write(ym_port, d); ymW++; return; }
  if(g_gen==2 && a==0x8000){ ayLatch=d; ymW++; return; }           // Gen2 AY latch (data sur le bus)
  if(a==0x6000){ nmi_rate=d; return; }
  if(a==0x7000){ dNmi=true; return; }
  if(a==0xa000){ nmi_enable=d&1; ym_port=(d&0x80)?1:0; if(g_gen==2) ayControl(d); return; }  // sound_control
}
static IRAM_ATTR uint8_t d_read(uint16_t a){ if(a<0x0800)return dRam[a];
  if(g_gen==1){ if(a==0x8000)return soundlatch; if(a>=0xe000)return dRom[a]; return 0; }   // GTS80BS1
  if(a==0x4000)return soundlatch; if(a>=0x8000)return dRom[a]; return 0; }
static IRAM_ATTR void d_write(uint16_t a,uint8_t d){ if(a<0x0800){dRam[a]=d;return;}
  if(g_gen==1){ if(a==0x4000){dac_vol=d;dacFromVolData();return;} if(a==0x4001){dac_data=d;dacFromVolData();return;} return; } // Gen1 DAC @4000/4001 (vol SORT aussi, cf. PinMAME)
  if(a==0x8000){ dac_vol=d; dacFromVolData(); return; }  // Gen2/3 DAC volume : sort AUSSI un echantillon (vol*data,
                                                         // comme PinMAME) — la parole hotshots streame PAR le volume !
  if(a==0x8001){ dac_data=d; dacFromVolData(); return; } // Gen2/3 DAC data -> sample
}
static uint32_t s_ynmiCache = 0; static uint8_t s_ynmiRate = 0xFF;
static IRAM_ATTR uint32_t ynmiPeriod(){                      // CACHE : l'ancien calcul refaisait des divisions
  if (nmi_rate == s_ynmiRate && s_ynmiCache) return s_ynmiCache;   // DOUBLE (logicielles !) a CHAQUE quantum
  int cl1=16-(nmi_rate&0x0f), cl2=16-((nmi_rate>>4)&0x0f);
  if(cl1<1)cl1=1; if(cl2<1)cl2=1; double hz=976.5625/((double)cl1*cl2); if(hz<1)hz=1;
  uint32_t p=(uint32_t)((g_gen==1?1.0:2.0)*1000000.0/hz);
  s_ynmiRate = nmi_rate; s_ynmiCache = p?p:1; return s_ynmiCache; }  // wallclk = cycles CPU : Gen1 1MHz, Gen2/3 2MHz

// ---- Fake6502 hooks (dispatch on board + current CPU) ----
extern "C" IRAM_ATTR uint8_t read6502(uint16_t a){ return (g_board==GTS80S)?s_read(a):(g_cpu==0?y_read(a):d_read(a)); }
extern "C" IRAM_ATTR void write6502(uint16_t a,uint8_t v){ if(g_board==GTS80S)s_write(a,v); else if(g_cpu==0)y_write(a,v); else d_write(a,v); }

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
                 PSG_setVolumeMode(g_psg[i],2); PSG_setQuality(g_psg[i],0); PSG_reset(g_psg[i]); }   // qualite 0 : ~2x moins cher (le live a besoin de marge coeur 1)
      else if(g_psg[i]){ PSG_delete(g_psg[i]); g_psg[i]=nullptr; }
    }
    if(g_gen==3) ym2151w::begin();                     // demarre la tache YM sur le coeur 0 (perf)
    if(!rom1||!rom2||!len1||!len2) return false;
    if(len1>0x10000||len2>0x10000) return false;       // ROM > 64K : 0x10000-len deborderait (ecriture sauvage) -> refus propre
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
    soundlatch=0; nmi_rate=0; nmi_enable=0; dac_vol=0; dac_data=0; ymW=0; firstCmd=0; ym_port=0;
    ayLatch=0; ayAddr[0]=ayAddr[1]=0; ayCtlLast=0; spLatch=0; g_dacHeld=0;
    memset(g_ayReg,0,sizeof(g_ayReg)); g_ymKeyMask=0; g_ymPendAddr=0; g_spFeeds=0;  // suivi chef : SANS ce reset, les volumes
    dacPrev=0; dacChanges=0;                          // AY d'un jeu precedent restent "sonores" -> la detection de fin est MORTE apres un changement de jeu
    for(int i=0;i<2;i++) if(g_psg[i]) PSG_reset(g_psg[i]);
    if(g_gen==1) sp0250::reset();
    if(g_gen==3) ym2151w::reset();
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
                                                           // (PAS de retombee du latch : le ROAR relit le port en jouant
                                                           //  et s'avorte si la commande a disparu — bug du 12/06 !)
}

IRAM_ATTR uint32_t run(uint32_t cycles) {
  uint32_t start=clockticks6502;
  if (g_board==GTS80S) {
    while(clockticks6502-start<cycles){ step6502();                 // compare par soustraction -> sain au wrap uint32
      if(!rt.fired && clockticks6502>=rt.uflow_clk){ rt.fired=true; rt.irq_state|=TIMERIRQ;
        if(rt.timer_irq_en && !(status&0x04)) irq6502(); } }
    return clockticks6502-start;
  }
  // 80B: alternate Y and D in quanta
  const uint32_t Q=128; uint32_t rounds=cycles/Q+1;       // 128 : moitie moins de save/load contexte (couplage inter-CPU OK a 128 us)
  for(uint32_t r=0;r<rounds;r++){
    g_cpu=0; loadCpu(cY);
    if(yNmi){ nmi6502(); yNmi=false; }
    if(yIrq && !(status&0x04)){ irq6502(); yIrq=false; }
    { uint32_t c0=clockticks6502; while(clockticks6502-c0<Q) step6502();
      wallclk += clockticks6502 - c0; }                  // credite les cycles REELS (l'arrondi a Q perdait ~2,3 % de temps -> deficit de synthese)
    saveCpu(cY);
    if(nmi_enable){ if(nextYnmi==0xffffffff) nextYnmi=wallclk+ynmiPeriod();
                    if((int32_t)(wallclk-nextYnmi)>=0){ yNmi=true; g_yNmiCnt++; nextYnmi=wallclk+ynmiPeriod(); } }   // modulaire (wrap wallclk)
    g_cpu=1; loadCpu(cD);
    if(dNmi){ nmi6502(); dNmi=false; g_dNmiCnt++; }
    if(dIrq && !(status&0x04)){ irq6502(); dIrq=false; }
    { uint32_t c0=clockticks6502; while(clockticks6502-c0<Q) step6502(); }
    saveCpu(cD);
  }
  return clockticks6502-start;
}

int dacDrain(int16_t* out,int maxN){ int n=0; while(n<maxN&&dacTail!=dacHead){out[n++]=dacBuf[dacTail];dacTail=(dacTail+1)&4095;} return n; }
uint16_t pcNow(){ return pc; }
void dbgNmi(uint32_t* y, uint32_t* d, uint8_t* en, uint8_t* rate){   // debug : etat de la chaine NMI (Y timer -> D pompe DAC)
  if(y)*y=g_yNmiCnt; if(d)*d=g_dNmiCnt; if(en)*en=nmi_enable; if(rate)*rate=nmi_rate; }
void dbgPc(uint16_t* ypc, uint16_t* dpc){ if(ypc)*ypc=cY.pc; if(dpc)*dpc=cD.pc; }   // debug : PC des deux CPU
uint8_t dbgYRam(uint16_t a){ return (a<0x800)?yRam[a]:0; }                          // debug : RAM Y
uint32_t dacCount(){ return dacWrites; }
uint32_t insCount(){ return instructions; }
uint32_t ymWrites(){ return ymW; }
// ROM-chef : "activite SON" du CPU (pas juste les ecritures, qui montent toujours). Le firmware
// echantillonne le delta : s'il FIGE -> le CPU a coupe le son -> le chef arrete la boucle WAV.
//   Gen1/Gen2 : DAC qui VARIE (son digitalise) + canaux AY qui sonnent (volume/enveloppe != 0).
//   Le CPU met les volumes AY a 0 quand il finit un son -> le signal fige -> stop. C'est LUI qui pilote.
static uint32_t s_ayTick = 0;
uint32_t activity(){              // "activite SON" : change tant que ca sonne, FIGE au silence -> le chef arrete la boucle
  bool snd = (g_ymKeyMask != 0);                                  // Gen3 : au moins une voix YM keyee
  for(int c=0;c<2 && !snd;c++)                                    // Gen1/2 : un canal AY a du volume (registres suivis)
    if((g_ayReg[c][8]&0x1f)||(g_ayReg[c][9]&0x1f)||(g_ayReg[c][10]&0x1f)) snd=true;
  if(snd) s_ayTick++;
  return dacChanges + s_ayTick + g_spFeeds;                       // + DAC qui varie (digit.) + SP0250 nourri (parole)
}
// Idem mais SEPARE par sous-systeme : le chef peut alors arreter la MUSIQUE (AY/YM) et les EFFETS (DAC)
// independamment (chacun fige quand le CPU coupe ce sous-systeme la). Avance s_ayTick comme activity().
void activitySplit(uint32_t* dac, uint32_t* tone, uint32_t* sp){
  bool snd = (g_ymKeyMask != 0);
  for(int c=0;c<2 && !snd;c++)
    if((g_ayReg[c][8]&0x1f)||(g_ayReg[c][9]&0x1f)||(g_ayReg[c][10]&0x1f)) snd=true;
  if(snd) s_ayTick++;
  if(dac)  *dac  = dacChanges;     // samples digitalises (DAC) : varie tant qu'un sample joue
  if(tone) *tone = s_ayTick;       // musique tonale (AY Gen1/2 + YM2151 Gen3) : monte tant qu'un canal sonne
  if(sp)   *sp   = g_spFeeds;      // voix (SP0250 Gen1) : monte tant que ca parle
}
#if defined(ESP_PLATFORM)
extern "C" int64_t esp_timer_get_time(void);
#endif
int synthBench(int ms){                                          // ech/s que la SYNTHESE seule peut produire (AYx2+SP+mix)
  if(!g_psg[0]) return -1;
  uint32_t n=0; volatile int32_t sink=0;
#if defined(ESP_PLATFORM)
  int64_t t0=esp_timer_get_time();
  while(esp_timer_get_time()-t0 < (int64_t)ms*1000){
#else
  for(int k=0;k<ms*50;k++){
#endif
    for(int i=0;i<64;i++){ int32_t s=0;
      s += PSG_calc(g_psg[0]); if(g_psg[1]) s += PSG_calc(g_psg[1]);
      s += sp0250::next(); sink += s; }
    n += 64;
  }
  (void)sink;
  return (int)((uint64_t)n*1000/(ms?ms:1));
}
void setSynth(bool on){ g_noSynth = !on; }                        // on -> renderMix synthetise (extraction hors-ligne)
static uint8_t g_mixMask = 0x0F;                                  // stems (host) : bit0=DAC bit1=AY0 bit2=AY1 bit3=SP/YM
void setMixMask(uint8_t m){ g_mixMask = m; }
// ROM-chef : masque des canaux tonals QUI SONNENT (bit par canal). AY: vol!=0 ET (tone OU noise actif
// dans le mixer reg7, bits actifs a 0). Gen1/2 = 6 bits AY (2 puces x 3 canaux), Gen3 = 8 bits YM (<<6).
// Permet de distinguer LES canaux d'un effet de ceux de la musique deja en cours (episode par canal).
uint32_t toneMask(){ uint32_t m=0;
  for(int c=0;c<2;c++){ uint8_t r7=g_ayReg[c][7];
    for(int ch=0;ch<3;ch++)
      if((g_ayReg[c][8+ch]&0x1f) && (!(r7&(1u<<ch)) || !(r7&(1u<<(ch+3))))) m|=1u<<(c*3+ch); }
  m |= (uint32_t)g_ymKeyMask<<6;
  return m; }
void dbgGen1(uint32_t* o){ o[0]=cY.pc; o[1]=cD.pc; o[2]=nmi_enable; o[3]=g_yNmiCnt; for(int i=0;i<16;i++)o[4+i]=g_ywHist[i]; } // DEBUG
// Rend n echantillons @ AY_FS = melange des AY actifs (etat courant). Pour validation host + futur mixeur.
int ayRender(int16_t* out, int n){
  for(int i=0;i<n;i++){ int32_t s=0;
    if(g_psg[0]) s+=PSG_calc(g_psg[0]);
    if(g_psg[1]) s+=PSG_calc(g_psg[1]);
    if(s>32767)s=32767; else if(s<-32768)s=-32768; out[i]=(int16_t)s; }
  return n;
}
uint32_t stateHash() {                                   // empreinte exacte de l'etat emule (verif d'equivalence du coeur rapide)
  // (les registres 6502 sont des symboles C globaux du fake6502)
  uint32_t h = 2166136261u;
  #define H(v) h = (h ^ (uint32_t)(v)) * 16777619u
  H(pc); H(sp); H(a); H(x); H(y); H(status); H(clockticks6502); H(wallclk);
  H(cY.pc); H(cY.sp); H(cY.a); H(cY.x); H(cY.y); H(cY.st);
  H(cD.pc); H(cD.sp); H(cD.a); H(cD.x); H(cD.y); H(cD.st);
  for (int i = 0; i < 0x800; i++) { H(yRam[i]); H(dRam[i]); }
  H(soundlatch); H(nmi_rate); H(dac_vol); H(dac_data); H(ayLatch); H(ayCtlLast); H(spLatch);
  #undef H
  return h;
}
int ayFs(){ return (int)AY_FS; }

// Coeur 1 : applique les evenements murs (jusqu'a wallclk - marge) et synthetise a AY_FS.
// Le mixage est EXACT : memes niveaux que renderMix (DAC ref, AY x13.65, SP x2), mais le temps
// vient des evenements du VRAI programme — un canal vole est vole, un son coupe est coupe.
int liveRender(int16_t* out, int n) {
  static double cyc = 0, spAcc = 0; static int16_t held = 0, spHeld = 0;
  static uint32_t synthClk = 0;
  if (s_liveRst) { s_liveRst = false; cyc = spAcc = 0; held = spHeld = 0; synthClk = wallclk; }
  const double per   = 1000000.0 / (double)AY_FS;    // Gen1 : wallclk = 1 MHz
  const double spPer = 10000.0 / (double)AY_FS;
  uint32_t safe = wallclk - 2000;                    // marge 2 ms emulees (des evenements peuvent encore arriver avant)
  int done = 0;
  while (done < n) {
    if ((int32_t)(safe - synthClk) < 64) break;      // on ne depasse JAMAIS la marge (sain au wrap : diff signee)
    cyc += per; uint32_t step = (uint32_t)cyc; cyc -= (double)step;
    uint32_t tgt = synthClk + step;
    while (s_levT != s_levH && (int32_t)(s_lev[s_levT].t - tgt) <= 0) {
      LiveEv& e = s_lev[s_levT];
      switch (e.ty) {
        case 0: if (g_psg[0]) PSG_writeReg(g_psg[0], e.a & 0x0F, e.b); break;
        case 1: if (g_psg[1]) PSG_writeReg(g_psg[1], e.a & 0x0F, e.b); break;
        case 2: sp0250::feed(e.a); break;
        case 3: held = (int16_t)(e.a | ((uint16_t)e.b << 8)); break;
      }
      s_levT = (uint16_t)((s_levT + 1) & LEVMASK);
    }
    synthClk = tgt;
    int32_t v = held;
    if (g_psg[0]) v += ((int32_t)PSG_calc(g_psg[0]) * 1129) >> 8;
    if (g_psg[1]) v += ((int32_t)PSG_calc(g_psg[1]) * 1129) >> 8;
    spAcc += spPer; if (spAcc >= 1.0) { spAcc -= 1.0; spHeld = (int16_t)(sp0250::next() << 7); }
    v += ((int32_t)spHeld * 353) >> 8;
    v >>= 1;
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    out[done++] = (int16_t)v;
  }
  return done;
}

// Mixeur a cadence fixe AY_FS : fait avancer l'emulateur (~1MHz wallclk) et produit n echantillons
// = DAC maintenu (sample&hold) + AY. Appele a la cadence AY_FS par la boucle -> temps-reel + mix.
int renderMix(int16_t* out, int n){
  static double cyc=0, spAcc=0, ymAcc=0; static int16_t spHeld=0, ymHeld=0;
  const double per = (g_gen==1?1000000.0:2000000.0)/(double)AY_FS;  // cycles CPU/echantillon (Gen1 1MHz: ~22.7 ; Gen2/3 2MHz: ~45.4)
  const double spPer = 10000.0/(double)AY_FS;        // SP0250 : trame ~10 kHz -> sur-echantillonne a AY_FS
  const double ymPer = 55930.0/(double)AY_FS;        // YM2151 : natif ~55.93 kHz -> decime a AY_FS
  for(int i=0;i<n;i++){
    cyc += per;
    while(cyc >= 64.0){ run(1); cyc -= 64.0; }       // run(1) = 1 quantum -> wallclk += 64
    // Niveaux relatifs PinMAME GTS80B (DAC=50, AY=25/puce, SP0250=50, YM2151=75) en Q8 (xN puis >>8) :
    int32_t s = (g_mixMask & 1) ? g_dacHeld : 0;                     // DAC = reference (niveau 50)
    if(g_psg[0]){ int32_t v=((int32_t)PSG_calc(g_psg[0]) * 1129) >> 8; if(g_mixMask & 2) s += v; }  // AY puce0 — x4.41 : CALIBRE sur les STEMS PinMAME (AY/DAC=0.665)
    if(g_psg[1]){ int32_t v=((int32_t)PSG_calc(g_psg[1]) * 1129) >> 8; if(g_mixMask & 4) s += v; }  // AY puce1   vraie-machine (lit melodique/pics 0.172) ; PinMAME x2.73 enterrait la melodie 5x trop bas
    if(g_gen==1){ spAcc += spPer; if(spAcc>=1.0){ spAcc-=1.0; spHeld=(int16_t)(sp0250::next()<<7); } if(g_mixMask & 8) s += ((int32_t)spHeld * 353) >> 8; } // voix (~2x, niveau 50)
    if(g_gen==3){ ymAcc += ymPer; while(ymAcc>=1.0){ ymAcc-=1.0; ymHeld = ym2151w::nextSample(); } if(g_mixMask & 8) s += ((int32_t)ymHeld * 2097) >> 8; } // YM core-0 (~8.2x, niveau 75)
    s >>= 1;                                            // headroom maitre (le boost YM peut saturer)
    if(s>32767)s=32767; else if(s<-32768)s=-32768; out[i]=(int16_t)s;
  }
  return n;
}

} // namespace psorom
