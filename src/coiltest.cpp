// coiltest.cpp — see coiltest.h for the idea. This file is the state machine.
// (C) 2026 Valere Pillet / Pstore. Original implementation.
#include "coiltest.h"
#include "fpgalink.h"
#include "gamedata.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

// lisyctrl registers we drive. Same numbers as diag.cpp; repeated here (rather than
// shared through a header) because diag.cpp keeps them as #defines in its own TU.
#define REG_SW_ROW0    0x10   // 0x10..0x17, bit = return, 1 = closed
#define REG_COIL       0x30   // write coil# 1..9 -> pulse; 0 = explicit off
#define REG_PULSE_MS   0x31
#define REG_COIL_FAULT 0x32   // R: b0 clamp, b1 refire-blocked, b2 wd-with-coil; b7..4 coil#

namespace coiltest {
namespace {

// ---------------------------------------------------------------------------
// TIMING. Every number below is read off GottFA80_PLuS/lib_common/lisyctrl.vhd,
// not estimated -- they are the generics the FPGA is actually compiled with:
//   refire_ms     = 40   min OFF time, armed when the pulse ENDS (pulse_left = 1)
//   max_pulse_ms  = 150  hard thermal ceiling; a bigger request is clamped AND
//                        latches COIL_FAULT b0
//   wd_timeout_ms = 120  with outputs armed, 120 ms of SPI silence trips the
//                        watchdog, kills the coil and latches COIL_FAULT b2
//   scan_div      = 5000 clk (~100 us) per strobe -> the whole matrix is refreshed
//                        every ~800 us, so a 4 ms sample period cannot alias past
//                        a closure that lasts longer than one scan.
// The two consequences that shape this file: we must never re-fire sooner than
// pulse + 40 ms (a premature fire is REFUSED, not merely delayed), and we must keep
// reading registers throughout -- including while waiting -- or the watchdog trips
// and latches a fault against whichever coil happens to be next.
// ---------------------------------------------------------------------------
constexpr uint16_t WIN_MS        = 400;  // observation window, and the idle control window
constexpr uint16_t SAMPLE_MS     = 4;    // full-matrix poll period (~100 samples/window)
constexpr uint8_t  REFIRE_MS     = 40;   // lisyctrl refire_ms
constexpr uint8_t  COOL_MARGIN   = 25;   // slack on top of pulse+refire, for ms-tick skew
constexpr uint8_t  SAFE_PULSE_MS = 150;  // lisyctrl max_pulse_ms — clamp OUR request so a
                                         // clamp fault always means the FPGA, never us
constexpr uint8_t  DEF_PULSE_MS  = 60;   // lisyctrl's own power-on default

// WHY UP TO 3 REPETITIONS, AND WHY THE COUNT IS NOT FIXED.
// The pollution to guard against is a direct-wired device (pop bumper, slingshot,
// flipper) firing on its own leaf switch while a window is open: it toggles a switch
// that has nothing to do with the coil under test. Call p the chance that a given
// foreign switch moves during one 400 ms window.
//   1 rep           -> that switch is accepted outright.            P(false) = p
//   3 reps, need 3  -> P(false) = p^3, but ONE missed rep on a genuine switch also
//                      destroys a correct signature.
//   3 reps, need 2  -> P(false) = 3p^2(1-p) + p^3 ~ 3p^2, and a real switch survives
//                      one missed rep.
// At a pessimistic p = 0.05 that is 0.75% instead of 5%. So repetition is worth having
// WHEN IT IS AVAILABLE.
// It usually is not. Every switch-observable coil on a pinball drives a ONE-SHOT
// mechanism: the reset lifts the drop targets, the kicker empties the saucer, the
// outhole coil ejects the ball. After pulse 1 the precondition is gone, so pulses 2 and
// 3 are guaranteed silences and a healthy coil scores 1/3 -- under LEARN_MIN, signature
// discarded. A fixed repetition count does not merely waste time here, it inverts the
// result. So LEARN re-derives the precondition from what pulse 1 actually did (see
// preArmed) and stops as soon as the playfield can no longer repeat that transition.
// The accept threshold then follows the repetitions that were really possible --
// min(LEARN_MIN, reps_done) -- and the honest denominator is reported, so the UI shows
// "1/1" rather than a fake "1/3" or a fake "3/3".
// A single repetition does lose the p^2 guard, which is why the idle CONTROL WINDOW that
// precedes every pulse matters more than ever: any switch that moves with no coil
// energised is blacklisted for that repetition, measuring the noise floor at test time
// instead of assuming one. And where the title has a game table (gamedata.h), a switch
// that also appears in the manual's own coil->switch table is corroborated by a source
// built by other people from other artefacts -- stronger evidence than three identical
// pulses on the same afternoon.
constexpr uint8_t  LEARN_REPS = 3;       // ceiling, not a quota
constexpr uint8_t  LEARN_MIN  = 2;
constexpr uint8_t  MAX_SIG_SW = 8;       // per coil; beyond this it is chatter, not mechanics

// --- verdicts ---------------------------------------------------------------
// V_PREREQ is the verdict this whole design turns on: the coil was NOT fired, or fired
// with nothing observable available, because the mechanism was already in its post-pulse
// state. It must never be presented like V_NOREACT. One says "prepare the playfield",
// the other says "this coil is broken", and confusing them is how a tool starts lying.
enum : uint8_t { V_NONE = 0, V_OK, V_PARTIAL, V_NOREACT, V_PREREQ, V_NOSIG, V_ABORT };
const char* vName(uint8_t v) {
  switch (v) {
    case V_OK:      return "ok";
    case V_PARTIAL: return "partiel";
    case V_NOREACT: return "muet";
    case V_PREREQ:  return "precond";
    case V_NOSIG:   return "nonteste";
    case V_ABORT:   return "abandon";
    default:        return "";
  }
}

// --- the learned table ------------------------------------------------------
struct Sig {
  bool     learned = false;      // this coil went through a LEARN pass
  uint64_t mask    = 0;          // switches accepted into the signature
  uint64_t closed  = 0;          // ...seen going open -> closed at least once
  uint64_t opened  = 0;          // ...seen going closed -> open at least once
  uint8_t  hit[N_SW] = {0};      // reps in which each switch reacted
  uint8_t  reps    = 0;          // repetitions that were actually POSSIBLE for this coil
                                 // (see the LEARN_REPS note): the honest denominator of
                                 // hit[], which for a one-shot mechanism is legitimately 1
};
Sig     g_sig[N_COIL + 1];       // 1-based; [0] unused
int     g_sigKey  = -1;          // key whose signatures are in memory, -1 = none
uint8_t g_sigReps = LEARN_REPS;  // run-wide ceiling the loaded file was learned with

// --- last run's results -----------------------------------------------------
struct Res {
  uint8_t  verdict = V_NONE;
  uint64_t seen    = 0;    // expected switches that DID react
  uint64_t missing = 0;    // expected switches that did not, precondition being satisfied
  uint64_t blocked = 0;    // expected switches that could not move from where they were
  uint64_t extra   = 0;    // switches that reacted but are not in the signature
  uint64_t confirm = 0;    // signature switches the game table also expects (corroborated)
  uint8_t  fault   = 0;    // COIL_FAULT latched during this coil's pulse(s)
  uint8_t  reps    = 0;    // repetitions actually run
  bool     fired   = false;// false when the precondition check refused to pulse at all
};
Res g_res[N_COIL + 1];

// --- run state --------------------------------------------------------------
// ST_ARM exists so that start() -- which is called from the AsyncTCP task, both by the
// WebSocket handler and by GET /coiltest -- touches NO SPI at all. It validates, latches
// the parameters and hands over; the first tick() on loopTask does the opening register
// writes. Same contract as net.cpp's job slot: the handler returns immediately and every
// byte on the shared bus is issued from one task.
enum : uint8_t { ST_IDLE = 0, ST_ARM, ST_QUIET, ST_FIRE, ST_WATCH, ST_SCORE, ST_COOL, ST_DONE };
Bus      g_bus     = {nullptr, nullptr, nullptr};
uint8_t  g_state   = ST_IDLE;
bool     g_learn   = false;
bool     g_abort   = false;
int      g_key     = KEY_GENERIC;
uint8_t  g_pulse   = DEF_PULSE_MS;
uint8_t  g_coil    = 1;
uint8_t  g_rep     = 0;
uint8_t  g_reps    = 1;
uint32_t g_stateMs = 0;      // millis() when the current window opened
uint32_t g_fireMs  = 0;      // millis() of the pulse command
uint32_t g_sampMs  = 0;      // millis() of the last matrix sample
uint32_t g_startMs = 0;
uint32_t g_runMs   = 0;      // duration of the last completed run
char     g_err[80] = "";     // why the run stopped early (empty = clean)

uint8_t  g_base[8] = {0};    // matrix reference for the window in progress
uint64_t g_react = 0, g_close = 0, g_open = 0;   // accumulated over the watch window
uint64_t g_accClose = 0, g_accOpen = 0;          // ...accumulated across this coil's reps
uint64_t g_noise = 0;                            // from the idle control window
uint8_t  g_hit[N_SW] = {0};                      // tally for the coil being learned
uint8_t  g_fault = 0;                            // COIL_FAULT seen for the coil in progress
uint8_t  g_done  = 0;                            // repetitions completed for this coil
uint16_t g_only  = 0;                            // 0 = every coil, else bit0 = coil 1
uint64_t g_armed = 0;                            // signature switches whose precondition held

// ---------------------------------------------------------------------------
inline uint8_t swId(uint8_t bit) { return (uint8_t)((bit >> 3) * 10 + (bit & 7)); }  // matrix UI numbering

void readMatrix(uint8_t out[8]) {
  for (uint8_t s = 0; s < 8; s++) out[s] = g_bus.rd((uint8_t)(REG_SW_ROW0 + s));
}
// Fold one sample against the window's reference into the change/direction masks.
void fold(const uint8_t now[8], const uint8_t ref[8],
          uint64_t& chg, uint64_t* closed, uint64_t* opened) {
  for (uint8_t s = 0; s < 8; s++) {
    uint8_t d = (uint8_t)(now[s] ^ ref[s]);
    if (!d) continue;
    for (uint8_t r = 0; r < 8; r++) {
      if (!(d & (1u << r))) continue;
      uint64_t b = 1ULL << (s * 8 + r);
      chg |= b;
      if (now[s] & (1u << r)) { if (closed) *closed |= b; }   // ref open   -> now closed
      else                    { if (opened) *opened |= b; }   // ref closed -> now open
    }
  }
}

void coilOff() { if (g_bus.wr) g_bus.wr(REG_COIL, 0); }   // 0 = explicit off, never faults
void setErr(const char* m) { strncpy(g_err, m, sizeof g_err - 1); g_err[sizeof g_err - 1] = 0; }

// Pack a matrix sample into one bit per switch, 1 = closed.
uint64_t closedMask(const uint8_t m[8]) {
  uint64_t v = 0;
  for (uint8_t s = 0; s < 8; s++) v |= (uint64_t)m[s] << (s * 8);
  return v;
}

// THE PRECONDITION CHECK.
// `mask` is the set of switches we expect to move; `wasClosed`/`wasOpen` say which way each
// of them was seen to go (wasOpen = "found closed, ended open", and vice versa). A switch
// can only repeat that transition if it is back where it started, so:
//   seen only opening  -> it must currently be CLOSED
//   seen only closing  -> it must currently be OPEN
//   seen going both ways over several reps -> ambiguous, demand nothing and just watch it
// Returns the subset that CAN still move. An empty result means firing would prove
// nothing: the mechanism is already in its post-pulse state.
uint64_t preArmed(const uint8_t now[8], uint64_t mask, uint64_t wasClosed, uint64_t wasOpen) {
  uint64_t cur       = closedMask(now);
  uint64_t needClose = wasOpen & ~wasClosed;    // must be closed now to be able to open
  uint64_t needOpen  = wasClosed & ~wasOpen;    // must be open now to be able to close
  uint64_t ambiguous = mask & ~(needClose | needOpen);
  return mask & ((needClose & cur) | (needOpen & ~cur) | ambiguous);
}

const char* sigPath(int key, char* buf, size_t n) {
  snprintf(buf, n, "/coilsig-%02d.json", key);
  return buf;
}

void clearSigs() {
  for (uint8_t c = 0; c <= N_COIL; c++) g_sig[c] = Sig();
  g_sigReps = LEARN_REPS;
  g_sigKey  = -1;
}

void saveSigs(int key) {
  JsonDocument d;
  d["v"] = 1; d["key"] = key; d["pulse"] = g_pulse;
  d["reps"] = LEARN_REPS; d["min"] = LEARN_MIN; d["win"] = WIN_MS;
  JsonArray a = d["c"].to<JsonArray>();
  for (uint8_t c = 1; c <= N_COIL; c++) {
    if (!g_sig[c].learned) continue;
    JsonObject o = a.add<JsonObject>(); o["n"] = c;
    o["r"] = g_sig[c].reps;      // per-coil denominator: 1 for a one-shot mechanism
    JsonArray s = o["s"].to<JsonArray>();
    for (uint8_t b = 0; b < N_SW; b++) {
      if (!(g_sig[c].mask & (1ULL << b))) continue;
      JsonObject e = s.add<JsonObject>();
      e["i"] = swId(b); e["h"] = g_sig[c].hit[b];
      e["d"] = (uint8_t)(((g_sig[c].closed >> b) & 1) | (((g_sig[c].opened >> b) & 1) << 1));
    }
  }
  char p[32]; sigPath(key, p, sizeof p);
  File f = LittleFS.open(p, "w");
  if (!f) { Serial.printf("[coil] cannot write %s\n", p); setErr("écriture LittleFS impossible"); return; }
  serializeJson(d, f); f.close();
  g_sigKey = key;
  Serial.printf("[coil] signatures saved -> %s\n", p);
}

// swId -> bit index, or 0xFF when the file holds something that is not strobe*10+return.
uint8_t bitOfId(int id) {
  int s = id / 10, r = id % 10;
  if (id < 0 || s > 7 || r > 7) return 0xFF;
  return (uint8_t)(s * 8 + r);
}

void addSw(JsonArray a, uint64_t mask, const Sig* s) {
  for (uint8_t b = 0; b < N_SW; b++) {
    if (!(mask & (1ULL << b))) continue;
    JsonObject o = a.add<JsonObject>();
    uint8_t id = swId(b);
    o["i"] = id;
    // The name turns "muet: 22 manquant" into "muet: cible tombante HAUT #3 n'a pas bougé",
    // which is the difference between a number to look up and an instruction to act on.
    const char* nm = gamedata::swName(id);
    if (nm[0]) o["nm"] = nm;
    if (s) {
      o["h"] = s->hit[b];
      // Direction, and therefore the state the switch must be in before the next pulse:
      // 1 = was seen closing (so it must be OPEN to move), 2 = was seen opening (must be
      // CLOSED), 3 = both, no requirement.
      o["d"] = (uint8_t)(((s->closed >> b) & 1) | (((s->opened >> b) & 1) << 1));
    }
  }
}

// Does TEST have anything to check on this coil? (LEARN always fires: that is how we
// find out.) Skipping the unsignatured ones keeps a TEST short and stops it banging
// the knocker for no information.
bool testable(uint8_t c) { return g_sig[c].learned && g_sig[c].mask != 0; }

// Why a coil has no signature -- and these are NOT the same thing:
//   * the knocker, a coin counter, a flasher will NEVER move a switch. "non testable" is
//     the final answer and the operator should go and listen to it instead.
//   * a subway kicker with no ball in the subway has no signature YET. Calling that "non
//     testable" tells the operator to stop trying, when all it needs is a ball -- the same
//     class of mistake as calling it "muette", just quieter.
// The game table separates them: `x` means structurally blind, `s` means something was
// expected. Observed on the bench 2026-07-30, where coils 1, 5 and 9 read "non testable"
// after a LEARN that had simply never had a ball in front of them.
uint8_t classifyUnlearned(uint8_t c) {
  if (gamedata::coilWhyNot(c)[0]) return V_NOSIG;    // will never move a switch
  if (gamedata::coilSw(c))        return V_PREREQ;   // expected to; just never got the chance
  return V_NOSIG;                                   // no table: nothing honest to say
}

// Fold the finished repetitions into a signature (LEARN) or a verdict (TEST).
void finishCoil() {
  Sig& s = g_sig[g_coil];
  Res& r = g_res[g_coil];
  r.fault = g_fault;
  r.reps  = g_done;
  const uint64_t expect = gamedata::coilSw(g_coil);   // 0 when the title has no table

  if (g_learn) {
    uint64_t close = g_accClose, open = g_accOpen;
    uint8_t  done  = g_done ? g_done : 1;
    // A LEARN that observes NOTHING must never erase what a previous LEARN measured. Without
    // this, re-running the learn on a coil whose mechanism happens to be in its post-pulse
    // state (drop targets already up, no ball in the saucer) overwrites a good signature with
    // an empty one -- the good measurement destroyed by a badly-prepared re-run, which is the
    // same failure this whole file exists to prevent, just one level up.
    bool nothing = true;
    for (uint8_t b = 0; b < N_SW && nothing; b++) if (g_hit[b]) nothing = false;
    if (nothing && s.learned && s.mask) {
      r.verdict = V_PREREQ;
      r.blocked = s.mask;          // what it should have moved, from the signature we keep
      r.seen    = s.mask;
      r.confirm = s.mask & expect;
      Serial.printf("[coil] learn %u : rien vu, signature précédente CONSERVÉE\n", (unsigned)g_coil);
      return;
    }
    // The threshold follows what was actually achievable. Demanding 2 hits from a coil
    // that could only be fired once is what discarded every one-shot mechanism's
    // signature; see the LEARN_REPS note.
    uint8_t  need0 = (uint8_t)(done < LEARN_MIN ? done : LEARN_MIN);
    s = Sig(); s.learned = true; s.reps = done;
    uint8_t taken = 0;
    // Accept in descending confidence, so that a coil which somehow disturbs more than
    // MAX_SIG_SW switches keeps the most repeatable ones rather than the lowest-numbered.
    for (int nd = done; nd >= need0 && taken < MAX_SIG_SW; nd--) {
      for (uint8_t b = 0; b < N_SW && taken < MAX_SIG_SW; b++) {
        if (g_hit[b] != nd) continue;
        s.mask |= (1ULL << b); s.hit[b] = (uint8_t)nd; taken++;
      }
    }
    s.closed  = close & s.mask;
    s.opened  = open  & s.mask;
    r.seen    = s.mask;
    r.confirm = s.mask & expect;          // switches the manual's table expects too
    if (s.mask) {
      r.verdict = V_OK;
    } else if (expect) {
      // Learning found nothing, but the game table says this coil MOVES something. Either
      // the coil is dead or the mechanism was already in its post-pulse state, and no
      // amount of pulsing tells those apart -- so ask, and never record "no signature",
      // which would permanently mark a healthy coil untestable.
      r.verdict = V_PREREQ;
      r.blocked = expect;
    } else {
      r.verdict = V_NOSIG;                // no reaction and nothing expected: a RESULT
    }
    Serial.printf("[coil] learn %u -> %s (%u rep%s)\n", (unsigned)g_coil,
                  s.mask ? "signature" : (expect ? "rien vu alors qu'attendu" : "aucune signature"),
                  (unsigned)done, done > 1 ? "s" : "");
  } else if (!testable(g_coil)) {
    r.verdict = classifyUnlearned(g_coil);
    if (r.verdict == V_PREREQ) r.blocked = expect;
  } else if (!r.fired) {
    // The precondition gate refused to pulse: nothing was proved and nothing was consumed.
    r.verdict = V_PREREQ;
    r.blocked = s.mask;
  } else {
    // Only the switches whose precondition held can be held against the coil. The rest
    // were physically unable to move and are reported separately, not as "missing" --
    // that distinction is the difference between a diagnosis and a false accusation.
    r.seen    = g_react & g_armed;
    r.missing = g_armed & ~g_react;
    r.blocked = s.mask  & ~g_armed;
    r.extra   = g_react & ~s.mask;
    r.confirm = s.mask  & expect;
    // NEVER say "muet" while part of the signature was physically unable to move.
    // preArmed() is a UNION: it arms a coil as soon as ONE of its switches could travel, and
    // the gate above only refuses to fire when the WHOLE signature is stuck. That leaves the
    // partially-blocked case firing — and a mixed-direction signature is the NORM, not an
    // edge case: Volcano's outhole coil opens switch 20 and closes 30/40, so with the outhole
    // empty and the trough empty, 30/40 read as armable, the coil fires into nothing, and a
    // perfectly healthy coil scored V_NOREACT in red. Found by adversarial review of this
    // file on 2026-07-30 with that exact sequence; r.blocked was already being computed one
    // line above and simply never read.
    r.verdict = r.seen ? (r.missing ? V_PARTIAL : V_OK)
                       : (r.blocked ? V_PREREQ : V_NOREACT);
  }
}

// --- which title are we on? -------------------------------------------------------------
// fpgalink::gameNo() is -1 until the FPGA announces a game, and on a board whose
// game_select never moves after reset it stays -1 FOR EVER: sound_link.vhd latches the
// value at reset and only emits the token on a LATER change (fpgalink.h says so). That is
// the normal state of a machine that has merely been switched on, and it was leaving the
// board on the generic slot with no game table at all -- observed on the bench 2026-07-30,
// where a freshly powered Volcano reported key 99 and got no names and no preconditions.
// So the operator can name the title and the choice is remembered. Precedence: whatever the
// FPGA reports wins, because it cannot be wrong about its own DIP switches; the manual
// override only fills the silence.
constexpr const char* KEYFILE = "/gamekey.txt";
int g_override = -1;      // -1 = none, else 0..62

void loadOverride() {
  File f = LittleFS.open(KEYFILE, "r");
  if (!f) return;
  int v = f.parseInt();
  f.close();
  if (v >= 0 && v < 63) { g_override = v; Serial.printf("[coil] jeu forcé (mémorisé) = %d\n", v); }
}

// Is this coil part of the requested run? `only` = 0 means all of them.
bool selected(uint8_t c) { return !g_only || (g_only & (1u << (c - 1))); }

// Set up the next coil, or finish the run. Returns false when the run is over.
bool enterCoil(uint32_t now) {
  while (g_coil <= N_COIL) {
    if (selected(g_coil) && (g_learn || testable(g_coil))) {
      g_rep = g_done = 0;
      memset(g_hit, 0, sizeof g_hit);
      g_react = g_close = g_open = g_noise = 0;
      g_accClose = g_accOpen = g_armed = 0; g_fault = 0;
      g_stateMs = g_sampMs = now;
      readMatrix(g_base);                       // reference for the first window
      g_state = g_learn ? ST_QUIET : ST_FIRE;
      return true;
    }
    // Not in this run, or nothing to look at: leave the previous verdict alone when the
    // operator asked for a subset, so a targeted retry does not blank the coils that
    // already passed and force the whole playfield to be set up again.
    if (selected(g_coil)) {
      g_res[g_coil].verdict = classifyUnlearned(g_coil);
      if (g_res[g_coil].verdict == V_PREREQ) g_res[g_coil].blocked = gamedata::coilSw(g_coil);
    }
    g_coil++;
  }
  coilOff();
  if (g_learn) saveSigs(g_key);
  g_runMs = now - g_startMs;
  g_state = ST_DONE;
  Serial.printf("[coil] %s done in %u ms\n", g_learn ? "LEARN" : "TEST", (unsigned)g_runMs);
  return false;
}

} // anonymous namespace

// ===========================================================================

void begin(const Bus& b) {
  g_bus = b;
  clearSigs();
  loadOverride();          // before keyFor(), which consults it
  load(keyFor());          // signatures + game table; both absent is normal, not an error
}

int keyFor() {
  int g = fpgalink::gameNo();
  if (g >= 0 && g < 64) return g;
  if (g_override >= 0)  return g_override;
  return KEY_GENERIC;
}

bool setGame(int key) {
  if (busy()) return false;
  if (key < 0 || key > 62) {                    // out of range = clear the override
    g_override = -1;
    LittleFS.remove(KEYFILE);
    Serial.println("[coil] jeu forcé effacé");
  } else {
    g_override = key;
    File f = LittleFS.open(KEYFILE, "w");
    if (f) { f.print(key); f.close(); }
    else Serial.println("[coil] impossible de mémoriser le jeu forcé");
    Serial.printf("[coil] jeu forcé = %d\n", key);
  }
  load(keyFor());        // signatures + game table for the slot we are now on
  return true;
}
int  gameOverride() { return g_override; }
int loadedKey() { return g_sigKey; }

bool load(int key) {
  clearSigs();
  // The game table travels with the slot: switching slots without switching tables would
  // label one title's signature with another title's switch names, which reads as
  // authoritative and is wrong. Absent for most titles, which is fine (see gamedata.h).
  gamedata::load(key);
  char p[32]; sigPath(key, p, sizeof p);
  File f = LittleFS.open(p, "r");
  if (!f) return false;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) { Serial.printf("[coil] %s unreadable (%s)\n", p, e.c_str()); return false; }
  g_sigReps = d["reps"] | LEARN_REPS;
  for (JsonObject o : d["c"].as<JsonArray>()) {
    int c = o["n"] | 0;
    if (c < 1 || c > N_COIL) continue;
    g_sig[c].learned = true;                          // present in the file = went through
    // "r" is absent in files written before repetitions became adaptive; those were all
    // learned at the fixed ceiling, so falling back to it keeps their hit counts honest.
    g_sig[c].reps = (uint8_t)(o["r"] | (int)g_sigReps);
    for (JsonObject s : o["s"].as<JsonArray>()) {     // LEARN, even with an empty list
      uint8_t b = bitOfId(s["i"] | -1);
      if (b == 0xFF) continue;
      uint8_t dir = s["d"] | 0;
      g_sig[c].mask |= (1ULL << b);
      g_sig[c].hit[b] = (uint8_t)(s["h"] | 0);
      if (dir & 1) g_sig[c].closed |= (1ULL << b);
      if (dir & 2) g_sig[c].opened |= (1ULL << b);
    }
  }
  g_sigKey = key;
  Serial.printf("[coil] signatures loaded from %s\n", p);
  return true;
}

