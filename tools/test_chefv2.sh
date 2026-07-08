#!/bin/bash
# test_chefv2.sh — regression du CHEF v2 sur les 7 scenarios Arena (chaque plainte machine reelle).
# Build chefsim + sig3 puis verifie les invariants du journal d'actions. Usage: tools/test_chefv2.sh
set -e
cd "$(dirname "$0")/.."
g++ -std=c++17 -Isrc -c src/fake6502.c -o /tmp/f2.o 2>/dev/null || gcc -c -O2 -Isrc src/fake6502.c -o /tmp/f2.o
gcc -c -O2 -Isrc src/emu2149.c -o /tmp/e2.o
g++ -std=c++17 -Isrc tools/host_chefsim.cpp src/chefv2.cpp src/psorom.cpp src/sp0250.cpp src/ym2151w.cpp src/ymfm_opm.cpp /tmp/f2.o /tmp/e2.o -o /tmp/chefsim
Y=/tmp/arena_y.snd; D=~/gosowav_sd/games/arena/drom1.snd; SIG=~/gosowav_build/arena/sounds.sig
run(){ /tmp/chefsim 1 $Y $D $SIG "$1" $2; }
ok=0; ko=0
chk(){ # chk <nom> <journal> <motif attendu present> [motif interdit]
  if echo "$2" | /usr/bin/grep -qE "$3" && { [ -z "$4" ] || ! echo "$2" | /usr/bin/grep -qE "$4"; }
  then ok=$((ok+1)); echo "OK  $1"
  else ko=$((ko+1)); echo "KO  $1"; echo "$2" | sed 's/^/    /'; fi }

J=$(run "100:6 4000:18 9000:31" 14)
chk "musique survit a l'effet"        "$J" "START   18" ""
chk "game-over coupe la musique <500ms" "$J" " 9[0-4][0-9][0-9] ms  STOP    6"
J=$(run "100:6 3000:21 3200:21 3400:21 3600:21" 8)
chk "retrigger roulette = RESTART x3" "$J" "RESTART 21" "STOP    6"
J=$(run "100:6 3000:5 3400:5 3800:5" 6)
chk "bip mur coexiste (canal prive)"  "$J" "START   5" "STOP    6"
J=$(run "100:6 5000:8" 9)
chk "changement musique = stop+start" "$J" " 5[0-9][0-9][0-9] ms  STOP    6"
J=$(run "100:26 5000:18" 16)
chk "roar survit a l effet (oracle ROM) et finit a 13,3s" "$J" " 1[34][0-9][0-9][0-9] ms  STOP    26" " [0-9][0-9][0-9][0-9] ms  STOP    26"
J=$(run "100:30 5000:18 12000:31" 16)
chk "musique DAC suspendue (pas de stop au vol)" "$J" "STOP    18" " [5-9][0-9][0-9][0-9] ms  STOP    30"
chk "musique DAC stoppee au game-over" "$J" " 12[0-4][0-9][0-9] ms  STOP    30"
J=$(run "100:26 380:26 660:26 940:26 1040:14 16000:31" 20)
chk "show de fin : le roar survit ~6,5s sous la musique (ttl mesure)" "$J" " [78][0-9][0-9][0-9] ms  STOP    26" "  [0-9][0-9][0-9] ms  STOP    26"
echo "---- $ok OK, $ko KO ----"; [ $ko -eq 0 ]
