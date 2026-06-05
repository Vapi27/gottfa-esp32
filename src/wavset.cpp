// wavset.cpp — see wavset.h. Pure C++ (host-testable).
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "wavset.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

namespace wavset {

static bool isnum(const char* s) {
  if (!s || !*s) return false;
  for (const char* p = s; *p; p++) if (!isdigit((unsigned char)*p)) return false;
  return true;
}

static uint8_t parseAttrs(const char* s) {
  uint8_t a = 0;
  for (; *s; s++) switch (*s) {
    case 'l': a |= A_LOOP;  break;  case 'b': a |= A_BREAK; break;
    case 'i': a |= A_INIT;  break;  case 'v': a |= A_VOICE; break;
    case 'k': a |= A_KILL;  break;  case 'c': a |= A_SKILL; break;
    case 'q': a |= A_QUIT;  break;  case 'x': a |= A_PLACE; break;
    default: break;
  }
  return a;
}

bool parseName(const char* fname, Entry& e) {
  e.id = -1; e.attr = 0; e.vol = 100; e.file[0] = 0;
  if (!fname) return false;
  strncpy(e.file, fname, sizeof(e.file) - 1); e.file[sizeof(e.file) - 1] = 0;
  char buf[64]; strncpy(buf, fname, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  char* dot = strrchr(buf, '.'); if (dot) *dot = 0;          // strip extension
  char* save = nullptr;
  char* t0 = strtok_r(buf, "-", &save);
  if (!isnum(t0)) return false;                              // must start with NNNN
  e.id = atoi(t0);
  char* t1 = strtok_r(nullptr, "-", &save);
  if (t1) {
    if (isnum(t1)) {                                         // short form NNNN-VVV
      e.vol = (uint8_t)atoi(t1);
    } else {                                                 // NNNN-AAAA[-VVV]
      e.attr = parseAttrs(t1);
      char* t2 = strtok_r(nullptr, "-", &save);
      if (isnum(t2)) e.vol = (uint8_t)atoi(t2);
    }
  }
  if (e.vol > 100) e.vol = 100;
  return true;
}

bool parseGroup(const char* fname, Group& g) {
  g.id = -1; g.random = false; g.n = 0; g.seq = 0;
  if (!fname) return false;
  char buf[80]; strncpy(buf, fname, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  char* dot = strrchr(buf, '.'); if (!dot || strcmp(dot, ".grp")) return false; *dot = 0;
  char* save = nullptr;
  char* t0 = strtok_r(buf, "-", &save); if (!isnum(t0)) return false;
  g.id = atoi(t0);
  char* t1 = strtok_r(nullptr, "-", &save); if (!t1) return false;
  g.random = (t1[0] == 'm');                                 // m random, r sequential
  for (char* t = strtok_r(nullptr, "-", &save); t && g.n < 8; t = strtok_r(nullptr, "-", &save)) {
    if (!isnum(t)) break;                                    // description starts -> stop
    g.member[g.n++] = atoi(t);
  }
  return g.n > 0;
}

void defaultConfig(Config& c) {
  c.volv = 100; c.vols = 100; c.mix = 1;                     // div2
  strncpy(c.stheme, "orgsnd", sizeof(c.stheme) - 1); c.stheme[sizeof(c.stheme) - 1] = 0;
}

void parseConfig(const char* text, Config& c) {
  defaultConfig(c);
  if (!text) return;
  const char* p = text;
  char line[80];
  while (*p) {
    size_t i = 0;
    while (*p && *p != '\n' && i < sizeof(line) - 1) { if (*p != '\r') line[i++] = *p; p++; }
    while (*p == '\n' || *p == '\r') p++;
    line[i] = 0;
    char* eq = strchr(line, '='); if (!eq) continue; *eq = 0;
    const char* key = line; const char* val = eq + 1;
    if      (!strcmp(key, "volv"))   c.volv = (uint8_t)atoi(val);
    else if (!strcmp(key, "vols"))   c.vols = (uint8_t)atoi(val);
    else if (!strcmp(key, "stheme")) { strncpy(c.stheme, val, sizeof(c.stheme) - 1); c.stheme[sizeof(c.stheme) - 1] = 0; }
    else if (!strcmp(key, "mix"))    c.mix = !strcmp(val, "sum") ? 0 : !strcmp(val, "sqrt") ? 2 : 1;
  }
  if (c.volv > 100) c.volv = 100;
  if (c.vols > 100) c.vols = 100;
}

void Set::reset() { nEntry = 0; nGroup = 0; }

bool Set::addName(const char* fname) {
  const char* dot = fname ? strrchr(fname, '.') : nullptr;
  if (!dot) return false;
  if (!strcmp(dot, ".grp")) {
    Group g; if (nGroup < MAX_GROUPS && parseGroup(fname, g)) { group[nGroup++] = g; return true; }
    return false;
  }
  if (!strcmp(dot, ".wav")) {
    Entry e; if (nEntry < MAX_ENTRIES && parseName(fname, e)) { entry[nEntry++] = e; return true; }
    return false;
  }
  return false;
}

const Entry* Set::find(int id) const {
  for (int i = 0; i < nEntry; i++) if (entry[i].id == id) return &entry[i];
  return nullptr;
}

int Set::pick(int id, uint32_t rnd) {
  for (int i = 0; i < nGroup; i++) if (group[i].id == id && group[i].n > 0) {
    Group& g = group[i];
    int idx = g.random ? (int)(rnd % g.n) : (g.seq++ % g.n);
    return g.member[idx];
  }
  return id;
}

} // namespace wavset
