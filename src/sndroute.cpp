// sndroute.cpp — the per-game sound-routing table. See sndroute.h.
// Mirrors GOSOF80.vhd's per-game case (game_number -> SB_type + speech_ctrl). The 32-bit speech
// value is exactly GOSOF80's `speech_ctrl`: bit c (=command c) is 0 for a SPEECH command (which the
// ESP plays) and 1 otherwise (GOSOF80 synthesises it). cls: 0=G(gosof-synth) 1=H(hybride) 2=E(esp-full).
// (C) 2026 Valere Pilpil / Pstore.
#include "sndroute.h"
#include <stdint.h>

namespace {
  struct R { uint8_t cls; uint32_t speech; };
  enum { G = 0, H = 1, E = 2 };
  // indexed by FPGA game No (GottFA80_PLuS gamelist / GOSOF80 game_number); 43..63 = is_special (80B).
  const R T[64] = {
    /* 0 Panthera     */ {G, 0xFFFFFFFFu},
    /* 1 Spiderman    */ {G, 0xFFFFFFFFu},
    /* 2 Circus       */ {G, 0xFFFFFFFFu},
    /* 3 Counterforce */ {G, 0xFFFFFFFFu},
    /* 4 Star Race    */ {G, 0xFFFFFFFFu},
    /* 5 James Bond T */ {G, 0xFFFFFFFFu},
    /* 6 James Bond B */ {G, 0xFFFFFFFFu},
    /* 7 Time Line    */ {G, 0xFFFFFFFFu},
    /* 8 Force II      */ {G, 0xFFFFFFFFu},
    /* 9 Pink Panther */ {H, 0xFFFFFFFDu},   // 1 speech cmd
    /*10 Mars (speech)*/ {H, 0x040179E7u},
    /*11 Mars (snd)   */ {G, 0xFFFFFFFFu},
    /*12 Volcano (sp) */ {H, 0x10636367u},
    /*13 Volcano (snd)*/ {G, 0xFFFFFFFFu},
    /*14 Black Hole   */ {H, 0x003F5FF7u},
    /*15 Black Hole sn*/ {G, 0xFFFFFFFFu},
    /*16 Haunted House*/ {G, 0xFFFFFFFFu},
    /*17 Eclipse      */ {G, 0xFFFFFFFFu},
    /*18 Devils Dare  */ {H, 0x3FA5BFFFu},
    /*19 Devils Dare s*/ {G, 0xFFFFFFFFu},
    /*20 Rocky        */ {H, 0x001FFFFFu},
    /*21 Spirit       */ {G, 0xFFFFFFFFu},
    /*22 Punk!        */ {G, 0xFFFFFFFFu},
    /*23 Striker      */ {H, 0x07FB7FF7u},
    /*24 Krull        */ {G, 0xFFFFFFFFu},
    /*25 QBerts Quest */ {H, 0xFFFF0F75u},
    /*26 Super Orbit  */ {G, 0xFFFFFFFFu},
    /*27 Royal Flush  */ {G, 0xFFFFFFFFu},
    /*28 Goin Nuts    */ {G, 0xFFFFFFFFu},
    /*29 Amazon Hunt  */ {G, 0xFFFFFFFFu},
    /*30 Rack Em Up   */ {G, 0xFFFFFFFFu},
    /*31 Ready Aim Fir*/ {G, 0xFFFFFFFFu},
    /*32 Jacks Open   */ {G, 0xFFFFFFFFu},
    /*33 Alien Star   */ {G, 0xFFFFFFFFu},
    /*34 The Games    */ {G, 0xFFFFFFFFu},
    /*35 Touchdown    */ {G, 0xFFFFFFFFu},
    /*36 El Dorado CoG*/ {G, 0xFFFFFFFFu},
    /*37 Ice Fever    */ {G, 0xFFFFFFFFu},
    /*38 Caveman      */ {H, 0x07FBFFFBu},
    /*39 Caveman (v2) */ {H, 0x07FBFFFBu},
    /*40 Bounty Hunter*/ {G, 0xFFFFFFFFu},   // early 80B, MA-490: GOSOF80 supports it
    /*41 Triple Play  */ {G, 0xFFFFFFFFu},
    /*42 Tag Team     */ {G, 0xFFFFFFFFu},
    /*43 */ {E, 0xFFFFFFFFu}, /*44 */ {E, 0xFFFFFFFFu}, /*45 */ {E, 0xFFFFFFFFu},
    /*46 */ {E, 0xFFFFFFFFu}, /*47 */ {E, 0xFFFFFFFFu}, /*48 */ {E, 0xFFFFFFFFu},
    /*49 */ {E, 0xFFFFFFFFu}, /*50 */ {E, 0xFFFFFFFFu}, /*51 Arena */ {E, 0xFFFFFFFFu},
    /*52 */ {E, 0xFFFFFFFFu}, /*53 */ {E, 0xFFFFFFFFu}, /*54 */ {E, 0xFFFFFFFFu},
    /*55 */ {E, 0xFFFFFFFFu}, /*56 */ {E, 0xFFFFFFFFu}, /*57 Bad Girls */ {E, 0xFFFFFFFFu},
    /*58 Big House */ {E, 0xFFFFFFFFu}, /*59 Hot Shots */ {E, 0xFFFFFFFFu},
    /*60 Bone Busters */ {E, 0xFFFFFFFFu}, /*61 Night Moves */ {E, 0xFFFFFFFFu},
    /*62 Amazon Hunt II*/ {E, 0xFFFFFFFFu}, /*63 (spare)*/ {E, 0xFFFFFFFFu},
  };
}

namespace sndroute {
bool espPlays(int gameNo, int cmd) {
  if (gameNo < 0 || gameNo >= 64) return true;          // unknown game -> play (safe)
  const R& r = T[gameNo];
  if (r.cls == E) return true;                           // esp-full: ESP plays everything
  if (r.cls == G) return false;                          // gosof-synth: GOSOF80 plays it, ESP silent
  return cmd >= 0 && cmd < 32 && ((r.speech >> cmd) & 1u) == 0;  // hybride: ESP plays speech cmds
}
}
