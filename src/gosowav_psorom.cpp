// gosowav_psorom.cpp — PSOROM multi-jeux pour la carte GOSOWAV de Ralf (ESP32-WROVER + MCP4921 SPI
// DAC + 4-bit SDMMC). Fait tourner le VRAI 6502 + ROM son Gottlieb 80B sur l'ESP et envoie le DAC
// émulé vers le MCP4921 -> TDA7267 -> HP. Carte SD = TOUS les jeux 80B (Gen1/2/3) ; on choisit le
// jeu depuis la page web. Affiche aussi le débit réel 6502-cycles/s (le risque PSOROM non mesuré).
//
// 100% notre code : PSOROM = notre émulateur sur le Fake6502 DOMAINE PUBLIC ; rien de PWAVplayer.
// Compilé UNIQUEMENT dans l'env `gosowav` (build_src_filter).
//
// CARTE SD :
//   /games.idx                 -> lignes "short|gen|title" (gen = 1/2/3)
//   /games/<short>/yrom1.snd    (Y-CPU)
//   /games/<short>/yrom2.snd    (optionnel : présent sur certains Gen1, ex. Arena)
//   /games/<short>/drom1.snd    (D-CPU / DAC)
// (générée par tools/build_games_sd.py + ~/gosowav_sd/make_sd.sh ; gen lue dans la source PinMAME).
//
// ARCHI : le serveur web tourne dans SA tâche FreeRTOS (core 0 = cœur WiFi) -> la page répond dès
// que l'AP est là, quoi que fasse l'émulateur. SD/manifeste montés dans une tâche dédiée (un SD qui
// bloque ne gèle ni loop ni le web). L'émulateur (begin/run/command) n'est touché QUE par loop()
// (core 1) ; le web ne fait que poser des intentions (volatile) que loop() applique -> mono-thread.
// (C) 2026 Valere Pilpil / Pstore. Original (cœur CPU = Fake6502 PD).
#ifdef GOSOWAV_BENCH
#include <Arduino.h>
#include <SPI.h>
#include "FS.h"
#include "SD_MMC.h"
#include <WiFi.h>
#include <WebServer.h>
#include <string.h>
#include "psorom.h"

// MCP4921 sur la WROVER GOSOWAV (fait matériel de la carte) : SCK=18, SDI/MOSI=23, CS=5.
static const int DAC_SCK = 18, DAC_SDI = 23, DAC_CS = 5;
static SPIClass dacspi(HSPI);

static WebServer server(80);

#define MAXGAMES 32
struct Game { char id[16]; uint8_t gen; char title[40]; };
static Game  g_games[MAXGAMES];
static int   g_nGames = 0;
static int   g_sel = 0;                  // jeu courant (index dans g_games)

static float        g_thrM = 0;          // débit mesuré (M 6502-cycles/s)
static bool         g_benched = false;   // bench déjà fait ?
static volatile int g_lastCmd = -1;      // dernière commande son
static volatile int g_pendingCmd = -1;   // web/série -> loop() : commande à injecter
static volatile int g_pendingLoad = -1;  // web/init   -> loop() : index de jeu à (re)charger
static volatile bool g_ready = false;    // un jeu est chargé + émulateur tourne ?
static char         g_status[96] = "booting...";

static inline void dacOut(int16_t s) {
  uint16_t v = (uint16_t)(((int32_t)s + 32768) >> 4) & 0x0FFF;     // 16-bit signé -> 12-bit non signé
  dacspi.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
  digitalWrite(DAC_CS, LOW);
  dacspi.transfer16(0x3000 | v);                                   // 0x3000 = cfg MCP4921 (DAC-A,1x,actif)
  digitalWrite(DAC_CS, HIGH);
  dacspi.endTransaction();
}

static uint8_t* loadFile(const char* path, size_t& len) {
  File f = SD_MMC.open(path); if (!f) { len = 0; return nullptr; }
  len = f.size(); uint8_t* b = (uint8_t*)malloc(len);
  if (b && f.read(b, len) != (int)len) { free(b); b = nullptr; len = 0; }
  f.close(); return b;
}

