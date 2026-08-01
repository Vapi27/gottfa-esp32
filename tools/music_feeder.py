#!/usr/bin/env python3
"""Feed the wall's music mode from this computer's microphone.

    pip install sounddevice
    python3 tools/music_feeder.py [--host arena.local]

Captures the mic, computes energy / bass / treble the same way the on-board
mic path does (RMS + one-pole low-pass split, adaptive peak), and pushes
/api/music at ~15 Hz. This is the zero-hardware way to run music mode: play
music in the room, the computer listens, the wall dances. Ctrl-C to stop —
the wall drops back to its gentle breathe 2 s later.
"""
import math, sys, time, urllib.request
import numpy as np
import sounddevice as sd

host = "arena.local"
if "--host" in sys.argv:
    host = sys.argv[sys.argv.index("--host") + 1]
BASE = "http://%s" % host

RATE, BLOCK = 16000, 1024          # 64 ms blocks -> ~15 pushes/s
lp = 0.0
peak = 0.05

urllib.request.urlopen(BASE + "/api/set?mode=music", timeout=5).read()
print("music mode on — playing the room's sound onto the wall (Ctrl-C stops)")

def cb(indata, frames, t, status):
    global lp, peak
    x = indata[:, 0].astype(np.float32)
    # one-pole low-pass ~ bass proxy, same crude split as the firmware
    out = np.empty_like(x)
    l = lp
    for i in range(len(x)):
        l += 0.10 * (x[i] - l)
        out[i] = l
    lp = l
    rms  = float(np.sqrt(np.mean(x * x)))
    bass = float(np.sqrt(np.mean(out * out)))
    peak = max(rms, peak * 0.995)
    e = min(1.0, rms / peak)
    b = min(1.0, bass / peak)
    tr = min(1.0, max(0.0, (rms - bass) / peak))
    try:
        urllib.request.urlopen(BASE + "/api/music?e=%d&b=%d&t=%d"
                               % (e * 255, b * 255, tr * 255), timeout=2).read()
    except Exception:
        pass
    bar = "#" * int(e * 40)
    print("\r%5.1f%% %-40s" % (e * 100, bar), end="", flush=True)

with sd.InputStream(channels=1, samplerate=RATE, blocksize=BLOCK, callback=cb):
    while True:
        time.sleep(1)
