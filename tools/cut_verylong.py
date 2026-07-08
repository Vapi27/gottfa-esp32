#!/usr/bin/env python3
# cut_verylong.py <long.wav> <out.wav> — musiques EVOLUTIVES : cherche une vraie periode jusqu'a ~140 s
# (grille decimee) ; sinon tranche TRES LONGUE (~4-5 min) au meilleur raccord. Imprime "introMs seam".
import sys, wave, struct, math

def load(p):
    w = wave.open(p); fr = w.getframerate(); n = w.getnframes()
    s = list(struct.unpack('<%dh' % n, w.readframes(n))); w.close(); return s, fr

def corr(e, a, b, L):
    L = min(L, len(e)-a, len(e)-b)
    if L < 30: return -1
    x = e[a:a+L]; y = e[b:b+L]
    mx, my = sum(x)/L, sum(y)/L
    num = sum((x[i]-mx)*(y[i]-my) for i in range(L))
    dx = math.sqrt(sum((v-mx)**2 for v in x)) or 1; dy = math.sqrt(sum((v-my)**2 for v in y)) or 1
    return num/(dx*dy)

s, fr = load(sys.argv[1])
d = [s[i]-s[i-1] for i in range(1, len(s))]
blk = fr // 10                                                # enveloppe 100 ms (grille longue)
E = [math.sqrt(sum(v*v for v in d[i:i+blk])/blk) for i in range(0, len(d)-blk, blk)]
t0 = 100                                                      # 10 s
best = (0, -1)
for P in range(20, min(1400, (len(E)-t0)//2 - 10)):           # periode 2 s .. 140 s
    L = min(900, len(E)-t0-P)
    c = corr(E, t0, t0+P, L)
    if c > best[1]: best = (P, c)
P100, conf = best
if conf > 0.75:                                               # vraie periode longue trouvee
    P_ms = P100 * 100
    cut = None                                                # intro au meilleur raccord (grille 100 ms)
    for t in range(0, min(600, len(E)-2*P100-15)):
        seam = corr(E, t+P100, t, min(150, len(E)-(t+P100)-5))
        if cut is None or seam > cut[1]: cut = (t*100, seam)
    intro, seam = cut
    mode = f"periode-longue {P_ms} ms (conf {conf:.2f})"
else:                                                         # pas de periode -> tranche pleine ~280 s, meilleur raccord
    P_ms = 280000
    cut = None
    for t in range(0, 600):
        endE = (t*100 + P_ms) // 100
        if endE + 60 > len(E): break
        seam = corr(E, endE, t, 120)
        if cut is None or seam > cut[1]: cut = (t*100, seam)
    intro, seam = cut if cut else (0, 0.0)
    mode = "tranche pleine 280 s"
end = intro + P_ms
out = s[:int(end * fr / 1000)]
w = wave.open(sys.argv[2], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(fr)
w.writeframes(struct.pack('<%dh' % len(out), *[max(-32768, min(32767, int(v))) for v in out])); w.close()
print(f"{intro} {seam:.2f} # {mode}, fichier {end/1000:.1f} s")
