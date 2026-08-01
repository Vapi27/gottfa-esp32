#!/usr/bin/env python3
"""Pack a raw lamp capture (capture_attract JSON) into the board's attract .bin.

    python3 tools/pack_attract.py raw.json -o data/arena_attract.bin [--skip 20] [--length 120]

Replays the change-list into a full lamp state per step, drops the first
--skip seconds (the power-on lamp test must not be part of the loop), keeps
--length seconds, and writes [u16 step_ms][u16 frames][u64 mask x frames].
"""
import json, struct, sys

args = sys.argv[1:]
def opt(name, default):
    if name in args:
        i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
    return default
out    = opt("-o", "data/arena_attract.bin")
skip_s = float(opt("--skip", "20"))
len_s  = float(opt("--length", "120"))
if not args:
    sys.exit(__doc__)

cap = json.load(open(args[0]))
STEP, frames = cap["step_ms"], cap["frames"]
end = frames[-1]["t"]
state, timeline, idx = [0] * 64, [], 0
for t in range(0, end + STEP, STEP):
    while idx < len(frames) and frames[idx]["t"] <= t:
        for lamp, s in frames[idx]["c"]:
            if lamp < 64:
                state[lamp] = s
        idx += 1
    m = 0
    for i, v in enumerate(state):
        if v:
            m |= 1 << i
    timeline.append(m)

start = int(skip_s * 1000 / STEP)
n     = int(len_s  * 1000 / STEP)
seq   = timeline[start:start + n]
if len(seq) < n:
    print("warning: capture shorter than requested — packing %d frames" % len(seq), file=sys.stderr)
blob = struct.pack("<HH", STEP, len(seq)) + b"".join(struct.pack("<Q", m) for m in seq)
open(out, "wb").write(blob)

lamps = sorted({l for m in seq for l in range(64) if m >> l & 1})
counts = [bin(m).count("1") for m in seq]
print("%s: %d frames x %d ms = %.1f s, %d bytes" % (out, len(seq), STEP, len(seq) * STEP / 1000, len(blob)), file=sys.stderr)
print("lamps driven: %d %s" % (len(lamps), lamps), file=sys.stderr)
print("lit at once: min %d, max %d" % (min(counts), max(counts)), file=sys.stderr)
if not lamps:
    sys.exit("ERROR: no lamps in the packed window — attract not running? try a bigger --skip")
