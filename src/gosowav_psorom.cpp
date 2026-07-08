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
#include <ESPmDNS.h>             // gosowav.local : pas besoin de connaitre l'IP DHCP
#include <WebServer.h>
#include <string.h>
#include <stdio.h>
#include "driver/sdmmc_host.h"   // montage SD via l'IDF (comme Ralf), avec SDMMC_SLOT_FLAG_INTERNAL_PULLUP
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"           // esp_timer_get_time() : horloge us pour le cadencement temps-reel
#include "soc/gpio_struct.h"     // GPIO.out_w1ts/w1tc : bit-bang du MCP4921 en ISR (comme Ralf)
#include "psorom.h"
#include "chefv2.h"              // CHEF v2 : conducteur evenementiel (actif si <jeu>/sounds.sig present)
#include "wavmix.h"              // PSOWAV : moteur de mixage WAV (clean-room, hybride par defaut)
#include "wavfile.h"
#include "wavsrc.h"
#include "wavset.h"
#include <dirent.h>
#include <Preferences.h>        // NVS : jeu installe + volume persistes (montage physique en machine)
#include <Update.h>             // OTA : flash du firmware par WiFi (/ota, multipart .bin)
#include <SPI.h>                // DAC MCP4921 en SPI MATERIEL (broches VSPI natives du schema)
#include "soc/spi_struct.h"
#include "soc/gpio_sig_map.h"

// MCP4921 sur la WROVER GOSOWAV (fait matériel de la carte) : SCK=18, SDI/MOSI=23, CS=5.
static const int DAC_SCK = 18, DAC_SDI = 23, DAC_CS = 5;

static WebServer server(80);

#define MAXGAMES 32
struct Game { char id[16]; uint8_t gen; char title[40]; };
static Game  g_games[MAXGAMES];
static int   g_nGames = 0;
static int   g_sel = 0;                  // jeu courant (index dans g_games)

static float        g_thrM = 0;          // débit mesuré (M 6502-cycles/s, ISR DAC active)
static float        g_thrOff = 0;        // (libre)
static float        g_thrNoIsr = 0;      // débit run() avec l'ISR DAC COUPEE -> mesure ce que le streaming audio vole
static uint32_t     g_mixSps = 0;        // débit renderMix (echantillons/s ; besoin = Fs pour temps-reel)
static bool         g_benched = false;   // bench déjà fait ?
static volatile int g_lastCmd = -1;      // dernière commande son
// FILE de commandes (8) : la vraie machine envoie des RAFALES (cmd->cmd sans repos) ; une seule case
// se faisait ecraser -> sons manquants en pleine partie. Producteurs: bus (cœur 1), web (cœur 0), série.
static volatile uint8_t s_cq[8]; static volatile uint8_t s_cqH = 0, s_cqT = 0;
static portMUX_TYPE s_cqMux = portMUX_INITIALIZER_UNLOCKED;
static void cmdPush(int c) { portENTER_CRITICAL(&s_cqMux); uint8_t n = (s_cqH + 1) & 7;
                             if (n != s_cqT) { s_cq[s_cqH] = (uint8_t)c; s_cqH = n; } portEXIT_CRITICAL(&s_cqMux); g_lastCmd = c; }
static int  cmdPop()       { int c = -1; portENTER_CRITICAL(&s_cqMux);
                             if (s_cqT != s_cqH) { c = s_cq[s_cqT]; s_cqT = (s_cqT + 1) & 7; } portEXIT_CRITICAL(&s_cqMux); return c; }
static volatile int g_pendingLoad = -1;  // web/init   -> loop() : index de jeu à (re)charger
static volatile bool g_ready = false;    // un jeu est chargé + émulateur tourne ?
static char         g_status[96] = "booting...";
static volatile int g_vol = 100;         // volume maitre % (0..200, 100 = normal) applique au DAC
static volatile bool g_wifiHold = false; // WiFi maintenu allume (debug, persiste) ; sinon OFF par defaut (bruit RF dans le DAC)
static bool          g_webUp = false;    // la pile web/radio est-elle demarree ? (bouton BOOT ou TEST machine l'allume)
static volatile uint32_t g_lastWeb = 0;  // millis() de la derniere requete web (page ouverte = WiFi maintenu vivant)
#define WIFI_WINDOW_MS 60000             // coupure WiFi apres 60s SANS activite web -> audio propre (page fermee)
static Preferences  g_prefs;             // NVS : jeu installe ("game") + volume ("vol")

// --- Sortie audio cadencee par TIMER MATERIEL (comme Ralf) : un ring SPSC rempli par loop(), vide
// --- par une ISR a Fs exact qui bit-bang le MCP4921. Fini les rafales SPI -> flux DAC regulier = son
// --- propre. (producteur = loop core1, consommateur = ISR core1 -> meme coeur, pas de race.)
#define ARING 4096                                            // 93 ms de coussin : absorbe tout micro-blocage (prints, ticks, NVS)
static volatile int16_t  s_ring[ARING];
static volatile uint32_t s_rHead = 0, s_rTail = 0;   // head = producteur (loop), tail = conso (ISR)
static volatile uint32_t s_underruns = 0;            // ring vide a un tick ISR = coupure audio (diag)
static volatile uint32_t s_minFill = 0xFFFF;          // niveau MINIMAL du ring sur la fenetre de stats (diagnostic decrochages)
static volatile uint32_t s_maxGap = 0;                // pire ecart entre deux passages de la pompe loop() (ms)
static volatile uint32_t s_prodN = 0;                 // echantillons produits par liveRender (fenetre de stats)
static volatile uint32_t s_thrN = 0;                  // nb de sommeils du throttle (fenetre)
static hw_timer_t*       s_dacTimer = nullptr;

// MCP4921 sur le SPI MATERIEL (VSPI : les broches du schema SONT les natives -> CS0 hardware, zero bit-bang).
// L'ISR ne fait que deposer le mot : le peripherique pousse les 16 bits seul (~2 us a 8 MHz, fini bien
// avant le tick suivant a 22,7 us). Ancien bit-bang logiciel : ~40 % d'un coeur ; ici ~7 %.
static void dacSpiInit() {
  SPIClass* vspi = new SPIClass(VSPI);
  vspi->begin(DAC_SCK, -1, DAC_SDI, -1);                           // configure SCK/MOSI via la matrice GPIO
  vspi->beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  vspi->endTransaction();                                          // laisse les registres regles (horloge/mode)
  pinMatrixOutAttach(DAC_CS, VSPICS0_OUT_IDX, false, false);       // CS5 -> signal CS0 hardware du VSPI
  SPI3.pin.cs0_dis = 0; SPI3.pin.cs1_dis = 1; SPI3.pin.cs2_dis = 1;
  SPI3.user.usr_mosi = 1; SPI3.user.usr_miso = 0;
  SPI3.user.usr_command = 0; SPI3.user.usr_addr = 0; SPI3.user.usr_dummy = 0;
  SPI3.mosi_dlen.usr_mosi_dbitlen = 15;                            // 16 bits
}
static inline void IRAM_ATTR dacBitbang(uint16_t word) {           // (nom conserve) -> ecriture SPI materielle
  if (SPI3.cmd.usr) return;                                        // transfert precedent en cours (impossible a 44.1k) -> saute
  SPI3.data_buf[0] = (uint32_t)((uint16_t)((word >> 8) | (word << 8)));  // octet fort d'abord (buffer little-endian)
  SPI3.cmd.usr = 1;                                                // GO — pas d'attente : le hard finit seul, CS0 auto
}

static volatile uint8_t s_idle = 0;                                // niveau repos (polarite auto, AUTO-APPRENANT : 5 s stables = repos)
static volatile uint8_t g_busRing[4]; static volatile uint8_t g_busH = 0, g_busT = 0;   // commandes detectees par l'ISR -> loop()
static volatile uint8_t g_busRaw = 0;                              // dernier etat brut des lignes (diagnostic telemetrie)
static volatile uint8_t g_busHold = 0;                             // valeur non-repos TENUE >1 s (diagnostic bip mur)
static volatile uint32_t g_pinTog[7] = {0};                        // MULTIMETRE : transitions par broche {27,26,25,33,32,35,34}
static volatile uint32_t g_cmdN = 0, g_cmdOdd = 0, g_cmd5 = 0;     // stats commandes depuis le boot (impaires = test S1)
static volatile uint8_t g_fsEdge[8]; static volatile uint8_t g_fsH = 0, g_fsT = 0;   // transitions F(IO35)/Strobe(IO34) -> loop
static volatile uint16_t g_fN = 0, g_strN = 0;                     // compteurs de fronts (telemetrie ; une ligne qui flotte se voit)
static volatile uint32_t g_rstEdgeMs = 0;                          // dernier front sur IO34 (RESET machine via Strobe_in)
static volatile uint16_t g_rstEdges = 0;
static volatile bool     g_rstReq = false, g_rstJingle = false;    // demande de reset (chefTask) / jingle a jouer (loop)
static volatile uint32_t g_chefPause = 0;                          // pause du conducteur jusqu'a millis() (maintenance WiFi)
// --- BOITE NOIRE : tout evenement notable est journalise sur SD (releve par WiFi /trace, pas d'USB requis) ---
static char     g_bb[2048]; static volatile uint16_t g_bbN = 0;    // tampon RAM (ecritures SD groupees, 4 s)
static void bbLog(const char* fmt, ...) {
  char ln[120]; va_list ap; va_start(ap, fmt);
  int n = snprintf(ln, sizeof(ln), "[%lu] ", millis());
  n += vsnprintf(ln + n, sizeof(ln) - n - 2, fmt, ap); va_end(ap);
  ln[n++] = '\n';
  if (g_bbN + n < sizeof(g_bb)) { memcpy(g_bb + g_bbN, ln, n); g_bbN = (uint16_t)(g_bbN + n); }
}
static void bbFlush(bool force) {
  static uint32_t lastMs = 0;
  if (!g_bbN || (!force && millis() - lastMs < 4000)) return;
  lastMs = millis();
  FILE* f = fopen("/sdcard/trace.log", "ab");
  if (f) { if (ftell(f) > 262144) { fclose(f); f = fopen("/sdcard/trace.log", "wb"); }   // rotation 256 Ko
           if (f) { fwrite(g_bb, 1, g_bbN, f); fclose(f); } }
  g_bbN = 0;
}
static void IRAM_ATTR onResetEdge() { g_rstEdges++; g_rstEdgeMs = millis(); }   // pulse trop court pour l'echantillonnage -> interruption
                                                                   // (ring 4 : une rafale bus ne perd plus la 2e commande)
