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
| Controller | **WEMOS/LOLIN D1 Mini ESP32** (ESP32-WROOM-32, 4 MB), data on **GPIO27**. The board on the bench carries a **CP2104**, not the CH340C this doc assumed: `/dev/cu.usbserial-*`, MAC `68:09:47:48:7a:1c`, ESP32-D0WD-V3 rev 3.1, 4 MB flash. Batches vary — do not hard-code the bridge. |
| Firmware | complete, builds for 4 targets, adversarially reviewed (5 defects found and fixed before first power-on) |
| Hardware bring-up | **41 pixels chained and working** (2026-08-01) — main playfield plus the upper deck; 34 placed on inserts, the 7 newest still to map |
| Attract mode | **Arena's own**, captured from the game ROM and replayed on the wall (§7) |

### Measured on hardware — 2026-07-31, one LED board

First power-on of the project. Nothing here is a model or an estimate; each line
is something the bench returned.

| | Measured |
|---|---|
| Colour order | **GRBW confirmed.** Commanded pure red (`mode=classic&r=255&g=0&b=0&w=0`), the pixel lit **red**. The `ARENA_ORDER_DEFAULT "grbw"` guess was right. |
| Bus voltage | supply **5.3 V**, one series **silicon** diode, **4.53 V** at the LED. Drop 0.77 V — a plain PN junction, not a Schottky (~0.3 V). |
| Data margin | works at that voltage, and the arithmetic says why: `VIH = 0.7 x 4.53 = 3.17 V` against the ESP32's 3.3 V, so ~130 mV of margin. This is **option B's mechanism** (§4: drop the LED's VDD with series diodes) applied to the whole bench rather than to a repeater pixel. It must NOT be "corrected" up to 5.0 V, where VIH becomes 3.50 V and the ESP is out of spec. |
| ⚠ Does not scale | that diode is fine for 1-3 LEDs and **wrong for the playfield**. A junction drop is current-dependent (0.77 V at bench current, more under load), so the bus voltage would sag as the wall lights up, and the whole chain current — up to 10 A at 150 LEDs — would have to pass through one diode that dissipates `0.8 V x I`. At 10 A that is 8 W in a part usually rated well under 1 A. For the build, use **option A instead**: trim the PSU to 4.4 V and delete the diode. Same voltage at the pixel, no heat, no load-dependent sag. |
| Power estimator | self-consistent: predicted 4.4 mA in TEST mode and 18.5 mA on one die at full drive, both matching a hand calculation from the source. **Not yet checked against a multimeter** — the model constant `LED_MA_PER_CHANNEL` is still unvalidated. |
| Network | STA on the house WiFi, driven over REST from the workstation; `up` monotonic, heap stable ~221 kB, 63 fps. |

**34 pixels chained, 2026-07-31.** Nineteen lit, the rest dark. The break was a
**dead SK6812**, not the design: VDD was good on the first dark board, the hop
had continuity, IN/OUT were the right way round, and swapping that board with a
working one moved nothing — a replacement brought the whole chain back. Worth
recording because the symptom is indistinguishable from a wiring fault: a pixel
is a repeater, so one that cannot run its logic takes everything downstream with
it, whether it is unpowered or dead.

Diagnosis order that worked, cheapest first: measure VDD on the first dark board
(an unpowered pixel is far more common than a dead one), then hop continuity,
then IN/OUT orientation, then **swap the suspect board with a known-good one** —
that last test is the one that separates "this board is dead" from "this position
is bad", which are different repairs. The BOM's +10% spares exist for this.

It also proved the chain itself: nineteen consecutive regenerations, the repeater
and the 3.3 V first hop all behaved.

Still open after this session: the **real** per-LED current (multimeter in series
with +5 V; the firmware predicts 618 mA for 34 pixels on the white die at full
brightness, which is the easiest point to check it against).

### Roadmap
- [x] v1.0 — firmware: 7 modes, insert map, power meter + limiter, web UI + REST + OTA
- [x] LED boards designed, panelized, assembled and delivered
- [x] Adversarial preflight review — 5 defects found and fixed before first power-on
- [x] D1 Mini ESP32 target (GPIO27) + one-command flash script
- [x] §8 steps 1–2 — one board lit, colour order GRBW **confirmed on hardware**
- [x] **Data level settled by measurement, and built: option B.** Chain and ESP both
      straight off the 5.3 V supply; only the hidden repeater is dropped, through
      2 x 1N4148, and held dark at 4.2 V. No diode carries chain current, so nothing
      here limits how many pixels can be added.
- [ ] Playfield populated, bus + injection + fuse — **in progress**
- [ ] Per-LED current confirmed with a multimeter (model says 18.5 mA on one die)
- [x] **Insert map built on hardware** — 34 pixels placed on real matrix lamps
      (L1..L48) by clicking the plan, covering the full height of the playfield;
      geometric `arena` mode confirmed running off it (current swings 56-123 mA
      as the wave crosses, which a chain-order effect would not do)
- [x] **Arena's own attract mode**, captured from the ROM and replayed on the wall
- [x] **Arena's own attract mode** captured from the ROM and replayed (§7)
- [x] Incandescent filament model — black-body colour, T^4 luminance, radiative decay
- [x] Per-insert colour and per-insert renaming, edited on the plan
- [ ] Remaining inserts placed (7 upper-deck pixels outstanding), boot preset saved
- [ ] V2 ideas: motion sensor, audio-reactive, MQTT / Home Assistant

**Next action: populate the playfield** (§3 and §8 step 4). The electrical design
is settled and built; what is left is wiring discipline — bus, injection every
30-40 pixels, fuse — and doing it **in stages**, powering up at ~10, ~30 and ~75
pixels rather than wiring 150 and hoping. Each stage costs a minute and tells you
which hop broke; the finished wall does not.

Open question still worth closing: the real per-LED current against a multimeter.
The whole power budget rests on `LED_MA_PER_CHANNEL = 17.5 mA`, and that constant
is still the one number in this project that has never been measured.

### Working on this locally

The firmware is on `main`.

```sh
git clone https://github.com/Vapi27/gottfa-esp32.git
cd gottfa-esp32

pip install --user platformio     # Linux/macOS; on Windows: pip install platformio
tools/arena_flash.sh              # build + flash + monitor (Windows: use the pio commands in §7)
```

Per-OS notes. **The USB bridge varies between batches** — the bench board is a
**CP2104**, others carry a CH340C — so check what you actually have rather than
installing a driver on faith (`pio device list` names it):

- **Linux** — `sudo usermod -aG dialout $USER`, then log out and back in. Port is `/dev/ttyUSB0`.
- **macOS** — CP210x needs no driver on macOS 11+; a CH340 does. Port is
  `/dev/cu.usbserial-*` or `/dev/cu.wchusbserial*`.
- **Windows** — install the matching CH340 or CP210x driver; port is `COM3`/`COM4`/…
  Run the `pio` commands from §7 directly (the shell script needs Git Bash or WSL).

Once the board is mounted, you do not need USB at all: firmware and web UI both
go over WiFi (§7, "Build and flash").

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

Feed **only the first LED** through two series **silicon** diodes so it sits
around 4.1-4.4 V and therefore accepts 3.3 V data. Its **DATA OUT is a
regenerated, full-VDD-swing signal**, which then drives the rest of the chain at
a full 5 V.

> **Use silicon (1N4148), not Schottky — corrected 2026-07-31.** This section
> used to specify 2 x 1N5819 "since LED1 draws <= 70 mA". That reasoning is wrong
> as soon as `LED_REPEATER_PIXEL 1` is set, because the firmware then keeps that
> pixel **dark**: it draws about **1 mA**, not 70 mA. A Schottky at 1 mA drops
> only ~0.2 V, so two of them would leave the repeater near 4.9 V (on a 5.3 V
> supply) — a VIH of 3.43 V, *above* the ESP32's 3.3 V. The recommended circuit
> would not have worked. A 1N4148 drops ~0.6 V at 1 mA, which is what this needs.
> Diode current here is ~1 mA either way, so the rating never matters; the
> forward drop at that current is the whole point.

```
 +5V ──┬──────────────────────────────▶ LED2..n  VDD (5.0-5.3 V)
       │
       └─▶ ▶| ─ ▶| ─┬─▶ LED1 VDD (~4.1-4.4 V)  2 x 1N4148 (silicon; the pixel is
          (2 x silicon)                          held dark, so it draws ~1 mA)
                    └─ 100 nF to GND

 ESP32 GPIO27 ── 330 R ──▶ LED1 DATA IN       LED1 DATA OUT ──▶ LED2 DATA IN
```

**Measured on the bench, 2026-07-31** (5.3 V supply, 2 x 1N4148, one visible LED):

| Repeater state | Its VDD | Chain needs `0.7 x 5.3` | Margin |
|---|---|---|---|
| driven **lit** (before `LED_REPEATER_PIXEL 1`) | **3.70 V** | 3.71 V | **-10 mV** — worked, out of spec |
| held **dark** (after) | **4.2 V** | 3.71 V | **+490 mV** |

That 0.5 V swing is the entire argument for keeping the pixel dark, and it is
current, not theory: at ~70 mA a 1N4148 drops ~0.8 V, at ~1 mA about 0.55 V.
Wire the repeater as a visible pixel and its supply — hence the whole chain's
noise margin — moves with whatever colour it happens to be showing.

**Measure it, do not trust the arithmetic**: with everything powered, the
repeater's VDD must land between **4.0 and 4.4 V**. Above 4.4 V the ESP loses its
input margin (`VIH = 0.7 x VDD` climbs past 3.3 V) — add a third diode. Below
4.0 V the repeater's own output starts crowding the `0.7 x 5.3 = 3.71 V` the rest
of the chain demands — drop back to one diode. The window is wide; hitting it
blind is not.

Mount that first pixel **hidden behind the frame** and set
`LED_REPEATER_PIXEL 1` in `include/arena_config.h`: the firmware keeps it dark
and shifts all indices, so LED 0 in the web UI is still your first visible
insert.

**How to tell which build is actually running**, with no serial cable: read
`/api/state` in `mode=off`. The idle estimate is `(count + repeater) x 1 mA`, so
`count=1` reports **1.0 mA** without the repeater and **2.0 mA** with it. That
number settled the question on the bench when an OTA left no other trace.

**OTA over WiFi answers nothing — that is normal.** `curl -F` returns
`HTTP=000` with the full firmware uploaded, because the handler restarts the
board before the TCP response is flushed. Do not conclude the update failed and
do not re-flash blindly: wait ~10 s, read `/api/state`, and check that `up` has
reset and the mA signature above has changed.

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

**Once it is on the wall, use WiFi instead** — both halves update over the air,
which is the only sane option for a board mounted behind a playfield:

```sh
pio run -e arenaled_d1mini32                      # firmware
curl -F u=@.pio/build/arenaled_d1mini32/firmware.bin  http://arena.local/update

pio run -e arenaled_d1mini32 -t buildfs           # web UI
curl -F u=@.pio/build/arenaled_d1mini32/littlefs.bin "http://arena.local/update?target=fs"
```

Both **return `HTTP 000` when they succeed**: the board restarts before the
response is flushed. Do not re-send. Wait ~10 s and check `/api/state` — `up`
back near zero means it took. Flash the firmware before the filesystem if you
are doing both, and note that `?target=fs` unmounts LittleFS, so the web UI is
unreachable from the moment the upload starts until the reboot.

The same thing over USB, by hand:

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

### The original attract mode

`attract` does not imitate Arena's attract mode: it **replays it**, from the
game's own ROM.

`tools/capture_attract.cpp` runs `prom1.cpu` / `prom2.cpu` under PinMAME and
writes down the lamp matrix the ROM drives, every 50 ms of **game** time.
`data/arena_attract.bin` is 120 s of that — one 64-bit lamp mask per frame,
19 KB, loaded to RAM at boot.

> **Pace the capture on the emulated clock, never on the host's.** Headless
> PinMAME is not throttled: on this Mac it runs Arena at **3x real time**
> (measured — the driver is `MDRV_FRAMES_PER_SECOND(60)` and the display
> callback fires 180 times a wall second). The first capture sampled every 20 ms
> of wall clock, which is 60 ms of game time, and played back at 20 ms: the wall
> ran the right sequence three times too fast. The tool now waits on
> `OnDisplayUpdated`, which ticks once per emulated frame, so the interval is
> right whatever the host is doing.

```sh
# PinMAME source lives in ../gottfa-upstream/lisy_5_28 and builds natively:
cp cmake/libpinmame/CMakeLists_osx-arm64.txt CMakeLists.txt     # or linux-x64
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=ON . && cmake --build build -j8
cp arena.zip ~/.pinmame/roms/
clang++ -std=c++17 -O2 -I src/libpinmame tools/capture_attract.cpp \
        -o capture_attract -Lbuild -lpinmame
./capture_attract 45 > raw.json          # then pack to data/arena_attract.bin
```

It lands on the wall because every identifier lines up: the ROM drives lamp
`n`, the playfield plan calls that insert `L<n>` — both extracted from the same
machine — and the wizard recorded which pixel sits on it. All 44 lamps the ROM
touches were checked against the plan before any of this was trusted; none were
orphaned.

Falls back to the five generic animations when either half is missing (no
capture on the board, or no pixel placed yet).

**Lamps 1, 2 and 3 stay dark, and that is correct.** Over five minutes of
captured attract they are never lit; L1 comes on only during the first 0.7 s,
which is the power-on lamp test, not attract. A pixel placed on one of those
inserts looking dead in `attract` is the ROM, not the wiring — check it with
the wizard, which lights any pixel on demand.

**One liberty is taken**, and it is the only thing here not in the ROM: each
pixel rises over ~40 ms and falls over ~90 ms. #47 bulbs do not switch instantly,
and without that envelope the chases read as digital blinking rather than as a
playfield.

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
in NVS on the board, filled in from the web UI — no laptop, no serial cable, a
phone under the playfield is enough.

**The mapping wizard (use this one).** Web UI -> **Mapping wizard** -> *Start
walking*. The board draws the playfield, spotlights one pixel, and you click the
insert it sits under. Nothing to read off, nothing to type.

- The plan is the real Arena: **99 inserts** with their true positions and their
  Gottlieb lamp names, extracted from the Visual Pinball table by
  `tools/vpx_inserts.py` and shipped as `/arena_pf.json`. It is the machine, so
  it never changes.
- What you are filling in is only **which chain index landed on which insert** —
  150 bytes, saved to the board **on every click**. A power cut at pixel 80 costs
  you pixel 80, not the other 79.
- A filled dot already holds a pixel and shows its chain number, so you can see
  at a glance what is left and spot a double assignment.
- *Not on an insert* skips a pixel that lights nothing (a wash or a spare).

This is what makes the effects spatial: `arena` mode switches to geometry as soon
as one pixel is placed — a wave climbing from the flippers to the back panel, a
ripple leaving the centre — instead of walking zones in cable order. Unplaced
pixels stay on the base wash rather than pretending to be at (0,0).

> Scripting the map instead? POST the JSON **raw**. `curl -d` sends it as
> `application/x-www-form-urlencoded`, which this web-server stack folds into
> request params and never hands to the body handler, so the board sees an empty
> body. Use `--data-binary` with `-H 'Content-Type: application/json'`. The
> firmware now says exactly that instead of "bad map json".

The built-in template is a 100-LED starting point (`src/arena_map.cpp`), chain
order bottom-left → up the playfield → back panel:
`slings-outlanes, lower-inserts, drop-targets, left-orbit, pop-bumpers, spinner,
right-orbit, arena-letters, top-lanes, bonus-ladder, center-star, ramp, apron,
wash`. Rename and re-cut them to your actual Arena layout — nothing in the
firmware depends on those names.

### Editing the inserts

The plan does two jobs behind a mode switch in the wizard.

**Place pixels** is the walk described above. **Edit inserts** selects an insert
instead, lights whichever pixel sits on it so you can see what you are touching,
and lets you change two things about it:

**Its name.** The shipped labels are derived from the Visual Pinball table plus
PinMAME's `GTS80_lamp2m` offset, and that is a **guess**: it matched the one
insert checked against the real playfield and missed another, so the offset is
not the constant it looks like. The machine's own documentation is the
authority, and only its owner has it. A rename overrides the shipped label and
persists in NVS; `lamp` — the index the ROM sequence is addressed by — is never
touched, so renaming cannot break the attract mode. `/api/latch` accepts either
name.

**Its colour.** On a real playfield the colour is the moulded plastic, so it
belongs to the insert and survives re-routing the chain. It **multiplies** the
bulb rather than replacing it: the plastic sets the hue, the filament sets how
hard it is lit. An insert with no colour shows the bulb's own, which is the
right default — a wall where every insert is tinted looks like a light show, one
where a few are looks like a playfield.

    /api/insert?ins=6&name=L48&r=255&g=0&b=0&w=0
    /api/insert?ins=6&clear=1

### Lamps latched from the last game

A machine that has been played does not return to a virgin attract: some lamps
stay lit from the last game. A capture taken from a cold boot cannot contain
that, which is why the LOCK "L" was missing on the wall while it is lit on the
real machine — confirmed in the emulator, where a coin and a START light exactly
that lamp.

Rather than doctor the captured sequence, those lamps are held lit on top of it:

    /api/latch?n=L9,L48      hold these lit through attract (machine's names)
    /api/latch?clear=1       release them

Which keeps "this is the ROM" and "this is my machine" true at the same time.

### Music mode

`music` makes the wall follow the room. Spatial, because the pixels have
positions: **bass breathes the field, a beat ripples out from the playfield
centre, treble sparkles random inserts**. The power limiter meters every frame,
so a loud passage cannot overrun the supply. With no signal it breathes gently
instead of going dark.

Two ways to feed it:

- **A mic on the board** (autonomous): MAX9814 (auto-gain, ~2) or MAX4466 —
  `OUT -> GPIO34, VCC -> 3V3, GND -> GND`, then set `ARENA_MIC_ENABLE 1` in
  `arena_config.h` and reflash. GPIO34 is ADC1, so it coexists with WiFi.
  The flag defaults to **0**: a floating ADC pin reads WiFi noise as music and
  the wall dances to static (measured — 290 mA of it).
- **`/api/music?e=..&b=..&t=..`** (0..255, ~10-20 Hz): anything that can hit
  REST drives the wall — a PC script watching an audio output, a phone app.
  An external push silences the mic for 2 s, so both can coexist.

Browser-mic from the web page is NOT offered: browsers block `getUserMedia` on
plain HTTP, and the board cannot serve HTTPS a phone will trust.

### Live look controls

Four settings, all in NVS, all reachable from the web UI, none needing a reflash:

| | |
|---|---|
| **Glow** (`gi`) | general illumination behind the attract. A real Arena's GI stays lit, so inserts the ROM never drives still glow — but a wall piece that never goes fully dark is taste. 0 is properly off. |
| **Warmth** (`warm`) | which die carries a hot filament. 0 is the pure spectral split, which is colorimetrically right and looks orange; 255 hands it to the white die. Both ends are the same black-body curve. |
| **Filament** (`inc`) | the incandescent simulation itself. Off is a plain switch — no thermal lag, no colour ramp — which suits inserts that carry their own colours. |
| **Speed** | scales the ROM sequence. 128 is the machine's real rate. |

### Porting to another table

The firmware is game-agnostic: it reads two files and never needs recompiling
for a different machine. Everything table-specific is data, produced by one
command:

```sh
tools/mkgame.sh "Genesis (Gottlieb 1986).vpx" genesis
pio run -e arenaled_d1mini32 -t buildfs        # then OTA ?target=fs
```

What that runs, and what each step needs:

| Step | Tool | Needs |
|---|---|---|
| Inserts (positions + lamp numbers) | `tools/vpx_inserts.py` | the game's **VP table** (.vpx) |
| Attract capture (the game's own) | `tools/capture_attract` | libpinmame built once + the **ROM zip** in `~/.pinmame/roms/` |
| Pack | `tools/pack_attract.py` | — |

Per-game knowledge is optional and lives in `tools/games/<name>.json`: author
fixes for VP-table naming errors, and the lamp chart from the machine's service
manual (functions shown on the plan tooltips). Without it the plan still works —
labels come from the VP object names. Arena's config documents both kinds of
entry, and the OCR'd manual corpus in `../gottfa-tools/pdfocr/out/` (128
Gottlieb manuals) is where the lamp charts come from.

Two lessons from Arena worth carrying to every port:

- **Trust the VP names until the real playfield disagrees**, then fix the
  *object*, not the numbering: VP authors bind lights by name to
  `Controller.Lamp(n)`, and a misnamed object is wired to the wrong lamp in a
  way VP itself never reveals (Arena's #1 top rollover was bound to the Game On
  relay — invisible in VP, dead on the wall).
- **A cold-boot capture has no game-latched lamps.** If an insert is lit on the
  real machine's attract but never in the capture, it is probably a game lamp
  held from the last game (`/api/latch` covers it) or a mis-bound object, not a
  wiring fault.

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
| `tools/host_arenaphase_test.cpp` | host test for the animation clock (30 days of uptime in a second) |
| `tools/arena_flash.sh` | one-command build + flash + monitor, with troubleshooting output |
| `hardware/arena-led-bom.csv` | BOM |
