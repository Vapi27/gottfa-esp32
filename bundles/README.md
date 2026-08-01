# Game bundles — your machine's attract mode on the wall

One folder per game. A bundle is two files, uploaded from the board's web page
(**Game** panel — no tools, no compiler needed):

| File | What it is | Made from |
|---|---|---|
| `pf.json` | the playfield: inserts, positions, lamp numbers, functions | the game's Visual Pinball table |
| `attract.bin` | the game's own attract mode, lamp by lamp | the game's ROM, run under PinMAME |

## Using one
Open the board's page → **Game** → upload `pf.json`, then `attract.bin`.
The board validates each file before accepting it, reboots, and keeps your
pixel mapping. New table = new inserts: re-run the mapping wizard after.

## Making one (any computer, ~15 min)
You need the game's `.vpx` table and its ROM zip (you own the machine — the ROM
is the one thing nobody can ship for you):

```sh
cp <game>.zip ~/.pinmame/roms/
tools/mkgame.sh "<table>.vpx" <pinmame_name>
mkdir bundles/<name> && cp data/arena_pf.json bundles/<name>/pf.json \
                     && cp data/arena_attract.bin bundles/<name>/attract.bin
```

`tools/games/<name>.json` (optional) carries the game's manual lamp chart and
fixes for VP naming errors — see `arena.json` for both kinds of entry, and
ARENA_LED.md "Porting to another table" for the lessons learned doing Arena.

Contributions welcome: a bundle is a light-show recording (lamp masks), not the
ROM — the ROM itself is never redistributed here.

| Bundle | Game | Attract | Lamp chart |
|---|---|---|---|
| `arena/` | Arena (Gottlieb #709, 1987) | 120 s from ROM | Premier manual E-25440 |
