# period2.py <long.wav> : periode fondamentale + intro ; REPLI pour morceaux evolutifs = grande tranche calee mesure.
import wave, struct, math, sys
w = wave.open(sys.argv[1]); fr = w.getframerate(); n = w.getnframes()
s = struct.unpack('<%dh' % n, w.readframes(n)); w.close()
blk = fr // 100
env = [math.sqrt(sum(v*v for v in s[i:i+blk])/blk) for i in range(0, n-blk, blk)]
def corr(aOff, bOff, L):
    a = env[aOff:aOff+L]; b = env[bOff:bOff+L]
    ma, mb = sum(a)/L, sum(b)/L
    num = sum((a[i]-ma)*(b[i]-mb) for i in range(L))
    da = math.sqrt(sum((v-ma)**2 for v in a)) or 1; db = math.sqrt(sum((v-mb)**2 for v in b)) or 1
    return num/(da*db)
t0 = 1200
cands = []
for P in range(200, 3200, 2):
    L = min(2200, len(env)-t0-P)
    if L < 400: break
    cands.append((P, corr(t0, t0+P, L)))
mx = max(c for _, c in cands)
hit = [p for p, c in cands if c >= 0.97*mx and c > 0.5]
if hit:
    P = hit[0]
    P = max(range(P-3, P+4), key=lambda q: corr(t0, t0+q, min(2200, len(env)-t0-q)))
    intro = 0
    for t in range(0, 1500, 5):
        if t + 2*P + 100 > len(env): break
        if corr(t, t+P, P) > 0.93: intro = t; break
    print(f"{P*10} {intro*10} {mx:.2f} exact")
else:
    # REPLI evolutif : mesure musicale (autocorr 0.3..3.5 s) -> P = k*mesure le plus proche de 28 s
    best = (60, -9)
    for m in range(30, 350):
        L = min(2500, len(env)-t0-m)
        c = corr(t0, t0+m, L)
        if c > best[1]: best = (m, c)
    m = best[0]
    P = (2800 // m) * m
    print(f"{P*10} 0 {best[1]:.2f} mesure({m*10}ms)")
