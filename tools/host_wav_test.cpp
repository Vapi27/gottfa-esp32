// host_wav_test.cpp — host unit test for wavmix (STEREO) + wavfile + wavsrc. No HW.
// Build & run on the container:
//   g++ -std=c++17 -Isrc tools/host_wav_test.cpp src/wavmix.cpp src/wavfile.cpp src/wavsrc.cpp -o /tmp/wt && /tmp/wt
#include "wavmix.h"
#include "wavfile.h"
#include "wavsrc.h"
#include <cstdio>
#include <cstring>

// synthetic stereo source: emit (l,r) for `left` frames, then EOF (short return)
struct Tone { int16_t l, r; size_t left; };
static size_t fill_tone(void* c, int16_t* d, size_t frames) {
  Tone* s = (Tone*)c; size_t k = frames < s->left ? frames : s->left;
  for (size_t i = 0; i < k; i++) { d[2*i] = s->l; d[2*i+1] = s->r; } s->left -= k; return k;
}

// memory byte reader for wavsrc (stands in for an SD file)
struct MemRd { const uint8_t* p; size_t len, pos; };
static size_t mem_read(void* c, uint8_t* d, size_t n) {
  MemRd* m = (MemRd*)c; size_t k = n; if (k > m->len - m->pos) k = m->len - m->pos;
  for (size_t i = 0; i < k; i++) d[i] = m->p[m->pos + i]; m->pos += k; return k;
}
// "drip" reader: returns at most `chunk` bytes per call (exercises odd/short reads)
struct Drip { const uint8_t* p; size_t len, pos, chunk; };
static size_t drip_read(void* c, uint8_t* d, size_t n) {
  Drip* m = (Drip*)c; size_t k = n; if (k > m->chunk) k = m->chunk; if (k > m->len - m->pos) k = m->len - m->pos;
  for (size_t i = 0; i < k; i++) d[i] = m->p[m->pos + i]; m->pos += k; return k;
}

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("PASS %s\n", m); } while (0)

