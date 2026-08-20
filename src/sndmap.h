// sndmap.h — per-title sound-command map: turns a raw Gottlieb System 80/80A/80B sound
// command into "play sample N" / "stop everything" / "nothing", from DATA on the SD card
// instead of hardcoded firmware rules.
//
// WHY. The sound board is not emulated: the FPGA reports what the game CPU latched onto the
// sound bus and the ESP plays a sample. But what a 5-bit command MEANS is a property of the
// title's sound ROM, not of the hardware:
//   * System 80/80A drive a tone generator with a LEVEL: releasing the bus silences it.
//   * System 80B Gen1 (e.g. Arena) latch a 5-bit value straight into the sound CPU — 32 ids,
//     no bank protocol at all (verified: PinMAME gts80s.c s80bs_sh_w only does soundlatch_w +
//     IRQ, and gts80.c sends nothing when the strobe bit is clear).
//   * Later 80B ROMs extend the space with PREFIX commands: one command selects a bank and
//     the NEXT one is the payload.
// The firmware used to hardcode "30 arms +32, 29 arms +64, 31 stops" for every title with
// banked samples. That is a guess RE'd from one generation and is wrong for others — on Arena
// it would swallow cmd 30, which is the real wall drone. So the rules live in a file.
//
// PURE C++: no Arduino, no SD, no globals. Parsing and decoding are host-unit-testable
// (tools/test_sndmap.cpp), which is how the corner cases below are actually proven.
// (C) 2026 Valere Pillet / Pstore. Original implementation.
#pragma once
#include <stdint.h>

namespace sndmap {

constexpr int MAX_ID    = 96;   // sample ids 0..95 (base 0-31 + bank1 32-63 + bank2 64-95)
constexpr int MAX_REMAP = 32;

enum Gen : uint8_t { GEN_UNKNOWN = 0, GEN_80, GEN_80A, GEN_80B1, GEN_80B2, GEN_80B3 };

// What a "sound bus released / no command selected" event (wire byte 0x30) means.
enum Rel : uint8_t {
  REL_IGNORE = 0,   // 80B: the command was latched on the strobe, the release says nothing
  REL_STOP   = 1,   // System 80/80A: the tone plays WHILE a code is on the bus -> release = stop
};

enum Action : uint8_t {
  ACT_NONE = 0,     // nothing to do: ignored command, or a header that only armed a bank
  ACT_PLAY,         // play Out::id
  ACT_STOPALL,      // stop every voice
};

struct Map {
  bool     loaded;          // false = defaults only (no sound.map on the card)
  Gen      gen;
  uint32_t ignoreMask;      // bit c => command c is never playable   (default: bit 0)
  uint32_t stopMask;        // bit c => command c stops all sound
  uint32_t voice[3];        // bit i => sample id i is speech    (overlay on the filename attrs)
  uint32_t loop[3];         // bit i => sample id i is a loop/drone (idem)
  uint8_t  hdrBase[32];     // per command: the bank base armed for the NEXT command
  uint32_t hdrMask;         // bit c => command c IS a header (base may legitimately be 0)
  uint16_t hdrMs;           // an armed header expires after this long (default 250 ms)
  Rel      rel;
  uint16_t repeatMs;        // identical id again within this window is dropped (0 = never drop)
  uint8_t  remapFrom[MAX_REMAP], remapTo[MAX_REMAP], nRemap;
  char     title[24];
};

struct Out { Action act; int id; bool voice; bool loop; };

// Rolling decode state. One per sound path; keep it next to the Map it is bound to.
struct Decoder {
  const Map* m;
  uint8_t    pendBase;      // bank base armed by a header (0 = none), applies to the next cmd
  bool       armed;         // a header is armed (pendBase may legitimately be 0)
  uint32_t   armedMs;
  int        lastId;        // last id played (-1 = none) — for repeatMs
  uint32_t   lastMs;
};

// defaults(): the safe generic behaviour when a title ships no sound.map —
// command 0 ignored (it is the bus-release artefact), no headers, no stop command,
// release ignored, every other command plays the sample of the same number.
void defaults(Map& m);
// legacy80b(): EXACTLY what the firmware hardcoded before this module existed —
// 30 arms +32, 29 arms +64, 31 = stop all. Reverse-engineered from Gen2/3 titles and NEVER
// ground-truthed on hardware; kept so existing banked sets behave identically, and so it can
// be written down in a file and disproven with /sndtrace instead of hiding in the code.
void legacy80b(Map& m);

// Parse a whole sound.map file. Unknown keys are ignored (forward compatibility), the map is
// reset to defaults() first. Returns false only if `text` is null; a file with nothing usable
// in it still yields a valid default map (never a silent machine).
bool parse(const char* text, Map& m);

void bind(Decoder& d, const Map* m);       // reset the rolling state onto a map
Out  feed(Decoder& d, uint8_t cmd, uint32_t nowMs);   // one latched sound command (0..31)
Out  release(Decoder& d, uint32_t nowMs);             // one bus-release event (wire byte 0x30)

inline bool bitGet(const uint32_t* w, int i) {
  return (i >= 0 && i < MAX_ID) ? ((w[i >> 5] >> (i & 31)) & 1u) != 0 : false;
}

} // namespace sndmap
