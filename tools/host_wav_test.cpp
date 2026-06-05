// host_wav_test.cpp — host unit test for wavmix + wavfile (no ESP/hardware needed).
// Build & run on the container:
//   g++ -std=c++17 -Isrc tools/host_wav_test.cpp src/wavmix.cpp src/wavfile.cpp -o /tmp/wt && /tmp/wt
#include "wavmix.h"
#include "wavfile.h"
#include <cstdio>
#include <cstring>

// synthetic source: emit `val` for `left` samples, then EOF (short return)
struct Const { int16_t val; size_t left; };
static size_t fill_const(void* c, int16_t* d, size_t n) {
  Const* s = (Const*)c; size_t k = n < s->left ? n : s->left;
  for (size_t i = 0; i < k; i++) d[i] = s->val; s->left -= k; return k;
}

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("PASS %s\n", m); } while (0)

int main() {
  // 1) two voices sum
  wavmix::Mixer m; m.reset();
  Const a{10000, 100}, b{9000, 100};
  m.trigger(fill_const, &a); m.trigger(fill_const, &b);
  CHECK(m.activeCount() == 2, "two voices active");
  int16_t out[64]; m.mix(out, 64);
  CHECK(out[0] == 19000, "mix 10000+9000 = 19000");

  // 2) saturation
  wavmix::Mixer m2; m2.reset();
  Const c{30000, 10}, d{30000, 10};
  m2.trigger(fill_const, &c); m2.trigger(fill_const, &d);
  int16_t o2[8]; m2.mix(o2, 8);
  CHECK(o2[0] == 32767, "mix saturates to +32767");

  // 3) voice frees at EOF, then silence
  wavmix::Mixer m3; m3.reset();
  Const e{5000, 4};
  m3.trigger(fill_const, &e);
  int16_t o3[16]; m3.mix(o3, 16);
  CHECK(m3.activeCount() == 0, "voice freed at EOF");
  CHECK(o3[0] == 5000 && o3[4] == 0, "samples then silence");

  // 4) per-voice gain (255 = unity, so 127 -> (127+1)/256 = half)
  wavmix::Mixer m4; m4.reset();
  Const f{1000, 8};
  m4.trigger(fill_const, &f, 127);       // half volume
  int16_t o4[8]; m4.mix(o4, 8);
  CHECK(o4[0] == 500, "gain 127 halves 1000 -> 500");

  // 5) WAV header parse (mono PCM16 22050 Hz, 1000 data bytes)
  uint8_t h[44] = {0};
  memcpy(h, "RIFF", 4);     h[4] = 36;
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); h[16] = 16;
  h[20] = 1;                 h[22] = 1;            // PCM, 1 channel
  h[24] = 0x22; h[25] = 0x56;                      // 22050 = 0x5622
  h[34] = 16;                                      // 16 bits
  memcpy(h + 36, "data", 4); h[40] = 0xE8; h[41] = 0x03;  // 1000
  WavInfo w = wav_parse(h, 44);
  CHECK(w.ok && w.rate == 22050 && w.bits == 16 && w.channels == 1
        && w.dataOffset == 44 && w.dataLen == 1000, "WAV header parsed");

  printf(fails ? "\n=== %d FAIL ===\n" : "\n===== WAVMIX TESTS PASSED =====\n", fails);
  return fails ? 1 : 0;
}
