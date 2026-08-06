# GottFA80 — field report from a System 80 bench

Ralf,

This is what we measured on real machines while building a switch-feedback coil diagnostic on
top of GottFA80. It is separated into **things we measured**, **bugs we found in the original
sources**, and **things we believe but have not proven** — the third list is there so you can
ignore it, not so you can rely on it.

Hardware used: a **Volcano** (System 80, game #667) on the bench for every measurement below,
plus an **Arena** (System 80B, game #709) for cross-checks that did not need the machine
powered. Everything marked *measured* was observed through the LISYcontrol register file on a
running machine, not simulated.

---

## 1. Bugs in the original sources

These are independent of anything we added, and they are the part of this report we think is
worth your time regardless of what you make of the rest.

### 1.1 `lib_common/detect_sw.vhd` — the door TEST switch commits settings only once per power-up

The `delay` state clears `long_push` **twice** and never clears `short_push`:

```vhdl
when delay =>
    check_counter <= check_counter + 1;
    if (check_counter > 125000) then
        long_push  <= '0';
        long_push  <= '0';     -- <-- should be short_push
        state <= Idle;
    end if;
```

On the door test switch (`detect_test_sw` in `SYS80.vhd`), `short_push` drives the EEPROM
`w_trigger(2)`, and the EEPROM module saves the 5101 NVRAM on any **change** of `w_trigger`.
A trigger bit that latches high produces exactly one edge per power-up — so "press the door
test switch to commit the settings" works the first time and never again. That is precisely
when an operator needs it, right after adjusting the book-keeping. `long_push` (→ `lisy_trig`,
diag entry) was already cleared correctly and its timing is unchanged by the fix.

### 1.2 `lib_common/SN7448_GTB.vhd` — digit `1` lights the wrong segments

`when '1' => Dout <= "00000001"` — that lights only the eighth bit, the decimal point. The
digit 1 must light segments **b and c**, i.e. `"01100000"`.

The file carries its own proof, so you do not have to take our word for it: the nine other
digits in `SN7448_GTB.vhd` are exactly the seven-bit patterns of your own `SN7448.vhd` with a
`0` appended — `'0'` = `1111110`+`0`, `'4'` = `0110011`+`0`, `'7'` = `1110000`+`0`, and so on.
`'1'` is the only entry of the ten that breaks the rule.

Scope, so we do not overstate it: `sn7448_gtb` is instantiated **only** by `boot_message.vhd`
(three sites). `SYS80.vhd` drives the game displays through the plain `sn7448`, so this affects
the **boot banner only**, not scoring digits during play.

### 1.3 `lib_common/boot_message.vhd` — the status digits are fed from the player 3/4 data

The third `sn7448_gtb` instance, which drives `bm_segments(17 to 24)` (the status / credit-ball
digits), takes `Din => Din_Seg_B` — the player 3/4 data — instead of `Din_Seg_C`. The process
loads `status_d` into `Din_Seg_C` at counts 12000/13000/14000/15000 and blanks it elsewhere, so
`Din_Seg_C` is the correct source and the status digits currently show player 3/4 content.

---

## 2. Measured facts

### 2.1 Drop-target switch polarity: **closed = target DOWN**

Method: read the switch matrix with the playfield at rest, knock both target banks down by
hand, read again. On Volcano, exactly the ten switches the manual assigns to the two banks
closed, and nothing else moved. This is stated as a fact and not an assumption because the
whole coil diagnostic depends on it.

An incidental result worth mentioning: the first read showed **nine** of ten closed. The tenth
target simply had not been pushed down. A switch-matrix read is a fast, non-invasive way to
find a target that is not seating.

### 2.2 Coil → switch relationships on Volcano (game #667)

Learned by pulsing each solenoid and watching the matrix, then compared against the manual's
switch-matrix drawing (E-21110) and its solenoid page:

| Solenoid | Manual name | Switches observed | Agrees with manual |
|---|---|---|---|
| 1 | Subway Ball Release | `10` | yes |
| 2 | Top Bank Drop Target | `02 12 22 32 42` | yes, all five |
| 6 | Right Bank Drop Target | `01 11 21 31 41` | yes, all five |
| 9 | Outhole | `20` opens, `40` closes | yes |

Solenoid 9 is the clearest case: one pulse, the ball leaves the outhole (`20` opens) and
arrives in the left trough (`40` closes) — the exact path the drawing predicts.

### 2.3 Lamp-driver outputs are numbered from **zero**

The lamp bit index equals the L number printed in the manual. Measured: the bit at index 12
fires the **Motor Relay**, which the Volcano manual calls **L12**.

Corroborated independently by PinMAME, `src/wpc/gts80.c`:

```c
// Lamp 0 controls GameOn relay (map as sol 10)
// Lamp 1 controls Tilt relay (map as sol 11) for S80 & S80A
gameOn = (coreGlobals.lampMatrix[0] & 0x03);
```

This also explains why the Arena manual lists an `L0` at all. Any UI that labels the first lamp
"1" is off by one against the manuals.

### 2.4 There are **9** CPU solenoids on all three families

`gts80.c:266-277` (`riot6532_2a_w`) decodes solenoids 1-4, 5-8 and 9 with **no family test**
whatsoever, and `lisy80.c:754-790` emulates the same 74LS139 arrangement. The Arena (80B)
manual's own self-test lists `Sol.1` through `Sol.9` and nothing beyond, testing the relays
separately and identifying them by **lamp driver** number.

If you have seen a Visual Pinball table for a 80B title declaring `SolCallback(10)`: that is the
GameOn relay, which lives on a lamp output and is mapped as a pseudo-solenoid by PinMAME — not
a tenth driver.

### 2.5 Some real coils hang off LAMP outputs, and are invisible to a solenoid test

On Volcano, per the master-driver schematic: **L15** Ball Release, **L16** Hole Kicker,
**L8** Fire Pit, **L14** Right Ball Gate, plus the L12 Motor and L13 Ball Saver relays. On
Arena: **L13** Inside Gate, **L14** Outside Gate.

Any diagnostic that only exercises the 9 solenoids will silently miss these mechanisms. We flag
them per title so the operator knows to fire them from the lamp page.

### 2.6 The game number is only announced on a **change**

The FPGA emits its game number when `game_select` changes, so a board that has merely been
switched on reports nothing at all. Anything keying per-title data off it must handle "never
announced" as a normal state rather than folding it into game 0.

### 2.7 lisyctrl timings, as compiled

`refire_ms = 40` (a premature re-fire is **refused**, not delayed — and a refused fire reads
exactly like a dead coil if you are not watching for it), `max_pulse_ms = 150` (a larger request
is clamped and latches `COIL_FAULT` b0), `wd_timeout_ms = 120` (with outputs armed, 120 ms of
SPI silence kills the coil and latches b2). Practical consequence for anyone driving the
register file: you must keep reading registers even while waiting out a cooldown, or the
watchdog trips and latches a fault against whichever coil happens to be next.

---

## 3. Off-board memory: SPI NOR in place of the SD card

We replaced the SD card as the game-ROM store with a serial NOR flash — `lib_common/nor_flash.vhd`,
a drop-in for `SD_Card.vhd` with an identical port map, so `SYS80.vhd` needs no other change.

- W25Q32 (JEDEC `0xEF4016`), 4 MB ≈ 256 games. Game N at `base_addr + N*0x4000`, and the 16 KB
  image is byte-for-byte the same content as the SD image, so nothing downstream changes.
- **Measured:** a game boots from NOR on the real machine, with the bitstream burned to EPCS.
  The behavioural testbench (`sim/tb_nor_flash.vhd`) also passes.
- Motivation, beyond removing a mechanical part: the ESP32 sits on the same SPI pins, so the
  store can be **(re)programmed over WiFi** — erase 4 KB sector, page-program 256 B, read-back
  verify.

### One open question we could not settle, and you can

`norprog::enter()` on the ESP side takes the bus by **holding the FPGA in reset**, on the
assumption that the FPGA then tri-states MOSI/CLK and releases CS. Our own note in
`NOR_FLASH.md` already flags this as unconfirmed ("TODO: confirm release via the Debug-line
handshake"), and our working understanding from the bench is that the release actually happens
in **diag mode** (`lisy_active`), not in reset.

We have not instrumented this properly, so we are asking rather than reporting: **in your top
level, does holding `Reset` low actually release the shared SPI bus, or is `lisy_active` the
only condition that does?** If it is the latter, anyone programming the NOR from the ESP is
contending with the FPGA on the same pins and getting away with it by luck.

## 4. The goal behind all of this: let an owner dump their **own** EPROMs

This is the part we care most about and the reason the NOR store exists at all.

We do not want to distribute game ROMs, and we do not want users downloading them. The clean
path is that the owner copies **their own chips** into their own board: nothing is distributed,
nothing is downloaded. For that the machine has to be able to read an EPROM by itself — no PC,
no separate programmer.

Design (`EPROM_READER.md`, `src/epromdump.{h,cpp}`): the ESP has only ~5 free GPIOs, far short
of the 23 signals an EPROM needs, so the dump runs through **2× 74HC595** (address + /CE + /OE,
serial out) and **1× 74HC165** (data, serial in) on five dedicated pins. Roughly 6-8 € of logic
on a small daughterboard. Covers 2716 / 2732 / 2764.

The part that may interest you: the Gottlieb 80/80A **system mask ROMs U2/U3 (2332, 4K×8)** come
out with **firmware alone — no adapter and no 7404**. A 2332 sits bottom-justified in a
2764-wired ZIF socket, which lands its three awkward pins on controllable 595 outputs
(A11→Q13, CS1→Q14, CS2→Q11), so the chip-select polarity is just a firmware constant:
U2 = pin 20 high + pin 21 low, U3 = both high.

**Status: designed and written, NOT proven.** `EPROM_READER_ENABLE` is 0 and the daughterboard
has not been built, so no chip has actually been read. We are describing an intent and a
schematic, not a result. Honest limits are in `EPROM_READER.md`: a 24-pin jumper is needed, the
6530 RIOT is unreadable by any reader, and merging raw dumps into a 16 KB image is a separate
step. A 50 € USB programmer is less effort for the same chip coverage — this only makes sense
if "dump your own, integrated" is a product goal, which for us it is.

## 5. The method, and two mistakes worth avoiding

The diagnostic watches the switch matrix: a healthy coil moves something, and almost everything
that moves is watched by a switch. The trap is that **the coil→switch link is conditional on
playfield state**. A drop-target reset moves nothing when the targets are already up; an outhole
kicker moves nothing with an empty outhole. A naive implementation therefore reports a perfectly
healthy coil as "no reaction" — a false accusation against good hardware, which is worse than
having no test at all.

Two concrete mistakes we made and fixed, in case you build something similar:

1. **Fixed repetition destroys one-shot mechanisms.** Pulsing each coil three times and
   accepting a switch at two hits sounds like sensible noise rejection. But every
   switch-observable coil on a pinball is one-shot: the first pulse consumes its own
   precondition, so pulses two and three are guaranteed silences and a healthy coil scores 1/3
   — below threshold, signature discarded. The repetition count has to adapt and the honest
   denominator reported (1/1).

2. **A "can it still move?" test must be per mechanism, not per coil.** We gated firing on
   whether *any* signature switch could still travel. Mixed-direction signatures are the norm —
   Volcano's outhole coil opens `20` and closes `30`/`40` — so with the outhole empty the trough
   switches still looked armable, the coil fired into nothing, and the verdict came back "no
   reaction" on healthy hardware. The rule that works: never conclude "dead" while any part of
   the expected reaction was physically unable to happen.

---

## 6. Believed, not proven

Listed so you can discount them.

- Family decode (80 / 80A / 80B from the game number) compiles and simulates; never validated
  against a real 80A.
- The 80B alphanumeric display back-end is absent. `disp80b_diag` does not fit — the design is
  at roughly 96% of LABs with the diagnostic in, and adding it reaches 100%.
- `boot_message` produces nothing visible on our Volcano's display glass. We worked around it
  rather than diagnosing it, so treat that as unexplained, not as a defect report.

---

## 7. Licence

Everything here derives from GottFA80 and is offered back under **GPL-3**, the licence of the
original. The bug fixes in section 1 are three-line changes to your files and are yours to take
without attribution if that is simpler. Anything we wrote from scratch is marked as such in its
own header and can be relicensed at your request.
