// romdb.h — known-good ROM checksum database (verify a user's dump is the correct, intact ROM).
// DB = /db/roms.csv on the SD (crc,name,game,title), generated from PinMAME by tools/build_romdb.py
// (every Gottlieb game / version / sound ROM, ~389 CRCs). A CRC match = correct + intact; no match
// = corrupted chip OR an unlisted revision. Also the "proof of ownership" check: a verified dump
// identifies which game the user owns. (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace romdb {

struct Match {
  bool     found;
  uint32_t crc;
  char     name[24];     // ROM file name (e.g. "668-4.cpu")
  char     game[16];     // romset id (e.g. "blckhole")
  char     title[48];    // human title (e.g. "Black Hole (rev. 4)")
};

uint32_t crc32(const uint8_t* d, size_t n);          // IEEE CRC-32 (matches PinMAME)
bool     begin();                                     // /db/roms.csv present?
int      count();                                     // number of known ROMs in the DB
bool     identify(uint32_t crc, Match* out);          // CRC -> known-good entry
bool     identifyFile(const char* path, Match* out);  // crc32 a file then identify (out->crc set even if !found)

} // namespace romdb
