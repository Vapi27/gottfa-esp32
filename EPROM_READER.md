# Pstore EPROM reader — extension board (daughterboard)

Dump the user's **own** Gottlieb System 80 ROM chips straight into the device SD, driven by the
ESP32-S3 over a few cheap logic ICs. No PC, no separate programmer. Legal-clean: the user copies
their own chip, nothing is distributed.

> **Feasibility verdict:** ✅ doable, cheap (~6-8 €), integrated. The ESP can't wire 23 EPROM
> signals directly (only ~5 free GPIOs), so it drives the dump through **2× 74HC595** (address +
> control, serial-out) and **1× 74HC165** (data, serial-in) on 5 dedicated GPIOs. Honest limits at
> the end (24-pin jumper; mask-ROM / 6530 RIOT unreadable by *any* reader; raw→16 KB merge is a
> separate step). A USB T48 (~50 €) is still *less effort* for the same chip coverage — build this
> only if "integrated dump-your-own" is a product goal.

## 1. Architecture
```
                 ESP32-S3 (3.3 V)                         5 V
   GPIO15 SER  ───────────────►┐
   GPIO21 SCLK ──────┬─────────┼──► 74HC595 ×2  Q0..Q12 ─► EPROM A0..A12   (3.3 V high = TTL OK)
   GPIO42 RCLK ──────┼─────────┘            Q13 ─► /CE
                     │                       Q14 ─► /OE
   GPIO2  LOAD ──────┼───────────────► 74HC165  SH/LD
   GPIO21 SCLK ──────┘ (shared clock) ─────► 74HC165  CLK
   GPIO1  QH   ◄──────────────────────────── 74HC165  QH  ◄─ EPROM D0..D7
                                                              (5 V → 3.3 V: see §4)
   EPROM Vcc = 5 V ; Vpp//PGM = 5 V (read = high) ; GND common
```
Flow: ESP shifts a 16-bit word (address + /CE + /OE) into the 595 chain → latches → the EPROM
drives its 8 data lines → ESP pulses the 165 load and clocks the byte back → writes to SD. Repeat
for every address. ~60 lines of firmware (`src/epromdump.cpp`).

## 2. ESP32-S3 connector (5 dedicated GPIOs — all currently free)
| ESP GPIO | Signal | To | Note |
|---|---|---|---|
| 15 | `SER`  | 595 U1 SER (pin 14) | serial data to the 595 chain |
| 21 | `SCLK` | 595 SRCLK (pin 11, both) **+** 165 CLK (pin 2) | shared shift clock |
| 42 | `RCLK` | 595 RCLK (pin 12, both) | latch 595 outputs |
| 2  | `LOAD` | 165 SH/LD (pin 1) | parallel-load the data byte |
| 1  | `QH`   | 165 QH (pin 9) | serial data in (ADC pin reused as digital in) |

