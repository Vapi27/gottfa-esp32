// tourney.h — tournament / score-keeping for the Pstore Pinball Platform.
//
// The game RULES live in the Gottlieb ROM (run by the FPGA), so this is a SCORE/TOURNAMENT
// manager, not a rules engine: the ESP tracks players + their game scores and ranks them,
// driven from the web UI. v1 = MANUAL score entry (the TD reads the pinball display and types
// the score); auto-read from the display is a later add-on (needs the multiplexed display
// decode, which is hardware-gated). State persists to LittleFS (/tourney.json) across reboots.
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include <Arduino.h>

namespace tourney {
  void   begin();                              // mount-agnostic: load /tourney.json if present
  int    addPlayer(const char* name);          // -> player id, or -1 if full / dup
  void   removePlayer(int id);
  void   recordScore(int id, uint32_t score);  // append a game score for a player (manual)
  void   undo(int id);                          // drop a player's last score
  void   resetScores();                         // keep players, clear all scores
  void   clearAll();                            // remove everyone
  String json();                                // ranked leaderboard + meta (for the web UI)
  // --- round / auto-scoring (the next game's players in P1..Pn order) ---
  void   setRound(const int* ids, int n);       // assign tournament players to game slots P1..Pn
  void   applyScores(const uint32_t* s, int n); // AUTO: record game-slot scores to the round players
  String roundJson();                           // {round:[ids], names:[...]} for the UI
  // --- scoring mode: 0 = pinball score (manual/auto), 1 = TIME ATTACK ---------------------
  // Time-attack is an INCREMENTAL countdown, not a formula over a start timestamp: the live
  // value is state, so it can be paused (the FPGA auto-restart gap between games must not cost
  // the player points) and topped up (sound-command bonuses). It also has to be: the old
  // start-minus-decay*elapsed form overflowed uint32 (9,999,999 x 430 s > 2^32) and the score
  // wrapped back UP mid-game. All decay/bonus arithmetic below is 64-bit then clamped.
  constexpr uint32_t TA_MAX = 9999999u;         // 7 display digits (and the 24-bit lisyctrl reg)
  void   setMode(uint8_t mode, uint32_t startPts, uint32_t decayPerSec);
  void   setBonus(uint32_t bonusPts, uint32_t capPts);   // per-sound-hit bonus + total cap/game
  uint32_t taStart();                           // current time-attack start points (for FPGA sync)
  uint32_t taDecay();                           // current time-attack decay/sec   (for FPGA sync)
  uint32_t taBonus();                           // points added per qualifying sound command
  uint32_t taCap();                             // max total bonus per game (0 = uncapped)
  void   startGame(int id, uint32_t nowMs);     // time-attack: load the countdown and run it
  uint32_t stopGame(uint32_t nowMs);            // time-attack: end -> record the remainder, return it
  void   tickTA(uint32_t nowMs);                // advance the countdown (idempotent; call often)
  void   setPaused(bool p, uint32_t nowMs);     // freeze/resume the countdown (between games)
  bool   paused();
  uint32_t addBonus(uint32_t pts, uint32_t nowMs);  // top up (cap-limited) -> new remaining
  uint32_t bonusGiven();                        // bonus points granted so far this game
  bool   gameActive();                          // a time-attack game is being timed
  uint32_t liveScore(uint32_t nowMs);           // current countdown value (0 if none) — for the on-display inject
  int    activePlayer();                         // the player currently timed (0 = none)
  void   arm(int id);                           // arm a player to auto-time at the next game start
  int    armed();                               // armed player id (0 = none)
  uint8_t curMode();                            // 0 = pinball score, 1 = time-attack
}
