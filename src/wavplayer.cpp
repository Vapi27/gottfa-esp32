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
#include "driver/i2s.h"

namespace {
  constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
  constexpr int RATE   = AUDIO_RATE;
  constexpr int FRAMES = 256;                 // stereo frames per mix block (~5.8 ms @ 44.1k)

  wavmix::Mixer mixer;                         // owned solely by the audio task
  SPIClass      sdspi(HSPI);                   // dedicated SPI bus for the sound SD
  char          theme[24] = "orgsnd";
  volatile bool g_ready = false;

  struct Slot { File f; wavsrc::Source src; int vid; bool used; };
  Slot slot[wavmix::MAX_VOICES];

  struct PlayReq { char path[48]; };
  QueueHandle_t playQ = nullptr;

  size_t file_read(void* ctx, uint8_t* d, size_t n) { return ((File*)ctx)->read(d, n); }

  void startVoice(const char* path) {
    int si = -1;
    for (int i = 0; i < wavmix::MAX_VOICES; i++) if (!slot[i].used) { si = i; break; }
    if (si < 0) return;                                       // all voices busy -> drop
    File f = SD.open(path, FILE_READ);
    if (!f) { log_w("[snd] open fail %s", path); return; }
    uint8_t hdr[64];
    size_t hn = f.read(hdr, sizeof(hdr));
    WavInfo wi = wav_parse(hdr, hn);
    if (!wi.ok) { log_w("[snd] not PCM16 %s", path); f.close(); return; }
    f.seek(wi.dataOffset);
    slot[si].f = f;
    wavsrc::init(slot[si].src, file_read, &slot[si].f, (uint8_t)wi.channels);
    int vid = mixer.trigger(wavsrc::fill, &slot[si].src);
    if (vid < 0) { slot[si].f.close(); return; }
    slot[si].vid = vid; slot[si].used = true;
  }

  void audioTask(void*) {
    static int16_t buf[FRAMES * 2];
    PlayReq req;
    size_t written;
    for (;;) {
      while (xQueueReceive(playQ, &req, 0) == pdTRUE) startVoice(req.path);  // start new sounds
      mixer.mix(buf, FRAMES);                                                // mix stereo
      i2s_write(I2S_PORT, buf, sizeof(buf), &written, portMAX_DELAY);        // paces the loop (DMA)
      for (int i = 0; i < wavmix::MAX_VOICES; i++)                            // reap finished voices
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
    cfg.dma_buf_count        = 8;                            // 8 x 256 frames ~= 46 ms buffer
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
  playQ = xQueueCreate(8, sizeof(PlayReq));
  if (!playQ) return false;
  sdspi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, sdspi)) { log_e("[snd] SD mount failed"); return false; }
  if (!i2sInit())                  { log_e("[snd] I2S init failed");  return false; }
  mixer.reset();
  // audio task on core 0 (the Arduino loop / net run on core 1)
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 6, nullptr, 0);
  g_ready = true;
  log_i("[snd] wavplayer ready (%d Hz stereo)", RATE);
  return true;
}

void setTheme(const char* t) { strncpy(theme, t, sizeof(theme) - 1); theme[sizeof(theme) - 1] = 0; }

bool play(int soundId) {
  if (!g_ready) return false;
  PlayReq req;
  snprintf(req.path, sizeof(req.path), "/%s/%d.wav", theme, soundId);
  return playQ && xQueueSend(playQ, &req, 0) == pdTRUE;     // non-blocking enqueue
}

void stopAll() { /* voices end naturally; explicit stop handled in the audio domain */ }
bool ready()   { return g_ready; }

} // namespace wavplayer
#endif // !BOARD_C3
