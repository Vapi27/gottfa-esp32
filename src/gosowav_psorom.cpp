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
#include "soc/gpio_struct.h"     // GPIO.out_w1ts/w1tc : bit-bang du MCP4921 en ISR (comme Ralf)
#include "psorom.h"
#include "wavmix.h"              // PSOWAV : moteur de mixage WAV (clean-room, hybride par defaut)
#include "wavfile.h"
#include "wavsrc.h"
#include "wavset.h"
#include <dirent.h>
#include <Preferences.h>        // NVS : jeu installe + volume persistes (montage physique en machine)

// MCP4921 sur la WROVER GOSOWAV (fait matériel de la carte) : SCK=18, SDI/MOSI=23, CS=5.
static const int DAC_SCK = 18, DAC_SDI = 23, DAC_CS = 5;

static WebServer server(80);

#define MAXGAMES 32
struct Game { char id[16]; uint8_t gen; char title[40]; };
static Game  g_games[MAXGAMES];
static int   g_nGames = 0;
static int   g_sel = 0;                  // jeu courant (index dans g_games)

static float        g_thrM = 0;          // débit mesuré (M 6502-cycles/s)
static uint32_t     g_mixSps = 0;        // débit renderMix (echantillons/s ; besoin = Fs pour temps-reel)
static bool         g_benched = false;   // bench déjà fait ?
static volatile int g_lastCmd = -1;      // dernière commande son
static volatile int g_pendingCmd = -1;   // web/série -> loop() : commande à injecter
static volatile int g_pendingLoad = -1;  // web/init   -> loop() : index de jeu à (re)charger
static volatile bool g_ready = false;    // un jeu est chargé + émulateur tourne ?
static char         g_status[96] = "booting...";
static volatile int g_vol = 100;         // volume maitre % (0..200, 100 = normal) applique au DAC
static Preferences  g_prefs;             // NVS : jeu installe ("game") + volume ("vol")

// --- Sortie audio cadencee par TIMER MATERIEL (comme Ralf) : un ring SPSC rempli par loop(), vide
// --- par une ISR a Fs exact qui bit-bang le MCP4921. Fini les rafales SPI -> flux DAC regulier = son
// --- propre. (producteur = loop core1, consommateur = ISR core1 -> meme coeur, pas de race.)
#define ARING 2048
static volatile int16_t  s_ring[ARING];
static volatile uint32_t s_rHead = 0, s_rTail = 0;   // head = producteur (loop), tail = conso (ISR)
static hw_timer_t*       s_dacTimer = nullptr;

static inline void IRAM_ATTR dacBitbang(uint16_t word) {           // MCP4921, MSB first, SCK18/MOSI23/CS5
  GPIO.out_w1tc = (1u << DAC_CS);                                  // CS bas
  for (int b = 15; b >= 0; b--) {
    if (word & (1u << b)) GPIO.out_w1ts = (1u << DAC_SDI); else GPIO.out_w1tc = (1u << DAC_SDI);
    GPIO.out_w1ts = (1u << DAC_SCK); GPIO.out_w1tc = (1u << DAC_SCK);  // front montant SCK
  }
  GPIO.out_w1ts = (1u << DAC_CS);                                  // CS haut -> latch
}

static void IRAM_ATTR onDacTimer() {                              // tire 1 echantillon du ring a chaque tick Fs
  uint32_t t = s_rTail;
  static int16_t held = 0;
  if (t != s_rHead) { held = s_ring[t]; s_rTail = (t + 1) & (ARING - 1); }  // sinon : maintient (sample&hold)
  int32_t x = ((int32_t)held * g_vol) / 100;                      // volume maitre
  if (x > 32767) x = 32767; else if (x < -32768) x = -32768;
  dacBitbang(0x3000 | (uint16_t)(((x + 32768) >> 4) & 0x0FFF));
}

static inline bool ringFull()  { return ((s_rHead + 1) & (ARING - 1)) == s_rTail; }
static inline void ringPush(int16_t s) { uint32_t h = s_rHead; s_ring[h] = s; s_rHead = (h + 1) & (ARING - 1); }

