#!/usr/bin/env bash
# The rollback check on one PC (docs\GAME.md, "Determinism"): a scripted VS fight (Siegfried against Siegfried, seed
# 12345), run once plainly and once with SC5_SYNCTEST=<n> (every frame of the fight is put back n frames and run
# again). Prints both runs' battle timelines, which must match, and what the sync test found.
#
#   tools/synctest.sh [n=1] [seconds=170] [name=synctest] [plain|roll|both=both]
#
# Git Bash, from anywhere; the release build must be there (sc5\build.cmd release). Other SC5_* variables in the
# environment pass through (SC5_SYNCTEST_EVERY, SC5_DBG_*). Logs: build\runs\<name>_plain.log, <name>_roll.log.
set -u
N=${1:-1}
T=${2:-170}
NAME=${3:-synctest}
WHICH=${4:-both}
cd "$(dirname "$0")/.." || exit 1
B=sc5/out/build/win-amd64-release
W() { cygpath -w "$1"; }
mkdir -p build/runs
export SC5_GAME_DIR="$(W "$PWD/game/disc")" SC5_UPDATE_DIR="$(W "$PWD/game/update")" SC5_USER_DIR="$(W "$PWD/game/user")"
export SC5_PADS=2 SC5_TEST_SEED=12345 SC5_BATTLE_RAW=1
# menus: title, the main menu's two windows (title update 3's notice of its changes, then the first visit's), VS
# Battle, both players pick, stage, fight
export SC5_INPUT_P1="${SC5_INPUT_P1_OVERRIDE:-1500:START,1510:-,1810:START,1820:-,2820:A,2830:-,3100:A,3110:-,3200:UP,3208:-,3248:UP,3256:-,3316:A,3326:-,3480:DOWN,3488:-,3540:A,3550:-,3840:A,3850:-,4180:A,4190:-,4480:A,4490:-,4680:A,4690:-,4880:A,4890:-,5280:A,5290:-,5580:A,5590:-,5880:A,5890:-}"
export SC5_INPUT_P2="${SC5_TEST_P2_MENU:-4240:A,4250:-,4340:A,4350:-,4540:A,4550:-}"
# the fight, in fight frames (0: round 1's call); SC5_FIGHT_P1_OVERRIDE / _P2_OVERRIDE replace them
export SC5_FIGHT_INPUT_P1="${SC5_FIGHT_P1_OVERRIDE:-130:RIGHT,170:-,180:A,186:-,210:B,216:-,240:X,246:-,270:A+B,276:-,300:DOWN+A,306:-,340:Y,346:-,370:RIGHT,400:-,410:A,416:-,450:X+Y,456:-,500:B,506:-,530:A,536:-,600:RIGHT,700:-,710:A,716:-,740:B,746:-,780:A+B,786:-,820:X,826:-,860:A,866:-,900:B,906:-,940:A,946:-,980:Y,986:-}"
export SC5_FIGHT_INPUT_P2="${SC5_FIGHT_P2_OVERRIDE:-140:LEFT,180:-,190:B,196:-,220:A,226:-,250:Y,256:-,290:B+X,296:-,320:UP,330:-,360:A,366:-,390:LEFT,420:-,440:B,446:-,480:X,486:-,520:A+B,526:-}"

runs=()
case "$WHICH" in plain) runs=(plain) ;; roll) runs=(roll) ;; *) runs=(plain roll) ;; esac
for run in "${runs[@]}"; do
  if [ "$run" = roll ]; then export SC5_SYNCTEST="$N"; else unset SC5_SYNCTEST; fi
  log="build/runs/${NAME}_$run.log"
  rm -f "$log"
  timeout "$T" "$B/sc5.exe" --log_file="$(W "$PWD/$log")" --log_level=info --fullscreen=false --window_width=960 \
    --window_height=540 >/dev/null 2>&1
done
for run in "${runs[@]}"; do
  log="build/runs/${NAME}_$run.log"
  echo "== $run: $(grep -oE "frame [0-9]+ battle [A-Za-z ]+ -> [A-Za-z ]+" "$log" |
    sed -E 's/frame ([0-9]+) battle [A-Za-z ]+ -> ([A-Za-z ]+[A-Za-z]) *$/\1 \2/' | tr '\n' '|' | sed 's/|/ | /g')"
done
[ -f "build/runs/${NAME}_roll.log" ] && grep -E "sc5 synctest|sc5 dbg: wide" "build/runs/${NAME}_roll.log" |
  sed 's/^.*\] sc5/sc5/' | head -30
