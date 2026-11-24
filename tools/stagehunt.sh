#!/usr/bin/env bash
# One online match on a chosen stage, looked at for where two PCs' games part (docs/GAME.md, "Two PCs"): two games
# through the link (tools/linktest.py), game 2's heap 2 MB higher, first to 3 rounds, the match going on after a desync
# (SC5_NET_DESYNC_CONTINUE) with every check frame's snapshot dumped in both and compared (tools/py/snapsweep.py).
# Both players mash varied moves (tools/py/mash.py), or with HUNT_SCRIPT=ringout player 1 pushes player 2 out of the
# ring (tools/py/ringout.py). Prints the stage's id, the result, each game's rounds (the match frames each round's
# fighting began and ended at, and both players' health at its end: all the same in both games), the first checks
# that differed and the words that did.
#   tools/stagehunt.sh <name> <p1> <p2> <stage place 1-27 | random> [seed 1] [lag ms 40] [frames 9000] [more linktest args]
# Git Bash, from anywhere. Logs: build/runs/<name>_1.log, _2.log; the dumps are deleted (HUNT_KEEP=1 keeps them).
set -u
NAME=$1; P1=$2; P2=$3; PLACE=$4; SEED=${5:-1}; LAG=${6:-40}; FRAMES=${7:-9000}
shift $(( $# < 7 ? $# : 7 ))
cd "$(dirname "$0")/.." || exit 1
RUNS=build/runs
mkdir -p "$RUNS"
DUMPS="$RUNS/$NAME-dumps"
rm -rf "$DUMPS"; mkdir -p "$DUMPS"
if [ "${HUNT_SCRIPT:-mash}" = ringout ]; then
  F1=$(python -I tools/py/ringout.py 1 "$FRAMES" "$SEED"); F2=$(python -I tools/py/ringout.py 2 "$FRAMES" "$SEED")
else
  F1=$(python -I tools/py/mash.py $((SEED * 2 + 1)) "$FRAMES"); F2=$(python -I tools/py/mash.py $((SEED * 2 + 2)) "$FRAMES")
fi
timeout $((FRAMES / 30 + 200)) python -I tools/linktest.py --name "$NAME" --p1 "$P1" --p2 "$P2" --rounds 5 --delay 1 \
  --lag "$LAG" --heap-shift2 2 --seconds $((FRAMES / 30 + 180)) --seed "$SEED" --fight1 "$F1" --fight2 "$F2" \
  --env "SC5_NET_DUMP_DIR=$(cygpath -w "$PWD/$DUMPS")" --env SC5_NET_DUMP_FROM=0 --env SC5_NET_DUMP_COUNT=600 \
  --env SC5_NET_DESYNC_CONTINUE=1 --env "SC5_TEST_STAGE=$PLACE" "$@" > "$RUNS/$NAME.out" 2>&1
echo "=== $NAME: $P1 vs $P2, place $PLACE, seed $SEED, lag $LAG, ${HUNT_SCRIPT:-mash}: $(grep -oE "stage [0-9A-F]+," "$RUNS/${NAME}_1.log" | head -1)"
grep -E "result|desync|error|exited|done after" "$RUNS/$NAME.out" | sed 's/^ *//' | head -5
for i in 1 2; do
  echo "  game $i rounds: $(grep -oE "sc5 net: (rollback|lockstep) from frame [0-9]+" "$RUNS/${NAME}_$i.log" |
    sed -E 's/sc5 net: rollback from frame /+/; s/sc5 net: lockstep from frame /-/' | tr '\n' ' ')"
  echo "  game $i ends: $(grep -oE "fighting over at frame [0-9]+: health [0-9.-]+ / [0-9.-]+( .nobody)?" "$RUNS/${NAME}_$i.log" |
    sed -E 's/fighting over at frame //; s/: health /:/; s/ .nobody/=OUT-OR-TIME/; s| / |/|' | tr '\n' ' ')"
  echo "  game $i: $(grep -cE "sc5 net: DESYNC" "$RUNS/${NAME}_$i.log") checks apart; first: $(grep -oE "DESYNC at frame [0-9]+" "$RUNS/${NAME}_$i.log" | head -3 | tr '\n' ' ')"
done
grep -hE "sc5 desync:" "$RUNS/${NAME}_1.log" | sed 's/^.*sc5 desync: /  desync: /' | head -${HUNT_LINES:-25}
python -I tools/py/snapsweep.py "$DUMPS" 2 2>&1 | head -${HUNT_LINES:-25}
[ -n "${HUNT_KEEP:-}" ] || rm -rf "$DUMPS"
