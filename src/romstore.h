// romstore.h — per-game ROM-image store on the ESP's SD card.
//
// Foundation for the "one-card" architecture (PSOROM / route a|c): the ESP holds the FPGA's
// game ROMs on ITS SD and serves image N to the FPGA (program a NOR, or feed the FPGA's RAM at
// boot). Each game keeps TWO ROM variants — stock and Free-Play — mirroring how GottFA ships a
// normal image and an FP image; a global Free-Play device setting selects which one is served.
// Audio (PSOWAV WAV sets) lives separately under /<romname>/ and is SHARED by both variants
// (Free Play changes credit logic only, not sounds).
//
// Layout on the ESP SD:
//   /roms/<NN>.img     stock   game image   (NN = FPGA game No 0..62 = DIP S1 = games.txt index)
//   /roms/<NN>fp.img   free-play game image
//   Each file is EITHER a 16384-byte plaintext GottFA-format image (32 sectors × 512 B, byte-
//   identical to the FPGA SD's game region at sector 660 + N*32, so feeding the FPGA is a pure
//   offset copy — no conversion), OR a 16412-byte encrypted "PSRC" container (see romcrypt.h).
//   read() handles both transparently; store() writes encrypted.
// Encryption is device-bound anti-extraction, NOT a legal mechanism — the FPGA's own boot SD
// stays plaintext (its reader can't decrypt); encryption applies to this ESP-owned store.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace romstore {

constexpr int IMG_SIZE = 16384;   // 16 KB per game (32 sectors of 512 B) — must match the FPGA
constexpr int MAX_GAME = 63;      // FPGA game No 0..62

void   begin();                                  // check /roms/ + init romcrypt + load FP setting

// Per-variant access. fp=false -> stock (/roms/NN.img), fp=true -> Free Play (/roms/NNfp.img).
bool   has(int gameNo, bool fp);                 // present (plaintext 16 KB or PSRC container)?
bool   encrypted(int gameNo, bool fp);           // stored as an encrypted PSRC container?
size_t read(int gameNo, bool fp, uint8_t* buf, size_t bufLen);   // 16 KB plaintext (auto-decrypt); IMG_SIZE or 0
bool   store(int gameNo, bool fp, const uint8_t* plain);         // encrypt + write the variant; true on success
bool   remove(int gameNo, bool fp);              // delete one variant's file; true if gone after

int    count();                                  // games with at least one variant present (0..MAX_GAME)

// Global Free-Play device setting (persisted in NVS) — which variant is served to the FPGA.
bool   freePlay();
void   setFreePlay(bool on);

// Read the ACTIVE variant for the FPGA: Free Play if enabled AND present, else stock. 16 KB or 0.
// This is what the NOR/route-c delivery feeds to the FPGA — the FPGA-native 16 KB blob, no convert.
size_t readActive(int gameNo, uint8_t* buf, size_t bufLen);

// pure helper (no SD / host-testable): build "/roms/NN.img" or "/roms/NNfp.img" into out
void   path(int gameNo, bool fp, char* out, size_t outLen);

} // namespace romstore
