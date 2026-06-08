// gosowav_psorom.cpp — minimal PSOROM bench firmware for Ralf's GOSOWAV board (ESP32-WROVER +
// MCP4921 SPI DAC + 4-bit SDMMC). Runs the REAL Gottlieb 80B sound 6502(s) on the ESP and streams
// the emulated DAC to the MCP4921 -> TDA7267 -> speaker. Prints the real 6502-cycles/sec throughput
// (the one unmeasured PSOROM risk) and lets you inject a sound command over Serial.
//
// 100% our code: PSOROM = our emulator on the PUBLIC-DOMAIN Fake6502; nothing from PWAVplayer is
// reused (only the board's GPIO map, which is a hardware fact). Built ONLY in the `gosowav` env
// (build_src_filter), so it doesn't pull in the S3 app (FPGA bridge / I2S / SPI-SD).
//
// SD: put the game's /yrom1.snd (Y-CPU) + /drom1.snd (D-CPU) on the card (Gen3 = YM2151 games).
// (C) 2026 Valere Pilpil / Pstore. Original implementation (CPU core = PD Fake6502).
#ifdef GOSOWAV_BENCH
#include <Arduino.h>
#include <SPI.h>
#include "FS.h"
#include "SD_MMC.h"
#include <WiFi.h>
#include <WebServer.h>
#include "psorom.h"

// MCP4921 on the GOSOWAV WROVER (hardware fact from the board): SCK=18, SDI/MOSI=23, CS=5.
static const int DAC_SCK = 18, DAC_SDI = 23, DAC_CS = 5;
static SPIClass dacspi(HSPI);

static WebServer server(80);
static float g_thrM = 0;          // measured throughput (M 6502-cycles/sec)
static int   g_lastCmd = -1;

static inline void dacOut(int16_t s) {
  uint16_t v = (uint16_t)(((int32_t)s + 32768) >> 4) & 0x0FFF;     // 16-bit signed -> 12-bit unsigned
  dacspi.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
  digitalWrite(DAC_CS, LOW);
  dacspi.transfer16(0x3000 | v);                                   // 0x3000 = MCP4921 cfg (DAC-A,1x,active)
  digitalWrite(DAC_CS, HIGH);
  dacspi.endTransaction();
}

static uint8_t* loadFile(const char* path, size_t& len) {
  File f = SD_MMC.open(path); if (!f) { len = 0; return nullptr; }
  len = f.size(); uint8_t* b = (uint8_t*)malloc(len);
  if (b && f.read(b, len) != (int)len) { free(b); b = nullptr; len = 0; }
  f.close(); return b;
}

