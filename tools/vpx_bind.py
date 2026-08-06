#!/usr/bin/env python3
"""Extract a playfield plan from a Visual Pinball table — from the binding the
table actually uses, not from the object's name.

    python3 tools/vpx_bind.py <table.vpx> [--attract data/arena_attract.bin]
                              [-o data/arena_pf.json] [--report report.json]

WHY THIS EXISTS
tools/vpx_inserts.py derives the lamp number with a regex on the object name,
`^L(\\d+)`. That name is a label the author types for their own convenience; it
is NOT what drives the game. VPM tables call `vpmMapLights <collection>`, and
core.vbs does:

    idx = obj.TimerInterval : Set Lights(idx) = obj

So the lamp number is each light's **TimerInterval**, stored in the BIFF record
`TMIN`. Measured on the real files, 2026-08-04:

  Arena   — the object named "L1" has TMIN=9, "L2" has TMIN=48. Those are exactly
            two of the three corrections that had previously been found by hand,
            standing in front of the physical playfield. They were in the file
            all along; we were reading the wrong field.
  Volcano — the regex matches 0 of 300 lights (they are named "Light44",
            "sLight94", "GI_Bulb2"). TMIN yields 53. The old tool therefore
            produced an EMPTY plan, silently, and /api/game would have accepted
            it and rebooted the board onto it, losing the owner's mapping.

WHAT IT CANNOT DO
If the author made the same mistake in both the name and the TimerInterval, both
sources agree and nothing in the file can see it. That is the residual risk of
publishing a bundle for a machine nobody has looked at.
"""
import json, re, struct, sys, math
from collections import defaultdict

try:
    import olefile
except ImportError:
    sys.exit("pip install olefile   (pure python, no build)")

LIGHT = 7          # GameItem type


# --- BIFF ------------------------------------------------------------------
def biff(d):
    """Yield (tag, payload). Records are [uint32 size][4-byte tag][size-4 bytes]."""
    i = 0
    while i + 8 <= len(d):
        size = struct.unpack_from("<I", d, i)[0]
        if size < 4 or i + 4 + size > len(d):
            break
        yield d[i + 4:i + 8], d[i + 8:i + 4 + size]
        i += 4 + size


def wstr(pl):
    """A BIFF string: [uint32 length][bytes]. The encoding is NOT uniform across
    the file - GameItem NAME records are UTF-16LE, GameData ones (IMAG...) are
    plain 8-bit. Decoding UTF-16 as latin-1 gives 'L 1 2'; decoding 8-bit as
    UTF-16 gives 'Playfield_' as '汐祡楦汥彤'. Both happened here. Sniff instead
    of assuming: real UTF-16LE ASCII has a zero as every second byte."""
    if len(pl) < 4:
        return ""
    n = struct.unpack_from("<I", pl, 0)[0]
    raw = pl[4:4 + n]
    if len(raw) >= 4 and raw[1::2].count(0) > len(raw) // 4:
        return raw.decode("utf-16-le", "replace").rstrip("\x00")
    return raw.decode("latin-1", "replace").rstrip("\x00")


def find_code(data):
    """The script. Format trap: the CODE record declares size 4 (the tag alone)
    and the text follows OUTSIDE the record as [uint32 length][text], so a naive
    BIFF walk reads the start of the script as the next tag."""
    at = data.find(b"CODE")
    if at < 0:
        return ""
    n = struct.unpack_from("<I", data, at + 4)[0]
    if not (0 < n < len(data)):
        return ""
    return data[at + 8:at + 8 + n].decode("latin-1", "replace")


# --- reading the table ------------------------------------------------------
def read_table(path):
    ole = olefile.OleFileIO(path)
    gd = ole.openstream("GameStg/GameData").read()

    bounds, image = {}, ""
    for tag, pl in biff(gd):
        if tag in (b"LEFT", b"RGHT", b"TOPX", b"BOTM") and len(pl) >= 4:
            bounds[tag.decode()] = struct.unpack_from("<f", pl, 0)[0]
        elif tag == b"IMAG" and not image:
            image = wstr(pl)

    script = find_code(gd)

    # Which collection does the table hand to vpmMapLights? Never hard-code the
    # name: Arena says AllLights, Volcano says InsertLights.
    m = re.search(r"vpmMapLights\s+([A-Za-z_]\w*)", script)
    coll_name = m.group(1) if m else ""

    collections = {}
    for e in ole.listdir():
        if len(e) != 2 or e[0] != "GameStg" or not e[1].startswith("Collection"):
            continue
        nm, items = "", []
        for tag, pl in biff(ole.openstream("/".join(e)).read()):
            if tag == b"NAME" and not nm:
                nm = wstr(pl)
            elif tag == b"ITEM":
                items.append(wstr(pl))
        if nm:
            collections[nm] = items

    lights = {}
    for e in ole.listdir():
        if len(e) != 2 or e[0] != "GameStg" or not e[1].startswith("GameItem"):
            continue
        d = ole.openstream("/".join(e)).read()
        if len(d) < 4 or struct.unpack_from("<I", d, 0)[0] != LIGHT:
            continue
        rec = {"name": "", "x": None, "y": None, "lamp": None,
               "radius": None, "image": "", "colour": None, "surface": ""}
        for tag, pl in biff(d[4:]):
            if tag == b"NAME" and not rec["name"]:
                rec["name"] = wstr(pl)
            elif tag == b"VCEN" and len(pl) >= 8:
                rec["x"], rec["y"] = struct.unpack_from("<ff", pl, 0)
            elif tag == b"TMIN" and len(pl) >= 4:
                rec["lamp"] = struct.unpack_from("<i", pl, 0)[0]
            elif tag == b"RADI" and len(pl) >= 4:
                rec["radius"] = round(struct.unpack_from("<f", pl, 0)[0], 2)
            elif tag == b"IMG1" and not rec["image"]:
                rec["image"] = wstr(pl)
            elif tag == b"COLR" and len(pl) >= 4:
                rec["colour"] = struct.unpack_from("<I", pl, 0)[0]
            elif tag == b"SURF" and not rec["surface"]:
                rec["surface"] = wstr(pl)
        if rec["name"]:
            lights[rec["name"]] = rec

    return {"bounds": bounds, "image": image, "script": script,
            "collection": coll_name, "collections": collections, "lights": lights}