// Lit /games.idx -> g_games[] (lignes "short|gen|title"). Retourne le nombre de jeux.
static int parseManifest() {
  File f = SD_MMC.open("/games.idx");
  if (!f) { snprintf(g_status, sizeof(g_status), "games.idx absent (relancer make_sd.sh)"); return 0; }
  g_nGames = 0;
  while (f.available() && g_nGames < MAXGAMES) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.length() == 0) continue;
    int p1 = line.indexOf('|'); int p2 = line.indexOf('|', p1 + 1);
    if (p1 < 0 || p2 < 0) continue;
    Game& g = g_games[g_nGames];
    String id = line.substring(0, p1), gn = line.substring(p1 + 1, p2), ti = line.substring(p2 + 1);
    strncpy(g.id, id.c_str(), sizeof(g.id) - 1);   g.id[sizeof(g.id) - 1] = 0;
    g.gen = (uint8_t)gn.toInt();
    strncpy(g.title, ti.c_str(), sizeof(g.title) - 1); g.title[sizeof(g.title) - 1] = 0;
    g_nGames++;
  }
  f.close();
  return g_nGames;
}

// Charge le jeu idx : lit ses .snd, démarre l'émulateur avec le bon mapping (Gen). Appelé UNIQUEMENT
// depuis loop() (core 1). Les buffers fichiers sont temporaires : begin() les recopie en interne.
static bool loadGame(int idx) {
  if (idx < 0 || idx >= g_nGames) return false;
  const Game& gm = g_games[idx];
  snprintf(g_status, sizeof(g_status), "chargement %s...", gm.title);
  char p[80]; size_t yl, yl2 = 0, dl;
  snprintf(p, sizeof(p), "/games/%s/yrom1.snd", gm.id); uint8_t* y1 = loadFile(p, yl);
  snprintf(p, sizeof(p), "/games/%s/yrom2.snd", gm.id); uint8_t* y2 = loadFile(p, yl2);   // optionnel
  snprintf(p, sizeof(p), "/games/%s/drom1.snd", gm.id); uint8_t* d  = loadFile(p, dl);
  if (!y1 || !d) {
    snprintf(g_status, sizeof(g_status), "%s: ROMs manquantes (y=%u d=%u)", gm.id, (unsigned)yl, (unsigned)dl);
    if (y1) free(y1); if (y2) free(y2); if (d) free(d);
    return false;
  }
  psorom::Board board = (gm.gen == 1) ? psorom::GTS80B_GEN1
                      : (gm.gen == 2) ? psorom::GTS80B_GEN2 : psorom::GTS80B_GEN3;
  uint8_t* yrom; size_t ylen; uint8_t* yconcat = nullptr;
  if (gm.gen == 1 && y2 && yl2) {                       // Gen1 avec yrom2 : Y-CPU = yrom1 ++ yrom2
    ylen = yl + yl2; yconcat = (uint8_t*)malloc(ylen);
    if (!yconcat) { snprintf(g_status, sizeof(g_status), "%s: malloc Y FAILED", gm.id); free(y1); free(y2); free(d); return false; }
    memcpy(yconcat, y1, yl); memcpy(yconcat + yl, y2, yl2); yrom = yconcat; ylen = yl + yl2;
  } else {                                              // Gen1 sans yrom2 (ex. Raven), Gen2, Gen3
    yrom = y1; ylen = yl;
  }
  bool ok = psorom::begin(board, yrom, ylen, d, dl);
  psorom::command(0); psorom::run(20000);               // amorçage (80B ignore le 1er octet)
  if (yconcat) free(yconcat);
  free(y1); if (y2) free(y2); free(d);                  // begin() a recopié -> on libère les temporaires
  if (!ok) { snprintf(g_status, sizeof(g_status), "%s: begin() FAILED", gm.id); return false; }
  snprintf(g_status, sizeof(g_status), "OK %s (Gen%d)", gm.title, gm.gen);
  Serial.printf("loaded %-10s Gen%d  y=%u%s d=%u\n", gm.id, gm.gen, (unsigned)yl, (y2 ? "+yrom2" : ""), (unsigned)dl);
  return true;
}