static void IRAM_ATTR onDacTimer() {                              // tire 1 echantillon du ring a chaque tick Fs
  { static uint8_t div = 0, prev = 0xFF; static uint16_t stab = 0;
    if (++div >= 4) { div = 0;                                     // sondage 11 kHz : les RAFALES du bip mur (gaps <1 ms) etaient INVISIBLES a 2,76 kHz
      uint32_t in0 = GPIO.in, in1 = GPIO.in1.val;                  // S1=27 S2=26 S4=25 (banc 0) ; S8=33 S16=32 (banc 1)
      uint8_t raw = (uint8_t)(((in0 >> 27) & 1) | (((in0 >> 26) & 1) << 1) | (((in0 >> 25) & 1) << 2)
                            | (((in1 >> 1) & 1) << 3) | (((in1 >> 0) & 1) << 4));   // S1=GPIO27 (mapping standard)
      { static uint8_t pp = 0; static bool pi = false;             // MULTIMETRE : compte l'activite de CHAQUE broche candidate
        uint8_t cur = (uint8_t)(((in0>>27)&1) | (((in0>>26)&1)<<1) | (((in0>>25)&1)<<2)   // 27,26,25,33,32,35,34
                     | (((in1>>1)&1)<<3) | (((in1>>0)&1)<<4) | (((in1>>3)&1)<<5) | (((in1>>2)&1)<<6));
        if (pi) { uint8_t chg = cur ^ pp; for (int b = 0; b < 7; b++) if (chg & (1u<<b)) g_pinTog[b]++; }
        pp = cur; pi = true; }
      if (raw == prev) { if (stab < 1000) stab++; } else { prev = raw; stab = 0; }
      g_busRaw = raw;
      { static uint8_t fsPrev = 0xFF, fsStab = 0, fsLast = 0xFF;   // F(IO35)=in1 bit3, Strobe(IO34)=in1 bit2 : observe les
        uint8_t fs = (uint8_t)(((in1 >> 3) & 1) | (((in1 >> 2) & 1) << 1));   // lignes que la capture n'utilise pas encore
        if (fs == fsPrev) { if (fsStab < 250) fsStab++; } else { fsPrev = fs; fsStab = 0; }
        if (fsStab >= 1 && fs != fsLast) {
          if (fsLast != 0xFF) { uint8_t chg = (uint8_t)(fs ^ fsLast);
            if (chg & 1) { g_fN++;   uint8_t n = (uint8_t)((g_fsH + 1) & 7); if (n != g_fsT) { g_fsEdge[g_fsH] = (uint8_t)(0 | (fs & 1));        g_fsH = n; } }
            if (chg & 2) { g_strN++; uint8_t n = (uint8_t)((g_fsH + 1) & 7); if (n != g_fsT) { g_fsEdge[g_fsH] = (uint8_t)(2 | ((fs >> 1) & 1)); g_fsH = n; } } }
          fsLast = fs;
        } }
      uint8_t c = (uint8_t)((raw ^ s_idle) & 0x1F);
      // ACCEPTE TOUTES LES COMMANDES, comme la carte d'origine. Toute valeur STABLE != repos est une
      // commande ; la MEME valeur re-presentee apres un retour au repos re-declenche (bibibip du mur).
      // Plus de filtre fantome / suppression de repetition / apprentissage de repos : c'etaient des
      // rustines de l'epoque ou S1 etait casse. Seul l'anti-rebond reste (rejette les transitoires).
      { static uint8_t settled = 0xFF, accepted = 0;
        if (stab >= 8) {                                           // ~0,7 ms stable = valeur reellement presentee
          if (c != settled) { settled = c;
            if (c != 0 && c != accepted) {                         // nouvelle commande (ou meme valeur apres retour repos)
              uint8_t n = (uint8_t)((g_busH + 1) & 3);
              if (n != g_busT) { g_busRing[g_busH] = c; g_busH = n; }
              accepted = c;
            }
            if (c == 0) accepted = 0;                              // retour au repos -> la meme commande pourra re-declencher
          }
        } }
    } }
  uint32_t t = s_rTail;
  { uint32_t f = (s_rHead - t) & (ARING - 1); if (f < s_minFill) s_minFill = f; }
  static int16_t held = 0;
  if (t != s_rHead) { held = s_ring[t]; s_rTail = (t + 1) & (ARING - 1); }  // sinon : maintient (sample&hold)
  else { s_underruns++; held -= held >> 6; }                      // B6: ring vide -> held decroit vers 0 (centre) au lieu de figer un pic DC (moins de pop)
  int32_t x = ((int32_t)held * g_vol) / 100;                      // volume maitre (jusqu'a 300 %)
  // Limiteur doux (soft-knee saturant, continu C1). Ces WAV ont un RMS faible : pousser le volume monte
  // le niveau MOYEN (= volume percu) ; le limiteur sature gentiment les pics au lieu d'ecreter dur ->
  // bien plus fort, propre, SANS recablage (marche sur la sortie A5 actuelle). Asymptote vers ±32767.
  { int32_t a = x < 0 ? -x : x; const int32_t KNEE = 22000, RANGE = 32767 - 22000;
    if (a > KNEE) { int32_t o = a - KNEE; o = (o * RANGE) / (o + RANGE); a = KNEE + o; x = (x < 0) ? -a : a; } }
  // 16->12 bit : dither TPDF (~±1 LSB12) + noise-shaping 1er ordre (error feedback). Sur un DAC 12-bit
  // la troncature brute des 4 bits faibles cree une distorsion de quantification correlee (audible sur
  // les sons faibles / fins de notes) ; le dither la decorrele, le shaping repousse le bruit vers l'aigu.
  static int32_t s_nsErr = 0; static uint32_t s_rng = 0x2545F491;
  s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; // xorshift32 (rapide en ISR)
  int32_t tri = (int32_t)(s_rng & 0xF) - (int32_t)((s_rng >> 8) & 0xF);  // bruit triangulaire (TPDF) ±15/65535 ≈ ±1 LSB12
  int32_t qin = (x + 32768) + tri + s_nsErr;
  if (qin < 0) qin = 0; else if (qin > 65535) qin = 65535;
  int32_t code = (qin + 8) >> 4;                                  // arrondi (pas troncature) au pas 12-bit
  if (code > 4095) code = 4095;
  s_nsErr = qin - (code << 4);                                    // residu reinjecte au prochain echantillon (shaping)
  dacBitbang(0x3000 | (uint16_t)(code & 0x0FFF));
}

static inline bool ringFull()  { return ((s_rHead + 1) & (ARING - 1)) == s_rTail; }
static inline void ringPush(int16_t s) { uint32_t h = s_rHead; s_ring[h] = s; s_rHead = (h + 1) & (ARING - 1); }

