#!/bin/bash
# make_sd.sh — prépare la carte SD GOSOWAV multi-jeux en une commande.
# Copie games.idx + games/<jeu>/*.snd (16 jeux 80B Gen1/2/3) à la racine de la carte, vérifie, éjecte.
# Le firmware lit games.idx et propose le menu de jeux sur la page web.
#
# Usage:
#   ./make_sd.sh                 # auto-détecte la carte SD (disque externe), copie tout
#   ./make_sd.sh NOM_DU_VOLUME   # cible /Volumes/NOM_DU_VOLUME explicitement
#   ./make_sd.sh --format        # FORMATE la carte détectée en FAT32 (nom GOSOWAV) PUIS copie
#
# Sécurité : le formatage n'agit QUE sur un disque EXTERNE physique. Jamais le disque système.
set -uo pipefail
SRC="$HOME/gosowav_sd"

DO_FORMAT=0; ARGVOL=""
for a in "$@"; do
  if [ "$a" = "--format" ]; then DO_FORMAT=1; else ARGVOL="$a"; fi
done

# --- vérifie les sources ---
[ -f "$SRC/games.idx" ] || { echo "❌ $SRC/games.idx manquant"; exit 1; }
[ -d "$SRC/games" ]     || { echo "❌ $SRC/games/ manquant"; exit 1; }
NSRC=$(find "$SRC/games" -name '*.snd' | wc -l | tr -d ' ')

# --- liste les volumes candidats (externes, writable, hors système/installeurs) ---
candidates=()
while IFS= read -r vol; do
  [ -z "$vol" ] && continue
  case "$vol" in
    "Macintosh HD"|"Macintosh HD - Data"|"Fedora"*|"com.apple"*) continue;;
  esac
  [ -w "/Volumes/$vol" ] && candidates+=("$vol")
done < <(ls -1 /Volumes 2>/dev/null)

# --- choisit le volume cible ---
if [ -n "$ARGVOL" ]; then VOL="$ARGVOL"
elif [ "${#candidates[@]}" -eq 1 ]; then VOL="${candidates[0]}"
elif [ "${#candidates[@]}" -eq 0 ]; then
  echo "❌ Aucune carte SD détectée. Branche-la (lecteur USB) puis relance."
  echo "   Volumes vus: $(ls -1 /Volumes | tr '\n' ' ')"; exit 1
else
  echo "⚠️  Plusieurs cartes possibles — relance avec le nom exact :"
  for v in "${candidates[@]}"; do echo "   ./make_sd.sh \"$v\""; done; exit 1
fi

MP="/Volumes/$VOL"
[ -d "$MP" ] || { echo "❌ $MP introuvable (carte montée ?)"; exit 1; }
NODE=$(diskutil info "$MP" 2>/dev/null | awk -F': +' '/Device Node/{print $2}' | xargs)
FS=$(diskutil info "$MP" 2>/dev/null | awk -F': +' '/File System Personality|Type \(Bundle\)/{print $2}' | head -1 | xargs)
EXT=$(diskutil info "$MP" 2>/dev/null | grep -iE "Device Location|Removable Media|Internal")
echo "Carte cible : $MP   (node $NODE,  FS '$FS')"

# --- format optionnel (FAT32, externe uniquement) ---
if [ "$DO_FORMAT" -eq 1 ]; then
  echo "$EXT" | grep -qiE "External|Removable" || { echo "❌ refus de formater : $MP ne semble PAS externe/amovible. (sécurité)"; exit 1; }
  PARENT=$(echo "$NODE" | sed -E 's/s[0-9]+$//')
  echo "⚠️  FORMATAGE de $PARENT en FAT32 (nom GOSOWAV) dans 4 s — Ctrl-C pour annuler..."; sleep 4
  diskutil eraseDisk MS-DOS GOSOWAV MBRFormat "$PARENT" || { echo "❌ échec formatage"; exit 1; }
  MP="/Volumes/GOSOWAV"; VOL="GOSOWAV"; echo "✅ formaté FAT32 -> $MP"
elif ! echo "$FS" | grep -qiE "FAT|MS-?DOS"; then
  echo "⚠️  ATTENTION : FS '$FS' n'est PAS FAT32 — l'ESP32 ne montera PAS la carte."
  echo "    Relance avec :  ./make_sd.sh --format   (efface la carte et la met en FAT32)"
fi

# --- copie ---
echo "Copie de $NSRC fichiers .snd + manifeste (16 jeux 80B)..."
cp -f "$SRC/games.idx" "$MP/games.idx" || { echo "❌ copie games.idx échouée"; exit 1; }
rm -rf "$MP/games" 2>/dev/null
cp -R "$SRC/games" "$MP/games" || { echo "❌ copie games/ échouée"; exit 1; }
sync

# --- vérif ---
NDST=$(find "$MP/games" -name '*.snd' | wc -l | tr -d ' ')
echo "Vérification :"
echo "  manifeste  : $([ -f "$MP/games.idx" ] && echo OK || echo MANQUANT)  ($(wc -l <"$MP/games.idx" | tr -d ' ') jeux)"
echo "  fichiers   : $NDST / $NSRC .snd copiés"
echo "--- racine carte ---"; ls -1 "$MP"
if [ -f "$MP/games.idx" ] && [ "$NDST" = "$NSRC" ]; then
  echo "✅ Carte prête ($NSRC fichiers). Éjection..."
  diskutil eject "$MP" >/dev/null 2>&1 && echo "✅ Éjectée — mets-la dans la carte GOSOWAV."
else
  echo "❌ Vérif échouée — ne pas utiliser."
fi
