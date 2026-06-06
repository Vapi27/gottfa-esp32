# Tournament mode — auto-scoring design

The game RULES run in the Gottlieb ROM (on the FPGA), so the ESP is a **score/tournament
manager**, not a rules engine. v1 already works with **manual** score entry (web UI). This
doc is the design for **automatic** scoring + showing whose turn it is — which needs FPGA
support, because today the score never reaches the ESP.

## Why it's not ESP-only (architecture reality)
- The FPGA→ESP link (`sound_link.vhd`, 1 wire) carries **only** sound (`0x80|s`), game number
  (`0x40|g`), and the diag token (`0xF0|d`). **Not the score / display.**
- The lisyctrl display registers (`0x40-0x43` SEG_A/B/C + U5 digit strobe) are **write-only**
  (for the ESP to *drive* the display in diag mode). **No read-display register.**
- During gameplay the **FPGA owns the SPI bus**; the ESP is Hi-Z (lisyctrl is reachable only
  in diag mode = game stopped).

→ The score must be **pushed by the FPGA**. Good news: the FPGA emulates the **SN7448
(BCD→7-seg)** for 80/80A and `byte_to_ascii` for 80B, so it already has the score **as digits**
(no 7-seg decoding needed on the ESP).

## Design
1. **FPGA (VHDL — to do with Ralf, GPL core, hardware-tested before PR):**
   - Detect **game over** (`game_running` falls).
   - Read the **final scores** (already BCD/ASCII in the display path).
   - Send a **scores packet** on the link (new message range, e.g. `0xE0` start … `0xE1` end,
     digits in between; must not collide with the 0x80/0x40/0xF0 token ranges or interleave
     with the diag heartbeat — frame accordingly).
2. **ESP (built, host-pluggable):**
   - `fpgalink` decodes the scores packet → `tourney::applyScores(scores, n)`.
   - A **round** = the tournament players assigned to game slots P1..Pn (`tourney::setRound`).
     `applyScores` records each slot's final score to its round player. Persisted to LittleFS.
3. **"Who plays":**
   - Simplest: use the game's **native multiplayer** (P1-4) — the game already shows the active
     player on the displays.
   - Optional ESP prompt ("PLAYER 2 — GO") = a display **write** via lisyctrl SEG/U5, only
     possible in **diag/idle** (game stopped) — i.e. between games. Also FPGA/hardware-gated.

## Tournament game modifications (FPGA interception)

Beyond scoring, tournament mode can **change the game** at the FPGA I/O level — the FPGA is the
MPU, so it sees/drives every coil, switch and the NVRAM. Three levers, easiest first:

1. **Forced settings (NVRAM preset).** The game's replay/ball/extra-ball adjustments live in
   NVRAM (the FPGA holds it). A tournament preset (no free play, fixed balls, no replay/extra
   ball/match) written to the per-game NVRAM addresses makes the *game itself* play fair. Needs
   the per-game NVRAM map.
2. **Solenoid suppression** — `lib_common/tourney_block.vhd` (**built + ghdl-verified**, 5/5).
   In tournament mode it neutralises a "free-game" solenoid (e.g. the **knocker** that fires on
   replay/match) by remapping its select code to a no-op the external decoder ignores. **Off by
   default** (tournament_mode=0 ⇒ exact passthrough). Integration in `SYS80.vhd` is one line at
   `u6pa_src` (line 379):
   ```vhdl
   u6pa_raw <= lisy_u6pa when lisy_active='1' else U6_pa_out;
   TBLK: entity work.tourney_block
     generic map ( BLOCK_CODE => <knocker select code> )      -- per game, from the SYS80 solenoid map
     port map ( port_in => u6pa_raw, sol_active => <sol strobe>,
                tournament_mode => tournament_mode, port_out => u6pa_src );
   ```
   (`u6pa_src` then feeds U6_PA exactly as today.) Need from Ralf/schematic: the `sol_active`
   strobe + the per-game knocker `BLOCK_CODE`.
3. **Arbitrary new rules** = reprogram the ROM (disassembly per game). Out of scope.

**`tournament_mode` control:** a latched bit set by the ESP via a new lisyctrl register
(e.g. `0x04 CTRL2` b0) written in diag mode and **latched so it survives into gameplay**
(or a spare DIP). The web UI's tournament screen arms it before the round.

## Time-attack countdown ON the pinball display (FPGA)

The web UI shows the time-attack countdown live, but to show it **on the machine's displays
during play** the FPGA must draw it — the game owns the displays during gameplay, the ESP
can only write them in diag mode. So time-attack lives best **on the FPGA**:

- **`lib_common/tourney_countdown.vhd`** (**built + ghdl-verified**, 9/9): while `run='1'`
  (game in play + time-attack mode) it loads `START_VAL` and decays by `DECAY` each 1 Hz
  `tick`, clamped at 0 (`dead`). The value is the live score **and** the final score at game
  over (so this mode needs **no pinball-score read** — the FPGA already has the number).
- **Display path — the full RTL chain is built + ghdl-verified:**
  `tourney_countdown.value` → **`bin_to_bcd`** (binary→BCD, 7/7) → **`value_to_dispstr`**
  (→ 7-char string, leading-zeros blanked, 6/6) → **`boot_message`** (string → segments +
  digit strobes via the existing `sn7448_gtb`, already in the FPGA). So only the **SYS80
  wiring** remains (with Ralf):
  1. Instantiate `tourney_countdown` → `value_to_dispstr` → feed its `dstr` to a `boot_message`
     display input (e.g. `display1`).
  2. **Activate** that draw during gameplay when time-attack is armed (today `boot_message`
     only runs when `game_running='0'`); extend the display mux at `SYS80.vhd` ~line 682
     (`bm_segments`/`bm_digit_strobe`) to win when `tournament_display='1'`.
- **`run` / `tick`:** `run` = `game_running` AND time-attack mode; `tick` = a 1 Hz strobe.
- **Final score → ESP:** at game over send `value` on the link (one number) → `tourney::recordScore`.
- `START_VAL`/`DECAY` + the `tournament_display` arm come from the ESP via a lisyctrl register.

Remaining = the SYS80 instantiation + mux + 1 Hz tick (small, with Ralf) + hardware test.
The whole data path (timer → BCD → display string) is sim-proven.

## Status
- ✅ ESP: `tourney` module (players/scores/leaderboard, LittleFS), **round + `applyScores`**
  auto-path, WS `t_round`/`t_apply`, "Tournoi" web tab (manual entry working now). Builds s3+c3.
- ⏳ FPGA: game-over detect + score read + scores-packet on the link — **VHDL, with Ralf**.
- ⏳ Hardware: end-to-end test on a real machine (per the project rule: PR only when tested).
- ⏳ ESP: `fpgalink` scores-packet decoder (once the wire format is agreed) + a round/turn UI.
