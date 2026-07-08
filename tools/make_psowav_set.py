#!/usr/bin/env python3
# make_psowav_set.py — GENERATEUR UNIVERSEL de set PSOWAV pour System 80B (toutes generations).
# Tu lui donnes un jeu (ses ROM), il sort le set COMPLET, sans aucune intervention :
#   1. classifie chaque commande en faisant tourner la VRAIE ROM (muet / one-shot+duree / boucle)
#   2. detecte les MIROIRS (plage 32-63 dupliquee, ex. Arena Gen1) -> copies, pas de re-rendu
#   3. musiques : rendu long (70 s) -> VRAIE periode musicale + intro par auto-similarite -> decoupe exacte
#   4. one-shots : rendu a leur duree ROM exacte (+0,35 s de queue)
#   5. post-traitement = le FILTRE DE SORTIE du vrai board (passe-bas 3 kHz, calibre sur des
#      enregistrements de vraie machine) + niveau normalise (RMS cible ~3000 = niveau des sets LISY)
#   6. loops.txt genere : intros + "@sil=" (seuil de silence du chef AUTO-CALIBRE sur les pauses
#      internes maximales des musiques DE CE JEU)
#   7. --push IP : selectionne le jeu sur la carte et pousse tout via /up + /loops (zero carte SD a sortir)
#
# Usage:  python3 tools/make_psowav_set.py <gameId> [--push IP] [--sd <racine_sd>]
#         python3 tools/make_psowav_set.py --all [--sd ...]        (les 16 jeux)
# Exemple: python3 tools/make_psowav_set.py genesis
import os, sys, subprocess, wave, struct, math, glob, shutil, urllib.request, urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))      # repo gottfa-esp32
SD   = os.path.expanduser('~/gosowav_sd')
OUT0 = os.path.expanduser('~/gosowav_build')
TARGET_RMS = 3000.0          # niveau des sets LISY de reference (mesure sur arena)
LP_FC      = 3000.0          # filtre de sortie du vrai board (calibre : hf/rms 0.27->0.119, cible 0.112)
TAIL_MS    = 350             # queue ajoutee apres la fin ROM d'un one-shot
LONG_S     = 70              # duree du rendu long pour la detection de periode

def sh(cmd, **kw): return subprocess.run(cmd, shell=True, capture_output=True, text=True, **kw)

def build_tools():
    """Compile psosig + render une fois (host)."""
    b = '/tmp/psowav_bin'; os.makedirs(b, exist_ok=True)
    if not os.path.exists(b + '/f6502.o'):
        assert sh(f'cd {ROOT} && gcc -c src/fake6502.c -o {b}/f6502.o && gcc -c src/emu2149.c -o {b}/emu2149.o').returncode == 0
    common = f'{ROOT}/src/psorom.cpp {ROOT}/src/sp0250.cpp {ROOT}/src/ym2151w.cpp {ROOT}/src/ymfm_opm.cpp {b}/f6502.o {b}/emu2149.o'
    for t in ['psosig', 'render']:
        src_m = max(os.path.getmtime(f'{ROOT}/tools/host_{t}.cpp'), os.path.getmtime(f'{ROOT}/src/psorom.cpp'))
        if not os.path.exists(f'{b}/{t}') or os.path.getmtime(f'{b}/{t}') < src_m:
            r = sh(f'cd {ROOT} && g++ -std=c++17 -Isrc tools/host_{t}.cpp {common} -o {b}/{t}')
            assert r.returncode == 0, r.stderr[:400]
    return b

def game_info(gid):
    for ln in open(SD + '/games.idx'):
        f = ln.strip().split('|')
        if len(f) >= 3 and f[0] == gid: return int(f[1]), f[2]
    raise SystemExit(f"jeu '{gid}' absent de games.idx")

def yrom_path(gid, gen):
    d = f'{SD}/games/{gid}'
    y1, y2 = f'{d}/yrom1.snd', f'{d}/yrom2.snd'
    if gen == 1 and os.path.exists(y2):                                  # Gen1 : yrom2 ++ yrom1 (vecteurs en haut)
        cat = f'/tmp/psowav_bin/y_{gid}.snd'
        with open(cat, 'wb') as o: o.write(open(y2, 'rb').read() + open(y1, 'rb').read())
        return cat
    return y1

