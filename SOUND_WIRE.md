# SOUND_WIRE — the FPGA→ESP sound-command contract

**What this is.** The DFPlayer module is gone: the ESP + PCM5102A play the sound, driven by
*samples*, and the only thing the FPGA has to do is tell the ESP **exactly what the game CPU put
on the sound bus**. Not emulate the sound board — report the bus. This file is the contract for
that report: what events the FPGA must send, on which byte codes, and what the ESP guarantees in
return. It is the spec the FPGA side implements against; the ESP side of it is already written
(`src/fpgalink.cpp`, `src/sndmap.cpp`) and ships decoding these bytes today.

Companion documents: `WIRING.md` §2 (the physical 1-wire link), `SOUND_MAP.md` (how a title's
commands become samples, no firmware change), `lib_common/sound_link.vhd` (the FPGA transmitter,
whose byte-map comment is the allocation authority).

---

## 1. What is wrong with the wire today

Three defects, each verified against the actual sources rather than assumed:

**(a) Events are coalesced — a header/payload pair silently loses its bank.**
`sound_link.vhd` keeps *one* pending command in *one* register:

```vhdl
if sound /= sound_r then sound_r <= sound; snd_pend <= '1'; end if;   -- every clk
```

`sound_r` is overwritten by the newest value while `snd_pend` is still waiting for its slot, and
the arbiter emits at most one byte per byte-time (86.8 µs at 115200) — with sound sitting *below*
all six level classes in the priority chain. A 6502 at 895 kHz can write the sound latch again
4–5 µs later (`STA abs` = 4 cycles), so any two-write sequence — a bank header followed by its
payload, or the same cue fired twice with a bus release between — can collapse into a single
byte. The bank is lost and the ESP plays the wrong sample, or nothing.

**(b) There is no strobe qualification, so lamp animation injects phantom commands.**
In `SYS80.vhd`:

```vhdl
Sound_S1 <= ((not u6_pa_out(0) and not u6_pa_out(4))) when mytest='1' else '1';   -- ... S2, S4, S8
sn74175_Sound16: ... clk => clk_Z3, D => U6_pb_out(3 downto 0), Q(1) => Sound_S16
clk_Z3 <= '1' when U6_pb_out(7 downto 4) = "0011" else '0';   -- DS3 = a LAMP-latch write
```

So the 5-bit vector is purely combinational on the RIOT U6 port-A latch *plus* a lamp-latch bit,
and `sound_link` triggers on any change of that vector. Two consequences:

* every **lamp write** that flips that one lamp bit changes `Sound_Meta(4)` and injects a sound
  token that the game never issued;
* every **bus release** (the CPU raising `u6_pa_out(4)`) drops S1..S8 to 0 and injects a
  *command 0* — which the ESP then plays as a real sample.

PinMAME models the same hardware and does neither. `gts80.c: riot6532_2a_w` re-evaluates the
sound command **only when port A is written**, never on a lamp write, and gates it on the strobe:

```c
data = ~data;
if (soundBoard == SNDBRD_GTS80B) { if (data & 0x10) GTS80_sndCmd_w(0, (lampMatrix[0]&0x10) | (data&0x0f)); }
else                             { GTS80_sndCmd_w(0, ((lampMatrix[1]&0x02)?0x10:0x00) | ((data&0x10) ? (data&0x0f) : 0)); }
```

`data & 0x10` is exactly `not u6_pa_out(4)` — **the strobe the FPGA already has and does not
use**. (Note the FPGA's `Sound_S16` tap, DS3 → bit 1, is precisely PinMAME's non-80B
`lampMatrix[1]&0x02`, i.e. the System-80 wiring; 80B uses a *different* lamp bit,
`lampMatrix[0]&0x10`. Worth a second look when 80B titles are the target.)

**(c) Command 0 is not a sound.** On 80B, PinMAME drops it outright — `gts80b_data_w` inverts and
`s80bs_sh_w` latches only `if (data != 0xff)`, i.e. never for command 0. On System 80/80A the
zero *is* meaningful, but as "no code is on the bus" (a level), not as "play sample 0".

