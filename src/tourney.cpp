// tourney.cpp — see tourney.h. Score/tournament manager, persisted to LittleFS.
// (C) 2026 Valere Pillet / Pstore. Original implementation.
#include "tourney.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <string.h>

namespace {
  constexpr int MAXP = 16, MAXS = 24;
  struct Player { int id; char name[24]; uint32_t score[MAXS]; uint8_t n; bool used; };
  Player roster[MAXP];
  int nextId = 1;
  int roundIds[4] = {0,0,0,0};   // tournament player ids in game slots P1..P4 (0 = empty)
  int nRound = 0;
  uint8_t  mode = 0;             // 0 = pinball score, 1 = time-attack
  uint32_t startPts = tourney::TA_DEF_START;   // time-attack: starting points
  uint32_t decayPS  = tourney::TA_DEF_DECAY;   // time-attack: points lost per second
  uint32_t bonusPts = tourney::TA_DEF_BONUS;   // time-attack: points added per qualifying sound command
  uint32_t bonusCap = tourney::TA_DEF_CAP;     // time-attack: max total bonus per game (0 = uncapped)
  // --- live time-attack countdown (transient, never persisted) ---
  int      activeId = 0;         // player being timed (0 = idle)
  uint32_t remain   = 0;         // points left on the clock — THE live score
  uint32_t accMs    = 0;         // sub-second remainder, so pausing loses no fraction
  uint32_t lastMs   = 0;         // millis() of the last tickTA
  bool     taPaused = false;     // frozen (attract gap between auto-restarted games)
  uint32_t bonusAcc = 0;         // bonus granted so far this game (vs bonusCap)
  int armedId = 0;                          // player armed for game-start auto-timing

  uint32_t ptotal(const Player& p){ uint32_t t = 0; for (int i = 0; i < p.n; i++) t += p.score[i]; return t; }
  uint32_t pbest (const Player& p){ uint32_t b = 0; for (int i = 0; i < p.n; i++) if (p.score[i] > b) b = p.score[i]; return b; }
  Player* find(int id){ for (int i = 0; i < MAXP; i++) if (roster[i].used && roster[i].id == id) return &roster[i]; return nullptr; }

  void save(){
    JsonDocument d; d["nextId"] = nextId;
    d["mode"] = mode; d["start"] = startPts; d["decay"] = decayPS;   // scoring mode + config
    d["bonus"] = bonusPts; d["cap"] = bonusCap;                      // time-attack bonus config
    JsonArray a = d["players"].to<JsonArray>();
    for (int i = 0; i < MAXP; i++) if (roster[i].used) {
      JsonObject o = a.add<JsonObject>(); o["id"] = roster[i].id; o["name"] = roster[i].name;
      JsonArray s = o["s"].to<JsonArray>(); for (int k = 0; k < roster[i].n; k++) s.add(roster[i].score[k]); }
    File f = LittleFS.open("/tourney.json", "w"); if (!f) return; serializeJson(d, f); f.close();
  }
}

