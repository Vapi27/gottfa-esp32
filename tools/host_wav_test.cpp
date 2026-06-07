// host_wav_test.cpp — host unit test for wavmix + wavfile + wavsrc + wavset. No HW.
// Build & run on the container:
//   g++ -std=c++17 -Isrc tools/host_wav_test.cpp src/wavmix.cpp src/wavfile.cpp \
//       src/wavsrc.cpp src/wavset.cpp -o /tmp/wt && /tmp/wt
#include "wavmix.h"
#include "wavfile.h"
#include "wavsrc.h"
#include "wavset.h"
#include <cstdio>
#include <cstring>

// synthetic stereo source: emit (l,r) for `left` frames, then EOF (short return)
struct Tone { int16_t l, r; size_t left; };
static size_t fill_tone(void* c, int16_t* d, size_t frames) {
  Tone* s = (Tone*)c; size_t k = frames < s->left ? frames : s->left;
  for (size_t i = 0; i < k; i++) { d[2*i] = s->l; d[2*i+1] = s->r; } s->left -= k; return k;
}
// endless constant source (always fills the whole block; never frees)
static size_t fill_const(void* c, int16_t* d, size_t frames) {
  int16_t v = *(int16_t*)c; for (size_t i = 0; i < frames; i++) { d[2*i] = v; d[2*i+1] = v; } return frames;
}
// short looping source: emits `left` frames of v, rewind() resets left to period
struct Loop { int16_t v; size_t left, period; };
static size_t fill_loop(void* c, int16_t* d, size_t frames) {
  Loop* s = (Loop*)c; size_t k = frames < s->left ? frames : s->left;
  for (size_t i = 0; i < k; i++) { d[2*i] = s->v; d[2*i+1] = s->v; } s->left -= k; return k;
}
static bool rewind_loop(void* c) { Loop* s = (Loop*)c; s->left = s->period; return true; }

struct MemRd { const uint8_t* p; size_t len, pos; };
static size_t mem_read(void* c, uint8_t* d, size_t n) {
  MemRd* m = (MemRd*)c; size_t k = n; if (k > m->len - m->pos) k = m->len - m->pos;
  for (size_t i = 0; i < k; i++) d[i] = m->p[m->pos + i]; m->pos += k; return k;
}
struct Drip { const uint8_t* p; size_t len, pos, chunk; };
static size_t drip_read(void* c, uint8_t* d, size_t n) {
  Drip* m = (Drip*)c; size_t k = n; if (k > m->chunk) k = m->chunk; if (k > m->len - m->pos) k = m->len - m->pos;
  for (size_t i = 0; i < k; i++) d[i] = m->p[m->pos + i]; m->pos += k; return k;
}

static int trig(wavmix::Mixer& m, wavmix::FillFn f, void* c, uint8_t g = 255) {
  wavmix::VoiceCfg v; v.fill = f; v.ctx = c; v.gain = g; return m.trigger(v);
}

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("PASS %s\n", m); } while (0)

