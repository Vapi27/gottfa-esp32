#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include "diag.h"
#include "board_config.h"
#include "net.h"
#include "wavplayer.h"
#include "fpgalink.h"
#include "tourney.h"
#include "dispinject.h"
#include "gamedata.h"
#include "coiltest.h"

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
#define REG_CTRL2    0x04   // b0 tournament_mode (latched in lisyctrl -> persists into gameplay)
#define REG_TA_START 0x05   // 0x05..0x07 time-attack start points (24-bit LMH), persists into gameplay
#define REG_TA_DECAY 0x08   // 0x08..0x0A time-attack decay/sec   (24-bit LMH), persists into gameplay
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
  // fw was char[12] — long enough for "0.5.0" and nothing else. v1 reports the full
  // build id ("1.0.0+951b327-dirty") so a board can be identified from the UI alone,
  // and strncpy would have silently cut that in half.
  char fw[40]="?", idcode[12]="0x0", mode[12]="?", ip[20]="0.0.0.0";
  char game[24]="—"; uint16_t gamenr=0; bool is80B=false;
  char busmode[8]="normal";      // diag-bus state: "normal" | "diag" | "bus?"
  uint8_t lisyId=0;              // lisyctrl ID register (0x80 = slave answering)
  bool    tourneyFpga=false;     // FPGA tournament mode (CTRL2 b0) — armed via diag, persists in game

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
  // Push the time-attack start/decay to the FPGA (24-bit each, low-mid-high) so the on-display
  // countdown matches the ESP's tourney config. Persists in lisyctrl past diag exit into gameplay.
  inline void pushTaConfig(uint32_t start, uint32_t decay) {
    if(!busOwned) return;
    bridgeWrite(REG_TA_START+0,  start        & 0xFF);
    bridgeWrite(REG_TA_START+1, (start >>  8) & 0xFF);
    bridgeWrite(REG_TA_START+2, (start >> 16) & 0xFF);
    bridgeWrite(REG_TA_DECAY+0,  decay        & 0xFF);
    bridgeWrite(REG_TA_DECAY+1, (decay >>  8) & 0xFF);
    bridgeWrite(REG_TA_DECAY+2, (decay >> 16) & 0xFF);
  }

  void txt(AsyncWebSocketClient*c, const String&s){ if(c) c->text(s); else if(g_ws) g_ws->textAll(s); }

  // --- coiltest bus lending -------------------------------------------------
  // coiltest owns no hardware; it borrows this file's shared-bus bridge through three
  // function pointers. ctReady() is the single source of truth for "may a coil fire":
  // the FPGA must have granted the bus (diag mode) AND the outputs must be armed. It is
  // re-read by coiltest on every tick, so dropping either one stops a run in progress.
  uint8_t ctRead (uint8_t r)            { return bridgeRead(r); }
  void    ctWrite(uint8_t r, uint8_t v) { bridgeWrite(r, v); }
  // fpgalink::diagActive() is tested on top of busOwned on purpose: bus arbitration below
  // needs 3 confirming ticks before it releases, and firing a solenoid into a bus the FPGA
  // has already taken back is not something to do for even one pass.
  bool    ctReady() { return busOwned && outputs && fpgalink::diagActive(); }

  void sendCoilTest(AsyncWebSocketClient*c){ txt(c, coiltest::statusJson()); }

  void sendInfo(AsyncWebSocketClient*c){
    JsonDocument d; d["t"]="info"; d["fw"]=fw; d["idcode"]=idcode; d["ip"]=ip; d["mode"]=mode;
    d["game"]=game; d["gamenr"]=gamenr; d["is80B"]=is80B?1:0; d["outputs"]=outputs?1:0;
    char lid[8]; snprintf(lid,sizeof(lid),"0x%02X",lisyId);
    d["bus"]=busmode; d["lisy"]=lid;                        // diag-bus state + lisyctrl ID
    d["tfpga"]=tourneyFpga?1:0;                             // FPGA tournament mode (CTRL2)
    String s; serializeJson(d,s); txt(c,s);
  }
  // ball in play (fpgalink token 0xA0|value): same trio que /link, ballAge=-1 si jamais recu.
  void addBall(JsonDocument&d){
    uint8_t bv; uint32_t bn, ba; fpgalink::ballStats(bv,bn,ba);
    d["ball"]=bv; d["ballN"]=bn; d["ballAge"]=(ba==0xFFFFFFFF)?-1:(int32_t)ba;
  }
  void sendStatus(){ JsonDocument d; d["t"]="status"; d["outputs"]=outputs?1:0; d["wd"]=wd_tripped?1:0;
    addBall(d);
    String s; serializeJson(d,s); txt(nullptr,s); }
  void sendArr(const char*type, const uint8_t*a, int n, AsyncWebSocketClient*c){
    JsonDocument d; d["t"]=type; JsonArray j=d["d"].to<JsonArray>();
    for(int i=0;i<n;i++) j.add(a[i]); String s; serializeJson(d,s); txt(c,s);
  }

  uint32_t rebootAt=0;               // set by {c:'reboot'} -> tick() restarts the ESP
  uint32_t exitAt=0;                 // set by {c:'exit'} -> tick() pulses S8 reset to leave diag

  // ESP device status for the Système tab.
  void sendSysInfo(AsyncWebSocketClient*c){
    JsonDocument d; d["t"]="sysinfo";
    d["fw"]=fw; d["idcode"]=idcode; d["ip"]=ip; d["mode"]=mode;
    d["built"]=FW_BUILD;                                   // commit date (see version.py)
    d["heap"]=(uint32_t)ESP.getFreeHeap(); d["up"]=(uint32_t)(millis()/1000);
#ifndef BOARD_C3
    d["sets"]=wavplayer::themeCount(); d["sd"]=wavplayer::ready()?1:0; d["c3"]=0;
#else
    d["sets"]=0; d["sd"]=0; d["c3"]=1;
#endif
    String s; serializeJson(d,s); txt(c,s);
  }

  void sendTourney(AsyncWebSocketClient*c){ txt(c, tourney::json()); }   // tournament leaderboard

  // ======================= TIME-ATTACK ENGINE ==============================================
  // Runs entirely OUTSIDE diag mode and touches no SPI: diag holds the 6502 in reset, so a
  // tournament game can only ever be played with the bus released. Everything here is driven
  // by the FPGA link (game state + sound commands) and drives the one-wire control/display UART.
  //
  // Game state comes from fpgalink::gameInProgress() = RIOT RAM $0072. The previous trigger
  // (fpgalink::gameRunning(), link token 0xF2/0xF3) was proven on hardware to be a "the 6502
  // has booted" latch — high ~0.25 s after reset and never clearing — so the auto-timer built
  // on it could never fire. $0072 is the real 0 = attract / 1 = playing flag.
  //
  // Session shape: arm a player -> control bit0 (auto-restart) + bit1 (countdown on the glass)
  // go out at 4 Hz -> the machine starts a game -> the countdown runs -> at every game over the
  // FPGA re-coins/re-starts, and the countdown PAUSES for that gap so the restart costs nothing
  // -> when the clock hits 0 we send bit2 (end the game NOW), drop the flags and record.
  constexpr uint32_t TA_DISP_MS   = 250;    // countdown -> glass (4 Hz)
  constexpr uint32_t TA_PUSH_MS   = 500;    // countdown -> web UI
  constexpr uint32_t TA_SND_CD_MS = 150;    // per-command bonus cooldown
  constexpr uint32_t TA_GAP_MS    = 30000;  // attract gap tolerated before we call the session over
  uint32_t taSndMs[32] = {0};               // last bonus per sound command (cooldown)
  uint32_t taGapMs = 0;                     // millis() when the current attract gap started

  void sendTa(){
    uint32_t now = millis();
    JsonDocument d; d["t"]="ta";
    d["mode"]=tourney::curMode(); d["armed"]=tourney::armed(); d["id"]=tourney::activePlayer();
    d["rem"]=tourney::liveScore(now); d["pause"]=tourney::paused()?1:0;
    d["bgot"]=tourney::bonusGiven(); d["gip"]=fpgalink::gameInProgress()?1:0;
    d["ctrl"]=dispinject::ctrl();
    d["start"]=tourney::taStart(); d["decay"]=tourney::taDecay();
    d["bonus"]=tourney::taBonus(); d["cap"]=tourney::taCap();
    String s; serializeJson(d,s); txt(nullptr,s);
  }

  // End the running time-attack session: record the remainder, drop the FPGA flags and, when
  // asked, tell the FPGA to end the game on the machine (control bit2, edge-triggered one-shot).
  // setCtrl(0) BEFORE pulseKill() matters: the frame carrying the kill edge must already have
  // auto-restart cleared, else the FPGA would helpfully start another game.
  void taFinish(bool killGame){
    uint32_t sc = tourney::stopGame(millis());
    tourney::arm(0);                        // one armed session = one run; re-arm explicitly
    taGapMs = 0;
    dispinject::setCtrl(0);
    if(killGame) dispinject::pulseKill();
    dispinject::send(sc);                   // final value stays on the glass until the overlay drops
    JsonDocument a; a["t"]="timer"; a["id"]=0; a["score"]=sc; String s; serializeJson(a,s); txt(nullptr,s);
    sendTourney(nullptr); sendTa();
  }

  // One live sound command -> maybe a time bonus. Ignored when no game is running, so a queued
  // burst can never pay out later; ignored for looping/drone cues (holding one would farm
  // infinite time); rate-limited per command; the overall per-game cap lives in tourney.
  void taSoundHit(uint8_t cmd, uint32_t now){
    if(cmd >= 32) return;
    if(!tourney::gameActive() || tourney::paused()) return;
    if(!tourney::taBonus()) return;
#ifndef BOARD_C3
    if(wavplayer::loopMask() & (1u << cmd)) return;    // drone/loop: not a scoring event
#endif
    if(taSndMs[cmd] && (now - taSndMs[cmd]) < TA_SND_CD_MS) return;
    taSndMs[cmd] = now ? now : 1;                      // 0 means "never", so skip that tick
    tourney::addBonus(tourney::taBonus(), now);
  }

  void taTick(){
    uint32_t now = millis();
    // Always drain the sound FIFO, even with no game running: stale commands must not pile up
    // and pay out in a burst at the next start.
    uint8_t cmd; while(fpgalink::popSound(cmd)) taSoundHit(cmd, now);

    const bool ta = (tourney::curMode() == 1);
    // Keep the FPGA flags alive while a session is armed or running. setCtrl() is idempotent and
    // only writes on change; dispinject::tick() does the 4 Hz repeat the 2 s fail-safe needs.
    if(ta && (tourney::armed() > 0 || tourney::gameActive()))
      dispinject::setCtrl(dispinject::CTRL_AUTORESTART | dispinject::CTRL_TA_DISPLAY);

    static bool lastGip = false;
    bool gip = fpgalink::gameInProgress();
    if(gip != lastGip){ lastGip = gip;
      if(ta){
        if(gip){
          if(tourney::gameActive()){ tourney::setPaused(false, now); taGapMs = 0; }  // auto-restart served
          else if(tourney::armed() > 0){
            tourney::startGame(tourney::armed(), now);
            memset(taSndMs, 0, sizeof taSndMs); taGapMs = 0;
            JsonDocument a; a["t"]="timer"; a["id"]=tourney::activePlayer();
            String s; serializeJson(a,s); txt(nullptr,s);
          }
        } else if(tourney::gameActive()){
          tourney::setPaused(true, now);                  // between games: the clock stops
          taGapMs = now ? now : 1;                        // 0 is the "no gap open" value
        }
        sendTa();
      }
    }

    // Armed but idle: keep the UI's arm hint fresh. It reads `gip` to say "lance une partie" vs
    // "la manche démarrera à la prochaine partie", and a missed gip edge would otherwise leave
    // the operator staring at the wrong sentence. 0.5 Hz, one small frame — free.
    if(!tourney::gameActive()){
      if(tourney::curMode()==1 && tourney::armed()>0){
        static uint32_t lastIdle=0;
        if(now-lastIdle >= 2000){ lastIdle=now; sendTa(); }
      }
      return;
    }
    uint32_t rem = tourney::liveScore(now);              // advances the countdown

    // Stream the countdown to the machine display. DIGITS AND BLANK ONLY — the System-80 glass
    // renders no letters, so the value goes out as-is ("%7lu" in dispinject::send).
    { static uint32_t lastDisp=0;
      if(now-lastDisp >= TA_DISP_MS){ lastDisp = now; dispinject::send(rem); } }
    { static uint32_t lastPush=0;
      if(now-lastPush >= TA_PUSH_MS){ lastPush = now; sendTa(); } }

    if(rem == 0){ taFinish(true); return; }             // clock out -> end the game on the machine
    // Auto-restart failed (3 attempts exhausted, machine off, link down): close the session and
    // keep the remaining points rather than pausing forever.
    if(tourney::paused() && taGapMs && (now-taGapMs) >= TA_GAP_MS) taFinish(false);
  }
  // ======================= end time-attack engine ==========================================

  // game-select reference (games.txt): FPGA No -> romname, for the Déploiement tab.
  void sendGameList(AsyncWebSocketClient*c){
#ifndef BOARD_C3
    JsonDocument d; d["t"]="gamelist"; JsonArray g=d["g"].to<JsonArray>();
    for(int i=0;i<64;i++){ const char* r=wavplayer::gameRom(i); if(r&&r[0]){
      JsonObject o=g.add<JsonObject>(); o["n"]=i; o["rom"]=r; } }
    String s; serializeJson(d,s); txt(c,s);
#else
    (void)c;
#endif
  }

  // PSOWAV (ESP WAV engine) state for the web UI: ready, current game, which sounds exist, game list.
  void sendSndInfo(AsyncWebSocketClient*c){
#ifndef BOARD_C3
    JsonDocument d; d["t"]="sndinfo";
    d["ready"]=wavplayer::ready()?1:0; d["theme"]=wavplayer::curTheme();
    d["mask"]=wavplayer::soundMask(); d["n"]=wavplayer::soundCount();
    d["loop"]=wavplayer::loopMask(); d["voice"]=wavplayer::voiceMask();
    JsonArray th=d["themes"].to<JsonArray>();
    for(int i=0;i<wavplayer::themeCount();i++) th.add(wavplayer::themeName(i));
    { uint16_t lst[96]; int sn=wavplayer::soundList(lst,96);   // real per-ROM sound list (ids 0..95, incl. banks)
      JsonArray a=d["snds"].to<JsonArray>(); for(int i=0;i<sn;i++) a.add(lst[i]); }
    String s; serializeJson(d,s); txt(c,s);
#else
    (void)c;
#endif
  }

  // ---- bus arbitration: follow the FPGA Debug handshake (= lisy_active) -----
  void busRelease(){
    coiltest::abort();                 // before the bus goes: a run cannot outlive diag mode
    if(busOwned) SPI.end();
    busOwned=false;
    pinMode(PIN_SPI_SCLK,INPUT); pinMode(PIN_SPI_MOSI,INPUT); pinMode(PIN_SPI_MISO,INPUT);
    lisyId=0; outputs=false; blink=false; coilFault=0;
    strncpy(busmode,"normal",sizeof(busmode)-1); busmode[sizeof(busmode)-1]=0;
    memset(sw,0,sizeof(sw));
    Serial.println("[diag] bus released -> Hi-Z (FPGA owns SPI)");
    sendInfo(nullptr); sendArr("sw",sw,8,nullptr); sendStatus();
  }
  // one 20-char row to the 80B glass (lisyctrl 0x50..0x77; disp80b_diag streams it)
  void writeDispRow(int p, const char* txt){
    bool end=false;
    for(int i=0;i<20;i++){
      char ch=' ';
      if(!end){ if(txt[i]) ch=toupper((unsigned char)txt[i]); else end=true; }
      bridgeWrite(0x50 + p*20 + i, (uint8_t)ch);
    }
  }
  void busAcquire(){
    SPI.begin(PIN_SPI_SCLK,PIN_SPI_MISO,PIN_SPI_MOSI,-1);   // shared bus, no hardware CS
    busOwned=true;
    lisyId = bridgeRead(REG_ID);                            // 0x80 if lisyctrl answers
    uint8_t st = bridgeRead(REG_STATUS);                    // b0 active,b1 wd,b2 is80B
    is80B = (st&0x04)!=0; wd_tripped=(st&0x02)!=0;
    tourneyFpga = (bridgeRead(REG_CTRL2)&1)!=0;             // current latched tournament mode
    pushTaConfig(tourney::taStart(), tourney::taDecay());   // sync the FPGA countdown to our TA config
    bridgeWrite(REG_CTRL,0x00);                             // outputs OFF (safe on entry)
    bridgeWrite(REG_COIL_FAULT,0x00); coilFault=0;          // clear any latched coil fault
    outputs=false; blink=false;
    memset(lamps,0,sizeof(lamps)); memset(sw,0,sizeof(sw));
    strncpy(busmode,(lisyId==0x80)?"diag":"bus?",sizeof(busmode)-1); busmode[sizeof(busmode)-1]=0;
    // welcome banner on the 80B glass: mode + where to point the browser
    char l2[24]; snprintf(l2,sizeof(l2),"WEB %s", netIp());
    writeDispRow(0, "PSTORE DIAG MODE");
    writeDispRow(1, l2);
    Serial.printf("[diag] bus acquired: lisy ID=0x%02X status=0x%02X\n",lisyId,st);
    sendInfo(nullptr); sendArr("lamps",lamps,6,nullptr); sendArr("sw",sw,8,nullptr); sendStatus();
  }
}

