# GottFA80+ connected platform — status (autonomous session 2026-06-03)

Everything below is **validated by real toolchains tonight** (ghdl + PlatformIO,
both installed via Homebrew). No physical ESP/board was needed.

## Repos
| Path | What | Git |
|---|---|---|
| `~/gottfa-esp32/` | ESP32-S3 companion firmware + LISYcontrol web UI | not init'd (on disk) |
| `~/gottfa-upstream/GottFA80/` | bontango's GPL VHDL + our additions, branch **`lisyctrl`** | clone, uncommitted |
| `~/gottfa-tools/` | (existing) ROM-RE / patcher toolset | — |

## ✅ Validated tonight
1. **`lisyctrl` VHDL** (FPGA diagnostic bridge) — `ALL TESTS PASSED`
   ```sh
   cd ~/gottfa-upstream/GottFA80
   ghdl -a lib_common/spi_slave.vhd lib_common/lisyctrl.vhd sim/tb_lisyctrl.vhd
   ghdl -e tb_lisyctrl && ghdl -r tb_lisyctrl
   ```
   Covers: SPI slave, register file, switch-matrix scan, gts80 coil pulse + auto-clear, comms watchdog trip + re-arm.
2. **`nor_flash` VHDL** (SD→NOR ROM store, drop-in for SD_Card) — `NOR TESTS PASSED`
   ```sh
   ghdl -a lib_common/SPI_Master.vhd lib_common/nor_flash.vhd sim/tb_nor_flash.vhd
   ghdl -e tb_nor_flash && ghdl -r tb_nor_flash
   ```
3. **ESP firmware v0.2** (async + WebSocket + LittleFS + ArduinoJson + diag + norprog) — `SUCCESS`
   ```sh
   cd ~/gottfa-esp32 && pio run
   ```

## The 3 product features — where each stands
| Feature | FPGA side | ESP side | Status |
|---|---|---|---|
| **LISYcontrol diagnostics** | `lisyctrl.vhd` ✓ sim | web UI + WS + `diag.cpp` (mock) ✓ build | logic proven; needs Quartus integration + hardware |
| **ROM update (NOR)** | `nor_flash.vhd` ✓ sim | `norprog.cpp` (W25Q erase/program/verify) ✓ build | proven; needs wiring + the SPI bus arbitration |
| **OTA bitstream (JTAG)** | stock (EPCS16 on module) | `jtag.cpp` IDCODE ✓ | bring-up only; SVF/JAM player TODO |

## Try the UI right now (no hardware)
Open `~/gottfa-esp32/data/index.html` in a browser → DEMO mode (faithful LISYcontrol:
8×8 switch matrix red=closed, 48 lamps + blink, 9 coils, display 80/80B, 32 sounds, DIP).

## Next steps (in priority order)
1. **Run the two sims** yourself to confirm (commands above).
2. **Quartus integration**: apply `LISYCTRL.md` changes to `SYS80.vhd` (entity `MOSI/CLK/MISO`→`inout`, muxes, `lisy_active`, hold T65) and swap `SD_Card`→`nor_flash`. Compile for `10CL006YE144C8G`. *(Quartus needed — not available here.)*
3. **Send `LISYCTRL.md` + `NOR_FLASH.md` to bontango** — he's already doing ESP32-S3; align on the `inout`/mode-entry before merging (we're upstreaming everything, GPL).
4. **When the ESP arrives**: wire per `README.md` (J3a/K2/S8/P5/TP4), `pio run -t uploadfs` + `-t upload`, read JTAG IDCODE (expect `0x020F10DD`).
5. **TODO-confirm in VHDL**: lamp matrix DS group→latch mapping (vs IC11/IC13+SN74175); mode-entry mechanism (reco: `reset_sw` long-press); 80B `is80B`.

## Note on git
Nothing was committed (respecting "commit only when asked"). To checkpoint:
`cd ~/gottfa-upstream/GottFA80 && git add -A && git commit` (branch `lisyctrl`), and
`cd ~/gottfa-esp32 && git init && git add -A && git commit`.
