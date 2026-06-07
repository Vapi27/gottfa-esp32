// wavmix.cpp — see wavmix.h. (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "wavmix.h"

namespace wavmix {

// 256 / sqrt(n) for n = 0..8 (loudness-preserving mix when MIX_SQRT and >1 voice)
static const uint16_t INV_SQRT[9] = { 256, 256, 181, 148, 128, 114, 105, 97, 90 };

void Mixer::reset() { for (auto& vo : v_) vo = Voice{}; }

int Mixer::trigger(const VoiceCfg& c) {
  for (int i = 0; i < MAX_VOICES; i++) {
    if (!v_[i].active) {
      v_[i] = Voice{ c.fill, c.ctx, c.rewind, c.gain, c.tag, c.loop, c.bg, c.voice, true };
      return i;
    }
  }
  return -1;                              // no free voice (caller may steal/ignore)
}

void Mixer::stop(int id)  { if (id >= 0 && id < MAX_VOICES) v_[id].active = false; }
void Mixer::stopAll()     { for (auto& vo : v_) vo.active = false; }
void Mixer::stopTag(int tag) { for (auto& vo : v_) if (vo.active && vo.tag == tag) vo.active = false; }
// mono background music: a new loop should replace the current one, not stack. Stops active
// looping voices that are NOT on the voice bus (speech). Oneshots (loop=false) are untouched.
void Mixer::stopActiveLoops() { for (auto& vo : v_) if (vo.active && vo.loop && !vo.voice) vo.active = false; }

void Mixer::stopExcept(bool keepBg, bool keepLoop, bool keepVoice) {
  for (auto& vo : v_) {
    if (!vo.active) continue;
    if (keepBg && vo.bg) continue;
    if (keepLoop && vo.loop) continue;
    if (keepVoice && vo.voice) continue;
    vo.active = false;
  }
}

bool Mixer::active(int id) const { return (id >= 0 && id < MAX_VOICES) && v_[id].active; }
int  Mixer::activeCount() const { int c = 0; for (auto& vo : v_) if (vo.active) c++; return c; }

void Mixer::mix(int16_t* out, size_t frames) {
  int32_t* acc = acc_;
  size_t done = 0;
  while (done < frames) {
    size_t blk = frames - done; if (blk > (size_t)BLOCK) blk = BLOCK;
    size_t ns = blk * 2;                              // interleaved samples this block
    for (size_t i = 0; i < ns; i++) acc[i] = 0;
    int nact = 0;
    for (auto& vo : v_) {
      if (!vo.active) continue;
      nact++;
      size_t got = vo.fill(vo.ctx, tmp_, blk);
      if (got < blk && vo.loop && vo.rewind && vo.rewind(vo.ctx))   // seamless loop
        got += vo.fill(vo.ctx, tmp_ + got * 2, blk - got);
      size_t gs = got * 2;
      for (size_t i = 0; i < gs; i++)
        acc[i] += (int32_t(tmp_[i]) * (vo.gain + 1)) >> 8;          // gain: 255=unity
      if (got < blk) vo.active = false;                            // ended
    }
    for (size_t i = 0; i < ns; i++) {
      int32_t s = acc[i];
      if (mix_ == MIX_DIV2) s >>= 1;
      else if (mix_ == MIX_SQRT && nact > 1) s = (s * INV_SQRT[nact > 8 ? 8 : nact]) >> 8;
      if (s >  32767) s =  32767;                      // saturate per channel
      if (s < -32768) s = -32768;
      out[done * 2 + i] = (int16_t)s;
    }
    done += blk;
  }
}

} // namespace wavmix
