// wavplayer.cpp — see wavplayer.h. ESP32-S3 sound tier. Not built on C3.
//
// Output: MCP4921 12-bit SPI DAC (mono). A cycle-paced "dac" task (core 0) clocks one
// framed SPI word per sample from a lock-free SPSC ring that the "mix"+SD task (core 1)
// fills. The mix task owns the mixer + SD + the sound-set index (wavset): it scans the
// theme folder, resolves a sound id (incl. random/sequential groups) to a file, applies
// the PSOWAV attributes (loop / break / kill / soft-kill / quit / voice bus / per-sound
// volume), streams the WAV into a mixer voice, mixes, down-mixes to mono 12-bit.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#ifndef BOARD_C3
#include "wavplayer.h"
#include "ownership.h"
#include "wavmix.h"
#include "wavsrc.h"
#include "wavfile.h"
#include "wavset.h"
#include "sndroute.h"
#include "board_config.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <string.h>
#include <atomic>
#include "esp_task_wdt.h"
#include "esp_random.h"
#include "driver/i2s.h"   // legacy I2S (Arduino-ESP32 2.0.17 / IDF 4.4) — PCM5102A output

namespace {
  constexpr int RATE   = AUDIO_RATE;
  constexpr int FRAMES = 256;                  // stereo frames mixed per I2S block

  wavmix::Mixer  mixer;                         // mix task only
  wavset::Set    sndset;                        // mix task only (theme index)
  wavset::Config cfg;                           // global config.txt (loaded once)
  SPIClass       sdspi(HSPI);                   // SD only (the DAC is now I2S/DMA, no SPI)
  char           theme[24] = "orgsnd";          // mix task only
  volatile bool  g_ready = false;
  // cached status for the web UI (written by mix task, read for display elsewhere)
  volatile uint32_t sndMask = 0;                  // bit i => sound id i present in the set
  volatile uint32_t loopM = 0, voiceM = 0;        // bit i => sound i loops / is on the voice bus
  volatile int   nSnd   = 0;                       // # sounds in the loaded set
  char           themes[24][24];                   // SD-root game folders (cached at begin)
  volatile int   nThemes = 0;
  char           gameMap[64][24] = {{0}};          // /games.txt: FPGA game No -> romname/folder
  volatile int   g_lastSound = -1;                 // last sound id played (OLED/status)
  volatile int   curGameNo = -1;                   // FPGA game No of the loaded set (for hybrid routing)
  bool           g_hybrid = false;                 // config.txt sndmode=hybrid -> GOSOF80 does part of the sound
  bool           g_hasBanks = false;               // loaded set has banked sounds (id>=32) -> 80B bank/stop semantics

  struct Slot {
    File f; wavsrc::Source src;
    uint32_t dataOffset, dataLen; uint8_t channels;
    int vid; bool used;
  };
  Slot slot[wavmix::MAX_VOICES];

  struct Req { uint8_t type; int sound; char theme[24]; };   // 0=play id, 1=set-theme
  QueueHandle_t reqQ = nullptr;

  size_t file_read(void* ctx, uint8_t* d, size_t n) { return ((File*)ctx)->read(d, n); }
  // voice adapters: ctx = &Slot, so loop can re-seek the file
  size_t slot_fill(void* p, int16_t* dst, size_t frames) { return wavsrc::fill(&((Slot*)p)->src, dst, frames); }
  bool   slot_rewind(void* p) {
    Slot* s = (Slot*)p;
    if (!s->f || !s->f.seek(s->dataOffset)) return false;
    wavsrc::init(s->src, file_read, &s->f, s->channels, s->dataLen);
    return true;
  }

  // Walk RIFF chunks to find fmt + data (offset/len). Leaves f at data on success.
  bool findWav(File& f, WavInfo& wi) {
    wi = WavInfo{};
    uint8_t b[16];
    if (f.read(b, 12) != 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return false;
    while (f.read(b, 8) == 8) {
      uint32_t len = b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24);
      if (!memcmp(b, "fmt ", 4)) {
        uint8_t fmt[16];
        if (f.read(fmt, 16) != 16) return false;
        wi.format   = fmt[0]  | (fmt[1] << 8);
        wi.channels = fmt[2]  | (fmt[3] << 8);
        wi.rate     = fmt[4]  | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
        wi.bits     = fmt[14] | (fmt[15] << 8);
        uint32_t extra = (len > 16 ? len - 16 : 0) + (len & 1);
        if (extra) f.seek(f.position() + extra);
      } else if (!memcmp(b, "data", 4)) {
        wi.dataOffset = f.position();
        wi.dataLen    = len;
        wi.ok = (wi.format == 1 && (wi.channels == 1 || wi.channels == 2) && wi.bits == 16);
        return wi.ok;
      } else {
        f.seek(f.position() + len + (len & 1));
      }
    }
    return false;
  }

