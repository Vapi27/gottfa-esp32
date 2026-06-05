// wavsrc.cpp — see wavsrc.h. (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "wavsrc.h"

namespace wavsrc {

static constexpr size_t PEND_CAP = sizeof(((Source*)0)->pend) / sizeof(int16_t);

void init(Source& s, ReadFn read, void* ctx, uint8_t channels, uint32_t dataLen) {
  s.read = read; s.ctx = ctx;
  s.channels = (channels == 2) ? 2 : 1;
  s.remain = dataLen;
  s.have = 0; s.pos = 0;
  s.half = 0; s.haveHalf = false;
  s.eof = false;
}

// compact consumed samples, then append fresh data (bounded by the data chunk),
// carrying any odd byte across reads so samples never misalign.
static void topup(Source& s) {
  if (s.eof) return;
  if (s.pos > 0) {                              // compact leftover to the front
    size_t left = s.have - s.pos;
    for (size_t i = 0; i < left; i++) s.pend[i] = s.pend[s.pos + i];
    s.have = left; s.pos = 0;
  }
  size_t cap = PEND_CAP - s.have;               // room (in samples)
  if (cap == 0) return;
  static uint8_t raw[1024];                      // single audio task -> static keeps it off the stack
  size_t start = s.have;
  while (s.have == start) {                       // loop until >=1 new sample (handles odd/short reads)
    if (s.remain == 0 && !s.haveHalf) { s.eof = true; return; }  // data chunk exhausted
    size_t off = 0;
    if (s.haveHalf) { raw[0] = s.half; off = 1; s.haveHalf = false; }
    size_t want = cap * 2;
    if (want > sizeof(raw) - off) want = sizeof(raw) - off;
    if ((uint32_t)want > s.remain) want = (size_t)s.remain;     // never read past 'data'
    size_t got = (want > 0) ? s.read(s.ctx, raw + off, want) : 0;
    if (got == 0) { s.eof = true; return; }        // physical EOF or data done (drop any half)
    s.remain -= (uint32_t)got;
    size_t total = off + got;
    if (total & 1) { s.half = raw[total - 1]; s.haveHalf = true; }   // carry the odd byte
    size_t n = total / 2;                          // whole little-endian PCM16 samples
    for (size_t i = 0; i < n; i++)
      s.pend[s.have + i] = (int16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
    s.have += n;
  }
}

size_t fill(void* vp, int16_t* dst, size_t frames) {
  Source& s = *(Source*)vp;
  size_t out = 0;
  while (out < frames) {
    if (s.have - s.pos < (size_t)s.channels) {
      topup(s);
      if (s.have - s.pos < (size_t)s.channels) break;  // exhausted -> short return
    }
    if (s.channels == 1) {
      int16_t m = s.pend[s.pos++];
      dst[2 * out] = m; dst[2 * out + 1] = m;   // mono -> centre (L = R)
    } else {
      dst[2 * out]     = s.pend[s.pos++];        // L
      dst[2 * out + 1] = s.pend[s.pos++];        // R
    }
    out++;
  }
  return out;
}

} // namespace wavsrc
