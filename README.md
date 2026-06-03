# GottFA80 PLuS — ESP32-S3 companion firmware

WiFi companion for the GottFA80+ (Cyclone 10 LP) Gottlieb System 80/80A/80B board.
Part of the **Pstore Pinball Platform** (see `../gottfa-tools/PROJECT_BRIEF.md`).

Three target features:
1. **SPI bridge / LISYcontrol** — read/write FPGA game state + diagnostics (lamps / coils / switches) over WiFi.
2. **ROM update** — flash the SD / NOR game ROMs over the shared SPI bus.
3. **OTA bitstream** — reflash the FPGA over JTAG (module header **P5**).

## Status — v0.2 (LISYcontrol web UI + WebSocket)
- WiFi STA with SoftAP fallback, mDNS `http://gottfa.local/`
- **ESPAsyncWebServer + WebSocket**, UI served from **LittleFS** (`data/index.html`)
- **Modern LISYcontrol UI** (faithful to lisy80_control): live 8×8 switch matrix (red=closed, strobe×10+return), 48 lamps + blink, 9 coils (pulse+safety), display 80/80A & 80B, 32 sounds (toggle), DIP, info — with a **mock backend** (`src/diag.cpp`) so it runs with no FPGA
- WS protocol maps 1:1 onto the **lisyctrl SPI register map** (see ../gottfa-upstream/GottFA80/LISYCTRL.md)
- **JTAG IDCODE read** on P5 — validates wiring; expects `0x020F10DD` (10CL006)
- Group-A SPI/control pins held Hi-Z (the FPGA owns the bus in normal mode)

## Hardware target
ESP32-S3 (DevKitC-1). All 3.3 V — **no level shifting**. **Common ground mandatory.**
Power the ESP from board **+5V (TP4)** → its onboard regulator (don't load the FPGA 3.3 V rail).

| Group | ESP signals | Board tap |
|---|---|---|
| SPI bus | SCLK / MOSI / MISO / CS_SD | carrier SD socket **J3a** |
| Control | Reset (open-drain) / Debug | **S8** / **K2** |
| JTAG | TCK / TMS / TDI / TDO | module header **P5** |
| Power | 5 V in + GND | **TP4** + GND |

GPIO map is in `include/board_config.h` (with FPGA pin numbers per signal). Adjust if your S3 board differs.

## Build / flash / monitor (PlatformIO)
```sh
pio run                 # build
pio run -t upload       # flash
pio device monitor      # serial @ 115200
```
First boot prints the FPGA IDCODE and the WiFi IP, then serves `http://gottfa.local/`.
No JTAG wired yet? IDCODE reads `0x00000000` / `0xFFFFFFFF` — expected, harmless.

## Roadmap
- [x] v0.1 — WiFi / web / mDNS + JTAG IDCODE bring-up
- [x] v0.2 — ESPAsyncWebServer + WebSocket + LittleFS; **modern LISYcontrol UI**; `diag` backend (mock); compiles ✓
- [x] FPGA `lisyctrl.vhd` (SPI slave + register file + switch scan + coil watchdog) — **sim ✓** (`../gottfa-upstream/GottFA80`)
- [x] FPGA `nor_flash.vhd` (SD→NOR ROM store) — **sim ✓**; ESP `norprog.cpp` (W25Q erase/program/verify) — compiles ✓
- [x] JTAG TAP primitives (`shiftIR/shiftDR/reset/runTest`) — OTA groundwork
- [ ] Quartus: integrate `lisyctrl` + `nor_flash` into `SYS80` (see LISYCTRL.md / NOR_FLASH.md)
- [ ] Bind `diag.cpp` mock → real SPI register access (needs bus arbitration + hardware)
- [ ] JTAG vector player (SVF/JAM) → SRAM load, then EPCS16 indirect = OTA

## Notes
- The SPI / Reset features require the matching **FPGA VHDL** (SPI slave + reset-triggered tristate + lisyctrl register file). **JTAG works against the stock bitstream.**
- Reading IDCODE does not disturb a running FPGA (TAP-only reset).
- Carrier is now at rev **220**; pin analysis was done on rev 211 — the FPGA/module pins are identical, just confirm the carrier tap-point silk (J3a / K2 / S8 / TP4).
