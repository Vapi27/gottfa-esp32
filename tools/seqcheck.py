#!/usr/bin/env python3
# seqcheck.py — VALIDATION du chef v2 contre PinMAME : rejoue une sequence dans le simulateur
# (chefsim, MEME code que le firmware), reconstruit le mix WAV resultant (memes regles que
# wavTrigger : boucle intro+region, stop exact), et compare son ENVELOPPE a l'audio de
# reference PinMAME (seq.wav du rig). Verdict par fenetres de 100 ms.
# Usage: seqcheck.py <gid> <nom> "<t:cmd ...>" <totalSec> <ref.wav> [wavdir]
import os, sys, subprocess, wave, array, glob

SD   = os.path.expanduser('~/gosowav_sd')
OUT  = os.path.expanduser('~/gosowav_build')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def sh(c): return subprocess.run(c, shell=True, capture_output=True, text=True)

def game_info(gid):
    for ln in open(SD + '/games.idx'):
        f = ln.strip().split('|')
        if len(f) >= 3 and f[0] == gid: return int(f[1]), f[2]
    raise SystemExit(f"jeu '{gid}' inconnu")

def yrom(gid, gen):
    d = f'{SD}/games/{gid}'
    if gen == 1 and os.path.exists(f'{d}/yrom2.snd'):
        cat = f'/tmp/y_{gid}.snd'
        with open(cat, 'wb') as o: o.write(open(f'{d}/yrom2.snd','rb').read() + open(f'{d}/yrom1.snd','rb').read())
        return cat
    return f'{d}/yrom1.snd'

