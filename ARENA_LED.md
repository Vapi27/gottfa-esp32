# Arena Wall-Art LED

Turning a Gottlieb **Arena** playfield into an illuminated wall piece: realistic
pinball-style lighting, animated RGBW effects, Wi-Fi control, low voltage,
modular and repairable, designed from day one for **up to 150 LEDs**.

Decorative only — no gameplay electronics, no FPGA, no sound. The firmware lives
in this repo as its own PlatformIO environment (`arenaled`) and shares nothing
with the GottFA80 companion app.

---

## 0. Where this stands

| | State |
|---|---|
| LED boards | **made and in hand** — SK6812MINIRGBW-NW-P6 carrier, slot pads for the bus, single round pads for DATA in/out |
| Controller | **WEMOS/LOLIN D1 Mini ESP32** (ESP32-WROOM-32, 4 MB, CH340C), data on **GPIO27** |
| Firmware | complete, builds for 4 targets, adversarially reviewed (5 defects found and fixed before first power-on) |
| Hardware bring-up | **not started** — nothing has been powered yet |

### Roadmap
- [x] v1.0 — firmware: 7 modes, insert map, power meter + limiter, web UI + REST + OTA
- [x] LED boards designed, panelized, assembled and delivered
- [x] Adversarial preflight review — 5 defects found and fixed before first power-on
- [x] D1 Mini ESP32 target (GPIO27) + one-command flash script
- [ ] Bench bring-up §8 steps 1–4 (one board → three chained → thirty)
- [ ] Data-level option chosen on evidence: 4.4 V trim (§4 A) or repeater pixel (§4 B)
- [ ] Playfield populated, bus + injection + fuse
- [ ] Inserts mapped to real zones, boot preset saved
- [ ] V2 ideas: motion sensor, audio-reactive, MQTT / Home Assistant

**Next action: §8 step 1** — buzz out one board with a multimeter, then power it
alone on a current-limited supply (5 V, 150 mA): it must draw ~1 mA and stay
dark. Then §8 step 2, the single-LED bench test.

Open questions that only the bench can answer: whether the 3.3 V → 5 V data
margin needs option A or B (§4), what the real per-LED current is, and whether
the pixel colour order is really GRBW.

### Working on this locally

