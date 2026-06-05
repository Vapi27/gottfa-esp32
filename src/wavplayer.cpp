// wavplayer.cpp — see wavplayer.h. ESP32-S3 sound tier. Not built on C3.
//
// Output: MCP4921 12-bit SPI DAC (mono). The MCP4921 has no clock of its own and
// latches one 16-bit word per /CS frame, so samples must be clocked out at the audio
// rate. We dedicate core 0 to a cycle-paced "dac" task that pops a lock-free ring and
// writes one framed SPI word per sample (legal Arduino SPI — no ISR/flash hazard).
// The "mix" task (core 1) owns the mixer + SD, streams/mixes WAVs, down-mixes to mono
// 12-bit, and fills the ring in blocks. Ring is single-producer / single-consumer.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#ifndef BOARD_C3
#include "wavplayer.h"
#include "wavmix.h"
#include "wavsrc.h"
#include "wavfile.h"
#include "board_config.h"
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <string.h>
#include <atomic>
#include "esp_task_wdt.h"

namespace {
  constexpr int RATE   = AUDIO_RATE;
  constexpr int FRAMES = 256;                  // stereo frames mixed per block

  // MCP4921 command nibble (bits 15..12): DAC-A, unbuffered Vref, 1x gain, active.
  // The low 12 bits carry the sample. Output = Vref * D / 4096.
  constexpr uint16_t DAC_CFG = 0x3000;
  constexpr uint16_t DAC_MID = 0x0800;         // mid-scale = silence (no DC step)

  // Lock-free SPSC ring of mono 12-bit samples (power-of-two size). ~93 ms @ 44.1 kHz
  // absorbs SD-read latency spikes, mirroring the old 16 x 256-frame I2S DMA reserve.
  constexpr uint32_t RING = 4096;
  uint16_t              ringBuf[RING];
  std::atomic<uint32_t> rHead{0};              // producer: mix task
  std::atomic<uint32_t> rTail{0};              // consumer: dac task

  wavmix::Mixer mixer;                          // owned solely by the mix task
  SPIClass      sdspi(HSPI);                    // dedicated SPI bus for the sound SD
  SPIClass      dacspi(FSPI);                   // dedicated SPI bus for the MCP4921
  char          theme[24] = "orgsnd";          // owned solely by the mix task
  volatile bool g_ready = false;

  struct Slot { File f; wavsrc::Source src; int vid; bool used; };
  Slot slot[wavmix::MAX_VOICES];

  // one queued request: a sound to play, or a theme change (mix task owns 'theme')
  struct Req { uint8_t type; int sound; char theme[24]; };   // type 0=play, 1=set-theme
  QueueHandle_t reqQ = nullptr;

  size_t file_read(void* ctx, uint8_t* d, size_t n) { return ((File*)ctx)->read(d, n); }