// --- Bus commande son d'une vraie machine Gottlieb System 80 (via ULN2803 IC2). Méthode de Ralf
// (if_G80.c, prouvée sur cette carte) : PAS de strobe — on déclenche sur le CHANGEMENT des 5 bits de
// données (S1_A=27, S2_B=26, S4_C=25, S8_D=33, S16_E=32 = bits 0..4), settle, puis on lit. En plus :
// POLARITÉ AUTO — on échantillonne le niveau de repos au boot (s_idle) et on compare par XOR, donc ça
// marche que l'ULN inverse ou non. 0 = repos ; bits != repos = commande. (Strobe/F non utilisés.)
static int g_curGenFwd();                                          // (g_curGen est defini plus bas)
#define g_curGen g_curGenFwd()
static const int PIN_S[6] = {27, 26, 25, 33, 32, 35};             // S1_A,S2_B,S4_C,S8_D,S16_E + S32_F (GPIO35) : les 80B parlent sur 6 BITS
static const int PIN_STROBE = 34;                                  // (commandes 0-63 ; en 5 bits on RATAIT/ALIASAIT la moitie des sons !)
static uint8_t s_lastBus = 0;
static inline uint8_t readBus5() { uint8_t v = 0; for (int i = 0; i < 6; i++) if (digitalRead(PIN_S[i])) v |= (1u << i); return v; }
static void cmdInputBegin() {
  for (int i = 0; i < 6; i++) pinMode(PIN_S[i], INPUT);
  pinMode(PIN_STROBE, INPUT);
  delay(50); s_idle = readBus5(); s_lastBus = 0;                   // repos -> polarité-agnostique (XOR)
  Serial.printf("cmd-input: repos bus = 0x%02X (polarite auto, pas de strobe)\n", s_idle);
}
static inline void cmdInputPoll() {                                // declenche sur TOUT changement vers une valeur non-repos
  uint8_t c = (uint8_t)((readBus5() ^ s_idle) & 0x1F);             // 5 bits pour TOUTES les gens (DECOMPILE : les ROM font AND #$1F ;
                                                                   // les >31 passent par PAIRES header+valeur, geres par @hdr ci-dessous)
  if (c != 0 && c != s_lastBus) {                                  // repos->cmd ET cmd->cmd2 DIRECT (avant : seule la 1re d'une rafale etait vue)
    delayMicroseconds(30);                                         // settle (de-skew des 5 bits)
    uint8_t c2 = (uint8_t)(readBus5() ^ s_idle);
    if (c2 != 0 && c2 != s_lastBus) { cmdPush(c2); s_lastBus = c2; }
  } else if (c == 0) s_lastBus = 0;                                // re-arme au retour au repos
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
static volatile bool g_liveMode = false;               // PSOLIVE : emulation complete en direct (Gen1) — zero WAV, zero heuristique
static bool          g_wavForce = false;               // loops.txt "@wav" = forcer le mode WAV meme en Gen1
static bool          g_liveForce = false;              // loops.txt "@live" = emulation directe (experimental)
static volatile bool g_liveSticky = false;             // interrupteur live a chaud (serie 'L' / web /live), survit au reload
// ROM-chef : le CPU (logique seule) pilote le lecteur WAV (start/loop/stop) selon son comportement reel.
static volatile int  g_chefReq = -1;                   // commande a (re)jouer : web/serie/materiel -> chef
#undef g_curGen
static int           g_curGen = 3;                     // generation du jeu (cadence CPU : Gen1=1MHz, Gen2/3=2MHz/CPU)
static int g_curGenFwd() { return g_curGen; }
static int64_t       g_chefT0 = 0, g_chefEmu = 0;      // cadencement temps-reel du CPU
// --- ROM-chef en 2 cœurs : tache DECODEUR (cœur 0) lit le CPU et leve des drapeaux ; le PLAYER (loop, cœur 1) les applique ---
static volatile int  g_chefCmd  = -1;                  // commande a DECODER (player -> decodeur)
static volatile bool g_stopFx   = false;               // decodeur -> player : effets (DAC) finis  -> stop boucles d'effet
static volatile bool g_stopMus  = false;               // decodeur -> player : musique (AY/YM) finie -> stop boucle de fond
static volatile bool g_chefArmed = false;              // un son est lance -> on surveille sa fin
static volatile bool g_musWatch  = false;              // une MUSIQUE de fond a ete lancee PAR COMMANDE -> le chef surveille sa fin dans la ROM
static uint32_t      g_pDac = 0;                       // derniere valeur d'activite DAC
static int64_t       g_dacFreeze = 0, g_tonFreeze = 0; // g_chefEmu au moment ou l'activite a FIGE (= debut du silence)
static bool          g_dacSeen = false, g_tonSeen = false;  // ce sous-systeme a-t-il VRAIMENT joue depuis la commande ? (sinon on ne stoppe rien)
static bool          g_firedFx = false, g_firedTon = false; // stop deja leve pour cet episode de silence ? (anti-repetition)
static volatile float g_chefMcps = 0;                  // debit REEL du decodeur sur cœur 0 (M cyc/s) -> mesure le temps-reel (besoin Gen1 2.0)
static volatile bool g_chefInEmu = false;              // handshake rechargement : le decodeur est DANS psorom -> loadGame attend qu'il soit sorti
static volatile bool g_stopReq   = false;              // bouton STOP (web, cœur 0) -> loop() (cœur 1) coupe les WAV (pas d'appel mixer inter-cœurs)
// --- stop PAR EPISODE : chaque commande a SES canaux tonals ; quand ILS meurent, on coupe CE son ---
// (la regle "silence global" ne trouve jamais sa fenetre en pleine partie -> boucles immortelles)
struct ChefEp { int16_t id; uint32_t chan; int64_t tonFreeze; bool seen, fired, lp, dacEp; };
static ChefEp        g_eps[8]; static int g_epCur = -1;        // table cote chef (coeur 0 seul)
static volatile int  g_stopTagReq = -1;                        // chef -> player : coupe les voix de CE son precis
static volatile bool g_cmdLp = false;                          // la derniere commande a lance une BOUCLE ? (fixe AVANT g_chefCmd)
static volatile uint64_t g_aliveMask = 0;                      // bit n = un episode VIVANT (pas fini) existe pour le son n (chef -> GC player)
static uint32_t g_tagMs[64];                                   // derniere commande par son (le GC ne tue pas un son juste commande)
static volatile uint16_t g_hist[16]; static volatile uint32_t g_histMs[16];   // 16 dernieres commandes (diagnostic jeu reel)
static volatile int g_silLoopMs = 6000;                        // seuil silence-canaux des BOUCLES (ms emulees), auto-calibre par jeu via loops.txt @sil
static uint8_t  g_hdrVals[4]; static int g_hdrN = 0;           // valeurs bus qui sont des HEADERS de banque (loops.txt @hdr=30,29)
static int      g_hdrBank = 0; static uint32_t g_hdrMs = 0;    // banque armee par le dernier header + son age
// --- CHEF v2 (evenementiel) : signatures mesurees par jeu, regles de propriete de canal (chefv2.cpp) ---
static bool          g_sigMode = false;                        // <jeu>/sounds.sig present -> v2 remplace les heuristiques v1
static chefv2::Sig*  g_sigs = nullptr; static int g_nSigs = 0; // 96 entrees en PSRAM (lecture rare : commande + tick)
static volatile int  g_chefCmdExt = -1;                        // id ETENDU (banques resolues) pour chefv2 ; fixe AVANT g_chefCmd
static chefv2::Action g_v2a[32];                               // actions chef (coeur 0 -> coeur 1), ring SPSC
static volatile uint8_t g_v2aH = 0, g_v2aT = 0;
struct WSlot { FILE* f; wavsrc::Source src; uint32_t dataOff, dataLen, loopOff, loopBytes; uint8_t ch; int vid; bool used;
               uint32_t born; bool isBg, isVoice; };   // pour le VOL de voix (mixer plein) : anciennete + categorie
static WSlot s_ws[wavmix::MAX_VOICES];
static uint16_t g_loopMs[256];                          // debut de boucle (ms) ; 0 = au debut du fichier
static uint16_t g_loopLen[256];                         // LONGUEUR de boucle (ms) = nb ENTIER de mesures musicales ; 0 = jusqu'a la fin du fichier
static bool     g_oneShot[256];                         // la ROM dit que ce son est un ONE-SHOT -> override l'attribut 'l' (ne boucle PAS)

static size_t wfRead(void* ctx, uint8_t* d, size_t n) { return fread(d, 1, n, (FILE*)ctx); }
static size_t wsFill(void* p, int16_t* dst, size_t fr) { return wavsrc::fill(&((WSlot*)p)->src, dst, fr); }
static volatile uint32_t g_rwN = 0, g_rwLastMs = 0, g_rwIntMs = 0;  // diagnostic boucle : nb rewinds + intervalle REEL entre 2 rewinds (= periode jouee)
static bool   wsRewind(void* p) { WSlot* s = (WSlot*)p;  // reboucle [loopOff, loopOff+loopBytes] = nb ENTIER de mesures (raccord sur le temps)
                                  if (!s->f || fseek(s->f, s->dataOff + s->loopOff, SEEK_SET)) return false;
                                  uint32_t len = s->loopBytes ? s->loopBytes : (s->dataLen - s->loopOff);
                                  uint32_t now = millis(); if (g_rwLastMs) g_rwIntMs = now - g_rwLastMs; g_rwLastMs = now; g_rwN++;
                                  wavsrc::init(s->src, wfRead, s->f, s->ch, len); return true; }

// Demarre le WAV du son `id` en ONE-SHOT (le ROM-chef decidera de reboucler). Retourne le slot, ou -1.
static int wavTrigger(int id, bool loop) {
  const wavset::Entry* e = g_set.find(id); if (!e || (e->attr & wavset::A_PLACE)) return -1;
  for (int i = 0; i < wavmix::MAX_VOICES; i++)            // libere les voix terminees
    if (s_ws[i].used && !g_mixer.active(s_ws[i].vid)) { fclose(s_ws[i].f); s_ws[i].used = false; }
  int si = -1; for (int i = 0; i < wavmix::MAX_VOICES; i++) if (!s_ws[i].used) { si = i; break; }
  if (si < 0) {                                            // mixer PLEIN -> VOLE la voix la plus ANCIENNE (effets d'abord, jamais la voix parlee,
    uint32_t old = 0xFFFFFFFF; int bi = -1;                // le fond en dernier recours) : la nouvelle commande compte plus qu'un vieil effet
    for (int pass = 0; pass < 2 && bi < 0; pass++)
      for (int i = 0; i < wavmix::MAX_VOICES; i++)
        if (s_ws[i].used && !s_ws[i].isVoice && (pass == 1 || !s_ws[i].isBg) && s_ws[i].born < old) { old = s_ws[i].born; bi = i; }
    if (bi < 0) { Serial.printf("[%lu] trig %d: AUCUN SLOT (tout est voix parlee)\n", millis(), id); return -1; }
    g_mixer.stop(s_ws[bi].vid); fclose(s_ws[bi].f); s_ws[bi].used = false; si = bi;
    Serial.printf("[%lu] trig %d: vole le slot %d (plus ancien)\n", millis(), id, bi);
  }
  char path[96]; snprintf(path, sizeof(path), "%s/%s/%s", MP, g_theme, e->file);
  FILE* f = fopen(path, "rb"); if (!f) { Serial.printf("[%lu] trig %d: fopen KO %s\n", millis(), id, path); return -1; }
  uint8_t hdr[64]; size_t got = fread(hdr, 1, sizeof(hdr), f);
  WavInfo wi = wav_parse(hdr, got);
  if (!wi.ok || fseek(f, wi.dataOffset, SEEK_SET)) { fclose(f); return -1; }
  s_ws[si].f = f; s_ws[si].dataOff = wi.dataOffset; s_ws[si].dataLen = wi.dataLen; s_ws[si].ch = (uint8_t)wi.channels;
  uint32_t bpms = wi.rate ? (uint32_t)wi.rate * wi.channels * 2 : 0;   // octets par seconde
  uint32_t lo = 0, lb = 0;                               // debut + LONGUEUR de boucle (octets), alignes frame
  uint32_t lms = g_loopMs[id & 0xFF], llms = g_loopLen[id & 0xFF];
  if (lms && bpms)  { lo = (uint32_t)((uint64_t)lms  * bpms / 1000); lo -= lo % (wi.channels * 2); if (lo >= wi.dataLen) lo = 0; }
  if (llms && bpms) { lb = (uint32_t)((uint64_t)llms * bpms / 1000); lb -= lb % (wi.channels * 2); if (lo + lb > wi.dataLen) lb = wi.dataLen - lo; }
  s_ws[si].loopOff = lo; s_ws[si].loopBytes = lb;
  uint32_t firstLen = lb ? (lo + lb) : wi.dataLen;       // 1er passage : [0, fin de boucle] (intro+1 mesure) sinon fichier complet
  wavsrc::init(s_ws[si].src, wfRead, f, (uint8_t)wi.channels, firstLen);
  wavmix::VoiceCfg vc; vc.fill = wsFill; vc.ctx = &s_ws[si]; vc.rewind = wsRewind;
  vc.gain = 150; vc.tag = id; vc.loop = loop;             // -4,6 dB par voix : deux sons CHAUDS sommes restent sous le genou (200 les faisait s'ecraser mutuellement)
  vc.bg    = (e->attr & wavset::A_INIT)  != 0;            // fond (i) : survit aux stop de 1er plan
  vc.voice = (e->attr & wavset::A_VOICE) != 0;            // voix (v) : bus parole, survit au soft-kill
  int vid = g_mixer.trigger(vc); if (vid < 0) { fclose(f); Serial.printf("[%lu] trig %d: MIXER PLEIN\n", millis(), id); return -1; }
  s_ws[si].vid = vid; s_ws[si].used = true;
  s_ws[si].born = millis(); s_ws[si].isBg = vc.bg; s_ws[si].isVoice = vc.voice;
  g_rwLastMs = 0; g_rwIntMs = 0;                          // nouveau son -> l'intervalle de rewind repart de zero (pas de mesure inter-sons)
  Serial.printf("[%lu] trig %d: OK slot=%d vid=%d loop=%d off=%lu len=%lu\n", millis(), id, si, vid, (int)loop, (unsigned long)lo, (unsigned long)lb);
  return si;
}

// PROTOCOLE A BANQUES (decompile hotshots F14A) : un header (ex. bus 30/29) ne JOUE rien, il arme la
// banque ; la valeur suivante devient un son etendu (32-95). Retourne -1 pour un header consomme.
static int bankDecode(int id) {
  for (int i = 0; i < g_hdrN; i++)
    if (id == g_hdrVals[i]) {
      g_hdrBank = (~id) & 0x1F;                                      // valeur ROM du header = numero de banque (1 ou 2)
      g_hdrMs = millis();
      Serial.printf("[%lu] chef: header banque %d (bus %d)\n", millis(), g_hdrBank, id);
      return -1;
    }
  if (g_hdrBank && millis() - g_hdrMs < 3000) { id = g_hdrBank * 32 + (id & 0x1F); }
  g_hdrBank = 0;
  return id;
}

// ROM-chef WAV : applique les attributs PSOWAV de la commande (loop/break/kill/voice) — le set decrit
// le COMPORTEMENT (boucle vs one-shot, coupe), la CPU emulee cadence start/stop via son activite.
static void wavCommand(int id) {
  id = bankDecode(id);
  if (id < 0) return;
  const wavset::Entry* e = g_set.find(id);
  if (!e) { Serial.printf("[%lu] chef: cmd %d SANS WAV (decode ROM seul -> stop si elle se tait, ex. 31)\n", millis(), id); return; }
  uint8_t at = e->attr;
  if (at & wavset::A_KILL)  g_mixer.stopAll();                       // k : coupe tout
  if (at & wavset::A_SKILL) g_mixer.stopExcept(true,  false, false); // c : soft-kill (garde le fond)
  if (at & wavset::A_QUIT)  g_mixer.stopExcept(true,  true,  true);  // q : soft-kill (garde fond+boucles+voix)
  if (at & wavset::A_BREAK) g_mixer.stopTag(id);                     // b : coupe les instances du meme son
  if (at & wavset::A_PLACE) return;                                  // x : placeholder, pas d'audio
  bool lp = (at & wavset::A_LOOP) != 0;
  if (g_oneShot[id & 0xFF]) lp = false;                              // V1 : la ROM dit one-shot -> override l'attribut 'l' (ne boucle PAS)
  if (!lp) g_mixer.stopTag(id);                                      // one-shot RE-déclenché = REDEMARRE (la ROM est mono : un spinner re-clique,
  g_cmdLp = lp;                                                      // il ne s'empile pas) ; informe le chef du type (boucle/one-shot)
  Serial.printf("[%lu] chef: cmd %d attr=%02x lp=%d voix_actives=%d\n", millis(), id, at, lp, g_mixer.activeCount());
  if (lp) {                                                          // musique MONO (regle PWAVplayer) : une NOUVELLE boucle commandee
    g_mixer.stopActiveLoops();                                       // REMPLACE la precedente (les sets type Arena n'ont pas d'attr 'i' :
    if (at & wavset::A_INIT) g_mixer.stopBgLoops();                  // sans ca les musiques s'EMPILENT) ; un nouveau fond remplace aussi le fond
  }                                                                  // (one-shots et voix, eux, se superposent toujours)
  wavTrigger(id, lp);
  if (lp) g_musWatch = true;                                         // boucle commandee par la ROM -> le chef surveillera SA fin (stopMus)
}

static void loadWavSet(const char* theme) {              // scanne /sdcard/<theme>/ -> g_set ; g_psowav si non vide
  g_mixer.stopAll(); g_mixer.setMix(wavmix::MIX_SUM);   // somme + GENOU DOUX dans mix() : plus de pompage 1/sqrt(N), plus d'ecretage dur
  for (int i = 0; i < wavmix::MAX_VOICES; i++) if (s_ws[i].used) { fclose(s_ws[i].f); s_ws[i].used = false; }
  g_set.reset(); strncpy(g_theme, theme, sizeof(g_theme) - 1); g_theme[sizeof(g_theme) - 1] = 0;
  g_psowav = false;
  char dp[64]; snprintf(dp, sizeof(dp), "%s/%s", MP, theme);
  DIR* dir = opendir(dp);
  if (dir) { struct dirent* de; while ((de = readdir(dir))) g_set.addName(de->d_name); closedir(dir); g_psowav = g_set.nEntry > 0; }
  memset(g_loopMs, 0, sizeof(g_loopMs)); memset(g_loopLen, 0, sizeof(g_loopLen)); memset(g_oneShot, 0, sizeof(g_oneShot));
  char lpath[80]; snprintf(lpath, sizeof(lpath), "%s/%s/loops.txt", MP, theme);    // "id=oneshot" | "id=debutMs" | "id=debutMs,longueurMs"
  FILE* lf = fopen(lpath, "r");                                                     // longueur = nb ENTIER de mesures (raccord sur le temps) ; oneshot = override 'l'
  g_silLoopMs = 6000; g_wavForce = false; g_liveForce = false; g_hdrN = 0;                               // defaut : 6 s (marge sur pauses internes typiques)
  if (lf) { char ln[48], val[24]; int nlp = 0, id, st, len;
            while (fgets(ln, sizeof(ln), lf)) {
              if (sscanf(ln, "@sil=%d", &st) == 1 && st >= 1000 && st <= 20000) { g_silLoopMs = st; nlp++; continue; }  // seuil par JEU (genere par make_psowav_set)
              if (!strncmp(ln, "@wav", 4)) { g_wavForce = true; nlp++; continue; }                                       // forcer le mode WAV (debug/comparaison)
              if (!strncmp(ln, "@live", 5)) { nlp++; continue; }                                                          // LIVE RETIRE : @live ignore
              if (!strncmp(ln, "@hdr=", 5)) { g_hdrN = 0; const char* q = ln + 5;                                          // headers de banque (Gen2/3)
                while (*q && g_hdrN < 4) { int v = atoi(q); if (v > 0 && v < 32) g_hdrVals[g_hdrN++] = (uint8_t)v;
                                           while (*q && *q != ',') q++; if (*q == ',') q++; }
                nlp++; continue; }
              if (sscanf(ln, "%d=%23s", &id, val) == 2 && id >= 0 && id < 256) {   // lit l'id + la valeur (chaine), PUIS decide
                if (!strncmp(val, "oneshot", 7)) { g_oneShot[id] = true; }
                else { st = 0; len = 0; sscanf(val, "%d,%d", &st, &len); g_loopMs[id] = (uint16_t)st; g_loopLen[id] = (uint16_t)len; }
                nlp++;
              } }
            fclose(lf); Serial.printf("loops.txt : %d entree(s)\n", nlp); }
  g_nSigs = 0; g_sigMode = false;                                    // CHEF v2 : signatures mesurees (host_sig2 -> sounds.sig, JSONL)
  { char spath[80]; snprintf(spath, sizeof(spath), "%s/%s/sounds.sig", MP, theme);
    FILE* sf = fopen(spath, "r");
    if (sf && !g_sigs) { g_sigs = (chefv2::Sig*)ps_malloc(96 * sizeof(chefv2::Sig));
                         if (!g_sigs) g_sigs = (chefv2::Sig*)malloc(96 * sizeof(chefv2::Sig)); }
    if (sf && !g_sigs) { fclose(sf); sf = nullptr; }
    if (sf) { char ln[256];
      auto ji = [](const char* s, const char* k) -> int { char pat[24]; snprintf(pat, sizeof(pat), "\"%s\":", k);
                                                          const char* p = strstr(s, pat); return p ? atoi(p + strlen(pat)) : 0; };
      auto dec = [](const char* q) -> int { int h = atoi(q); const char* d = strchr(q, '.');
                                            return d ? (((~h) & 0x1F) * 32 + (atoi(d + 1) & 0x1F)) : h; };
      chefv2::clearKills();
      while (fgets(ln, sizeof(ln), sf) && g_nSigs < 96) {
        { const char* k = strstr(ln, "\"kills\":\"");                            // paires MESUREES (pair_scan) :
          if (k) { const char* by = strstr(ln, "\"by\":\"");                     // "by" REMPLACE "kills"
                   if (by) chefv2::setKill((uint8_t)dec(k + 9), (uint8_t)dec(by + 6));
                   continue; } }
        { const char* k = strstr(ln, "\"keeps\":\"");                            // paires MESUREES (keeps_scan) :
          if (k) { const char* un = strstr(ln, "\"under\":\"");                  // "keeps" SURVIT sous "under"
                   if (un) { int ttl = 0; const char* tt = strstr(ln, "\"ttlMs\":"); if (tt) ttl = atoi(tt + 8);
                             chefv2::setKeep((uint8_t)dec(k + 9), (uint8_t)dec(un + 9), (uint16_t)ttl); }
                   continue; } }
        const char* c = strstr(ln, "\"cmd\":\"");
        if (!c) continue;
        chefv2::Sig& g = g_sigs[g_nSigs];
        int h = atoi(c + 7); const char* dot = strchr(c + 7, '.');   // "h.v" (banque) -> id etendu, comme bankDecode
        g.id  = (uint8_t)(dot ? (((~h) & 0x1F) * 32 + (atoi(dot + 1) & 0x1F)) : h);
        g.ay0 = (uint8_t)ji(ln, "ay0"); g.ay1 = (uint8_t)ji(ln, "ay1");
        g.dac = (uint8_t)ji(ln, "dac"); g.sp  = (uint8_t)ji(ln, "sp");
        g.ym  = (uint8_t)ji(ln, "ym");
        { const char* p = strstr(ln, "\"sustained\":"); g.sustained = (p && !strncmp(p + 12, "true", 4)) ? 1 : 0; }
        g.durMs = (uint32_t)ji(ln, "durMs"); g.gapMs = (uint32_t)ji(ln, "gapMs"); g.onMs = (uint32_t)ji(ln, "onMs");
        g_nSigs++;
      }
      fclose(sf); g_sigMode = g_nSigs > 0;
      Serial.printf("sounds.sig : %d signatures -> CHEF v2 evenementiel\n", g_nSigs); } }
  g_v2aH = g_v2aT = 0; g_chefCmdExt = -1;
  for (int i = 0; i < 64; i++) g_tagMs[i] = millis();   // grace GC de 8 s pour tout id apres un chargement
  bbLog("=== BOOT %s (chef v2: %d sigs) ===", theme, g_nSigs);
  if (g_sigMode && g_set.find(0)) wavTrigger(0, false);              // 0000 = jingle de DEMARRAGE (la ROM le joue toute
                                                                     // seule a son reset, sans commande -> on fait pareil)
  int nbg = 0;                                                       // auto-play de la musique de fond (attribut i)
  for (int i = 0; i < g_set.nEntry; i++)
    if (g_set.entry[i].attr & wavset::A_INIT) { wavTrigger(g_set.entry[i].id, (g_set.entry[i].attr & wavset::A_LOOP) != 0); nbg++; }
  Serial.printf("PSOWAV set '%s' : %d sons (%d fond auto) -> %s\n", theme, g_set.nEntry, nbg, g_psowav ? "PSOWAV" : "PSOROM (pas de WAV)");
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
  g_liveMode = false;                                   // LIVE RETIRE (decision user 2026-06-13 : "tout est pire en live") — chef uniquement
  if (g_liveMode) { psorom::liveEvents(true); Serial.println("MODE LIVE : emulation complete en direct (Gen1) — les WAV sont ignores"); }
  else { psorom::liveEvents(g_sigMode);                  // CHEF v2 : le conducteur emet ses evenements (ring 2048, DRAM)
         if (g_sigMode) { chefv2::begin(g_sigs, g_nSigs, (gm.gen == 1) ? 1000 : 2000);   // wallclk/ms : Gen1 1 MHz (mesure), Gen2/3 2 MHz
                          Serial.println("CHEF v2 : conducteur evenementiel actif (sounds.sig)"); } }
  g_chefReq = -1; g_chefT0 = esp_timer_get_time(); g_chefEmu = 0;
  g_chefCmd = -1; g_stopFx = false; g_stopMus = false; g_chefArmed = false; g_musWatch = false;   // reset decodeur
  memset(g_eps, 0, sizeof(g_eps)); g_epCur = -1; g_stopTagReq = -1;
  g_hdrBank = 0; g_hdrMs = 0;                                   // (g_hdrN est gere par loadWavSet : defaut 0, @hdr= le renseigne)
  g_pDac = 0; g_dacFreeze = 0; g_tonFreeze = 0;
  g_dacSeen = false; g_tonSeen = false; g_firedFx = false; g_firedTon = false;
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
<div>Volume <b class=s id=volv>100</b>%<input type=range min=0 max=300 value=100 id=vol style="width:100%"> <span style="opacity:.6">(>100% = limiteur doux, plus fort)</span></div>
<div style="margin-top:1rem;border-top:1px solid #2a3340;padding-top:.6rem">
<label class=n>Reseau WiFi &middot; connecte : <b class=s id=sta>--</b></label>
<div style="display:flex;gap:6px;margin:.4rem 0"><input id=wssid list=wlist placeholder="nom reseau WiFi" style="flex:1;background:#1d2430;color:#e7ecf3;border:1px solid #2a3340;border-radius:8px;padding:.5rem"><datalist id=wlist></datalist><button onclick=scan() style="width:auto;padding:.5rem .8rem">scan</button></div>
<div style="display:flex;gap:6px"><input id=wpass placeholder="mot de passe" style="flex:1;background:#1d2430;color:#e7ecf3;border:1px solid #2a3340;border-radius:8px;padding:.5rem"><button onclick=wconn() style="width:auto;padding:.5rem .8rem">connecter</button></div>
<div class=n id=wmsg></div></div>
<div class=g id=pad></div>
<div class=n>cmd libre <input type=number id=cmdn min=0 max=255 value=32 style="width:4rem;background:#1d2430;color:#e7ecf3;border:1px solid #2a3340;border-radius:6px"> <button onclick="fetch('/cmd?n='+cmdn.value)">envoyer</button> <button onclick="fetch('/stop')" style="background:#5a1d1d">STOP</button> &middot; ou clique le pave (0-63)</div>
<script>const pad=document.getElementById('pad'),sel=document.getElementById('game'),vol=document.getElementById('vol'),volv=document.getElementById('volv');
vol.oninput=()=>{volv.textContent=vol.value;fetch('/vol?v='+vol.value);};
for(let i=0;i<64;i++){const b=document.createElement('button');b.textContent=i;b.onclick=()=>fetch('/cmd?n='+i);pad.appendChild(b);}
fetch('/games').then(r=>r.json()).then(d=>{sel.innerHTML='';d.g.forEach(x=>{const o=document.createElement('option');o.value=x.i;o.textContent='G'+x.n+'  '+x.t;if(x.i==d.sel)o.selected=true;sel.appendChild(o);});});
sel.onchange=()=>fetch('/load?i='+sel.value);
function scan(){wmsg.textContent='scan en cours (~3s)...';fetch('/scan').then(r=>r.json()).then(d=>{wlist.innerHTML='';d.forEach(s=>{const o=document.createElement('option');o.value=s;wlist.appendChild(o);});wmsg.textContent=d.length+' reseaux (ou tape le nom a la main)';}).catch(()=>wmsg.textContent='scan KO - tape le nom a la main');}
function wconn(){wmsg.textContent='connexion...';fetch('/wifi?ssid='+encodeURIComponent(wssid.value)+'&pass='+encodeURIComponent(wpass.value)).then(r=>r.text()).then(t=>wmsg.textContent=t+' (attends l IP ci-dessus)');}
function poll(){fetch('/status').then(r=>r.json()).then(d=>{st.textContent=d.st;thr.textContent=d.thr;mix.textContent=d.mix;dac.textContent=d.dac;ym.textContent=d.ym;cmd.textContent=d.cmd<0?"--":d.cmd;bus.textContent=d.bus;sta.textContent=d.sta?d.sta:'(non connecte)';if(document.activeElement!=vol){vol.value=d.vol;volv.textContent=d.vol;}}).catch(()=>{});}
setInterval(poll,500);poll();</script></body></html>)HTML";

// HTTP servi sur SA tâche FreeRTOS (core 0 = cœur WiFi) -> page joignable dès l'AP, indépendamment
// de loop()/SD. Les handlers ne posent que des intentions (volatile) ; loop() (core 1) les applique.
static void httpTask(void*) { for (;;) { server.handleClient(); vTaskDelay(1); } }

static void connectSTA() {                                       // rejoint le WiFi maison (non bloquant)
  String ss = g_prefs.getString("wssid", "");                   // SSID/pass fournis via /wifi (stockes en NVS)
  if (!ss.length()) return;                                      // aucun reseau configure -> reste en AP seul
  WiFi.begin(ss.c_str(), g_prefs.getString("wpass", "").c_str());
  Serial.printf("WiFi STA: connexion a '%s'... (puis http://gosowav.local/)\n", ss.c_str());
}

static void startWeb() {
  WiFi.mode(WIFI_AP_STA);                                        // AP (secours, 192.168.4.1) + STA (reseau maison)
  WiFi.setSleep(true);                                           // sommeil modem ON : la radio dort entre balises -> bien moins
  WiFi.setTxPower(WIFI_POWER_8_5dBm);                            //   de bruit RF couple dans le DAC (le "son de tram" au boot)
  WiFi.softAP("GOSOWAV-PSOROM");
  Serial.printf("web AP 'GOSOWAV-PSOROM' -> http://%s/\n", WiFi.softAPIP().toString().c_str());
  connectSTA();                                                  // si des identifiants sont memorises
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/radio", []() {                                     // gestion WiFi (le WiFi bruite dans le DAC)
    if (server.hasArg("wifi") && server.arg("wifi") == "0") {    //   /radio?wifi=0 : coupe maintenant (audio propre)
      server.send(200, "text/plain", "WiFi OFF. Power-cycle -> 60s de fenetre config au boot.");
      delay(150); WiFi.mode(WIFI_OFF);
    } else if (server.hasArg("keep")) {                          //   /radio?keep=1 : maintient allume (persistant, debug) ; keep=0 : auto-OFF 60s
      g_wifiHold = server.arg("keep") != "0";
      g_prefs.putInt("wifihold", g_wifiHold ? 1 : 0);
      server.send(200, "text/plain", g_wifiHold ? "WiFi maintenu allume (persiste les boots)" : "WiFi: auto-OFF apres 60s -> audio propre");
    } else server.send(200, "text/plain", g_wifiHold ? "wifi: maintenu (hold)" : "wifi: auto-off 60s");
  });
  server.on("/wifi", []() {                                      // /wifi?ssid=X&pass=Y -> memorise + rejoint ; sans arg -> etat
    if (server.hasArg("ssid")) {
      g_prefs.putString("wssid", server.arg("ssid"));
      g_prefs.putString("wpass", server.hasArg("pass") ? server.arg("pass") : String(""));
      server.send(200, "text/plain", "ok, connexion...");
      connectSTA();
    } else {
      String s = "ssid=" + g_prefs.getString("wssid", "(aucun)") + " sta=" +
                 (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("(non connecte)"));
      server.send(200, "text/plain", s);
    }
  });
  server.on("/cmd", []() {
    if (server.hasArg("n")) { int n = server.arg("n").toInt(); if (n >= 0 && n <= 255) cmdPush(n); }
    server.send(200, "text/plain", "ok");
  });
  server.on("/stop", []() { g_stopReq = true; server.send(200, "text/plain", "stop"); });  // intention -> loop() (cœur 1) coupe les WAV
  server.on("/live", []() { server.send(200, "text/plain", "LIVE retire (chef uniquement)"); });
  server.on("/loops", []() {                                       // ecrit /sdcard/<theme>/loops.txt par le web (';' -> retour ligne) + recharge
    if (server.hasArg("d")) {
      char path[80]; snprintf(path, sizeof(path), "%s/%s/loops.txt", MP, g_theme);
      FILE* f = fopen(path, "w");
      if (!f) { server.send(500, "text/plain", "ecriture KO (SD ?)"); return; }
      String d = server.arg("d"); d.replace(";", "\n");
      fwrite(d.c_str(), 1, d.length(), f); fputc('\n', f); fclose(f);
      g_pendingLoad = g_sel;                                       // recharge le jeu courant (loop -> relit loops.txt, thread-safe)
      server.send(200, "text/plain", "loops.txt ecrit, rechargement...");
    } else {                                                       // lecture : renvoie le fichier courant
      char path[80]; snprintf(path, sizeof(path), "%s/%s/loops.txt", MP, g_theme);
      FILE* f = fopen(path, "r"); String o;
      if (f) { char ln[64]; while (fgets(ln, sizeof(ln), f)) o += ln; fclose(f); }
      server.send(200, "text/plain", o.length() ? o : String("(vide)"));
    }
  });
  server.on("/cmdcal", []() {                                      // recale le repos du bus (machine au CALME) -> polarite
    delay(20); s_idle = readBus5(); s_lastBus = 0;
    char b[40]; snprintf(b, sizeof(b), "repos bus -> 0x%02X", s_idle); server.send(200, "text/plain", b);
  });
  server.on("/scan", []() {                                        // liste les reseaux 2.4 GHz (libere la radio d'abord)
    WiFi.scanDelete(); WiFi.disconnect(false, false); delay(150);  // un begin() en cours bloque le scan -> 0 reseaux
    int n = WiFi.scanNetworks(false, true); String j = "[";        // sync, montre les SSID caches
    for (int i = 0; i < n && i < 30; i++) { if (j.length() > 1) j += ","; String s = WiFi.SSID(i); s.replace("\"", ""); if (s.length()) j += "\"" + s + "\""; }
    j += "]"; WiFi.scanDelete(); server.send(200, "application/json", j);
    connectSTA();                                                  // le disconnect() du scan ORPHELINAIT le STA (plus jamais reconnecte) -> rejoint le reseau
  });
  server.on("/vol", []() {                                         // volume maitre 0..200 %
    if (server.hasArg("v")) { int v = server.arg("v").toInt(); if (v >= 0 && v <= 300) g_vol = v; }
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
    g_lastWeb = millis();                                          // page ouverte (poll 500ms) -> maintient le WiFi vivant
    char b[400]; String sta = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
    snprintf(b, sizeof(b),
             "{\"live\":%d,\"thr\":%.2f,\"thrNoIsr\":%.2f,\"cmcps\":%.2f,\"carm\":%d,\"cseen\":%d,\"vd\":%u,\"nv\":%d,\"rw\":%u,\"rwi\":%u,\"clk\":%d,\"mix\":%u,\"dac\":%u,\"ur\":%u,\"cmd\":%d,\"idle\":%d,\"vol\":%d,\"sta\":\"%s\",\"bus\":\"S%d%d%d%d%d F%d STB%d\",\"st\":\"%s\"}",
             g_liveMode ? 1 : 0, g_thrM, g_thrNoIsr, g_chefMcps, g_chefArmed ? 1 : 0, (g_dacSeen ? 2 : 0) | (g_tonSeen ? 1 : 0),
             (unsigned)g_mixer.deaths, g_mixer.activeCount(), (unsigned)g_rwN, (unsigned)g_rwIntMs,
             getCpuFrequencyMhz(), g_mixSps, (unsigned)psorom::dacCount(), (unsigned)s_underruns, g_lastCmd, s_idle, g_vol, sta.c_str(),
             digitalRead(PIN_S[0]), digitalRead(PIN_S[1]), digitalRead(PIN_S[2]), digitalRead(PIN_S[3]), digitalRead(PIN_S[4]),
             digitalRead(35), digitalRead(PIN_STROBE), g_status);
    server.send(200, "application/json", b);
  });
  server.on("/hist", []() {                                        // 16 dernieres commandes recues (id@ms), diagnostic jeu reel
    String h = "";
    for (int i = 0; i < 16; i++) { if (g_histMs[i]) { if (h.length()) h += " "; h += String((int)g_hist[i]) + "@" + String((unsigned long)g_histMs[i]); } }
    server.send(200, "text/plain", h.length() ? h : String("(aucune)"));
  });
  server.on("/ota", HTTP_POST, []() {                              // OTA : nouveau firmware par WiFi -> fini le cable USB
    bool ok = !Update.hasError();
    server.send(200, "text/plain", ok ? "OK, reboot..." : "ECHEC");
    if (ok) { delay(300); ESP.restart(); }
  }, []() {
    HTTPUpload& u = server.upload();
    if (u.status == UPLOAD_FILE_START) { Serial.printf("[ota] debut %s\n", u.filename.c_str()); Update.begin(UPDATE_SIZE_UNKNOWN); }
    else if (u.status == UPLOAD_FILE_WRITE) Update.write(u.buf, u.currentSize);
    else if (u.status == UPLOAD_FILE_END) { Update.end(true); Serial.printf("[ota] fin %u octets -> %s\n", (unsigned)u.totalSize, Update.hasError() ? "ECHEC" : "OK"); }
    else if (u.status == UPLOAD_FILE_ABORTED) Update.abort();
  });
  static FILE* s_upF = nullptr;                                    // /up : POST multipart -> ecrit le fichier dans /sdcard/<theme>/
  server.on("/trace", []() {                                       // BOITE NOIRE : releve du journal par WiFi
    bbFlush(true);
    FILE* f = fopen("/sdcard/trace.log", "rb");
    if (!f) { server.send(404, "text/plain", "pas de trace"); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    String out; out.reserve(sz < 65536 ? sz : 65536);
    if (sz > 65536) fseek(f, sz - 65536, SEEK_SET);                  // les 64 derniers Ko suffisent
    char buf[512]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.concat(buf, n);
    fclose(f);
    server.send(200, "text/plain", out);
  });
  server.on("/up", HTTP_POST, []() { server.send(200, "text/plain", "ok"); }, []() {   // (pousse les WAV regeneres SANS toucher la SD)
    HTTPUpload& u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
      String fn = u.filename; fn.replace("/", ""); fn.replace("\\", "");
      char p[96]; snprintf(p, sizeof(p), "%s/%s/%s", MP, g_theme, fn.c_str());
      s_upF = fopen(p, "wb");
      Serial.printf("[up] %s -> %s\n", fn.c_str(), s_upF ? "ecriture" : "fopen KO");
    } else if (u.status == UPLOAD_FILE_WRITE) { if (s_upF) fwrite(u.buf, 1, u.currentSize, s_upF); }
    else if (u.status == UPLOAD_FILE_END)     { if (s_upF) { fclose(s_upF); s_upF = nullptr; Serial.printf("[up] fin %u octets\n", (unsigned)u.totalSize); } }
    else if (u.status == UPLOAD_FILE_ABORTED) { if (s_upF) { fclose(s_upF); s_upF = nullptr; } }
  });
  server.begin();
  xTaskCreatePinnedToCore(httpTask, "http", 4096, nullptr, 1, nullptr, 0);   // core 0 = cœur WiFi
}