# --- the checks -------------------------------------------------------------
def detect_bench(members):
    """Diagnostic/service bulb banks: rows of identical lights, evenly spaced.

    This is what recovers the third Arena correction, and it needs neither the
    ROM nor the playfield. The tempting shortcut — "a lamp the ROM never lights
    in attract is not an insert" — is WRONG and measured so: Arena's lamp 3 is
    SHOOT AGAIN, a real insert on the Premier chart, and it is lit 0 frames out
    of 2400. That rule would condemn three genuine inserts.
    """
    by_look = defaultdict(list)
    for r in members:
        by_look[(r["radius"], r["image"], r["colour"], r["surface"])].append(r)

    bench = set()
    rows = []
    for look, group in by_look.items():
        if len(group) < 4:
            continue
        # rows: same y (within a tolerance), regular spacing in x
        for _, band in group_by_band(group).items():
            if len(band) < 4:
                continue
            xs = sorted(r["x"] for r in band)
            gaps = [b - a for a, b in zip(xs, xs[1:])]
            mean = sum(gaps) / len(gaps)
            if mean <= 0:
                continue
            cv = (sum((g - mean) ** 2 for g in gaps) / len(gaps)) ** .5 / mean
            if cv < 0.15:                      # regularly spaced = a bank, not artwork
                names = [r["name"] for r in band]
                bench.update(names)
                rows.append({"n": len(band), "pitch": round(mean, 1),
                             "cv": round(cv, 3), "names": names})
    return bench, rows


def group_by_band(group, tol=12.0):
    bands = defaultdict(list)
    for r in sorted(group, key=lambda r: r["y"]):
        for key in bands:
            if abs(r["y"] - key) <= tol:
                bands[key].append(r)
                break
        else:
            bands[r["y"]].append(r)
    return bands


def driven_lamps(path):
    blob = open(path, "rb").read()
    step, n = struct.unpack_from("<HH", blob, 0)
    union = 0
    for m in struct.unpack_from("<%dQ" % n, blob, 4):
        union |= m
    return {b for b in range(64) if (union >> b) & 1}, n, step


# --- the check sheet --------------------------------------------------------
def playfield_png(ole, want):
    """The table's own artwork, so the plan can be checked against the real
    playfield without owning it. Stored in an Image stream under a record still
    called JPEG for historical reasons - Arena's is a 13.9 MB PNG."""
    for e in ole.listdir():
        if len(e) != 2 or not e[1].startswith("Image"):
            continue
        d = ole.openstream("/".join(e)).read()
        nm = ""
        for tag, pl in biff(d):
            if tag == b"NAME" and not nm:
                nm = wstr(pl)
        if nm != want:
            continue
        for sig in (b"\x89PNG\r\n\x1a\n", b"\xff\xd8\xff"):
            at = d.find(sig)
            if at >= 0:
                return d[at:]
    return None


def make_sheet(ole, image_name, inserts, path, rom=None):
    try:
        from PIL import Image, ImageDraw
        import io
    except ImportError:
        return "pillow not installed - no sheet"
    blob = playfield_png(ole, image_name)
    if not blob:
        return "playfield image '%s' not found in the table" % image_name
    im = Image.open(io.BytesIO(blob)).convert("RGB")
    scale = 1100.0 / im.width
    im = im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS)
    d = ImageDraw.Draw(im, "RGBA")
    driven = set(rom.get("driven", [])) if rom else None

    for r in inserts:
        x, y = r["x"] * im.width, r["y"] * im.height
        lamp = r["l"]
        # Colour says what to look at: grey = flagged as bank, amber = a lamp the
        # ROM never drives in this capture, green = seen driven.
        if lamp < 0:                       col = (150, 150, 150, 210)
        elif driven and lamp not in driven: col = (255, 170, 40, 230)
        else:                               col = (60, 220, 120, 230)
        rr = 15
        d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=(0, 0, 0, 170), outline=col, width=3)
        txt = str(lamp) if lamp >= 0 else "-"
        d.text((x - 4 * len(txt), y - 6), txt, fill=col)
    im.save(path, quality=88)
    return None


