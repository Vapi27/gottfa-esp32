// coiltest.h — "which solenoid did NOT fire?", using switch feedback only.
//
// THE IDEA.  A working solenoid MOVES something, and on a pinball almost everything
// that moves is watched by a switch: the outhole kicker empties the outhole (its
// switch OPENS), a ball release lands on the shooter-lane rollover (CLOSES), a
// drop-target reset lifts a whole bank (several switches flip at once), a saucer
// kickout empties the saucer (OPENS). So we do not need a current sensor to tell a
// dead coil from a live one — we need to know, per machine, WHICH switch each coil
// is supposed to disturb. Nobody has that table, so the machine learns it once
// (LEARN) and replays it afterwards (TEST).
//
// WHAT IT CANNOT DO — and says so.  A knocker, the chimes, a coin lockout and the
// outhole-to-trough kicker of an empty machine move nothing any switch can see.
// LEARN marks those "aucune signature — non testable" instead of inventing one.
// Being explicit about the blind spots is the whole point: an operator who is told
// "coil 5 is not testable" will go and listen to it, whereas a fake green tick
// would send them away happy with a dead knocker. Electrical open/short detection
// needs the current shunt on PIN_COIL_SENSE (see board_config.h) and is out of
// scope here.
//
// THE PRECONDITION PROBLEM — the thing that decides whether this tool is honest.
// A coil→switch link is CONDITIONAL ON PLAYFIELD STATE. A drop-target reset moves
// nothing when the targets are already UP. An outhole kicker moves nothing with no
// ball in the outhole. A ball release moves nothing on an empty trough. So a
// perfectly healthy coil reads "aucune réaction" whenever its mechanism happens to
// sit in its post-fire state — a false accusation against good hardware, which is
// strictly worse than having no test at all.
// Two consequences run through this whole file:
//   1. A signature records the TRANSITION, not just "this switch moved": which way
//      each switch went. The state it must be in beforehand is then the opposite of
//      where it ended up, so TEST can check the precondition BEFORE firing and answer
//      "précondition non remplie — mets les cibles en bas" instead of blaming the
//      coil. `précondition` and `panne` are different verdicts and must look it.
//   2. These mechanisms are ONE-SHOT: the first pulse consumes the precondition.
//      Firing a fixed 3 repetitions therefore measures the first pulse and then two
//      guaranteed silences, scoring a healthy coil 1/3 — below the accept threshold,
//      so the signature is thrown away. LEARN adapts instead: before each extra
//      repetition it re-derives the precondition from what it just saw and stops the
//      moment the playfield can no longer produce the same transition, then reports
//      the honest denominator (1/1, not 1/3). Where a game table exists (gamedata.h)
//      agreement with the manual's own coil→switch table is a far better confirmation
//      than three identical pulses ever was.
//
// WHY NOT A jobRun() BODY.  net.cpp's single-slot job queue exists so a slow route
// cannot stall the AsyncTCP task, and its runners execute to completion INSIDE
// netLoop(). A full LEARN is ~25 s of pulse-and-watch; running it as a job body
// would simply move the stall from the network task to the main loop and take
// diag::tick(), wifiprov::tick() and the WebSocket pings down with it. So the same
// contract is honoured -- the handler validates, returns immediately, the work runs
// on loopTask -- with an INCREMENTAL state machine: tick() does a few hundred
// microseconds of SPI and returns. Progress is polled exactly like /jobstatus.
//
// (C) 2026 Valere Pilpil / Pstore. Original implementation.
#pragma once
#include <Arduino.h>

namespace coiltest {

  constexpr uint8_t N_COIL = 9;    // SOL1..SOL9 — the CPU-driven solenoids. lisyctrl's
                                   // f_coil() returns 0x00 (all drivers off) outside 1..9,
                                   // so 9 is a hardware fact, not a guess. Flippers, pop
                                   // bumpers and slingshots are wired direct on this machine:
                                   // they never appear here, but they CAN fire at any moment
                                   // and disturb switches while a run is in progress — which
                                   // is what the idle control window below is for.
  constexpr uint8_t N_SW   = 64;   // 8 strobes x 8 returns

  // --- what the caller has to lend us -----------------------------------------------
  // coiltest owns no hardware. diag.cpp owns the shared-bus SPI bridge (it is only
  // legal to touch it while the FPGA has granted the bus), so it passes the three
  // things we need. `ready` is re-checked EVERY tick: losing diag mode or disarming
  // the outputs mid-run aborts instead of silently pulsing into a dead bus.
  struct Bus {
    uint8_t (*rd)(uint8_t reg);            // lisyctrl register read
    void    (*wr)(uint8_t reg, uint8_t v); // lisyctrl register write
    bool    (*ready)();                    // bus owned AND outputs armed
  };
  void begin(const Bus& b);

  // Start a run. Returns nullptr on success, or a ready-to-display French reason.
  // `key` is the persistence slot (see keyFor()); `pulseMs` is clamped to SAFE_PULSE_MS.
  // `only` is a bitmask of coils to run, bit0 = coil 1 (0 = all of them). It exists for
  // the loop the precondition problem forces: the operator sets the playfield up, tests,
  // gets "précondition non remplie" on three coils, rearranges the playfield and must be
  // able to retry JUST those three — redoing all nine would re-consume the mechanisms
  // that had just passed and turn their verdicts back into unknowns.
  const char* start(bool learn, int key, uint8_t pulseMs, uint16_t only = 0);
  // Coils whose last verdict was "précondition non remplie", as an `only` mask. 0 = none.
  uint16_t    inconclusiveMask();
  void        abort();          // stop at the next tick, coil off, verdict "abandonné"
  void        tick();           // call from diag::tick() on loopTask, every pass
  bool        busy();

  // Persistence slot for the running title: the FPGA game number when it announced one,
  // else the operator's remembered choice, else KEY_GENERIC. Never guesses a game number.
  constexpr int KEY_GENERIC = 99;
  int  keyFor();
  // Name the title by hand and remember it across reboots. Needed because the FPGA only
  // emits its game number when game_select CHANGES, so a board that was simply switched on
  // reports nothing at all and would otherwise sit for ever on the generic slot with no
  // game table (observed on the bench). A number the FPGA reports always wins over this.
  // key outside 0..62 clears the override. Refused while a run is in progress.
  bool setGame(int key);
  int  gameOverride();       // -1 = none stored
  bool load(int key);           // read /coilsig-NN.json into the live signature table
  int  loadedKey();             // key currently in memory, -1 = nothing loaded

  // Live progress + last results, as the JSON the web UI and GET /coiltest consume.
  String statusJson();

} // namespace coiltest
