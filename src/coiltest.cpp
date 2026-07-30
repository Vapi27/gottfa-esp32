// coiltest.cpp — see coiltest.h for the idea. This file is the state machine.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#include "coiltest.h"
#include "fpgalink.h"
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

// WHY 3 REPETITIONS, ACCEPT AT 2.
// The pollution this guards against is a direct-wired device (pop bumper, slingshot,
// flipper) firing on its own leaf switch while a window is open: it toggles a switch
// that has nothing to do with the coil under test. Call p the chance that a given
// foreign switch moves during one 400 ms window.
//   1 rep           -> that switch is accepted outright.            P(false) = p
//   3 reps, need 3  -> P(false) = p^3, but ONE missed rep on a genuine switch also
//                      destroys a correct signature.
//   3 reps, need 2  -> P(false) = 3p^2(1-p) + p^3 ~ 3p^2, and a real switch survives
//                      one missed rep.
// At a pessimistic p = 0.05 that is 0.75% instead of 5%, while staying tolerant of the
// real mechanism being marginal -- which is exactly the failure we are hunting. Five
// reps buy one more decimal place and cost 40% more time on a job the operator runs
// once per machine; 3/2 is the knee. The per-switch hit count is kept and reported
// ("3/3" vs "2/3") so a marginal accept is visible instead of implied.
// On top of that, every repetition runs an IDLE CONTROL WINDOW of the same length
// before the pulse: any switch that moves with no coil energised is blacklisted for
// that repetition. That measures the noise floor at test time instead of assuming one.
constexpr uint8_t  LEARN_REPS = 3;
constexpr uint8_t  LEARN_MIN  = 2;
constexpr uint8_t  MAX_SIG_SW = 8;       // per coil; beyond this it is chatter, not mechanics

// --- verdicts ---------------------------------------------------------------
enum : uint8_t { V_NONE = 0, V_OK, V_PARTIAL, V_NOREACT, V_NOSIG, V_ABORT };
const char* vName(uint8_t v) {
  switch (v) {
    case V_OK:      return "ok";
    case V_PARTIAL: return "partiel";
    case V_NOREACT: return "muet";
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
  uint8_t  hit[N_SW] = {0};      // reps in which each switch reacted (0..LEARN_REPS)
};
Sig     g_sig[N_COIL + 1];       // 1-based; [0] unused
int     g_sigKey  = -1;          // key whose signatures are in memory, -1 = none
uint8_t g_sigReps = LEARN_REPS;  // reps the loaded signature was learned with

// --- last run's results -----------------------------------------------------
struct Res {
  uint8_t  verdict = V_NONE;
  uint64_t seen    = 0;    // expected switches that DID react
  uint64_t missing = 0;    // expected switches that did not
  uint64_t extra   = 0;    // switches that reacted but are not in the signature
  uint8_t  fault   = 0;    // COIL_FAULT latched during this coil's pulse(s)
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
uint64_t g_noise = 0;                            // from the idle control window
uint8_t  g_hit[N_SW] = {0};                      // tally for the coil being learned
uint8_t  g_fault = 0;                            // COIL_FAULT seen for the coil in progress

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
    o["i"] = swId(b);
    if (s) {
      o["h"] = s->hit[b];
      o["d"] = (uint8_t)(((s->closed >> b) & 1) | (((s->opened >> b) & 1) << 1));
    }
  }
}

// Does TEST have anything to check on this coil? (LEARN always fires: that is how we
// find out.) Skipping the unsignatured ones keeps a TEST short and stops it banging
// the knocker for no information.
bool testable(uint8_t c) { return g_sig[c].learned && g_sig[c].mask != 0; }

// Fold the finished repetitions into a signature (LEARN) or a verdict (TEST).
void finishCoil() {
  Sig& s = g_sig[g_coil];
  Res& r = g_res[g_coil];
  r.fault = g_fault;

  if (g_learn) {
    uint64_t close = g_close, open = g_open;
    s = Sig(); s.learned = true;
    uint8_t taken = 0;
    // Accept in descending confidence, so that a coil that somehow disturbs more than
    // MAX_SIG_SW switches keeps the most repeatable ones rather than the lowest-numbered.
    for (int need = LEARN_REPS; need >= LEARN_MIN && taken < MAX_SIG_SW; need--) {
      for (uint8_t b = 0; b < N_SW && taken < MAX_SIG_SW; b++) {
        if (g_hit[b] != need) continue;
        s.mask |= (1ULL << b); s.hit[b] = (uint8_t)need; taken++;
      }
    }
    s.closed = close & s.mask;
    s.opened = open  & s.mask;
    r.verdict = s.mask ? V_OK : V_NOSIG;      // "learned nothing" is a RESULT, not a failure
    r.seen    = s.mask;
    Serial.printf("[coil] learn %u -> %s\n", (unsigned)g_coil,
                  s.mask ? "signature" : "aucune signature (non testable)");
  } else if (!testable(g_coil)) {
    r.verdict = V_NOSIG;
  } else {
    r.seen    = g_react &  s.mask;
    r.missing = s.mask  & ~g_react;
    r.extra   = g_react & ~s.mask;
    r.verdict = r.seen ? (r.missing ? V_PARTIAL : V_OK) : V_NOREACT;
  }
}

