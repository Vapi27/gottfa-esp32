// test_sndmap.cpp — host unit test for src/sndmap.{h,cpp} (the per-title sound-command map).
// Same code as the board: sndmap is pure C++ on purpose so the decode corner cases are proven
// here, on a Mac, instead of on a pinball that has to be powered up to answer a question.
// Build+run:
//   g++ -std=c++17 -I/Users/vapi27/gottfa-esp32/src /Users/vapi27/gottfa-esp32/tools/test_sndmap.cpp \
//       /Users/vapi27/gottfa-esp32/src/sndmap.cpp -o /tmp/test_sndmap && /tmp/test_sndmap
// (C) 2026 Valere Pilpil / Pstore.
#include "sndmap.h"
#include <cstdio>
using namespace sndmap;

static int fails = 0;
#define CHECK(cond, msg) do { if (cond) printf("  OK  %s\n", msg); \
                              else { printf("  FAIL %s\n", msg); fails++; } } while (0)

int main() {
  printf("1) defaults: cmd 0 is the bus-release artefact, never a sample\n");
  { Map m; defaults(m); Decoder d; bind(d, &m);
    CHECK(feed(d, 0, 1000).act == ACT_NONE, "cmd 0 ignored by default");
    Out o = feed(d, 21, 1000);
    CHECK(o.act == ACT_PLAY && o.id == 21, "cmd 21 -> sample 21 (identity)");
    CHECK(release(d, 1001).act == ACT_NONE, "release ignored by default (80B semantics)");
  }

  printf("2) Arena (80B Gen1, DIRECT not banked) — the ground-truthed map\n");
  { const char* txt =
      "# Arena\n"
      "name=Arena\n"
      "gen=80b1\n"
      "voice=14\n"
      "loop=30\n";
    Map m; CHECK(parse(txt, m), "parse ok");
    Decoder d; bind(d, &m);
    CHECK(m.gen == GEN_80B1 && m.loaded, "gen=80b1, loaded");
    Out sp = feed(d, 21, 100);
    CHECK(sp.act == ACT_PLAY && sp.id == 21, "spinner cmd21 -> 21");
    Out wall = feed(d, 14, 200);
    CHECK(wall.act == ACT_PLAY && wall.id == 14 && wall.voice, "wall cmd14 -> 14, voice bus");
    Out drone = feed(d, 30, 300);
    CHECK(drone.act == ACT_PLAY && drone.id == 30 && drone.loop,
          "cmd30 is the DRONE, not a bank header (the legacy rule would swallow it)");
    Out beep = feed(d, 28, 400);
    CHECK(beep.act == ACT_PLAY && beep.id == 28, "cmd28 beep -> 28");
    CHECK(feed(d, 28, 460).act == ACT_PLAY, "beep retriggers immediately (repeatMs=0 default)");
    CHECK(feed(d, 31, 500).act == ACT_PLAY, "cmd31 plays: Gen1 has no native stop");
  }

  printf("3) legacy 80B bank protocol, now DATA not code\n");
  { const char* txt = "gen=80b2\nlegacy80b=1\n";
    Map m; parse(txt, m); Decoder d; bind(d, &m);
    CHECK(feed(d, 30, 1000).act == ACT_NONE, "cmd30 header arms bank1, plays nothing");
    Out o = feed(d, 5, 1010);
    CHECK(o.act == ACT_PLAY && o.id == 37, "payload 5 after bank1 -> id 37");
    CHECK(feed(d, 29, 1020).act == ACT_NONE, "cmd29 header arms bank2");
    o = feed(d, 5, 1030);
    CHECK(o.act == ACT_PLAY && o.id == 69, "payload 5 after bank2 -> id 69");
    CHECK(feed(d, 31, 1040).act == ACT_STOPALL, "cmd31 -> stop all");
    o = feed(d, 5, 1050);
    CHECK(o.act == ACT_PLAY && o.id == 5, "bank does not stick to the next command");
  }

  printf("4) the header/payload pair survives the release BETWEEN them\n");
  { const char* txt = "gen=80b2\nheader=30:32\n";
    Map m; parse(txt, m); Decoder d; bind(d, &m);
    feed(d, 30, 2000);                     // header latched
    release(d, 2001);                      // bus released between the two latches — normal
    Out o = feed(d, 7, 2002);
    CHECK(o.act == ACT_PLAY && o.id == 39, "release does NOT disarm the header (39 = 7+32)");
  }

  printf("5) an armed header expires (hdrMs) instead of poisoning a later command\n");
  { const char* txt = "gen=80b2\nheader=30:32\nhdrms=250\n";
    Map m; parse(txt, m); Decoder d; bind(d, &m);
    feed(d, 30, 3000);
    Out o = feed(d, 7, 3000 + 251);
    CHECK(o.act == ACT_PLAY && o.id == 7, "stale header dropped after 251 ms");
    feed(d, 30, 4000);
    o = feed(d, 7, 4000 + 249);
    CHECK(o.act == ACT_PLAY && o.id == 39, "header still valid at 249 ms");
  }

  printf("6) an ignored command does not consume an armed header\n");
  { const char* txt = "gen=80b2\nheader=30:32\nignore=0\n";
    Map m; parse(txt, m); Decoder d; bind(d, &m);
    feed(d, 30, 5000);
    CHECK(feed(d, 0, 5010).act == ACT_NONE, "phantom 0 between header and payload: dropped");
    Out o = feed(d, 7, 5020);
    CHECK(o.act == ACT_PLAY && o.id == 39, "payload still gets its bank");
  }

  printf("7) System 80: releasing the bus STOPS the tone\n");
  { Map m; parse("gen=80\n", m); Decoder d; bind(d, &m);
    CHECK(m.rel == REL_STOP, "gen=80 implies release=stop");
    CHECK(release(d, 6000).act == ACT_STOPALL, "release -> stop all");
    Map m2; parse("gen=80b1\n", m2);
    CHECK(m2.rel == REL_IGNORE, "gen=80b1 implies release=ignore");
    Map m3; parse("gen=80\nrelease=ignore\n", m3);
    CHECK(m3.rel == REL_IGNORE, "explicit release= overrides the generation default");
  }

  printf("8) list syntax, remap, and an EMPTY list clearing a default\n");
  { const char* txt = "ignore=\nvoice=1-3,9\nmap=60->28\nstop=31,25\n";
    Map m; parse(txt, m); Decoder d; bind(d, &m);
    CHECK(feed(d, 0, 7000).act == ACT_PLAY, "'ignore=' (empty) re-enables command 0");
    CHECK(bitGet(m.voice, 1) && bitGet(m.voice, 3) && bitGet(m.voice, 9) && !bitGet(m.voice, 4),
          "voice=1-3,9 range + list");
    CHECK(feed(d, 31, 7010).act == ACT_STOPALL && feed(d, 25, 7020).act == ACT_STOPALL,
          "stop=31,25 both stop");
    Map m2; parse("header=30:32\nmap=60->28\n", m2); Decoder d2; bind(d2, &m2);
    feed(d2, 30, 8000);
    Out o = feed(d2, 28, 8010);
    CHECK(o.act == ACT_PLAY && o.id == 28, "remap 60->28 applied after the bank (28+32=60)");
  }

  printf("9) repeatms de-bounce, and gen= parsed whatever its position in the file\n");
  { Map m; parse("repeatms=100\n", m); Decoder d; bind(d, &m);
    CHECK(feed(d, 5, 9000).act == ACT_PLAY,  "first hit plays");
    CHECK(feed(d, 5, 9050).act == ACT_NONE,  "same id inside 100 ms dropped");
    CHECK(feed(d, 5, 9101).act == ACT_PLAY,  "same id after 100 ms plays");
    Map m2; parse("release=stop\ngen=80b1\n", m2);
    CHECK(m2.rel == REL_STOP, "explicit key wins even when it precedes gen= in the file");
  }

  printf("10) junk input is survivable (never a silent machine)\n");
  { Map m; CHECK(parse("", m), "empty file parses");
    CHECK(m.ignoreMask == 1u && !m.loaded, "empty file = defaults, loaded=false");
    Map m2; parse("!!! not a map\nfuturekey=42\nheader=99:1\nmap=1\n", m2);
    Decoder d; bind(d, &m2);
    CHECK(m2.hdrMask == 0 && m2.nRemap == 0, "out-of-range header / malformed map rejected");
    CHECK(feed(d, 12, 100).act == ACT_PLAY, "unknown keys ignored, decode still works");
    Decoder d3; bind(d3, nullptr);
    CHECK(feed(d3, 12, 100).act == ACT_NONE && release(d3, 100).act == ACT_NONE,
          "null map decodes to NONE instead of crashing");
  }

  printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
  return fails ? 1 : 0;
}
