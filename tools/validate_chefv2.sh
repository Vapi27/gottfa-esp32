#!/bin/bash
# validate_chefv2.sh — VALIDATION CROISEE du chef v2 contre PinMAME (Phase 3 de l'outil ultime).
# Niveau 1 : tools/test_chefv2.sh (regles seules, scenarios Arena, rapide).
# Niveau 2 (ce script) : sequences completes -> enveloppe du mix WAV simule vs audio PinMAME (rig).
# Couvre les 3 generations : Gen1 Arena (AY+SP+DAC), Gen2 Diamond (AY 2MHz), Gen3 Hot Shots
# (YM2151 + parole DAC + banques). References : ~/gosowav_build/seq2/*_ref.wav (produites par
# PSOWAV_SEQ2 sur le rig — voir l'historique ; rejouer si les ROMs changent).
# Seuil : >= 95% d'accord, 0 fenetre sim-muet hors artefacts de contenu (voir hs1, valide a la main).
set -e
cd "$(dirname "$0")/.."
tools/test_chefv2.sh > /tmp/chefv2_l1.log 2>&1 && echo "Niveau 1 : 8/8 OK" || { tail -5 /tmp/chefv2_l1.log; exit 1; }
R=~/gosowav_build/seq2
run(){ printf "%-4s " "$1"; python3 tools/seqcheck.py "$2" "$1" "$3" "$4" "$R/$1_ref.wav" $5 2>&1 | /usr/bin/grep -E "accord"; }
run sc1 arena    "3000:6 7000:18 12000:31" 17
run sc6 arena    "3000:30 8000:18 15000:31" 19
run sc7 arena    "3000:26 7000:6 11000:18 12000:21 12500:21 15000:5 17000:31" 21
run dl1 diamond  "3000:8 9000:9 14000:31" 18 ~/gosowav_build/diamond_rig
run hs1 hotshots "3000:6 9000:29.4 15000:31" 19 ~/gosowav_build/hotshots_rig
run hs2 hotshots "3000:30.19 10000:7 15000:31" 19 ~/gosowav_build/hotshots_rig
