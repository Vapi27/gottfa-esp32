// ownership.h — proof-of-ownership gate (the user's principle: a verified dump of a game's CPU ROM
// proves they own that machine -> unlock that game's PSOWAV sound). When the gate is ON, a game's
// sound only plays once the game is in the owned set; a verified dump (romdb match) adds it.
// Owned set = /owned.txt on the SD (one romset id per line). Gate flag persisted in NVS (default OFF
// = current behaviour, all sound plays). NOTE: this is a good-faith / risk-reduction measure, NOT a
// legal mechanism (the PSOWAV samples remain derivative — see project notes).
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once

namespace ownership {

void begin();                       // load the gate flag
bool gateEnabled();
void setGate(bool on);              // persist the gate flag (NVS)

bool owns(const char* game);        // is this romset id / theme in /owned.txt ?
bool own(const char* game);         // record proven ownership (append /owned.txt); true if newly added
bool allowed(const char* game);     // gate OFF -> always true ; gate ON -> owns(game)

int  list(char* out, int cap);      // comma-joined owned ids into out (NUL-terminated); returns count

} // namespace ownership