// Monte la SD + lit le manifeste dans SA tâche (un SD bloqué ne gèle ni loop ni le web), puis
// demande le chargement du jeu par défaut via g_pendingLoad (loop() fait le vrai chargement).
// /sdcard/wifi.txt : ligne 1 = SSID, ligne 2 = mot de passe. Si present, il PRIME (change le WiFi sans reflasher).
static void readWifiFromSD() {
  char path[64]; snprintf(path, sizeof(path), "%s/wifi.txt", MP);
  FILE* f = fopen(path, "r"); if (!f) return;
  char ss[64] = {0}, pw[64] = {0};
  if (!fgets(ss, sizeof(ss), f)) { fclose(f); return; }
  fgets(pw, sizeof(pw), f); fclose(f);
  for (size_t n = strlen(ss); n && (ss[n-1]=='\r'||ss[n-1]=='\n'||ss[n-1]==' '||ss[n-1]=='\t'); ) ss[--n] = 0;
  for (size_t n = strlen(pw); n && (pw[n-1]=='\r'||pw[n-1]=='\n'||pw[n-1]==' '||pw[n-1]=='\t'); ) pw[--n] = 0;
  if (!ss[0]) return;
  String cur = g_prefs.getString("wssid", "SFR_E1EF");             // = le reseau que connectSTA a deja utilise au boot
  bool changed = (String(ss) != cur);
  g_prefs.putString("wssid", ss); g_prefs.putString("wpass", pw);  // memorise toujours
  if (changed || WiFi.status() != WL_CONNECTED) {                  // ne reconnecte QUE si reseau different / pas connecte
    Serial.printf("WiFi depuis SD /wifi.txt : '%s' -> (re)connexion\n", ss);
    connectSTA();
  }                                                                 // sinon : on NE COUPE PAS un WiFi qui marche deja
}