The firmware lives on branch `claude/arena-wall-art-led-b7d10r` (PR #1).

```sh
git clone https://github.com/Vapi27/gottfa-esp32.git
cd gottfa-esp32
git checkout claude/arena-wall-art-led-b7d10r

pip install --user platformio     # Linux/macOS; on Windows: pip install platformio
tools/arena_flash.sh              # build + flash + monitor (Windows: use the pio commands in §7)
```

Per-OS notes for the CH340C on this board:

- **Linux** — `sudo usermod -aG dialout $USER`, then log out and back in. Port is `/dev/ttyUSB0`.
- **macOS** — install the CH340 driver; port is `/dev/cu.wchusbserial*`.
- **Windows** — install the CH340 driver; port is `COM3`/`COM4`/… Run the `pio`
  commands from §7 directly (the shell script needs Git Bash or WSL).

Flashing must happen on the machine the board is plugged into — a cloud session
has no USB. If you want an assistant driving the board directly, run Claude Code
locally in this repo (`npm install -g @anthropic-ai/claude-code`, then `claude`);
it will have the serial port. That is a fresh session with no memory of this one,
which is what this section is for — it plus §8 is the whole handover.

---

## 1. System overview

```
   5 V / 15 A PSU
        │
        ├──────── power bus (AWG18: +5V / GND, under the playfield)
        │             ├── injection 1 (LED 1)
        │             ├── injection 2 (LED ~35)
        │             ├── injection 3 (LED ~75)
        │             └── injection 4 (LED ~115)
        │
   D1 Mini ESP32 ── 330 R ──▶ DATA ──▶ LED1 ▶ LED2 ▶ … ▶ LED150
   GPIO27 (Wi-Fi, OTA)                     (data only through the round pads)
```

| Block | Choice | Why |
|---|---|---|
| LED | **SK6812MINI-RGBW-NW-P6** | addressable, integrated controller, RGB **+ dedicated neutral white**, 3.5 mm package, single wire |
| MCU | **D1 Mini ESP32** (WROOM-32) — S3 / C3 also supported | Wi-Fi, OTA, RMT peripheral drives the chain with ~0 CPU |
| Bus | 5 V / GND, AWG18 | 10.5 A worst case can't run through thin wire |
| Data | one daisy chain, 800 kHz | independent from power — data connectors carry DATA + GND only |
| Supply | 5 V / 15 A (75 W) | 150 × 70 mA = 10.5 A + ~30 % margin |

The neutral-white die is the reason for this part: a real playfield is lit by
incandescent lamps, and warm white made from R+G+B always looks like a
disco. Here `W` does the lighting and `RGB` does the effects.

---

## 2. Power budget

| LEDs | Worst case (all four dice, full) | Realistic (warm white ~60 %) |
|---|---|---|
| 50 | 3.5 A | ~1.3 A |
| 100 | 7.0 A | ~2.6 A |
| 150 | **10.5 A** | ~3.9 A |

- **PSU: 5 V / 15 A / 75 W.** 20 % margin over the 10.5 A worst case is the
  minimum; 15 A gives 30 %.
- **Inline fuse: 10 A slow-blow** between the PSU and the bus. The firmware caps
  the chain at 9 A (`LED_POWER_BUDGET_MA`), so a 10 A fuse only ever blows on a
  real fault (a pinched wire, a shorted pad), never on a bright frame.
- Worst case is a number you should never actually reach: the firmware meters
  every frame and dims the whole wall rather than exceed the budget (§7).

### Wire gauge and voltage drop

AWG18 is ~21 mΩ/m per conductor. A 2 m bus run carrying 5 A drops
`2 × 2 m × 0.021 Ω × 5 A ≈ 0.42 V` — visible as a colour shift (the white die
browns out first). Hence:

| Wire | Use |
|---|---|
| **AWG18** | main +5 V / GND bus (recommended) |
| AWG20 | acceptable minimum for the bus, or for injection stubs |
| AWG22–24 | per-LED taps from the bus to the PCB pads (short) |
| AWG26 / ribbon | DATA + GND jumpers between boards |

---

## 3. Power injection

**Never feed 150 LEDs from one end.** Inject fresh +5 V and GND from the PSU
into the bus every **30–40 LEDs** — four injection points for a 150-LED build:

```
PSU ─┬─▶ injection 1  (LED 1)
     ├─▶ injection 2  (LED ~35)
     ├─▶ injection 3  (LED ~75)
     └─▶ injection 4  (LED ~115)
```

- Star-wire the injections back to the PSU terminals, don't chain them.
- **1000 µF / 16 V** electrolytic across +5 V / GND at each injection point —
  it absorbs the switching surge when a bright frame turns on.
- One common ground everywhere. The ESP32 ground **must** be tied to the LED
  ground, even when the ESP is powered from USB (it usually is — see §4).

---

## 4. Data signal without a 74AHCT125

The SK6812 wants `VIH ≥ 0.7 × VDD`. At `VDD = 5.0 V` that is **3.5 V**, and an
ESP32 GPIO only swings to **3.3 V** — marginal. It often works on the bench and
then fails on a long chain, or at temperature, or only on the far half. The
usual fix is a 74AHCT125 level shifter. **You don't need one.** Three options,
in order of preference:

### Option A — run the whole chain at 4.3–4.5 V *(recommended, zero parts)*

Trim the PSU output down instead of raising the logic. At `VDD = 4.4 V`,
`VIH = 0.7 × 4.4 = 3.08 V` — the ESP's 3.3 V is now **in spec with margin**.

- Nearly every 5 V metal-case supply has a **V-ADJ trim pot** (typically ±10–15 %,
  so ~4.25–5.75 V). Set it to **4.4 V measured at the PSU terminals under load**,
  and re-check at the far end of the bus (keep it above ~4.0 V).
- The SK6812MINI runs from ~3.7 V up, so 4.4 V is comfortable.
- Side effects, all benign: ~10 % less white output (just raise brightness),
  lower current draw, and slightly warmer whites — which suits a vintage
  playfield anyway.
- **Power the ESP32 from USB-C, not from the trimmed 4.4 V bus.** A DevKitC-1's
  AMS1117 regulator needs ≳4.6 V in to hold 3.3 V out. Tie the grounds together
  and leave the ESP on its own 5 V/USB feed.

### Option B — "repeater pixel" *(2 diodes, chain stays at 5 V)*

Feed **only the first LED** through two series Schottky diodes so it sits at
~4.4 V and therefore accepts 3.3 V data. Its **DATA OUT is a regenerated,
full-VDD-swing signal**, which then drives the rest of the chain at a full 5 V.

```
 +5V ──┬──────────────────────────────▶ LED2..n  VDD (5.0 V)
       │
       └─▶ ▶| ─ ▶| ─┬─▶ LED1 VDD (~4.4 V)     2 × 1N5819 (any small Schottky —
          (2 × Schottky)                        LED1 draws ≤ 70 mA)
                    └─ 100 nF to GND

 ESP32 GPIO5 ── 330 R ──▶ LED1 DATA IN        LED1 DATA OUT ──▶ LED2 DATA IN
```

Mount that first pixel **hidden behind the frame** and set
`LED_REPEATER_PIXEL 1` in `include/arena_config.h`: the firmware keeps it dark
and shifts all indices, so LED 0 in the web UI is still your first visible
insert.

### Option C — discrete transistor shifter *(only if A and B are impossible)*

Two cascaded common-emitter inverters (2 × 2N3904/BC547, 1 kΩ base, 470 Ω
collector pull-up to +5 V) restore polarity and give a 5 V swing. It works, but
BJT storage time is significant next to an 800 kHz / 1.25 µs bit, so keep leads
short and treat it as a fallback. A single 2N7002/BS170 open-drain stage has the
same caveat and inverts, so it also needs two stages.

### If you can order one small part

Any of these is a drop-in for the 74AHCT125 and is a single cheap IC:
`74HCT245`, `74HCT04`, `74HCT14`, `SN74LV1T34` (SOT-23-5, single gate),
`SN74AHCT1G125`. Note the **T** in HCT/LVT — plain `74HC` at 5 V has the same
0.7 × VDD threshold problem as the LED and does **not** solve anything.

### Which option suits the D1 Mini ESP32

The controller in use is a **WEMOS/LOLIN D1 Mini ESP32** (ESP32-WROOM-32, 4 MB,
CH340C, micro-USB). It changes nothing about the LED side, but it does decide how
the ESP itself is powered, and that interacts with option A:

| Your board's LDO | What to do |
|---|---|
| ME6211 / SGM2212 class (~200 mV dropout) | Option A: trim the bus to 4.4 V and feed the ESP's **5V** pin straight off the bus. Verify: the **3V3** pin must still read ≥ 3.2 V with WiFi up. |
| AMS1117 (1.1 V dropout) or unknown | Either power the ESP from **micro-USB** (and tie ESP GND to the bus GND), or take **option B** and leave the bus at 5.0 V. |

Cheapest reliable answer during bring-up: ESP on USB, chain on the bench supply,
grounds tied. Decide the permanent arrangement afterwards.

### Signal hygiene (do this regardless of the option chosen)

- **330 Ω in series** at the ESP output, physically at the ESP end.
- **100 nF** across VDD/GND on **every** LED board (§5) — not optional at 150 pixels.
- ESP → LED1 wire **short** (< 15 cm), run alongside its ground return.
- **The board as built has no ground pin on its data links** (§5), so every data
  hop returns through the power bus. That is workable — it is how most commercial
  pixel strings do it — but it makes three things mandatory rather than optional:
  keep each board-to-board data hop **short** (≤ 10–15 cm) and lying **along the
  bus wires**, never looping away from them; keep the GND bus **continuous and
  low-impedance** (it is now the data return path, not just the power return);
  and prefer **option A**, whose fatter noise margin is exactly what a
  single-wire hop needs.

---

## 5. The LED board (as built)

The boards are made and in hand. One per insert, single-sided, with the
SK6812MINI-RGBW-NW-P6 and its 100 nF decoupling cap, and four connection points:

```
        ┌───────────────────────┐
        │  ▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄   │   ← GND slot   (bus wire passes through)
        │         GND           │
        │   ┌──────────────┐    │
        │   │  SK6812MINI  │ ▪  │   ▪ = 100 nF
        │   │     RGBW     │    │
   ●    │   └──────────────┘    │   ●   ← LED OUT (left) / LED IN (right)
  OUT   │                       │  IN       single round pads, DATA only
        │  ▐▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▌   │   ← +5V slot  (bus wire passes through)
        │         +5V           │
        └───────────────────────┘
```

Three consequences of this layout worth planning around:

1. **The slots are the mount.** The +5 V and GND bus wires thread through the two
   slot pads and are soldered there, so each board is carried mechanically by the
   bus itself. That means the **slot pitch fixes the spacing of the two bus
   wires** — run them as a parallel pair (a ladder), not as two independently
   routed wires. If a given insert needs the board somewhere the pair does not
   reach, solder short AWG22 stubs into the slots instead and keep the board loose.
2. **The data links are single pads** — DATA only, no ground pin. See §4 signal
   hygiene: short hops, laid along the bus.
3. **Solder the bus wires with a chisel tip and be quick.** An AWG18 wire in a
   large slot pad is a lot of thermal mass sitting ~10 mm from an already-reflowed
   SK6812, and those parts do not enjoy a second long heat soak. Pre-tin the wire,
   ~350 °C, in and out in a couple of seconds, and let each board cool before the
   next one.

### Before you wire a hundred of them: buzz out one board

The KiCad net names in the layout read `unconnected-(LED1-VDD-Pad6)`,
`unconnected-(LED1-GND-Pad2)`, `unconnected-(LED1-DIN-Pad3)` — the signature of a
PCB drawn without a schematic/netlist behind it. The copper is there, but **DRC
had no netlist to check it against**, so it could not have told you if a
connection were missing. Ten minutes with a multimeter on one board removes all
doubt before it is multiplied by a hundred:

| Check | Expected |
|---|---|
| +5 V slot ↔ LED VDD pad | continuity (< 1 Ω) |
| GND slot ↔ LED GND pad | continuity (< 1 Ω) |
| C1 legs ↔ +5 V slot / GND slot | continuity, one leg each |
| LED IN pad ↔ LED DIN pad | continuity |
| LED OUT pad ↔ LED DOUT pad | continuity |
| +5 V slot ↔ GND slot | **not** a short — expect kΩ or a diode drop through the LED, never < 1 Ω |
| LED orientation | pin 1 marker against the footprint — if the assembler rotated it, all boards are rotated |

A short between the slots on **one** board takes down the whole bus, so it is
also worth a quick +5 V/GND buzz on each board as you solder it into the chain.

### Fab spec (for reference / a v2 run)

| Parameter | Value |
|---|---|
| Layers / copper | 2 layers (this run is routed single-sided), 1 oz |
| Thickness | 1.0 mm — thin sits flush under the playfield |
| Solder mask | black hides better against the playfield underside; green is what this run used and is fine |
| Finish | ENIG — flat pads, kinder to the SK6812's side pads than HASL |
| Power traces | 1.0–1.5 mm |
| Data traces | 0.3 mm |
| Optional v2 | a second pad next to each data link for a local GND, and a schematic so DRC has a netlist to verify |

### Panelize — don't order small boards individually

At this size, board count is irrelevant to the price; **area** is. A 100 × 100 mm
panel holds dozens of boards with V-cut, for the price of a handful of separate
PCBs. Ask for **V-cut** (straight lines only) rather than mouse-bites — cleaner
edges on a board this small.

## 6. Bill of materials

For a 100-LED build (scale linearly; the design is validated to 150):

| # | Item | Qty | Note |
|---|---|---|---|
| 1 | ESP32-S3 DevKitC-1 | 1 | USB-C, Wi-Fi, OTA |
| 2 | LED PCB (12 × 12 mm) | 100 | panelized, §5 |
| 3 | SK6812MINI-RGBW-NW-P6 | 100 | +10 % spares |
| 4 | 100 nF X7R 0603 | 100 | one per board |
| 5 | 1000 µF 16 V electrolytic | 4 | one per injection point |
| 6 | PSU 5 V / 15 A / 75 W | 1 | with V-ADJ trim pot (§4 option A) |
| 7 | Fuse holder + 10 A slow-blow | 1 | PSU → bus |
| 8 | 330 Ω resistor | 1 | ESP data out |
| 9 | 1N5819 Schottky | 2 | only for §4 option B |
| 10 | AWG18 wire (red/black) | ~10 m | power bus — threads through the board slots |
| 11 | AWG26–24 wire | ~15 m | board-to-board data hops (single wire per hop) |
| 12 | AWG22 wire | ~5 m | only if some boards get stub taps instead of the through-bus |

No connectors: the boards as built take wire directly (slot pads for the bus,
round pads for DATA in/out), and the bus wire doubles as the mechanical mount.

A machine-readable version is in [`hardware/arena-led-bom.csv`](hardware/arena-led-bom.csv).

---

## 7. Firmware

Custom firmware rather than WLED — this is a mapped playfield, not a light strip.

| | WLED | This firmware |
|---|---|---|
| Setup | minutes | `pio run -t upload` |
| Effects | generic strip effects | **mapped to Arena inserts** (zones) |
| Power safety | brightness cap | **per-frame current meter + limiter** |
| RGBW | yes | yes, W-first warm white |
| Mapping | segment-based | named zones, editable in the UI, stored as JSON |
| Control | app / web | web UI + REST + OTA |

### Build and flash

One command, on the machine the board is plugged into — it finds the port,
builds, uploads the UI then the firmware, and opens the monitor, printing what to
try next on any failure:

```sh
tools/arena_flash.sh                 # or: tools/arena_flash.sh /dev/ttyUSB0
```

The same thing by hand:

```sh
# WEMOS/LOLIN D1 Mini ESP32 (the board in use):
pio run -e arenaled_d1mini32 -t uploadfs   # web UI + default map -> LittleFS (do this FIRST)
pio run -e arenaled_d1mini32 -t upload     # firmware
pio device monitor -e arenaled_d1mini32    # 115200

# ESP32-S3 DevKitC-1: -e arenaled     ESP32-C3: -e arenaled_c3
# Another board:      ARENA_ENV=arenaled tools/arena_flash.sh
```

No toolchain on that machine? A merged image (bootloader + partitions +
firmware + web UI) can be flashed with esptool alone:

```sh
pip install esptool
esptool.py --chip esp32 --port /dev/ttyUSB0 write_flash 0x0 arena-d1mini32-full.bin
```

Data is on **GPIO27** on the D1 Mini ESP32 (GPIO5 on S3/C3) — see
`include/arena_config.h` for why: 27 dodges the flash pins, the input-only pins,
the strapping pins and the CH340C's UART lines. If auto-reset fails on a CH340C
board, hold IO0 to GND while the upload starts.
The firmware fills ~70 % of that board's 1.3 MB app slot, so OTA still fits.

First boot brings up the SoftAP **`Arena-LED`** (password `pinball87`) →
open <http://192.168.4.1/>, enter your Wi-Fi in the last panel, and after the
reboot the wall is at **<http://arena.local/>**. Later updates can go over
Wi-Fi from that same page (OTA), no USB needed.

A cheaper ESP32-C3 build is `pio run -e arenaled_c3` — same firmware.

### Modes

| Mode | What it does |
|---|---|
| **classic** | static warm white (W channel) with a ±5 % filament wobble — reads as incandescent, not LED |
| **attract** | pulses → waves → chases → random inserts → zone sweep, auto-cycled with crossfades |
| **arena** | layout-driven: a "ball" runs the mapped zones, each hit flashes and decays, jackpot flash every 20 s |
| **night** | warm white capped at ~10 % |
| **rainbow** | full RGB hue sweep along the chain |
| **test** | R → G → B → W field cycle with one bright pixel walking the chain — wiring, colour order, LED count |
| **off** | dark (chain still powered and refreshed) |

Colour reference values from the vintage-incandescent palette are the UI presets:
amber `255/100/0/0`, golden `255/140/0/10`, warm white `0/0/0/255`.

**Colour order is a live setting, not a reflash.** SK6812MINI-RGBW is GRBW and
that is the default, but reels and clones vary. Run `test`: the field must cycle
**red → green → blue → white** in that order. If two of them are swapped, pick
another order from the dropdown (`grbw` / `rgbw` / `gbrw` / `brgw` / `rbgw` /
`bgrw`) — it re-types the chain immediately and is stored in NVS.

**Soft start**: at boot the firmware ramps global brightness from 0 to the saved
value over ~900 ms (`ARENA_SOFTSTART_MS`) instead of lighting 100+ pixels in one
frame, so the inrush into the bulk caps cannot trip a PSU's over-current hiccup.
The ramp is armed on the first *rendered frame*, not at the end of setup —
Wi-Fi bring-up blocks for 0.5–12 s in between and would otherwise consume the
whole window, leaving the chain to snap on at full brightness.

`test` mode is usable with a **single LED**: below two pixels the walking white
marker is suppressed, so the R → G → B → W field stays visible (otherwise the
walker would sit on the only pixel and hold it permanently white).

### Power metering

Every frame is summed channel-by-channel, converted to mA
(`17.5 mA` per die at full drive + `1 mA` quiescent per pixel), and if the total
exceeds `LED_POWER_BUDGET_MA` (default **9000 mA**) the whole frame is scaled
down uniformly before it is pushed. Colours and relative levels are preserved —
the wall just dims. The UI shows live current, the budget bar, and a
`⚠ power-limited` flag when the limiter is active. Set the budget from the UI to
match your PSU and fuse.

### Mapping the inserts

The chain is anonymous pixels; which LED is "spinner" or "pop-bumpers" is data
in `/arena_map.json`, editable from the **Insert map** panel:

1. **Walk LED** ◀ / ▶ blinks one pixel at a time — note which insert lights up.
2. Type the zone boundaries into the table (name, first LED, count).
3. **Save map** — written to LittleFS, used immediately by attract and arena modes.

The built-in template is a 100-LED starting point (`src/arena_map.cpp`), chain
order bottom-left → up the playfield → back panel:
`slings-outlanes, lower-inserts, drop-targets, left-orbit, pop-bumpers, spinner,
right-orbit, arena-letters, top-lanes, bonus-ladder, center-star, ramp, apron,
wash`. Rename and re-cut them to your actual Arena layout — nothing in the
firmware depends on those names.

### REST API

| Endpoint | Purpose |
|---|---|
| `GET /api/state` | JSON: mode, brightness, speed, colour, count, amps, budget, fps, ip |
| `GET /api/set?mode=arena&bright=180&speed=128&r=&g=&b=&w=&count=&budget=` | any subset |
| `GET /api/save` | persist current settings as the boot default (NVS) |
| `GET /api/zones` · `POST /api/zones` · `GET /api/zones/reset` | insert map |
| `GET /api/identify?led=N` · `?zone=N` · `?clear=1` | mapping wizard |
| `GET /api/wifi?ssid=…&pass=…` | store credentials, reboot |
| `POST /update` | OTA firmware upload |

Handy for Home Assistant / a shell script / a physical button elsewhere in the
room — e.g. `curl "http://arena.local/api/set?mode=night"`.

### Front-panel button

`GPIO0` (the DevKit BOOT button, or any NO push button to GND):
**short press** = next mode, **long press** = night mode toggle. Set
`ARENA_BUTTON_ENABLE 0` in `arena_config.h` to disable.

### Configuration

Everything hardware-ish is in [`include/arena_config.h`](include/arena_config.h):
LED count and data pin, frame rate, power model and budget, palette values,
Wi-Fi defaults, `LED_REPEATER_PIXEL` (§4 option B), `LED_CHAIN2_ENABLE`
(split the playfield across two GPIOs if one long run proves noisy).

---

## 8. Bring-up, with the boards in hand

Do this in order. Each step is cheap; the mistakes they catch are not.

**1 — One board, no ESP.** Buzz it out per §5. Then power it alone from a bench
supply at 5 V with a current limit of ~150 mA. It should draw ~1 mA and stay
dark. If it draws hundreds of mA or the limit trips, the LED is backwards or
there is a bridge — stop and fix that before touching the other 99.

**2 — One board + ESP, on the bench.** Flash the firmware (`uploadfs` first, then
`upload`), set the LED count to 1, and run `test` mode. You are checking four
things: the pixel lights at all (data path OK), the sequence is red → green →
blue → white (colour order OK — if not, change the dropdown, §7), the white step
uses the dedicated W die and looks neutral rather than pinkish (you really do
have the RGBW part), and current at full white is ~70 mA (the die drive is
healthy).

**3 — Three boards chained.** This is the step that proves the *link*, which is
the part of this build with no ground pin. Set count to 3, run `test`, and watch
the walking pixel reach board 3. Then try to break it: wiggle the hops, run a
longer hop, put the ESP's 330 Ω in and out. If board 3 flickers or shows garbage
while boards 1–2 are clean, you are seeing the level-margin problem from §4 —
apply option A (trim to 4.4 V) or option B (repeater pixel) *now*, not after the
playfield is populated.

**4 — Thirty boards, still on the bench.** Enough to be representative of a real
bus. Run `classic` at full brightness and measure: the voltage at the *far* end
of the bus (must stay above ~4.0 V), the total current (compare with the figure
the UI reports — they should agree within ~15 %), and how warm the bus wire gets
(it should not be noticeably warm at all). Leave it running for an hour.

**5 — Populate the playfield.** Fit boards to inserts, thread the bus pair, chain
the data hops **in the order you want them numbered**. A chain that follows the
artwork makes mapping far easier later.

**6 — Injection and fuse.** Star-wire the injection points back to the PSU, add
the bulk caps, fit the 10 A fuse. Power up with the brightness slider low, then
walk it up while watching the current.

**7 — Map and save.** Use the mapping wizard to name the zones, save the map,
pick your boot mode and brightness, then **Save as boot default**.

## 9. Whole-chain colour glitches

**Symptom: every so often the whole string flashes white (or a wrong colour) for
a fraction of a second, as if a frame had been dropped.** That reading is exactly
right. The pixel protocol is self-clocked and has no error checking: one bit
disturbed early in a frame shifts every pixel after it, so a single glitched bit
near the start of the chain repaints the *entire* wall for one frame (~16 ms).
There are only two families of cause.

**A — the refresh was starved.** The bit stream must not stall mid-frame. If the
WiFi/TCP stack delays the refresh long enough, the chain latches a half-written
frame. Since v1.1 the renderer has its own task pinned to core 1 above the web
stack, which removes the usual source of this on a dual-core ESP32.

**B — the data signal is marginal.** 3.3 V driving a 5 V chain sits right on the
threshold (§4); a bit lands ambiguously and the shift register takes it wrong.
Long data hops, a hop routed away from its ground return, or a missing series
resistor all make it worse.

### Telling them apart — one experiment

The web UI has **Glitch Test (Wi-Fi off 30 s)** (or `GET /api/radiotest?sec=30`).
It kills the radio, leaves the LEDs running, and reboots at the end of the window:

| During the window | Conclusion | Fix |
|---|---|---|
| flashes **stop** | the radio was starving the refresh | lower the refresh rate (LED Chain → Refresh Rate, try 30 Hz), keep the page closed when not in use |
| flashes **continue** | electrical | §4 option A (drop the bus to ~4.4 V) or option B (repeater pixel); shorten the data hops, run each hop alongside the bus, fit the 330 Ω at the ESP |

Do this before changing anything else — the two fixes have nothing in common, and
guessing costs an afternoon.

### If it is electrical

In order of how much they buy you per minute spent: put a diode in the +5 V feed
so the chain runs at ~4.3 V (instant, one part, §4 A); shorten the ESP → LED 1 wire
and twist it with its ground; check the 330 Ω is at the *ESP* end; make sure every
board-to-board hop runs along the bus rather than looping away from it; then the
repeater pixel (§4 B) if the chain must stay at 5.0 V.

---

## 10. Future (V2)

Motion sensor (PIR on a spare GPIO → wake from night mode), audio-reactive mode
(I2S MEMS mic), ambient light sensor for auto-brightness, IR remote,
MQTT / Home Assistant integration, BLE control, syncing several decorations over
Wi-Fi. The REST API already covers most of these from the outside — a Home
Assistant `rest_command` needs no firmware change at all.

---

## Files

| Path | What |
|---|---|
| `include/arena_config.h` | all hardware/tuning constants |
| `src/arena_main.cpp` | setup/loop, front-panel button |
| `src/arenaled.{h,cpp}` | render engine: effects, crossfades, gamma, power limiter, NVS |
| `src/arena_map.{h,cpp}` | insert map (zones), JSON load/save |
| `src/arena_net.{h,cpp}` | Wi-Fi, mDNS, web UI, REST, OTA |
| `data/arena.html` | web UI (LittleFS) |
| `data/arena_map.json` | default insert map (LittleFS, editable from the UI) |
| `tools/host_arenaphase_test.cpp` | host test for the animation clock (30 days of uptime in a second) |
| `data/logo.png` | Pinballs Store logo used by the web UI (replaceable) |
| `tools/arena_flash.sh` | one-command build + flash + monitor, with troubleshooting output |
| `hardware/arena-led-bom.csv` | BOM |
