#!/usr/bin/env bash
# the 14 character pairs of tools/heapsweep.sh, each a first to 3 with both players mashing, as two players' games run
# (HUNT_PLAY), 60 ms each way with jitter and loss: tools/pairsweep.sh <out file> <tag> <port base> <first pair 0> <last pair 13>
OUT=$1; TAG=$2; PORT=$3; FIRST=$4; LAST=$5
cd "$(dirname "$0")/.." || exit 1
PAIRS=("Ezio Viola" "ZWEI Algol" "Cervantes Yoshimitsu" "Hilde Pyrrha" "Patroklos Natsu" "Xiba Leixia"
       "AeonCalcos Siegfried" "Nightmare Ivy" "Tira Mitsurugi" "Astaroth Voldo" "Raphael Maxi"
       "AlphaPatroklos PyrrhaOmega" "Elysium EdgeMaster" "Kilik Ezio")
EXE="$(cygpath -w "$PWD/build/runs/huntbin/sc5.exe")"
for i in $(seq "$FIRST" "$LAST"); do
  read -r p1 p2 <<< "${PAIRS[$i]}"
  HUNT_PLAY=1 bash tools/stagehunt.sh "$TAG$i" "$p1" "$p2" random $((100 + i)) 60 9000 --port-base "$PORT" --exe "$EXE" \
    --env SC5_NET_JITTER=25 --env SC5_NET_LOSS=3 > "build/runs/$TAG$i.sum" 2>&1
  echo "$(grep -E '^===' "build/runs/$TAG$i.sum" | cut -c1-90) | $(grep -oE 'result winner=[0-9] p1_rounds=[0-9] p2_rounds=[0-9]' "build/runs/$TAG$i.sum" | sort -u | tr '\n' ';') | $(grep -E 'game 1: [0-9]+ checks apart' "build/runs/$TAG$i.sum" | sed 's/^ *game 1: //' | cut -c1-120)" >> "$OUT"
  grep -E 'desync: (frame|P|8)|error|exited' "build/runs/$TAG$i.sum" | head -6 | cut -c1-200 >> "$OUT"
  rm -f build/runs/$TAG${i}_*net-*.log build/runs/$TAG${i}_*stats-*.json build/runs/$TAG$i.out
done
echo "MASH $TAG DONE" >> "$OUT"
