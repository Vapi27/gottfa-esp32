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
#include <WiFi.h>
#include <WebServer.h>
#include <string.h>
#include <stdio.h>
#include "driver/sdmmc_host.h"   // montage SD via l'IDF (comme Ralf), avec SDMMC_SLOT_FLAG_INTERNAL_PULLUP
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"           // esp_timer_get_time() : horloge us pour le cadencement temps-reel
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
static volatile int g_vol = 100;         // volume maitre % (0..200, 100 = normal) applique au DAC

static inline void dacOut(int16_t s) {
  int32_t x = ((int32_t)s * g_vol) / 100;                          // volume maitre (autour de 0 = silence)
  if (x > 32767) x = 32767; else if (x < -32768) x = -32768;       // clamp anti-wrap si boost > 100%
  uint16_t v = (uint16_t)((x + 32768) >> 4) & 0x0FFF;              // 16-bit signé -> 12-bit non signé
  dacspi.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));
  digitalWrite(DAC_CS, LOW);
  dacspi.transfer16(0x3000 | v);                                   // 0x3000 = cfg MCP4921 (DAC-A,1x,actif)
  digitalWrite(DAC_CS, HIGH);
  dacspi.endTransaction();
}

static const char* MP = "/sdcard";          // point de montage VFS
static sdmmc_card_t* s_card = nullptr;

// Monte la SD via l'API IDF (comme le firmware de Ralf), avec SDMMC_SLOT_FLAG_INTERNAL_PULLUP — le
// wrapper Arduino SD_MMC NE met PAS ce flag, d'ou l'echec sur cette carte (zero pull-up externe).
// Pins slot-1 fixes en IOMUX sur ESP32 classique (CLK14/CMD15/D0=2/D1=4/D2=12/D3=13) -> pas a regler.
static bool mountSD() {
  esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
  mcfg.format_if_mount_failed = false;       // surtout pas : on garde les 16 jeux
  mcfg.max_files = 12;                        // = FAT_MAX_FILES de Ralf
  mcfg.allocation_unit_size = 0;
  struct { int width; int khz; const char* tag; } ladder[] = {
    { 4, SDMMC_FREQ_HIGHSPEED, "4-bit 40MHz" },   // config exacte de Ralf
    { 4, SDMMC_FREQ_DEFAULT,   "4-bit 20MHz" },
    { 1, SDMMC_FREQ_DEFAULT,   "1-bit 20MHz" },
    { 1, SDMMC_FREQ_PROBING,   "1-bit 400kHz" },  // repli le plus robuste
  };
  for (int i = 0; i < (int)(sizeof(ladder) / sizeof(ladder[0])); i++) {
    snprintf(g_status, sizeof(g_status), "SD: montage %s...", ladder[i].tag);
    Serial.printf("SD: tentative %s\n", ladder[i].tag);
    // PAS de unmount ici : esp_vfs_fat_sdmmc_mount() nettoie deja seul en cas d'echec, et appeler
    // esp_vfs_fat_sdmmc_unmount() quand rien n'est monte crashe (null deref dans call_host_deinit, IDF 4.x).
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = ladder[i].khz;
    if (ladder[i].width == 1) host.flags = SDMMC_HOST_FLAG_1BIT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width  = ladder[i].width;
    // IDF 4.x (Arduino-ESP32) : les broches SDMMC slot-1 sont FIXES en IOMUX (CLK14/CMD15/D0=2/D1=4/D2=12/
    // D3=13), pas de champs d1/d2/d3 a assigner (contrairement a l'IDF 5.x de Ralf). Le seul vrai delta avec
    // le wrapper Arduino qui echoue, c'est le flag pull-up interne ci-dessous (la carte n'a aucun pull-up externe).
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;   // = ce que Ralf met (slot.flags |= ... bloc WROVER)
    esp_err_t e = esp_vfs_fat_sdmmc_mount(MP, &host, &slot, &mcfg, &s_card);
    if (e == ESP_OK) {
      snprintf(g_status, sizeof(g_status), "SD: monte (%s)", ladder[i].tag);
      Serial.printf("SD mounted (%s).\n", ladder[i].tag);
      if (s_card) sdmmc_card_print_info(stdout, s_card);
      return true;
    }
    Serial.printf("  -> echec 0x%x (%s)\n", e, esp_err_to_name(e));
  }
  return false;
}

static uint8_t* loadFile(const char* rel, size_t& len) {     // rel = "/games/<id>/yrom1.snd"
  char full[96]; snprintf(full, sizeof(full), "%s%s", MP, rel);
  FILE* f = fopen(full, "rb"); if (!f) { len = 0; return nullptr; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n <= 0) { fclose(f); len = 0; return nullptr; }
  uint8_t* b = (uint8_t*)malloc(n);
  if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = nullptr; n = 0; }
  fclose(f); len = (size_t)n; return b;
}