// --- Bus commande son d'une vraie machine Gottlieb System 80 (via ULN2803 IC2). Méthode de Ralf
// (if_G80.c, prouvée sur cette carte) : PAS de strobe — on déclenche sur le CHANGEMENT des 5 bits de
// données (S1_A=27, S2_B=26, S4_C=25, S8_D=33, S16_E=32 = bits 0..4), settle, puis on lit. En plus :
// POLARITÉ AUTO — on échantillonne le niveau de repos au boot (s_idle) et on compare par XOR, donc ça
// marche que l'ULN inverse ou non. 0 = repos ; bits != repos = commande. (Strobe/F non utilisés.)
static const int PIN_S[5] = {27, 26, 25, 33, 32};
static const int PIN_STROBE = 34;
static uint8_t s_idle = 0;                                         // niveau repos des 5 bits (polarité auto)
static uint8_t s_lastBus = 0;
static inline uint8_t readBus5() { uint8_t v = 0; for (int i = 0; i < 5; i++) if (digitalRead(PIN_S[i])) v |= (1u << i); return v; }
static void cmdInputBegin() {
  for (int i = 0; i < 5; i++) pinMode(PIN_S[i], INPUT);
  pinMode(35, INPUT); pinMode(PIN_STROBE, INPUT);
  delay(50); s_idle = readBus5(); s_lastBus = 0;                   // repos -> polarité-agnostique (XOR)
  Serial.printf("cmd-input: repos bus = 0x%02X (polarite auto, pas de strobe)\n", s_idle);
}
static inline void cmdInputPoll() {                                // comme if_G80 : declenche au changement des bits
  uint8_t c = (uint8_t)(readBus5() ^ s_idle);                      // bits differents du repos = commande
  if (c != 0 && s_lastBus == 0) {                                  // repos -> commande
    delayMicroseconds(30);                                         // settle (de-skew des 5 bits)
    uint8_t c2 = (uint8_t)(readBus5() ^ s_idle);
    if (c2 != 0) { g_pendingCmd = c2; g_lastCmd = c2; }
    s_lastBus = c2;
  } else s_lastBus = c;                                            // re-arme au retour au repos
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

// ---- PSOWAV : set WAV par jeu (hybride : si un dossier /sdcard/<jeu>/ existe -> PSOWAV par defaut) ----
static wavmix::Mixer g_mixer;
static wavset::Set   g_set;
static char          g_theme[24] = "";
static volatile bool g_psowav = false;                 // un set WAV est-il charge pour ce jeu ?
// ROM-chef : le CPU (logique seule) pilote le lecteur WAV (start/loop/stop) selon son comportement reel.
static volatile int  g_chefReq = -1;                   // commande a (re)jouer : web/serie/materiel -> chef
static int           g_chefId = -1, g_chefSlot = -1;   // son courant pilote + son slot WAV
static int           g_curGen = 3;                     // generation du jeu (cadence CPU : Gen1=1MHz, Gen2/3=2MHz/CPU)
static uint32_t      g_actVal = 0, g_actMs = 0;        // suivi activite CPU (sustain/idle)
static int64_t       g_chefT0 = 0, g_chefEmu = 0;      // cadencement temps-reel du CPU
struct WSlot { FILE* f; wavsrc::Source src; uint32_t dataOff, dataLen; uint8_t ch; int vid; bool used; };
static WSlot s_ws[wavmix::MAX_VOICES];

static size_t wfRead(void* ctx, uint8_t* d, size_t n) { return fread(d, 1, n, (FILE*)ctx); }
static size_t wsFill(void* p, int16_t* dst, size_t fr) { return wavsrc::fill(&((WSlot*)p)->src, dst, fr); }
static bool   wsRewind(void* p) { WSlot* s = (WSlot*)p; if (!s->f || fseek(s->f, s->dataOff, SEEK_SET)) return false;
                                  wavsrc::init(s->src, wfRead, s->f, s->ch, s->dataLen); return true; }

// Demarre le WAV du son `id` en ONE-SHOT (le ROM-chef decidera de reboucler). Retourne le slot, ou -1.
static int wavTrigger(int id) {
  const wavset::Entry* e = g_set.find(id); if (!e || (e->attr & wavset::A_PLACE)) return -1;
  for (int i = 0; i < wavmix::MAX_VOICES; i++)            // libere les voix terminees
    if (s_ws[i].used && !g_mixer.active(s_ws[i].vid)) { fclose(s_ws[i].f); s_ws[i].used = false; }
  int si = -1; for (int i = 0; i < wavmix::MAX_VOICES; i++) if (!s_ws[i].used) { si = i; break; }
  if (si < 0) return -1;
  char path[96]; snprintf(path, sizeof(path), "%s/%s/%s", MP, g_theme, e->file);
  FILE* f = fopen(path, "rb"); if (!f) return -1;
  uint8_t hdr[64]; size_t got = fread(hdr, 1, sizeof(hdr), f);
  WavInfo wi = wav_parse(hdr, got);
  if (!wi.ok || fseek(f, wi.dataOffset, SEEK_SET)) { fclose(f); return -1; }
  s_ws[si].f = f; s_ws[si].dataOff = wi.dataOffset; s_ws[si].dataLen = wi.dataLen; s_ws[si].ch = (uint8_t)wi.channels;
  wavsrc::init(s_ws[si].src, wfRead, f, (uint8_t)wi.channels, wi.dataLen);
  wavmix::VoiceCfg vc; vc.fill = wsFill; vc.ctx = &s_ws[si]; vc.rewind = wsRewind;
  vc.gain = 255; vc.tag = id; vc.loop = false;            // one-shot : le chef rejoue si le CPU entretient le son
  int vid = g_mixer.trigger(vc); if (vid < 0) { fclose(f); return -1; }
  s_ws[si].vid = vid; s_ws[si].used = true; return si;
}
static void wavStopSlot(int si) { if (si >= 0 && s_ws[si].used) { g_mixer.stop(s_ws[si].vid); fclose(s_ws[si].f); s_ws[si].used = false; } }
static bool wavSlotPlaying(int si) { return si >= 0 && s_ws[si].used && g_mixer.active(s_ws[si].vid); }

static void loadWavSet(const char* theme) {              // scanne /sdcard/<theme>/ -> g_set ; g_psowav si non vide
  g_mixer.stopAll(); g_mixer.setMix(wavmix::MIX_DIV2);
  for (int i = 0; i < wavmix::MAX_VOICES; i++) if (s_ws[i].used) { fclose(s_ws[i].f); s_ws[i].used = false; }
  g_set.reset(); strncpy(g_theme, theme, sizeof(g_theme) - 1); g_theme[sizeof(g_theme) - 1] = 0;
  g_psowav = false;
  char dp[64]; snprintf(dp, sizeof(dp), "%s/%s", MP, theme);
  DIR* dir = opendir(dp);
  if (dir) { struct dirent* de; while ((de = readdir(dir))) g_set.addName(de->d_name); closedir(dir); g_psowav = g_set.nEntry > 0; }
  Serial.printf("PSOWAV set '%s' : %d sons -> %s\n", theme, g_set.nEntry, g_psowav ? "PSOWAV" : "PSOROM (pas de WAV)");
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
  loadWavSet(gm.id);                                    // set WAV du jeu (hybride) ; PSOROM reste pret en repli
  g_curGen = gm.gen;                                     // cadence CPU du ROM-chef
  g_chefId = g_chefSlot = -1; g_chefReq = -1; g_chefT0 = esp_timer_get_time(); g_chefEmu = 0;
  g_actVal = psorom::activity(); g_actMs = millis();
  snprintf(g_status, sizeof(g_status), "OK %s (Gen%d, %s)", gm.title, gm.gen, g_psowav ? "ROM-chef+WAV" : "PSOROM");
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
<div>Debit <b class=s id=thr>--</b> M cyc/s &middot; mix <b class=s id=mix>--</b>/s &middot; DAC <b class=s id=dac>0</b> &middot; YM <b class=s id=ym>0</b> &middot; cmd <b class=s id=cmd>--</b></div>
<div class=n>bus cmd materiel (brut, live) : <b class=s id=bus>--</b> &middot; regarde-le changer quand tu injectes une commande sur la carte</div>
<div>Volume <b class=s id=volv>100</b>%<input type=range min=0 max=200 value=100 id=vol style="width:100%"></div>
<div class=g id=pad></div>
<div class=n>cmd libre <input type=number id=cmdn min=0 max=255 value=32 style="width:4rem;background:#1d2430;color:#e7ecf3;border:1px solid #2a3340;border-radius:6px"> <button onclick="fetch('/cmd?n='+cmdn.value)">envoyer</button> &middot; ou clique le pave (0-63)</div>
<script>const pad=document.getElementById('pad'),sel=document.getElementById('game'),vol=document.getElementById('vol'),volv=document.getElementById('volv');
vol.oninput=()=>{volv.textContent=vol.value;fetch('/vol?v='+vol.value);};
for(let i=0;i<64;i++){const b=document.createElement('button');b.textContent=i;b.onclick=()=>fetch('/cmd?n='+i);pad.appendChild(b);}
fetch('/games').then(r=>r.json()).then(d=>{sel.innerHTML='';d.g.forEach(x=>{const o=document.createElement('option');o.value=x.i;o.textContent='G'+x.n+'  '+x.t;if(x.i==d.sel)o.selected=true;sel.appendChild(o);});});
sel.onchange=()=>fetch('/load?i='+sel.value);
function poll(){fetch('/status').then(r=>r.json()).then(d=>{st.textContent=d.st;thr.textContent=d.thr;mix.textContent=d.mix;dac.textContent=d.dac;ym.textContent=d.ym;cmd.textContent=d.cmd<0?"--":d.cmd;bus.textContent=d.bus;if(document.activeElement!=vol){vol.value=d.vol;volv.textContent=d.vol;}}).catch(()=>{});}
setInterval(poll,500);poll();</script></body></html>)HTML";

// HTTP servi sur SA tâche FreeRTOS (core 0 = cœur WiFi) -> page joignable dès l'AP, indépendamment
// de loop()/SD. Les handlers ne posent que des intentions (volatile) ; loop() (core 1) les applique.
static void httpTask(void*) { for (;;) { server.handleClient(); vTaskDelay(1); } }

static void startWeb() {
  WiFi.mode(WIFI_AP); WiFi.softAP("GOSOWAV-PSOROM");
  Serial.printf("web: join WiFi 'GOSOWAV-PSOROM' -> http://%s/\n", WiFi.softAPIP().toString().c_str());
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/cmd", []() {
    if (server.hasArg("n")) { int n = server.arg("n").toInt(); if (n >= 0 && n <= 255) { g_pendingCmd = n; g_lastCmd = n; } }
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
    char b[260]; snprintf(b, sizeof(b),
             "{\"thr\":%.2f,\"mix\":%u,\"dac\":%u,\"ym\":%u,\"cmd\":%d,\"vol\":%d,\"bus\":\"S%d%d%d%d%d F%d STB%d\",\"st\":\"%s\"}",
             g_thrM, g_mixSps, (unsigned)psorom::dacCount(), (unsigned)psorom::ymWrites(), g_lastCmd, g_vol,
             digitalRead(PIN_S[0]), digitalRead(PIN_S[1]), digitalRead(PIN_S[2]), digitalRead(PIN_S[3]), digitalRead(PIN_S[4]),
             digitalRead(35), digitalRead(PIN_STROBE), g_status);
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
  String gid = g_prefs.getString("game", "arena");                // jeu installe (defaut arena) -> auto-boot
  int idx = 0; for (int i = 0; i < g_nGames; i++) if (gid == g_games[i].id) { idx = i; break; }
  Serial.printf("manifeste : %d jeux. Jeu installe -> %s\n", g_nGames, g_games[idx].title);
  g_pendingLoad = idx;                                             // loop() charge le jeu de la machine (mono-thread émulateur)
  vTaskDelete(nullptr);
}

void setup() {
  Serial.begin(115200); delay(500);
  Serial.println("\n=== GOSOWAV PSOROM multi-jeux (notre 6502 emu sur hardware reel) ===");
  g_prefs.begin("gosowav", false); g_vol = g_prefs.getInt("vol", 100);   // reglages persistes (montage machine)
  pinMode(DAC_SCK, OUTPUT); pinMode(DAC_SDI, OUTPUT); pinMode(DAC_CS, OUTPUT);
  GPIO.out_w1ts = (1u << DAC_CS); GPIO.out_w1tc = (1u << DAC_SCK); // CS haut, SCK bas
  dacBitbang(0x3000 | 2048);                                       // mi-échelle = silence
  s_dacTimer = timerBegin(0, 2, true);                             // 80MHz/2 = 40 MHz
  timerAttachInterrupt(s_dacTimer, &onDacTimer, true);
  timerAlarmWrite(s_dacTimer, 40000000 / psorom::ayFs(), true);    // 40MHz/44100 = 907 -> 44.1 kHz
  timerAlarmEnable(s_dacTimer);                                    // -> flux DAC regulier des maintenant
  cmdInputBegin();                                                 // bus commande son (vraie machine System 80)
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
      g_prefs.putString("game", g_games[i].id);                    // memorise le jeu installe (auto-boot machine)
      if (!g_benched) {                                            // bench débit une seule fois
        g_benched = true;
        psorom::command(22);
        uint32_t t0 = millis(), cyc = 0;
        while (millis() - t0 < 1000) { cyc += psorom::run(200000); vTaskDelay(0); }
        g_thrM = cyc / 1e6f;
        Serial.printf("THROUGHPUT: %.2f M 6502-cycles/sec   (80B temps-reel ~2.0 M)\n", g_thrM);
        int16_t tb[64]; uint32_t t1 = millis(), smp = 0;          // debit renderMix (emu+puces) = temps-reel ?
        while (millis() - t1 < 500) smp += psorom::renderMix(tb, 64);
        g_mixSps = smp * 2;
        Serial.printf("renderMix: %u ech/s  (besoin %d = temps-reel%s)\n", g_mixSps, psorom::ayFs(),
                      g_mixSps >= (uint32_t)psorom::ayFs() ? " OK" : " TROP LENT !");
      }
      g_ready = true;
    }
    return;
  }
  if (!g_ready) { vTaskDelay(5 / portTICK_PERIOD_MS); return; }    // pas de jeu chargé : le web reste vivant
  cmdInputPoll();                                                  // bus commande matériel (vraie machine) -> g_pendingCmd
  int pc = g_pendingCmd;                                           // commande web/série/matériel (handoff lock-free)
  if (pc >= 0) { g_pendingCmd = -1;
    if (g_psowav) g_chefReq = pc;                                  // ROM-chef : CPU + WAV traités dans le mix
    else psorom::command((uint8_t)pc);                             // PSOROM live (pas de set WAV)
  }

  int guard = 0;
  if (g_psowav) {
    // --- ROM-CHEF : le CPU (logique seule, AUCUNE synthese) interprete la commande et PILOTE le WAV ---
    int tpu = (g_curGen == 1) ? 2 : 4;                             // ticks/us combine : Gen1=1MHz/CPU, Gen2/3=2MHz/CPU
    int64_t now = esp_timer_get_time();
    int64_t tgt = (now - g_chefT0) * tpu;
    if (tgt - g_chefEmu > (int64_t)tpu * 2000000) { g_chefT0 = now; g_chefEmu = 0; }   // recale si gros retard
    else { int r = 0; while (g_chefEmu < tgt && r++ < 64) g_chefEmu += psorom::run(64); }  // (a) avance le CPU au temps-reel
    if (g_chefReq >= 0) {                                          // (b) nouvelle commande -> CPU (comportement) + WAV
      wavStopSlot(g_chefSlot); psorom::command((uint8_t)g_chefReq);
      g_chefSlot = wavTrigger(g_chefReq); g_chefId = (g_chefSlot >= 0) ? g_chefReq : -1;
      g_chefReq = -1; g_actVal = psorom::activity(); g_actMs = millis();
    }
    uint32_t a = psorom::activity(); if (a != g_actVal) { g_actVal = a; g_actMs = millis(); }  // (c) activite CPU
    bool cpuActive = (millis() - g_actMs) < 120;
    if (g_chefId >= 0) {                                           // (d) le CPU pilote : WAV fini+CPU actif->loop ; CPU idle->stop
      if (!wavSlotPlaying(g_chefSlot)) { if (cpuActive) g_chefSlot = wavTrigger(g_chefId); else g_chefId = -1; }
      else if (!cpuActive) { wavStopSlot(g_chefSlot); g_chefSlot = -1; g_chefId = -1; }
    }
    static int16_t wb[wavmix::BLOCK * 2];                          // (e) ring <- wavmix (down-mix mono)
    while (!ringFull() && guard++ < 16) {
      g_mixer.mix(wb, wavmix::BLOCK);
      for (int i = 0; i < wavmix::BLOCK && !ringFull(); i++) ringPush((int16_t)((wb[2*i] + wb[2*i+1]) >> 1));
    }
  } else {
    while (!ringFull() && guard++ < 96) {
      int16_t buf[32]; int n = psorom::renderMix(buf, 32);         // DAC + AY + voix (émulateur live, exact)
      for (int i = 0; i < n && !ringFull(); i++) ringPush(buf[i]);
    }
  }
  delayMicroseconds(300);                                          // ring plein -> petite pause (l'ISR vide)
  static int s_volSaved = -1, s_volPrev = -1; static uint32_t s_volMs = 0;   // persiste le volume (debounce 3s, anti-usure NVS)
  if (g_vol != s_volPrev) { s_volPrev = g_vol; s_volMs = millis(); }
  if (g_vol != s_volSaved && millis() - s_volMs > 3000) { g_prefs.putInt("vol", g_vol); s_volSaved = g_vol; }
  if (Serial.available()) { int c = Serial.parseInt(); if (c >= 0 && c <= 255) { g_pendingCmd = c; Serial.printf("-> cmd %d\n", c); } }
}
#endif // GOSOWAV_BENCH
