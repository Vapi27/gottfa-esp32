#!/usr/bin/env python
"""Pull the insert layout out of a Visual Pinball .vpx table -> data/arena_pf.json.

    pip install olefile
    python tools/vpx_inserts.py "Arena (Gottlieb 1987) 1.0c.vpx" > data/arena_pf.json

Why this exists: the firmware needs to know WHERE each insert is before any
effect can be about the playfield rather than about the cable. Measuring 99
positions off a real table by hand is a bad afternoon; the VP table already has
them, to the pixel, and its light names are the actual Gottlieb lamp numbers
(L26, LS10, F4a), so the virtual table and the physical machine end up speaking
the same language.

A .vpx is an OLE compound file. Storage "GameStg" holds one stream per table
object ("GameItem0", "GameItem1", ...). Each stream starts with a uint32 item
type, then a run of BIFF records: [uint32 size][4-byte tag][size-4 bytes].

We want item type 7 (Light) and its VCEN record — the light's centre on the
playfield, in VP table units — plus NAME. Table bounds come from GameData
(LEFT/RGHT/TOPX/BOTM), so positions can be normalised to 0..1.
"""
import json, re, struct, sys
import olefile

ITEM_LIGHT, ITEM_BUMPER, ITEM_FLIPPER = 7, 5, 1
WANTED = {ITEM_LIGHT: "light", ITEM_BUMPER: "bumper", ITEM_FLIPPER: "flipper"}


def biff_records(data, start=0):
    """Yield (tag, payload) for every BIFF record in the stream."""
    pos = start
    n = len(data)
    while pos + 8 <= n:
        (size,) = struct.unpack_from("<I", data, pos)
        if size < 4 or pos + 4 + size > n:
            return
        tag = data[pos + 4:pos + 8]
        yield tag, data[pos + 8:pos + 4 + size]
        pos += 4 + size
        if tag == b"ENDB":
            return


def as_text(payload):
    """VPX strings: [uint32 len][UTF-16LE] in modern tables, raw bytes in old ones."""
    if len(payload) >= 4:
        (ln,) = struct.unpack_from("<I", payload, 0)
        if 0 < ln <= len(payload) - 4:
            body = payload[4:4 + ln]
            for enc in ("utf-16-le", "latin-1"):
                try:
                    s = body.decode(enc).rstrip("\x00")
                    if s.isprintable() and s:
                        return s
                except UnicodeDecodeError:
                    pass
    return payload.split(b"\x00")[0].decode("latin-1", "replace")


