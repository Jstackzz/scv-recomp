#!/usr/bin/env bash
# Two games on this PC playing one online match (sc5/net/netplay.h over 127.0.0.1): the scripted VS fight of
# tools/synctest.sh, player 1's fight script played in game 1 and player 2's in game 2, each sent to the other.
# Prints both games' battle timelines, which must match, their netplay lines, and any desync.
#
#   tools/nettest.sh [seconds=210] [name=nettest] [lag ms=0] [delay=2] [jitter ms=0]
#
# Git Bash, from anywhere; the release build must be there (sc5\build.cmd release). Each game gets a copy of
# game\user (its own saves), deleted afterwards. Logs: build\runs\<name>_1.log, _2.log, the netplay's frame logs
# <name>_net1.log, _net2.log. Other SC5_* variables in the environment pass through.
set -u
T=${1:-210}
NAME=${2:-nettest}
LAG=${3:-0}
DELAY=${4:-2}
JITTER=${5:-0}
cd "$(dirname "$0")/.." || exit 1
B=sc5/out/build/win-amd64-release
W() { cygpath -w "$1"; }
RUNS=build/runs
mkdir -p "$RUNS"
export SC5_GAME_DIR="$(W "$PWD/game/disc")" SC5_UPDATE_DIR="$(W "$PWD/game/update")"
export SC5_PADS=2 SC5_TEST_SEED=12345
# menus: title, main menu, VS Battle, the select (both games alike), stage, fight
export SC5_INPUT_P1="1500:START,1510:-,1810:START,1820:-,2820:A,2830:-,2920:UP,2928:-,2968:UP,2976:-,3036:A,3046:-,3200:DOWN,3208:-,3260:A,3270:-,3560:A,3570:-,3900:A,3910:-,4200:A,4210:-,4400:A,4410:-,4600:A,4610:-,5000:A,5010:-,5300:A,5310:-,5600:A,5610:-"
export SC5_INPUT_P2="3960:A,3970:-,4060:A,4070:-,4260:A,4270:-"
# the fight, in fight frames (0: round 1's call)
export SC5_FIGHT_INPUT_P1="130:RIGHT,170:-,180:A,186:-,210:B,216:-,240:X,246:-,270:A+B,276:-,300:DOWN+A,306:-,340:Y,346:-,370:RIGHT,400:-,410:A,416:-,450:X+Y,456:-,500:B,506:-,530:A,536:-,600:RIGHT,700:-,710:A,716:-,740:B,746:-,780:A+B,786:-,820:X,826:-,860:A,866:-,900:B,906:-,940:A,946:-,980:Y,986:-"
export SC5_FIGHT_INPUT_P2="140:LEFT,180:-,190:B,196:-,220:A,226:-,250:Y,256:-,290:B+X,296:-,320:UP,330:-,360:A,366:-,390:LEFT,420:-,440:B,446:-,480:X,486:-,520:A+B,526:-"
export SC5_NET_DELAY="$DELAY" SC5_NET_LAG="$LAG" SC5_NET_JITTER="$JITTER"

pids=()
for i in 1 2; do
  user="$RUNS/${NAME}-user$i"
  rm -rf "$user"
  cp -r game/user "$user"
  other=$((3 - i))
  rm -f "$RUNS/${NAME}_$i.log" "$RUNS/${NAME}_net$i.log"
  # SC5_TEST_HEAP_SHIFT_2: game 2's heap shifted (tests of what in a snapshot is a raw pointer)
  shift=0
  [ "$i" = 2 ] && shift="${SC5_TEST_HEAP_SHIFT_2:-0}"
  SC5_TEST_HEAP_SHIFT="$shift" \
  SC5_USER_DIR="$(W "$PWD/$user")" SC5_NET_BIND=$((7100 + i)) SC5_NET_PEER="127.0.0.1:$((7100 + other))" \
    SC5_NET_SIDE=$i SC5_NET_LOG="$(W "$PWD/$RUNS/${NAME}_net$i.log")" \
    timeout "$T" "$B/sc5.exe" --log_file="$(W "$PWD/$RUNS/${NAME}_$i.log")" --log_level=info --fullscreen=false \
    --window_width=960 --window_height=540 >/dev/null 2>&1 &
  pids+=($!)
  sleep 2
done
wait "${pids[@]}"
for i in 1 2; do
  log="$RUNS/${NAME}_$i.log"
  echo "== game $i: $(grep -oE "frame [0-9]+ battle [A-Za-z ]+ -> [A-Za-z ]+" "$log" |
    sed -E 's/frame ([0-9]+) battle [A-Za-z ]+ -> ([A-Za-z ]+[A-Za-z]) *$/\1 \2/' | tr '\n' '|' | sed 's/|/ | /g')"
  grep -E "sc5 net:" "$log" | sed 's/^.*\] sc5 net: /  /' | head -14
done
for i in 1 2; do rm -rf "$RUNS/${NAME}-user$i"; done