bool busy() { return g_state != ST_IDLE && g_state != ST_DONE; }

uint16_t inconclusiveMask() {
  uint16_t m = 0;
  for (uint8_t c = 1; c <= N_COIL; c++)
    if (g_res[c].verdict == V_PREREQ) m = (uint16_t)(m | (1u << (c - 1)));
  return m;
}

const char* start(bool learn, int key, uint8_t pulseMs, uint16_t only) {
  if (busy())                    return "un test de bobines est déjà en cours";
  if (!g_bus.rd || !g_bus.ready) return "pont SPI non initialisé";
  // The gate the brief is emphatic about: refuse loudly, never do nothing in silence.
  if (!g_bus.ready())
    return "mode diag inactif ou sorties non armées — appui long sur le bouton TEST "
           "de la portière, puis « mode contrôle »";
  if (key < 0 || key > KEY_GENERIC) return "numéro de jeu hors plage";

  if (!pulseMs) pulseMs = DEF_PULSE_MS;
  if (pulseMs > SAFE_PULSE_MS) pulseMs = SAFE_PULSE_MS;   // never provoke COIL_FAULT b0 ourselves

  only &= (uint16_t)((1u << N_COIL) - 1);      // bits above coil 9 cannot select anything

  // A subset LEARN would silently drop every other coil's signature, because saveSigs()
  // writes whatever is in memory. Reload first so the untouched coils survive the save.
  if (learn) {
    if (only) { if (g_sigKey != key) load(key); }
    else clearSigs();
  } else {
    if (g_sigKey != key && !load(key))
      return "aucune signature pour ce jeu — lance d'abord l'apprentissage";
    bool any = false;
    for (uint8_t c = 1; c <= N_COIL; c++) if (testable(c) && (!only || (only & (1u << (c - 1))))) any = true;
    if (!any) return only ? "aucune des bobines demandées n'a de signature"
                          : "aucune bobine testable pour ce jeu — relance l'apprentissage";
  }

  // A targeted retry must keep the verdicts it is not re-running, otherwise the operator
  // loses the results they just earned and has to set the whole playfield up again.
  for (uint8_t c = 0; c <= N_COIL; c++)
    if (!only || c == 0 || (only & (1u << (c - 1)))) g_res[c] = Res();

  g_learn = learn; g_key = key; g_pulse = pulseMs; g_only = only;
  g_reps  = learn ? LEARN_REPS : 1;
  g_coil  = 1; g_rep = 0; g_done = 0; g_abort = false; g_err[0] = 0; g_runMs = 0;
  gamedata::load(key);           // names + the manual's coil->switch expectations, if we have them
  g_startMs = g_stateMs = g_sampMs = millis();
  g_state = ST_ARM;              // the first tick(), on loopTask, opens the bus traffic
  Serial.printf("[coil] %s armed: key=%d pulse=%ums reps<=%u only=0x%03X\n",
                learn ? "LEARN" : "TEST", key, (unsigned)g_pulse, (unsigned)g_reps,
                (unsigned)g_only);
  return nullptr;
}

