#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "diag.h"

// ===========================================================================
//  LISYcontrol backend.  Browser <--WebSocket(JSON)--> here <--SPI--> lisyctrl.
//  The SPI side is stubbed (bridgeWrite/bridgeRead) so the UI runs with no FPGA;
//  each handler notes the lisyctrl register it will drive. Register map = LISYCTRL.md.
// ===========================================================================

// lisyctrl SPI registers (see lisyctrl.vhd / LISYCTRL.md)
#define REG_CTRL     0x03   // b0 outputs_en, b1 lamp_blink
#define REG_SW_ROW0  0x10   // 0x10..0x17  switch matrix (bit=return, 1=closed)
#define REG_DIPSLAM  0x18
#define REG_LAMP0    0x20   // 0x20..0x25  48 lamp bits
#define REG_COIL     0x30   // write coil# -> pulse
#define REG_PULSE_MS 0x31
#define REG_SEG_A    0x40   // 0x40..0x42 display segments
#define REG_U5       0x43

namespace {
  AsyncWebSocket *g_ws = nullptr;

  // machine-state image (mirrors the lisyctrl register file)
  bool    outputs = false, blink = false, wd_tripped = false;
  uint8_t lamps[6] = {0};        // 48 lamp bits
  uint8_t sw[8]    = {0};        // 8 strobes x 8 returns (1=closed)
  uint8_t snd[4]   = {0};        // 32 sound bits (toggle)
  uint8_t dipv[4]  = {0};        // 32 dip bits

  // info
  char fw[12]="?", idcode[12]="0x0", mode[12]="?", ip[20]="0.0.0.0";
  char game[24]="—"; uint16_t gamenr=0; bool is80B=false;

  uint32_t lastTick=0, lastSw=0, seed=0xC0FFEE;

  // ---- SPI bridge stubs (TODO: ESP32 SPI master + bus arbitration) ----
  inline void bridgeWrite(uint8_t reg, uint8_t val) { (void)reg; (void)val; /* TODO SPI */ }
  inline uint8_t bridgeRead(uint8_t reg) { (void)reg; return 0; /* TODO SPI */ }

  uint32_t prng(){ seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; }

  void txt(AsyncWebSocketClient*c, const String&s){ if(c) c->text(s); else if(g_ws) g_ws->textAll(s); }

  void sendInfo(AsyncWebSocketClient*c){
    JsonDocument d; d["t"]="info"; d["fw"]=fw; d["idcode"]=idcode; d["ip"]=ip; d["mode"]=mode;
    d["game"]=game; d["gamenr"]=gamenr; d["is80B"]=is80B?1:0; d["outputs"]=outputs?1:0;
    String s; serializeJson(d,s); txt(c,s);
  }
  void sendStatus(){ JsonDocument d; d["t"]="status"; d["outputs"]=outputs?1:0; d["wd"]=wd_tripped?1:0;
    String s; serializeJson(d,s); txt(nullptr,s); }
  void sendArr(const char*type, const uint8_t*a, int n, AsyncWebSocketClient*c){
    JsonDocument d; d["t"]=type; JsonArray j=d["d"].to<JsonArray>();
    for(int i=0;i<n;i++) j.add(a[i]); String s; serializeJson(d,s); txt(c,s);
  }
}

void diag::begin() {}
void diag::attach(AsyncWebSocket *ws){ g_ws = ws; }
void diag::setInfo(const char*f,uint32_t id,const char*m,const char*i){
  strncpy(fw,f,sizeof(fw)-1); fw[sizeof(fw)-1]=0;
  snprintf(idcode,sizeof(idcode),"0x%08X",(unsigned)id);
  strncpy(mode,m,sizeof(mode)-1); mode[sizeof(mode)-1]=0;
  strncpy(ip,i,sizeof(ip)-1); ip[sizeof(ip)-1]=0;
}

void diag::onConnect(AsyncWebSocketClient*c){
  sendInfo(c); sendArr("lamps",lamps,6,c); sendArr("sw",sw,8,c);
  sendArr("sound",snd,4,c); sendArr("dip",dipv,4,c);
  JsonDocument d; d["t"]="status"; d["outputs"]=outputs?1:0; d["wd"]=wd_tripped?1:0;
  String s; serializeJson(d,s); c->text(s);
}