  void reapSlots() {                            // free slots whose voice ended/was stopped
    for (int i = 0; i < wavmix::MAX_VOICES; i++)
      if (slot[i].used && !mixer.active(slot[i].vid)) { slot[i].f.close(); slot[i].used = false; }
  }

  void startVoice(const wavset::Entry* e) {
    // attribute-driven stops first (apply to existing voices), then reclaim their slots
    if      (e->attr & wavset::A_KILL)  mixer.stopAll();
    else if (e->attr & wavset::A_SKILL) mixer.stopExcept(true, false, false);
    else if (e->attr & wavset::A_QUIT)  mixer.stopExcept(true, true, true);
    if (e->attr & wavset::A_BREAK)      mixer.stopTag(e->id);
    // Mono background music: a new looping (non-voice) sound REPLACES the current loop instead
    // of stacking. Verified against the board (SEQ test on excaliba: a sustained sound is taken
    // over by the next command) and prevents loop pile-up that would exhaust the 8 voices.
    // Oneshot effects still layer + self-terminate; speech (voice bus) is untouched.
    if ((e->attr & wavset::A_LOOP) && !(e->attr & wavset::A_VOICE))
      mixer.stopActiveLoops();
    reapSlots();
    if (e->attr & wavset::A_PLACE) return;       // x = placeholder, no audio

    int si = -1;
    for (int i = 0; i < wavmix::MAX_VOICES; i++) if (!slot[i].used) { si = i; break; }
    if (si < 0) {
      static uint32_t lastW = 0; uint32_t now = millis();
      if (now - lastW > 1000) { lastW = now; log_w("[snd] all voices busy, dropped id %d", e->id); }
      return;
    }
    char path[96]; snprintf(path, sizeof(path), "/%s/%s", theme, e->file);
    File f = SD.open(path, FILE_READ);
    if (!f) { log_w("[snd] open fail %s", path); return; }
    WavInfo wi;
    if (!findWav(f, wi)) { log_w("[snd] not PCM16 %s", path); f.close(); return; }
    if (wi.dataOffset >= (uint32_t)f.size() || !f.seek(wi.dataOffset)) { f.close(); return; }
    uint32_t avail = (uint32_t)f.size() - wi.dataOffset;
    uint32_t dlen  = (wi.dataLen && wi.dataLen <= avail) ? wi.dataLen : avail;

    slot[si].f = f; slot[si].dataOffset = wi.dataOffset; slot[si].dataLen = dlen; slot[si].channels = (uint8_t)wi.channels;
    wavsrc::init(slot[si].src, file_read, &slot[si].f, (uint8_t)wi.channels, dlen);

    uint8_t bus = (e->attr & wavset::A_VOICE) ? cfg.volv : cfg.vols;       // voice vs sound scaling
    uint32_t g = (uint32_t)e->vol * bus * 255 / 10000; if (g > 255) g = 255;

    wavmix::VoiceCfg vc;
    vc.fill = slot_fill; vc.ctx = &slot[si]; vc.rewind = slot_rewind;
    vc.gain = (uint8_t)g; vc.tag = e->id;
    vc.loop = (e->attr & wavset::A_LOOP) != 0;
    vc.bg   = (e->attr & wavset::A_INIT) != 0;
    vc.voice= (e->attr & wavset::A_VOICE) != 0;
    int vid = mixer.trigger(vc);
    if (vid < 0) { slot[si].f.close(); return; }
    slot[si].vid = vid; slot[si].used = true;
  }

