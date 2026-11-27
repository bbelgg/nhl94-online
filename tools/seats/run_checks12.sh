#!/bin/bash
# run_checks12.sh -- 6- and 12-seat player-lock checks (virtual pads, seats 5-12;
# docs/PLAYER_LOCK.md "6 and 12 seats"). The 1-4 seat battery is run_checks.sh.
#   tools/seats/run_checks12.sh <build dir> <out dir>
# Prints state hashes (twice each + with views), seat_stats.py tables and
# rb_probe verdicts. Seats beyond the engine's GENESIS_SIM_MAX_PLAYERS get no
# input: with a 6-pad engine the 12-seat run shows seats 7-12 vacant (AI).
set -u
# Per-player builds (docs/BUILDS.md) are ON by default and change the seat hashes; these batteries are the
# pre-builds baselines, so they run with builds off. tools/seats/builds_checks.sh covers builds on.
export NHL94_BUILDS=off
B=$(cd "${1:?build dir}" && pwd); OUT="${2:?out dir}"
HERE=$(cd "$(dirname "$0")" && pwd); ROM="$HERE/../../game/NHL94.bin"
mkdir -p "$OUT"
S6="home:C,away:C,home:RD,away:G,home:LW,away:RW"
S6SW="home:LW,away:RW,home:RD,away:G,home:C,away:C"
S12="home:C,away:C,home:LW,away:LW,home:RW,away:RW,home:LD,away:LD,home:RD,away:RD,home:G,away:G"
run() {  # name seats frames script [env...]
    local name=$1 seats=$2 frames=$3 script=$4; shift 4
    rm -f "$B/NHL94.srm"
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 NHL94_SEATS="$seats" "$@" \
        "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark "$frames" --input-script "$script" \
        > "$OUT/$name.out" 2> "$OUT/$name.err"
    printf '%-14s %s %s\n' "$name" "$(grep -o '"state_fnv1a64":"[0-9A-F]*"' "$OUT/$name.out")" \
        "$(grep -o '"complete":[a-z]*' "$OUT/$name.out")"
}
grep -m1 -o 'GENESIS_SIM_MAX_PLAYERS [0-9]*' "$HERE/../../segagenesisrecomp/runner/sim_step.h"
echo "== 6 seats ($S6)"
run lock6_a "$S6" 4600 "$HERE/lock6.input" NHL94_SEATS_TRACE=10
run lock6_b "$S6" 4600 "$HERE/lock6.input"
run lock6_6views "$S6" 4600 "$HERE/lock6.input" GENESIS_VIEWS=6 NHL94_VIEWS="$S6"
python3 "$HERE/seat_stats.py" "$OUT/lock6_a.err"
grep -h "nhl94_seats: summary" "$OUT/lock6_a.err"
echo "== 6 seats, roles swapped between real and virtual pads ($S6SW)"
run lock6_swapped "$S6SW" 4600 "$HERE/lock6_swapped.input" NHL94_SEATS_TRACE=10
python3 "$HERE/seat_stats.py" "$OUT/lock6_swapped.err"
echo "== 12 seats ($S12)"
run lock12_a "$S12" 5200 "$HERE/lock12.input" NHL94_SEATS_TRACE=10
run lock12_b "$S12" 5200 "$HERE/lock12.input"
python3 "$HERE/seat_stats.py" "$OUT/lock12_a.err" --expect-joy-all
grep -h "nhl94_seats: summary\|engine carries" "$OUT/lock12_a.err"
echo "== rb_probe (no ROM copy)"
RB_PROBE_OUT="$OUT/rbp_lock6" bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 4600 30:8 "$HERE/lock6.input" -- --seats "$S6" | grep "summary: \|PASS\|FAIL"
RB_PROBE_OUT="$OUT/rbp_lock12" bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 5200 30:8 "$HERE/lock12.input" -- --seats "$S12" | grep "summary: \|PASS\|FAIL"
