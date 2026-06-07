// romcrypt.h — device-bound encryption for the ESP /roms ROM-image store.
//
// Goal: the SD store holds ROM images that are USELESS without THIS ESP. A 256-bit key is
// generated on first boot (hardware RNG) and kept in NVS — it never leaves the device. Pull the
// SD card out and you get only ciphertext.
//
// IMPORTANT — what this is / is NOT:
//   * Anti-extraction / anti-redistribution (DRM-ish) + good-faith. It does NOT change legality:
//     owning the machine is what makes holding the ROM legal; the user always supplies their own.
//   * The FPGA's OWN boot SD must stay PLAINTEXT — its SD_Card.vhd reads raw sectors with no
//     decryptor. Encryption applies only to this ESP-owned store (and the future NOR-boot path,
//     where the ESP decrypts in RAM and feeds the FPGA, so no plaintext ROM is ever at rest).
//   * Without ESP flash-encryption enabled, the NVS key is recoverable by desoldering+dumping the
//     ESP flash. That is out of the threat model here (which is "someone pulls the SD card").
//
// Container layout (one per game image), little-endian:
//   [0]   magic   "PSRC"
//   [4]   ver     = 1
//   [5]   flags   bit0 = free-play variant (informational only)
//   [6]   rsv[2]  = 0
//   [8]   iv[16]                  random per file (AES-CTR initial counter block)
//   [24]  crc32[4]                CRC-32 of the 16384-byte plaintext (wrong-key / corruption check)
//   [28]  ciphertext[16384]
//   total = 16412 bytes.  Cipher = AES-256-CTR (mbedtls, HW-accelerated on the S3).
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace romcrypt {

constexpr int     HDR_SIZE     = 28;
constexpr int     PLAIN_SIZE   = 16384;
constexpr int     CONT_SIZE    = HDR_SIZE + PLAIN_SIZE;   // 16412
constexpr uint8_t VERSION      = 1;
constexpr uint8_t FLAG_FREEPLAY = 0x01;

bool     begin();        // load or create the device key (NVS). false if RNG/NVS unusable.
bool     available();    // a key is loaded
uint32_t keyId();        // non-secret 32-bit fingerprint of the key (UI / "same device?")

// Encrypt PLAIN_SIZE bytes of `plain` into a CONT_SIZE `container`. `flags` stored verbatim.
bool encrypt(const uint8_t* plain, uint8_t* container, uint8_t flags = 0);

// True if `buf`/`len` looks like one of our containers (size + magic).
bool isContainer(const uint8_t* buf, size_t len);

// Decrypt a CONT_SIZE container into PLAIN_SIZE `plain`; verifies the CRC (false on wrong key /
// corruption / bad size). `outFlags` (optional) receives the stored flags byte.
bool decrypt(const uint8_t* container, size_t len, uint8_t* plain, uint8_t* outFlags = nullptr);

} // namespace romcrypt
