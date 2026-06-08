// ym2151w.cpp — wrapper around ymfm's YM2151. See ym2151w.h. ymfm = BSD-3-Clause (Aaron Giles).
#include "ymfm_opm.h"
#include "ym2151w.h"

namespace {
// Minimal interface: pure FM synthesis, no timers/IRQ/busy needed for playback.
struct Intf : public ymfm::ymfm_interface {};
Intf          s_intf;
ymfm::ym2151  s_chip(s_intf);
}

namespace ym2151w {
void reset() { s_chip.reset(); }
void write(uint8_t off, uint8_t d) { s_chip.write(off, d); }
uint32_t sampleRate() { return s_chip.sample_rate(3579545); }
void generate(int16_t* out, int n) {
  ymfm::ym2151::output_data o;
  for (int i = 0; i < n; i++) {
    s_chip.generate(&o, 1);
    int32_t m = (o.data[0] + o.data[1]) >> 1;          // stereo -> mono
    if (m > 32767) m = 32767; else if (m < -32768) m = -32768;
    out[i] = (int16_t)m;
  }
}
}