# --- main -------------------------------------------------------------------
def main(argv):
    args = list(argv)
    def opt(flag, default=None):
        if flag in args:
            i = args.index(flag); v = args[i + 1]; del args[i:i + 2]; return v
        return default
    out     = opt("-o", "arena_pf.json")
    report  = opt("--report", "report.json")
    attract = opt("--attract")
    sheet   = opt("--sheet")
    if not args:
        sys.exit(__doc__)

    t = read_table(args[0])
    problems, notes = [], []

    # A — dialect. Refuse rather than guess.
    if not t["script"]:
        problems.append("no script in the file (VP 10.8 can keep it in an external .vbs)")
    if not t["collection"]:
        problems.append("no vpmMapLights call found - this table binds its lamps another way")
    for pat, why in (("Lampz.MassAssign", "Lampz"), ("LampCallback", "LampCallback")):
        if pat in t["script"]:
            notes.append("script also uses %s - check it does not rebind lamps" % why)

    members = []
    if t["collection"]:
        names = t["collections"].get(t["collection"], [])
        if not names:
            problems.append("collection '%s' is empty or missing" % t["collection"])
        members = [t["lights"][n] for n in names if n in t["lights"]]

    bench, rows = detect_bench(members)

    # C — lamp numbers must be in range once the bank is removed
    inserts, disagree = [], []
    W = t["bounds"].get("RGHT", 1000.0) or 1000.0
    H = t["bounds"].get("BOTM", 2000.0) or 2000.0
    for r in members:
        lamp = -1 if r["name"] in bench else (r["lamp"] if r["lamp"] is not None else -1)
        if lamp is not None and lamp > 63:
            notes.append("%s: lamp %d out of range, treated as not driven" % (r["name"], lamp))
            lamp = -1
        # E — the two sources disagreeing is the most discriminating flag there is
        m = re.match(r"^L(?:ight)?\s*(\d+)", r["name"])
        if m and lamp >= 0 and int(m.group(1)) != lamp:
            disagree.append({"name": r["name"], "from_name": int(m.group(1)), "from_binding": lamp})
        inserts.append({"n": r["name"], "l": lamp, "k": "g" if r["name"] in bench else "i",
                        "x": round((r["x"] or 0) / W, 4), "y": round((r["y"] or 0) / H, 4)})

    if not inserts:
        problems.append("no insert extracted - never fall back on the name, queue the title instead")

    rom = {}
    if attract:
        driven, frames, step = driven_lamps(attract)
        claimed = {i["l"] for i in inserts if i["l"] >= 0}
        rom = {"frames": frames, "step_ms": step,
               "driven": sorted(driven),
               "driven_not_claimed": sorted(driven - claimed),
               "claimed_never_driven": sorted(claimed - driven)}
        if rom["driven_not_claimed"]:
            notes.append("ROM drives lamps no insert claims: %s" % rom["driven_not_claimed"])
        if rom["claimed_never_driven"]:
            notes.append("inserts whose lamp never lights in this capture: %s "
                         "(WARNING ONLY - Arena's lamp 3 is a real insert and is never lit)"
                         % rom["claimed_never_driven"])

    light = "ROUGE" if problems else ("ORANGE" if (disagree or rows or notes) else "VERT")
    rep = {"table": args[0], "light": light, "collection": t["collection"],
           "playfield_image": t["image"], "lights_in_file": len(t["lights"]),
           "members": len(members), "inserts": len(inserts),
           "bench_rows": rows, "bench_count": len(bench),
           "name_vs_binding_disagreements": disagree,
           "rom": rom, "problems": problems, "notes": notes}

    json.dump({"inserts": inserts}, open(out, "w"), separators=(",", ":"))
    json.dump(rep, open(report, "w"), indent=1)

    print("%-8s %s" % (light, args[0]))
    print("  collection      : %s" % (t["collection"] or "(none)"))
    print("  lights / members: %d / %d" % (len(t["lights"]), len(members)))
    print("  inserts written : %d  (%d flagged as bank)" % (len(inserts), len(bench)))
    for r in rows:
        print("     bank row n=%d pitch=%.1f cv=%.3f  %s" % (r["n"], r["pitch"], r["cv"],
                                                             " ".join(r["names"][:8])))
    for d in disagree:
        print("     name says %s, binding says %d  -> %s"
              % (d["from_name"], d["from_binding"], d["name"]))
    for p in problems:
        print("     PROBLEM: %s" % p)
    for n in notes:
        print("     note: %s" % n)
    if sheet:
        err = make_sheet(olefile.OleFileIO(args[0]), t["image"], inserts, sheet, rom)
        print("  sheet           : %s" % (err or sheet))
    print("  -> %s   report: %s" % (out, report))
    return 0 if light != "ROUGE" else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
