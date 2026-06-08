#!/usr/bin/env python3
# build_romdb.py — extract a known-good ROM checksum DB from PinMAME's Gottlieb driver
# (gts80games.c). Every game/version/sound ROM is defined there as  "<file>", CRC(xxxxxxxx).
# Output: roms.csv  ->  crc,size,name,game,title   (loaded by the ESP romdb module to verify a
# user's dump: CRC match = correct + intact; size-only match = corrupted or unlisted revision).
#
# Usage:  build_romdb.py <gts80games.c> <out.csv>
import re, sys

# "<file>" ... CRC(xxxxxxxx) on one line — handles BOTH the compact macro form
#   GTS80_1_ROMSTART("652.cpu", CRC(....))   and the full  ROM_LOAD("u2_80.bin",0x2000,0x1000,CRC(....))
# The non-greedy [^"]*? stops at the first CRC after the name, so it never spans to the next filename.
ROM_RE  = re.compile(r'"([^"]+)"[^"]*?CRC\(([0-9a-fA-F]{8})\)')
# CORE_GAMEDEFNV(id,"Title",year,...)  or  CORE_CLONEDEFNV(id,parent,"Title",year,...)
DEF_RE  = re.compile(r'CORE_(?:GAME|CLONE)DEF(?:NV)?\s*\(\s*([A-Za-z0-9_]+)\b')
TITLE_RE = re.compile(r'"([^"]+)"')

def main():
    if len(sys.argv) < 3:
        print("usage: build_romdb.py <gts80games.c> <out.csv>"); sys.exit(1)
    src = open(sys.argv[1], encoding="latin-1").read().split("\n")
    out = []                       # (crc, name, game, title)
    seen = set()                   # dedup by crc
    pending = []                   # (name, crc) accumulated until the next CORE_*DEF
    n_games = 0
    for line in src:
        for m in ROM_RE.finditer(line):
            pending.append((m.group(1), m.group(2).lower()))
        d = DEF_RE.search(line)
        if d:
            game = d.group(1)
            # title = first quoted string AFTER the id on this line
            rest = line[d.end():]
            tm = TITLE_RE.search(rest)
            title = tm.group(1) if tm else game
            n_games += 1
            for name, crc in pending:
                if crc in seen:
                    continue
                seen.add(crc)
                out.append((crc, name, game, title))
            pending = []
    # write csv
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write("crc,name,game,title\n")
        for crc, name, game, title in out:
            title = title.replace(",", " ")
            f.write("%s,%s,%s,%s\n" % (crc, name, game, title))
    print("games parsed: %d  |  unique ROM CRCs: %d  ->  %s" % (n_games, len(out), sys.argv[2]))
    # sanity: known Black Hole rev.4 game rom
    for crc, name, game, title in out:
        if crc == "01b53045":
            print("  sanity: 01b53045 = %s (%s / %s)" % (name, game, title))

if __name__ == "__main__":
    main()
