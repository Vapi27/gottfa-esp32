# SOUND_MAP — adding a title without touching the firmware

**The promise.** To give a game sound on this board you copy **one folder of WAV files** and
**one text file** onto the ESP's microSD. No recompile, no reflash, no firmware release. The
folder is the samples; the text file, `sound.map`, says what the machine's sound commands *mean*.

Read `SOUND_WIRE.md` for how the commands get from the game CPU to the ESP, and `PSOWAV.md` for
how the sample sets are produced.

---

## 1. What goes on the card

```
/config.txt                 global settings (volumes, mix, start theme, I2S buffer)
/games.txt                  "<FPGA game No> <folder>" — which folder each machine loads
/arena/                     one folder per title, named like the romset
   0014-v-100-wall.wav
   0021-100-spinner.wav
   0028-100-beep.wav
   0030-l-100-drone.wav
   sound.map                ← this file
```

**WAV files** must be **PCM 16-bit, mono or stereo, 44.1 kHz** (the player does not resample: a
22 kHz file plays an octave low). The name carries the id and the attributes:

```
NNNN-AAAA-VVV-description.wav      0021-100-spinner.wav      0030-l-100-drone.wav
│    │    └── volume 0..100        (short forms "NNNN-VVV.wav" and "NNNN.wav" also work)
│    └─────── attributes: l loop · b break(stop same id) · i init/background · v voice bus
└──────────── sample id, 0..95
```

`NNNN-m-A-B-C-description.grp` defines a random (`m`) or sequential (`r`) group: playing that id
picks one of the listed member ids.

**`/games.txt`** maps the game number the FPGA sends (the DIP-selected gamelist number) to the
folder: `51 arena`. Nothing else is needed — plug the card in, select the game on the board, and
the ESP loads that folder.

---

## 2. `sound.map`

Plain text, one `key=value` per line, `#` starts a comment, blank lines ignored. Unknown keys are
ignored on purpose, so a card written for a newer build still works on an older one. **A missing
`sound.map` is fine** — you get the safe defaults below. Order does not matter.

Numeric lists accept single values, comma lists and ranges: `voice=1-3,9,17`.

| key | default | meaning |
|-----|---------|---------|
| `name=` | — | free text, for humans |
| `gen=` | unknown | `80`, `80a`, `80b1`, `80b2`, `80b3`. Sets **only** the release behaviour (below) and documents the title. It deliberately implies **no** bank rules. |
| `ignore=` | `0` | command values that are never a sound. Command 0 is the bus-release artefact — leave it unless you have a trace proving otherwise. `ignore=` (empty) re-enables it. |
| `stop=` | none | command values that stop every playing sound. |
| `header=C:B` | none | command `C` is a **bank prefix**: the *next* command plays sample `next + B`. Repeatable (`header=30:32`, `header=29:64`). The header itself plays nothing. |
| `hdrms=` | `250` | how long an armed header stays armed. After this, it is forgotten instead of poisoning a much later command. |
| `map=F:T` | none | remap decoded sample id `F` to `T` (also written `F->T`). Repeatable, up to 32. Applied *after* the bank is added. |
| `voice=` | none | sample ids that are speech — mixed on the voice bus (`volv`) and preserved by soft-kill. |
| `loop=` | none | sample ids that loop (drones, background). |
| `release=` | from `gen=` | `stop` or `ignore`: what "the sound bus went idle" does. |
| `repeatms=` | `0` | drop an identical id repeated within this many ms. `0` = never drop (a drum roll needs 0). |
| `legacy80b=1` | off | shorthand for `header=30:32`, `header=29:64`, `stop=31` — see the warning in §5. |

### Decode order

For each command the ESP does, in this order: expire a stale header → drop it if `ignore` →
stop-all if `stop` → arm the bank if `header` → add any armed bank → apply `map` → drop it if it
repeats within `repeatms` → **play**.

Two rules that exist because the real bus behaves that way, both covered by the host test suite:

* an **ignored** command does not consume an armed header (a phantom 0 between a header and its
  payload cannot steal the bank);
