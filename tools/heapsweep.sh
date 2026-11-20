#!/usr/bin/env bash
# Two PCs on one (docs/GAME.md, "Two PCs"): every character in 14 pairs, two games through the link (tools/linktest.py)
# with game 2's heap 2 MB higher, as a second PC's lies elsewhere, both players mashing varied moves (tools/py/mash.py:
# throws, quarter circles, Critical Edge motions, guards) for 7,000 fight frames; every check frame's snapshot dumped in
# both and compared (tools/py/snapsweep.py). Three matches at a time, on a copy of the release build (a build can go on
# meanwhile). SC5_TEST_STAGE=random stands in for the player who picks the stage (else the stage select waits a minute
# for them and the match runs out of time). Prints each pair's summary: the check frames compared and every word that
# differed.
#   tools/heapsweep.sh <work dir> [first pair 0] [last pair 13] [lag ms 0]
set -u
W=$1; FIRST=${2:-0}; LAST=${3:-13}; LAG=${4:-0}
cd "$(dirname "$0")/.." || exit 1
R=$PWD
PAIRS=("Ezio Viola" "ZWEI Algol" "Cervantes Yoshimitsu" "Hilde Pyrrha" "Patroklos Natsu" "Xiba Leixia"
       "AeonCalcos Siegfried" "Nightmare Ivy" "Tira Mitsurugi" "Astaroth Voldo" "Raphael Maxi"
       "AlphaPatroklos PyrrhaOmega" "Elysium EdgeMaster" "Kilik Ezio")
mkdir -p "$W/bin"
B=sc5/out/build/win-amd64-release
cp "$B/sc5.exe" "$B"/*.dll "$W/bin/" || exit 1
EXE=$(cygpath -w "$W/bin/sc5.exe")
run_pair() {
  local i=$1 slot=$2 p1 p2
  read -r p1 p2 <<< "${PAIRS[$i]}"
  local dumps="$W/d$i"
  rm -rf "$dumps"; mkdir -p "$dumps"
  timeout 360 python -I tools/linktest.py --name "sw$i" --p1 "$p1" --p2 "$p2" --rounds 5 --delay 1 --lag "$LAG" \
    --heap-shift2 2 --seconds 340 --seed $((1000 + i)) --port-base $((7101 + slot * 100)) --exe "$EXE" \
    --runs "$(cygpath -w "$W/runs$slot")" \
    --fight1 "$(python -I tools/py/mash.py $((i * 2 + 1)) 7000)" --fight2 "$(python -I tools/py/mash.py $((i * 2 + 2)) 7000)" \
    --env "SC5_NET_DUMP_DIR=$(cygpath -w "$dumps")" --env SC5_NET_DUMP_FROM=0 --env SC5_NET_DUMP_COUNT=400 \
    --env SC5_NET_DESYNC_CONTINUE=1 --env SC5_TEST_STAGE=random > "$W/sw$i.out" 2>&1
  { echo "=== pair $i: $p1 vs $p2"; grep -E "result|desync|error|exited" "$W/sw$i.out" | head -4
    python -I tools/py/snapsweep.py "$dumps" 2; } > "$W/sw$i.sum"
  rm -rf "$dumps"
}
i=$FIRST
while [ "$i" -le "$LAST" ]; do
  for slot in 0 1 2; do
    [ "$i" -le "$LAST" ] || break
    run_pair "$i" "$slot" &
    i=$((i + 1))
    sleep 5
  done
  wait
done
cat "$W"/sw*.sum
