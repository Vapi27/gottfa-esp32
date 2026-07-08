// romstore.cpp — see romstore.h. Per-game ROM-image store on the ESP SD with stock + Free-Play
// variants, transparent decrypt (plaintext 16384 B or encrypted PSRC 16412 B), and a persisted
// Free-Play device setting selecting which variant is served to the FPGA.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "romstore.h"
#include "romcrypt.h"
#include <stdio.h>
#include <stdlib.h>
#ifndef BOARD_C3
#include <Arduino.h>
#include <SD.h>
#include <Preferences.h>
#endif

namespace romstore {

void path(int gameNo, bool fp, char* out, size_t outLen) {     // pure — host-testable
  snprintf(out, outLen, fp ? "/roms/%02dfp.img" : "/roms/%02d.img", gameNo);
}

#ifndef BOARD_C3

static bool g_ready    = false;
static bool g_freePlay = false;

static long fileSize(int gameNo, bool fp) {
  char p[28]; path(gameNo, fp, p, sizeof(p));
  File f = SD.open(p, FILE_READ);
  if (!f) return -1;
  long s = (long)f.size();
  f.close();
  return s;
}

void begin() {
  romcrypt::begin();                                  // load/create the device key (NVS)
  { Preferences pr; if (pr.begin("romstore", true)) { g_freePlay = pr.getBool("fp", false); pr.end(); } }
  g_ready = SD.exists("/roms");
  log_i("[rom] store %s (%d games, FP=%d, key %s id=%08X)",
        g_ready ? "ready" : "absent", g_ready ? count() : 0, g_freePlay ? 1 : 0,
        romcrypt::available() ? "OK" : "MISSING", (unsigned)romcrypt::keyId());
}

bool has(int gameNo, bool fp) {
  if (!g_ready || gameNo < 0 || gameNo >= MAX_GAME) return false;
  long s = fileSize(gameNo, fp);
  return (s == IMG_SIZE) || (s == romcrypt::CONT_SIZE);
}

bool encrypted(int gameNo, bool fp) {
  if (gameNo < 0 || gameNo >= MAX_GAME) return false;
  return fileSize(gameNo, fp) == romcrypt::CONT_SIZE;
}

int count() {
  int n = 0;
  for (int i = 0; i < MAX_GAME; i++) if (has(i, false) || has(i, true)) n++;
  return n;
}

size_t read(int gameNo, bool fp, uint8_t* buf, size_t bufLen) {
  if (!g_ready || gameNo < 0 || gameNo >= MAX_GAME || !buf || bufLen < (size_t)IMG_SIZE) return 0;
  char p[28]; path(gameNo, fp, p, sizeof(p));
  File f = SD.open(p, FILE_READ);
  if (!f) return 0;
  long s = (long)f.size();

  if (s == IMG_SIZE) {                                          // plaintext
    size_t r = f.read(buf, IMG_SIZE);
    f.close();
    return (r == (size_t)IMG_SIZE) ? (size_t)IMG_SIZE : 0;
  }
  if (s == romcrypt::CONT_SIZE) {                               // encrypted PSRC container
    uint8_t* cont = (uint8_t*)malloc(romcrypt::CONT_SIZE);
    if (!cont) { f.close(); return 0; }
    size_t r = f.read(cont, romcrypt::CONT_SIZE);
    f.close();
    bool ok = (r == (size_t)romcrypt::CONT_SIZE) &&
              romcrypt::decrypt(cont, romcrypt::CONT_SIZE, buf, nullptr);
    free(cont);
    return ok ? (size_t)IMG_SIZE : 0;
  }
  f.close();
  return 0;
}

size_t readActive(int gameNo, uint8_t* buf, size_t bufLen) {
  if (g_freePlay && has(gameNo, true)) return read(gameNo, true, buf, bufLen);  // FP if set & present
  return read(gameNo, false, buf, bufLen);                                      // else stock
}

bool store(int gameNo, bool fp, const uint8_t* plain) {
  if (gameNo < 0 || gameNo >= MAX_GAME || !plain) return false;
  if (!romcrypt::available() && !romcrypt::begin()) { log_e("[rom] no device key"); return false; }

  uint8_t* cont = (uint8_t*)malloc(romcrypt::CONT_SIZE);
  if (!cont) return false;
  bool ok = romcrypt::encrypt(plain, cont, fp ? romcrypt::FLAG_FREEPLAY : 0);

  if (ok) {
    if (!SD.exists("/roms")) SD.mkdir("/roms");
    char p[28]; path(gameNo, fp, p, sizeof(p));
    // Robust overwrite: remove, then confirm the delete settled before re-opening.
    // On the SPI SD (FatFS) an open() racing a just-issued remove() can fail, which is
    // why a re-write of an existing slot used to fail while a fresh slot always worked.
    if (SD.exists(p)) {
      SD.remove(p);
      for (int i = 0; i < 20 && SD.exists(p); i++) delay(5);   // wait out the FAT update
    }
    File f = SD.open(p, FILE_WRITE);
    if (!f) { delay(20); f = SD.open(p, FILE_WRITE); }         // one retry
    ok = (bool)f;
    if (ok) {
      f.seek(0);                                               // ensure we start at byte 0
      ok = (f.write(cont, romcrypt::CONT_SIZE) == (size_t)romcrypt::CONT_SIZE);
      f.flush();                                               // commit before close
      f.close();
    }
    if (ok) { g_ready = true; log_i("[rom] stored game %d %s (%d B)", gameNo, fp ? "FP" : "stock", romcrypt::CONT_SIZE); }
    else log_e("[rom] write failed for game %d %s", gameNo, fp ? "FP" : "stock");
  }
  free(cont);
  return ok;
}

bool remove(int gameNo, bool fp) {
  if (gameNo < 0 || gameNo >= MAX_GAME) return false;
  char p[28]; path(gameNo, fp, p, sizeof(p));
  if (!SD.exists(p)) return true;                 // already gone
  SD.remove(p);
  for (int i = 0; i < 20 && SD.exists(p); i++) delay(5);
  bool gone = !SD.exists(p);
  if (gone) log_i("[rom] removed game %d %s", gameNo, fp ? "FP" : "stock");
  return gone;
}

bool freePlay() { return g_freePlay; }

void setFreePlay(bool on) {
  g_freePlay = on;
  Preferences pr; if (pr.begin("romstore", false)) { pr.putBool("fp", on); pr.end(); }
  log_i("[rom] Free Play %s", on ? "ON" : "OFF");
}

#else   // BOARD_C3 — no SD/sound tier

void   begin() {}
bool   has(int, bool) { return false; }
bool   encrypted(int, bool) { return false; }
size_t read(int, bool, uint8_t*, size_t) { return 0; }
size_t readActive(int, uint8_t*, size_t) { return 0; }
bool   store(int, bool, const uint8_t*) { return false; }
int    count() { return 0; }
bool   freePlay() { return false; }
void   setFreePlay(bool) {}

#endif

} // namespace romstore