  void loadTheme(const char* name) {
    mixer.stopAll(); reapSlots();
    strncpy(theme, name, sizeof(theme) - 1); theme[sizeof(theme) - 1] = 0;
    sndset.reset();
    if (!ownership::allowed(theme)) {                 // proof-of-ownership gate: locked until a
      g_hasBanks = false; sndMask = loopM = voiceM = 0; nSnd = 0;   // verified dump of this game
      log_w("[snd] '%s' LOCKED (ownership gate) — dump this game's CPU ROM to unlock", theme);
      return;
    }
    char dirpath[32]; snprintf(dirpath, sizeof(dirpath), "/%s", theme);
    File dir = SD.open(dirpath);
    if (dir && dir.isDirectory()) {
      for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (!f.isDirectory()) {
          const char* nm = f.name(); const char* base = strrchr(nm, '/');
          sndset.addName(base ? base + 1 : nm);
        }
        f.close();
      }
    } else {
      log_w("[snd] theme dir missing: %s", dirpath);
    }
    if (dir) dir.close();
    mixer.setMix(cfg.mix);
    g_hasBanks = false;
    { uint32_t m = 0, lm = 0, vm = 0; for (int i = 0; i < sndset.nEntry; i++) {
        const wavset::Entry& e = sndset.entry[i]; if (e.id >= 32) g_hasBanks = true; if (e.id < 0 || e.id >= 32) continue;
        m |= (1u << e.id);
        if (e.attr & wavset::A_LOOP)  lm |= (1u << e.id);
        if (e.attr & wavset::A_VOICE) vm |= (1u << e.id); }            // cache set status for web UI
      sndMask = m; loopM = lm; voiceM = vm; nSnd = sndset.nEntry; }
    log_i("[snd] theme '%s': %d sounds, %d groups", theme, sndset.nEntry, sndset.nGroup);
    for (int i = 0; i < sndset.nEntry; i++)               // autoplay init/background sounds
      if (sndset.entry[i].attr & wavset::A_INIT) startVoice(&sndset.entry[i]);
  }

  void handleReq(const Req& r) {
    if (r.type == 1) { loadTheme(r.theme); return; }
    if (r.type == 2) { mixer.stopAll(); reapSlots(); return; }   // stop all voices (web UI)
    int id = sndset.pick(r.sound, esp_random());          // resolve random/sequential group
    const wavset::Entry* e = sndset.find(id);
    if (e) startVoice(e);
    else   log_w("[snd] no sound %d in theme '%s'", r.sound, theme);
  }

  // --- core 1: mixer + SD + set owner; writes mixed audio straight to I2S (DMA paces it) -------
  // No more SPSC ring + cycle-paced DAC task: the I2S peripheral clocks the DMA buffer out by
  // itself, so i2s_write() blocks only when the DMA queue is full -> natural pacing, ~0 CPU, and
  // core 0 is now free.
  void mixTask(void*) {
    static int16_t buf[FRAMES * 2];   // mixer output (stereo int16)
    static int16_t out[FRAMES * 2];   // mono down-mix duplicated to L/R for the I2S frame
    esp_task_wdt_add(nullptr);
    Req req;
    for (;;) {
      if (xQueueReceive(reqQ, &req, 0) == pdTRUE) handleReq(req);   // one set/play op per pass
      mixer.mix(buf, FRAMES);
      for (int i = 0; i < FRAMES; i++) {                            // down-mix to mono (clip), duplicate L/R
        int32_t m = ((int32_t)buf[2 * i] + buf[2 * i + 1]) >> 1;
        if (m > 32767) m = 32767; else if (m < -32768) m = -32768;
        out[2 * i] = (int16_t)m; out[2 * i + 1] = (int16_t)m;
      }
      size_t wrote;
      i2s_write(I2S_NUM_0, out, sizeof(out), &wrote, portMAX_DELAY); // blocks only when DMA full -> paces to RATE
      reapSlots();
      esp_task_wdt_reset();
    }
  }

  void loadConfig() {                            // /config.txt at SD root (optional)
    wavset::defaultConfig(cfg);
    File f = SD.open("/config.txt", FILE_READ);
    if (!f) return;
    char txt[512]; size_t n = f.read((uint8_t*)txt, sizeof(txt) - 1); txt[n] = 0; f.close();
    wavset::parseConfig(txt, cfg);
    // sndmode=hybrid => the FPGA's GOSOF80 synthesises part of the sound; the ESP plays only what
    // GOSOF80 can't (speech + complex 80B), per sndroute. Default (absent) = full = ESP plays all.
    g_hybrid = false;
    char* p = strstr(txt, "sndmode");
    if (p) { char* nl = strchr(p, '\n'); char sv = nl ? *nl : 0; if (nl) *nl = 0;
             g_hybrid = strstr(p, "hybrid") != nullptr; if (nl) *nl = sv; }
    log_i("[snd] config: mix=%d volv=%d vols=%d stheme=%s sndmode=%s",
          cfg.mix, cfg.volv, cfg.vols, cfg.stheme, g_hybrid ? "hybrid" : "full");
  }

  // /games.txt at SD root: lines "<No> <romname>" (# = comment). FPGA game-select No -> folder.
  // No = GottFA80_PLuS gamelist index (manual Appendix A), as sent on the link (0x40|No).
  void loadGames() {
    for (int i = 0; i < 64; i++) gameMap[i][0] = 0;
    File f = SD.open("/games.txt", FILE_READ);
    if (!f) { log_i("[snd] no /games.txt (FPGA game-select uses raw number)"); return; }
    char buf[2048]; size_t len = f.read((uint8_t*)buf, sizeof(buf) - 1); buf[len] = 0; f.close();
    int n = 0; char* save = nullptr;
    for (char* line = strtok_r(buf, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
      if (line[0] == '#' || line[0] == '\r' || !line[0]) continue;
      int no; char rom[24];
      if (sscanf(line, "%d %23s", &no, rom) == 2 && no >= 0 && no < 64) {
        strncpy(gameMap[no], rom, 23); gameMap[no][23] = 0; n++; }
    }
    log_i("[snd] games.txt: %d game mappings", n);
  }
}