int main() {
  using namespace wavmix;
  // 1) two voices sum (MIX_SUM), per channel
  Mixer m; m.reset(); m.setMix(MIX_SUM);
  Tone a{10000, 10000, 100}, b{9000, 9000, 100};
  trig(m, fill_tone, &a); trig(m, fill_tone, &b);
  CHECK(m.activeCount() == 2, "two voices active");
  int16_t out[64 * 2]; m.mix(out, 64);
  CHECK(out[0] == 19000 && out[1] == 19000, "L and R sum to 19000");

  // 2) STEREO independence
  Mixer ms; ms.reset(); ms.setMix(MIX_SUM);
  Tone s{8000, -3000, 50}; trig(ms, fill_tone, &s);
  int16_t os[8 * 2]; ms.mix(os, 8);
  CHECK(os[0] == 8000 && os[1] == -3000, "stereo L=8000 R=-3000 independent");

  // 3) saturation per channel
  Mixer m2; m2.reset(); m2.setMix(MIX_SUM);
  Tone c{30000, -30000, 10}, d{30000, -30000, 10};
  trig(m2, fill_tone, &c); trig(m2, fill_tone, &d);
  int16_t o2[8 * 2]; m2.mix(o2, 8);
  CHECK(o2[0] == 32767 && o2[1] == -32768, "L saturates +32767, R -32768");

  // 4) voice frees at EOF, then silence
  Mixer m3; m3.reset(); m3.setMix(MIX_SUM);
  Tone e{5000, 5000, 4}; trig(m3, fill_tone, &e);
  int16_t o3[16 * 2]; m3.mix(o3, 16);
  CHECK(m3.activeCount() == 0, "voice freed at EOF");
  CHECK(o3[0] == 5000 && o3[4 * 2] == 0, "samples then silence");

  // 5) per-voice gain (255 = unity, 127 -> half)
  Mixer m4; m4.reset(); m4.setMix(MIX_SUM);
  Tone f{1000, 1000, 8}; trig(m4, fill_tone, &f, 127);
  int16_t o4[8 * 2]; m4.mix(o4, 8);
  CHECK(o4[0] == 500 && o4[1] == 500, "gain 127 halves 1000 -> 500 both ch");

  // 6) MIX_DIV2 halves the bus (default mode)
  Mixer md; md.reset();  // default = MIX_DIV2
  int16_t cv = 8000; trig(md, fill_const, &cv);
  int16_t od[8 * 2]; md.mix(od, 8);
  CHECK(od[0] == 4000, "MIX_DIV2 halves 8000 -> 4000");

  // 7) seamless loop: rewind refills the same block; voice stays active
  Mixer ml; ml.reset(); ml.setMix(MIX_SUM);
  Loop lp{7, 5, 20};
  { VoiceCfg v; v.fill = fill_loop; v.ctx = &lp; v.loop = true; v.rewind = rewind_loop; ml.trigger(v); }
  int16_t ol[8 * 2]; ml.mix(ol, 8);
  CHECK(ol[0] == 7 && ol[7 * 2] == 7 && ml.activeCount() == 1, "loop rewinds mid-block, no gap");

  // 8) break: stopTag stops only voices with that sound id
  Mixer mt; mt.reset();
  int16_t v5 = 1, v6 = 1;
  { VoiceCfg v; v.fill = fill_const; v.ctx = &v5; v.tag = 5; mt.trigger(v); mt.trigger(v); }
  { VoiceCfg v; v.fill = fill_const; v.ctx = &v6; v.tag = 6; mt.trigger(v); }
  CHECK(mt.activeCount() == 3, "3 voices (two tag5, one tag6)");
  mt.stopTag(5);
  CHECK(mt.activeCount() == 1, "stopTag(5) leaves only tag6");

  // 9) kill / soft-kill / quit via stopExcept(keepBg, keepLoop, keepVoice)
  Mixer mk; mk.reset();
  int16_t one = 1;
  VoiceCfg A; A.fill = fill_const; A.ctx = &one; A.bg = true;
  VoiceCfg B; B.fill = fill_const; B.ctx = &one; B.loop = true; B.rewind = rewind_loop;
  VoiceCfg C; C.fill = fill_const; C.ctx = &one; C.voice = true;
  VoiceCfg D; D.fill = fill_const; D.ctx = &one;
  mk.trigger(A); mk.trigger(B); mk.trigger(C); mk.trigger(D);
  mk.stopExcept(true, true, true);                    // quit: keep bg+loop+voice, drop plain
  CHECK(mk.activeCount() == 3, "quit keeps bg+loop+voice (drops plain)");
  mk.stopExcept(true, false, false);                  // soft-kill: keep only bg
  CHECK(mk.activeCount() == 1, "soft-kill keeps only background");
  mk.stopAll();
  CHECK(mk.activeCount() == 0, "stopAll clears");

  // 10-12) WAV header parse
  uint8_t h[44] = {0};
  memcpy(h, "RIFF", 4); h[4] = 36; memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); h[16] = 16; h[20] = 1; h[22] = 1;
  h[24] = 0x22; h[25] = 0x56; h[34] = 16;
  memcpy(h + 36, "data", 4); h[40] = 0xE8; h[41] = 0x03;
  CHECK(wav_parse(h, 44).ok, "mono WAV header parsed");
  uint8_t h2[44]; memcpy(h2, h, 44); h2[22] = 2;
  CHECK(wav_parse(h2, 44).ok, "stereo WAV header parsed");
  uint8_t h6[44]; memcpy(h6, h, 44); h6[22] = 6;
  CHECK(!wav_parse(h6, 44).ok, "6-channel WAV rejected");

  // 13) wavsrc mono -> stereo upmix + EOF
  { uint8_t mono[] = {100,0, 200,0, 0x2C,0x01}; MemRd mr{mono, sizeof(mono), 0};
    wavsrc::Source ws; wavsrc::init(ws, mem_read, &mr, 1, sizeof(mono));
    int16_t fr[8 * 2]; size_t got = wavsrc::fill(&ws, fr, 8);
    CHECK(got == 3 && fr[0] == 100 && fr[1] == 100 && fr[4] == 300 && fr[5] == 300, "wavsrc mono -> stereo upmix"); }
  // 14) wavsrc stereo passthrough
  { uint8_t st[] = {0xE8,0x03, 0x18,0xFC}; MemRd mr{st, sizeof(st), 0};
    wavsrc::Source ws; wavsrc::init(ws, mem_read, &mr, 2, sizeof(st));
    int16_t fr[4 * 2]; size_t got = wavsrc::fill(&ws, fr, 4);
    CHECK(got == 1 && fr[0] == 1000 && fr[1] == -1000, "wavsrc stereo passthrough"); }
  // 15) dataLen enforced (trailing junk ignored)
  { uint8_t buf[] = {100,0, 50,0, 'L','I','S','T', 9,9,9,9}; MemRd mr{buf, sizeof(buf), 0};
    wavsrc::Source ws; wavsrc::init(ws, mem_read, &mr, 1, 4);
    int16_t fr[8 * 2]; size_t got = wavsrc::fill(&ws, fr, 8);
    CHECK(got == 2 && fr[0] == 100 && fr[2] == 50, "wavsrc stops at dataLen"); }
  // 16) odd/short reads realign
  { uint8_t mono[] = {100,0, 200,0, 0x2C,0x01}; Drip dr{mono, sizeof(mono), 0, 3};
    wavsrc::Source ws; wavsrc::init(ws, drip_read, &dr, 1, sizeof(mono));
    int16_t fr[8 * 2]; size_t got = wavsrc::fill(&ws, fr, 8);
    CHECK(got == 3 && fr[0] == 100 && fr[2] == 200 && fr[4] == 300, "wavsrc odd/short reads realign"); }

  // 17-20) wavset filename parsing
  { wavset::Entry e;
    CHECK(wavset::parseName("0005-lv-080-music.wav", e) && e.id == 5 &&
          (e.attr & wavset::A_LOOP) && (e.attr & wavset::A_VOICE) && e.vol == 80, "parseName attrs+vol");
    CHECK(wavset::parseName("12.wav", e) && e.id == 12 && e.attr == 0 && e.vol == 100, "parseName bare id");
    CHECK(wavset::parseName("7-070.wav", e) && e.id == 7 && e.vol == 70, "parseName short form NNNN-VVV");
    CHECK(!wavset::parseName("readme.wav", e), "parseName rejects non-numeric"); }

  // 21-22) wavset group parsing
  { wavset::Group g;
    CHECK(wavset::parseGroup("0009-m-1-2-3-rand.grp", g) && g.id == 9 && g.random && g.n == 3 &&
          g.member[0] == 1 && g.member[2] == 3, "parseGroup random m");
    CHECK(wavset::parseGroup("9-r-4-5.grp", g) && !g.random && g.n == 2 && g.member[1] == 5, "parseGroup sequential r"); }

  // 23) config.txt parsing
  { wavset::Config cf; wavset::parseConfig("mix=sqrt\nvolv=50\nvols=80\nstheme=747\n", cf);
    CHECK(cf.mix == 2 && cf.volv == 50 && cf.vols == 80 && !strcmp(cf.stheme, "747"), "parseConfig mix/volv/vols/stheme"); }

  // 24) Set: index + sequential group resolve + passthrough
  { wavset::Set st; st.reset();
    st.addName("3-080.wav"); st.addName("0010-l-100-bg.wav"); st.addName("5-r-10-3.grp");
    CHECK(st.nEntry == 2 && st.find(10) && (st.find(10)->attr & wavset::A_LOOP), "Set index + find");
    int p1 = st.pick(5, 0), p2 = st.pick(5, 0), p3 = st.pick(5, 0);
    CHECK(p1 == 10 && p2 == 3 && p3 == 10, "Set sequential group 10,3,10");
    CHECK(st.pick(3, 0) == 3, "Set non-group id passthrough"); }

  // 25) stopActiveLoops: a new loop replaces the current loop; oneshots & voice survive
  { Mixer ml; ml.reset();
    int16_t cv = 100;
    VoiceCfg lv; lv.fill = fill_const; lv.ctx = &cv; lv.loop = true;   int idL = ml.trigger(lv);
    VoiceCfg ov; ov.fill = fill_const; ov.ctx = &cv;                   int idO = ml.trigger(ov);
    VoiceCfg vv; vv.fill = fill_const; vv.ctx = &cv; vv.voice = true;  int idV = ml.trigger(vv);
    CHECK(ml.activeCount() == 3, "3 voices before stopActiveLoops");
    ml.stopActiveLoops();
    CHECK(!ml.active(idL), "stopActiveLoops stops the looping voice");
    CHECK(ml.active(idO) && ml.active(idV), "stopActiveLoops keeps oneshot + voice"); }

  printf(fails ? "\n=== %d FAIL ===\n" : "\n===== WAVMIX + WAVSET TESTS PASSED =====\n", fails);
  return fails ? 1 : 0;
}
