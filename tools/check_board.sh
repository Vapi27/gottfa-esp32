#!/bin/sh
# check_board.sh — controle de sante GOSOWAV a distance (carte allumee + WiFi).
# Usage: tools/check_board.sh [ip]    (defaut 192.168.1.15, ou gosowav.local)
IP=${1:-192.168.1.15}
S=$(curl -s -m 4 "http://$IP/status") || { echo "KO: carte injoignable ($IP)"; exit 1; }
echo "status: $S"
echo "(mesure des underruns sur 5 s...)"
sleep 5
S2=$(curl -s -m 4 "http://$IP/status") || S2="$S"
python3 - "$S" "$S2" << 'EOF'
import json, sys
d = json.loads(sys.argv[1]); d2 = json.loads(sys.argv[2]); ok = True
def chk(c, msg):
    global ok
    print(("  OK  " if c else "  KO  ") + msg)
    if not c: ok = False
dur = d2.get("ur", 0) - d.get("ur", 0)
chk(d.get("clk") == 240,                 f"horloge CPU 240 MHz (lu: {d.get('clk')})")
chk(d.get("thr", 0) >= 1.0,              f"debit emulateur >= 1.0 M (lu: {d.get('thr')})")
chk(d.get("cmcps", 0) >= 0.8,            f"decodeur ROM-chef vivant >= 0.8 M cyc/s (lu: {d.get('cmcps')}) — si 0: chefTask bloquee")
chk(dur < 100,                           f"underruns VIVANTS ~0 (delta 5 s: {dur} ; le total {d.get('ur')} inclut le boot/bench, normal)")
chk("OK" in d.get("st", ""),             f"jeu charge (lu: {d.get('st')})")
print("\n=> " + ("CARTE SAINE — teste a l'oreille: stop des boucles, pas d'empilement de musiques" if ok else "VOIR LES KO CI-DESSUS"))
EOF
