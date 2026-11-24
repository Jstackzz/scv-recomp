#!/usr/bin/env bash
# tools/stagehunt.sh over a list of stage places, one match after the other, with each match's fight draws compared
# too (SC5_DBG_DRAWS, tools/py/drawscmp.py). A summary line a place goes to the output file as it ends.
#   tools/stagesweep.sh <out file> <tag> <port base> <p1> <p2> <lag> <frames> <place> [<place> ...]
# HUNT_SCRIPT, HUNT_EXE (a copy of the game) and HUNT_ENV ("NAME=VALUE NAME=VALUE": more variables for both games) pass
# through. Two sweeps at once need two port bases.
set -u
OUT=$1; TAG=$2; PORT=$3; P1=$4; P2=$5; LAG=$6; FRAMES=$7
shift 7
cd "$(dirname "$0")/.." || exit 1
RUNS=build/runs
mkdir -p "$RUNS"
D="$(cygpath -w "$PWD/$RUNS")"
extra=()
[ -n "${HUNT_EXE:-}" ] && extra+=(--exe "$HUNT_EXE")
for kv in ${HUNT_ENV:-}; do extra+=(--env "$kv"); done
for place in "$@"; do
  name="$TAG$place"
  bash tools/stagehunt.sh "$name" "$P1" "$P2" "$place" "$place" "$LAG" "$FRAMES" --port-base "$PORT" \
    --env "SC5_DBG_DRAWS=$D\\${name}_draws{i}.txt" "${extra[@]}" > "$RUNS/$name.sum" 2>&1
  {
    echo "$(grep -E '^===' "$RUNS/$name.sum" | cut -c1-110) | $(grep -oE 'result winner=[0-9] p1_rounds=[0-9] p2_rounds=[0-9]' "$RUNS/$name.sum" | head -1) | $(grep -E 'game 1: [0-9]+ checks apart' "$RUNS/$name.sum" | sed 's/^ *game 1: //' | cut -c1-90) | ends $(grep -E 'game 1 ends:' "$RUNS/$name.sum" | sed 's/^ *game 1 ends: //' | cut -c1-120) | draws: $(python -I tools/py/drawscmp.py "$RUNS/${name}_draws1.txt" "$RUNS/${name}_draws2.txt" 1 2>&1 | tail -1 | cut -c1-90)"
    grep -E 'desync:|other|pointer|error|exited' "$RUNS/$name.sum" | head -8 | cut -c1-200
  } >> "$OUT"
  rm -f "$RUNS/${name}"_draws*.txt "$RUNS/${name}"_*net-*.log "$RUNS/${name}"_*stats-*.json "$RUNS/$name.out"
done
echo "SWEEP $TAG DONE" >> "$OUT"
