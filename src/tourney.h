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
  // --- scoring mode: 0 = pinball score (manual/auto), 1 = TIME ATTACK (start high, decay/sec) ---
  void   setMode(uint8_t mode, uint32_t startPts, uint32_t decayPerSec);
  void   startGame(int id, uint32_t nowMs);     // time-attack: begin timing a player's game
  uint32_t stopGame(uint32_t nowMs);            // time-attack: end -> max(0,start-decay*s), record, return
  bool   gameActive();                          // a time-attack game is being timed
  int    activePlayer();                         // the player currently timed (0 = none)
  void   arm(int id);                           // arm a player to auto-time at the next FPGA game-start
  int    armed();                               // armed player id (0 = none)
  uint8_t curMode();                            // 0 = pinball score, 1 = time-attack
}
