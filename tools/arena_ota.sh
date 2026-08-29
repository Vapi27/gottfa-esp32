#!/usr/bin/env bash
# Update a wall over WiFi, and PROVE the new image is the one running.
#
#   ./tools/arena_ota.sh <ip> <arena_matter.bin>       # firmware  (pull, default)
#   ./tools/arena_ota.sh <ip> <littlefs.bin> fs        # web UI    (pull)
#
# HOW IT WORKS, AND WHY IT IS BUILT THIS WAY
#
# Pushing a 1.6 MB POST at /update kills the ESP32-S3. Update.begin() erases the
# OTA partition in one blocking call, which freezes the AsyncTCP task for
# seconds; the client keeps sending, lwIP runs out of buffers, the chip dies.
# Measured 2026-08-02: 250 kB of 1.6 MB, then a reboot that is indistinguishable
# from success. Capping the client at 10 kB/s gets the image through in 3m40 —
# a crutch, not a fix.
#
# So the firmware pulls instead. /api/otapull hands the board a URL and it
# fetches the image itself, at its own pace, from the main task. Nothing can pile
# up in its buffers, and the erase happens while nobody is pushing. Measured on
# the same board: 1.64 MB in ~12 s, free heap flat at 101 kB, and the LEDs kept
# animating at 60 fps throughout. This script serves the file from a throwaway
# local HTTP server for exactly as long as the transfer takes.
#
# VERIFICATION. /api/state exposes "build": the first 8 bytes of the running
# image's ELF SHA256. It is the only honest witness. Do NOT verify by uptime — a
# crashed upload reboots the board just like a good one — nor by any counter that
# a reboot resets. Reading uptime as success is exactly how this bug hid for
# hours.
#
# The filesystem partition goes the same way (?target=fs): pullOta unmounts
# LittleFS, erases and writes it directly. Its witness is different though - the
# firmware is untouched, so "build" cannot change. This script hashes the page
# the board serves, before and after.
set -euo pipefail

IP="${1:-}"; IMG="${2:-}"; TARGET="${3:-app}"
PORT="${ARENA_OTA_PORT:-8099}"
RATE="${ARENA_OTA_RATE:-10k}"

die() { printf '\033[31m%s\033[0m\n' "$*" >&2; exit 1; }
ok()  { printf '\033[32m%s\033[0m\n' "$*"; }

[ -n "$IP" ] && [ -n "$IMG" ] || die "usage: $0 <ip> <image.bin> [fs]"
[ -f "$IMG" ] || die "introuvable: $IMG"

state()  { curl -s -m 6 "http://$IP/api/state" 2>/dev/null; }
buildid() { printf '%s' "$1" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("build",""))
except Exception: print("")'; }

BEFORE=$(state) || true
[ -n "$BEFORE" ] || die "$IP ne repond pas"
FP_BEFORE=$(buildid "$BEFORE")
SIZE=$(wc -c < "$IMG" | tr -d ' ')
echo "carte $IP  build actuel: ${FP_BEFORE:-<absent>}"
echo "image $(basename "$IMG")  $((SIZE/1024)) ko"

# --- transfert -------------------------------------------------------------
# A filesystem update leaves the firmware untouched, so "build" cannot witness
# it. The page itself is the witness: hash what the board serves, before and
# after. (Do not fall back to comparing sizes - two builds of a page often have
# exactly the same length.)
PAGEHASH=""
if [ "$TARGET" = "fs" ]; then
  PAGEHASH=$(curl -s -m 10 "http://$IP/" | shasum -a 256 | cut -c1-16)
  echo "page servie actuellement: $PAGEHASH"
fi

  # The board must be able to reach us: bind to the LAN address, not 127.0.0.1.
  MYIP="${ARENA_OTA_HOST_IP:-$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)}"
  [ -n "$MYIP" ] || die "IP LAN du Mac introuvable. Force-la: ARENA_OTA_HOST_IP=192.168.1.x $0 ..."

  SERVE=$(mktemp -d)
  cp "$IMG" "$SERVE/fw.bin"

  # A server left over from an earlier run keeps the port and serves ITS old
  # directory, so the board fetches /fw.bin and gets a 404 that looks like a
  # firmware bug. Refuse to start on a busy port rather than publish the wrong
  # image. (Cost me a confusing 404 on 2026-08-02.)
  if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; then
    die "le port $PORT est deja pris:
$(lsof -nP -iTCP:"$PORT" -sTCP:LISTEN | tail -n +2)
Tue ce processus, ou choisis un autre port: ARENA_OTA_PORT=8100 $0 ..."
  fi

  ( cd "$SERVE" && exec python3 -m http.server "$PORT" --bind "$MYIP" >/dev/null 2>&1 ) &
  HTTPD=$!
  # The server dies with the script whatever happens - a stray one would keep a
  # firmware image published on the LAN (and would break the next run).
  trap 'kill '"$HTTPD"' 2>/dev/null || true; rm -rf '"$SERVE" EXIT
  sleep 1

  # Prove OUR server answers with OUR file before handing the URL to the board.
  SERVED=$(curl -s -m 5 -o /dev/null -w '%{http_code} %{size_download}' \
                "http://$MYIP:$PORT/fw.bin" || true)
  case "$SERVED" in
    "200 $SIZE") : ;;
    *) die "le serveur local ne rend pas la bonne image (recu: '$SERVED', attendu '200 $SIZE').