static const char* PAGE = R"HTML(<!doctype html><html lang=fr><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>GOSOWAV PSOROM</title>
<style>body{background:#0d1017;color:#e7ecf3;font-family:system-ui;margin:0;padding:1rem}
h1{font-size:1.05rem;margin:.2rem 0}.s{color:#39b6ff;font-variant-numeric:tabular-nums}
.g{display:grid;grid-template-columns:repeat(8,1fr);gap:6px;margin-top:1rem}
button{background:#1d2430;color:#e7ecf3;border:1px solid #2a3340;border-radius:8px;padding:.7rem 0;font-size:1rem;cursor:pointer}
button:active{background:#39b6ff;color:#04243a}.n{color:#8b97a8;font-size:.78rem;margin-top:1rem}</style></head><body>
<h1>GOSOWAV &middot; PSOROM <span class=n>(notre 6502 emule sur ton hardware)</span></h1>
<div>Debit <b class=s id=thr>--</b> M cyc/s &middot; DAC <b class=s id=dac>0</b> &middot; YM <b class=s id=ym>0</b> &middot; cmd <b class=s id=cmd>--</b></div>
<div class=g id=pad></div>
<div class=n>SD: yrom1.snd + drom1.snd (jeu 80B Gen3). Clique une commande pour jouer le son du vrai 6502.</div>
<script>const pad=document.getElementById('pad');
for(let i=0;i<32;i++){const b=document.createElement('button');b.textContent=i;b.onclick=()=>fetch('/cmd?n='+i);pad.appendChild(b);}
function poll(){fetch('/status').then(r=>r.json()).then(d=>{thr.textContent=d.thr;dac.textContent=d.dac;ym.textContent=d.ym;cmd.textContent=d.cmd<0?'--':d.cmd;}).catch(()=>{});}
setInterval(poll,500);poll();</script></body></html>)HTML";

static void startWeb() {
  WiFi.mode(WIFI_AP); WiFi.softAP("GOSOWAV-PSOROM");
  Serial.printf("web: join WiFi 'GOSOWAV-PSOROM' -> http://%s/\n", WiFi.softAPIP().toString().c_str());
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/cmd", []() {
    if (server.hasArg("n")) { int n = server.arg("n").toInt(); if (n >= 0 && n <= 95) { psorom::command((uint8_t)n); g_lastCmd = n; } }
    server.send(200, "text/plain", "ok");
  });
  server.on("/status", []() {
    char b[96]; snprintf(b, sizeof(b), "{\"thr\":%.2f,\"dac\":%u,\"ym\":%u,\"cmd\":%d}",
             g_thrM, (unsigned)psorom::dacCount(), (unsigned)psorom::ymWrites(), g_lastCmd);
    server.send(200, "application/json", b);
  });
  server.begin();
}

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n=== GOSOWAV PSOROM bench (our 6502 emu on real hardware) ===");
  pinMode(DAC_CS, OUTPUT); digitalWrite(DAC_CS, HIGH);
  dacspi.begin(DAC_SCK, -1, DAC_SDI, DAC_CS);
  dacOut(0);                                                       // mid-scale = silence

  if (!SD_MMC.begin()) { Serial.println("SD_MMC mount FAILED"); return; }
  size_t yl, dl;
  uint8_t* y = loadFile("/yrom1.snd", yl);
  uint8_t* d = loadFile("/drom1.snd", dl);
  if (!y || !d) { Serial.printf("need /yrom1.snd + /drom1.snd on the SD (got y=%u d=%u)\n", (unsigned)yl, (unsigned)dl); return; }
  Serial.printf("ROMs loaded: yrom1=%u  drom1=%u bytes\n", (unsigned)yl, (unsigned)dl);

  psorom::begin(psorom::GTS80B_GEN3, y, yl, d, dl);
  psorom::command(0); psorom::run(20000);                         // prime (80B ignores the 1st byte)

  // --- THROUGHPUT BENCH: how many 6502-cycles/sec does this WROVER sustain? (80B needs ~2.0 M) ---
  psorom::command(22);                                            // a DAC-music command (badgirls)
  uint32_t t0 = millis(), cyc = 0;
  while (millis() - t0 < 1000) cyc += psorom::run(200000);
  g_thrM = cyc / 1e6f;
  Serial.printf("THROUGHPUT: %.2f M 6502-cycles/sec   (real-time 80B needs ~2.0 M)\n", g_thrM);
  Serial.printf("1s of cmd22: dac=%u  ym=%u\n", (unsigned)psorom::dacCount(), (unsigned)psorom::ymWrites());
  Serial.println("Type a sound command number (0-31) + Enter to play it.");
  startWeb();                                                     // small web UI (SoftAP)
}

void loop() {
  server.handleClient();
  // best-effort real-time: 2000 6502-cycles ~= 1 ms at 2 MHz; stream the produced DAC samples.
  static uint32_t lastUs = 0;
  psorom::run(2000);
  int16_t buf[256]; int n = psorom::dacDrain(buf, 256);
  for (int i = 0; i < n; i++) dacOut(buf[i]);
  uint32_t now = micros(); int32_t dt = 1000 - (int32_t)(now - lastUs);
  if (dt > 0 && dt < 2000) delayMicroseconds(dt);                 // pace to ~real-time
  lastUs = micros();
  if (Serial.available()) { int c = Serial.parseInt(); if (c >= 0 && c <= 95) { psorom::command((uint8_t)c); Serial.printf("-> cmd %d\n", c); } }
}
#endif // GOSOWAV_BENCH