static const char* PAGE = R"HTML(<!doctype html><html lang=fr><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>GOSOWAV PSOROM</title>
<style>body{background:#0d1017;color:#e7ecf3;font-family:system-ui;margin:0;padding:1rem}
h1{font-size:1.05rem;margin:.2rem 0}.s{color:#39b6ff;font-variant-numeric:tabular-nums}
select{background:#1d2430;color:#e7ecf3;border:1px solid #2a3340;border-radius:8px;padding:.5rem;font-size:1rem;width:100%;margin:.4rem 0}
.g{display:grid;grid-template-columns:repeat(8,1fr);gap:6px;margin-top:.8rem}
button{background:#1d2430;color:#e7ecf3;border:1px solid #2a3340;border-radius:8px;padding:.7rem 0;font-size:1rem;cursor:pointer}
button:active{background:#39b6ff;color:#04243a}.n{color:#8b97a8;font-size:.78rem;margin-top:1rem}</style></head><body>
<h1>GOSOWAV &middot; PSOROM <span class=n>(notre 6502 emule sur ton hardware)</span></h1>
<label class=n>Jeu</label><select id=game></select>
<div>etat <b class=s id=st>...</b></div>
<div>Debit <b class=s id=thr>--</b> M cyc/s &middot; DAC <b class=s id=dac>0</b> &middot; YM <b class=s id=ym>0</b> &middot; cmd <b class=s id=cmd>--</b></div>
<div class=g id=pad></div>
<div class=n>Choisis un jeu, puis clique une commande son (0-31) pour jouer le son du vrai 6502.</div>
<script>const pad=document.getElementById('pad'),sel=document.getElementById('game');
for(let i=0;i<32;i++){const b=document.createElement('button');b.textContent=i;b.onclick=()=>fetch('/cmd?n='+i);pad.appendChild(b);}
fetch('/games').then(r=>r.json()).then(d=>{sel.innerHTML='';d.g.forEach(x=>{const o=document.createElement('option');o.value=x.i;o.textContent='G'+x.n+'  '+x.t;if(x.i==d.sel)o.selected=true;sel.appendChild(o);});});
sel.onchange=()=>fetch('/load?i='+sel.value);
function poll(){fetch('/status').then(r=>r.json()).then(d=>{st.textContent=d.st;thr.textContent=d.thr;dac.textContent=d.dac;ym.textContent=d.ym;cmd.textContent=d.cmd<0?'--':d.cmd;}).catch(()=>{});}
setInterval(poll,500);poll();</script></body></html>)HTML";

// HTTP servi sur SA tâche FreeRTOS (core 0 = cœur WiFi) -> page joignable dès l'AP, indépendamment
// de loop()/SD. Les handlers ne posent que des intentions (volatile) ; loop() (core 1) les applique.
static void httpTask(void*) { for (;;) { server.handleClient(); vTaskDelay(1); } }

static void startWeb() {
  WiFi.mode(WIFI_AP); WiFi.softAP("GOSOWAV-PSOROM");
  Serial.printf("web: join WiFi 'GOSOWAV-PSOROM' -> http://%s/\n", WiFi.softAPIP().toString().c_str());
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/cmd", []() {
    if (server.hasArg("n")) { int n = server.arg("n").toInt(); if (n >= 0 && n <= 95) { g_pendingCmd = n; g_lastCmd = n; } }
    server.send(200, "text/plain", "ok");
  });
  server.on("/load", []() {
    if (server.hasArg("i")) { int i = server.arg("i").toInt(); if (i >= 0 && i < g_nGames) g_pendingLoad = i; }
    server.send(200, "text/plain", "ok");
  });
  server.on("/games", []() {                                       // liste des jeux + sélection courante
    String j = "{\"sel\":" + String(g_sel) + ",\"g\":[";
    for (int i = 0; i < g_nGames; i++) {
      if (i) j += ",";
      j += "{\"i\":" + String(i) + ",\"n\":" + String(g_games[i].gen) + ",\"t\":\"" + g_games[i].title + "\"}";
    }
    j += "]}";
    server.send(200, "application/json", j);
  });
  server.on("/status", []() {
    char b[200]; snprintf(b, sizeof(b), "{\"thr\":%.2f,\"dac\":%u,\"ym\":%u,\"cmd\":%d,\"st\":\"%s\"}",
             g_thrM, (unsigned)psorom::dacCount(), (unsigned)psorom::ymWrites(), g_lastCmd, g_status);
    server.send(200, "application/json", b);
  });
  server.begin();
  xTaskCreatePinnedToCore(httpTask, "http", 4096, nullptr, 1, nullptr, 0);   // core 0 = cœur WiFi
}

// Monte la SD + lit le manifeste dans SA tâche (un SD bloqué ne gèle ni loop ni le web), puis
// demande le chargement du jeu par défaut via g_pendingLoad (loop() fait le vrai chargement).
static void sdInitTask(void*) {
  snprintf(g_status, sizeof(g_status), "SD: montage 1-bit...");
  bool ok = SD_MMC.begin("/sdcard", true);                         // 1-bit : robuste, libère GPIO12 (strap)
  if (!ok) { snprintf(g_status, sizeof(g_status), "SD: montage 4-bit..."); ok = SD_MMC.begin(); }
  if (!ok) { snprintf(g_status, sizeof(g_status), "SD: pas de carte / FAT32 ?"); Serial.println(g_status); vTaskDelete(nullptr); return; }
  Serial.println("SD mounted.");
  if (parseManifest() == 0) { Serial.println(g_status); vTaskDelete(nullptr); return; }
  Serial.printf("manifeste : %d jeux. Defaut -> %s\n", g_nGames, g_games[0].title);
  g_pendingLoad = 0;                                               // loop() charge le 1er jeu (mono-thread émulateur)
  vTaskDelete(nullptr);
}

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n=== GOSOWAV PSOROM multi-jeux (notre 6502 emu sur hardware reel) ===");
  pinMode(DAC_CS, OUTPUT); digitalWrite(DAC_CS, HIGH);
  dacspi.begin(DAC_SCK, -1, DAC_SDI, DAC_CS);
  dacOut(0);                                                       // mi-échelle = silence
  startWeb();                                                      // AP + tâche web EN PREMIER
  xTaskCreatePinnedToCore(sdInitTask, "sdinit", 8192, nullptr, 1, nullptr, 1);
  Serial.println("web up (core 0); SD/manifeste dans sa tache. Choix du jeu sur la page.");
}

void loop() {
  if (g_pendingLoad >= 0) {                                        // (re)chargement de jeu demandé
    int i = g_pendingLoad; g_pendingLoad = -1;
    g_ready = false;
    if (loadGame(i)) {
      g_sel = i;
      if (!g_benched) {                                            // bench débit une seule fois
        g_benched = true;
        psorom::command(22);
        uint32_t t0 = millis(), cyc = 0;
        while (millis() - t0 < 1000) { cyc += psorom::run(200000); vTaskDelay(0); }
        g_thrM = cyc / 1e6f;
        Serial.printf("THROUGHPUT: %.2f M 6502-cycles/sec   (80B temps-reel ~2.0 M)\n", g_thrM);
      }
      g_ready = true;
    }
    return;
  }
  if (!g_ready) { vTaskDelay(5 / portTICK_PERIOD_MS); return; }    // pas de jeu chargé : le web reste vivant
  int pc = g_pendingCmd;                                           // commande web/série (handoff lock-free)
  if (pc >= 0) { g_pendingCmd = -1; psorom::command((uint8_t)pc); }
  psorom::run(800);
  int16_t buf[128]; int n = psorom::dacDrain(buf, 128);
  for (int i = 0; i < n; i++) dacOut(buf[i]);
  if (Serial.available()) { int c = Serial.parseInt(); if (c >= 0 && c <= 95) { g_pendingCmd = c; Serial.printf("-> cmd %d\n", c); } }
}
#endif // GOSOWAV_BENCH