// Lit /games.idx -> g_games[] (lignes "short|gen|title"). Retourne le nombre de jeux.
static int parseManifest() {
  char full[64]; snprintf(full, sizeof(full), "%s/games.idx", MP);
  FILE* f = fopen(full, "r");
  if (!f) { snprintf(g_status, sizeof(g_status), "games.idx absent (relancer make_sd.sh)"); return 0; }
  g_nGames = 0; char line[160];
  while (g_nGames < MAXGAMES && fgets(line, sizeof(line), f)) {
    char* nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;            // strip EOL
    if (!line[0]) continue;
    char* p1 = strchr(line, '|'); if (!p1) continue; *p1 = 0;
    char* p2 = strchr(p1 + 1, '|'); if (!p2) continue; *p2 = 0;
    Game& g = g_games[g_nGames];
    strncpy(g.id, line, sizeof(g.id) - 1);          g.id[sizeof(g.id) - 1] = 0;
    g.gen = (uint8_t)atoi(p1 + 1);
    strncpy(g.title, p2 + 1, sizeof(g.title) - 1);  g.title[sizeof(g.title) - 1] = 0;
    g_nGames++;
  }
  fclose(f);
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
  if (gm.gen == 1 && y2 && yl2) {                       // Gen1 (GTS80BSSOUND888) : yrom2 @0xC000, yrom1 @0xE000
    ylen = yl + yl2; yconcat = (uint8_t*)malloc(ylen); // -> Y-CPU = yrom2 ++ yrom1 (yrom1 porte les vecteurs @0xFFFx)
    if (!yconcat) { snprintf(g_status, sizeof(g_status), "%s: malloc Y FAILED", gm.id); free(y1); free(y2); free(d); return false; }
    memcpy(yconcat, y2, yl2); memcpy(yconcat + yl2, y1, yl); yrom = yconcat; ylen = yl + yl2;
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
<div>Volume <b class=s id=volv>100</b>%<input type=range min=0 max=200 value=100 id=vol style="width:100%"></div>
<div class=g id=pad></div>
<div class=n>Choisis un jeu, puis clique une commande son (0-31) pour jouer le son du vrai 6502.</div>
<script>const pad=document.getElementById('pad'),sel=document.getElementById('game'),vol=document.getElementById('vol'),volv=document.getElementById('volv');
vol.oninput=()=>{volv.textContent=vol.value;fetch('/vol?v='+vol.value);};
for(let i=0;i<32;i++){const b=document.createElement('button');b.textContent=i;b.onclick=()=>fetch('/cmd?n='+i);pad.appendChild(b);}
fetch('/games').then(r=>r.json()).then(d=>{sel.innerHTML='';d.g.forEach(x=>{const o=document.createElement('option');o.value=x.i;o.textContent='G'+x.n+'  '+x.t;if(x.i==d.sel)o.selected=true;sel.appendChild(o);});});
sel.onchange=()=>fetch('/load?i='+sel.value);
function poll(){fetch('/status').then(r=>r.json()).then(d=>{st.textContent=d.st;thr.textContent=d.thr;dac.textContent=d.dac;ym.textContent=d.ym;cmd.textContent=d.cmd<0?'--':d.cmd;if(document.activeElement!=vol){vol.value=d.vol;volv.textContent=d.vol;}}).catch(()=>{});}
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
  server.on("/vol", []() {                                         // volume maitre 0..200 %
    if (server.hasArg("v")) { int v = server.arg("v").toInt(); if (v >= 0 && v <= 200) g_vol = v; }
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
    char b[220]; snprintf(b, sizeof(b), "{\"thr\":%.2f,\"dac\":%u,\"ym\":%u,\"cmd\":%d,\"vol\":%d,\"st\":\"%s\"}",
             g_thrM, (unsigned)psorom::dacCount(), (unsigned)psorom::ymWrites(), g_lastCmd, g_vol, g_status);
    server.send(200, "application/json", b);
  });
  server.begin();
  xTaskCreatePinnedToCore(httpTask, "http", 4096, nullptr, 1, nullptr, 0);   // core 0 = cœur WiFi
}

// Monte la SD + lit le manifeste dans SA tâche (un SD bloqué ne gèle ni loop ni le web), puis
// demande le chargement du jeu par défaut via g_pendingLoad (loop() fait le vrai chargement).
static void sdInitTask(void*) {
  if (!mountSD()) { snprintf(g_status, sizeof(g_status), "SD: echec montage (voir serie)"); Serial.println(g_status); vTaskDelete(nullptr); return; }
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
  static int64_t s_t0 = 0, s_emu = 0;                              // base temps-reel (us) + ticks combines emules
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
      s_t0 = esp_timer_get_time(); s_emu = 0;                      // (re)cale le temps-reel sur ce jeu
      g_ready = true;
    }
    return;
  }
  if (!g_ready) { vTaskDelay(5 / portTICK_PERIOD_MS); return; }    // pas de jeu chargé : le web reste vivant
  int pc = g_pendingCmd;                                           // commande web/série (handoff lock-free)
  if (pc >= 0) { g_pendingCmd = -1; psorom::command((uint8_t)pc); }

  // --- MIXEUR A CADENCE FIXE (Fs) : on sort des echantillons mixes (DAC maintenu + AY) a la frequence
  // Fs ; renderMix() fait avancer l'emulateur a ~1 MHz (temps-reel) en interne. On cale la sortie sur
  // Fs via esp_timer -> pitch correct + AY mixe. (DAC seul si Gen3 sans AY, identique a avant.)
  const int FS = psorom::ayFs();
  int64_t now = esp_timer_get_time();
  int64_t target = (now - s_t0) * FS / 1000000;                    // nb d'echantillons dus a l'instant
  if (target - s_emu > (int64_t)FS * 2) { s_t0 = now; s_emu = 0; } // >2s de retard -> recale
  else if (s_emu < target) {
    int16_t buf[32]; int n = psorom::renderMix(buf, 32);           // DAC + AY, 32 ech.
    for (int i = 0; i < n; i++) dacOut(buf[i]);
    s_emu += n;
  } else {
    delayMicroseconds(150);                                        // en avance -> on rend la main
  }
  if (Serial.available()) { int c = Serial.parseInt(); if (c >= 0 && c <= 95) { g_pendingCmd = c; Serial.printf("-> cmd %d\n", c); } }
}
#endif // GOSOWAV_BENCH
