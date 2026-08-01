// gamedata.h — per-title switch and solenoid names, and what each coil is supposed to move.
//
// WHY THIS EXISTS.  The coil test works by watching the switch matrix: a healthy coil
// MOVES something, and almost everything that moves is watched by a switch. That only
// yields a verdict if we know WHICH switch each coil is supposed to disturb — and that
// table is per-title. Nobody publishes it in machine-readable form:
//   * PinMAME gives every System 80 title the same generic input ports (verified:
//     gts80games.c line 14 is `GTS80_INPUT_PORTS_START(gts80,1) GTS80_INPUT_PORTS_END`),
//   * LISY names only the DIP switches.
// It IS in each game's own manual, in the "SWITCH MATRIX" schematic and the "SOLENOID
// ASSIGNMENTS" table. Those manuals are scans with no text layer, so the tables were
// read off the pixels (gottfa-tools/pdfocr) and, where a Visual Pinball table exists for
// the title, cross-checked against its script — two independent sources that were built
// by different people from different artefacts.
//
// WHY THE MACHINE NEEDS IT AND NOT JUST THE UI.  Because of the precondition problem,
// which is the whole reason this file exists. A drop-target reset coil moves nothing if
// the targets are ALREADY up; an outhole kicker moves nothing with no ball in the outhole.
// Without knowing what a coil is supposed to move, a perfectly healthy coil reads as
// "aucune réaction" — a false accusation against good hardware, which is strictly worse
// than having no test. So the firmware itself has to know the expectation, in order to
// answer "précondition non remplie, mets les cibles en bas" instead of "muet".
//
// PROVENANCE IS PART OF THE DATA.  Every file carries `src` (where the table came from)
// and `conf` (1 = one source, 2 = two independent sources agreeing). The UI shows it. A
// number that drives a diagnosis on someone's machine should never be anonymous.
//
// FORMAT — /gd-NN.json, NN = the FPGA game number 0..62 (see GottFA80_PLuS_GameSelect):
//   {"v":1,"g":12,"t":"Volcano","fam":80,"src":"...","conf":2,"note":"...",
//    "sw":{"20":"Outhole", ...},                 // key = strobe*10+return, as the manual prints it
//    "c":[{"n":9,"f":"Outhole","s":[20,30,40],"p":"une bille dans l'outhole"},
//         {"n":8,"f":"Knocker","x":"ne déplace aucun switch"}]}
//   `s` = switches this coil should disturb, `p` = what the operator must set up first,
//   `x` = why this coil can never be checked this way (mutually exclusive with s/p).
//   {"alias":12} redirects a variant (speech vs sound-only) to the shared playfield table.
//
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include <Arduino.h>
#include <stdint.h>

namespace gamedata {

  // Load /gd-NN.json (following one level of "alias"). Returns false when the title has
  // no table yet — the common case for the 60-odd titles nobody has run this on, and NOT
  // an error: the coil test still works from what it learns, it just cannot name anything
  // or explain a precondition. Loading a new game replaces whatever was in memory.
  bool load(int game);
  void unload();
  int  loaded();              // game number in memory, -1 = none

  // --- language ---------------------------------------------------------------------
  // NAMES are NOT translated. A switch is "#1 Top Drop Target" in both languages because
  // that is the string printed in the game's own manual and on the schematic the operator
  // is holding: translating it would force them to translate back before they could find
  // it on the drawing, and would put a second, unverifiable wording between the machine
  // and its documentation. Only the OPERATOR PROSE is bilingual -- the preconditions and
  // the "why this can never be tested" reasons, which are ours, not Gottlieb's.
  void        setLang(const char* two);   // "fr" or "en"; anything else keeps the current one
  const char* lang();
  const char* title();        // "" when nothing is loaded
  const char* source();       // provenance, in the selected language when the file has both
  const char* note();         // per-title caveat (e.g. "hole kicker is on a lamp output")
  uint8_t     conf();         // 0 = nothing loaded, 1 = single source, 2 = cross-checked

  // Switch name for the manual's own numbering (strobe*10 + return). "" when unknown —
  // callers must render the bare number rather than invent a label.
  const char* swName(uint8_t id);

  // --- per coil, 1..9 --------------------------------------------------------------
  const char* coilFn(uint8_t c);      // "Reset banc de cibles HAUT", or "" if unknown
  // Why this coil can NEVER be verified by switch feedback (knocker, coin counter,
  // flasher). "" means it is expected to be observable. This is the honest blind-spot
  // list: an operator told "coil 8 is not testable, listen to it" will go and listen,
  // whereas a green tick on a dead knocker sends them away happy.
  const char* coilWhyNot(uint8_t c);
  // What the playfield must look like BEFORE the pulse for the test to prove anything,
  // in plain French ("au moins une cible du banc HAUT abaissée"). "" when not applicable.
  const char* coilPre(uint8_t c);
  // Switches this coil should disturb, as a bit per (strobe*8 + return) so it lines up
  // with coiltest's masks. 0 = no expectation on record.
  uint64_t    coilSw(uint8_t c);
  bool        hasExpectation(uint8_t c);   // coilSw(c) != 0

  // --- controlled lamps, L0..L47 ----------------------------------------------------
  // The manuals name the lamp-driver outputs too ("PLAYBOARD ILLUMINATION"), so the 48
  // anonymous squares in the lamp tab become readable. Worth more than cosmetics: on
  // several titles some of those outputs do not drive a LAMP at all but a real coil or
  // relay through a power transistor -- on Volcano L15 is the ball release, L16 the hole
  // kicker, L8 the fire pit, L14 the ball gate. Those mechanisms are invisible to the
  // 9-solenoid coil test, and an operator hunting a dead hole kicker needs to know it is
  // fired from the lamp tab. lampIsCoil() marks them.
  const char* lampName(uint8_t n);   // n = the manual's L number, 0..47 = the bit index
                                     // (measured on hardware: L12 is bit 12). "" if unknown
  bool        lampIsCoil(uint8_t n);

  // id (strobe*10+return) <-> bit index (strobe*8+return). 0xFF for a malformed id, so a
  // hand-edited file with "48" in it is dropped instead of aliasing onto another switch.
  uint8_t bitOfId(int id);
  uint8_t idOfBit(uint8_t bit);

  // Emit the {t:"names", sw:{}, lamp:{}, coil:{}} payload the web UI already knows how to
  // merge, so the labels land on the switch matrix, the lamp grid and the coil pad at once
  // rather than only inside the coil test. Keys match what the UI builds: switches by
  // strobe*10+return, lamps by BIT INDEX, which equals the manual's L number
  // (L12 -> "12", shown as square 13 in the UI), coils by 1..9.
  String namesJson();

  // Append "20 Outhole, 30 Couloir DROIT" for every bit in `mask` — used to build the
  // operator-facing sentence for a failed precondition.
  String describe(uint64_t mask);

} // namespace gamedata
