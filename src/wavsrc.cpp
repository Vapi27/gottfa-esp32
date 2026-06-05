// wavsrc.cpp — see wavsrc.h. (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "wavsrc.h"

namespace wavsrc {

static constexpr size_t PEND_CAP = sizeof(((Source*)0)->pend) / sizeof(int16_t);

void init(Source& s, ReadFn read, void* ctx, uint8_t channels) {
  s.read = read; s.ctx = ctx;
  s.channels = (channels == 2) ? 2 : 1;
  s.have = 0; s.pos = 0; s.eof = false;
}

// compact consumed samples and append fresh data from the source
static void topup(Source& s) {
  if (s.eof) return;
  if (s.pos > 0) {                              // compact leftover to the front
    size_t left = s.have - s.pos;
    for (size_t i = 0; i < left; i++) s.pend[i] = s.pend[s.pos + i];
    s.have = left; s.pos = 0;
  }
  size_t cap = PEND_CAP - s.have;               // room (in samples)
  if (cap == 0) return;
  uint8_t raw[1024];
  size_t want = cap * 2; if (want > sizeof(raw)) want = sizeof(raw);
  size_t got = s.read(s.ctx, raw, want);
  if (got == 0) { s.eof = true; return; }
  size_t n = got / 2;                           // whole PCM16 samples (drop odd byte)
  for (size_t i = 0; i < n; i++)
    s.pend[s.have + i] = (int16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));  // little-endian
  s.have += n;
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
