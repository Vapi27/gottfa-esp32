#!/usr/bin/env python3
"""Compare le brochage COMPILE avec le netlist de la carte.

hardware/NETLIST.md decrit le cuivre. include/arena_config.h decrit ce que le
firmware pilote. Quand les deux divergent, le firmware s'adresse a des broches
qui ne portent pas ce qu'il croit - et cela ne se voit qu'au banc, sous la forme
d'un symptome trompeur : le 2026-08-25, trois poussoirs parfaitement cables ont
ete "corriges" dans le firmware alors que le vrai defaut etait ailleurs, parce
que personne n'a compare les deux sources. Ce script fait cette comparaison.

    python3 tools/check_pinout.py          -> 0 si tout concorde, 1 sinon
"""
import re, subprocess, sys, tempfile, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Signal du netlist -> macro du firmware. Un signal absent d'ici n'est pas
# verifie : ajouter la ligne plutot que de supposer.
PAIRS = [
    ("BTN_LEFT",  "PIN_ARENA_BTN_UP"),
    ("BTN_RIGHT", "PIN_ARENA_BTN_DOWN"),
    ("BTN_OK",    "PIN_ARENA_BTN_OK"),
    ("LED_DATA",  "PIN_LED_DATA"),
    ("LED_FAULT", "PIN_ARENA_LED_FAULT"),
    ("STATUS_PX", "ARENA_STATUS_PIN"),
    ("I2C_SDA",   "PIN_ARENA_OLED_SDA"),
    ("I2C_SCL",   "PIN_ARENA_OLED_SCL"),
    ("ADC_IN",    "PIN_ARENA_MIC"),
]

def netlist_pins():
    """Les IOxx declares dans la table des liaisons de NETLIST.md."""
    out, txt = {}, open(os.path.join(ROOT, "hardware/NETLIST.md"), encoding="utf-8").read()
    for line in txt.splitlines():
        if not line.lstrip().startswith("|"):
            continue
        m = re.search(r"\*\*([A-Z0-9_]+)\*\*.*?\(\*\*IO(\d+)\*\*\)", line)
        if m:
            out.setdefault(m.group(1), int(m.group(2)))
    return out

def compiled_pins():
    """Les valeurs que le preprocesseur retient pour la cible S3."""
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "Arduino.h"), "w").close()
        src = os.path.join(d, "probe.c")
        open(src, "w").write('#include "arena_config.h"\n')
        r = subprocess.run(["gcc", "-E", "-dM", "-I", os.path.join(ROOT, "include"),
                            "-I", d, src], capture_output=True, text=True)
    raw = {}
    for line in r.stdout.splitlines():
        p = line.split()
        if len(p) >= 3 and p[0] == "#define":
            raw[p[1]] = " ".join(p[2:])
    def resolve(v, depth=0):
        while v in raw and depth < 8:
            v, depth = raw[v], depth + 1
        return v
    return {k: resolve(v) for k, v in raw.items()}

def main():
    net, code, bad = netlist_pins(), compiled_pins(), 0
    print(f"{'SIGNAL':<12}{'NETLIST':<10}{'FIRMWARE':<10}")
    for sig, macro in PAIRS:
        want = net.get(sig)
        got  = code.get(macro)
        if want is None:
            print(f"{sig:<12}{'absent':<10}{'':<10}  <-- pas dans NETLIST.md")
            bad += 1
            continue
        ok = (str(want) == str(got))
        bad += 0 if ok else 1
        print(f"{sig:<12}{want:<10}{str(got):<10}{'' if ok else '  <-- ECART: ' + macro}")
    print()
    if bad:
        print(f"{bad} ecart(s). Le cuivre fait foi : corriger arena_config.h, pas le netlist.")
        return 1
    print("Le firmware pilote exactement les broches que porte la carte.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
