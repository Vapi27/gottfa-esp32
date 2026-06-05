// wavmix.h — polyphonic STEREO 16-bit PCM mixer for the GottFA80+ sound engine.
//
// Original implementation for the Pstore Pinball Platform — NOT derived from any other
// player (we recode the GOSOWAV/pwavplayer concept from scratch for licensing freedom).
// Platform-agnostic: no Arduino/ESP deps, so the mixing logic is host-unit-testable
// (tools/host_wav_test.cpp).
//
// Voices carry the pwavplayer-style attributes so the player can implement loop,
// break (stop same id), kill / soft-kill / quit groups, and a voice vs sound bus.
// Looping is seamless: on source exhaustion the mixer calls the voice's rewind() and
// keeps filling the same block.
// (C) 2026 Valere Pilpil / Pstore.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace wavmix {

constexpr int MAX_VOICES = 8;          // simultaneous sounds (streaming from SD)
constexpr int BLOCK      = 128;        // stereo frames per inner mix block

enum MixMode : uint8_t { MIX_SUM = 0, MIX_DIV2 = 1, MIX_SQRT = 2 };

// A voice fills up to `frames` INTERLEAVED stereo frames into dst (2*frames int16:
// L,R,L,R,...); returns the frame count produced. A short/zero return means the
// source is exhausted (the mixer then loops via rewind, or frees the voice).
typedef size_t (*FillFn)(void* ctx, int16_t* dst, size_t frames);
// Re-init the source to its start for looping; return false if it cannot (ends voice).
typedef bool   (*RewindFn)(void* ctx);

struct VoiceCfg {
  FillFn   fill   = nullptr;
  void*    ctx    = nullptr;
  uint8_t  gain   = 255;     // 0..255 (255 = unity, 0 = mute)
  int      tag    = -1;      // sound id (for break / stopTag)
  bool     loop   = false;   // re-fill via rewind on exhaustion
  bool     bg     = false;   // background/init (survives soft-kill)
  bool     voice  = false;   // voice bus (survives quit)
  RewindFn rewind = nullptr; // required if loop
};

class Mixer {
public:
  void reset();
  int  trigger(const VoiceCfg& c);     // returns voice id, or -1 if all busy
  void stop(int id);
  void stopAll();
  void stopTag(int tag);               // break: stop voices playing this sound id
  void stopExcept(bool keepBg, bool keepLoop, bool keepVoice);   // kill/soft-kill/quit
  bool active(int id) const;
  int  activeCount() const;
  void setMix(uint8_t mode) { mix_ = mode; }
  void mix(int16_t* out, size_t frames);   // mix `frames` stereo frames, per mix mode
private:
  struct Voice { FillFn fill; void* ctx; RewindFn rewind; uint8_t gain;
                 int tag; bool loop, bg, voice, active; };
  Voice   v_[MAX_VOICES] = {};
  int16_t tmp_[BLOCK * 2];             // per-voice stereo scratch
  int32_t acc_[BLOCK * 2];             // mix accumulator (off the audio-task stack)
  uint8_t mix_ = MIX_DIV2;
};

} // namespace wavmix
