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
import json, struct, sys
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

    out = {"bounds": bounds, "counts": counts, "items": items}
    print(json.dumps(out, indent=1))
    print("\n--- résumé ---", file=sys.stderr)
    print("bornes du plateau : %s" % bounds, file=sys.stderr)
    print("types d'objets    : %s" % dict(sorted(counts.items())), file=sys.stderr)
    for k in WANTED.values():
        print("  %-8s : %d" % (k, sum(1 for i in items if i["kind"] == k)), file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv[1])
