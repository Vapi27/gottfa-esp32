// wavplayer.cpp — see wavplayer.h. ESP32-S3 sound tier. Not built on C3.
// I2S via the legacy driver/i2s.h API (Arduino-ESP32 2.0.x / IDF 4.4).
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
#include "driver/i2s.h"
#include "esp_task_wdt.h"

namespace {
  constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
  constexpr int RATE   = AUDIO_RATE;
  constexpr int FRAMES = 256;                 // stereo frames per mix block

  wavmix::Mixer mixer;                         // owned solely by the audio task
  SPIClass      sdspi(HSPI);                   // dedicated SPI bus for the sound SD
  char          theme[24] = "orgsnd";          // owned solely by the audio task
  volatile bool g_ready = false;

  struct Slot { File f; wavsrc::Source src; int vid; bool used; };
  Slot slot[wavmix::MAX_VOICES];

  // one queued request: a sound to play, or a theme change (audio task owns 'theme')
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

  void audioTask(void*) {
    static int16_t buf[FRAMES * 2];
    esp_task_wdt_add(nullptr);                                       // feed the WDT from our progress
    Req req;
    size_t written;
    for (;;) {
      if (xQueueReceive(reqQ, &req, 0) == pdTRUE) handleReq(req);    // at most one SD op per pass
      mixer.mix(buf, FRAMES);
      i2s_write(I2S_PORT, buf, sizeof(buf), &written, portMAX_DELAY);
      esp_task_wdt_reset();
      for (int i = 0; i < wavmix::MAX_VOICES; i++)                    // reap finished voices
        if (slot[i].used && !mixer.active(slot[i].vid)) { slot[i].f.close(); slot[i].used = false; }
    }
  }

  bool i2sInit() {
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;   // stereo
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = 0;
    cfg.dma_buf_count        = 16;                           // 16 x 256 frames ~= 93 ms reserve
    cfg.dma_buf_len          = FRAMES;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;
    if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK) return false;
    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = PIN_I2S_BCK;
    pins.ws_io_num    = PIN_I2S_WS;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) return false;
    i2s_zero_dma_buffer(I2S_PORT);
    return true;
  }
}

namespace wavplayer {

bool begin() {
  reqQ = xQueueCreate(8, sizeof(Req));
  if (!reqQ) return false;
  sdspi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, sdspi, 20000000)) { log_e("[snd] SD mount failed"); return false; }  // 20 MHz
  if (!i2sInit())                            { log_e("[snd] I2S init failed");  return false; }
  mixer.reset();
  // audio task on core 0 (the Arduino loop / net run on core 1); 8 KB stack for the SD/FATFS depth
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 6, nullptr, 0);
  g_ready = true;
  log_i("[snd] wavplayer ready (%d Hz stereo)", RATE);
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
