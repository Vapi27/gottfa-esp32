#!/usr/bin/env bash
# Mesure de consommation du Wall Pinball Playfield — version courte.
#
# Cinq points, deux pentes :
#   - la LUMINOSITE a nombre de pixels fixe (25 / 50 / 100 %)
#   - le NOMBRE DE PIXELS a luminosite fixe (10 / 20 / 40)
# Le point "40 pixels a 100 %" sert aux deux, d'ou cinq mesures et non six.
#
# C'est ce qu'il faut pour extrapoler honnetement : jusqu'ici tout le
# dimensionnement reposait sur UN seul point, sur un mur de 42 pixels, a une
# seule luminosite.
#
# Usage :  tools/mesure_conso.sh <ip-du-mur>

set -u
IP="${1:-}"
[ -n "$IP" ] || { echo "usage: $0 <ip-du-mur>" >&2; exit 1; }
PAUSE="${PAUSE:-20}"

api() { curl -s -m 6 "http://$IP/api/set?$1" >/dev/null 2>&1; }

N=0
etape() {
  N=$((N+1))
  local desc="$1"; shift
  for p in "$@"; do api "$p"; sleep 1; done
  sleep 3
  printf "\n\033[1m  %d. %s\033[0m\n" "$N" "$desc"
  curl -s -m 6 "http://$IP/api/state" | python3 -c 'import json,sys;d=json.load(sys.stdin);print("      etat reel : %d pixels, luminosite %d/255, mode %s  (firmware estime %.2f A)"%(d["count"],d["bright"],d["mode"],d.get("amps",0)))' 2>/dev/null
  for i in $(seq "$PAUSE" -1 1); do printf "\r      >>> RELEVE LE WATTMETRE   (%2ds) " "$i"; sleep 1; done
  printf "\r      >>> valeur %d notee                \n" "$N"
}

ORIG=$(curl -s -m 6 "http://$IP/api/state" | python3 -c 'import json,sys;d=json.load(sys.stdin);print("%s %d %d"%(d["mode"],d["bright"],d["count"]))')
set -- $ORIG; OMODE=$1; OBRIGHT=$2; OCOUNT=$3

cat <<EOF
=====================================================================
 MESURE — $IP
 Wattmetre sur la prise secteur. $PAUSE s par point, 5 points.
 Mode "classic" (toutes les LED allumees) pour que ce soit stable.
=====================================================================
EOF

etape "40 pixels — luminosite  25 %"  "mode=classic&count=40" "bright=64"
etape "40 pixels — luminosite  50 %"  "bright=128"
etape "40 pixels — luminosite 100 %"  "bright=255"
etape "20 pixels — luminosite 100 %"  "count=20"
etape "10 pixels — luminosite 100 %"  "count=10"

api "mode=$OMODE&bright=$OBRIGHT&count=$OCOUNT"
curl -s -m 6 "http://$IP/api/save" >/dev/null 2>&1

cat <<EOF

=====================================================================
 TERMINE — mur remis dans son etat d'origine ($OCOUNT px, $OMODE).

 Redonne-moi les 5 valeurs en watts, dans l'ordre :
   40px@25%   40px@50%   40px@100%   20px@100%   10px@100%
=====================================================================
EOF