namespace wavplayer {

bool begin() {
  reqQ = xQueueCreate(8, sizeof(Req));
  if (!reqQ) return false;

  sdspi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  // 4 MHz (was 20): flying-wire SD wiring drops out during write bursts at 20 MHz
  // (the card unmounts -> "File system is not mounted"). 4 MHz is far more tolerant
  // of long leads / weak 3.3V; raise again once the SD is on a clean, decoupled board.
  if (!SD.begin(PIN_SD_CS, sdspi, 4000000)) { log_e("[snd] SD mount failed"); return false; }

  // I2S TX -> PCM5102A (16-bit, DMA). Mono is sent on both L/R. SCK->GND on the module (no MCLK).
  i2s_config_t i2scfg = {};
  i2scfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  i2scfg.sample_rate = RATE;
  i2scfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2scfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2scfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2scfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2scfg.dma_buf_count = 8;
  i2scfg.dma_buf_len = 256;
  i2scfg.use_apll = true;            // cleaner audio clock
  i2scfg.tx_desc_auto_clear = true;  // output silence on underrun (no click/repeat)
  if (i2s_driver_install(I2S_NUM_0, &i2scfg, 0, nullptr) != ESP_OK) { log_e("[snd] I2S install failed"); return false; }
  i2s_pin_config_t i2spin = {};
  i2spin.mck_io_num   = I2S_PIN_NO_CHANGE;
  i2spin.bck_io_num   = PIN_I2S_BCK;
  i2spin.ws_io_num    = PIN_I2S_LRCK;
  i2spin.data_out_num = PIN_I2S_DOUT;
  i2spin.data_in_num  = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_0, &i2spin);

  mixer.reset();

  // cache the game folders on the SD root for the web UI (now, before the tasks touch SD)
  nThemes = 0;
  { File root = SD.open("/");
    if (root && root.isDirectory())
      for (File f = root.openNextFile(); f && nThemes < 24; f = root.openNextFile()) {
        if (f.isDirectory()) { const char* nm = f.name(); const char* b = strrchr(nm, '/'); b = b ? b + 1 : nm;
          if (b[0] != '.' && strcmp(b, "System Volume Information")) {
            strncpy(themes[nThemes], b, 23); themes[nThemes][23] = 0; nThemes++; } }
        f.close(); }
    if (root) root.close(); }

  loadConfig();
  loadGames();                                    // /games.txt FPGA game-select map
  loadTheme(cfg.stheme);                          // index + autoplay init sounds

  // I2S DMA clocks the output -> no core-0 busy-loop. One mix task (core 1); core 0 is free.
  xTaskCreatePinnedToCore(mixTask, "mix", 8192, nullptr, 3, nullptr, 1);