* a **release** does not disarm a header either — on the bus a two-command sequence is
  *latch header → release → latch payload*, so the release sits between the pair.

### `voice=` / `loop=` vs the filename

They are an **overlay**: the filename attributes still apply, and the map can only *add*. Use the
filename when you author the set, use the map when you need to fix a set you did not make.

---

## 3. Worked example — Arena (System 80B, sound Gen1)

Arena's commands were ground-truthed on the real machine by capturing one action at a time —
spinner = command 21, wall = command 14 with an alternating command 30 drone and a command 28
beep. Crucially Arena is **Gen1: the 5-bit latch goes straight to the sound CPU, there is no bank
protocol**, so declaring a bank header on 30 would swallow the drone and the wall would go quiet.
That is exactly the mistake the old hardcoded firmware rule made.

`/arena/sound.map`:

```
# Arena — Gottlieb System 80B, sound board Gen1 (gl_mGTS80BS1).
# Commands ground-truthed on the machine with /sndtrace, one action at a time.
name=Arena
gen=80b1

# Gen1 latches the 5-bit value directly: NO bank headers, NO native stop command.
# (Do not add header=30:32 here — command 30 is the wall drone, not a prefix.)

# 0 is the bus-release artefact, and 16 has been seen as a constant background token:
# re-check 16 against a fresh trace before trusting this line.
ignore=0

loop=30           # wall drone — a real standalone loop
voice=14          # wall rumble / speech bus
```

and the samples beside it:

```
0014-v-100-wall.wav        wall rumble        (voice bus)
0021-100-spinner.wav       spinner            ~680 ms, one-shot, keep ≥400 ms
0028-100-beep.wav          the "bibibip"      ~330 ms so the train stays crisp
0030-l-100-drone.wav       wall drone         loops
```

Add `51 arena` to `/games.txt`, select game 51 on the board, done.

---

## 4. Finding the commands for a title you do not know

There is no shortcut and no need for one — the machine will tell you:

1. `curl "http://gottfa.local/sndtrace?clear=1"` — arm the trace.
2. Hit **one** target, once. Wait ~5 s. Do nothing else.
3. `curl "http://gottfa.local/sndtrace"` — read the `snd` rows and their `dms` gaps.
4. Repeat per target, one at a time. Two `snd` rows a few ms apart is the signature of a
   header/payload pair; a lone row is a direct command.
5. Name the WAV `00NN-...wav` after the command number you saw, and write the map.

Watch `lost=` in the trace header: anything but 0 means the link dropped events and the trace is
incomplete (see `SOUND_WIRE.md` §2.2).

---

## 5. Gotchas

* **`legacy80b=1` is a guess, not a fact.** `header=30:32` / `header=29:64` / `stop=31` was
  reverse-engineered from later 80B titles and has never been confirmed on hardware. The firmware
  used to apply it automatically to *any* set containing banked samples (ids ≥ 32); it still does
  when a set has banked samples **and no `sound.map`**, purely so existing cards behave exactly as
  before. Once you write a `sound.map`, you own the rules — and a trace beats the guess.
* **A missing folder is silence, not chaos.** If the selected game has no folder (or no usable
  WAVs), the ESP stops, reports `status=nofolder` / `empty` at `GET /snd`, and plays nothing. It
  will not fall back to the previous game's samples. Re-scanning the *same* theme after an SD
  glitch keeps the sounds that are already loaded.
* **Ownership gate.** If it is enabled, a folder stays locked (`status=locked`) until a verified
  dump of that game's CPU ROM has been made on this board.
* **Limits.** 96 sample ids (0–31 direct, 32–63 bank 1, 64–95 bank 2), 16 groups, 8 simultaneous
  voices, 32 remap entries, `sound.map` read up to 1 KB.
* **Volume/mix are global**, in `/config.txt`: `vols=`, `volv=`, `mix=sum|div2|sqrt`, `stheme=`,
  plus `i2sn=`/`i2slen=` for the output buffer (see `SOUND_WIRE.md` §4).
