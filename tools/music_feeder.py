#!/usr/bin/env python3
"""Feed the wall's music mode from this computer's microphone.

    pip install sounddevice numpy
    python3 tools/music_feeder.py [--host arena.local]

The zero-hardware way to run music mode: the computer listens to the room and
pushes energy/bass/treble to /api/music. Ctrl-C stops; the wall drops back to
its gentle breathe 2 s later.

Two design points, both learned the slow way:
- The host is resolved ONCE and the HTTP connection is persistent. Resolving
  arena.local through mDNS and opening a fresh TCP connection per frame cost
  5031 ms per push — the wall lagged five seconds behind the music. Keep-alive
  to the bare IP costs 27 ms.
- The audio callback never touches the network. It only stores the newest
  envelope; a sender thread ships the freshest value at 20 Hz and DROPS stale
  ones. Queueing them would turn any hiccup into permanent lag.
"""
import socket, sys, threading, time
import http.client
import numpy as np
import sounddevice as sd

host = "arena.local"
if "--host" in sys.argv:
    host = sys.argv[sys.argv.index("--host") + 1]
ip = socket.gethostbyname(host)                       # mDNS once, then raw IP

RATE, BLOCK = 16000, 512                              # 32 ms blocks
latest = [0, 0, 0]
lp, peak = 0.0, 0.05
run = True

def sender():
    conn = http.client.HTTPConnection(ip, 80, timeout=1)
    while run:
        e, b, t = latest
        try:
            conn.request("GET", "/api/music?e=%d&b=%d&t=%d" % (e, b, t))
            conn.getresponse().read()
        except Exception:
            try: conn.close()
            except Exception: pass
            conn = http.client.HTTPConnection(ip, 80, timeout=1)
        time.sleep(0.05)                              # 20 Hz, freshest frame only

def cb(indata, frames, t_, status):
    global lp, peak
    x = indata[:, 0].astype(np.float32)
    out = np.empty_like(x)
    l = lp
    for i in range(len(x)):                           # one-pole LP = bass proxy
        l += 0.10 * (x[i] - l)
        out[i] = l
    lp = l
    rms  = float(np.sqrt(np.mean(x * x)))
    bass = float(np.sqrt(np.mean(out * out)))
    peak = max(rms, peak * 0.997)
    e  = min(1.0, rms / peak)
    b  = min(1.0, bass / peak)
    tr = min(1.0, max(0.0, (rms - bass) / peak))
    latest[0], latest[1], latest[2] = int(e * 255), int(b * 255), int(tr * 255)
    print("\r%5.1f%% %-40s" % (e * 100, "#" * int(e * 40)), end="", flush=True)

http.client.HTTPConnection(ip, 80, timeout=5).request("GET", "/api/set?mode=music")
th = threading.Thread(target=sender, daemon=True)
th.start()
print("music mode on — %s (%s), Ctrl-C stops" % (host, ip))
try:
    with sd.InputStream(channels=1, samplerate=RATE, blocksize=BLOCK, callback=cb):
        while True:
            time.sleep(1)
except KeyboardInterrupt:
    run = False
    print("\nstopped — the wall breathes again in 2 s")
