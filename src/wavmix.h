// wavmix.h — polyphonic STEREO 16-bit PCM mixer for the GottFA80+ sound engine.
//
// Original implementation for the Pstore Pinball Platform — NOT derived from any
// other player (we recode the GOSOWAV/pwavplayer concept from scratch for licensing
// freedom). Platform-agnostic: no Arduino/ESP deps, so the mixing logic is host-
// unit-testable (tools/host_wav_test.cpp).
//
// Design choices for our use case (pinball):
//  - STEREO throughout (some System 80B games — e.g. Arena — are stereo). Mono
//    sources are duplicated to L/R by their fill adapter; stereo sources pass L/R.
//  - fixed project sample rate (e.g. 44.1 kHz) — WAV sample sets are pre-converted
//    so there is NO runtime resampling (cheap on the ESP).
//  - a few simultaneous voices (music + effects + speech); 8 is ample.
//
// (C) 2026 Valere Pilpil / Pstore.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace wavmix {

constexpr int MAX_VOICES = 8;          // simultaneous sounds
constexpr int BLOCK      = 128;        // stereo frames per inner mix block

// A voice fills up to `frames` INTERLEAVED stereo frames into dst
// (2*frames int16: L,R,L,R,...); returns the frame count produced. A short/zero
// return means the source is exhausted and the voice frees itself.
typedef size_t (*FillFn)(void* ctx, int16_t* dst, size_t frames);

class Mixer {
public:
  void reset();
  // start a source on a free voice; returns the voice id, or -1 if all busy.
  int  trigger(FillFn fill, void* ctx, uint8_t gain = 255);
  void stop(int id);
  void stopAll();
  bool active(int id) const;           // is voice id still playing?
  int  activeCount() const;
  // mix `frames` stereo frames into out[] (2*frames interleaved samples), saturating.
  void mix(int16_t* out, size_t frames);
private:
  struct Voice { FillFn fill; void* ctx; uint8_t gain; bool active; };
  Voice   v_[MAX_VOICES] = {};
  int16_t tmp_[BLOCK * 2];             // per-voice stereo scratch (L,R interleaved)
  int32_t acc_[BLOCK * 2];             // mix accumulator (member -> kept off the audio-task stack)
};

} // namespace wavmix