  g_ready = true;
  log_i("[snd] wavplayer ready (PCM5102A I2S, %d Hz, mono->L/R)", RATE);
  return true;
}

void setTheme(const char* t) {
  if (!g_ready || !reqQ) return;
  Req r; r.type = 1; r.sound = 0;
  strncpy(r.theme, t, sizeof(r.theme) - 1); r.theme[sizeof(r.theme) - 1] = 0;
  xQueueSend(reqQ, &r, 0);
}

bool play(int soundId) {
  if (!g_ready || !reqQ) return false;
  g_lastSound = soundId;
  Req r; r.type = 0; r.sound = soundId; r.theme[0] = 0;
  bool ok = xQueueSend(reqQ, &r, 0) == pdTRUE;
  if (!ok) {
    static uint32_t lastW = 0; uint32_t now = millis();
    if (now - lastW > 1000) { lastW = now; log_w("[snd] queue full, dropped sound %d", soundId); }
  }
  return ok;
}

void stopAll() {
  if (!g_ready || !reqQ) return;
  Req r; r.type = 2; r.sound = 0; r.theme[0] = 0;
  xQueueSend(reqQ, &r, 0);                         // mix task does mixer.stopAll()
}
bool ready()   { return g_ready; }

// --- cached status for the web UI (benign cross-task reads, display only) ---
const char* curTheme()        { return theme; }
uint32_t    soundMask()       { return sndMask; }
int soundList(uint16_t* out, int max) {           // present sounds (incl. banked 32..95) for the web UI
  int n = 0;
  for (int i = 0; i < sndset.nEntry && n < max; i++) {
    const wavset::Entry& e = sndset.entry[i];
    if (e.id < 0 || e.id > 95) continue;
    uint8_t f = 0;
    if (e.attr & wavset::A_LOOP)  f |= 1;
    if (e.attr & wavset::A_VOICE) f |= 2;
    out[n++] = (uint16_t)((e.id << 2) | f);
  }
  return n;
}
uint32_t    loopMask()        { return loopM; }
uint32_t    voiceMask()       { return voiceM; }
int         soundCount()      { return nSnd; }
int         themeCount()      { return nThemes; }
const char* themeName(int i)  { return (i >= 0 && i < nThemes) ? themes[i] : ""; }
const char* gameRom(int no)   { return (no >= 0 && no < 64) ? gameMap[no] : ""; }
int         lastSound()       { return g_lastSound; }
void selectGame(int no) {                         // FPGA game No -> load that game's set
  if (no < 0 || no >= 64 || !gameMap[no][0]) { log_w("[snd] game No %d not in games.txt", no); return; }
  curGameNo = no;                                 // remember for hybrid routing (playLive)
  setTheme(gameMap[no]);
}

// FPGA live sound path: in hybrid mode, skip the commands GOSOF80 already synthesises (per sndroute);
// in full mode, play everything. play() stays unconditional so the web/diag sound test can play any id.
bool playLive(int soundId) {
  // Gottlieb System 80B command semantics (verified by ROM 6502 disasm + PinMAME renders),
  // auto-enabled ONLY when the loaded set has banked sounds (id>=32) so 80/80A & non-banking
  // sets are unaffected. The 5-bit cmd space is extended by PREFIX commands: cmd 30 arms
  // bank 1 (next cmd plays id N+32), cmd 29 arms bank 2 (N+64); cmd 31 (0x1F) = native STOP.
  // 29/30 are silent prefixes on the real board, so they trigger nothing themselves.
  if (g_hasBanks) {
    static uint8_t pendBank = 0;
    if (soundId == 31) { pendBank = 0; stopAll(); return true; }   // 0x1F native stop
    if (soundId == 30) { pendBank = 32; return true; }             // bank 1 prefix
    if (soundId == 29) { pendBank = 64; return true; }             // bank 2 prefix
    soundId += pendBank; pendBank = 0;                             // apply armed bank to this cmd
  }
  if (g_hybrid && !sndroute::espPlays(curGameNo, soundId)) return false;  // GOSOF80 handles it
  return play(soundId);
}
bool soundHybrid() { return g_hybrid; }

} // namespace wavplayer
#endif // !BOARD_C3