def classify(b, gflag, y, d, cmd, trace=None):
    """cmd = '5' ou '30.5' (paire). Classification par l'AUDIO REND (univ. toutes gens) :
    rend 12 s -> enveloppe du signal DERIVE (vire le DC des codes de controle) ->
    actif a la fin = boucle ; sinon duree = dernier instant actif ; <=80 ms = muet."""
    lf = f"/tmp/psowav_bin/cls_{cmd.replace('.', '_')}.wav"
    sh(f'{b}/render {gflag} {y} {d} {cmd} 12 {lf}')
    try: s_, fr = load_wav(lf)
    except Exception: return ('silent', 0)
    d_ = [s_[i] - s_[i-1] for i in range(1, len(s_))]               # derive = passe-haut (anti-DC)
    blk = fr // 100
    env = [math.sqrt(sum(v*v for v in d_[i:i+blk]) / blk) for i in range(0, len(d_) - blk, blk)]
    if not env: return ('silent', 0)
    thr = max(3.0, 0.04 * max(env))
    act = [i for i, v in enumerate(env) if v > thr]
    if not act: return ('silent', 0)
    durMs = act[-1] * 10
    tail = [v for v in env[-150:]]                                   # 1.5 dernieres secondes
    if sum(1 for v in tail if v > thr) > 0.25 * len(tail):
        if len(s_) < 30 * fr:                                        # actif a 12 s : CONFIRME a 35 s (les pieces de ~25 s sont des
            lf2 = lf.replace('.wav', '_35.wav')                      # one-shots, pas des boucles !)
            sh(f'{b}/render {gflag} {y} {d} {cmd} 35 {lf2}')
            try: s2, fr2 = load_wav(lf2)
            except Exception: return ('loop', -1)
            d2 = [s2[i] - s2[i-1] for i in range(1, len(s2))]
            blk2 = fr2 // 100
            e2 = [math.sqrt(sum(v*v for v in d2[i:i+blk2]) / blk2) for i in range(0, len(d2) - blk2, blk2)]
            thr2 = max(3.0, 0.04 * max(e2))
            t2 = e2[-200:]
            if sum(1 for v in t2 if v > thr2) <= 0.2 * len(t2):
                a2 = [i for i, v in enumerate(e2) if v > thr2]
                return ('oneshot', a2[-1] * 10 if a2 else 0)
        return ('loop', -1)
    return ('oneshot', durMs)

def load_wav(p):
    w = wave.open(p); fr = w.getframerate(); n = w.getnframes()
    s = list(struct.unpack('<%dh' % n, w.readframes(n))); w.close(); return s, fr

