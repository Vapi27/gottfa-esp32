#!/usr/bin/env python3
# find_loops.py — detecte la PERIODE MUSICALE de chaque son en boucle ('l') d'un theme et genere
# loops.txt avec "id=0,longueurMs" (longueur = nb ENTIER de mesures) -> le rebouclage tombe sur le temps.
# Pur Python (pas de numpy) : enveloppe RMS -> periode grossiere, puis raffinage sur l'audio brut.
# Usage: python3 tools/find_loops.py <dossier_theme>   (ex: ~/gosowav_sd/arena)
import wave, struct, math, os, sys, glob, re
def load(f):
    w=wave.open(f,'rb'); fr=w.getframerate(); n=w.getnframes(); raw=w.readframes(n); ch=w.getnchannels(); w.close()
    s=list(struct.unpack('<%dh'%(len(raw)//2), raw))
    if ch==2: s=[(s[i]+s[i+1])//2 for i in range(0,len(s),2)]
    return s, fr
def period(s, fr):
    N=len(s); dur=N/fr
    blk=max(1,int(fr*0.005)); env=[]
    for i in range(0,N-blk,blk):
        seg=s[i:i+blk]; env.append(math.sqrt(sum(v*v for v in seg)/len(seg)))
    m=sum(env)/len(env); env=[e-m for e in env]
    e0=sum(e*e for e in env) or 1
    lo=int(0.3/0.005); hi=min(len(env)//2, int(3.5/0.005)); best=(0,0.0)
    for L in range(lo,hi):
        c=sum(env[i]*env[i+L] for i in range(len(env)-L))/e0
        if c>best[1]: best=(L,c)
    coarse=best[0]*blk; conf=best[1]
    # raffinage echantillon autour de coarse
    W=int(fr*0.3); a=int(fr*0.5); span=int(fr*0.006); bl=(coarse,-9)
    if coarse>0 and a+coarse+span+W<N:
        for lag in range(coarse-span,coarse+span):
            num=sum(s[a+i]*s[a+lag+i] for i in range(0,W,4))
            d=math.sqrt((sum(s[a+i]**2 for i in range(0,W,4))+1)*(sum(s[a+lag+i]**2 for i in range(0,W,4))+1))
            c=num/d
            if c>bl[1]: bl=(lag,c)
    return bl[0], bl[1], dur, fr   # conf = correlation AUDIO BRUT (raffinage) = vraie periodicite, pas l'enveloppe
theme=os.path.expanduser(sys.argv[1] if len(sys.argv)>1 else '~/gosowav_sd/arena')
out=[]
for f in sorted(glob.glob(theme+'/*.wav')):
    nm=os.path.basename(f); mt=re.match(r'0*(\d+)-([a-z]*)-', nm)
    if not mt or 'l' not in mt.group(2): continue            # seulement les sons 'l' (boucle)
    sid=int(mt.group(1)); s,fr=load(f); P,conf,dur,_=period(s,fr)
    if conf>=0.65 and P>0 and P/fr>=0.6:                      # confiant + vraie mesure
        k=max(1,int(dur*fr//P)); looplen=int(1000*k*P/fr)     # nb entier de periodes tenant dans le WAV
        out.append((sid, 0, looplen, conf, 1000*P/fr, k))
        print(f"  id {sid:3d}: periode {1000*P/fr:6.1f}ms conf {conf:.2f} -> boucle 0,{looplen}ms ({k} mesures)")
    else:
        print(f"  id {sid:3d}: periode {1000*P/fr:6.1f}ms conf {conf:.2f} -> IGNORE (peu sur / sous-temps), boucle fichier entier")
print("\n--- loops.txt propose ---")
for sid,st,ll,conf,pms,k in out: print(f"{sid}={st},{ll}")