namespace tourney {

void begin(){
  for (int i = 0; i < MAXP; i++) roster[i].used = false;
  File f = LittleFS.open("/tourney.json", "r"); if (!f) return;
  JsonDocument d; DeserializationError e = deserializeJson(d, f); f.close(); if (e) return;
  nextId = d["nextId"] | 1;
  mode = d["mode"] | 0; startPts = d["start"] | TA_DEF_START; decayPS = d["decay"] | TA_DEF_DECAY;
  bonusPts = d["bonus"] | TA_DEF_BONUS; bonusCap = d["cap"] | TA_DEF_CAP;
  int idx = 0;
  for (JsonObject o : d["players"].as<JsonArray>()) { if (idx >= MAXP) break;
    Player& p = roster[idx]; p.used = true; p.id = o["id"] | 0;
    strncpy(p.name, o["name"] | "?", sizeof p.name - 1); p.name[sizeof p.name - 1] = 0;
    p.n = 0; for (uint32_t v : o["s"].as<JsonArray>()) if (p.n < MAXS) p.score[p.n++] = v;
    idx++; }
}

int addPlayer(const char* name){
  if (!name || !name[0]) return -1;
  for (int i = 0; i < MAXP; i++) if (roster[i].used && !strcmp(roster[i].name, name)) return -1;  // dup
  for (int i = 0; i < MAXP; i++) if (!roster[i].used) {
    Player& p = roster[i]; p.used = true; p.id = nextId++; p.n = 0;
    strncpy(p.name, name, sizeof p.name - 1); p.name[sizeof p.name - 1] = 0; save(); return p.id; }
  return -1;  // full
}
void removePlayer(int id){ Player* p = find(id); if (p) { p->used = false; save(); } }
void recordScore(int id, uint32_t s){ Player* p = find(id); if (p && p->n < MAXS) { p->score[p->n++] = s; save(); } }
void undo(int id){ Player* p = find(id); if (p && p->n > 0) { p->n--; save(); } }
void resetScores(){ for (int i = 0; i < MAXP; i++) if (roster[i].used) roster[i].n = 0; save(); }
void clearAll(){ for (int i = 0; i < MAXP; i++) roster[i].used = false; save(); }

void setRound(const int* ids, int n){
  if (n < 0) n = 0; if (n > 4) n = 4; nRound = n;
  for (int i = 0; i < 4; i++) roundIds[i] = (i < n) ? ids[i] : 0;
}
// AUTO-SCORING: the FPGA sends the final game scores (slots P1..Pn) at game-over;
// record each to the tournament player assigned to that slot in the current round.
void applyScores(const uint32_t* s, int n){
  if (n > nRound) n = nRound;
  for (int i = 0; i < n; i++) if (roundIds[i]) recordScore(roundIds[i], s[i]);   // recordScore saves
}
String roundJson(){
  JsonDocument d; d["t"] = "round"; JsonArray a = d["round"].to<JsonArray>();
  for (int i = 0; i < nRound; i++) a.add(roundIds[i]);
  String r; serializeJson(d, r); return r;
}

// --- scoring mode + time-attack countdown ---
// clamp to 7 decimal digits: the pinball display is 7 chars AND the lisyctrl TA register is 24-bit
// (<= 16,777,215), so 9,999,999 keeps the ESP-computed score and the FPGA on-display countdown identical.
void setMode(uint8_t m, uint32_t sp, uint32_t dp){
  mode = m ? 1 : 0;
  if (sp) startPts = (sp > TA_MAX) ? TA_MAX : sp;
  if (dp) decayPS  = (dp > TA_MAX) ? TA_MAX : dp;
  save();
}
// Restore the shipped time-attack tuning. /tourney.json is authoritative once written, so a
// firmware default change alone never reaches a board that has already saved a config — this is
// the one-click way back to the ground-truthed ~40 s run.
void resetTaDefaults(){
  startPts = TA_DEF_START; decayPS = TA_DEF_DECAY;
  bonusPts = TA_DEF_BONUS; bonusCap = TA_DEF_CAP;
  save();
}
void setBonus(uint32_t bp, uint32_t cap){
  bonusPts = (bp > TA_MAX) ? TA_MAX : bp;
  bonusCap = (cap > TA_MAX) ? TA_MAX : cap;                  // 0 = uncapped
  save();
}
uint32_t taStart(){ return startPts; }
uint32_t taDecay(){ return decayPS; }
uint32_t taBonus(){ return bonusPts; }
uint32_t taCap(){ return bonusCap; }

void startGame(int id, uint32_t nowMs){
  if (!find(id)) return;
  activeId = id; remain = startPts; accMs = 0; lastMs = nowMs; taPaused = false; bonusAcc = 0;
}
// Advance the clock by whole elapsed seconds (the leftover ms are kept in accMs, so pausing and
// resuming never rounds time away). 64-bit loss: seconds x decay overflows uint32 in ~7 minutes
// at the maximum decay, which is exactly the old bug that made the score jump back up.
void tickTA(uint32_t nowMs){
  if (!activeId) return;
  uint32_t dt = nowMs - lastMs; lastMs = nowMs;
  if (taPaused || !dt) return;
  accMs += dt;
  uint32_t secs = accMs / 1000; if (!secs) return;
  accMs -= secs * 1000;
  uint64_t loss = (uint64_t)secs * (uint64_t)decayPS;
  remain = (loss >= (uint64_t)remain) ? 0 : (uint32_t)((uint64_t)remain - loss);
}
void setPaused(bool p, uint32_t nowMs){
  if (!activeId || p == taPaused) return;
  tickTA(nowMs);                                  // bank the time up to the pause point
  taPaused = p; lastMs = nowMs;
}
bool paused(){ return taPaused; }
// Top up the clock. Cap-limited so a held drone (or any farmable cue) cannot buy infinite time,
// and clamped to the 7-digit display range.
uint32_t addBonus(uint32_t pts, uint32_t nowMs){
  if (!activeId || !pts) return remain;
  tickTA(nowMs);
  if (bonusCap) {
    if (bonusAcc >= bonusCap) return remain;
    if (bonusAcc + pts > bonusCap) pts = bonusCap - bonusAcc;
  }
  bonusAcc += pts;
  uint64_t v = (uint64_t)remain + (uint64_t)pts;
  remain = (v > (uint64_t)TA_MAX) ? TA_MAX : (uint32_t)v;
  return remain;
}
uint32_t bonusGiven(){ return bonusAcc; }
uint32_t stopGame(uint32_t nowMs){
  if (!activeId) return 0;
  tickTA(nowMs);
  uint32_t sc = remain;
  recordScore(activeId, sc);
  activeId = 0; remain = 0; accMs = 0; taPaused = false; bonusAcc = 0;
  return sc;
}
bool gameActive(){ return activeId != 0; }
// Current countdown (= what the display shows), 0 if idle. Advance-on-read: any caller, at any
// rate, sees an up-to-date value without a separate scheduler.
uint32_t liveScore(uint32_t nowMs){
  if (!activeId) return 0;
  tickTA(nowMs);
  return remain;
}
int  activePlayer(){ return activeId; }
void arm(int id){ armedId = find(id) ? id : 0; }
int  armed(){ return armedId; }
uint8_t curMode(){ return mode; }

String json(){
  int idx[MAXP], n = 0;
  for (int i = 0; i < MAXP; i++) if (roster[i].used) idx[n++] = i;
  for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++)                 // rank by total desc
    if (ptotal(roster[idx[b]]) > ptotal(roster[idx[a]])) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
  JsonDocument d; d["t"] = "tourney"; JsonArray arr = d["players"].to<JsonArray>();
  for (int r = 0; r < n; r++) { Player& p = roster[idx[r]]; JsonObject o = arr.add<JsonObject>();
    o["id"] = p.id; o["name"] = p.name; o["total"] = ptotal(p); o["best"] = pbest(p);
    o["games"] = p.n; o["last"] = p.n ? p.score[p.n - 1] : 0; }
  d["n"] = n;
  d["mode"] = mode; d["start"] = startPts; d["decay"] = decayPS; d["active"] = activeId;  // mode + live timer
  d["bonus"] = bonusPts; d["cap"] = bonusCap;                                            // time-attack bonus config
  d["armed"] = armedId;                                                                  // auto-timer armed player
  String s; serializeJson(d, s); return s;
}

} // namespace tourney
