#!/usr/bin/env bash
# Trouve tous les murs Playfield du reseau local et les nomme.
#
# Quatre murs, ce sont quatre adresses IP interchangeables tant qu'on ne les
# interroge pas. Ce script balaie le sous-reseau et rend, pour chacun, son nom,
# son adresse, son mode et son empreinte de build - de quoi savoir lequel mettre
# a jour sans se tromper de mur.
#
#   tools/find_walls.sh              # devine le sous-reseau
#   tools/find_walls.sh 192.168.1    # ou on l'impose
set -u
NET="${1:-$(route -n get default 2>/dev/null | awk '/interface:/{print $2}' \
      | xargs -I{} ipconfig getifaddr {} 2>/dev/null | cut -d. -f1-3)}"
[ -n "$NET" ] || { echo "sous-reseau introuvable, passe-le en argument" >&2; exit 1; }

echo "balayage de $NET.0/24 ..."
TMP=$(mktemp -d)
for i in $(seq 1 254); do
  ( curl -s -m 1 "http://$NET.$i/api/state" -o "$TMP/$i.json" 2>/dev/null ) &
done
wait

FOUND=0
printf "\n%-22s %-16s %-8s %-10s %s\n" NOM ADRESSE MODE BUILD MAC
printf -- "------------------------------------------------------------------------\n"
for f in "$TMP"/*.json; do
  [ -s "$f" ] || continue
  i=$(basename "$f" .json)
  python3 - "$f" "$NET.$i" <<'PY' 2>/dev/null && FOUND=$((FOUND+1))
import json, sys
d = json.load(open(sys.argv[1]))
if "mode" not in d or "build" not in d: raise SystemExit(1)
print("%-22s %-16s %-8s %-10s %s" % (d.get("name","(sans nom)"), sys.argv[2],
      d.get("mode","?"), d.get("build","")[:8], d.get("mac","?")))
PY
done
rm -rf "$TMP"
echo
[ "$FOUND" -gt 0 ] || echo "aucun mur trouve sur $NET.0/24"
echo "renommer :  curl \"http://<adresse>/api/name?v=Volcano\""
