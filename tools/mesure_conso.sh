#!/usr/bin/env bash
# Protocole de mesure de consommation du Wall Pinball Playfield.
#
# Pourquoi ce script existe : toutes les extrapolations de courant du dossier
# reposaient sur UN seul point de mesure, sur un mur de 42 pixels, a une seule
# luminosite. Et la base "electronique" de 1,1 W etait relevee a la prise
# secteur, donc gonflee par les pertes propres du bloc d'alimentation.
#
# Ce protocole isole chaque variable une par une :
#   - la perte du bloc, mesuree a vide
#   - l'electronique seule, LED eteintes
#   - la pente par PIXEL, a nombre de pixels variable
#   - la pente par LUMINOSITE
#   - le cout d'une couleur SATUREE (R+G+B) contre le blanc (canal W seul),
#     qui est la question a l'origine du facteur 4 du modele firmware
#
# Usage :  tools/mesure_conso.sh <ip-du-mur>
# Note les valeurs dans l'ordre ; le recapitulatif final les redemande.

set -u
IP="${1:-}"
[ -n "$IP" ] || { echo "usage: $0 <ip-du-mur>" >&2; exit 1; }
PAUSE="${PAUSE:-20}"

api()  { curl -s -m 6 "http://$IP/api/set?$1" >/dev/null 2>&1; }
etat() { curl -s -m 6 "http://$IP/api/state" | python3 -c 'import json,sys;d=json.load(sys.stdin);print("      firmware estime %.2f A | mode=%s bright=%d count=%d"%(d.get("amps",0),d["mode"],d["bright"],d["count"]))' 2>/dev/null; }

N=0
etape() {                      # $1 = description, $2... = parametres API
  N=$((N+1))
  local desc="$1"; shift
  for p in "$@"; do api "$p"; sleep 1; done
  sleep 3
  printf "\n\033[1m%2d. %s\033[0m\n" "$N" "$desc"
  etat
  printf "      >>> RELEVE LE WATTMETRE, note la valeur, "
  for i in $(seq "$PAUSE" -1 1); do printf "\r      >>> RELEVE LE WATTMETRE, note la valeur (%2ds) " "$i"; sleep 1; done
  printf "\r      >>> valeur %2d notee ?                          \n" "$N"
}

ORIG=$(curl -s -m 6 "http://$IP/api/state" | python3 -c 'import json,sys;d=json.load(sys.stdin);print("%s %d %d"%(d["mode"],d["bright"],d["count"]))')
set -- $ORIG; OMODE=$1; OBRIGHT=$2; OCOUNT=$3

cat <<EOF
=====================================================================
 PROTOCOLE DE MESURE — $IP
 Wattmetre sur la PRISE SECTEUR du bloc. $PAUSE s par point.
 Note chaque valeur en watts, dans l'ordre.
=====================================================================
EOF

printf "\n\033[1m 0. BLOC SEUL — debranche le cable USB-C du mur, laisse le bloc\033[0m\n"
printf "      branche au wattmetre. C'est la perte propre du bloc, a\n"
printf "      retrancher de TOUTES les autres mesures.\n"
printf "      >>> note la valeur, puis rebranche le mur et appuie sur Entree..."
read -r _

etape "ELECTRONIQUE SEULE — LED eteintes" "mode=off"
etape "42 pixels, blanc, luminosite  25%%" "mode=classic&count=42" "bright=64"
etape "42 pixels, blanc, luminosite  50%%" "bright=128"
etape "42 pixels, blanc, luminosite 100%%" "bright=255"
etape "20 pixels, blanc, luminosite 100%%" "count=20"
etape "10 pixels, blanc, luminosite 100%%" "count=10"
etape "42 pixels, ROUGE sature, 100%%"     "count=42" "r=255&g=0&b=0&w=0"
etape "42 pixels, R+V+B satures, 100%%"    "r=255&g=255&b=255&w=0"
etape "42 pixels, R+V+B+W tout a fond"     "r=255&g=255&b=255&w=255"
etape "ATTRACT — le mode reel d'usage"     "mode=attract"

api "mode=$OMODE&bright=$OBRIGHT&count=$OCOUNT"
curl -s -m 6 "http://$IP/api/save" >/dev/null 2>&1

cat <<EOF

=====================================================================
 TERMINE — le mur est remis dans son etat d'origine.

 Redonne-moi les 11 valeurs dans l'ordre, separees par des espaces :
   0=bloc seul  1=electronique  2..4=luminosite  5..6=nb de pixels
   7=rouge  8=RVB  9=RVBW  10=attract
=====================================================================
EOF