void diag::onText(AsyncWebSocketClient*c, const char*data, size_t len){
  JsonDocument d; if(deserializeJson(d,data,len)) return;
  const char* cmd = d["c"] | "";

  if(!strcmp(cmd,"hello")) { onConnect(c); }

  else if(!strcmp(cmd,"outputs")) {
    outputs = (int)d["v"]!=0;
    bridgeWrite(REG_CTRL, (outputs?0x01:0x00) | (blink?0x02:0x00));   // lisyctrl CTRL
    if(!outputs){ memset(lamps,0,6); memset(sw,0,8); sendArr("lamps",lamps,6,nullptr); sendArr("sw",sw,8,nullptr); }
    Serial.printf("[diag] outputs=%d\n", outputs); sendStatus();
  }
  else if(!strcmp(cmd,"lamp")) {
    if(!outputs) return; int i=d["i"]|0, v=(int)d["v"];
    if(i>=0&&i<48){ if(v) lamps[i>>3]|=(1<<(i&7)); else lamps[i>>3]&=~(1<<(i&7)); }
    bridgeWrite(REG_LAMP0+(i>>3), lamps[i>>3]);                       // lisyctrl LAMP
    sendArr("lamps",lamps,6,nullptr);
  }
  else if(!strcmp(cmd,"lampAll")) {
    if(!outputs) return; uint8_t v=(int)d["v"]?0xFF:0x00; memset(lamps,v,6);
    for(int k=0;k<6;k++) bridgeWrite(REG_LAMP0+k, lamps[k]);
    sendArr("lamps",lamps,6,nullptr);
  }
  else if(!strcmp(cmd,"lampBlink")) {
    if(!outputs) return; blink=(int)d["v"]!=0;
    bridgeWrite(REG_CTRL, (outputs?0x01:0x00)|(blink?0x02:0x00));
  }
  else if(!strcmp(cmd,"coil")) {
    if(!outputs) return; int i=d["i"]|0, ms=d["ms"]|60;
    bridgeWrite(REG_PULSE_MS, (uint8_t)ms); bridgeWrite(REG_COIL, (uint8_t)i);  // lisyctrl pulse
    Serial.printf("[diag] coil %d %dms\n", i, ms);
  }
  else if(!strcmp(cmd,"sound")) {
    if(!outputs) return; int n=d["n"]|0;
    if(n>=0&&n<32){ bool on=!((snd[n>>3]>>(n&7))&1); memset(snd,0,4); if(on) snd[n>>3]|=(1<<(n&7)); }
    bridgeWrite(REG_U5, 0); // TODO: sound code via U6_PA on lisyctrl
    sendArr("sound",snd,4,nullptr);
  }
  else if(!strcmp(cmd,"disp")) {
    if(!outputs) return; Serial.printf("[diag] disp %d='%s'\n",(int)(d["p"]|0),(const char*)(d["txt"]|""));
    // TODO: encode text -> SEG_A/B/C (lisyctrl 0x40..0x42)
  }
  else if(!strcmp(cmd,"dip")) {
    int i=d["i"]|0, v=(int)d["v"];
    if(i>=0&&i<32){ if(v) dipv[i>>3]|=(1<<(i&7)); else dipv[i>>3]&=~(1<<(i&7)); }
    sendArr("dip",dipv,4,nullptr);   // TODO: lisyctrl dip command (LISY 'V')
  }
}

void diag::tick(){
  uint32_t now=millis();
  if(now-lastTick<40) return; lastTick=now;            // ~25 Hz
  // MOCK: keep the switch grid alive in control mode.
  // REAL: read lisyctrl SW_ROW (0x10..0x17) over SPI and broadcast on change.
  if(outputs && now-lastSw>120){
    uint32_t r=prng(); sw[(r>>3)&7]^=(1<<((r>>8)&7));
    sendArr("sw",sw,8,nullptr); lastSw=now;
  }
}
