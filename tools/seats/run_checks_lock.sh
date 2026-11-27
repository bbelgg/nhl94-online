#!/bin/bash
# run_checks_lock.sh -- "a human always has a player" checks (docs/PLAYER_LOCK.md, "Line changes,
# substitutes, pulled goalie, penalty shots").
#   tools/seats/run_checks_lock.sh <build dir> <out dir> [old build dir (the pre-change build, for the neutrality diff)]
# Scripts come from gen_input.py (committed *.input): lc_auto, pen_sub, ps_4v4 (pull_g = pen_sub with G seats).
# Test-only env: NHL94_FORMAT_TEST_PENALTY (nhl94_format.c), NHL94_SEATS_TEST_PS / NHL94_SEATS_NO_AUTOLC (nhl94_seats.c).
set -u
B=$(cd "${1:?build dir}" && pwd); OUT="${2:?out dir}"; OLD="${3:-}"
HERE=$(cd "$(dirname "$0")" && pwd); ROM="$HERE/../../game/NHL94.bin"
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
S4="home:C,home:LW,away:C,away:LW"; SP="home:C,home:LW,home:RW,away:C"; SG="home:G,home:C,away:G,away:C"
S8="home:C,away:C,home:LW,away:LW,home:LD,away:LD,home:G,away:G"
PEN="NHL94_FORMAT_TEST_PENALTY=2400:home:LD"
PULL="NHL94_FORMAT_TEST_PENALTY=2400:home:LD,2500:away:LD,3600:home:RW,3700:away:RW,4800:home:LW,4900:away:LW,6000:home:RD,6100:away:RD"
PS="NHL94_SEATS_TEST_PS=2500:0:4,4000:1:3"
run() { "$HERE/run1.sh" "$B" "$OUT" "$@"; }
lines() { grep -h "nhl94_seats: summary lines\|summary setpersonel\|takes substitute\|leaves substitute\|MID-PLAY\|TEST" "$OUT/$1.err" | sed 's/^nhl94_seats: //' | head -${2:-12}; }
echo "== lc_auto: forced automatic line changes (expect sf7 bit 4, lcm_seen=0, midplay_handover=0, pending_released=0)"
run lc_a "$S4" 9000 "$HERE/lc_auto.input" NHL94_SEATS_TRACE=10
run lc_b "$S4" 9000 "$HERE/lc_auto.input"
lines lc_a
echo "-- negative control NHL94_SEATS_NO_AUTOLC=1 (expect lcm_seen > 0: the pad 1/2 box opens)"
run lc_neg "$S4" 9000 "$HERE/lc_auto.input" NHL94_SEATS_TRACE=10 NHL94_SEATS_NO_AUTOLC=1
lines lc_neg 2
echo "== pen_sub: penalty on home LD -> the RW seat takes a substitute and gets the RW body back"
run pen_a "$SP" 11000 "$HERE/pen_sub.input" NHL94_SEATS_TRACE=10 $PEN
run pen_b "$SP" 11000 "$HERE/pen_sub.input" $PEN
lines pen_a
echo "== pull_g: G seats, injected delayed penalties pull a goalie (the G seat takes a skater / the extra attacker, then the goalie again)"
run pull_a "$SG" 9000 "$HERE/pen_sub.input" NHL94_SEATS_TRACE=10 $PULL
run pull_b "$SG" 9000 "$HERE/pen_sub.input" $PULL
lines pull_a
echo "== ps_4v4: staged penalty shots (shooter = fouled role's seat, goalie = G seat); mode=2 lines in the trace"
run ps_a "$S8" 8000 "$HERE/ps_4v4.input" NHL94_FORMAT=4v4 NHL94_SEATS_TRACE=5 $PS
run ps_b "$S8" 8000 "$HERE/ps_4v4.input" NHL94_FORMAT=4v4 $PS NHL94_VIEW=off
lines ps_a 6
grep -o " mode=[0-9] " "$OUT/ps_a.err" | sort | uniq -c | tr '\n' ' '; echo
echo "== ps_12: 12 seats (virtual pads) in penalty shots: shooter away:RW (seat 5), goalie home:G (seat 10), then home:LW / G"
S12="home:C,away:C,home:LW,away:LW,home:RW,away:RW,home:LD,away:LD,home:RD,away:RD,home:G,away:G"
PS12="NHL94_SEATS_TEST_PS=2500:1:5,4500:0:3,6000:0:4"
run ps12_a "$S12" 7000 "$HERE/ps_12.input" NHL94_SEATS_TRACE=5 $PS12 $PEN
run ps12_b "$S12" 7000 "$HERE/ps_12.input" $PS12 $PEN
lines ps12_a 5
echo "== observation: setpersonel callers over a long 6v6 / 4v4 run with penalties (a MID-PLAY caller other than the goalie pulls would justify the pin)"
TP="2500:puck,2600:puck,5500:puck,8500:puck,8600:puck,11500:puck,14500:puck,14600:puck"
run obs4 "$S8" 18000 "$HERE/obs_long.input" NHL94_FORMAT=4v4 NHL94_FORMAT_TEST_PENALTY=$TP
run obs6 "home:C,home:LW,home:RD,away:C,away:RW,away:G" 18000 "$HERE/obs_long.input" NHL94_FORMAT_TEST_PENALTY=$TP
lines obs4 6; lines obs6 6
echo "== rb_probe 30:8"
rbp() { local n=$1 f=$2 s=$3 e=$4; shift 4
    RB_PROBE_OUT="$OUT/rbp_$n" RB_PROBE_ENV="$e" bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" "$f" 30:8 "$s" -- "$@" | grep "summary: \|PASS\|FAIL" | tr '\n' ' '; echo "[$n]"; }
rbp lc 9000 "$HERE/lc_auto.input" "NHL94_X=1" --seats "$S4"
rbp pen 9000 "$HERE/pen_sub.input" "$PEN,3000:away:RD" --seats "$SP"
rbp pull 9000 "$HERE/pen_sub.input" "$PULL" --seats "$SG"
rbp ps12 7000 "$HERE/ps_12.input" "$PS12 $PEN" --seats "$S12"
rbp ps 8000 "$HERE/ps_4v4.input" "$PS" --format 4v4 --seats "$S8"
if [ -n "$OLD" ]; then
echo "== neutrality: NHL94_SEATS_NO_AUTOLC=1 on the pre-change build vs this one (trace without the new fields must be identical)"
norm() { sed -E 's/ mode=[0-9] optline=[0-9]+ sf7=[0-9A-F]+ line=[0-9]+\/[0-9]+ tmg=-?[0-9]+\/-?[0-9]+ ap=[0-9]+\/[0-9]+//; s/\(sub\)//' "$1" | grep "^nhl94_seats: t="; }
while read -r name seats frames script; do
    "$HERE/run1.sh" "$B" "$OUT" n_$name "$seats" "$frames" "$HERE/$script.input" NHL94_SEATS_TRACE=10 NHL94_SEATS_NO_AUTOLC=1 > /dev/null
    "$HERE/run1.sh" "$OLD" "$OUT" o_$name "$seats" "$frames" "$HERE/$script.input" NHL94_SEATS_TRACE=10 > /dev/null
    norm "$OUT/n_$name.err" > "$OUT/n_$name.tr"; norm "$OUT/o_$name.err" > "$OUT/o_$name.tr"
    cmp -s "$OUT/n_$name.tr" "$OUT/o_$name.tr" && echo "$name: trace identical ($(wc -l < "$OUT/n_$name.tr") lines)" || echo "$name: trace DIFFERS"
done <<'LIST'
lock2 home:C,away:G 4800 lock2
lock4 home:C,home:RD,away:C,away:G 4200 lock4
callpass home:C 6300 callpass1
LIST
fi
