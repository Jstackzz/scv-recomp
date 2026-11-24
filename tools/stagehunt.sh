#!/usr/bin/env bash
# One online match on a chosen stage, looked at for where two PCs' games part (docs/GAME.md, "Two PCs"): two games
# through the link (tools/linktest.py), game 2's heap 2 MB higher, both players mashing varied moves (tools/py/mash.py),
# first to 3 rounds, the match going on after a desync (SC5_NET_DESYNC_CONTINUE) with every check frame's snapshot
# dumped in both and compared (tools/py/snapsweep.py). Prints the stage's id, the result, each game's rounds (the match
# frames each round's fighting began and ended at, which must be the same in both), the first checks that differed and
# the words that did.
#   tools/stagehunt.sh <name> <p1> <p2> <stage place 1-27 | random> [seed 1] [lag ms 40] [frames 9000] [more linktest args]
# Git Bash, from anywhere. Logs: build/runs/<name>_1.log, _2.log; the dumps are deleted.
set -u
NAME=$1; P1=$2; P2=$3; PLACE=$4; SEED=${5:-1}; LAG=${6:-40}; FRAMES=${7:-9000}
shift $(( $# < 7 ? $# : 7 ))
cd "$(dirname "$0")/.." || exit 1
RUNS=build/runs
mkdir -p "$RUNS"
DUMPS="$RUNS/$NAME-dumps"
rm -rf "$DUMPS"; mkdir -p "$DUMPS"
timeout $((FRAMES / 30 + 200)) python -I tools/linktest.py --name "$NAME" --p1 "$P1" --p2 "$P2" --rounds 5 --delay 1 \
  --lag "$LAG" --heap-shift2 2 --seconds $((FRAMES / 30 + 180)) --seed "$SEED" \
  --fight1 "$(python -I tools/py/mash.py $((SEED * 2 + 1)) "$FRAMES")" \
  --fight2 "$(python -I tools/py/mash.py $((SEED * 2 + 2)) "$FRAMES")" \
  --env "SC5_NET_DUMP_DIR=$(cygpath -w "$PWD/$DUMPS")" --env SC5_NET_DUMP_FROM=0 --env SC5_NET_DUMP_COUNT=600 \
  --env SC5_NET_DESYNC_CONTINUE=1 --env "SC5_TEST_STAGE=$PLACE" "$@" > "$RUNS/$NAME.out" 2>&1
echo "=== $NAME: $P1 vs $P2, place $PLACE, seed $SEED, lag $LAG: $(grep -oE "stage [0-9A-F]+," "$RUNS/${NAME}_1.log" | head -1)"
grep -E "result|desync|error|exited|done after" "$RUNS/$NAME.out" | sed 's/^ *//' | head -5
for i in 1 2; do
  echo "  game $i rounds: $(grep -oE "sc5 net: (rollback|lockstep) from frame [0-9]+" "$RUNS/${NAME}_$i.log" |
    sed -E 's/sc5 net: rollback from frame /+/; s/sc5 net: lockstep from frame /-/' | tr '\n' ' ')"
  echo "  game $i: $(grep -cE "sc5 net: DESYNC" "$RUNS/${NAME}_$i.log") checks apart; first: $(grep -oE "DESYNC at frame [0-9]+" "$RUNS/${NAME}_$i.log" | head -3 | tr '\n' ' ')"
done
python -I tools/py/snapsweep.py "$DUMPS" 2 2>&1 | head -${HUNT_LINES:-25}
[ -n "${HUNT_KEEP:-}" ] || rm -rf "$DUMPS"