void diag::begin() {
  // the FPGA Debug pin is now the fpgalink UART (diag-mode token + sound) -> fpgalink owns it
  pinMode(PIN_SPI_SCLK, INPUT);                // Group-A stays Hi-Z until the bus is granted
  pinMode(PIN_SPI_MOSI, INPUT);
  pinMode(PIN_SPI_MISO, INPUT);
  coiltest::begin({ ctRead, ctWrite, ctReady });   // LittleFS is already mounted (setup())
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
  sendArr("sound",snd,4,c); sendArr("dip",dipv,4,c); sendSndInfo(c); sendSysInfo(c); sendTourney(c);
  // Names from the title's own manual (gamedata.h): they land on the switch matrix, the
  // lamp grid AND the coil pad, so "SW 43" reads "éjecteur gauche (fire pit)" everywhere
  // instead of only inside the coil test.
  { String n = gamedata::namesJson(); txt(c, n); }
  sendCoilTest(c);                             // learned signatures + last run's verdicts
  sendTa();                                    // live time-attack state (countdown, arm, FPGA flags)
  JsonDocument d; d["t"]="status"; d["outputs"]=outputs?1:0; d["wd"]=wd_tripped?1:0;
  addBall(d);
  String s; serializeJson(d,s); c->text(s);
}

