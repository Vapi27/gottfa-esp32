#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include "diag.h"
#include "board_config.h"
#include "wavplayer.h"
#include "fpgalink.h"

// ===========================================================================
//  LISYcontrol backend.  Browser <--WebSocket(JSON)--> here <--SPI--> lisyctrl.
//  Real shared-bus SPI bridge: the FPGA releases the SD/EEPROM SPI bus to the ESP
//  only while diag mode is active, signalled on the Debug pin (= lisy_active).
//  Frames are mode-0, MSB-first, 2 bytes, separated by a >1.3us idle gap.
//  WRITE = {0x80|addr, val};  READ = {addr, 0x00} -> value in the 2nd byte.
//  Register map = LISYCTRL.md / lisyctrl.vhd.
// ===========================================================================

// lisyctrl SPI registers (see lisyctrl.vhd / LISYCTRL.md)
#define REG_ID       0x00   // reads 0x80 (slave-alive magic)
#define REG_STATUS   0x02   // b0 active, b1 wd_tripped, b2 is80B
#define REG_CTRL     0x03   // b0 outputs_en, b1 lamp_blink
#define REG_SW_ROW0  0x10   // 0x10..0x17  switch matrix (bit=return, 1=closed)
#define REG_DIPSLAM  0x18
#define REG_LAMP0    0x20   // 0x20..0x25  48 lamp bits
#define REG_COIL     0x30   // write coil# -> pulse
#define REG_PULSE_MS 0x31
#define REG_COIL_FAULT 0x32 // R: b0 clamp,b1 refire-blocked,b2 wd-with-coil; b7..4 coil#. W clears
#define REG_SEG_A    0x40   // 0x40..0x42 display segments
#define REG_U5       0x43
#define REG_SOUND    0x44   // write System 80 sound code 0..31 -> play via gosof80

namespace {
  AsyncWebSocket *g_ws = nullptr;

  // machine-state image (mirrors the lisyctrl register file)
  bool    outputs = false, blink = false, wd_tripped = false;
  uint8_t lamps[6] = {0};        // 48 lamp bits
  uint8_t sw[8]    = {0};        // 8 strobes x 8 returns (1=closed)
  uint8_t snd[4]   = {0};        // 32 sound bits (toggle)
  uint8_t dipv[4]  = {0};        // 32 dip bits
  uint8_t coilFault= 0;          // last COIL_FAULT (0x32): b0 clamp,b1 refire,b2 wd; b7..4 coil#

  // info
  char fw[12]="?", idcode[12]="0x0", mode[12]="?", ip[20]="0.0.0.0";
  char game[24]="—"; uint16_t gamenr=0; bool is80B=false;
  char busmode[8]="normal";      // diag-bus state: "normal" | "diag" | "bus?"
  uint8_t lisyId=0;              // lisyctrl ID register (0x80 = slave answering)

  uint32_t lastTick=0;

  // ---- shared-bus SPI bridge to the lisyctrl slave -------------------------
  const SPISettings SPI_CFG(1000000, MSBFIRST, SPI_MODE0);  // 1 MHz (clk >= ~6x SCLK)
  bool    busOwned=false;        // do we currently own the shared SPI bus?
  uint8_t dbgConfirm=0;          // debounce counter for the Debug handshake

  inline void bridgeWrite(uint8_t reg, uint8_t val) {
    if(!busOwned) return;
    uint8_t b[2]={ (uint8_t)(0x80|(reg&0x7F)), val };       // bit7=1 -> write
    delayMicroseconds(3);                                   // ensure the >1.3us frame gap
    SPI.beginTransaction(SPI_CFG); SPI.transfer(b,2); SPI.endTransaction();
  }
  inline uint8_t bridgeRead(uint8_t reg) {
    if(!busOwned) return 0;
    uint8_t b[2]={ (uint8_t)(reg&0x7F), 0x00 };             // bit7=0 -> read; value in byte1
    delayMicroseconds(3);
    SPI.beginTransaction(SPI_CFG); SPI.transfer(b,2); SPI.endTransaction();
    return b[1];
  }

  void txt(AsyncWebSocketClient*c, const String&s){ if(c) c->text(s); else if(g_ws) g_ws->textAll(s); }

  void sendInfo(AsyncWebSocketClient*c){
    JsonDocument d; d["t"]="info"; d["fw"]=fw; d["idcode"]=idcode; d["ip"]=ip; d["mode"]=mode;
    d["game"]=game; d["gamenr"]=gamenr; d["is80B"]=is80B?1:0; d["outputs"]=outputs?1:0;
    char lid[8]; snprintf(lid,sizeof(lid),"0x%02X",lisyId);
    d["bus"]=busmode; d["lisy"]=lid;                        // diag-bus state + lisyctrl ID
    String s; serializeJson(d,s); txt(c,s);
  }
  void sendStatus(){ JsonDocument d; d["t"]="status"; d["outputs"]=outputs?1:0; d["wd"]=wd_tripped?1:0;
    String s; serializeJson(d,s); txt(nullptr,s); }
  void sendArr(const char*type, const uint8_t*a, int n, AsyncWebSocketClient*c){
    JsonDocument d; d["t"]=type; JsonArray j=d["d"].to<JsonArray>();
    for(int i=0;i<n;i++) j.add(a[i]); String s; serializeJson(d,s); txt(c,s);
  }

