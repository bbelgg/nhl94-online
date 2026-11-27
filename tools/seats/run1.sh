#!/bin/bash
# run1.sh <build> <out> <name> <seats> <frames> <script> [env...] -- one headless run, prints the state hash.
B=$(cd "$1" && pwd); OUT=$2; name=$3; seats=$4; frames=$5; script=$6; shift 6
HERE=$(cd "$(dirname "$0")" && pwd); ROM="$HERE/../../game/NHL94.bin"; mkdir -p "$OUT"
rm -f "$B/NHL94.srm"
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 NHL94_SEATS="$seats" "$@" \
    "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark "$frames" --input-script "$script" \
    > "$OUT/$name.out" 2> "$OUT/$name.err"
printf '%-16s %s %s\n' "$name" "$(grep -o '"state_fnv1a64":"[0-9A-F]*"' "$OUT/$name.out")" "$(grep -o '"complete":[a-z]*' "$OUT/$name.out")"