// Set up the next coil, or finish the run. Returns false when the run is over.
bool enterCoil(uint32_t now) {
  while (g_coil <= N_COIL) {
    if (g_learn || testable(g_coil)) {
      g_rep = 0;
      memset(g_hit, 0, sizeof g_hit);
      g_react = g_close = g_open = g_noise = 0; g_fault = 0;
      g_stateMs = g_sampMs = now;
      readMatrix(g_base);                       // reference for the first window
      g_state = g_learn ? ST_QUIET : ST_FIRE;
      return true;
    }
    g_res[g_coil].verdict = V_NOSIG;            // TEST: nothing to look at, do not fire
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
  load(keyFor());          // best effort; a missing file just means "jamais appris"
}

// The persistence slot. fpgalink::gameNo() is -1 until the FPGA announces a game, and on
// a board whose game_select never changes after reset it can stay -1 for ever (see
// fpgalink.h) -- so we do NOT fold that case into game 0, which would quietly share one
// profile between every title. It gets its own visible slot instead.
int keyFor() {
  int g = fpgalink::gameNo();
  return (g >= 0 && g < 64) ? g : KEY_GENERIC;
}
int loadedKey() { return g_sigKey; }

bool load(int key) {
  clearSigs();
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

const char* start(bool learn, int key, uint8_t pulseMs) {
  if (busy())                    return "un test de bobines est déjà en cours";
  if (!g_bus.rd || !g_bus.ready) return "pont SPI non initialisé";
  // The gate the brief is emphatic about: refuse loudly, never do nothing in silence.
  if (!g_bus.ready())
    return "mode diag inactif ou sorties non armées — appui long sur le bouton TEST "
           "de la portière, puis « mode contrôle »";
  if (key < 0 || key > KEY_GENERIC) return "numéro de jeu hors plage";

  if (!pulseMs) pulseMs = DEF_PULSE_MS;
  if (pulseMs > SAFE_PULSE_MS) pulseMs = SAFE_PULSE_MS;   // never provoke COIL_FAULT b0 ourselves

  if (learn) clearSigs();
  else {
    if (g_sigKey != key && !load(key))
      return "aucune signature pour ce jeu — lance d'abord l'apprentissage";
    bool any = false;
    for (uint8_t c = 1; c <= N_COIL; c++) if (testable(c)) any = true;
    if (!any) return "aucune bobine testable pour ce jeu — relance l'apprentissage";
  }

  for (uint8_t c = 0; c <= N_COIL; c++) g_res[c] = Res();
  g_learn = learn; g_key = key; g_pulse = pulseMs;
  g_reps  = learn ? LEARN_REPS : 1;
  g_coil  = 1; g_rep = 0; g_abort = false; g_err[0] = 0; g_runMs = 0;
  g_startMs = g_stateMs = g_sampMs = millis();
  g_state = ST_ARM;              // the first tick(), on loopTask, opens the bus traffic
  Serial.printf("[coil] %s armed: key=%d pulse=%ums reps=%u\n",
                learn ? "LEARN" : "TEST", key, (unsigned)g_pulse, (unsigned)g_reps);
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

    case ST_FIRE:
      readMatrix(g_base);                           // reference = the instant before the pulse
      g_react = g_close = g_open = 0;
      g_bus.wr(REG_COIL_FAULT, 0x00);               // so the fault we read back is THIS pulse's
      g_bus.wr(REG_PULSE_MS, g_pulse);
      g_bus.wr(REG_COIL, g_coil);
      g_fireMs = g_stateMs = g_sampMs = now;
      g_state = ST_WATCH;
      break;

    case ST_WATCH:
      if (sampled) fold(m, g_base, g_react, &g_close, &g_open);
      if (now - g_stateMs >= WIN_MS) g_state = ST_SCORE;
      break;

    case ST_SCORE: {
      uint8_t f = g_bus.rd(REG_COIL_FAULT);
      if (f) g_fault = f;                           // lisyctrl overwrites, so keep the last set one
      if (g_learn) {
        uint64_t cand = g_react & ~g_noise;         // minus whatever moved with no coil energised
        for (uint8_t b = 0; b < N_SW; b++) if (cand & (1ULL << b)) g_hit[b]++;
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
        g_noise = 0;
        g_stateMs = now;
        readMatrix(g_base);
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
  d["gen"]   = (fpgalink::gameNo() < 0) ? 1 : 0;         // 1 = no game number ever announced
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
  {   // progress = coils finished + the fraction of the current coil's repetitions
    uint16_t steps = (uint16_t)((g_coil - 1) * g_reps + g_rep);
    uint16_t total = (uint16_t)(N_COIL * g_reps);
    d["pct"] = busy() ? (int)(100UL * steps / (total ? total : 1))
                      : (g_state == ST_DONE ? 100 : 0);
  }
  JsonArray a = d["c"].to<JsonArray>();
  for (uint8_t c = 1; c <= N_COIL; c++) {
    JsonObject o = a.add<JsonObject>();
    o["n"] = c;
    o["l"] = g_sig[c].learned ? 1 : 0;
    addSw(o["sig"].to<JsonArray>(), g_sig[c].mask, &g_sig[c]);
    o["v"] = vName(g_res[c].verdict);
    if (g_res[c].missing) addSw(o["miss"].to<JsonArray>(), g_res[c].missing, nullptr);
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
