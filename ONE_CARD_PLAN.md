# One-Card Plan — ESP serves the game ROM to the FPGA

Goal: **one SD card, owned by the ESP.** The FPGA no longer has its own SD; it
reads the game ROM from a NOR flash that the ESP programs. Result: one card,
100 % SD-card compatibility, and **WiFi-updatable ROMs** (the editor vision).

## Why this is needed (the real problem we hit)

The current wiring has the **ESP tapped onto the FPGA's SD card** (both are
masters on the same SPI card). That cannot work:

- Two SPI masters on one card → bus collision → `no token received`.
- Worse: the FPGA SD uses a **fixed-sector format** (not FAT). The ESP writing
  FAT (`/arena/*.wav`, `/roms/`, `games.txt`) onto it **corrupts the ROM area**
  the FPGA reads. This is almost certainly why the card kept dropping out.

Two clean fixes exist:
1. **Two separate cards** (the current firmware's design: ESP SD on GPIO 38-41,
   FPGA SD untouched). Simplest, works today, but two cards.
2. **One-card (this plan)**: the ESP owns the storage and feeds the FPGA.

## Route decision

| Route | Idea | Verdict |
|---|---|---|
| **A — NOR flash** | Add a W25Q SPI NOR on the shared bus. ESP writes it (`norprog.cpp`), FPGA reads it (`nor_flash.vhd`). | ✅ **CHOSEN** — VHDL is a drop-in for `SD_Card.vhd`; ESP code exists. |
| B — reuse IS25 (config flash) | Store the ROM in the free ~1.6 MB of the bitstream's own IS25LP016D via ASMI. | ⚠️ complex + risky (it's the config flash; a bad write bricks boot). |
| C — block-RAM at boot | ESP pushes the 16 KB ROM into FPGA block-RAM each boot. | ❌ **RULED OUT** — hybrid bitstream already uses 21/30 M9K (70 %); only ~83 Kbit free, ROM needs 128 Kbit. Doesn't fit. |

**Measured (hyb_ay fit report): M9K 21/30, memory bits 139264/276480 (50 %).**
→ Route C is off the table until the bitstream frees ~1.5 M9K. Route A it is.

## Route A — what it takes

### Hardware
- **One SPI NOR flash**, e.g. **W25Q32** (4 MB, ~1 €) or W25Q16.
  - 16 KB per game → 4 MB holds ~256 games; plenty.
- Wire it on the **shared SPI bus** (the same SCLK/MOSI/MISO the FPGA SD used):
  - NOR CLK ← FPGA SPI clock (and ESP SCLK when ESP owns the bus)
  - NOR DI  ← MOSI,  NOR DO → MISO,  NOR /CS ← a CS line
- **Arbitration**: the ESP may only drive the bus while the **FPGA is held in
  reset** (so exactly one master at a time). The reset line is already wired
  (`PIN_FPGA_RESET` → S8.2). ESP: assert reset → drive NOR → release reset.

### FPGA side (Quartus, with Ralf)
- `nor_flash.vhd` already exists and is a **drop-in for `SD_Card.vhd`**
  (same entity interface). Integration = swap the instance in `SYS80.vhd`:
  - address = `gamenumber*0x4000 + base_addr` (16 KB/game), READ (0x03) + 24-bit addr.
  - `i_Rst_L` = FPGA reset (so it re-reads after the ESP finishes programming).
- Recompile the hybrid bitstream **with `nor_flash` instead of `SD_Card`**, keep
  `hybrid=true` (sound_link stays). Flash the new `.jic` (same Fedora/Blaster flow).
- **Status today**: `nor_flash.vhd` sim ✓, NOT yet instantiated in SYS80. This is
  the main new work.

### ESP side
- `norprog.cpp` already does W25Q **JEDEC id / erase / page-program / verify**
  (compiles ✓). One TODO in it: confirm the FPGA released the bus via the
  Debug-line handshake before driving (see `norprog.cpp:54`).
- Flow: ESP reads the game image from **its own FAT SD** (or WiFi upload) →
  holds FPGA in reset → erases+programs the NOR at `game*0x4000` → releases reset
  → FPGA boots and reads the game from NOR.
- Web route to trigger it (like `/romup` today) → **WiFi-updatable ROMs**.

## Order of work (each step verifiable)

1. **Bench the NOR alone**: wire the W25Q to the ESP, `norprog::jedecId()` must
   return a valid id. (Pure ESP test, no FPGA — proves the SPI + wiring.)
2. **Program + verify** a known 16 KB image into the NOR from the ESP.
3. **Quartus**: integrate `nor_flash` into `SYS80` (swap `SD_Card`), keep hybrid.
   Recompile → new `.jic`. (Ralf / VPS Quartus, same flow as the hybrid flash.)
4. **Arbitration bring-up**: ESP holds reset, programs NOR, releases; confirm the
   FPGA boots the game from NOR (machine plays).
5. **Wire the web trigger** → upload a ROM by WiFi, ESP reprograms the NOR, done.

## Reality check

This is a **multi-day FPGA project**, not a wiring fix. Until it's done:
- **Keep the FPGA SD and the ESP SD as two separate cards** (the current design).
  Do NOT tap the ESP onto the FPGA's SD — that's what corrupts the ROMs.
- The audio path is already proven working (I2S DAC OK); it only needs the
  ESP's **own** small FAT SD for the WAVs.

## Files
- FPGA: `gottfa-upstream/GottFA80_PLuS/lib_common/nor_flash.vhd` (+ `spi_slave.vhd`)
- ESP:  `src/norprog.cpp` / `src/norprog.h`
- Ref:  `WIRING.md` §4 (shared bus + reset), `STATUS.md` (ROM-update row)