def save_wav(p, s, fr):
    w = wave.open(p, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(fr)
    w.writeframes(struct.pack('<%dh' % len(s), *[max(-32768, min(32767, int(v))) for v in s])); w.close()

def envelope(s, fr, ms=10):
    blk = max(1, fr * ms // 1000)
    return [math.sqrt(sum(v*v for v in s[i:i+blk]) / blk) for i in range(0, len(s) - blk, blk)]

def corr(env, a, b, L):
    x = env[a:a+L]; y = env[b:b+L]
    mx, my = sum(x)/L, sum(y)/L
    num = sum((x[i]-mx)*(y[i]-my) for i in range(L))
    dx = math.sqrt(sum((v-mx)**2 for v in x)) or 1; dy = math.sqrt(sum((v-my)**2 for v in y)) or 1
    return num/(dx*dy)

def find_period(env):
    """(periodeMs, introMs, conf, mode, seam) — la coupe est choisie pour MAXIMISER le raccord :
    corr(ce qui suit la coupe dans la realite, ce que la boucle va rejouer)."""
    t0 = 1200
    cands = []
    for P in range(200, 3200, 2):
        L = min(2200, len(env)-t0-P)
        if L < 400: break
        cands.append((P, corr(env, t0, t0+P, L)))
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
            iP = t + P
            L = min(200, len(env) - iP - 5)
            if L < 80: continue
            seam = corr(env, iP, t, L)
            if cut is None or seam > cut[2]: cut = (P, t, seam)
    if cut is None: cut = (peaks[0], 0, 0.0)
    P, intro, seam = cut
    return P*10, intro*10, mx, mode, seam

def max_gap_ms(env):
    """plus longue pause interne (enveloppe < 12% de la mediane des actifs) d'une musique."""
    med = sorted(env)[len(env)//2] or 1
    gap = cur = 0
    for v in env[50:-50]:
        if v < 0.12*med + 2: cur += 1
        else: gap = max(gap, cur); cur = 0
    return max(gap, cur) * 10

def post(s, fr):
    """DC-kill (HP 20 Hz) + filtre de sortie 3 kHz + AUCUN gain par fichier : les niveaux relatifs
    viennent des puces emulees (verifie : sons DAC = x0.97-1.00 des enregistrements vraie-machine).
    L'ancienne normalisation RMS etait trompee par l'offset DC du DAC -> ecrasait l'audio x3-6."""
    ah = 1.0 - math.exp(-2*math.pi*20/fr); yh = 0.0; h = []
    for v in s: yh += ah*(v-yh); h.append(v - yh)                # passe-haut 20 Hz = vire le DC du sample&hold
    a = 1.0 - math.exp(-2*math.pi*LP_FC/fr); y = 0.0; f = []
    for v in h: y += a*(v-y); f.append(y)
    return f

def push_board(ip, gid, outdir, loops):
    idx = None
    for i, ln in enumerate(open(SD + '/games.idx')):
        if ln.strip().split('|')[0] == gid: idx = i; break
    print(f"  push -> selection du jeu {gid} (index {idx}) sur {ip}")
    urllib.request.urlopen(f'http://{ip}/load?i={idx}', timeout=5).read()
    import time; time.sleep(6)
    for f in sorted(glob.glob(outdir + '/*.wav')):
        r = sh(f'curl -s -m 60 -F "f=@{f}" http://{ip}/up').stdout
        print(f"    {os.path.basename(f)} -> {r}")
    d = urllib.parse.quote(';'.join(loops))
    urllib.request.urlopen(f'http://{ip}/loops?d={d}', timeout=10).read()
    print("    loops.txt pousse + rechargement")

def make(gid, push_ip=None):
    gen, title = game_info(gid)
    b = build_tools()
    gflag = {1: '1', 2: '2', 3: 'b'}[gen]
    y, d = yrom_path(gid, gen), f'{SD}/games/{gid}/drom1.snd'
    outdir = f'{OUT0}/{gid}'; shutil.rmtree(outdir, ignore_errors=True); os.makedirs(outdir)
    print(f"== {title} (Gen{gen}) : commandes simples 1..31 ==")
    info = {}                                                            # extId -> (typ, dur) ; cmdarg[extId] = '5' ou '30.5'
    cmdarg = {}
    for c in range(1, 32):                                               # le bus 80B = 5 bits pour TOUTES les gens (ROM: AND #$1F)
        typ, dur = classify(b, gflag, y, d, str(c))
        if typ == 'oneshot' and dur <= 80: typ = 'silent'                # blip/code de controle
        info[c] = (typ, dur); cmdarg[c] = str(c)
        if typ != 'silent': print(f"  cmd {c:2d}: {typ}{f' {dur} ms' if dur > 0 else ''}")
    # --- detection AUTO des headers de banque (protocole decompile : ROM 1/2 = +0x20/+0x40, bus = ~v) ---
    probe = next((c for c in range(1, 32) if info[c][0] != 'silent'), None)
    headers = {}                                                         # busVal -> banque (1 ou 2)
    for h in range(1, 32):
        if info[h][0] != 'silent' or probe is None: continue
        bank = (~h) & 0x1F                                               # valeur ROM du header
        if bank not in (1, 2): continue
        t2, d2 = classify(b, gflag, y, d, f'{h}.{probe}', 8000)
        t1, d1 = info[probe]
        if (t2, d2) != (t1, d1) and not (t2 == t1 == 'loop'):            # la paire change le resultat -> header
            headers[h] = bank
        elif t2 == t1 == 'loop':                                         # deux boucles : depart en periode pour trancher ? simple : header probable
            headers[h] = bank
    if headers: print(f"  BANQUES detectees : headers bus {dict(headers)} (paires header.valeur)")
    for h, bank in sorted(headers.items()):                              # enumeration des banques
        for v in range(1, 32):
            typ, dur = classify(b, gflag, y, d, f'{h}.{v}')
            if typ == 'oneshot' and dur <= 80: typ = 'silent'
            ext = bank * 32 + v
            info[ext] = (typ, dur); cmdarg[ext] = f'{h}.{v}'
            if typ != 'silent': print(f"  cmd {h}.{v} -> son {ext}: {typ}{f' {dur} ms' if dur > 0 else ''}")
    mirrors = {}                                                         # (plus de balayage direct 32-63 : inaccessible sur le vrai bus)
    loops_txt, gaps = [], []
    made = {}
    for c, (typ, dur) in sorted(info.items()):
        if typ == 'silent' or c in mirrors: continue
        if typ == 'oneshot':
            f = f'{outdir}/{c:04d}-100-rom.wav'
            sh(f'{b}/render {gflag} {y} {d} {cmdarg[c]} {(dur + TAIL_MS) / 1000.0} {f}')
            s, fr = load_wav(f); save_wav(f, post(s, fr), fr); made[c] = f
            print(f"  one-shot {c:2d}: {dur} ms")
        else:
            lf = f'/tmp/psowav_bin/long_{gid}_{c}.wav'
            sh(f'{b}/render {gflag} {y} {d} {cmdarg[c]} {LONG_S} {lf}')
            s, fr = load_wav(lf); env = envelope(s, fr)
            P, intro, conf, mode, seam = find_period(env)
            gaps.append(max_gap_ms(env))
            f = f'{outdir}/{c:04d}-l-100-rom.wav'
            cut = s[:int((intro + P) * fr / 1000)]
            save_wav(f, post(cut, fr), fr); made[c] = f
            if intro: loops_txt.append(f'{c}={intro}')
            print(f"  musique  {c:2d}: periode {P} ms intro {intro} ms ({mode}, conf {conf:.2f}, raccord {seam:.2f}{' !! DOUTEUX' if seam < 0.7 else ''})")
    for c, base in sorted(mirrors.items()):                              # miroirs = copies du fichier de base
        if base in made:
            ext = '-l-100-rom.wav' if info[c][0] == 'loop' else '-100-rom.wav'
            shutil.copy(made[base], f'{outdir}/{c:04d}{ext}')
    if mirrors: print(f"  miroirs : {sorted(mirrors.keys())} (copies de {sorted(set(mirrors.values()))})")
    refdir = f'{SD}/{gid}'                                               # CALIBRATION-REFERENCE : si des enregistrements vraie-machine
    refs = {int(os.path.basename(rp).split('-')[0]): rp                  # existent (sets LISY), chaque son est CALE sur le sien
            for rp in glob.glob(refdir + '/*.wav')}
    ncal = 0
    for c, f in made.items():
        if c not in refs: continue
        s2, fr2 = load_wav(f); r2, rfr = load_wav(refs[c])
        win = max(0.4, min(3.5, len(r2)/rfr - 0.1))
        def _rw(x, xfr):
            n2 = min(len(x), int(win*xfr))
            d2 = [x[i]-x[i-1] for i in range(1, n2)]
            return math.sqrt(sum(v*v for v in d2)/len(d2)) or 1
        g2 = max(0.2, min(5.0, _rw(r2, rfr) / _rw(s2, fr2)))
        save_wav(f, [v*g2 for v in s2], fr2); ncal += 1
    if ncal: print(f"  calibration-reference : {ncal} sons cales sur les enregistrements vraie-machine")
    gaps = [g for g in gaps if g < 25000]                                # pause > 25 s = rendu degenere, pas une pause musicale
    sil = min(15000, max(4000, int(2.2 * max(gaps)) if gaps else 6000))  # seuil chef = 2.2x la pire pause interne DE CE JEU
    loops_txt.append(f'@sil={sil}')
    if headers: loops_txt.append('@hdr=' + ','.join(str(h) for h in sorted(headers)))
    open(f'{outdir}/loops.txt', 'w').write('\n'.join(loops_txt) + '\n')
    print(f"  loops.txt : {' '.join(loops_txt)}  (pause interne max mesuree {max(gaps) if gaps else 0} ms)")
    print(f"=> {len(glob.glob(outdir + '/*.wav'))} WAV dans {outdir}")
    if push_ip: push_board(push_ip, gid, outdir, loops_txt)

if __name__ == '__main__':
    args = sys.argv[1:]
    push = args[args.index('--push') + 1] if '--push' in args else None
    if '--all' in args:
        for ln in open(SD + '/games.idx'):
            make(ln.strip().split('|')[0])
    else:
        make(args[0], push)