void diag::onText(AsyncWebSocketClient*c, const char*data, size_t len){
  JsonDocument d; if(deserializeJson(d,data,len)) return;
  const char* cmd = d["c"] | "";

  if(!strcmp(cmd,"hello")) { onConnect(c); }

  else if(!strcmp(cmd,"outputs")) {
    if(!busOwned) return;                       // can only arm outputs while we own the bus
    outputs = (int)d["v"]!=0;
    if(!outputs) coiltest::abort();             // disarming mid-run stops it, loudly
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
  // --- coil test: learn / replay a switch signature per solenoid ---------------------
  // These three handlers run on the AsyncTCP task, so they do NOTHING but validate and
  // arm: the pulse-and-watch work is an incremental state machine ticked from loopTask
  // (see coiltest.h for why it is not a net.cpp jobRun() body). Every refusal is
  // reported -- the silent no-op that "if(!outputs) return;" above performs is exactly
  // the failure mode this feature exists to avoid.
  else if(!strcmp(cmd,"ct_learn") || !strcmp(cmd,"ct_test")) {
    bool learn = (cmd[3]=='l');
    int  key   = d["key"].is<int>() ? (int)d["key"] : coiltest::keyFor();
    // "only":"inc" retries just the coils that came back "précondition non remplie", after
    // the operator has rearranged the playfield. Re-running everything would re-consume
    // the mechanisms that had already passed and turn their verdicts back into unknowns.
    uint16_t only = 0;
    if (d["only"].is<const char*>() && !strcmp(d["only"], "inc")) only = coiltest::inconclusiveMask();
    else if (d["only"].is<int>())                                 only = (uint16_t)(int)d["only"];
    const char* err = coiltest::start(learn, key, (uint8_t)(d["ms"] | 0), only);
    JsonDocument a; a["t"]="ctstart"; a["ok"]=err?0:1; a["learn"]=learn?1:0;
    a["key"]=key; a["only"]=only; a["err"]=err?err:"";
    String s; serializeJson(a,s); txt(nullptr,s);
    // start() may have swapped in another title's table; re-broadcast so the labels on
    // screen belong to the game actually being tested.
    { String n = gamedata::namesJson(); txt(nullptr, n); }
    sendCoilTest(nullptr);
  }
  // The UI is bilingual, and the per-title PROSE (preconditions, "why this can never be
  // tested") lives on the board in both languages -- so the board has to be told which one.
  // NAMES are never translated: they are the manual's own wording (see gamedata.h).
  else if(!strcmp(cmd,"lang")) {
    gamedata::setLang(d["v"] | "fr");
    { String n = gamedata::namesJson(); txt(nullptr, n); }
    sendCoilTest(nullptr);
  }
  else if(!strcmp(cmd,"ct_abort")) { coiltest::abort(); sendCoilTest(nullptr); }
  else if(!strcmp(cmd,"ct_get"))   {                       // also: switch the loaded slot
    if(d["key"].is<int>() && !coiltest::busy()) coiltest::load((int)d["key"]);
    sendCoilTest(c);
  }
  // Name the title by hand and remember it. The FPGA only reports its game number when
  // game_select CHANGES, so a board that was merely switched on says nothing and has no
  // game table -- which is exactly when the operator most needs the names and the
  // preconditions. -1 clears the override and hands authority back to the FPGA.
  else if(!strcmp(cmd,"ct_game")) {
    int g = d["g"].is<int>() ? (int)d["g"] : -1;
    bool ok = coiltest::setGame(g);
    JsonDocument a; a["t"]="ctgame"; a["ok"]=ok?1:0; a["g"]=coiltest::gameOverride();
    if(!ok) a["err"]="test en cours";
    String s; serializeJson(a,s); txt(nullptr,s);
    { String n = gamedata::namesJson(); txt(nullptr, n); }
    sendCoilTest(nullptr);
  }
  else if(!strcmp(cmd,"sound")) {
    if(!outputs) return; int n=d["n"]|0;
    if(n>=0&&n<32){ bool on=!((snd[n>>3]>>(n&7))&1); memset(snd,0,4);
      if(on){ snd[n>>3]|=(1<<(n&7)); bridgeWrite(REG_SOUND,(uint8_t)n); } }   // play via gosof80
    sendArr("sound",snd,4,nullptr);
  }
  else if(!strcmp(cmd,"snd")) {        // PSOWAV play (S3 sound tier, no diag gate): {c:'snd',n:5}
#ifndef BOARD_C3
    wavplayer::play(d["n"]|0);
#endif
  }
  else if(!strcmp(cmd,"sndinfo")) { sendSndInfo(c); }                 // PSOWAV state for the UI
  else if(!strcmp(cmd,"gamelist")) { sendGameList(c); }               // games.txt No->rom reference
  // --- tournament / score-keeping (any change broadcasts the new leaderboard) ---
  else if(!strcmp(cmd,"tourney"))  { sendTourney(c); }
  else if(!strcmp(cmd,"t_add"))    { tourney::addPlayer(d["name"]|""); sendTourney(nullptr); }
  else if(!strcmp(cmd,"t_del"))    { tourney::removePlayer(d["id"]|0); sendTourney(nullptr); }
  else if(!strcmp(cmd,"t_score"))  { tourney::recordScore(d["id"]|0, (uint32_t)(d["v"]|0)); sendTourney(nullptr); }
  else if(!strcmp(cmd,"t_undo"))   { tourney::undo(d["id"]|0); sendTourney(nullptr); }
  else if(!strcmp(cmd,"t_reset"))  { tourney::resetScores(); sendTourney(nullptr); }
  else if(!strcmp(cmd,"t_clear"))  { tourney::clearAll(); sendTourney(nullptr); }
  else if(!strcmp(cmd,"t_round"))  { int ids[4],n=0;                          // assign players to P1..Pn
    for(JsonVariant v:d["ids"].as<JsonArray>()) if(n<4) ids[n++]=v|0;
    tourney::setRound(ids,n); txt(c,tourney::roundJson()); }
  else if(!strcmp(cmd,"t_apply"))  { uint32_t sc[4]; int n=0;                 // final game scores -> auto-record
    for(JsonVariant v:d["scores"].as<JsonArray>()) if(n<4) sc[n++]=(uint32_t)(v|0);
    tourney::applyScores(sc,n); sendTourney(nullptr); }
  else if(!strcmp(cmd,"t_mode"))   { tourney::setMode((uint8_t)(d["mode"]|0),  // 0 score / 1 time-attack
    (uint32_t)(d["start"]|0), (uint32_t)(d["decay"]|0));
    if(tourney::curMode()!=1){ tourney::arm(0); dispinject::setCtrl(0); }      // leaving TA disarms the FPGA
    pushTaConfig(tourney::taStart(), tourney::taDecay());                      // sync FPGA on-display countdown
    sendTourney(nullptr); sendTa(); }
  else if(!strcmp(cmd,"t_bonus"))  { tourney::setBonus((uint32_t)(d["v"]|0),    // time bonus per sound cue + cap
    (uint32_t)(d["cap"]|0)); sendTourney(nullptr); sendTa(); }
  else if(!strcmp(cmd,"t_start"))  { tourney::startGame(d["id"]|0, millis());   // time-attack: start the clock now
    memset(taSndMs,0,sizeof taSndMs); taGapMs=0;
    if(tourney::gameActive()) dispinject::setCtrl(dispinject::CTRL_AUTORESTART|dispinject::CTRL_TA_DISPLAY);
    JsonDocument a; a["t"]="timer"; a["id"]=tourney::activePlayer(); String s; serializeJson(a,s); txt(nullptr,s);
    sendTa(); }
  else if(!strcmp(cmd,"t_stop"))   { taFinish(true); }        // stop now: record + end the game on the machine
  else if(!strcmp(cmd,"t_pause"))  { tourney::setPaused(((int)(d["v"]|0))!=0, millis()); sendTa(); }
  else if(!strcmp(cmd,"t_top"))    { tourney::addBonus((uint32_t)(d["v"]|0), millis()); sendTa(); }  // manual top-up
  else if(!strcmp(cmd,"t_ta"))     { sendTa(); }              // UI asks for the live time-attack state
  else if(!strcmp(cmd,"t_fpga"))   {                                 // arm FPGA tournament mode (lisyctrl CTRL2)
    if(busOwned){ bool on=(int)(d["on"]|0)!=0; bridgeWrite(REG_CTRL2, on?0x01:0x00);
      tourneyFpga=(bridgeRead(REG_CTRL2)&1)!=0; }                    // read back the latched value
    JsonDocument a; a["t"]="tfpga"; a["on"]=tourneyFpga?1:0; a["bus"]=busOwned?1:0;
    String s; serializeJson(a,s); txt(nullptr,s); }
  // Arm a player: from now on the FPGA auto-restarts games (ctrl bit0) and shows our countdown
  // (bit1), and the countdown starts by itself at the next RISING edge of $0072 — so arming while
  // a game is already running does nothing visible until the NEXT game. NO diag / no SPI here:
  // everything goes out on the one-wire control UART, which is live in normal play.
  //
  // Arming used to be able to fail in total silence: an unknown id makes tourney::arm() latch 0,
  // and arming outside time-attack mode latches the id but leaves taTick() inert. The reply now
  // states the outcome so the UI can verify instead of assuming. id 0 = disarm.
  else if(!strcmp(cmd,"t_arm"))    { int id=d["id"]|0;
    const char* err = nullptr;
    if(id>0 && tourney::curMode()!=1) err = "mode time-attack non actif";
    else { tourney::arm(id); if(id>0 && tourney::armed()!=id) err = "joueur inconnu"; }
    if(tourney::armed()>0 && tourney::curMode()==1)
      dispinject::setCtrl(dispinject::CTRL_AUTORESTART|dispinject::CTRL_TA_DISPLAY);
    else if(!tourney::gameActive()) dispinject::setCtrl(0);
    sendTourney(nullptr); sendTa();
    JsonDocument a; a["t"]="armres"; a["ok"]=err?0:1; a["id"]=tourney::armed();
    a["err"]=err?err:""; a["gip"]=fpgalink::gameInProgress()?1:0;   // gip=1 -> the NEXT game, not this one
    a["ctrl"]=dispinject::ctrl(); String s; serializeJson(a,s); txt(nullptr,s); }
  // Restore the ground-truthed time-attack tuning. A saved /tourney.json outranks the firmware
  // defaults, so changing them in code alone never reaches a board that has already run.
  else if(!strcmp(cmd,"t_defaults")) { tourney::resetTaDefaults();
    pushTaConfig(tourney::taStart(), tourney::taDecay());
    sendTourney(nullptr); sendTa(); }
  else if(!strcmp(cmd,"t_disarm")) { tourney::arm(0);                          // disarm, keep any running game
    if(!tourney::gameActive()) dispinject::setCtrl(0); sendTourney(nullptr); sendTa(); }
  else if(!strcmp(cmd,"sndtheme")) {                                  // load a game's PSOWAV set
#ifndef BOARD_C3
    const char* nm=d["name"]|""; if(nm[0]) wavplayer::setTheme(nm);
#endif
  }
  else if(!strcmp(cmd,"sndstop")) {                                   // stop all PSOWAV voices
#ifndef BOARD_C3
    wavplayer::stopAll();
#endif
  }
  else if(!strcmp(cmd,"sysinfo")) { sendSysInfo(c); }                 // Système tab readouts
  else if(!strcmp(cmd,"reboot"))  { rebootAt = millis() + 500;        // reboot the ESP (deploy tab)
    JsonDocument a; a["t"]="toast"; a["m"]="redémarrage de l'ESP…"; String s; serializeJson(a,s); txt(nullptr,s);
    Serial.println("[sys] reboot requested via web"); }
  else if(!strcmp(cmd,"exit"))    { exitAt = millis() + 60;           // leave diag: pulse the board reset (S8) line
    JsonDocument a; a["t"]="toast"; a["m"]="sortie du test — le jeu redémarre…"; String s; serializeJson(a,s); txt(nullptr,s);
    Serial.println("[diag] exit-diag requested via web"); }
  else if(!strcmp(cmd,"disp")) {   // 80B display text: {c:'disp',p:0|1,txt:'...'} -> lisyctrl 0x50..0x77
    if(!outputs) return;           // row p = 20 chars (space-padded, uppercased); the disp80b_diag FSM
    int p = d["p"] | 0;            // streams the 10941 latch protocol to the glass (~10 Hz restream)
    const char* txt = d["txt"] | "";
    if (p < 0 || p > 1) return;
    Serial.printf("[diag] disp %d='%s'\n", p, txt);
    bool end = false;
    for (int i = 0; i < 20; i++) {
      char ch = ' ';
      if (!end) { if (txt[i]) ch = toupper((unsigned char)txt[i]); else end = true; }
      bridgeWrite(0x50 + p*20 + i, (uint8_t)ch);
    }
  }
  else if(!strcmp(cmd,"seg")) {        // raw display test: {c:'seg',v:[a,b,c]} -> SEG_A/B/C (+ optional u5)
    if(!outputs) return;               // FPGA auto-scans the digit strobes -> one pattern shows on every digit
    for(int k=0;k<3;k++) bridgeWrite(REG_SEG_A+k, (uint8_t)(d["v"][k]|0));
    if(d["u5"].is<int>()) bridgeWrite(REG_U5, (uint8_t)(d["u5"]|0));
    Serial.printf("[diag] seg %02X %02X %02X\n",(int)(d["v"][0]|0),(int)(d["v"][1]|0),(int)(d["v"][2]|0));
  }
  else if(!strcmp(cmd,"dip")) {
    int i=d["i"]|0, v=(int)d["v"];
    if(i>=0&&i<32){ if(v) dipv[i>>3]|=(1<<(i&7)); else dipv[i>>3]&=~(1<<(i&7)); }
    sendArr("dip",dipv,4,nullptr);   // TODO: lisyctrl dip command (LISY 'V')
  }
}

void diag::tick(){
  if(rebootAt && (int32_t)(millis()-rebootAt) > 0){ Serial.println("[sys] restarting"); delay(50); ESP.restart(); }

  if(exitAt && (int32_t)(millis()-exitAt) > 0){ exitAt=0;    // leave diag: brief reset pulse on S8 (GPIO14)
    Serial.println("[diag] exit -> reset pulse");
    busRelease();                                            // drop the shared bus first
    pinMode(PIN_FPGA_RESET, OUTPUT); digitalWrite(PIN_FPGA_RESET, LOW);
    delay(150);                                              // FPGA sees reset -> lisy_active<='0' + game reloads
    pinMode(PIN_FPGA_RESET, INPUT); }

  // Time-attack: game start/stop, countdown decay, sound bonuses, the countdown on the machine
  // display and the FPGA control frames. Deliberately ahead of the bus arbitration below — it
  // must run while the ESP is OFF the SPI bus, which is the only state a game can be played in.
  taTick();
#ifndef BOARD_C3
  // responsive-to-ROM: when the FPGA selects a different game (theme changes), push the new
  // per-ROM sound list to the web UI so the sound pad rebuilds live (no manual refresh).
  { static uint32_t lastChk=0; static String lastSnd;
    if(millis()-lastChk>200){ lastChk=millis(); String t=wavplayer::curTheme();
      if(t!=lastSnd){ lastSnd=t; sendSndInfo(nullptr); } } }
#endif
  // bus arbitration: acquire/release following the FPGA diag-mode token on the link UART
  // (fpgalink), which replaces the old Debug level. diag and gameplay sound never overlap.
  bool dbg = fpgalink::diagActive();
  if(dbg!=busOwned){ if(++dbgConfirm>=3){ dbgConfirm=0; if(dbg) busAcquire(); else busRelease(); } }
  else dbgConfirm=0;

  // Coil test: a few hundred microseconds of SPI per pass, ahead of the 25 Hz gate below
  // because its sampling window needs ~4 ms resolution, not 40. While it runs it owns the
  // switch/fault reads (it clears COIL_FAULT around every pulse), so the periodic scan
  // below stands down rather than fighting it for the bus and mis-attributing faults.
  coiltest::tick();          // unconditional: losing the bus must ABORT a run, not freeze it
  { static bool ctWas=false; static uint32_t ctPush=0;
    bool ctNow=coiltest::busy();
    if(ctNow && millis()-ctPush>=250){ ctPush=millis(); sendCoilTest(nullptr); }
    if(ctWas && !ctNow){ sendCoilTest(nullptr); sendArr("sw",sw,8,nullptr); }   // final report
    ctWas=ctNow;
    if(ctNow) return;
  }

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
