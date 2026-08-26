#!/usr/bin/env bash
# ============================================================================
#  One command to port the wall to another pinball table.
#
#      tools/mkgame.sh <table.vpx> <pinmame_name> [emulated_seconds]
#      # then:  pio run -e arenaled_d1mini32 -t buildfs   + OTA ?target=fs
#
#  Produces the two files the firmware reads — data/arena_pf.json (insert
#  positions + lamp numbers from the VP table) and data/arena_attract.bin
#  (the game's own attract mode, captured from its ROM). The firmware itself
#  is game-agnostic and never changes.
#
#  Per-game knowledge is optional and lives in tools/games/<name>.json
#  (author fixes + the manual's lamp chart). Without it you still get a
#  working plan — labels from the VP table, no function tooltips.
#
#  ATTENTION sur une table qui n'est pas Gottlieb. La traduction nom d'objet VP
#  -> type d'insert etait ecrite en dur dans vpx_inserts.py avec la convention
#  Gottlieb (L<n> insert, F flasher, GI/LS ignores) pendant que le reste de
#  l'outil se disait game-agnostic. Elle est passee dans le fichier de jeu
#  ("kinds" / "ignore"). Un motif qui ne correspond pas ne rend PAS d'erreur :
#  il rend un plateau a zero insert, ou un plateau plausible ou les flashers
#  sont comptes comme des lampes. Ouvrir la table dans Visual Pinball et
#  regarder comment les lumieres s'appellent vraiment AVANT de lancer ceci.
#  tools/games/alpok_l6.json (Alien Poker, Williams System 7) sert de modele.
#
#  Prerequisites for the capture step:
#    - libpinmame built once:  see ARENA_LED.md "Porting to another table"
#    - the game's ROM zip in ~/.pinmame/roms/
#  Skip it (no ROM at hand): the wall falls back to the generic attract.
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

VPX="${1:?usage: tools/mkgame.sh <table.vpx> <pinmame_name> [seconds]}"
GAME="${2:?usage: tools/mkgame.sh <table.vpx> <pinmame_name> [seconds]}"
SECS="${3:-300}"
CFG="tools/games/$GAME.json"
PM="${PINMAME_DIR:-../gottfa-upstream/lisy_5_28}"

say() { printf '\n\033[1;33m== %s\033[0m\n' "$*"; }

say "1/3 inserts <- $VPX"
if [ -f "$CFG" ]; then
  python3 tools/vpx_inserts.py "$VPX" --config "$CFG" > data/arena_pf.json
else
  echo "   (no $CFG - raw VP labels, no functions; create one after checking the real playfield)"
  python3 tools/vpx_inserts.py "$VPX" > data/arena_pf.json
fi

say "2/3 attract <- ROM '$GAME' ($SECS emulated seconds)"
if [ ! -x tools/capture_attract ]; then
  echo "   building capture_attract (needs libpinmame in $PM)"
  clang++ -std=c++17 -O2 -I "$PM/src/libpinmame" tools/capture_attract.cpp \
          -o tools/capture_attract -L "$PM/build" -lpinmame -Wl,-rpath,"$(cd "$PM/build" && pwd)"
fi
tools/capture_attract "$GAME" "$SECS" > "/tmp/${GAME}_raw.json"

say "3/3 pack"
python3 tools/pack_attract.py "/tmp/${GAME}_raw.json" -o data/arena_attract.bin

say "done - now:  pio run -e arenaled_d1mini32 -t buildfs   then OTA ?target=fs"