void abort() { if (busy()) g_abort = true; }

void tick() {
  if (g_state == ST_IDLE || g_state == ST_DONE) return;
  uint32_t now = millis();

  // Re-checked every pass, not only at start(): leaving diag mode or disarming the
  // outputs mid-run must stop the run loudly, not leave it pulsing into a dead bus.
  if (!g_bus.ready()) { setErr("mode diag ou sorties perdus pendant le test"); g_abort = true; }

  if (g_abort) {
    coilOff();
    if (!g_err[0]) setErr("interrompu par l'opérateur");
    for (uint8_t c = g_coil; c <= N_COIL; c++)
      if (g_res[c].verdict == V_NONE) g_res[c].verdict = V_ABORT;
    g_runMs = now - g_startMs;
    g_state = ST_DONE;
    Serial.printf("[coil] aborted: %s\n", g_err);
    return;
  }

  // One matrix sample per SAMPLE_MS, in EVERY active state. Besides feeding the windows,
  // this is what pets lisyctrl's 120 ms output watchdog while we wait out a cooldown --
  // a silent wait would trip it and latch a bogus fault against the next coil.
  bool sampled = false;
  uint8_t m[8];
  if (now - g_sampMs >= SAMPLE_MS) { g_sampMs = now; readMatrix(m); sampled = true; }

  switch (g_state) {
    case ST_ARM:                                    // first pass on loopTask: open the run
      g_bus.wr(REG_COIL_FAULT, 0x00);               // clean latch
      g_bus.wr(REG_PULSE_MS, g_pulse);
      g_startMs = now;
      enterCoil(now);
      break;

    case ST_QUIET:                                  // idle control window (LEARN only)
      if (sampled) fold(m, g_base, g_noise, nullptr, nullptr);
      if (now - g_stateMs >= WIN_MS) g_state = ST_FIRE;
      break;

    case ST_FIRE: {
      readMatrix(g_base);                           // reference = the instant before the pulse
      g_react = g_close = g_open = 0;
      // THE GATE. On a TEST we know from LEARN which way each signature switch travels, so
      // we can tell beforehand whether this pulse could possibly show anything. If not, do
      // not fire: pulsing would consume nothing (the mechanism is already there), waste a
      // 40 ms refire lockout, and -- the part that matters -- produce a silence that reads
      // as a dead coil. Refusing to fire and saying why is the honest answer.
      if (!g_learn) {
        const Sig& s = g_sig[g_coil];
        g_armed = preArmed(g_base, s.mask, s.closed, s.opened);
        if (!g_armed) {
          g_res[g_coil].fired = false;
          finishCoil();
          g_coil++;
          enterCoil(now);
          break;
        }
      }
      g_res[g_coil].fired = true;
      g_bus.wr(REG_COIL_FAULT, 0x00);               // so the fault we read back is THIS pulse's
      g_bus.wr(REG_PULSE_MS, g_pulse);
      g_bus.wr(REG_COIL, g_coil);
      g_fireMs = g_stateMs = g_sampMs = now;
      g_state = ST_WATCH;
      break;
    }

    case ST_WATCH:
      if (sampled) fold(m, g_base, g_react, &g_close, &g_open);
      if (now - g_stateMs >= WIN_MS) g_state = ST_SCORE;
      break;

    case ST_SCORE: {
      uint8_t f = g_bus.rd(REG_COIL_FAULT);
      if (f) g_fault = f;                           // lisyctrl overwrites, so keep the last set one
      g_done++;
      if (g_learn) {
        uint64_t cand = g_react & ~g_noise;         // minus whatever moved with no coil energised
        for (uint8_t b = 0; b < N_SW; b++) if (cand & (1ULL << b)) g_hit[b]++;
        g_accClose |= g_close & ~g_noise;
        g_accOpen  |= g_open  & ~g_noise;
      }
      g_state = ST_COOL; g_stateMs = now;
      break;
    }

    case ST_COOL:
      // lisyctrl arms refire_cnt when the pulse ENDS, so the earliest legal next fire is
      // fire + pulse + refire_ms. Firing sooner is REFUSED (COIL_FAULT b1), not delayed --
      // and a refused fire reads as "bobine muette", blaming the machine for our impatience.
      if (now - g_fireMs < (uint32_t)g_pulse + REFIRE_MS + COOL_MARGIN) break;
      if (++g_rep < g_reps) {                       // another repetition of the same coil
        readMatrix(g_base);
        // ADAPTIVE REPETITION. A drop-target reset, a saucer kickout and an outhole coil
        // are all one-shot: the pulse we just fired consumed its own precondition, so
        // repeating would record a guaranteed silence and drag a healthy coil below the
        // accept threshold. Re-derive the precondition from what this coil has actually
        // been seen to do and stop the moment it can no longer do it again -- then
        // finishCoil() reports the repetitions that were possible, not a fake quota.
        if (g_learn && (g_accClose | g_accOpen)) {
          uint64_t again = preArmed(g_base, g_accClose | g_accOpen, g_accClose, g_accOpen);
          if (!again) {
            Serial.printf("[coil] %u: mécanisme consommé, arrêt à %u rép.\n",
                          (unsigned)g_coil, (unsigned)g_done);
            finishCoil();
            g_coil++;
            enterCoil(now);
            break;
          }
        }
        g_noise = 0;
        g_stateMs = now;
        g_state = g_learn ? ST_QUIET : ST_FIRE;
        break;
      }
      finishCoil();
      g_coil++;
      enterCoil(now);
      break;

    default: break;
  }
}