int main() {
  // 1) two voices sum, per channel
  wavmix::Mixer m; m.reset();
  Tone a{10000, 10000, 100}, b{9000, 9000, 100};
  m.trigger(fill_tone, &a); m.trigger(fill_tone, &b);
  CHECK(m.activeCount() == 2, "two voices active");
  int16_t out[64 * 2]; m.mix(out, 64);
  CHECK(out[0] == 19000 && out[1] == 19000, "L and R sum to 19000");

  // 2) STEREO independence
  wavmix::Mixer ms; ms.reset();
  Tone s{8000, -3000, 50};
  ms.trigger(fill_tone, &s);
  int16_t os[8 * 2]; ms.mix(os, 8);
  CHECK(os[0] == 8000 && os[1] == -3000, "stereo L=8000 R=-3000 independent");

  // 3) saturation per channel
  wavmix::Mixer m2; m2.reset();
  Tone c{30000, -30000, 10}, d{30000, -30000, 10};
  m2.trigger(fill_tone, &c); m2.trigger(fill_tone, &d);
  int16_t o2[8 * 2]; m2.mix(o2, 8);
  CHECK(o2[0] == 32767 && o2[1] == -32768, "L saturates +32767, R -32768");

  // 4) voice frees at EOF, then silence
  wavmix::Mixer m3; m3.reset();
  Tone e{5000, 5000, 4};
  m3.trigger(fill_tone, &e);
  int16_t o3[16 * 2]; m3.mix(o3, 16);
  CHECK(m3.activeCount() == 0, "voice freed at EOF");
  CHECK(o3[0] == 5000 && o3[4 * 2] == 0, "samples then silence");

  // 5) per-voice gain (255 = unity, 127 -> half)
  wavmix::Mixer m4; m4.reset();
  Tone f{1000, 1000, 8};
  m4.trigger(fill_tone, &f, 127);
  int16_t o4[8 * 2]; m4.mix(o4, 8);
  CHECK(o4[0] == 500 && o4[1] == 500, "gain 127 halves 1000 -> 500 both ch");

  // 6/7) WAV header parse: mono ok, stereo ok, >2ch rejected
  uint8_t h[44] = {0};
  memcpy(h, "RIFF", 4);      h[4] = 36;
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); h[16] = 16;
  h[20] = 1;                 h[22] = 1;            // PCM, 1 channel
  h[24] = 0x22; h[25] = 0x56;                      // 22050
  h[34] = 16;                                      // 16 bits
  memcpy(h + 36, "data", 4); h[40] = 0xE8; h[41] = 0x03;  // 1000
  CHECK(wav_parse(h, 44).ok, "mono WAV header parsed");
  uint8_t h2[44]; memcpy(h2, h, 44); h2[22] = 2;
  CHECK(wav_parse(h2, 44).ok, "stereo WAV header parsed");
  uint8_t h6[44]; memcpy(h6, h, 44); h6[22] = 6;   // 5.1
  CHECK(!wav_parse(h6, 44).ok, "6-channel WAV rejected");

  // 8) wavsrc mono -> stereo upmix + EOF (samples 100,200,300)
  {
    uint8_t mono[] = {100,0, 200,0, 0x2C,0x01};
    MemRd mr{mono, sizeof(mono), 0};
    wavsrc::Source ws; wavsrc::init(ws, mem_read, &mr, 1, sizeof(mono));
    int16_t fr[8 * 2];
    size_t got = wavsrc::fill(&ws, fr, 8);
    CHECK(got == 3 && fr[0] == 100 && fr[1] == 100 && fr[4] == 300 && fr[5] == 300,
          "wavsrc mono -> stereo upmix + EOF");
  }
  // 9) wavsrc stereo passthrough (L=1000, R=-1000)
  {
    uint8_t st[] = {0xE8,0x03, 0x18,0xFC};
    MemRd mr{st, sizeof(st), 0};
    wavsrc::Source ws; wavsrc::init(ws, mem_read, &mr, 2, sizeof(st));
    int16_t fr[4 * 2];
    size_t got = wavsrc::fill(&ws, fr, 4);
    CHECK(got == 1 && fr[0] == 1000 && fr[1] == -1000, "wavsrc stereo passthrough");
  }
  // 10) wavsrc drives a mixer voice end-to-end (mono 10,20,30,40)
  {
    uint8_t mono[] = {10,0, 20,0, 30,0, 40,0};
    MemRd mr{mono, sizeof(mono), 0};
    wavsrc::Source ws; wavsrc::init(ws, mem_read, &mr, 1, sizeof(mono));
    wavmix::Mixer mx; mx.reset();
    mx.trigger(wavsrc::fill, &ws);
    int16_t o[8 * 2]; mx.mix(o, 8);
    CHECK(o[0] == 10 && o[6] == 40 && o[8] == 0 && mx.activeCount() == 0,
          "wavsrc -> mixer voice, plays then frees");
  }
  // 11) dataLen ENFORCED: 2 mono samples then trailing junk; only 2 frames decoded
  {
    uint8_t buf[] = {100,0, 50,0,   'L','I','S','T', 9,9,9,9};  // 4 audio bytes + junk
    MemRd mr{buf, sizeof(buf), 0};
    wavsrc::Source ws; wavsrc::init(ws, mem_read, &mr, 1, 4);    // dataLen = 4 (2 samples)
    int16_t fr[8 * 2];
    size_t got = wavsrc::fill(&ws, fr, 8);
    CHECK(got == 2 && fr[0] == 100 && fr[2] == 50, "wavsrc stops at dataLen (no trailing junk)");
  }
  // 12) ODD/short reads (drip): samples reassemble with no misalignment
  {
    uint8_t mono[] = {100,0, 200,0, 0x2C,0x01};                  // 100,200,300
    Drip dr{mono, sizeof(mono), 0, 3};                           // 3 bytes/read (odd)
    wavsrc::Source ws; wavsrc::init(ws, drip_read, &dr, 1, sizeof(mono));
    int16_t fr[8 * 2];
    size_t got = wavsrc::fill(&ws, fr, 8);
    CHECK(got == 3 && fr[0] == 100 && fr[2] == 200 && fr[4] == 300, "wavsrc odd/short reads realign");
  }

  printf(fails ? "\n=== %d FAIL ===\n" : "\n===== WAVMIX STEREO TESTS PASSED =====\n", fails);
  return fails ? 1 : 0;
}
