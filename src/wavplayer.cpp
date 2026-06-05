// wavplayer.cpp — see wavplayer.h. ESP32-S3 sound tier. Not built on C3.
//
// Output: MCP4921 12-bit SPI DAC (mono). A cycle-paced "dac" task (core 0) clocks one
// framed SPI word per sample from a lock-free SPSC ring that the "mix"+SD task (core 1)
// fills. The mix task owns the mixer + SD + the sound-set index (wavset): it scans the
// theme folder, resolves a sound id (incl. random/sequential groups) to a file, applies
// the pwavplayer-style attributes (loop / break / kill / soft-kill / quit / voice bus /
// per-sound volume), streams the WAV into a mixer voice, mixes, down-mixes to mono 12-bit.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#ifndef BOARD_C3
#include "wavplayer.h"
#include "wavmix.h"
#include "wavsrc.h"
#include "wavfile.h"
#include "wavset.h"
#include "board_config.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <string.h>
#include <atomic>
#include "esp_task_wdt.h"
#include "esp_random.h"

namespace {
  constexpr int RATE   = AUDIO_RATE;
  constexpr int FRAMES = 256;                  // stereo frames mixed per block

  constexpr uint16_t DAC_CFG = 0x3000;         // MCP4921: DAC-A, unbuffered, 1x, active
  constexpr uint16_t DAC_MID = 0x0800;         // mid-scale = silence

  constexpr uint32_t RING = 4096;              // mono 12-bit samples (~93 ms @ 44.1k)
  uint16_t              ringBuf[RING];
  std::atomic<uint32_t> rHead{0};              // producer: mix task
  std::atomic<uint32_t> rTail{0};              // consumer: dac task

  wavmix::Mixer  mixer;                         // mix task only
  wavset::Set    sndset;                        // mix task only (theme index)
  wavset::Config cfg;                           // global config.txt (loaded once)
  SPIClass       sdspi(HSPI);
  SPIClass       dacspi(FSPI);
  char           theme[24] = "orgsnd";          // mix task only
  volatile bool  g_ready = false;

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
    log_i("[snd] theme '%s': %d sounds, %d groups", theme, sndset.nEntry, sndset.nGroup);
    for (int i = 0; i < sndset.nEntry; i++)               // autoplay init/background sounds
      if (sndset.entry[i].attr & wavset::A_INIT) startVoice(&sndset.entry[i]);
  }

  void handleReq(const Req& r) {
    if (r.type == 1) { loadTheme(r.theme); return; }
    int id = sndset.pick(r.sound, esp_random());          // resolve random/sequential group
    const wavset::Entry* e = sndset.find(id);
    if (e) startVoice(e);
    else   log_w("[snd] no sound %d in theme '%s'", r.sound, theme);
  }

  // --- core 1: mixer + SD + set owner; fills the ring in whole blocks ---------
  void mixTask(void*) {
    static int16_t buf[FRAMES * 2];
    esp_task_wdt_add(nullptr);
    Req req;
    for (;;) {
      if (xQueueReceive(reqQ, &req, 0) == pdTRUE) handleReq(req);   // at most one set/play op per pass
      uint32_t h = rHead.load(std::memory_order_relaxed);
      uint32_t t = rTail.load(std::memory_order_acquire);
      if (RING - 1 - (h - t) >= (uint32_t)FRAMES) {
        mixer.mix(buf, FRAMES);
        for (int i = 0; i < FRAMES; i++) {
          int32_t mono = ((int32_t)buf[2 * i] + buf[2 * i + 1]) >> 1;
          ringBuf[(h + i) & (RING - 1)] = (uint16_t)((mono + 32768) >> 4);
        }
        rHead.store(h + FRAMES, std::memory_order_release);
      } else {
        vTaskDelay(1);
      }
      reapSlots();
      esp_task_wdt_reset();
    }
  }

  // --- core 0: cycle-paced DAC clock; one framed SPI word per sample ----------
  void dacTask(void*) {
    const uint32_t cyc = getCpuFrequencyMhz() * 1000000UL / RATE;
    uint32_t next = ESP.getCycleCount();
    for (;;) {
      next += cyc;
      while ((int32_t)(ESP.getCycleCount() - next) < 0) { }
      uint32_t t = rTail.load(std::memory_order_relaxed);
      uint32_t h = rHead.load(std::memory_order_acquire);
      uint16_t s = DAC_MID;
      if (h != t) { s = ringBuf[t & (RING - 1)]; rTail.store(t + 1, std::memory_order_release); }
      dacspi.transfer16(DAC_CFG | (s & 0x0FFF));
    }
  }

  void loadConfig() {                            // /config.txt at SD root (optional)
    wavset::defaultConfig(cfg);
    File f = SD.open("/config.txt", FILE_READ);
    if (!f) return;
    char txt[512]; size_t n = f.read((uint8_t*)txt, sizeof(txt) - 1); txt[n] = 0; f.close();
    wavset::parseConfig(txt, cfg);
    log_i("[snd] config: mix=%d volv=%d vols=%d stheme=%s", cfg.mix, cfg.volv, cfg.vols, cfg.stheme);
  }
}

namespace wavplayer {

bool begin() {
  reqQ = xQueueCreate(8, sizeof(Req));
  if (!reqQ) return false;

  sdspi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, sdspi, 20000000)) { log_e("[snd] SD mount failed"); return false; }

  dacspi.begin(PIN_DAC_SCK, -1, PIN_DAC_SDI, PIN_DAC_CS);
  dacspi.setHwCs(true);
  dacspi.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));

  mixer.reset();
  rHead.store(0); rTail.store(0);
  loadConfig();
  loadTheme(cfg.stheme);                          // index + autoplay init sounds

  disableCore0WDT();                              // core 0 is the DAC busy-loop
  xTaskCreatePinnedToCore(dacTask, "dac", 2048, nullptr, configMAX_PRIORITIES - 1, nullptr, 0);
  xTaskCreatePinnedToCore(mixTask, "mix", 8192, nullptr, 3, nullptr, 1);

  g_ready = true;
  log_i("[snd] wavplayer ready (MCP4921, %d Hz mono)", RATE);
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
  Req r; r.type = 0; r.sound = soundId; r.theme[0] = 0;
  bool ok = xQueueSend(reqQ, &r, 0) == pdTRUE;
  if (!ok) {
    static uint32_t lastW = 0; uint32_t now = millis();
    if (now - lastW > 1000) { lastW = now; log_w("[snd] queue full, dropped sound %d", soundId); }
  }
  return ok;
}

void stopAll() { /* voices end naturally; kill via a 'k'-tagged sound or theme change */ }
bool ready()   { return g_ready; }

} // namespace wavplayer
#endif // !BOARD_C3
