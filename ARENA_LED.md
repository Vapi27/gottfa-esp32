# Arena Wall-Art LED

Turning a Gottlieb **Arena** playfield into an illuminated wall piece: realistic
pinball-style lighting, animated RGBW effects, Wi-Fi control, low voltage,
modular and repairable, designed from day one for **up to 150 LEDs**.

Decorative only — no gameplay electronics, no FPGA, no sound. The firmware lives
in this repo as its own PlatformIO environment (`arenaled`) and shares nothing
with the GottFA80 companion app.

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
   ESP32-S3 ── 330 R ──▶ DATA ──▶ LED1 ▶ LED2 ▶ … ▶ LED150
   (USB-C, Wi-Fi, OTA)                 (data only through the small connectors)
```

| Block | Choice | Why |
|---|---|---|
| LED | **SK6812MINI-RGBW-NW-P6** | addressable, integrated controller, RGB **+ dedicated neutral white**, 3.5 mm package, single wire |
| MCU | **ESP32-S3** (DevKitC-1) | Wi-Fi, USB-C, OTA, RMT peripheral drives the chain with ~0 CPU |
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

### Signal hygiene (do this regardless of the option chosen)

- **330 Ω in series** at the ESP output, physically at the ESP end.
- **100 nF** across VDD/GND on **every** LED PCB (§5) — this is not optional at 150 pixels.
- ESP → LED1 wire **short** (< 15 cm) and **twisted with its GND return**.
- Every board-to-board data jumper carries **DATA + GND together** — never let
  the data return current find its way home through the power bus.

---

## 5. LED PCB

One tiny board per insert. Each carries:

- 1 × SK6812MINI-RGBW-NW-P6
- 1 × 100 nF X7R (0603) decoupling, right at the LED pins
- 2 × mounting holes
- 2 × data connectors (in / out)
- 2 × large solder pads for the power tap

```
        ┌───────────────────────┐
   ○    │         +5V           │    ○      ○ = 2.2 mm mounting hole
        │   ┌───────────────┐   │           (M2, or a 1.6 mm brad/screw)
 DATA   │   │   SK6812MINI  │   │   DATA
  OUT ◀─┤   │     RGBW      │   ├─◀ IN
 +GND   │   └───────────────┘   │   +GND
        │    ▪ 100 nF           │
   ○    │         GND           │    ○
        └───────────────────────┘
             ~12 × 12 mm
```

### Fab spec (JLCPCB)

| Parameter | Value |
|---|---|
| Size | 12 × 12 mm (fits between playfield inserts; 10 × 10 mm is possible if tight) |
| Layers / copper | 2 layers, 1 oz |
| Thickness | **1.0 mm** (thin = sits flush under the playfield) |
| Solder mask | **black** (disappears against the playfield underside) |
| Finish | **ENIG** (flat pads, kinder to the SK6812's side pads than HASL) |
| Power traces | 1.0–1.5 mm |
| Data traces | 0.3 mm |
| Power pads | 2.5 × 3 mm, mask-opened, tinned — takes an AWG22 tap directly |
| Data connectors | JST-PH 2.0 mm 2-pin (robust, repairable) or JST-SH 1.0 mm if space is critical |
| Optional | 100 Ω series in the local DATA IN trace — tames ringing on flying leads |

### Panelize — don't order 50 separate boards

At this size, board count is irrelevant to the price; **area** is. A 100 × 100 mm
panel holds **8 × 8 = 64** boards of 12 mm with V-cut. Five of those panels is
320 boards for the price of five small PCBs. Before ordering, price both in the
JLCPCB cart:

1. 50 individual boards, versus
2. 1 panel of ~50–64 boards (V-cut, 100 × 100 mm).

Option 2 normally wins by an order of magnitude. Ask for **V-cut** (straight
lines only) rather than mouse-bites — cleaner edges on a board this small.

### Assembly

The SK6812MINI has side-wettable pads under a 3.5 mm body: hot plate or hot air
with solder paste and a stencil is easy; a soldering iron alone is not. If you'd
rather not, JLCPCB assembly on a panel is an option — check current stock for the
SK6812MINI variant before committing the design.

---

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
| 10 | JST-PH 2.0 2-pin connector + housing | 200 | 2 per board |
| 11 | Pre-crimped JST-PH jumpers | 100 | board-to-board data |
| 12 | AWG18 wire (red/black) | ~10 m | power bus |
| 13 | AWG22 wire | ~10 m | per-LED power taps |
| 14 | M2 screws / brads | 200 | 2 per board |

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

```sh
pio run -e arenaled -t uploadfs     # web UI + default map -> LittleFS (do this first)
pio run -e arenaled -t upload       # firmware
pio device monitor -e arenaled      # 115200
```

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

## 8. Build order

1. **Bench first, 8 LEDs on a breadboard.** Prove the data path (§4) before
   anything is glued to a playfield. Run `test` mode: R/G/B/W must be correct
   and the walking pixel must reach the last LED.
2. Fit the PCBs to the playfield inserts, two screws each, from the back.
3. Lay the AWG18 bus, then the four injection points, then the per-LED taps.
4. Chain the data jumpers **in the order you want them numbered** — a chain that
   follows the artwork makes the effects far easier to map.
5. Fuse, power up at low brightness, check the far end of the bus with a meter
   under a full-white frame (`classic` at 255): it must stay above ~4.0 V.
6. Map the zones with the wizard, save, then set your boot mode and
   **Save as boot default**.

## 9. Future (V2)

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
| `hardware/arena-led-bom.csv` | BOM |