  // Walk the RIFF chunks in an open file to find fmt (format/channels/bits) + data
  // (offset/len). Robust to metadata chunks before 'data'. Leaves f positioned at data.
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
        uint32_t extra = (len > 16 ? len - 16 : 0) + (len & 1);     // ext bytes + word-align
        if (extra) f.seek(f.position() + extra);
      } else if (!memcmp(b, "data", 4)) {
        wi.dataOffset = f.position();
        wi.dataLen    = len;
        wi.ok = (wi.format == 1 && (wi.channels == 1 || wi.channels == 2) && wi.bits == 16);
        return wi.ok;
      } else {
        f.seek(f.position() + len + (len & 1));                     // skip unknown chunk
      }
    }
    return false;
  }

  void startVoice(const char* path) {
    int si = -1;
    for (int i = 0; i < wavmix::MAX_VOICES; i++) if (!slot[i].used) { si = i; break; }
    if (si < 0) {
      static uint32_t lastW = 0; uint32_t now = millis();
      if (now - lastW > 1000) { lastW = now; log_w("[snd] all voices busy, dropped %s", path); }
      return;
    }
    File f = SD.open(path, FILE_READ);
    if (!f) { log_w("[snd] open fail %s", path); return; }
    WavInfo wi;
    if (!findWav(f, wi)) { log_w("[snd] not PCM16 %s", path); f.close(); return; }
    if (wi.dataOffset >= (uint32_t)f.size() || !f.seek(wi.dataOffset)) {
      log_w("[snd] bad data offset %s", path); f.close(); return;
    }
    uint32_t avail = (uint32_t)f.size() - wi.dataOffset;
    uint32_t dlen  = (wi.dataLen && wi.dataLen <= avail) ? wi.dataLen : avail;  // clamp to file
    slot[si].f = f;
    wavsrc::init(slot[si].src, file_read, &slot[si].f, (uint8_t)wi.channels, dlen);
    int vid = mixer.trigger(wavsrc::fill, &slot[si].src);
    if (vid < 0) { slot[si].f.close(); return; }
    slot[si].vid = vid; slot[si].used = true;
  }

  void handleReq(const Req& r) {
    if (r.type == 1) { strncpy(theme, r.theme, sizeof(theme) - 1); theme[sizeof(theme) - 1] = 0; }
    else { char p[64]; snprintf(p, sizeof(p), "/%s/%d.wav", theme, r.sound); startVoice(p); }
  }

  // --- core 1: mixer + SD owner; fills the ring in whole blocks ---------------
  void mixTask(void*) {
    static int16_t buf[FRAMES * 2];
    esp_task_wdt_add(nullptr);                                      // catch an SD hang
    Req req;
    for (;;) {
      if (xQueueReceive(reqQ, &req, 0) == pdTRUE) handleReq(req);   // at most one SD op per pass
      uint32_t h    = rHead.load(std::memory_order_relaxed);
      uint32_t t    = rTail.load(std::memory_order_acquire);
      uint32_t freeN = RING - 1 - (h - t);
      if (freeN >= (uint32_t)FRAMES) {
        mixer.mix(buf, FRAMES);                                     // interleaved L/R int16
        for (int i = 0; i < FRAMES; i++) {
          int32_t mono = ((int32_t)buf[2 * i] + buf[2 * i + 1]) >> 1;     // down-mix to mono
          ringBuf[(h + i) & (RING - 1)] = (uint16_t)((mono + 32768) >> 4); // -> 0..4095
        }
        rHead.store(h + FRAMES, std::memory_order_release);
      } else {
        vTaskDelay(1);                                              // ring full: let dac drain
      }
      for (int i = 0; i < wavmix::MAX_VOICES; i++)                  // reap finished voices
        if (slot[i].used && !mixer.active(slot[i].vid)) { slot[i].f.close(); slot[i].used = false; }
      esp_task_wdt_reset();
    }
  }

  // --- core 0: cycle-paced DAC clock; one framed SPI word per sample ----------
  void dacTask(void*) {
    const uint32_t cyc = getCpuFrequencyMhz() * 1000000UL / RATE;   // CPU cycles per sample
    uint32_t next = ESP.getCycleCount();
    for (;;) {
      next += cyc;
      while ((int32_t)(ESP.getCycleCount() - next) < 0) { }         // spin to the next tick
      uint32_t t = rTail.load(std::memory_order_relaxed);
      uint32_t h = rHead.load(std::memory_order_acquire);
      uint16_t s = DAC_MID;
      if (h != t) { s = ringBuf[t & (RING - 1)]; rTail.store(t + 1, std::memory_order_release); }
      dacspi.transfer16(DAC_CFG | (s & 0x0FFF));                    // /CS hardware-framed
    }
  }
}

namespace wavplayer {

bool begin() {
  reqQ = xQueueCreate(8, sizeof(Req));
  if (!reqQ) return false;

  sdspi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, sdspi, 20000000)) { log_e("[snd] SD mount failed"); return false; }  // 20 MHz

  // MCP4921 on a dedicated bus; hardware CS frames each 16-bit word. MCP4921 takes
  // up to 20 MHz SCK -> one sample ~0.8 us, far inside the 22.7 us sample period.
  dacspi.begin(PIN_DAC_SCK, -1, PIN_DAC_SDI, PIN_DAC_CS);
  dacspi.setHwCs(true);
  dacspi.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE0));

  mixer.reset();
  rHead.store(0); rTail.store(0);

  // core 0 is dedicated to clocking the DAC (busy-loop) -> drop its idle WDT watcher.
  disableCore0WDT();
  xTaskCreatePinnedToCore(dacTask, "dac", 2048, nullptr, configMAX_PRIORITIES - 1, nullptr, 0);
  // mixer + SD on core 1 (alongside the Arduino loop / net); 8 KB stack for FATFS depth
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

void stopAll() { /* voices end naturally; explicit stop handled in the audio domain */ }
bool ready()   { return g_ready; }

} // namespace wavplayer
#endif // !BOARD_C3
