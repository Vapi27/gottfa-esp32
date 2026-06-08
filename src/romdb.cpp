// romdb.cpp — see romdb.h. Streams /db/roms.csv to verify a dump's CRC against PinMAME known-good.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "romdb.h"
#include <string.h>
#include <stdio.h>

#ifndef BOARD_C3
#include <Arduino.h>
#include <SD.h>

namespace romdb {

static bool g_ready = false;
static const char* DB_PATH = "/db/roms.csv";

uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
  }
  return ~c;
}

bool begin() {
  g_ready = SD.exists(DB_PATH);
  log_i("[romdb] %s (%d ROMs)", g_ready ? "ready" : "absent", g_ready ? count() : 0);
  return g_ready;
}

int count() {
  if (!SD.exists(DB_PATH)) return 0;
  File f = SD.open(DB_PATH, FILE_READ);
  if (!f) return 0;
  int n = 0;
  while (f.available()) { if (f.read() == '\n') n++; }
  f.close();
  return n > 0 ? n - 1 : 0;                 // minus the header line
}

// copy CSV field (comma/newline-terminated) into dst (truncated, NUL-terminated)
static void field(const String& s, int from, int to, char* dst, size_t cap) {
  int n = to - from; if (n < 0) n = 0; if ((size_t)n >= cap) n = cap - 1;
  for (int i = 0; i < n; i++) dst[i] = s[from + i];
  dst[n] = 0;
}

bool identify(uint32_t crc, Match* out) {
  if (out) { out->found = false; out->crc = crc; out->name[0] = out->game[0] = out->title[0] = 0; }
  if (!g_ready) return false;
  char want[9]; snprintf(want, sizeof(want), "%08x", (unsigned)crc);
  File f = SD.open(DB_PATH, FILE_READ);
  if (!f) return false;
  bool found = false;
  f.readStringUntil('\n');                   // skip header
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() < 10 || line[8] != ',') continue;
    if (!line.startsWith(want)) continue;
    if (out) {
      out->found = true;
      int c1 = line.indexOf(',', 9);
      int c2 = c1 >= 0 ? line.indexOf(',', c1 + 1) : -1;
      int end = line.length();
      while (end > 0 && (line[end - 1] == '\r' || line[end - 1] == '\n')) end--;
      field(line, 9, c1 >= 0 ? c1 : end, out->name, sizeof(out->name));
      if (c1 >= 0) field(line, c1 + 1, c2 >= 0 ? c2 : end, out->game, sizeof(out->game));
      if (c2 >= 0) field(line, c2 + 1, end, out->title, sizeof(out->title));
    }
    found = true;
    break;
  }
  f.close();
  return found;
}

bool identifyFile(const char* path, Match* out) {
  if (out) { out->found = false; out->crc = 0; out->name[0] = out->game[0] = out->title[0] = 0; }
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  uint32_t c = 0xFFFFFFFFu;
  uint8_t buf[512]; size_t r;
  while ((r = f.read(buf, sizeof(buf))) > 0)
    for (size_t i = 0; i < r; i++) {
      c ^= buf[i];
      for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
  f.close();
  return identify(~c, out);
}

} // namespace romdb

#else   // BOARD_C3 — no SD

namespace romdb {
uint32_t crc32(const uint8_t*, size_t) { return 0; }
bool begin() { return false; }
int  count() { return 0; }
bool identify(uint32_t, Match* out) { if (out) out->found = false; return false; }
bool identifyFile(const char*, Match* out) { if (out) out->found = false; return false; }
} // namespace romdb

#endif
