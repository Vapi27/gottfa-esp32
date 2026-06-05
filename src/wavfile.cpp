// wavfile.cpp — see wavfile.h. (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "wavfile.h"

static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static bool     tag (const uint8_t* p, const char* t) { return p[0]==t[0] && p[1]==t[1] && p[2]==t[2] && p[3]==t[3]; }

WavInfo wav_parse(const uint8_t* p, size_t n) {
  WavInfo w = {};
  if (n < 12 || !tag(p, "RIFF") || !tag(p + 8, "WAVE")) return w;
  size_t off = 12;
  while (off + 8 <= n) {
    const uint8_t* ck = p + off;
    uint32_t len = rd32(ck + 4);
    if (tag(ck, "fmt ") && off + 24 <= n) {
      w.format   = rd16(ck + 8);
      w.channels = rd16(ck + 10);
      w.rate     = rd32(ck + 12);
      w.bits     = rd16(ck + 22);
    } else if (tag(ck, "data")) {
      w.dataOffset = (uint32_t)(off + 8);
      w.dataLen    = len;
      w.ok = (w.format == 1 && w.channels >= 1 && w.bits == 16);  // prototype: PCM16
      return w;
    }
    off += 8 + len + (len & 1);          // chunks are word-aligned
  }
  return w;
}
