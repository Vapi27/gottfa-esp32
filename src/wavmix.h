// wavmix.h — polyphonic 16-bit PCM mixer for the GottFA80+ sound engine.
//
// Original implementation for the Pstore Pinball Platform — NOT derived from any
// other player (we deliberately recode the GOSOWAV/pwavplayer concept from scratch
// for licensing freedom). Platform-agnostic: no Arduino/ESP dependencies, so the
// mixing logic is unit-testable on a host (see tools/host_wav_test.cpp).
//
// (C) 2026 Valere Pilpil / Pstore.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace wavmix {

constexpr int MAX_VOICES = 8;          // simultaneous sounds (music + effects + speech)
constexpr int BLOCK      = 256;        // samples processed per inner mix block

// A voice pulls up to n mono 16-bit samples into dst; returns the count actually
// produced. A short/zero return means the source is exhausted and the voice frees.
typedef size_t (*FillFn)(void* ctx, int16_t* dst, size_t n);

class Mixer {
public:
  void reset();
  // start a source on a free voice; returns the voice id, or -1 if all are busy.
  int  trigger(FillFn fill, void* ctx, uint8_t gain = 255);
  void stop(int id);
  void stopAll();
  int  activeCount() const;
  // mix n mono samples into out[], summing active voices with saturation.
  void mix(int16_t* out, size_t n);
private:
  struct Voice { FillFn fill; void* ctx; uint8_t gain; bool active; };
  Voice   v_[MAX_VOICES] = {};
  int16_t tmp_[BLOCK];                  // per-voice scratch
};

} // namespace wavmix