These 5 are free in `board_config.h` (esp32s3): SPI/JTAG/I2S/SD/link/OLED use the rest; pins 1/2 are
the optional coil-sense ADC inputs (mutually exclusive with the dumper — you don't dump while playing).

## 3. 74HC595 output map (2 chips daisy-chained, U1=low, U2=high)
| 595 | Q output | EPROM signal |
|---|---|---|
| U1 | Q0..Q7 | A0..A7 |
| U2 | Q0..Q4 | A8, A9, A10, A11, A12 |
| U2 | Q5 | /CE |
| U2 | Q6 | /OE |
| U2 | Q7 | spare (hold high) — A13 if you ever add 27128/27256 |

Daisy: ESP `SER`→U1.SER ; U1.QH'(pin 9)→U2.SER ; firmware shifts the **high byte first**.
74HC165 inputs A..H = EPROM **D0..D7**.

## 4. The one real gotcha: 5 V data → 3.3 V
The EPROM data outputs swing 0–5 V; the S3 is **not** 5 V-tolerant. Two clean options:
- **Cheapest (0 extra parts):** use a **5 V-tolerant 74HC165** (e.g. *Nexperia 74HC165*, inputs
  tolerant to ~15 V) powered at **3.3 V** → reads 5 V data directly. *(The TI SN74HC165N is NOT safe
  this way — abs-max input = Vcc+0.5 V.)*
- **Safe/explicit (+~1.5 €):** a **74LVC245** (5 V-tolerant inputs, Vcc = 3.3 V) on the 8 data lines
  between a normal 165 (at 5 V) and the ESP.

Address/control go **3.3 V → 5 V EPROM**: no shifting needed (EPROM Vih = 2.0 V TTL, 3.3 V reads as
high). Run the **595s at 3.3 V**. **Vpp and /PGM** = read condition is *high* → tie to **5 V** via a
pull-up (don't drive them from the 595).

## 5. ZIF socket + 24- vs 28-pin
One **28-pin ZIF** is the native socket for **2764 / 27128 / 27256** (8/16/32 KB) — this includes the
**80B 8 KB system ROM** (the soldered piggyback, the one chip in-system dumping really helps).
- **24-pin parts (2716/2732)** insert **bottom-justified** (chip pin 1 → socket pin 3). Bottom-
  justification aligns A0–A10, D0–D7, GND, /CE, /OE **perfectly**; only **Vcc** differs (24-pin Vcc
  lands on socket 26, not 28) → **JP1 jumper** routes 5 V to socket-28 (28-pin) or socket-26 (24-pin).
- **2716** also needs its Vpp (socket 23) held high → **JP2** to 5 V (or accept the 595 A11 line ≈
  3.3 V; safer to jumper).
Firmware just sets the **address width** per type (2716 = 2 KB, 2732 = 4 KB, 2764 = 8 KB) — /CE, /OE
and the data path are identical.

## 6. BOM (~6-8 €, one-off)
| Qty | Part | ~€ |
|---|---|---|
| 1 | 28-pin ZIF socket | 2–4 |
| 2 | 74HC595 | 0.7 |
| 1 | 74HC165 (5 V-tolerant, Nexperia) | 0.7 |
| (1) | 74LVC245 (only if non-tolerant 165) | 1.5 |
| — | 2×0.1 µF decoupling, 1–2 jumpers, header | <1 |
Protoboard or a tiny 2-layer PCB. Powered from the board's existing 5 V + 3.3 V.

## 7. U2/U3 mask ROMs (80/80A system) — solved in firmware, NO extra hardware
The 80/80A system ROMs **U2/U3** are **2332** mask ROMs (4Kx8), not EPROMs — different pinout and
**mask-defined chip-select polarity** (the thing that needs a "2332 adapter + 7404" on a normal
programmer). On THIS board it's **firmware-only**: a 2332 inserted **bottom-justified** in the
2764-wired ZIF lands its three quirky pins on our controllable 595 outputs —
| 2332 signal | chip pin | → socket | → 595 |
|---|---|---|---|
| A11 | 18 | 20 | **Q13** (the /CE line) |
| CS1 | 20 | 22 | **Q14** (the /OE line) |
| CS2 | 21 | 23 | **Q11** (the A11 line) |

— so we drive each independently (no 7404 needed). `epromdump` modes **`T2332_U2` / `T2332_U3`**
put A11 on Q13 and drive the selects at the Gottlieb polarity: **U2 = CS1(pin20) HIGH + CS2(pin21)
LOW**, **U3 = both HIGH**. Endpoint: `GET /dump?type=u2` / `?type=u3`. Same insertion as a 24-pin
part (JP1 = 24-pin Vcc); a 2332 has **no Vpp**. *(Polarity presets are from the verified study;
confirm on the first real chip — if a dump is all 0xFF/0x00, the CS2 level is flipped.)*

## 7b. Scope: the reader dumps CODE only — sound is provided by us (PSOWAV)
**Design decision (user, 2026-06-08): we ship the sound ourselves (PSOWAV samples), we do NOT read
the user's sound chips.** So the reader only ever dumps the **CPU / game / system code**:
- ✅ game ROM (2716/2732), 80B system (2764), **80/80A system U2/U3 (2332)** → all readable here.
- The **6530 RIOT** and the sound EPROMs are **never read** → the "RIOT is unreadable" limit is **moot**.
Code → `romstore` (`/roms/NN.img`); sound → PSOWAV folders (`/<game>/…wav`). The two are already
separate, so `/dump` feeds only the code side. (Legal note: dumping your own code = clean; the
PSOWAV samples are derived from the original sound ROMs = the bundled/derivative part — use original
re-created samples if you want the sound clean too.)
- A dumped chip is a **raw 2/4/8 KB image**. Assembling chips into the GottFA **16 KB game image**
  (pad + concat game + system) is a separate step — and the exact layout differs across GottFA
  manual versions (HW20 v1.01 = 4 KB game | 4 KB soundcard | 8 KB system; a v4.00 doc = 8 KB game |
  8 KB system). **Reverify the layout for the target software version before auto-merging.** For now
  `epromdump` writes the **raw** chip to `/dumps/<name>.bin` (unambiguous).

## 8. Software
`src/epromdump.{h,cpp}` — the dump engine (bit-banged 595/165 → SD). Endpoint `GET /dump?...`.
Gated by `EPROM_READER_ENABLE` (default 0 = no-op, compiles clean); set to 1 when the daughterboard
is fitted. See the header for the API.