Un autre serveur repond peut-etre sur $MYIP:$PORT." ;;
  esac

  QS="url=http://$MYIP:$PORT/fw.bin"
  [ "$TARGET" = "fs" ] && QS="$QS&target=fs"
  echo "cible: $([ "$TARGET" = fs ] && echo "interface web" || echo firmware) -> la carte telecharge depuis http://$MYIP:$PORT/fw.bin"
  RESP=$(curl -s -m 15 "http://$IP/api/otapull?$QS" || true)
  # ⚠️ Ne rien envoyer a une carte qui n'est pas STABLE.
#
# Enchainer deux envois - firmware puis systeme de fichiers - sans attendre que
# le premier ait redemarre ET repris une adresse a mis un mur hors ligne. On
# exige donc trois lectures d'etat consecutives reussies avant d'ecrire quoi que
# ce soit dans une partition.
echo "  verification que la carte est stable..."
STABLE=0
for _ in $(seq 1 30); do
  if [ -n "$(state)" ]; then
    STABLE=$((STABLE + 1))
    [ "$STABLE" -ge 3 ] && break
  else
    STABLE=0
  fi
  sleep 2
done
[ "$STABLE" -ge 3 ] || die "la carte ne repond pas de facon stable - rien n'a ete envoye.
Attends qu'elle soit revenue sur le reseau, ou coupe et rebranche son alimentation."

case "$RESP" in
    *lance*) : ;;
    "")      die "aucune reponse de /api/otapull" ;;
    *)       die "refus de /api/otapull: $RESP
Firmware trop ancien pour le mode pull ? Utilise l'envoi pousse bride:
  ARENA_OTA_RATE=10k curl --limit-rate 10k -F update=@$IMG http://$IP/update" ;;
  esac

# Le suivi ne doit JAMAIS faire tomber le script.
#
# Il lisait /api/state en supposant un objet JSON. Pendant un redemarrage la
# carte peut rendre autre chose - une reponse tronquee, un entier, du vide - et
# json.load() rendait alors un int : "AttributeError: 'int' object has no
# attribute 'get'". Le script mourait AU MILIEU de l'envoi, sans dire ou il en
# etait, et l'operateur enchainait un second envoi sur une carte en train de
# redemarrer. C'est exactement ce qui a mis un mur hors ligne le 2026-08-27.
#
# Un affichage de progression n'est pas une raison d'echouer : tout ce qui n'est
# pas un objet exploitable est ignore, et l'erreur ECHEC reste detectee.
ota_field() {                                  # $1 = champ, lit $S, "" si illisible
  printf '%s' "$S" | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    d = None
print(d.get(sys.argv[1], "") if isinstance(d, dict) else "")' "$1" 2>/dev/null || true
}

for _ in $(seq 1 60); do
  sleep 2
  S=$(state) || true
  [ -n "$S" ] || break                         # muet = redemarrage, c'est la suite
  ST=$(ota_field otast)
  TOT=$(ota_field otatot); DONE=$(ota_field otadone)
  if [ -n "$ST" ]; then
    PCT=0
    [ -n "$TOT" ] && [ "$TOT" -gt 0 ] 2>/dev/null && PCT=$(( 100 * ${DONE:-0} / TOT ))
    printf '  %-16s %d%%\n' "$ST" "$PCT"
  fi
  case "$ST" in
    echec*) die "la carte rapporte: $ST" ;;
  esac
done

# --- verdict ---------------------------------------------------------------
echo "  attente du redemarrage..."
for _ in $(seq 1 24); do
  sleep 5
  AFTER=$(state) || true
  [ -n "$AFTER" ] || continue
  if [ "$TARGET" = "fs" ]; then
    NOW=$(curl -s -m 10 "http://$IP/" | shasum -a 256 | cut -c1-16)
    [ -n "$NOW" ] || continue
    if [ "$NOW" != "$PAGEHASH" ]; then
      ok "  OK - interface remplacee"
      echo "     page avant : $PAGEHASH"
      echo "     page apres : $NOW"
      exit 0
    fi
    continue
  fi
  FP_AFTER=$(buildid "$AFTER")
  [ -n "$FP_AFTER" ] || die "la carte ne publie pas de champ \"build\" (firmware
anterieur au 2026-08-02). Impossible de prouver que la mise a jour a pris."
  if [ "$FP_AFTER" != "$FP_BEFORE" ]; then
    ok "  OK - image remplacee"
    echo "     avant : ${FP_BEFORE:-<absent>}"
    echo "     apres : $FP_AFTER"
    exit 0
  fi
done
[ "$TARGET" = "fs" ] && die "la carte ressert EXACTEMENT la meme page ($PAGEHASH).
Soit l'image est identique a celle deja en place, soit l'ecriture a echoue."
die "la carte est revenue avec le MEME build ($FP_BEFORE).
Soit l'image envoyee est deja celle qui tournait, soit l'ecriture a echoue et la
carte a redemarre sur l'ancienne partition. Ne conclus pas au succes."