def load_pcm(path, rate):                                 # n'importe quel wav -> s16 mono @rate (ffmpeg)
    r = subprocess.run(f'ffmpeg -v error -i "{path}" -ac 1 -ar {rate} -f s16le -',
                       shell=True, capture_output=True)
    a = array.array('h'); a.frombytes(r.stdout[:len(r.stdout)//2*2]); return a

def main():
    gid, name, seq, total, ref_path = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), sys.argv[5]
    gen, _ = game_info(gid)
    wavdir = sys.argv[6] if len(sys.argv) > 6 else f'{OUT}/{gid}'
    sig = f'{OUT}/{gid}/sounds.sig'                                   # signatures : toujours le canonique
    if not os.path.exists(sig): sig = f'{OUT}/{gid}_sig.jsonl'        # repli (arena historique)
    # 1) journal d'actions du simulateur (le MEME chefv2.cpp que le firmware)
    r = sh(f'{ROOT}/tools/test_chefv2.sh >/dev/null 2>&1; true')      # garantit /tmp/chefsim a jour
    r = sh(f'/tmp/chefsim {gen} {yrom(gid,gen)} {SD}/games/{gid}/drom1.snd {sig} "{seq}" {total}')
    acts = []
    for ln in r.stdout.splitlines():
        p = ln.split()
        if len(p) >= 4 and p[1] == 'ms': acts.append((int(p[0]), p[2], int(p[3])))
    print('\n'.join(f'  {t:6d} ms {op:7s} {i}' for t, op, i in acts))
    # 2) reconstruction du mix simule
    rf = wave.open(ref_path, 'rb'); rate = rf.getframerate()
    ref = array.array('h'); ref.frombytes(rf.readframes(rf.getnframes()))
    if rf.getnchannels() > 1:                                          # downmix si besoin
        ch = rf.getnchannels(); ref = array.array('h', [sum(ref[i:i+ch])//ch for i in range(0, len(ref)//ch*ch, ch)])
    N = len(ref)
    loops = {}                                                         # id -> (debutMs, lenMs) ; oneshot via valeur 'oneshot'
    lp = f'{wavdir}/loops.txt'
    if not os.path.exists(lp): lp = f'{OUT}/{gid}/loops.txt'          # points de boucle : repli canonique
    if os.path.exists(lp):
        for ln in open(lp):
            if '=' in ln and not ln.startswith('@'):
                i, v = ln.strip().split('=', 1)
                if v.startswith('oneshot'): loops[int(i)] = 'oneshot'
                else:
                    st, _, le = v.partition(','); loops[int(i)] = (int(st or 0), int(le or 0))
    sus = {}                                                           # id -> sustained (verite ROM)
    for ln in open(sig):
        if '"cmd"' in ln:
            c = ln.split('"cmd":"')[1].split('"')[0]
            i = (((~int(c.split('.')[0])) & 0x1F) * 32 + (int(c.split('.')[1]) & 0x1F)) if '.' in c else int(c)
            sus[i] = '"sustained":true' in ln
    pcm = {}
    def wav_of(i):
        if i in pcm: return pcm[i]
        g = sorted(glob.glob(f'{wavdir}/{i:04d}-*.wav'))
        pcm[i] = load_pcm(g[0], rate) if g else None
        return pcm[i]
    mix = array.array('i', bytes(4 * N))
    starts = {}                                                        # id -> sample de depart (instance unique par id)
    stops  = {}
    spans  = []                                                        # (id, s0, s1)
    for t, op, i in acts:
        s = int(t * rate / 1000)
        if op in ('START', 'RESTART'):
            if i in starts: spans.append((i, starts.pop(i), s))        # RESTART coupe l'instance courante
            starts[i] = s
        else:
            if i in starts: spans.append((i, starts.pop(i), s))
    for i, s0 in starts.items(): spans.append((i, s0, N))              # toujours actifs a la fin
    for i, s0, s1 in spans:
        w = wav_of(i)
        if not w: print(f'  (pas de WAV pour {i})'); continue
        lo, ll = 0, 0
        if isinstance(loops.get(i), tuple):
            lo = int(loops[i][0] * rate / 1000); ll = int(loops[i][1] * rate / 1000)
        looped = sus.get(i, False) and loops.get(i) != 'oneshot'
        pos, src = s0, 0
        first_end = (lo + ll) if (looped and ll) else len(w)
        while pos < min(s1, N):
            end = min(s1, N, pos + (first_end - src))
            for k in range(pos, end): mix[k] += w[src + k - pos]
            pos = end
            if not looped: break
            src = lo if ll else 0
            first_end = (lo + ll) if ll else len(w)
            if first_end <= src: break
    sim = array.array('h', bytes(2 * N))
    for k in range(N):
        v = mix[k]
        sim[k] = -32768 if v < -32768 else (32767 if v > 32767 else v)
    sp = f'/tmp/sim_{name}.wav'
    with wave.open(sp, 'wb') as o:
        o.setnchannels(1); o.setsampwidth(2); o.setframerate(rate); o.writeframes(sim.tobytes())
    # 3) comparaison d'enveloppe (fenetres de 100 ms, on saute les 3 s de boot)
    W = rate // 10
    def env(a):
        out = []
        for s in range(0, N - W, W):
            acc = 0; mu = 0; n = W // 8
            for k in range(s, s + W, 8): mu += a[k]
            mu /= n
            for k in range(s, s + W, 8): acc += (a[k] - mu) * (a[k] - mu)   # RMS AC : un DAC gare sur un
            out.append((acc / n) ** 0.5)                                    # offset DC n'est PAS du son
        return out
    er, es = env(ref), env(sim)
    floor_r = max(80.0, sorted(er)[len(er)//10] * 3)                   # plancher : 3x le decile bas
    floor_s = max(80.0, sorted(es)[len(es)//10] * 3)
    n0 = 30                                                            # 3 s de boot ignorees
    agree = mismA = mismB = 0
    spans_bad = []
    cur = None
    for k in range(n0, min(len(er), len(es))):
        ra, sa = er[k] > floor_r, es[k] > floor_s
        if ra == sa:
            agree += 1
            if cur: spans_bad.append(cur); cur = None
        else:
            if ra: mismA += 1
            else: mismB += 1
            typ = 'SIM-MUET' if ra else 'SIM-EXTRA'
            if cur and cur[0] == typ: cur = (typ, cur[1], (k+1)*100)
            else:
                if cur: spans_bad.append(cur)
                cur = (typ, k*100, (k+1)*100)
    if cur: spans_bad.append(cur)
    tot = agree + mismA + mismB
    print(f'  accord {100.0*agree/tot:.1f}%  (sim-muet {mismA}, sim-extra {mismB} / {tot} fenetres)  sim: {sp}')
    for typ, a, b in spans_bad:
        if b - a >= 300: print(f'    {typ:9s} {a:6d}-{b} ms')

if __name__ == '__main__':
    main()
