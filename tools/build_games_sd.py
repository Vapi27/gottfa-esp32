#!/usr/bin/env python3
# build_games_sd.py — extrait les ROMs son de TOUS les jeux Gottlieb System 80B (Gen1/2/3) depuis les
# zips PinMAME, dans une arborescence carte SD multi-jeux pour le firmware GOSOWAV PSOROM.
# Génération = lue dans la source PinMAME (gts80games.c, CORE_GAMEDEFNV/CLONEDEFNV -> gl_mGTS80BSx).
# Sortie: OUT/games.idx (lignes "short|gen|title") + OUT/games/<short>/{yrom1,yrom2?,drom1}.snd
import re, os, sys, zipfile, zlib

SRC_C = sys.argv[1] if len(sys.argv) > 1 else "/root/pinmame/src/wpc/gts80games.c"
ROMS  = sys.argv[2] if len(sys.argv) > 2 else "/root/pinmame_data/roms"
OUT   = sys.argv[3] if len(sys.argv) > 3 else "/tmp/gxsd"
GAMES = os.path.join(OUT, "games")
GENMAP = {"S1": 1, "S2": 2, "S3": 3, "S3a": 3}   # gl_mGTS80BS1/2/3/3a ; gl_mGTS80B (base) ignoré

text = open(SRC_C, encoding="latin-1").read()
g2 = {}   # short -> (gen, title)
pat_game  = re.compile(r'CORE_GAMEDEFNV\(\s*(\w+)\s*,\s*"([^"]*)"\s*,\s*\d+\s*,\s*"[^"]*"\s*,\s*gl_mGTS80B(S1|S2|S3a|S3)\b')
pat_clone = re.compile(r'CORE_CLONEDEFNV\(\s*(\w+)\s*,\s*\w+\s*,\s*"([^"]*)"\s*,\s*\d+\s*,\s*"[^"]*"\s*,\s*gl_mGTS80B(S1|S2|S3a|S3)\b')
for pat in (pat_game, pat_clone):
    for m in pat.finditer(text):
        g2.setdefault(m.group(1), (GENMAP[m.group(3)], m.group(2)))

os.makedirs(GAMES, exist_ok=True)
rows = []
for z in sorted(os.listdir(ROMS)):
    if not z.endswith(".zip"):
        continue
    short = z[:-4]
    if short not in g2:
        continue
    gen, title = g2[short]
    try:
        zf = zipfile.ZipFile(os.path.join(ROMS, z)); names = zf.namelist()
    except Exception:
        continue
    if "yrom1.snd" not in names or "drom1.snd" not in names:
        continue
    d = os.path.join(GAMES, short); os.makedirs(d, exist_ok=True)
    crcs = []
    for f in ("yrom1.snd", "yrom2.snd", "drom1.snd"):
        if f in names:
            data = zf.read(f); open(os.path.join(d, f), "wb").write(data)
            crcs.append("%s=%08x" % (f, zlib.crc32(data) & 0xffffffff))
    title = title.replace("|", "-")
    rows.append((gen, short, title, " ".join(crcs)))

rows.sort(key=lambda r: (r[0], r[1]))
with open(os.path.join(OUT, "games.idx"), "w", encoding="utf-8") as fp:
    for gen, short, title, _ in rows:
        fp.write("%s|%d|%s\n" % (short, gen, title))

print("gen  short        title")
for gen, short, title, crcs in rows:
    print("G%d   %-12s %-26s %s" % (gen, short, title, crcs))
print("\n%d jeux -> %s/games.idx" % (len(rows), OUT))
