// wavmix.cpp — see wavmix.h. (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "wavmix.h"

namespace wavmix {

void Mixer::reset() { for (auto& vo : v_) vo = Voice{}; }

int Mixer::trigger(FillFn fill, void* ctx, uint8_t gain) {
  for (int i = 0; i < MAX_VOICES; i++) {
    if (!v_[i].active) { v_[i] = Voice{fill, ctx, gain, true}; return i; }
  }
  return -1;                              // no free voice (caller may steal/ignore)
}

void Mixer::stop(int id)    { if (id >= 0 && id < MAX_VOICES) v_[id].active = false; }
void Mixer::stopAll()       { for (auto& vo : v_) vo.active = false; }
bool Mixer::active(int id) const { return (id >= 0 && id < MAX_VOICES) && v_[id].active; }
int  Mixer::activeCount() const { int c = 0; for (auto& vo : v_) if (vo.active) c++; return c; }

void Mixer::mix(int16_t* out, size_t frames) {
  int32_t acc[BLOCK * 2];                 // interleaved L,R accumulator
  size_t done = 0;
  while (done < frames) {
    size_t blk = frames - done; if (blk > (size_t)BLOCK) blk = BLOCK;
    size_t ns = blk * 2;                  // interleaved samples this block
    for (size_t i = 0; i < ns; i++) acc[i] = 0;
    for (auto& vo : v_) {
      if (!vo.active) continue;
      size_t got = vo.fill(vo.ctx, tmp_, blk);          // got frames
      size_t gs  = got * 2;
      for (size_t i = 0; i < gs; i++)
        acc[i] += (int32_t(tmp_[i]) * (vo.gain + 1)) >> 8;  // gain: 255=unity, 0=mute
      if (got < blk) vo.active = false;                 // source exhausted this block
    }
    for (size_t i = 0; i < ns; i++) {
      int32_t s = acc[i];
      if (s >  32767) s =  32767;                        // saturate per channel
      if (s < -32768) s = -32768;
      out[done * 2 + i] = (int16_t)s;
    }
    done += blk;
  }
}

} // namespace wavmix