String statusJson() {
  JsonDocument d;
  d["t"]     = "coiltest";
  d["run"]   = busy() ? 1 : 0;
  d["mode"]  = g_learn ? "learn" : "test";
  d["key"]   = g_key;
  d["live"]  = keyFor();                                 // slot the FPGA says we are on now
  // 1 = we do not know the title AT ALL. Not the same as "the FPGA stayed silent": once the
  // operator has named the game, the slot is a real title even though the FPGA said nothing,
  // and calling that "profil générique" would tell them their choice had been ignored.
  d["gen"]   = (keyFor() == KEY_GENERIC) ? 1 : 0;
  d["sigk"]  = g_sigKey;
  d["coil"]  = busy() ? g_coil : 0;
  d["rep"]   = busy() ? (g_rep + 1) : 0;
  d["reps"]  = g_reps;
  d["pulse"] = g_pulse;
  d["win"]   = WIN_MS;
  d["lreps"] = LEARN_REPS;
  d["lmin"]  = LEARN_MIN;
  d["sreps"] = g_sigReps;   // reps the LOADED signature was built with — the honest
                            // denominator for its per-switch hit counts, which is not
                            // necessarily today's LEARN_REPS if the file is older
  d["ms"]    = g_runMs;
  d["err"]   = (const char*)g_err;
  d["ready"] = (g_bus.ready && g_bus.ready()) ? 1 : 0;
  {   // progress = coils finished + the fraction of the current coil's repetitions.
      // Counted over the SELECTED coils only: on a "retry the inconclusive ones" run of
      // three coils, dividing by nine would stall the bar at 33 % and read as a hang.
    uint16_t nsel = 0, before = 0;
    for (uint8_t c = 1; c <= N_COIL; c++) {
      if (!selected(c)) continue;
      nsel++;
      if (c < g_coil) before++;
    }
    uint16_t steps = (uint16_t)(before * g_reps + g_rep);
    uint16_t total = (uint16_t)(nsel * g_reps);
    d["pct"] = busy() ? (int)(100UL * steps / (total ? total : 1))
                      : (g_state == ST_DONE ? 100 : 0);
  }
  d["only"] = g_only;
  d["inc"]  = inconclusiveMask();      // coils the operator can retry after rearranging
  d["ovr"]  = g_override;              // -1 = the slot came from the FPGA, not from a human
  d["fpga"] = fpgalink::gameNo();      // what the FPGA actually said, -1 = never said anything
  {   // Which title's table we are using, and how much it can be trusted. Provenance
      // travels with the data: a diagnosis on someone's machine should never be anonymous.
    JsonObject g = d["gd"].to<JsonObject>();
    g["g"]    = gamedata::loaded();
    g["t"]    = gamedata::title();
    g["conf"] = gamedata::conf();
    g["src"]  = gamedata::source();
    g["note"] = gamedata::note();
  }
  JsonArray a = d["c"].to<JsonArray>();
  for (uint8_t c = 1; c <= N_COIL; c++) {
    JsonObject o = a.add<JsonObject>();
    o["n"] = c;
    o["l"] = g_sig[c].learned ? 1 : 0;
    o["r"] = g_sig[c].reps;            // honest denominator for the hit counts below
    // "fn", not "f": "f" is already the COIL_FAULT object below, and quietly reusing it
    // would make a coil name and an FPGA fault the same field.
    { const char* f = gamedata::coilFn(c);     if (f[0]) o["fn"] = f; }
    { const char* x = gamedata::coilWhyNot(c); if (x[0]) o["x"]  = x; }
    { const char* p = gamedata::coilPre(c);    if (p[0]) o["pre"] = p; }
    addSw(o["sig"].to<JsonArray>(), g_sig[c].mask, &g_sig[c]);
    // What the manual says this coil should move, whether or not we have learned it. With
    // no signature yet this is the only thing that can guide the operator, and after a
    // LEARN it is the independent check on what we measured.
    if (uint64_t e = gamedata::coilSw(c)) addSw(o["exp"].to<JsonArray>(), e, nullptr);
    o["v"] = vName(g_res[c].verdict);
    o["rr"]   = g_res[c].reps;
    o["fired"] = g_res[c].fired ? 1 : 0;
    if (g_res[c].confirm) addSw(o["conf"].to<JsonArray>(), g_res[c].confirm, nullptr);
    if (g_res[c].missing) addSw(o["miss"].to<JsonArray>(), g_res[c].missing, nullptr);
    if (g_res[c].blocked) addSw(o["blk"].to<JsonArray>(),  g_res[c].blocked, nullptr);
    if (g_res[c].extra)   addSw(o["xtra"].to<JsonArray>(), g_res[c].extra,   nullptr);
    if (g_res[c].fault) {
      JsonObject f = o["f"].to<JsonObject>();
      f["raw"]    = g_res[c].fault;
      f["clamp"]  = (g_res[c].fault & 1) ? 1 : 0;
      f["refire"] = (g_res[c].fault & 2) ? 1 : 0;
      f["wd"]     = (g_res[c].fault & 4) ? 1 : 0;
    }
  }
  String s; serializeJson(d, s); return s;
}

} // namespace coiltest
