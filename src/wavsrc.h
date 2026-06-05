// wavsrc.h — streaming WAV (PCM16) -> stereo-frames source for wavmix.
//
// Converts a sequential byte stream of PCM16 *sample data* (mono or stereo) into
// interleaved stereo frames on demand (the wavmix FillFn). Mono is up-mixed L=R;
// stereo passes through. The byte source is a read callback, so the decode/upmix/
// buffer logic is host-unit-testable (SD file on the ESP, memory on the host).
// The caller parses the header (wavfile) + seeks to the data, then feeds data bytes.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include "wavmix.h"
#include <stdint.h>
#include <stddef.h>

namespace wavsrc {

// read up to n bytes of PCM data into dst; return bytes read (0 = EOF / error).
typedef size_t (*ReadFn)(void* ctx, uint8_t* dst, size_t n);

struct Source {
  ReadFn   read;
  void*    ctx;
  uint8_t  channels;        // 1 = mono, 2 = stereo
  int16_t  pend[1024];      // decoded sample staging (in file-channel units)
  size_t   have, pos;       // samples valid / consumed
  bool     eof;
};

void init(Source& s, ReadFn read, void* ctx, uint8_t channels);

// wavmix-compatible FillFn (pass &Source as ctx): produces stereo frames.
size_t fill(void* src, int16_t* dst, size_t frames);

} // namespace wavsrc
