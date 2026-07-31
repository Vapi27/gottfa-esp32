# GottFA80+ connected platform — status (updated 2026-07-31)

Validated by real toolchains: **ghdl** (logic sims), **Quartus Prime Lite 22.1**
(full FPGA flow, on a dedicated x86_64 build host), **PlatformIO** (firmware).
No physical ESP/board needed yet — hardware bring-up is the last remaining gate.

## Repos (both git, pushed)
| Path | What | Git |
|---|---|---|
| `~/gottfa-esp32/` | ESP32-S3/C3 companion firmware + LISYcontrol web UI | `Vapi27/gottfa-esp32` · `main` |
| `~/gottfa-upstream/GottFA80_PLuS/` | bontango's GPL VHDL + our additions | fork `Vapi27/GottFA80_PLuS` · branch **`lisyctrl`** (PR-ready) |
| `~/gottfa-tools/` | ROM-RE / patcher toolset | — |

## ✅ Validated
1. **`lisyctrl` + `spi_slave` + `nor_flash` VHDL** — 4 ghdl testbenches pass
   (`sh sim/run_all.sh`): register file, switch scan, gts80 coil pulse + watchdog,
   NOR read, and the `inout` shared-bus glue end-to-end.
2. **Quartus full flow** for the lisyctrl-integrated `SYS80`, **both** targets —
   `10CL006YE144C8G` (Cyclone 10 LP) and `EP4CE6E22C8` (Cyclone IV E):
   Analysis&Synthesis / Fitter / Assembler **0 errors**, timing **met**,
   `SYS80.sof` generated. lisyctrl costs +522 LEs, **0 new pins, 0 new memory**;
   `lisy_enable=false` compiles it out (91% → 83%).
3. **ESP firmware v0.3** — builds `esp32s3` + `esp32c3` (`pio run`).
   **Real SPI bridge** to lisyctrl (no more mock): mode-0 MSB-first 2-byte frames,
   bus arbitration off the FPGA `Debug` pin (= `lisy_active`), real switch-matrix
   scan, safe-on-entry (outputs forced OFF), watchdog status.

## The 3 product features — where each stands
| Feature | FPGA side | ESP side | Status |
|---|---|---|---|
| **LISYcontrol diagnostics** | `lisyctrl.vhd` ✓ sim **+ Quartus** ✓ (both devices) | web UI + WS + `diag.cpp` **real SPI bridge** ✓ build | code-complete; needs hardware bring-up |
| **ROM update (NOR)** | `nor_flash.vhd` ✓ sim (not yet wired into SYS80) | `norprog.cpp` ✓ build | needs a NOR-equipped board + arbitration wiring |
| **OTA bitstream (JTAG)** | stock (EPCS16 on module) | `jtag.cpp` IDCODE ✓ | bring-up only; SVF/JAM player TODO |

## Try the UI right now (no hardware)
Open `~/gottfa-esp32/data/index.html` in a browser → DEMO mode (faithful LISYcontrol:
8×8 switch matrix red=closed, 48 lamps + blink, 9 coils, display 80/80B, 32 sounds, DIP).
On real hardware the master toggle "arms the outputs" — **diag mode itself is entered
by a long-press of the Gottlieb door TEST switch** (the FPGA then releases the SPI bus
to the ESP and raises `Debug`).

## Arena Wall-Art LED — the decoration branch of the platform
A Gottlieb **Arena** playfield turned into an illuminated wall piece. Same repo,
**separate firmware** (`env:arenaled_d1mini32` / `arenaled` / `arenaled_c3`), zero
overlap with the app above: no FPGA, no SPI, no sound. Full doc: **[ARENA_LED.md](ARENA_LED.md)**.

| | State |
|---|---|
| LED boards | **made and in hand** — SK6812MINIRGBW-NW-P6 carrier, bus-wire slot pads, single-pad DATA in/out |
| Controller | **D1 Mini ESP32** (WROOM-32, 4 MB, CH340C), data on **GPIO27** |
| Firmware | v1.0.0 — 7 modes, insert map, power meter/limiter, web UI + REST + OTA |
| Builds | `arenaled`, `arenaled_c3`, `arenaled_d1mini32` ✓ (and `esp32s3`/`esp32c3` unaffected) |
| Hardware bring-up | **not started** — nothing powered yet |

### ✅ Validated (no hardware)
1. **Four PlatformIO targets build** — the Arena envs plus both GottFA80 envs, which
   the new source filter keeps untouched.
2. **Adversarial firmware review** — 12 agents over 4 lenses, every finding attacked by an
   independent verifier before it counted. 5 root causes found and fixed *before* first
   power-on: soft start that never ran (blocking WiFi init ate the window), a float
   animation clock that froze the wall after ~5 days, a shrunk LED count leaving the
   dropped tail latched on while the power meter stopped counting it, `test` mode unusable
   at 1 LED, and a null-deref panic on concurrent map edits.
3. **Animation clock proven by simulation**, not argument — `tools/host_arenaphase_test.cpp`
   replays 30 days of frames: the old float clock ran >1.5x fast from day 3.2 and stopped
   dead at day 4.8; the current double + per-effect modulo reduction holds 0 s drift and
   stays continuous on all six rates.
4. **3.3 V → 5 V without a 74AHCT125** — three IC-free options documented and one
   (`LED_REPEATER_PIXEL`) supported in firmware.

### Next steps
1. **Bench bring-up** — ARENA_LED.md §8, in order: buzz out one board, one board on a
   current-limited supply (~1 mA, dark), one board + ESP (`test` mode: R→G→B→W, ~70 mA
   all-dice), then **three chained** — the step that actually exercises the ground-less
   data hop and reveals a level-margin problem while it is still cheap to fix.
2. **Decide the data-level option on evidence** (§4): trim the bus to 4.4 V, or fit the
   2-diode repeater pixel. The bench answers this, not the datasheet.
3. **Map the inserts** with the web wizard once the chain follows the artwork; the default
   zone names are a template, not a survey of a real Arena playfield.
4. **V2 board** (if a second run happens): a local GND pad beside each data link, and a
   schematic so DRC has a netlist to verify against — this run's nets are
   `unconnected-(LED1-…)`, so connectivity was never machine-checked.

## Next steps
1. **Hardware bring-up** (when the ESP arrives): wire per `README.md` (J3a/K2/S8/P5/TP4),
   `pio run -t uploadfs` + `-t upload`, read JTAG IDCODE (expect `0x020F10DD`), then
   long-press the door TEST button and confirm the web UI shows `bus: diag`, `lisy: 0x80`.
2. **Open the upstream PR** to `bontango/GottFA80_PLuS` (branch `lisyctrl`) — gated on (1).
3. **Remaining code** (codable without hardware): JTAG SVF/JAM bitstream player;
   per-game CSV names on LittleFS; sound (needs a lisyctrl sound register) + display
   segment encoding; NOR integration into SYS80 (build option).
4. **VHDL TODO-confirm on hardware**: lamp matrix DS group→latch mapping (vs IC11/IC13 +
   SN74175); switch return polarity; 80B `is80B` status bit.