**Consequence for reverse-engineering.** Because the release and the lamp writes both inject
tokens, a trace taken today is a mix of real cues and artefacts. That is very likely what the
"`cmd 16` = constant background heartbeat, ignore in traces" note on Arena actually was: a
release event with the S16 lamp bit latched high reads as command 16, not 0. **Unverified** —
`/sndtrace` on the real machine is what settles it.

---

## 2. The contract

The FPGA reports **events on the sound bus**, in order, without coalescing. It does not interpret
them; interpretation is the ESP's `sound.map` (see `SOUND_MAP.md`).

### 2.1 Byte-space claim

This document claims **0x30..0x3F** ("sound meta") out of the free 0x00..0x3F block. Nothing else
in that block is claimed, and `0xF4..0xFF` is left for the family/display tokens the FPGA-side
byte map already earmarks.

| byte | name | meaning |
|------|------|---------|
| `0x80 \| cmd[4:0]` | **SND** | one latched sound command. EVENT. Existing code, new semantics: one byte per *strobed latch*, never per bus change. |
| `0x30` | **REL** | the sound bus was released — no code is selected any more. EVENT. |
| `0x31` | **LOST** | the FPGA sound FIFO overflowed: at least one event was dropped since the last byte. EVENT. |
| `0x32..0x3F` | — | reserved for this contract. Do not allocate. |

`0x00..0x2F` must stay a **no-op forever**: a line break, or the Debug pin floating while the
FPGA is being reconfigured, decodes as `0x00`. It can never be given a meaning.

Decode is unambiguous — `0x3x & 0xC0 = 0x00` matches none of the existing masks (`0xC0`→`0x40`,
`0xE0`→`0x80`, `0xF0`→`0xA0/0xB0/0xE0`, `0xFE`→`0xF0/0xF2`) and is outside `0xBF`/`0xC0..0xDF`.

### 2.2 What the FPGA must do

1. **Qualify with the strobe.** Sample the 5-bit command **only on the rising edge of the U6
   port-A write** (`not u6_pa_out(4)` asserted), never on a change of the combinational vector. A
   lamp-latch write must produce **no** sound token. Formally: latch `{S16, S8, S4, S2, S1}` when
   the PA write strobe fires and PA4 is low → one **SND** event.
2. **Report the release separately.** When PA is written with PA4 high (no code selected), emit
   **REL** — not `0x80|0`. This is the event System 80/80A needs to stop a level-driven tone, and
   it is what lets the ESP tell "the same cue twice" from "one long cue".
3. **Do not coalesce: FIFO the events.** A small synchronous FIFO between the latch and the
   arbiter, ≥ **4** entries, **8** recommended (6 bits wide: 5 command bits + a release flag). At
   86.8 µs per byte, 8 entries absorb ~694 µs of burst — ~150 CPU instructions at 895 kHz, far
   more than any header/payload pair needs. *The design is deliberately tiny because the LABs are
   ~97 % full; 4 deep is an acceptable fallback, 1 (today) is not.*
4. **Never drop silently.** If the FIFO is full when an event arrives, drop the *new* event and
   set a sticky flag that emits **LOST** (`0x31`) once, ahead of the next SND. A trace that cannot
   distinguish "the game sent nothing" from "the link lost it" is not evidence.
5. **Keep sound an EVENT class.** Sound must stay out of the `lvl_hold` rate limit (it already
   is). Consider granting the sound FIFO priority *above* the level group when it is non-empty:
   level tokens are re-announced by the 50 ms heartbeat and lose nothing by waiting, a cue does.
6. **Reset behaviour.** Clear the FIFO on `rst`, and do **not** emit an initial SND for the
   power-up state of the bus. A level token announced at reset is helpful; a phantom cue is not.

Nothing about banks, stop commands or sample numbers belongs in the FPGA. Those are per-title
data on the ESP's SD card.

### 2.3 What the ESP guarantees

Already implemented, so the FPGA change is drop-in:

* every received byte is timestamped into a 512-entry ring and readable as CSV/JSON at
  **`GET /sndtrace`** (`src/fpgalink.cpp`);