static void sdInitTask(void*) {
  if (!mountSD()) { snprintf(g_status, sizeof(g_status), "SD: echec montage (voir serie)"); Serial.println(g_status); vTaskDelete(nullptr); return; }
#ifndef GOSOWAV_NOWIFI
  if (g_webUp) readWifiFromSD();                                  // WiFi depuis la SD SEULEMENT si la radio est deja voulue allumee
#endif                                                            // (sinon connectSTA RALLUMERAIT la radio malgre le OFF par defaut -> bruit RF)
  if (parseManifest() == 0) { Serial.println(g_status); vTaskDelete(nullptr); return; }
  String gid = g_prefs.getString("game", "arena");                // jeu installe (defaut arena) -> auto-boot
  int idx = 0; for (int i = 0; i < g_nGames; i++) if (gid == g_games[i].id) { idx = i; break; }
  Serial.printf("manifeste : %d jeux. Jeu installe -> %s\n", g_nGames, g_games[idx].title);
  g_pendingLoad = idx;                                             // loop() charge le jeu de la machine (mono-thread émulateur)
  vTaskDelete(nullptr);
}

static void chefTask(void*);                                       // DECODEUR ROM-chef (defini plus bas, lance ici)
void setup() {
  Serial.begin(115200); delay(500);
  Serial.printf("\n=== GOSOWAV PSOROM === CPU clock AVANT: %d MHz\n", getCpuFrequencyMhz());
  setCpuFrequencyMhz(240);                                         // FORCE 240 MHz (au cas ou le boot/WiFi l'aurait baisse -> regression 2x ?)
  Serial.printf("CPU clock APRES force 240: %d MHz\n", getCpuFrequencyMhz());
  g_prefs.begin("gosowav", false); g_vol = g_prefs.getInt("vol", 100);   // reglages persistes (montage machine)
  g_prefs.putInt("live", 0); g_liveSticky = false;        // LIVE RETIRE : on efface tout flag live persiste
  g_wifiHold = g_prefs.getInt("wifihold", 0) != 0;                       // WiFi maintenu allume ? (sinon auto-OFF apres la fenetre)
  dacSpiInit();                                                    // MCP4921 sur SPI materiel (VSPI natif du schema)
  dacBitbang(0x3000 | 2048);                                       // mi-échelle = silence
  s_dacTimer = timerBegin(0, 2, true);                             // 80MHz/2 = 40 MHz
  timerAttachInterrupt(s_dacTimer, &onDacTimer, true);
  timerAlarmWrite(s_dacTimer, 40000000 / psorom::ayFs(), true);    // 40MHz/44100 = 907 -> 44.1 kHz
  timerAlarmEnable(s_dacTimer);                                    // -> flux DAC regulier des maintenant
  cmdInputBegin();                                                 // bus commande son (vraie machine System 80)
  pinMode(0, INPUT_PULLUP);                                        // bouton BOOT de la carte = interrupteur WiFi
  pinMode(19, INPUT_PULLUP);                                       // bouton TEST de la carte (schema GOSOWAV_11 : Test=IO19)
  attachInterrupt(digitalPinToInterrupt(34), onResetEdge, CHANGE); // RESET machine sur Strobe_in/IO34 (cable user, A6 pin 9)
#ifdef GOSOWAV_NOWIFI
  WiFi.mode(WIFI_OFF);                                             // TEST BRUIT : aucune radio (ni AP ni STA) des le boot
  Serial.println("WiFi DESACTIVE (build test bruit) — l'Arena se charge seule, ecoute le bruit de fond");
#else
  if (g_wifiHold) { startWeb(); g_webUp = true; }                  // hold persistant (debug etabli) -> comme avant
  else {
    WiFi.mode(WIFI_OFF);                                           // PAR DEFAUT : radio ETEINTE (le WiFi bruite dans le DAC).
    Serial.println("WiFi OFF (audio propre). Pour l'allumer : bouton BOOT de la carte, ou TEST machine (sons croissants).");
  }
#endif
  xTaskCreatePinnedToCore(sdInitTask, "sdinit", 8192, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(chefTask,  "chef",  4096, nullptr, 3, nullptr, 0);   // prio 3 : le conducteur PREEMPTE le serveur web (prio 1) — temps reel d abord
  Serial.println("SD/manifeste dans sa tache. ROM-chef decodeur lance (cœur 0).");
}

// ===== ROM-chef : tache DECODEUR (cœur 0). Fait tourner le 6502 + ROM en ROUE LIBRE et DECODE sa
// sortie pour donner les ordres au player WAV : tant que le son vit (DAC varie / AY-YM sonne) on laisse
// jouer ; quand le CPU coupe un sous-systeme, son activite FIGE -> on leve le drapeau stop correspondant.
// Le silence est mesure en CYCLES EMULES -> fidele a l'original, independant de la vitesse reelle de l'ESP.
// Le DEPART d'un son reste temps-reel : c'est le player (loop, cœur 1) qui lance le WAV sur la commande.
// psorom n'est JAMAIS touche par les 2 cœurs a la fois : cette tache ne tourne que si g_ready && g_psowav,
// et dans ce cas loop() ne fait que mixer des WAV (aucun appel psorom). Pendant load/bench g_ready=false.
static IRAM_ATTR void chefTask(void*) {                            // IRAM : sa boucle ne se bat pas pour le cache flash avec la synthese (coeur 1)
  uint32_t maskBefore = 0, chanEp = 0;                             // canaux tonals AVANT la commande / adoptes par l'episode
  int64_t  epStart = 0, gTonFreeze = 0;                            // debut d'episode / dernier instant ou UN canal (global) sonnait
  bool     epMus = false;                                          // musWatch LATCHE au debut d'episode (jamais lu en cours -> pas de course inter-cœurs)
  bool     parked = true;
  int64_t  rt0 = 0, emu0 = 0;                                      // base du throttle temps-reel
  uint32_t lastYield = 0;
  for (;;) {
    g_chefInEmu = true;                                            // annonce AVANT de toucher psorom (handshake avec loadGame, cœur 1)
    if (!g_ready || !g_psowav) { g_chefInEmu = false; parked = true; vTaskDelay(10 / portTICK_PERIOD_MS); continue; }
    if (parked) { parked = false; rt0 = esp_timer_get_time(); emu0 = g_chefEmu; gTonFreeze = g_chefEmu; }
    if (g_sigMode && !g_liveMode) {                                // CHEF v2 : conducteur evenementiel (regles chefv2.cpp, validees host)
      if (g_chefPause && millis() < g_chefPause) {                 // pause maintenance : le serveur web a besoin du coeur
        g_chefInEmu = false; vTaskDelay(50 / portTICK_PERIOD_MS); continue; }
      if (g_rstReq) { g_rstReq = false;                            // RESET machine : la ROM redemarre comme la vraie carte
        psorom::reset(); psorom::command(0); psorom::run(20000);
        psorom::liveEvents(true);                                  // purge le ring d'evenements
        chefv2::reset();
        g_chefCmdExt = -1; g_chefCmd = -1;
        g_rstJingle = true;                                        // loop() jouera le 0000 (tututut)
      }
      int c2 = g_chefCmd;
      if (c2 >= 0) { int ext = g_chefCmdExt; g_chefCmdExt = -1; g_chefCmd = -1;
        psorom::command((uint8_t)c2);                              // la ROM recoit TOUTE valeur bus (headers de banque inclus)
        if (ext >= 0) chefv2::command((uint8_t)ext, psorom::clockNow());   // chefv2 ne voit que les ids RESOLUS
      }
      const int cycPerUs2 = (g_curGen == 1) ? 2 : 4;               // cycles combines (2 CPU) par us reel
      int64_t lag = (esp_timer_get_time() - rt0) * cycPerUs2 - (g_chefEmu - emu0);   // >0 = conducteur EN RETARD
      uint32_t batch = 4000;                                       // ~4 ms emulees / lot...
      if (lag > 8000) batch = (lag > 64000) ? 32000 : (uint32_t)lag / 2 + 4000;      // ...ou RATTRAPAGE borne (les yields
      static psorom::Ev evb[256];                                  // statique : pas de 2 Ko de pile par passe
      while (batch) {                                              // SOUS-LOTS de 4 ms : un lot de rattrapage entier
        uint32_t b = (batch > 4000) ? 4000 : batch; batch -= b;    // genere ~1 ring d'ecritures AY -> drainer ENTRE
        g_chefEmu += psorom::run(b);                               // les sous-lots (evdrop 80k/musique sinon !)
        int n; while ((n = psorom::liveDrain(evb, 256)) > 0) chefv2::feed(evb, n);
      }
      static uint8_t s_tickDiv = 0;
      if ((++s_tickDiv & 3) == 0) {                                // tick 1 passe sur 4 (~16 ms emulees) : les fenetres
        chefv2::tick(psorom::clockNow());                          // chef font >= 80 ms, le grain suffit — et ca rend
        g_aliveMask = chefv2::aliveMask();                         // au conducteur la marge CPU qui lui manquait
      }
      { chefv2::Action ab[8]; int na;
        while ((na = chefv2::drain(ab, 8)) > 0)
          for (int i = 0; i < na; i++) { uint8_t nx = (uint8_t)((g_v2aH + 1) & 31);
                                         if (nx == g_v2aT) break;  // ring plein : on jette (le GC rattrape)
                                         g_v2a[g_v2aH] = ab[i]; g_v2aH = nx; } }
      if (-lag > 8000) vTaskDelay(1);                              // throttle : jamais durablement PLUS VITE que le vrai board
      { static uint32_t s_vt0 = 0; static int64_t s_ve0 = 0;
        uint32_t nowV = millis();
        if (s_vt0 == 0 || g_chefEmu < s_ve0) { s_vt0 = nowV; s_ve0 = g_chefEmu; }
        else if (nowV - s_vt0 >= 1000) { g_chefMcps = (float)(g_chefEmu - s_ve0) / 1e6f * (1000.0f / (nowV - s_vt0)); s_vt0 = nowV; s_ve0 = g_chefEmu; }
        if (nowV - lastYield >= 100) { lastYield = nowV; vTaskDelay(1); }       // respiration 1% : en prio 3, l'IDLE0 mourait
                                                                                  // de faim WiFi allume -> WDT -> reboot en boucle
        static uint32_t s_pt0 = 0;                                              // telemetrie : LA mesure qui dit si le
        if (s_pt0 == 0) s_pt0 = nowV;                                           // conducteur tient le temps reel en jeu
        else if (nowV - s_pt0 >= 10000) { s_pt0 = nowV;
          Serial.printf("[chef2] cpu %.2fM/s cmd=%u odd=%u%% c5=%u tog 27=%u 26=%u 25=%u 33=%u 32=%u\n",
                        g_chefMcps, (unsigned)g_cmdN, (unsigned)(g_cmdN? 100*g_cmdOdd/g_cmdN : 0), (unsigned)g_cmd5,
                        (unsigned)g_pinTog[0],(unsigned)g_pinTog[1],(unsigned)g_pinTog[2],(unsigned)g_pinTog[3],(unsigned)g_pinTog[4]);
          bbLog("tog 27=%u 26=%u 25=%u 33=%u 32=%u 35=%u 34=%u", (unsigned)g_pinTog[0],(unsigned)g_pinTog[1],(unsigned)g_pinTog[2],(unsigned)g_pinTog[3],(unsigned)g_pinTog[4],(unsigned)g_pinTog[5],(unsigned)g_pinTog[6]);
          for (int b=0;b<7;b++) g_pinTog[b]=0;
          bbLog("telem cpu=%.2f lag=%ld F=%u STR=%u ur=%u vd=%u", g_chefMcps, (long)(lag / (cycPerUs2 * 1000)), (unsigned)g_fN, (unsigned)g_strN,
                (unsigned)s_underruns, (unsigned)g_mixer.deaths); } }
      g_chefInEmu = false;
      vTaskDelay(0);
      continue;
    }
    int c = g_chefCmd;
    if (c >= 0) {                                                  // nouvelle commande -> la CPU la decode + (re)arme l'episode
      g_chefCmd = -1;
      psorom::command((uint8_t)c);
      uint32_t d, t, s; psorom::activitySplit(&d, &t, &s);
      maskBefore = psorom::toneMask(); chanEp = 0; epStart = g_chefEmu;  // les canaux DEJA sonores (musique en cours) ne sont PAS a l'episode
      g_pDac = d; g_dacFreeze = g_chefEmu; g_tonFreeze = g_chefEmu; gTonFreeze = g_chefEmu;
      g_dacSeen = false; g_tonSeen = false; g_firedFx = false; g_firedTon = false;
      g_stopFx = false; g_stopMus = false;                         // stops de l'episode precedent perimes (le nouveau re-detectera)
      epMus = g_musWatch;                                          // latche : l'etat est FINAL (g_chefCmd part apres wavCommand)
      g_chefArmed = true;
      g_epCur = -1;                                                // episode PAR SON : reutilise l'entree du meme id, sinon une libre/tiree
      uint32_t inherit = 0;                                        // RE-declenchement du meme son (roulette en rafale) : ses canaux sont
      for (int i = 0; i < 8; i++) if (g_eps[i].id == c) { g_epCur = i; inherit = g_eps[i].chan; break; }   // DEJA allumes -> l'adoption serait
      if (g_epCur < 0) for (int i = 0; i < 8; i++) if (g_eps[i].fired || g_eps[i].id == 0) { g_epCur = i; break; }  // vide -> on HERITE des canaux connus
      if (g_epCur < 0) { int64_t oldest = INT64_MAX; for (int i = 0; i < 8; i++) if (g_eps[i].tonFreeze < oldest) { oldest = g_eps[i].tonFreeze; g_epCur = i; } }
      g_eps[g_epCur] = ChefEp{ (int16_t)c, inherit, g_chefEmu, inherit != 0, false, g_cmdLp, false };
    }
    if (g_liveMode) {                                              // PSOLIVE : le coeur 0 ne fait QUE tourner les 6502
      g_chefEmu += psorom::run(3000);                              // lots ~1,5 ms emulee
      // ASSERVISSEMENT AU RING AUDIO : l'horloge du DAC (44,1 kHz quartz) est LA reference temps.
      // Ring plein aux 3/4 -> le CPU souffle ; sinon il produit. Verrouillage exact, zero derive,
      // latence constante (~35 ms), et plus JAMAIS la course producteur==consommateur sans marge.
      uint32_t fillL = (s_rHead - s_rTail) & (ARING - 1);
      if (fillL > 1024) { s_thrN++; vTaskDelay(1); }               // cible 23 ms : latence commande->son basse ET le ring
                                                                   // d'evenements ne deborde plus dans les passages denses
      uint32_t nowL = millis();
      if (nowL - lastYield >= 250) { lastYield = nowL; vTaskDelay(1); }                           // respiration watchdog (1 ms / 250 ms = 0,4 % seulement)
      static uint32_t s_lt0 = 0; static int64_t s_le0 = 0; static uint32_t s_lur = 0;
      if (s_lt0 == 0 || g_chefEmu < s_le0) { s_lt0 = nowL; s_le0 = g_chefEmu; }
      else if (nowL - s_lt0 >= 10000) {                             // stats LIVE au serie toutes les 10 s (l'UART bloque ~5 ms le coeur 0 !)
        g_chefMcps = (float)(g_chefEmu - s_le0) / 1e6f * (1000.0f / (nowL - s_lt0));
        Serial.printf("[live] cpu %.2f M/s  ur +%u  ring-min %u  prod %u ech/s  throttle %u/s\n", g_chefMcps, (unsigned)(s_underruns - s_lur), (unsigned)s_minFill, (unsigned)(s_prodN / 10), (unsigned)(s_thrN / 10));
        bbLog("LIVE cpu=%.2f ur+%u ringmin=%u", g_chefMcps, (unsigned)(s_underruns - s_lur), (unsigned)s_minFill);
        s_minFill = 0xFFFF; s_maxGap = 0; s_prodN = 0; s_thrN = 0;
        s_lur = s_underruns; s_lt0 = nowL; s_le0 = g_chefEmu;
      }
      g_chefInEmu = false;
      vTaskDelay(0);
      continue;
    }
    g_chefEmu += psorom::run(4000);                                // ~4 ms emulees / lot
    // --- decode (noyau VALIDE par test host /tmp/test_chef sur la vraie ROM Arena, 4/4) :
    //     DAC (samples, global) + canaux tonals DE L'EPISODE (musique commandee) + tonal GLOBAL (effets) ---
    uint32_t d, t, s; psorom::activitySplit(&d, &t, &s);
    uint32_t mask = psorom::toneMask();
    const int64_t adoptCyc = (g_curGen == 1) ? 1000000 : 2000000;  // fenetre d'adoption ~0.5 s emulee : les canaux qui S'ALLUMENT
    if (g_chefEmu - epStart < adoptCyc) { uint32_t fresh = (mask & ~maskBefore); chanEp |= fresh;
                                          if (g_epCur >= 0) g_eps[g_epCur].chan |= fresh; }   // ... et a SON entree par-son
    if (d != g_pDac)   { g_pDac = d; g_dacFreeze = g_chefEmu; g_dacSeen = true; g_firedFx  = false;   // samples encore actifs
      if (g_epCur >= 0 && g_chefEmu - epStart < adoptCyc && !g_eps[g_epCur].dacEp) {
        g_eps[g_epCur].dacEp = true;                               // le DAC demarre dans la fenetre du nouvel episode -> il PREND le canal
        for (int i = 0; i < 8; i++)                                // (mono-DAC ROM) : l'ancien proprietaire DAC-PUR encore en boucle est TUE
          if (i != g_epCur && g_eps[i].id && g_eps[i].dacEp && !g_eps[i].fired && !g_eps[i].chan && g_eps[i].lp) {
            g_eps[i].fired = true; if (g_stopTagReq < 0) g_stopTagReq = g_eps[i].id;
          }
      }
    }
    if (mask & chanEp) {             g_tonFreeze = g_chefEmu; g_tonSeen = true; g_firedTon = false; }  // l'episode sonne encore (tonal)
    if (mask)          {             gTonFreeze  = g_chefEmu; }    // tonal GLOBAL : la traine DAC d'un ANCIEN son ne doit pas tuer une musique vivante
    {                                                              // PAR SON : ses canaux meurent (>6 s emulees, marge sur la pause max 4.4 s
      uint64_t alive = 0;
      for (int i = 0; i < 8; i++) if (g_eps[i].id && !g_eps[i].fired) alive |= 1ULL << (g_eps[i].id & 63);
      g_aliveMask = alive;
      for (int i = 0; i < 8; i++) { ChefEp& e = g_eps[i]; if (!e.id || e.fired || (!e.chan && !e.dacEp)) continue;
        // boucles : 6 s (marge sur pause interne max 4.4 s) ; one-shots : 1.2 s (la ROM est mono, un son
        // vole/fini doit lacher vite — ex. la roulette s'arrete ~1.6 s reelles apres le dernier clic)
        const int64_t silTon = (e.lp ? (int64_t)g_silLoopMs * 2000 : 2400000) * ((g_curGen == 1) ? 1 : 2);
        if (e.chan) {
          if (mask & e.chan) { e.tonFreeze = g_chefEmu; e.seen = true; }
          else if (e.seen && g_chefEmu - e.tonFreeze > silTon) { e.fired = true; if (g_stopTagReq < 0) g_stopTagReq = e.id; }
        } else if (e.dacEp && e.lp) {                              // DAC-PUR en boucle (roller) : meurt quand le DAC GLOBAL
          if (g_chefEmu - g_dacFreeze > silTon) { e.fired = true;  // se tait silTon (hors musique : spin fini = silence DAC)
            if (g_stopTagReq < 0) g_stopTagReq = e.id; }
        }
      }
    }
    if (g_chefArmed) {
      // ~1.0 s emulee. MESURE (host /tmp/gaps sur les ROM Arena) : pauses INTERNES d'un morceau jusqu'a
      // 4.4 s tonal (tune 9, vit sur le DAC) ou 2.9 s DAC (tune 11, vit sur le tonal), mais la pause
      // COMBINEE (ton+DAC muets ensemble) reste <= ~260 ms -> 1.0 s = marge x4 sans confondre soupir et fin.
      const int64_t silCyc = (g_curGen == 1) ? 2000000 : 4000000;
      bool quiet = (g_chefEmu - gTonFreeze > silCyc) && (g_chefEmu - g_dacFreeze > silCyc);   // PLUS RIEN : ni tonal (global) ni samples
      // SANS exigence d'evidence : une commande STOP (ex. 31) rend la ROM silencieuse SANS produire de son
      // -> il faut tirer quand meme (si rien ne joue cote WAV, c'est un no-op inoffensif).
      if (quiet && !g_firedFx)          { g_stopFx  = true; g_firedFx  = true; }   // la ROM est SILENCIEUSE -> fin des boucles d'effet
      if (epMus && quiet && !g_firedTon){ g_stopMus = true; g_firedTon = true; }   // -> fin de la musique surveillee (cmd stop ou fin naturelle)
    }
    // --- throttle temps-reel : ne jamais DEPASSER la cadence du vrai board (exact sur silicium rapide ; la WROVER, plus lente, ne throttle jamais) ---
    const int cycPerUs = (g_curGen == 1) ? 2 : 4;                  // cycles combines (2 CPU) par µs reel
    if ((g_chefEmu - emu0) - (esp_timer_get_time() - rt0) * cycPerUs > 8000) vTaskDelay(1);
    // --- debit mesure + respiration watchdog ---
    static uint32_t s_mt0 = 0; static int64_t s_me0 = 0;
    uint32_t nowMs = millis();
    if (s_mt0 == 0 || g_chefEmu < s_me0) { s_mt0 = nowMs; s_me0 = g_chefEmu; }   // (re)base apres un rechargement
    else if (nowMs - s_mt0 >= 1000) { g_chefMcps = (float)(g_chefEmu - s_me0) / 1e6f * (1000.0f / (nowMs - s_mt0)); s_mt0 = nowMs; s_me0 = g_chefEmu; }
    if (nowMs - lastYield >= 50) { lastYield = nowMs; vTaskDelay(1); }           // 1 tick / 50 ms : IDLE0 nourrit le task-watchdog (sinon famine -> WDT)
    g_chefInEmu = false;                                           // sorti de psorom -> un rechargement peut proceder
    vTaskDelay(0);                                                 // cede (WiFi/http partagent le cœur 0)
  }
}

void loop() {
  if (g_pendingLoad >= 0) {                                        // (re)chargement de jeu demandé
    int i = g_pendingLoad; g_pendingLoad = -1;
    g_ready = false;
    do { vTaskDelay(2 / portTICK_PERIOD_MS); } while (g_chefInEmu);  // attend que le decodeur (cœur 0) soit SORTI de psorom avant de le reinitialiser
    if (loadGame(i)) {
      g_sel = i;
      g_prefs.putString("game", g_games[i].id);                    // memorise le jeu installe (auto-boot machine)
      if (!g_benched) {                                            // bench débit une seule fois
        g_benched = true;
        psorom::command(22);
        uint32_t t0 = millis(), cyc = 0;
        while (millis() - t0 < 1000) { cyc += psorom::run(200000); vTaskDelay(0); }
        g_thrM = cyc / 1e6f;
        Serial.printf("THROUGHPUT (ISR DAC ON): %.2f M 6502-cycles/sec   (80B temps-reel ~2.0 M)\n", g_thrM);
        timerAlarmDisable(s_dacTimer);                            // coupe l'ISR DAC -> mesure run() SEUL (sans le streaming audio)
        uint32_t t0b = millis(), cyc2 = 0;
        while (millis() - t0b < 1000) { cyc2 += psorom::run(200000); vTaskDelay(0); }
        g_thrNoIsr = cyc2 / 1e6f;
        timerAlarmEnable(s_dacTimer);                             // remet l'ISR (audio reprend)
        Serial.printf("THROUGHPUT (ISR DAC OFF): %.2f M  -> le streaming DAC vole %.0f%% du CPU\n",
                      g_thrNoIsr, g_thrNoIsr > 0 ? 100.0f * (1.0f - g_thrM / g_thrNoIsr) : 0);
        int sb = psorom::synthBench(500);
        Serial.printf("SYNTHESE seule (AYx2+SP0250+mix): %d ech/s  (besoin 44100 sur le coeur 1 -> %s)\n",
                      sb, sb >= 44100 ? "OK" : "TROP LENT");
        int16_t tb[64]; uint32_t t1 = millis(), smp = 0;          // debit renderMix (emu+puces) = temps-reel ?
        while (millis() - t1 < 500) smp += psorom::renderMix(tb, 64);
        g_mixSps = smp * 2;
        Serial.printf("renderMix: %u ech/s  (besoin %d = temps-reel%s)\n", g_mixSps, psorom::ayFs(),
                      g_mixSps >= (uint32_t)psorom::ayFs() ? " OK" : " TROP LENT !");
      }
      if (g_liveMode || g_sigMode) psorom::liveEvents(true);       // le bench a inonde le ring -> repart proprement
      g_ready = true;
    }
    return;
  }
  if (!g_ready) { vTaskDelay(5 / portTICK_PERIOD_MS); return; }    // pas de jeu chargé : le web reste vivant
#ifdef GOSOWAV_NOWIFI
  static bool s_autoTest = false;                                 // build test : joue la boucle 6 sans WiFi -> ecoute si propre
  if (!s_autoTest) { s_autoTest = true; cmdPush(6); Serial.println("auto-test: boucle 6 (WiFi off)"); }
#endif
  if (g_busHold) { uint8_t h = g_busHold; g_busHold = 0;
    Serial.printf("[%lu] bus: valeur %d TENUE >1s\n", millis(), h); bbLog("HOLD %d >1s", h); }
  bbFlush(false);                                                  // boite noire -> SD (groupee, 4 s)
  while (g_busT != g_busH) { uint8_t bc = g_busRing[g_busT]; g_busT = (uint8_t)((g_busT + 1) & 3);
    g_cmdN++; if (bc & 1) g_cmdOdd++; if (bc == 5) g_cmd5++;
    cmdPush(bc); }   // bus : draine l'ISR (11 kHz) + stats S1
  { static uint32_t s_lastRst = 0;                                 // RESET machine : pulse termine + refractaire 500 ms
    uint16_t ne = g_rstEdges;
    if (ne && millis() - g_rstEdgeMs > 50 && millis() - s_lastRst > 500) {
      s_lastRst = millis(); g_rstEdges = 0;
      Serial.printf("[%lu] bus: RESET machine (%u fronts) -> silence + jingle\n", millis(), ne);
      bbLog("RESET machine (%u fronts)", ne);
      g_mixer.stopAll();                                           // la carte d'origine se tait au reset...
      g_rstReq = true;                                             // ...la ROM emulee redemarre (chefTask, coeur 0)
    } else if (ne && millis() - g_rstEdgeMs > 50) g_rstEdges = 0;  // fronts residuels dans la fenetre refractaire
    if (g_rstJingle) { g_rstJingle = false;
      if (g_sigMode && g_set.find(0)) wavTrigger(0, false); } }    // ...et joue son jingle (0000 = tututut)
  { static uint32_t s_fsMs = 0; int guardFs = 0;                   // fronts F/Strobe : imprime (rate-limite a 8/passe + 100 ms)
    while (g_fsT != g_fsH && guardFs++ < 8) { uint8_t e = g_fsEdge[g_fsT]; g_fsT = (uint8_t)((g_fsT + 1) & 7);
      if (millis() - s_fsMs >= 100) { s_fsMs = millis();
        Serial.printf("[%lu] bus: %s -> %d\n", millis(), (e & 2) ? "STROBE" : "F", e & 1); } } }
#ifndef GOSOWAV_NOWIFI
  if (!g_webUp) {                                                  // WiFi eteint : declencheurs d'allumage
    static uint32_t s_btMs = 0;
    static int s_tIdle = -1; if (s_tIdle < 0) s_tIdle = digitalRead(19);     // niveau repos du bouton TEST (polarite auto)
    bool press = (digitalRead(0) == LOW) || (digitalRead(19) != s_tIdle);    // BOOT (GPIO0) OU TEST (GPIO19, schema v11)
    if (press) { if (!s_btMs) s_btMs = millis();
      else if (millis() - s_btMs > 50) { g_webUp = true; Serial.println("WiFi: bouton carte -> ON"); bbLog("WiFi ON (bouton)"); startWeb(); } }
    else s_btMs = 0;
  }
#endif
  int pc = (g_chefReq < 0) ? cmdPop() : -1;                        // une commande par passe, et SEULEMENT si la place est
  if (pc >= 0) {                                                   // libre (rafale : g_chefReq ecrase = commande perdue)
    { static int hi = 0; g_hist[hi & 15] = (uint16_t)pc; g_histMs[hi & 15] = millis(); hi++; }   // trace des 16 dernieres (diagnostic /status)
#ifndef GOSOWAV_NOWIFI
    // (l'allumage WiFi par sequence croissante est SUPPRIME : les fantomes bus le declenchaient en pleine
    //  partie -> le serveur web volait ~5% du conducteur. Le WiFi s'allume par le bouton BOOT ou TEST.)
#endif
    if (g_psowav) g_chefReq = pc;                                  // ROM-chef : le player lance le WAV ; g_chefCmd part APRES wavCommand
    else psorom::command((uint8_t)pc);                             // (sinon le chef voit le musWatch du nouvel episode avec le quiet de l'ancien -> tir fantome)
  }

  int guard = 0;
  if (g_liveMode) {
    { static uint32_t lp = 0; uint32_t n = millis(); if (lp && n - lp > s_maxGap) s_maxGap = n - lp; lp = n; }
    // --- PSOLIVE : son = synthese pilotee par les evenements du VRAI programme. Aucun WAV. ---
    if (g_stopReq) g_stopReq = false;                              // (rien a couper : la ROM gere tout)
    if (g_chefReq >= 0 && g_chefCmd < 0) { int rq = g_chefReq; g_chefReq = -1; g_chefCmd = rq; }   // commande -> CPU (contre-pression)
    static int16_t lb[128];
    while (guard++ < 16) {
      uint32_t freeN = (s_rTail - s_rHead - 1) & (ARING - 1);
      if (freeN == 0) break;
      int want = (freeN < 128u) ? (int)freeN : 128;
      int n = psorom::liveRender(lb, want);
      if (n <= 0) break;
      s_prodN += n;
      for (int i = 0; i < n; i++) ringPush(lb[i]);
    }
  } else if (g_psowav) {
    // --- ROM-chef : le CPU emule (chefTask, cœur 0) DECODE la ROM et leve les stops ; ici (cœur 1) on
    // DEMARRE les WAV en temps-reel et on applique les ordres. Stops AVANT la commande : ils appartiennent
    // a l'episode PRECEDENT (sinon ils tueraient le son qu'on vient de lancer).
    if (g_stopFx)  { g_stopFx  = false; g_mixer.stopActiveLoops(); Serial.printf("[%lu] chef: STOP-FX\n", millis()); }   // la ROM est silencieuse -> stop boucles d'effet
    if (g_stopMus) { g_stopMus = false; g_mixer.stopActiveLoops(); g_mixer.stopBgLoops(); g_musWatch = false;
                     Serial.printf("[%lu] chef: STOP-MUSIQUE\n", millis()); }            // la ROM a fini SA musique -> stop la boucle musicale (bg ou non)
    if (g_stopReq) { g_stopReq = false; g_mixer.stopAll(); g_musWatch = false; }        // bouton STOP (web) -> coupe tout
    if (g_stopTagReq >= 0) { int t = g_stopTagReq; g_stopTagReq = -1; g_mixer.stopTag(t);    // la ROM a fini CE son -> on coupe SES voix
                             Serial.printf("[%lu] chef: STOP-SON %d (ses canaux ROM sont morts)\n", millis(), t); }
    if (g_chefReq >= 0 && g_chefCmd < 0) {                                              // CONTRE-PRESSION : on ne dispatch que si le
                          int rq = g_chefReq; g_chefReq = -1;                           // conducteur a consomme la precedente (ecraser
                          if (g_sigMode) { int ext = bankDecode(rq);                    // g_chefCmd = commande perdue pour la ROM ET chefv2)
                                           static uint32_t s_lastBusMs = 1;              // DEBUT DE PARTIE : 1re commande apres 20 s de
                                           if (millis() - s_lastBusMs > 20000                       // silence bus (l'attract est MUET sur cette
                                               && g_set.find(0)) {                                  // machine) -> jingle du START (remplace le
                                             wavTrigger(0, false);                                  // pulse RESET au fil fragile)
                                             Serial.printf("[%lu] debut de partie -> jingle\n", millis());
                                             bbLog("DEBUT PARTIE -> jingle");
                                           }
                                           s_lastBusMs = millis();
                                           if (ext >= 0) g_tagMs[ext & 63] = millis();  // (coeur 0) ordonne START/STOP/RESTART, appliques
                                           Serial.printf("[%lu] bus: cmd %d -> son %d\n", millis(), rq, ext);
                                           bbLog("bus %d -> %d", rq, ext);
                                           g_chefCmdExt = ext; g_chefCmd = rq; }        // ci-dessous (ext fixe AVANT g_chefCmd, pas de course)
                          else { g_tagMs[rq & 63] = millis();
                                 wavCommand(rq);                                        // DEPART temps-reel (fixe musWatch)
                                 g_chefCmd = rq; } }                                    // PUIS seulement le chef decode (etat final, pas de course)
    if (g_sigMode) {                                                                    // actions du CHEF v2 (ring SPSC coeur 0 -> coeur 1)
      while (g_v2aT != g_v2aH) {
        chefv2::Action a = g_v2a[g_v2aT]; g_v2aT = (uint8_t)((g_v2aT + 1) & 31);
        if (a.op == 1) { g_mixer.stopTag(a.id); Serial.printf("[%lu] chef2: STOP %d (t=%lu)\n", millis(), a.id, (unsigned long)a.tMs);
                         bbLog("STOP %d", a.id); }
        else {
          bool lp = false;
          for (int i = 0; i < g_nSigs; i++) if (g_sigs[i].id == a.id) { lp = g_sigs[i].sustained != 0; break; }
          if (g_oneShot[a.id]) lp = false;                                              // override loops.txt (verite terrain si besoin)
          g_mixer.stopTag(a.id);                                                        // la ROM est mono PAR SON -> (re)depart propre, pas d'empilage
          if (g_set.find(a.id)) { wavTrigger(a.id, lp);
                                  Serial.printf("[%lu] chef2: %s %d lp=%d\n", millis(), a.op == 2 ? "RESTART" : "START", a.id, (int)lp);
                                  bbLog("%s %d lp=%d", a.op == 2 ? "RESTART" : "START", a.id, (int)lp); }
          else { Serial.printf("[%lu] chef2: %s %d SANS WAV (a rendre/ajouter !)\n", millis(), a.op == 2 ? "RESTART" : "START", a.id);
                 bbLog("%s %d SANS-WAV", a.op == 2 ? "RESTART" : "START", a.id); }
        }
      }
    }
    static uint32_t s_gcMs = 0;                                                         // GC : toute boucle dont le son n'a PLUS d'episode vivant
    static uint8_t s_orph[64] = {0};                                                    // ET sans commande depuis 8 s = ORPHELINE -> coupe
    if (millis() - s_gcMs > 2000) { s_gcMs = millis();                                  // (confirmee sur 2 passes : g_aliveMask est un uint64
      int tags[8]; int nt = g_mixer.loopTags(tags, 8);                                  //  inter-coeurs, une lecture dechiree ne doit pas tuer)
      uint64_t seen = 0;
      for (int i = 0; i < nt; i++) { int t = tags[i] & 63; seen |= 1ULL << t;
        if (!((g_aliveMask >> t) & 1) && millis() - g_tagMs[t] > 8000) {
          if (++s_orph[t] >= 2) { s_orph[t] = 0; g_mixer.stopTag(tags[i]);
                                  Serial.printf("[%lu] chef: GC boucle orpheline %d\n", millis(), tags[i]); }
        } else s_orph[t] = 0; }
      for (int t = 0; t < 64; t++) if (!((seen >> t) & 1)) s_orph[t] = 0; }
    static int16_t wb[wavmix::BLOCK * 2];                          // ring <- wavmix (down-mix mono)
    while (guard++ < 16) {                                         // B2: ne mixer QUE ce qui rentre (sinon mix() rend 128 frames
      uint32_t freeN = (s_rTail - s_rHead - 1) & (ARING - 1);      //     dont jusqu'a 127 jetees + lectures SD gaspillees -> underruns rythmes)
      if (freeN == 0) break;
      int n = (freeN < (uint32_t)wavmix::BLOCK) ? (int)freeN : wavmix::BLOCK;
      g_mixer.mix(wb, n);
      for (int i = 0; i < n; i++) ringPush((int16_t)((wb[2*i] + wb[2*i+1]) >> 1));
    }
  } else {
    if (g_stopReq) g_stopReq = false;                              // (pas de WAV a couper en mode PSOROM)
    while (guard++ < 96) {                                         // B2 aussi ici : renderMix avance le temps emule par echantillon ;
      uint32_t freeN = (s_rTail - s_rHead - 1) & (ARING - 1);      // rendre 32 et en jeter une partie DECALAIT le son (echantillons perdus)
      if (freeN == 0) break;
      int16_t buf[32]; int want = (freeN < 32u) ? (int)freeN : 32;
      int n = psorom::renderMix(buf, want);                        // DAC + AY + voix (émulateur live, exact)
      if (n <= 0) break;
      for (int i = 0; i < n; i++) ringPush(buf[i]);
    }
  }
  delayMicroseconds(300);                                          // ring plein -> petite pause (l'ISR vide)
  static uint32_t s_vd = 0;                                        // diagnostic : une voix vient de mourir de famine (source/SD morte)
  if (g_mixer.deaths != s_vd) { s_vd = g_mixer.deaths; Serial.printf("[%lu] VOIX MORTE (famine source, total %u)\n", millis(), (unsigned)s_vd); }
  static int s_volSaved = -1, s_volPrev = -1; static uint32_t s_volMs = 0;   // persiste le volume (debounce 3s, anti-usure NVS)
  if (g_vol != s_volPrev) { s_volPrev = g_vol; s_volMs = millis(); }
  if (g_vol != s_volSaved && millis() - s_volMs > 3000 &&
      (g_mixer.activeCount() == 0 || millis() - s_volMs > 30000))  // ecrit la NVS aux moments SILENCIEUX (l'ISR DAC n'est pas en IRAM :
    { g_prefs.putInt("vol", g_vol); s_volSaved = g_vol; }          // une ecriture flash la suspend ~ms -> micro-trou audible) ; force apres 30 s
  while (Serial.available()) {                                     // commande serie NON-bloquante (parseInt bloquait 1 s sur le '\n'
    static int s_acc = -1; int ch = Serial.read();                 // -> underruns garantis apres chaque commande tapee)
    if (ch >= '0' && ch <= '9') s_acc = (s_acc < 0 ? 0 : s_acc * 10) + (ch - '0');
    else if (ch == 't') {                                          // 't' = deverse la boite noire par le cable
      bbFlush(true);
      FILE* tf = fopen("/sdcard/trace.log", "rb");
      if (tf) { Serial.println("=== TRACE DEBUT ==="); char tb[256]; size_t tn;
                while ((tn = fread(tb, 1, sizeof(tb), tf)) > 0) Serial.write((const uint8_t*)tb, tn);
                fclose(tf); Serial.println("=== TRACE FIN ==="); }
      else Serial.println("pas de trace");
      s_acc = -1; }
    else if (ch == 'w') { if (!g_webUp) { g_webUp = true; startWeb(); } s_acc = -1; }   // 'w' = WiFi on
    else if (ch == 'p') { g_chefPause = millis() + 60000; Serial.println("chef en pause 60s (maintenance)"); s_acc = -1; }   // 'p' = pause conducteur
    else if (ch == 'L') { Serial.println("LIVE retire (chef uniquement)"); s_acc = -1; }
    else if (s_acc >= 0) { if (s_acc <= 255) { cmdPush(s_acc); Serial.printf("-> cmd %d\n", s_acc); } s_acc = -1; }
  }
  static bool s_mdns = false;                                    // demarre mDNS + annonce l'IP des que la STA est connectee
  if (!s_mdns && WiFi.status() == WL_CONNECTED) {
    s_mdns = true;
    if (MDNS.begin("gosowav")) MDNS.addService("http", "tcp", 80);
    Serial.printf("WiFi STA OK -> http://%s/  (aussi http://gosowav.local/) — reste allume ; /radio?wifi=0 pour couper\n",
                  WiFi.localIP().toString().c_str());
  }
  // WiFi reste allume en permanence (hotspot toujours visible). Pour du son 100% propre en jeu :
  //   - coupe a la demande : /radio?wifi=0   (reboot pour le rallumer)
  //   - ou flashe gosowav_nowifi (zero radio).  Pas de coupure auto -> plus de hotspot qui disparait.
}
#endif // GOSOWAV_BENCH
