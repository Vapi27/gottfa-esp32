// ym2151w.cpp — wrapper around ymfm's YM2151. See ym2151w.h. ymfm = BSD-3-Clause (Aaron Giles).
#include "ymfm_opm.h"
#include "ym2151w.h"

#if defined(ARDUINO) || defined(ESP_PLATFORM)
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #define YM_TASKED 1
#endif

namespace {
struct Intf : public ymfm::ymfm_interface {};   // pure synth, no timers/IRQ
Intf          s_intf;
ymfm::ym2151  s_chip(s_intf);

static inline int16_t gen1() {                   // generate one mono sample (touches s_chip)
  ymfm::ym2151::output_data o; s_chip.generate(&o, 1);
  int32_t m = (o.data[0] + o.data[1]) >> 1;
  if (m > 32767) m = 32767; else if (m < -32768) m = -32768;
  return (int16_t)m;
}

#ifdef YM_TASKED
// s_chip is owned by the core-0 task ONLY. Core 1 talks to it via these lock-free SPSC queues:
struct W { uint8_t off, data; };
volatile W        s_wq[256]; volatile uint32_t s_wqH = 0, s_wqT = 0;     // writes  : core1 -> task
volatile int16_t  s_sr[512]; volatile uint32_t s_srH = 0, s_srT = 0;     // samples : task  -> core1
volatile bool     s_resetReq = false;
bool              s_started = false;

void ymTask(void*) {
  for (;;) {
    if (s_resetReq) { s_chip.reset(); s_wqT = s_wqH; s_resetReq = false; }
    while (s_wqT != s_wqH) { uint8_t o = s_wq[s_wqT].off, d = s_wq[s_wqT].data; s_wqT = (s_wqT + 1) & 255; s_chip.write(o, d); }
    int guard = 0;                                                       // remplit le ring tant qu'il a de la place
    while (((s_srH + 1) & 511) != s_srT && guard++ < 384) { s_sr[s_srH] = gen1(); s_srH = (s_srH + 1) & 511; }
    vTaskDelay(1);                                                       // back-pressure du ring -> cadence ~55.9kHz
  }
}
#endif
}

namespace ym2151w {
uint32_t sampleRate() { return s_chip.sample_rate(3579545); }
#ifdef YM_TASKED
void begin() { if (!s_started) { s_started = true; xTaskCreatePinnedToCore(ymTask, "ym", 4096, nullptr, 1, nullptr, 0); } }
void reset() { s_resetReq = true; }
void write(uint8_t off, uint8_t d) { uint32_t h = s_wqH, n = (h + 1) & 255; if (n != s_wqT) { s_wq[h].off = off; s_wq[h].data = d; s_wqH = n; } }
int16_t nextSample() { uint32_t t = s_srT; if (t != s_srH) { int16_t v = s_sr[t]; s_srT = (t + 1) & 511; return v; } return 0; }
#else
void begin() {}
void reset() { s_chip.reset(); }
void write(uint8_t off, uint8_t d) { s_chip.write(off, d); }
int16_t nextSample() { return gen1(); }
#endif
}
