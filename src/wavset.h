// wavset.h — parses a sound set's SD layout (the pwavplayer file convention, adopted
// for interchange) into an index the player resolves at play time. PURE (no Arduino):
// only the directory listing is platform; every parser here is host-unit-testable.
//
// File convention (clean-room — we read the data format, not pwavplayer's CC-NC code):
//   sounds : "NNNN-AAAA-VVV-description.wav"  NNNN=id, AAAA=attrs, VVV=volume 0..100
//            (short forms "NNNN-VVV.wav" and bare "NNNN.wav" also accepted)
//   groups : "NNNN-A-M1-M2-...-description.grp"  A = m (random) | r (sequential)
//   config : /config.txt at SD root, key=value lines (mix, volv, vols, stheme)
//   attrs  : l loop · b break(stop same id) · i init/background · v voice bus
//            k kill(all) · c soft-kill(non-bg) · q quit(keep loops+voices) · x placeholder
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include <stdint.h>

namespace wavset {

enum Attr : uint8_t {
  A_LOOP  = 1 << 0,   // l  loop indefinitely
  A_BREAK = 1 << 1,   // b  stop same-id instances before restart
  A_INIT  = 1 << 2,   // i  init/background (autoplay on load; +l = bg loop)
  A_VOICE = 1 << 3,   // v  voice bus (volv scaling instead of vols)
  A_KILL  = 1 << 4,   // k  stop all tracks
  A_SKILL = 1 << 5,   // c  soft-kill (stop non-background)
  A_QUIT  = 1 << 6,   // q  soft-kill preserving loops & voices
  A_PLACE = 1 << 7,   // x  placeholder (no audio)
};

struct Entry { int id; uint8_t attr; uint8_t vol; char file[64]; };
struct Group { int id; bool random; uint8_t n; int member[8]; uint16_t seq; };
struct Config { uint8_t volv; uint8_t vols; uint8_t mix; char stheme[24]; };  // mix 0=sum 1=div2 2=sqrt

// --- pure parsers (host-testable) ---
bool parseName (const char* fname, Entry& e);    // returns false if not "NNNN...".wav
bool parseGroup(const char* fname, Group& g);    // returns false if not a valid ".grp"
void parseConfig(const char* text, Config& c);   // whole /config.txt content
void defaultConfig(Config& c);                   // volv=vols=100, mix=div2, stheme="orgsnd"

constexpr int MAX_ENTRIES = 64;
constexpr int MAX_GROUPS  = 16;

struct Set {
  Entry entry[MAX_ENTRIES]; int nEntry;
  Group group[MAX_GROUPS];  int nGroup;
  void reset();
  bool addName(const char* fname);                 // dispatch by extension (.wav/.grp)
  const Entry* find(int id) const;                 // entry for a sound id, or nullptr
  int  pick(int id, uint32_t rnd);                 // group -> member id (random/seq); else id
};

} // namespace wavset
