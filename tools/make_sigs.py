#!/usr/bin/env python3
# make_sigs.py — genere sounds.sig (signatures evenementielles CHEF v2) pour TOUS les jeux 80B.
# Source de verite : host_sig2 (conducteur ROM exact). Zero reglage manuel : games.idx (gen),
# loops.txt @hdr (banques, detectees par make_psowav_set v8). Resumable (skip si deja fait).
# Usage : python3 tools/make_sigs.py [gid ...]   (defaut : tous)
import os, subprocess, sys

SD = os.path.expanduser('~/gosowav_sd')
OUT = os.path.expanduser('~/gosowav_build')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = '/tmp/psowav_bin'

def sh(c): return subprocess.run(c, shell=True, capture_output=True, text=True)

def build():
    os.makedirs(BIN, exist_ok=True)
    for f, cc in [('f6502.o', f'gcc -c -O2 -Isrc src/fake6502.c -o {BIN}/f6502.o'),
                  ('emu2149.o', f'gcc -c -O2 -Isrc src/emu2149.c -o {BIN}/emu2149.o')]:
        if not os.path.exists(f'{BIN}/{f}'):
            assert sh(f'cd {ROOT} && {cc}').returncode == 0, f
    src_m = max(os.path.getmtime(f'{ROOT}/tools/host_sig2.cpp'),
                os.path.getmtime(f'{ROOT}/src/psorom.cpp'))
    if not os.path.exists(f'{BIN}/sig2') or os.path.getmtime(f'{BIN}/sig2') < src_m:
        r = sh(f'cd {ROOT} && g++ -std=c++17 -Isrc tools/host_sig2.cpp src/psorom.cpp src/sp0250.cpp'
               f' src/ym2151w.cpp src/ymfm_opm.cpp {BIN}/f6502.o {BIN}/emu2149.o -o {BIN}/sig2')
        assert r.returncode == 0, r.stderr[:400]

def yrom_path(gid, gen):
    d = f'{SD}/games/{gid}'
    y1, y2 = f'{d}/yrom1.snd', f'{d}/yrom2.snd'
    if gen == 1 and os.path.exists(y2):                  # Gen1 : yrom2 ++ yrom1 (vecteurs en haut)
        cat = f'{BIN}/y_{gid}.snd'
        with open(cat, 'wb') as o: o.write(open(y2, 'rb').read() + open(y1, 'rb').read())
        return cat
    return y1

def headers(gid):                                        # banques detectees par make_psowav_set (@hdr=29,30)
    lp = f'{OUT}/{gid}/loops.txt'
    if os.path.exists(lp):
        for ln in open(lp):
            if ln.startswith('@hdr='):
                return [int(x) for x in ln[5:].strip().split(',') if x.strip().isdigit()]
    return []

def main():
    build()
    want = sys.argv[1:]
    games = []
    for ln in open(f'{SD}/games.idx'):
        f = ln.strip().split('|')
        if len(f) >= 3 and (not want or f[0] in want): games.append((f[0], int(f[1])))
    for gid, gen in games:
        sig = f'{OUT}/{gid}/sounds.sig'
        if os.path.exists(sig) and os.path.getsize(sig) > 0:
            print(f'{gid}: deja fait ({sig})'); continue
        os.makedirs(f'{OUT}/{gid}', exist_ok=True)
        gflag = {1: '1', 2: '2', 3: 'b'}[gen]
        y, d = yrom_path(gid, gen), f'{SD}/games/{gid}/drom1.snd'
        cmds = [str(c) for c in range(1, 32)]
        for h in headers(gid):                           # paires header.valeur -> sons etendus 32-95
            cmds += [f'{h}.{v}' for v in range(1, 32)]
        print(f'== {gid} (Gen{gen}) : {len(cmds)} commandes ==', flush=True)
        lines = []
        for c in cmds:
            r = sh(f'{BIN}/sig2 {gflag} {y} {d} {c} 80')
            ln = r.stdout.strip()
            if ln.startswith('{'): lines.append(ln); print(f'  {ln}', flush=True)
            else: print(f'  {c}: ECHEC {r.stderr[:120]}', flush=True)
        with open(sig, 'w') as o: o.write('\n'.join(lines) + '\n')
        print(f'{gid}: {len(lines)} signatures -> {sig}', flush=True)

if __name__ == '__main__':
    main()
