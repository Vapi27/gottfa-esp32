// tourney.cpp — see tourney.h. Score/tournament manager, persisted to LittleFS.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
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
  uint32_t startPts = 1000000;   // time-attack: starting points
  uint32_t decayPS  = 10000;     // time-attack: points lost per second
  int activeId = 0; uint32_t startMs = 0;   // time-attack live timer (transient)

  uint32_t ptotal(const Player& p){ uint32_t t = 0; for (int i = 0; i < p.n; i++) t += p.score[i]; return t; }
  uint32_t pbest (const Player& p){ uint32_t b = 0; for (int i = 0; i < p.n; i++) if (p.score[i] > b) b = p.score[i]; return b; }
  Player* find(int id){ for (int i = 0; i < MAXP; i++) if (roster[i].used && roster[i].id == id) return &roster[i]; return nullptr; }

  void save(){
    JsonDocument d; d["nextId"] = nextId;
    d["mode"] = mode; d["start"] = startPts; d["decay"] = decayPS;   // scoring mode + config
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
  mode = d["mode"] | 0; startPts = d["start"] | 1000000u; decayPS = d["decay"] | 10000u;
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

// --- scoring mode + time-attack timer ---
void setMode(uint8_t m, uint32_t sp, uint32_t dp){ mode = m ? 1 : 0; if (sp) startPts = sp; if (dp) decayPS = dp; save(); }
void startGame(int id, uint32_t nowMs){ if (find(id)) { activeId = id; startMs = nowMs; } }
uint32_t stopGame(uint32_t nowMs){
  if (!activeId) return 0;
  uint32_t secs = (nowMs - startMs) / 1000;
  uint32_t loss = secs * decayPS;
  uint32_t sc = (loss >= startPts) ? 0 : (startPts - loss);   // max(0, start - decay*seconds)
  recordScore(activeId, sc); activeId = 0; return sc;
}
bool gameActive(){ return activeId != 0; }
int  activePlayer(){ return activeId; }

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
  String s; serializeJson(d, s); return s;
}

} // namespace tourney
