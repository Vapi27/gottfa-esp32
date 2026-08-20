// sndmap.cpp — see sndmap.h. Pure C++ (host-testable, tools/test_sndmap.cpp).
// (C) 2026 Valere Pillet / Pstore. Original implementation.
#include "sndmap.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

namespace sndmap {

namespace {

inline void bitSet(uint32_t* w, int i) { if (i >= 0 && i < MAX_ID) w[i >> 5] |= (1u << (i & 31)); }

// "1,2,5-7" -> calls set(v) for every listed value. Tolerates spaces and a trailing comma.
// An EMPTY value list is meaningful: `ignore=` clears the default "ignore command 0".
template <typename F>
void eachInList(const char* v, F set) {
  while (*v) {
    while (*v == ' ' || *v == '\t' || *v == ',') v++;
    if (!isdigit((unsigned char)*v)) { if (!*v) break; v++; continue; }
    int a = 0; while (isdigit((unsigned char)*v)) a = a * 10 + (*v++ - '0');
    int b = a;
    if (*v == '-' && isdigit((unsigned char)v[1])) {
      v++; b = 0; while (isdigit((unsigned char)*v)) b = b * 10 + (*v++ - '0');
    }
    if (b < a) { int t = a; a = b; b = t; }
    for (int i = a; i <= b; i++) set(i);
  }
}

// "30:32" -> a=30 b=32. Accepts ':' or '-' or '>' as the separator ("30->32" reads well).
bool pair2(const char* v, int& a, int& b) {
  while (*v == ' ') v++;
  if (!isdigit((unsigned char)*v)) return false;
  a = 0; while (isdigit((unsigned char)*v)) a = a * 10 + (*v++ - '0');
  while (*v == ' ') v++;
  if (*v == ':' || *v == '-' || *v == '=') v++;
  if (*v == '>') v++;
  while (*v == ' ') v++;
  if (!isdigit((unsigned char)*v)) return false;
  b = 0; while (isdigit((unsigned char)*v)) b = b * 10 + (*v++ - '0');
  return true;
}

void trim(char* s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
  size_t i = 0; while (s[i] == ' ' || s[i] == '\t') i++;
  if (i) memmove(s, s + i, n - i + 1);
}

Gen parseGen(const char* v) {
  if (!strcasecmp(v, "80"))    return GEN_80;
  if (!strcasecmp(v, "80a"))   return GEN_80A;
  if (!strcasecmp(v, "80b")  || !strcasecmp(v, "80b1")) return GEN_80B1;
  if (!strcasecmp(v, "80b2"))  return GEN_80B2;
  if (!strcasecmp(v, "80b3"))  return GEN_80B3;
  return GEN_UNKNOWN;
}

// The ONLY thing a generation decides on its own: whether releasing the bus stops the sound.
// System 80/80A present a level to a tone generator (PinMAME gts80.c sends the command on
// every port-A write, zeroed when the strobe bit is clear), so a release is a real "silence".
// 80B latches on the strobe and ignores the release entirely. Bank/stop commands are NOT
// implied by the generation — they are a property of the title's sound ROM.
void applyGenDefaults(Map& m) {
  m.rel = (m.gen == GEN_80 || m.gen == GEN_80A) ? REL_STOP : REL_IGNORE;
}

} // namespace

void defaults(Map& m) {
  memset(&m, 0, sizeof(m));
  m.loaded     = false;
  m.gen        = GEN_UNKNOWN;
  m.ignoreMask = 1u;          // command 0 = the bus-release artefact, never a sample
  m.hdrMs      = 250;
  m.rel        = REL_IGNORE;
  m.repeatMs   = 0;           // by default an identical command always retriggers
}

void legacy80b(Map& m) {
  m.gen = GEN_80B2;
  m.hdrMask  |= (1u << 30) | (1u << 29);
  m.hdrBase[30] = 32;         // bank 1
  m.hdrBase[29] = 64;         // bank 2
  m.stopMask |= (1u << 31);   // 0x1F = stop all
  m.rel = REL_IGNORE;
}

bool parse(const char* text, Map& m) {
  defaults(m);
  if (!text) return false;

  // Pass 1: find gen= first, so its defaults can never overwrite an explicit key that
  // happened to appear earlier in the file (order-independent parsing).
  { const char* p = text; char line[128];
    while (*p) {
      size_t i = 0;
      while (*p && *p != '\n' && i < sizeof(line) - 1) line[i++] = *p++;
      while (*p && *p != '\n') p++;                // overlong line: skip the tail
      if (*p == '\n') p++;
      line[i] = 0; trim(line);
      if (line[0] == '#' || !line[0]) continue;
      char* eq = strchr(line, '='); if (!eq) continue;
      *eq = 0; trim(line);
      if (!strcasecmp(line, "gen")) { char* v = eq + 1; trim(v); m.gen = parseGen(v); }
    }
    applyGenDefaults(m);
  }

  // Pass 2: everything else.
  const char* p = text; char line[128];
  while (*p) {
    size_t i = 0;
    while (*p && *p != '\n' && i < sizeof(line) - 1) line[i++] = *p++;
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    line[i] = 0; trim(line);
    if (line[0] == '#' || !line[0]) continue;
    char* eq = strchr(line, '='); if (!eq) continue;
    *eq = 0; char* key = line; char* val = eq + 1;
    trim(key); trim(val);
    m.loaded = true;

    if      (!strcasecmp(key, "gen")) { /* pass 1 */ }
    else if (!strcasecmp(key, "name")) { strncpy(m.title, val, sizeof(m.title) - 1); m.title[sizeof(m.title) - 1] = 0; }
    else if (!strcasecmp(key, "ignore")) { m.ignoreMask = 0;   // explicit list REPLACES the default
             eachInList(val, [&](int v) { if (v >= 0 && v < 32) m.ignoreMask |= (1u << v); }); }
    else if (!strcasecmp(key, "stop"))   { m.stopMask = 0;
             eachInList(val, [&](int v) { if (v >= 0 && v < 32) m.stopMask |= (1u << v); }); }
    else if (!strcasecmp(key, "voice"))  { eachInList(val, [&](int v) { bitSet(m.voice, v); }); }
    else if (!strcasecmp(key, "loop"))   { eachInList(val, [&](int v) { bitSet(m.loop,  v); }); }
    else if (!strcasecmp(key, "header")) { int a, b;
             if (pair2(val, a, b) && a >= 0 && a < 32 && b >= 0 && b < MAX_ID) {
               m.hdrMask |= (1u << a); m.hdrBase[a] = (uint8_t)b; } }
    else if (!strcasecmp(key, "map"))    { int a, b;
             if (pair2(val, a, b) && a >= 0 && a < MAX_ID && b >= 0 && b < MAX_ID &&
                 m.nRemap < MAX_REMAP) {
               m.remapFrom[m.nRemap] = (uint8_t)a; m.remapTo[m.nRemap] = (uint8_t)b; m.nRemap++; } }
    else if (!strcasecmp(key, "hdrms"))    { long v = atol(val); if (v >= 0 && v <= 60000) m.hdrMs = (uint16_t)v; }
    else if (!strcasecmp(key, "repeatms")) { long v = atol(val); if (v >= 0 && v <= 60000) m.repeatMs = (uint16_t)v; }
    else if (!strcasecmp(key, "release"))  { m.rel = !strcasecmp(val, "stop") ? REL_STOP : REL_IGNORE; }
    else if (!strcasecmp(key, "legacy80b")) { if (atoi(val)) legacy80b(m); }
    // unknown key: ignored on purpose (a newer generator must not brick an older firmware)
  }
  return true;
}

void bind(Decoder& d, const Map* m) {
  d.m = m; d.pendBase = 0; d.armed = false; d.armedMs = 0; d.lastId = -1; d.lastMs = 0;
}

Out feed(Decoder& d, uint8_t cmd, uint32_t nowMs) {
  Out o{ ACT_NONE, -1, false, false };
  if (!d.m) return o;
  const Map& m = *d.m;
  cmd &= 0x1F;

  // An armed header that never got its payload must not poison a command a minute later.
  if (d.armed && m.hdrMs && (uint32_t)(nowMs - d.armedMs) > m.hdrMs) { d.armed = false; d.pendBase = 0; }

  // Ignored commands are dropped BEFORE the bank logic and do NOT disarm a pending header:
  // an ignored value is by definition not part of the protocol.
  if (cmd < 32 && (m.ignoreMask >> cmd) & 1u) return o;

  if ((m.stopMask >> cmd) & 1u) { d.armed = false; d.pendBase = 0; d.lastId = -1; o.act = ACT_STOPALL; return o; }

  if ((m.hdrMask >> cmd) & 1u) { d.pendBase = m.hdrBase[cmd]; d.armed = true; d.armedMs = nowMs; return o; }

  int id = (int)cmd + (d.armed ? (int)d.pendBase : 0);
  d.armed = false; d.pendBase = 0;
  for (uint8_t i = 0; i < m.nRemap; i++) if (m.remapFrom[i] == id) { id = m.remapTo[i]; break; }
  if (id < 0 || id >= MAX_ID) return o;

  // De-bounce an id repeated inside repeatMs (0 = off). Guards against a bus that reports the
  // same latch twice; NOT a musical decision — a drum roll needs repeatMs=0.
  if (m.repeatMs && id == d.lastId && (uint32_t)(nowMs - d.lastMs) < m.repeatMs) return o;
  d.lastId = id; d.lastMs = nowMs;

  o.act = ACT_PLAY; o.id = id;
  o.voice = bitGet(m.voice, id); o.loop = bitGet(m.loop, id);
  return o;
}

Out release(Decoder& d, uint32_t nowMs) {
  Out o{ ACT_NONE, -1, false, false };
  if (!d.m) return o;
  (void)nowMs;
  // A release does NOT disarm a header: on the real bus a two-command sequence is
  // "latch header / release / latch payload", so the release sits BETWEEN the pair.
  // Only hdrMs expires an armed header.
  d.lastId = -1;                                   // the next identical command must retrigger
  if (d.m->rel == REL_STOP) o.act = ACT_STOPALL;
  return o;
}

} // namespace sndmap
