// arena_attract.cpp — see arena_attract.h.
#include <LittleFS.h>
#include "arena_attract.h"

namespace arenaattract {

static const char* PATH = "/arena_attract.bin";

static uint64_t* s_frames = nullptr;
static uint16_t  s_n      = 0;
static uint16_t  s_step   = 20;

bool     available() { return s_frames != nullptr && s_n > 0; }
uint16_t frames()    { return s_n; }
uint16_t stepMs()    { return s_step; }

uint64_t maskAt(uint32_t ms) {
  if (!available()) return 0;
  const uint32_t total = (uint32_t)s_n * s_step;
  return s_frames[(ms % total) / s_step];
}

void begin() {
  if (!LittleFS.exists(PATH)) {
    Serial.printf("[rom] no %s — attract falls back to the generic effect\n", PATH);
    return;
  }
  File f = LittleFS.open(PATH, "r");
  if (!f) return;

  uint16_t hdr[2];
  if (f.read((uint8_t*)hdr, 4) != 4) { f.close(); return; }
  s_step = hdr[0];
  const uint16_t n = hdr[1];
  // A truncated or mis-sized file must not be half-loaded: a partial capture
  // would play as a wall that freezes halfway through the sequence, which reads
  // as a firmware bug rather than as a bad file.
  if (!n || !s_step || f.size() != (size_t)4 + (size_t)n * 8) {
    Serial.printf("[rom] %s malformed (%u frames, %u bytes) — ignored\n",
                  PATH, n, (unsigned)f.size());
    f.close();
    return;
  }
  s_frames = (uint64_t*)malloc((size_t)n * 8);
  if (!s_frames) { Serial.println("[rom] out of memory"); f.close(); return; }
  if (f.read((uint8_t*)s_frames, (size_t)n * 8) != (int)((size_t)n * 8)) {
    free(s_frames);
    s_frames = nullptr;
    f.close();
    return;
  }
  f.close();
  s_n = n;
  Serial.printf("[rom] original attract loaded: %u frames x %u ms = %.1f s\n",
                s_n, s_step, s_n * s_step / 1000.0f);
}

}  // namespace arenaattract
