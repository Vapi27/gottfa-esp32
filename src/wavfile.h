// wavfile.h — minimal RIFF/WAVE PCM header parser (host-testable, no deps).
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include <stdint.h>
#include <stddef.h>

struct WavInfo {
  bool     ok;          // true if a usable PCM16 stream was found
  uint16_t format;      // 1 = PCM
  uint16_t channels;    // 1 = mono, 2 = stereo
  uint32_t rate;        // sample rate (Hz)
  uint16_t bits;        // bits per sample
  uint32_t dataOffset;  // byte offset of the sample data
  uint32_t dataLen;     // bytes of sample data
};

// Parse a WAV header from the first `n` bytes (enough to cover the fmt + data
// chunk headers — 64 bytes is plenty for standard files).
WavInfo wav_parse(const uint8_t* p, size_t n);