def main(path):
    ole = olefile.OleFileIO(path)
    streams = ole.listdir()

    bounds = {}
    for tag, payload in biff_records(ole.openstream("GameStg/GameData").read()):
        if tag in (b"LEFT", b"RGHT", b"TOPX", b"BOTM") and len(payload) >= 4:
            bounds[tag.decode()] = struct.unpack_from("<f", payload, 0)[0]

    items, counts = [], {}
    for entry in streams:
        if len(entry) != 2 or entry[0] != "GameStg" or not entry[1].startswith("GameItem"):
            continue
        data = ole.openstream("/".join(entry)).read()
        if len(data) < 4:
            continue
        (itype,) = struct.unpack_from("<I", data, 0)
        counts[itype] = counts.get(itype, 0) + 1
        if itype not in WANTED:
            continue
        rec = {"kind": WANTED[itype], "stream": entry[1], "name": "", "x": None, "y": None}
        for tag, payload in biff_records(data, 4):
            if tag == b"VCEN" and len(payload) >= 8:
                rec["x"], rec["y"] = struct.unpack_from("<ff", payload, 0)
            elif tag == b"NAME" and not rec["name"]:
                rec["name"] = as_text(payload)
            elif tag == b"BULT" and len(payload) >= 4:
                rec["bulb"] = bool(struct.unpack_from("<I", payload, 0)[0])
        if rec["x"] is not None:
            items.append(rec)
    ole.close()

    # --- keep only what the machine actually commands -----------------------
    # Evidence, not naming convention: the table's own VBS says
    #   line 174  vpmMapLights AllLights   -> objects named L<n> ARE lamp <n>
    #   line 365  \'Ramp running Light     -> the LS* column is ONE lamp (17)
    #             that the table author animates as a chaser with his own timer
    # So the 24 LS* are a visual effect on the virtual table and do not exist as
    # lamps on the real playfield. GI is house light, never commanded. What is
    # left: L* (the lamp matrix) and F* (flashers, driven by the solenoid board
    # rather than the matrix — commanded all the same, kept and marked 'f').
    def kind(name):
        if name.startswith("GI"): return None            # general illumination
        if name.startswith("LS"): return None            # ramp chaser, not lamps
        if re.match(r"^L\d", name): return "i"           # lamp matrix insert
        if name.startswith("F"):  return "f"             # flasher
        return None

    # The VP table's light names FOLLOW the machine's own lamp chart (Premier
    # manual E-25440, printed p42: L3 Shoot Again, L5-L8 multipliers, L9-L11 top
    # rollovers, L36-L39 the W-A-L-L drop targets, L44-L48 rollovers...).
    # Verified by geometry: L36-L39 draw the drop-target diagonal mid-table,
    # L24-L26 sit on the upper deck, L45/46/47 are the P-I-T lanes. An earlier
    # revision added PinMAME's GTS80_lamp2m offset (+8) on the theory that VP
    # names were internal numbering; that relabelled correct names into wrong
    # ones and cost a day of confusion. lamp2m converts to the core matrix, not
    # to the manual.
    #
    # One AUTHOR error in the VP table, caught at the real playfield: the object
    # named "L1" sits in the top-rollover row — it IS the #1 TOP ROLLOVER, lamp
    # L9 in the manual — but its name binds it to Controller.Lamp(1), the Game
    # On RELAY. In VP nobody notices (the relay is on for the whole game); in
    # attract the insert goes dead while the real machine chases 9-10-11. Fixed
    # here: display L9, drive from lamp 9. The stray VP object also named "L9"
    # (bottom-left, not a top rollover) is renamed L9b to keep names unique.
    FUNC = {
        3: "SHOOT AGAIN", 4: "SOUND 16",
        5: "1X BONUS MULT", 6: "2X BONUS MULT", 7: "4X BONUS MULT", 8: "8X BONUS MULT",
        9: "#1 TOP ROLLOVER", 10: "#2 TOP ROLLOVER", 11: "#3 TOP ROLLOVER",
        18: "LIGHT WARRIOR PIT SPECIAL", 19: "WARRIOR PIT (RAMP)",
        20: "RAMP VALUE (UPPER)", 21: "EXTRA BALL (UPPER)", 22: "SPECIAL (UPPER)",
        23: "1,000,000 (UPPER)", 24: "CAPTURED #1", 25: "CAPTURED #2",
        26: "MULTI-BALL RELEASE", 27: "SPINNER",
        28: "CYS1 COMPLETED", 29: "CYS2 COMPLETED",
        30: "LEFT C SPOT", 31: "LEFT Y SPOT", 32: "LEFT S SPOT",
        33: "RIGHT C SPOT", 34: "RIGHT Y SPOT", 35: "RIGHT S SPOT",
        36: "W DROP TARGET", 37: "A DROP TARGET", 38: "L DROP TARGET",
        39: "L DROP TARGET", 40: "#1 GUARD SPOT", 41: "#2 GUARD SPOT",
        42: "#3 GUARD SPOT", 43: "ADVANCE RAMP VALUE",
        44: "R.OUTSIDE / L.RETURN ROLLOVER", 45: "P LEFT SIDE ROLLOVER",
        46: "I CENTER ROLLOVER", 47: "T RIGHT SIDE ROLLOVER",
        48: "L.OUTSIDE / R.RETURN ROLLOVER",
    }
    AUTHOR_FIXES = { "L1": ("L9", 9), "L9": ("L9b", -1) }

    keep = []
    for it in sorted(items, key=lambda z: (z["y"], z["x"])):
        if it["kind"] != "light":
            continue
        k = kind(it["name"])
        if not k:
            continue
        name = it["name"][:7]
        m = re.match(r"^L(\d+)", name)
        lamp = int(m.group(1)) if m else -1
        if name in AUTHOR_FIXES:
            name, lamp = AUTHOR_FIXES[name]
        rec = {"n": name, "l": lamp if 0 <= lamp < 64 else -1, "k": k}
        if lamp in FUNC:
            rec["f"] = FUNC[lamp]
        rec["x"] = round(it["x"] / bounds["RGHT"], 4)
        rec["y"] = round(it["y"] / bounds["BOTM"], 4)
        keep.append(rec)
    rows = ",\n".join(json.dumps(r, separators=(",", ":")) for r in keep)
    print("{\"inserts\":[\n" + rows + "\n]}")
    print("\n--- résumé ---", file=sys.stderr)
    print("bornes du plateau : %s" % bounds, file=sys.stderr)
    print("types d'objets    : %s" % dict(sorted(counts.items())), file=sys.stderr)
    for k in WANTED.values():
        print("  %-8s : %d" % (k, sum(1 for i in items if i["kind"] == k)), file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv[1])
