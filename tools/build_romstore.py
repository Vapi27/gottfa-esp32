#!/usr/bin/env python3
# build_romstore.py — populate the ESP romstore (/roms/<NN>.img) from a GottFA per-game .img
# library (Roms/.../all_roms/<romname>[_FP].img), mapped via the GottFA80_PLuS gamelist (No -> rom).
# Prefers the Free-Play (_FP) variant where present. Usage: build_romstore.py <all_roms_dir> <out_dir>
import sys, os, shutil, hashlib

# GottFA80_PLuS gamelist (No -> PinMAME romname) — mirrors games.txt / manual Appendix A.
GAMES = {0:"panthera",1:"spidermn",2:"circus",3:"cntforce",4:"starrace",5:"jamesb",6:"jamesb2",
 7:"timeline",8:"forceii",9:"pnkpnthr",10:"mars",11:"mars",12:"vlcno_ax",13:"vlcno_1b",
 14:"blckhole",15:"blckhole",16:"hh",17:"eclipse",18:"dvlsdre",19:"dvlsdre2",20:"rocky",
 21:"spirit",22:"punk",23:"striker",24:"krull",25:"qbquest",26:"sorbit",27:"rflshdlx",
 28:"goinnuts",29:"amazonh",30:"rackemup",31:"raimfire",32:"jack2opn",33:"touchdn",34:"alienstr",
 35:"thegames",36:"eldorado",37:"icefever",38:"caveman",39:"caveman",40:"bountyh",41:"triplay",
 42:"tagteam",43:"rock",44:"raven",45:"rock_enc",46:"hlywoodh",47:"genesis",48:"goldwing",
 49:"mntecrlo",50:"sprbreak",51:"arena",52:"victory",53:"diamond",54:"txsector",55:"robowars",
 56:"excalibr",57:"badgirls",58:"bighouse",59:"hotshots",60:"bonebstr",61:"nmoves",62:"amazonh2"}
# romname -> .img base name when they differ
ALIAS = {"amazonh2":"amazonhII"}
# per-No override (distinct .img for a clone slot)
NOVERRIDE = {39:"cave_fli"}

def main():
    if len(sys.argv) < 3:
        print("usage: build_romstore.py <all_roms_dir> <out_dir>"); sys.exit(1)
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    have = set(os.listdir(src))
    print("%-3s %-10s  %-9s %-9s" % ("No", "rom", "stock", "freeplay"))
    n_stock = n_fp = 0; missing = []
    for N in range(63):
        rom = GAMES.get(N)
        if not rom: continue
        base = NOVERRIDE.get(N, ALIAS.get(rom, rom))
        # emit BOTH variants where present: stock -> NN.img, free-play -> NNfp.img
        # (mirrors GottFA's normal + FP images; romstore.cpp serves the active one per the FP setting)
        variants = [(base + ".img", "%02d.img" % N, False), (base + "_FP.img", "%02dfp.img" % N, True)]
        got = []
        for srcname, outname, isfp in variants:
            if srcname not in have: got.append("-"); continue
            srcf = os.path.join(src, srcname)
            if os.path.getsize(srcf) != 16384:
                got.append("BADSZ"); continue
            shutil.copy(srcf, os.path.join(out, outname))
            got.append(hashlib.md5(open(srcf, "rb").read()).hexdigest()[:8])
            if isfp: n_fp += 1
            else: n_stock += 1
        if got == ["-", "-"]:
            missing.append((N, rom))
        print("%-3d %-10s  %-9s %-9s" % (N, rom, got[0], got[1]))
    print("=== romstore: %d stock + %d free-play images, %d games missing both -> %s ===" %
          (n_stock, n_fp, len(missing), out))
    if missing: print("missing:", ", ".join("%d:%s" % m for m in missing))

if __name__ == "__main__":
    main()