* `0x80|cmd` → `wavplayer::playLive(cmd)` → `sndmap::feed()`;
* `0x30` → `wavplayer::soundRelease()` → `sndmap::release()`, which **stops** on System 80/80A
  (`release=stop`) and is ignored on 80B — and which deliberately does **not** disarm an armed
  bank header, because on the real bus the release sits *between* the header and its payload
  (proven in `tools/test_sndmap.cpp` case 4);
* `0x31` → a counter surfaced in `/sndtrace` (`lost=`), never audible;
* command 0 is ignored by default (`SOUND_MAP.md`, `ignore=`);
* a 16-deep command FIFO already exists on the ESP side (`fpgalink::popSound`), so the ESP is not
  the narrow point.

---

## 3. How to verify it on the machine

The method that solved every other mystery on this project: capture first, conclude after.

```
curl "http://gottfa.local/sndtrace?clear=1"      # arm, right before the action
# … hit ONE playfield target, once, then stop touching the machine …
curl "http://gottfa.local/sndtrace" > spinner.csv
```

Output is `idx,ms,dms,hex,class,value`; `class` is `snd`, `rel`, `lost`, `game`, `gip`, `mode`,
`state`, `dinj`, `rxc`, `snap`, or `?` for a byte nobody has claimed. `dms` is the gap from the
previous row — the column that shows a header/payload pair (a few ms apart) or an accelerating
beep train.

By default the ring skips the 769 bytes/s of RAM-snapshot payload and repeated level tokens (they
carry the current value by construction, and the 50 ms heartbeat would otherwise flush the ring in
4 s). `?raw=1` records literally everything — about 0.6 s of history, so arm it, do the one thing,
read it back immediately. `?raw=0` returns to filtered.

**The acceptance test for the FPGA change:** with the machine in attract mode, lamps animating and
no sound playing, `/sndtrace` must show **zero** `snd` rows. Today it shows a stream of them.

---

## 4. Latency — where it actually is

Measured against the code and the datasheet path, not guessed:

| stage | cost | note |
|-------|------|------|
| link + `fpgalink::poll()` | ≤ 2 ms | `loop()` runs `delay(2)` |
| request queue → mix task | ≤ 5.8 ms | one 256-frame pass @ 44.1 kHz; the mix task now drains **2** requests per pass instead of 1, so a burst no longer starts one cue every 5.8 ms |
| `SD.open()` + first read | tens of ms | **the real cost**: the card runs at **1 MHz SPI** (`SD.begin(..., 1000000)`) |
| I2S DMA queue | 46 ms | `8 × 256` frames @ 44.1 kHz |

The DMA queue looks like free latency to reclaim. It is not: it is also the *only* elasticity
covering SD jitter. `wavsrc::topup()` reads 1024 bytes at a time = **8.2 ms of bus time at 1 MHz**,
and one 44.1 kHz mono voice needs 88 kB/s against a bus that tops out near 125 kB/s. Two voices
topping up in the same pass already spend ~17 ms; halving the queue to 23 ms would leave almost
nothing. **So the buffer size is now a measured knob, not a constant:**

* `/config.txt` on the SD accepts `i2sn=` (DMA buffer count) and `i2slen=` (frames per buffer).
  Default **unchanged** at `8 × 256` = 46 ms. Clamped to a 512-frame floor.
* `GET /snd` reports the meter: `busyMax` (µs a mix pass spent doing real work outside the paced
  `i2s_write`), `period` (5804 µs of audio per pass) and `late` (passes where busy ≥ period).
  `/snd?mixreset=1` zeroes it.

**The experiment**, on hardware, in this order: play the busiest scene you have, check `late` is
0 and `busyMax` is well under 5804 µs; halve the buffer (`i2sn=4`); repeat. Keep the shortest
geometry that still gives `late = 0` over several minutes of real play. If `busyMax` is already
close to `period` at 8×256, the fix is the **SD clock** (or a decoupling cap on the card), not the
queue — raising `SD.begin` above 1 MHz is worth far more latency than the DMA buffer is.
