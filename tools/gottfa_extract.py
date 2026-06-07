#!/usr/bin/env python3
# gottfa_extract.py — build the ESP romstore from a GottFA80_PLuS SD image.
#
# The GottFA FPGA reads each game's 16 KB ROM image from raw SD sectors:
#   sector = gamenumber*32 + 660   (512 B/sector)  -> byte offset = 660*512 + N*16384
# (verified in GottFA80/lib_common/SD_Card.vhd). This tool slices those 16 KB images out of a
# GottFA SD .img and writes them as /roms/<NN>.img — the layout romstore.cpp reads on the ESP SD.
#
# Usage:  gottfa_extract.py <gottfa_sd.img> <out_dir> [games.txt]
#         (out_dir gets NN.img files + a manifest.txt; copy out_dir to the ESP SD as /roms/)
import sys, os, hashlib

SECTOR = 512
GAME_SECTORS = 32                  # 32 * 512 = 16384
IMG_SIZE = GAME_SECTORS * SECTOR   # 16384
BASE_SECTOR = 660
BASE = BASE_SECTOR * SECTOR        # 337920
MAX_GAME = 63

def load_names(path):
    names = {}
    if path and os.path.exists(path):
        for line in open(path, encoding="latin-1"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            p = line.split()
            if len(p) >= 2 and p[0].isdigit():
                names[int(p[0])] = p[1]
    return names

def is_empty(b):
    return all(x == 0x00 for x in b) or all(x == 0xFF for x in b)

def main():
    if len(sys.argv) < 3:
        print("usage: gottfa_extract.py <gottfa_sd.img> <out_dir> [games.txt]"); sys.exit(1)
    sd, out = sys.argv[1], sys.argv[2]
    names = load_names(sys.argv[3] if len(sys.argv) > 3 else None)
    data = open(sd, "rb").read()
    os.makedirs(out, exist_ok=True)
    man = open(os.path.join(out, "manifest.txt"), "w")
    man.write("# game | rom | bytes | md5(first 12) | status\n")
    written = 0
    for n in range(MAX_GAME):
        off = BASE + n * IMG_SIZE
        if off + IMG_SIZE > len(data):
            break
        img = data[off:off + IMG_SIZE]
        if is_empty(img):
            continue
        fn = os.path.join(out, "%02d.img" % n)
        open(fn, "wb").write(img)
        h = hashlib.md5(img).hexdigest()[:12]
        line = "%2d | %-10s | %d | %s | OK" % (n, names.get(n, "?"), len(img), h)
        print("  " + line); man.write(line + "\n")
        written += 1
    man.close()
    print("=== extracted %d game images to %s (16 KB each) ===" % (written, out))

if __name__ == "__main__":
    main()