  // ---- bus arbitration: follow the FPGA Debug handshake (= lisy_active) -----
  void busRelease(){
    if(busOwned) SPI.end();
    busOwned=false;
    pinMode(PIN_SPI_SCLK,INPUT); pinMode(PIN_SPI_MOSI,INPUT); pinMode(PIN_SPI_MISO,INPUT);
    lisyId=0; outputs=false; blink=false; coilFault=0;
    strncpy(busmode,"normal",sizeof(busmode)-1); busmode[sizeof(busmode)-1]=0;
    memset(sw,0,sizeof(sw));
    Serial.println("[diag] bus released -> Hi-Z (FPGA owns SPI)");
    sendInfo(nullptr); sendArr("sw",sw,8,nullptr); sendStatus();
  }
  void busAcquire(){
    SPI.begin(PIN_SPI_SCLK,PIN_SPI_MISO,PIN_SPI_MOSI,-1);   // shared bus, no hardware CS
    busOwned=true;
    lisyId = bridgeRead(REG_ID);                            // 0x80 if lisyctrl answers
    uint8_t st = bridgeRead(REG_STATUS);                    // b0 active,b1 wd,b2 is80B
    is80B = (st&0x04)!=0; wd_tripped=(st&0x02)!=0;
    bridgeWrite(REG_CTRL,0x00);                             // outputs OFF (safe on entry)
    bridgeWrite(REG_COIL_FAULT,0x00); coilFault=0;          // clear any latched coil fault
    outputs=false; blink=false;
    memset(lamps,0,sizeof(lamps)); memset(sw,0,sizeof(sw));
    strncpy(busmode,(lisyId==0x80)?"diag":"bus?",sizeof(busmode)-1); busmode[sizeof(busmode)-1]=0;
    Serial.printf("[diag] bus acquired: lisy ID=0x%02X status=0x%02X\n",lisyId,st);
    sendInfo(nullptr); sendArr("lamps",lamps,6,nullptr); sendArr("sw",sw,8,nullptr); sendStatus();
  }
}

void diag::begin() {
  // the FPGA Debug pin is now the fpgalink UART (diag-mode token + sound) -> fpgalink owns it
  pinMode(PIN_SPI_SCLK, INPUT);                // Group-A stays Hi-Z until the bus is granted
  pinMode(PIN_SPI_MOSI, INPUT);
  pinMode(PIN_SPI_MISO, INPUT);
}
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
    if(!busOwned) return;                       // can only arm outputs while we own the bus
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
#if COIL_SENSE_ENABLE
    // optional electrical check: peak vs baseline on the shunt during the pulse
    // (needs a current-sense shunt + amp on the solenoid return — see board_config.h)
    int base=analogRead(PIN_COIL_SENSE), peak=base; uint32_t win=(ms<80?ms:80), t0=millis();
    while(millis()-t0<win){ int v=analogRead(PIN_COIL_SENSE); if(v>peak)peak=v; }
    int amp=peak-base; const char* vd=(peak>=COIL_SENSE_SHORT)?"short":(amp<=COIL_SENSE_OPEN)?"open":"ok";
    JsonDocument cs; cs["t"]="coilsense"; cs["i"]=i; cs["adc"]=peak; cs["amp"]=amp; cs["v"]=vd;
    String s; serializeJson(cs,s); txt(nullptr,s);
    Serial.printf("[coil] sense i=%d peak=%d amp=%d -> %s\n", i, peak, amp, vd);
#endif
  }
  else if(!strcmp(cmd,"sound")) {
    if(!outputs) return; int n=d["n"]|0;
    if(n>=0&&n<32){ bool on=!((snd[n>>3]>>(n&7))&1); memset(snd,0,4);
      if(on){ snd[n>>3]|=(1<<(n&7)); bridgeWrite(REG_SOUND,(uint8_t)n); } }   // play via gosof80
    sendArr("sound",snd,4,nullptr);
  }
  else if(!strcmp(cmd,"snd")) {        // ESP WAV player test (S3 sound tier): {c:'snd',n:5}
#ifndef BOARD_C3
    wavplayer::play(d["n"]|0);
#endif
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
  // bus arbitration: acquire/release following the FPGA diag-mode token on the link UART
  // (fpgalink), which replaces the old Debug level. diag and gameplay sound never overlap.
  bool dbg = fpgalink::diagActive();
  if(dbg!=busOwned){ if(++dbgConfirm>=3){ dbgConfirm=0; if(dbg) busAcquire(); else busRelease(); } }
  else dbgConfirm=0;

  uint32_t now=millis();
  if(now-lastTick<40) return; lastTick=now;              // ~25 Hz
  if(!busOwned) return;

  // real switch scan: read the 8 strobe rows, broadcast on change
  bool changed=false;
  for(int s=0;s<8;s++){ uint8_t v=bridgeRead(REG_SW_ROW0+s); if(v!=sw[s]){ sw[s]=v; changed=true; } }
  if(changed) sendArr("sw",sw,8,nullptr);

  // watchdog flag (STATUS b1)
  bool wd=(bridgeRead(REG_STATUS)&0x02)!=0;
  if(wd!=wd_tripped){ wd_tripped=wd; sendStatus(); }

  // coil-fault latch (0x32): report decoded fault when it changes
  uint8_t cf=bridgeRead(REG_COIL_FAULT);
  if(cf!=coilFault){ coilFault=cf;
    JsonDocument fd; fd["t"]="coilfault"; fd["raw"]=cf; fd["coil"]=(cf>>4)&0x0F;
    fd["clamp"]=(cf&1)?1:0; fd["refire"]=(cf&2)?1:0; fd["wd"]=(cf&4)?1:0;
    String s; serializeJson(fd,s); txt(nullptr,s);
    if(cf) Serial.printf("[coil] FAULT 0x%02X coil=%d\n", cf, (cf>>4)&0x0F);
  }
}
