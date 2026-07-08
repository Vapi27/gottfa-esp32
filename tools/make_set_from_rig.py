#!/usr/bin/env python3
# make_set_from_rig.py — set final a partir des RENDUS LONGS PinMAME du rig (moteur authentique).
# PinMAME fournit le CONTENU (balance/filtres corrects par construction) ; ici on ne fait QUE la
# decoupe musicale : vraie periode + intro par auto-similarite, coupe choisie pour MAXIMISER le
# raccord, loops.txt (intros + @sil auto). AUCUN filtre, AUCUNE calibration de niveau.
# Usage: make_set_from_rig.py <dossier_rendus_longs> <dossier_sortie>
import os, sys, glob, wave, struct, math, shutil

def load(p):
    w = wave.open(p); fr = w.getframerate(); n = w.getnframes(); ch = w.getnchannels()
    raw = w.readframes(n); w.close()
    s = list(struct.unpack('<%dh' % (len(raw)//2), raw))
    if ch == 2: s = [(s[i] + s[i+1]) // 2 for i in range(0, len(s) - 1, 2)]
    return s, fr

def save(p, s, fr):
    w = wave.open(p, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(fr)
    w.writeframes(struct.pack('<%dh' % len(s), *[max(-32768, min(32767, int(v))) for v in s])); w.close()

def envelope(s, fr):
    d = [s[i] - s[i-1] for i in range(1, len(s))]
    blk = max(1, fr // 100)
    return [math.sqrt(sum(v*v for v in d[i:i+blk]) / blk) for i in range(0, len(d) - blk, blk)]

def corr(env, a, b, L):
    x = env[a:a+L]; y = env[b:b+L]
    if L < 40: return -1
    mx, my = sum(x)/L, sum(y)/L
    num = sum((x[i]-mx)*(y[i]-my) for i in range(L))
    dx = math.sqrt(sum((v-mx)**2 for v in x)) or 1; dy = math.sqrt(sum((v-my)**2 for v in y)) or 1
    return num/(dx*dy)

def best_cut(env):
    """(periodeMs, introMs, conf, mode, raccord) — coupe qui maximise le raccord."""
    t0 = min(1200, max(300, len(env)//3))
    cands = []
    for P in range(200, min(3200, (len(env)-t0)//2 - 60), 2):
        L = min(2200, len(env)-t0-P)
        if L < 400: break
        cands.append((P, corr(env, t0, t0+P, L)))
    if not cands: return None
    mx = max(c for _, c in cands)
    if mx > 0.5:
        sel = [p for p, c in cands if c >= 0.93*mx]
        peaks = []
        for p_ in sel:
            if not peaks or p_ - peaks[-1] > 30: peaks.append(p_)
        peaks = peaks[:4]; mode = 'exact'
    else:
        best = max(((m, corr(env, t0, t0+m, min(2500, len(env)-t0-m))) for m in range(30, 350)), key=lambda x: x[1])
        k = max(1, 2800 // best[0]); peaks = [k * best[0]]
        mode = f'mesure({best[0]*10}ms)'
    cut = None
    for P in peaks:
        tmax = min(1500, len(env) - 2*P - 110)
        for t in range(0, max(10, tmax), 10):
            L = min(200, len(env) - (t+P) - 5)
            seam = corr(env, t+P, t, L)
            if cut is None or seam > cut[2]: cut = (P, t, seam)
    P, intro, seam = cut
    return P*10, intro*10, mx, mode, seam

def main(indir, outdir):
    shutil.rmtree(outdir, ignore_errors=True); os.makedirs(outdir)
    loops, gaps = [], []
    for p in sorted(glob.glob(indir + '/*.wav')):
        n = os.path.basename(p); cid = int(n.split('-')[0])
        if '-l-' not in n:                                            # one-shot : PinMAME l'a deja coupe au silence
            shutil.copy(p, f'{outdir}/{n}'); print(f"  one-shot {cid:2d}: copie tel quel")
            continue
        s, fr = load(p)
        if len(s) < 12*fr:                                            # boucle courte rendue <12 s : garder tel quel
            shutil.copy(p, f'{outdir}/{n}'); print(f"  boucle   {cid:2d}: courte ({len(s)/fr:.1f} s), tel quel")
            continue
        env = envelope(s, fr)
        r = best_cut(env)
        if r is None:
            shutil.copy(p, f'{outdir}/{n}'); print(f"  boucle   {cid:2d}: pas de coupe trouvee, tel quel"); continue
        P, intro, conf, mode, seam = r
        cut = s[:int((intro + P) * fr / 1000)]
        save(f'{outdir}/{n}', cut, fr)
        if intro: loops.append(f'{cid}={intro}')
        med = sorted(env)[len(env)//2] or 1                           # pause interne max (pour @sil)
        g = c = 0
        for v in env[50:-50]:
            if v < 0.12*med + 2: c += 1
            else: g = max(g, c); c = 0
        gaps.append(max(g, c) * 10)
        print(f"  musique  {cid:2d}: periode {P} ms intro {intro} ms ({mode}, conf {conf:.2f}, raccord {seam:.2f}{' !! DOUTEUX' if seam < 0.7 else ''})")
    gaps = [g for g in gaps if g < 25000]
    sil = min(15000, max(4000, int(2.2 * max(gaps)) if gaps else 6000))
    loops.append(f'@sil={sil}')
    open(f'{outdir}/loops.txt', 'w').write('\n'.join(loops) + '\n')
    print(f"=> {len(glob.glob(outdir + '/*.wav'))} WAV ; loops.txt : {' '.join(loops)}")

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
