#!/usr/bin/env python
"""Pull the insert layout out of a Visual Pinball .vpx table -> the board's pf.json.

    pip install olefile
    python tools/vpx_inserts.py <table.vpx> [--config tools/games/<game>.json] > data/arena_pf.json

Works for ANY VP table, not just Arena: the extraction (lights, positions,
bounds) is game-agnostic. What is per-game lives in an optional config file:

    author_fixes   VP-table author errors, caught at the real playfield:
                   {"L1": ["L9", 9]} renames the object and sets its real lamp.
    func           the machine's own lamp chart, from its service manual:
                   {"9": "#1 TOP ROLLOVER", ...} — shown on the plan tooltips.
    func_by_name   overrides for cross-wired lamps (one lamp, two inserts).

Without a config the plan still works — labels come from the VP object names,
functions are just absent until someone reads that game's manual.

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
try:
    import olefile
except ImportError:
    sys.exit("il manque olefile :  pip3 install olefile")

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


def names(path):
    """Dire ce que la table contient VRAIMENT, avant d'en extraire quoi que ce soit.

    La traduction nom -> type d'insert est une convention par constructeur, et un
    motif qui ne correspond a rien ne rend pas d'erreur : il rend un plateau a
    zero insert, ou un plateau plausible ou les flashers passent pour des lampes.
    La panne se decouvre alors devant le mur, apres le televersement.

    Demander a quelqu'un d'ouvrir Visual Pinball pour verifier, c'est lui faire
    faire a la main ce que le fichier sait deja dire.
    """
    ole = olefile.OleFileIO(path)
    found = []
    for entry in ole.listdir():
        if len(entry) != 2 or entry[0] != "GameStg" or not entry[1].startswith("GameItem"):
            continue
        data = ole.openstream("/".join(entry)).read()
        if len(data) < 4:
            continue
        (itype,) = struct.unpack_from("<I", data, 0)
        if itype != ITEM_LIGHT:
            continue
        for tag, payload in biff_records(data, 4):
            if tag == b"NAME":
                found.append(payload.decode("utf-16-le", "ignore").rstrip("\x00"))
                break
    found.sort()
    print("%d lumieres dans la table :" % len(found))
    for n in found:
        print("   ", n)
    print()
    print("Regarder le PREFIXE. Le defaut de l'outil est la convention Gottlieb :")
    print('   "kinds":  [["^L\\\\d", "i"], ["^F", "f"]]     "ignore": ["^GI", "^LS"]')
    print("Si les noms ci-dessus ne commencent pas par L<chiffre>, corriger ces")
    print("deux champs dans tools/games/<jeu>.json AVANT de lancer l'extraction.")


def main(path, cfg):
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
    # Comment un NOM d'objet VP se traduit en type d'insert. C'etait ecrit en
    # dur, et c'etait la convention Gottlieb : "GI"/"LS" ignores, "L<n>" insert
    # de matrice, "F" flasher. Sur une table Williams - Alien Poker, System 7 -
    # les auteurs ne nomment pas pareil, et un motif faux ne rend pas une erreur :
    # il rend un plateau a zero insert, ou pire un plateau plausible ou les
    # flashers sont comptes comme des lampes. Le reste de l'outil se disait
    # "game-agnostic" pendant que cette fonction ne l'etait pas.
    #
    # Les motifs vivent donc dans le fichier de jeu, et le defaut reste Gottlieb.
    KINDS  = cfg.get("kinds",  [["^L\\d", "i"], ["^F", "f"]])
    IGNORE = cfg.get("ignore", ["^GI", "^LS"])
    # Le numero de lampe se lisait par un "^L(\\d+)" ecrit en dur - la meme
    # convention Gottlieb que le reste, au meme endroit et avec le meme defaut.
    LAMPPAT = cfg.get("lamp", r"^L(\d+)")

    # Insensible a la CASSE : les auteurs de tables ecrivent "L12" ou "l12"
    # indifferemment. Une comparaison sensible a la casse ne rend pas d'erreur,
    # elle rend un plateau faux - GI comptes comme inserts, aucun numero de
    # lampe. Constate sur Alien Poker v600, dont les lumieres sont en minuscules.
    def kind(name):
        for pat in IGNORE:
            if re.match(pat, name, re.I): return None
        for pat, k in KINDS:
            if re.match(pat, name, re.I): return k
        return None

    # The VP table's light names FOLLOW the machine's own lamp chart (checked on
    # Arena by geometry against the Premier manual). Do NOT add PinMAME's
    # GTS80_lamp2m offset: it converts to the core matrix, not to the manual —
    # that mistake relabelled correct names into wrong ones and cost a day.
    # Per-game corrections and the manual's lamp chart come from the config;
    # Arena's (tools/games/arena.json) documents both kinds of fix.
    FUNC         = {int(k): v for k, v in cfg.get("func", {}).items()}
    AUTHOR_FIXES = {k: (v[0], v[1]) for k, v in cfg.get("author_fixes", {}).items()}
    DROP = {n.lower() for n in cfg.get("drop", [])}
    FUNC_BY_NAME = cfg.get("func_by_name", {})
    OFFX = float(cfg.get("offset_x", 0.0))
    OFFY = float(cfg.get("offset_y", 0.0))

    keep = []
    for it in sorted(items, key=lambda z: (z["y"], z["x"])):
        if it["kind"] != "light":
            continue
        k = kind(it["name"])
        if not k:
            continue
        name = it["name"][:7]
        m = re.match(LAMPPAT, name, re.I)
        lamp = int(m.group(1)) if m else -1
        if name in AUTHOR_FIXES:
            name, lamp = AUTHOR_FIXES[name]
        rec = {"n": name, "l": lamp if 0 <= lamp < 64 else -1, "k": k}
        if name in FUNC_BY_NAME:
            rec["f"] = FUNC_BY_NAME[name]
        elif lamp in FUNC:
            rec["f"] = FUNC[lamp]
        # Calage image/objets. Les bornes de la table et la texture de plateau
        # ont le meme rapport, donc l'echelle est juste - mais l'origine ne
        # coincide pas forcement : l'image inclut souvent une marge que les
        # coordonnees d'objets ne comptent pas. Constate sur Alien Poker v600,
        # ou tous les inserts tombaient 5 % trop haut. Le decalage est une
        # donnee de jeu, pas une constante du code.
        # Doublons d'auteur de table. Un auteur VPX pose souvent DEUX sources
        # lumineuses sur une meme lucarne - une pour l'insert, une pour le halo -
        # avec le meme numero de lampe et quelques pour cent d'ecart. Sur le vrai
        # plateau il n'y en a qu'une (confirme sur la machine). Ne PAS confondre
        # avec deux lucarnes reelles partagees par une meme lampe : sur Alien
        # Poker les lampes 18 et 37 en ont deux, a 0,57 et 0,59 l'une de l'autre,
        # soit plus d'un demi-plateau, et chacune porte sa propre LED. L'ecart
        # tranche sans ambiguite (0,04 contre 0,58) mais la liste est ecrite a la
        # main plutot que devinee par un seuil : un seuil se trompe en silence.
        if name.lower() in DROP:
            continue
        rec["x"] = round(it["x"] / bounds["RGHT"] + OFFX, 4)
        rec["y"] = round(it["y"] / bounds["BOTM"] + OFFY, 4)
        keep.append(rec)
    rows = ",\n".join(json.dumps(r, separators=(",", ":")) for r in keep)
    print("{\"inserts\":[\n" + rows + "\n]}")
    print("\n--- résumé ---", file=sys.stderr)
    print("bornes du plateau : %s" % bounds, file=sys.stderr)
    print("types d'objets    : %s" % dict(sorted(counts.items())), file=sys.stderr)
    for k in WANTED.values():
        print("  %-8s : %d" % (k, sum(1 for i in items if i["kind"] == k)), file=sys.stderr)


if __name__ == "__main__":
    args = sys.argv[1:]
    cfg = {}
    if "--config" in args:
        i = args.index("--config")
        cfg = json.load(open(args[i + 1]))
        del args[i:i + 2]
    want_names = "--names" in args
    if want_names:
        args.remove("--names")
    if not args:
        sys.exit("usage: vpx_inserts.py <table.vpx> [--names] "
                 "[--config tools/games/<game>.json]")
    if want_names:
        names(args[0])
    else:
        main(args[0], cfg)
