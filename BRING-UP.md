# GottFA80+ — hardware bring-up

How to take everything we built and run it on a real Gottlieb System 80/80A/80B machine.
Order matters: get the FPGA (MPU replacement) booting first, then the ESP (sound + diag).

## What you need
- A **GottFA80_PLuS** board (FPGA MPU replacement) + USB Blaster (FPGA programming).
- An **ESP32-S3** (DevKitC-1) wired to the board (sound + diag companion).
- **Two SD cards:** the **FPGA's** SD (128 MB image + game ROMs — bontango's format) and the
  **ESP's** own SD (PSOWAV sound sets). The board SD is FPGA-owned; the ESP has its own.
- Gottlieb game ROM images (not distributable).
- `MCP4921` DAC + the on-board `TDA7267` amp path to a speaker.

## SKUs
- **FULL** (`lisy_enable=true, esp_sound=true`): ESP does diag + all sound + protection.
- **LITE** (`lisy_enable=false, esp_sound=true`): bare MPU, ESP still does sound.
Both drop GOSOF80 integrated sound + the DFPlayer (PSOWAV replaces them).

## Step 1 — FPGA (the MPU)
1. Build/flash the bitstream (Quartus 22.1std) for your board variant (Cyclone 10 or IV) onto
   the FPGA (EPCS via USB Blaster), or our fork `lisyctrl` branch for the diag features.
2. Prepare the **FPGA SD** per the GottFA80_PLuS manual (128 MB image + merged game ROM images;
   sector-based — **no Sandisk cards**).
3. Set **DIP S1 (game select)** to the game's number — the **GottFA80_PLuS gamelist** (manual
   Appendix A, mirrored in our `games.txt`): e.g. Black Hole = 14, Arena = 51, Bounty Hunter = 40.
4. Replace the original MPU with the board, power on: the displays show FW ver / game# / lisy id.

## Step 2 — ESP firmware + UI
From `gottfa-esp32/`:
```
pio run -e esp32s3 -t upload       # firmware (sound + diag + web)
pio run -e esp32s3 -t uploadfs     # the web UI (data/ -> LittleFS)
```
(For the C3 variant: `-e esp32c3`. C3 has no sound tier.)
Set WiFi in `include/board_config.h` (`WIFI_STA_SSID/PASS`) or use the SoftAP `GottFA80-Setup`.

## Step 3 — PSOWAV sound set onto the ESP SD
Generate offline then deploy (see `PSOWAV.md`): `psowav-deploy.sh <out> <game...|--all>` →
copy the result to the **ESP SD root**:
```
/config.txt        # stheme = default game, volumes, mix
/games.txt         # FPGA game No -> romname (matches the gamelist DIP S1)
/<game>/NNNN-attr-VVV-snd.wav
```
For a single-game machine, `stheme=<game>` in config.txt is enough.

## Step 4 — wiring (see include/board_config.h)
- **Sound:** `MCP4921` SCK=16 / SDI=17 / CS=18 → DAC_R (Audio1 pin 4) → on-board `TDA7267`
  +12 V amp → cabinet speaker.
- **FPGA→ESP link:** the FPGA `Debug` pin (K2) → `PIN_FPGA_LINK` (S3 = GPIO8). Carries the
  diag-mode token + sound# (`0x80|s`) + game# (`0x40|No`).
- **Shared SPI** (diag): SCLK12/MOSI11/MISO13/CS10 (Hi-Z in normal mode; FPGA owns the bus).
- **Common ground** with the GottFA board is mandatory. All 3.3 V.

## Step 5 — power on + test
1. Browse to **http://gottfa.local/** (or the AP IP). The web UI loads (10 tabs).
2. **Son tab:** the game's set should be loaded (or pick it). Tap a sound → it plays via the
   MCP4921 → amp → speaker. Or `curl http://gottfa.local/snd?id=6`. No FPGA needed for this test.
3. **Start a game** on the machine → the FPGA sends sound commands on the link → the ESP plays
   the matching WAVs (PSOWAV). Confirm the sounds match the game.
4. **Diag:** long-press the door TEST switch → diag mode (the UI's "mode contrôle" arms) →
   test switches/lamps/coils/displays from the web.
5. **Tournament:** the Tournoi tab works standalone (manual scoring + time-attack live countdown).

## Known gaps (software done, hardware/FPGA pending)
- **Auto-scoring + countdown on the pinball display:** needs the FPGA to push game-over scores /
  draw the countdown (see `TOURNAMENT.md`; `tourney_countdown.vhd` + `tourney_block.vhd` are
  ghdl-verified, pending SYS80 integration + hardware, with Ralf).
- **OTA over WiFi:** the `/ota` endpoint is built but needs an OTA partition
  (`board_build.partitions`) + one USB flash to enable.
- **One-step flash** (ESP programs the FPGA bitstream + game ROM): JTAG SFL / NOR — TODO.
- **Sound sets:** 73/75 games clean; Excalibur = no PinMAME audio, Big House = partial (a
  PinmameReset hang — re-render per-command).

## Don't forget
- **Test everything on hardware before the bontango PR.** Coordinate the FPGA changes (GPL core)
  with Ralf; 80B integrated sound is his active WIP.
