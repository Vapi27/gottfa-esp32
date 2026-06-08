// ownership.cpp — see ownership.h. SD-file owned set (/owned.txt) + NVS gate flag.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "ownership.h"
#include <string.h>

#ifndef BOARD_C3
#include <Arduino.h>
#include <SD.h>
#include <Preferences.h>

namespace ownership {

static const char* OWNED_PATH = "/owned.txt";
static bool g_gate = false;

void begin() {
  Preferences p;
  if (p.begin("ownership", true)) { g_gate = p.getBool("gate", false); p.end(); }
  log_i("[own] gate %s", g_gate ? "ON" : "OFF");
}

bool gateEnabled() { return g_gate; }

void setGate(bool on) {
  g_gate = on;
  Preferences p; if (p.begin("ownership", false)) { p.putBool("gate", on); p.end(); }
  log_i("[own] gate -> %s", on ? "ON" : "OFF");
}

bool owns(const char* game) {
  if (!game || !*game) return false;
  File f = SD.open(OWNED_PATH, FILE_READ);
  if (!f) return false;
  bool found = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() && line == game) { found = true; break; }
  }
  f.close();
  return found;
}

bool own(const char* game) {
  if (!game || !*game || owns(game)) return false;
  File f = SD.open(OWNED_PATH, FILE_APPEND);
  if (!f) return false;
  f.print(game); f.print('\n');
  f.close();
  log_i("[own] + %s", game);
  return true;
}

bool allowed(const char* game) { return !g_gate || owns(game); }

int list(char* out, int cap) {
  if (out && cap) out[0] = 0;
  File f = SD.open(OWNED_PATH, FILE_READ);
  if (!f) return 0;
  int n = 0, len = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    if (out && len + (int)line.length() + 2 < cap) {
      if (n) { out[len++] = ','; }
      strcpy(out + len, line.c_str()); len += line.length();
    }
    n++;
  }
  f.close();
  return n;
}

} // namespace ownership

#else   // BOARD_C3 — no SD; gate disabled (sound always allowed)

namespace ownership {
void begin() {}
bool gateEnabled() { return false; }
void setGate(bool) {}
bool owns(const char*) { return false; }
bool own(const char*) { return false; }
bool allowed(const char*) { return true; }
int  list(char*, int) { return 0; }
} // namespace ownership

#endif
