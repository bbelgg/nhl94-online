#!/bin/bash
# drop_checks.sh -- offline checks of drop-in / drop-out (docs/DROPIN.md):
# GENESIS_SIM_TEST_ABSENT seals "seat k is no human from tick T" exactly as the
# online occupancy schedule does. Prints state hashes (twice), the rb_probe
# verdict with seats leaving mid-game, and the pause-orphan run.
#   tools/seats/drop_checks.sh <build dir> <out dir>
set -u
B=$(cd "${1:?build dir}" && pwd); OUT="${2:?out dir}"
HERE=$(cd "$(dirname "$0")" && pwd); ROM="$HERE/../../game/NHL94.bin"
mkdir -p "$OUT"
SEATS=home:C,home:RD,away:C,away:G
run() {  # name frames script absent
    local name=$1 frames=$2 script=$3 absent=$4
    rm -f "$B/NHL94.srm"
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 NHL94_SEATS=$SEATS \
        GENESIS_SIM_TEST_ABSENT="$absent" NHL94_SEATS_TRACE=${TRACE:-600} \
        "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark "$frames" --input-script "$script" \
        > "$OUT/$name.out" 2> "$OUT/$name.err"
    printf '%-16s %s joy:%s\n' "$name" "$(grep -o '"state_fnv1a64":"[0-9A-F]*"' "$OUT/$name.out")" \
        "$(grep -o 'joy=[0-9A-F]*' "$OUT/$name.err" | uniq | tr '\n' ' ')"
}
echo "== seats leave mid-game (lock4): none / 2@2600,3@3400 twice"
run absent_none 4200 "$HERE/lock4.input" ""
run absent_a    4200 "$HERE/lock4.input" "2@2600,3@3400"
run absent_b    4200 "$HERE/lock4.input" "2@2600,3@3400"
echo "== rb_probe with the seats leaving"
RB_PROBE_ENV="GENESIS_SIM_TEST_ABSENT=2@2600,3@3400" RB_PROBE_OUT="$OUT/rbp_absent" \
    bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 4200 30:8 "$HERE/lock4.input" -- --seats $SEATS | grep "summary: \|PASS\|FAIL"
echo "== a seat joins during play (stage C, NHL94 half): leaves 1830, back 1940 -> takes its player at the next whistle"
run join_a 3200 "$HERE/lock4.input" "2@1830-1940"
run join_b 3200 "$HERE/lock4.input" "2@1830-1940"
grep -h "joined during" "$OUT/join_a.err"
RB_PROBE_ENV="GENESIS_SIM_TEST_ABSENT=2@1830-1940" RB_PROBE_OUT="$OUT/rbp_join" \
    bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 3200 30:8 "$HERE/lock4.input" -- --seats $SEATS | grep "summary: \|PASS\|FAIL"
echo "== pause orphan: seat 0 pauses, then leaves"
run pause_orphan 3500 "$HERE/pause_orphan.input" "0@2380"
grep -h "orphaned" "$OUT/pause_orphan.err"
echo "== penalty shot with a join mid-attempt (docs/PLAYER_LOCK.md 'Penalty shots'): away:LW (seat 3) is absent 6000..6900,"
echo "   the staged away-LW shot is live from ~6335 to ~6935; the C seat (1) shoots and must keep it, the joiner waits"
S8="home:C,away:C,home:LW,away:LW,home:LD,away:LD,home:G,away:G"
PSENV="NHL94_FORMAT=4v4 NHL94_BUILDS=off NHL94_SEATS_TEST_PS=2500:0:4,4000:1:3"
psrun() {  # name absent
    rm -f "$B/NHL94.srm"
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 NHL94_SEATS=$S8 $PSENV \
        GENESIS_SIM_TEST_ABSENT="$2" NHL94_SEATS_TRACE=5 \
        "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark 8000 --input-script "$HERE/ps_4v4.input" \
        > "$OUT/$1.out" 2> "$OUT/$1.err"
    printf '%-16s %s\n' "$1" "$(grep -o '"state_fnv1a64":"[0-9A-F]*"' "$OUT/$1.out")"
}
psrun psjoin_a "3@6000-6900"
psrun psjoin_b "3@6000-6900"
python3 "$HERE/ps_latch_grade.py" "$OUT/psjoin_a.err" 3 1
echo "== penalty shot with the SHOOTER leaving mid-attempt (seat 3 gone from 6400): the AI finishes it, nobody is re-dealt the shot"
psrun psleave_a "3@6400"
psrun psleave_b "3@6400"
python3 "$HERE/ps_latch_grade.py" "$OUT/psleave_a.err" 1 3 --leave
echo "== rb_probe: join during the attempt / shooter leaving during it"
for sc in "psjoin 3@6000-6900" "psleave 3@6400"; do
    set -- $sc
    RB_PROBE_ENV="NHL94_FORMAT=4v4 NHL94_BUILDS=off NHL94_SEATS_TEST_PS=2500:0:4,4000:1:3 GENESIS_SIM_TEST_ABSENT=$2" RB_PROBE_OUT="$OUT/rbp_$1" \
        bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 8000 30:8 "$HERE/ps_4v4.input" -- --format 4v4 --seats $S8 | grep "summary: \|PASS\|FAIL" | tr '\n' ' '; echo "[$1]"
done
echo "== a joiner's role body is reserved (seat 1 home:LW is away until 3300; seat 2 home:RW took a substitute at the penalty and must not sit on it)"
SP="home:C,home:LW,home:RW,away:C"
for n in a b; do
    rm -f "$B/NHL94.srm"
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 NHL94_SEATS=$SP NHL94_BUILDS=off \
        NHL94_FORMAT_TEST_PENALTY=2400:home:LD GENESIS_SIM_TEST_ABSENT="1@2500-3300" NHL94_SEATS_TRACE=10 \
        "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark 7000 --input-script "$HERE/pen_sub.input" \
        > "$OUT/subres_$n.out" 2> "$OUT/subres_$n.err"
    printf '%-16s %s\n' "subres_$n" "$(grep -o '"state_fnv1a64":"[0-9A-F]*"' "$OUT/subres_$n.out")"
done
python3 - "$OUT/subres_a.err" <<'PY'
import re, sys
t_join, bad = None, []
for l in open(sys.argv[1], errors="replace"):
    m = re.search(r" t=(\d+) seat 1 joined during play", l)
    if m: t_join = int(m.group(1))
    m = re.search(r" t=(\d+) seat 2 leaves substitute -> -1", l)
    if m and t_join is not None and int(m.group(1)) - t_join < 500: bad.append(int(m.group(1)))
print("SUB_RESERVE %s (join t=%s, substitute lost at %s)" % ("PASS" if t_join and not bad else "FAIL", t_join, bad or "never"))
PY
RB_PROBE_ENV="NHL94_BUILDS=off NHL94_FORMAT_TEST_PENALTY=2400:home:LD GENESIS_SIM_TEST_ABSENT=1@2500-3300" RB_PROBE_OUT="$OUT/rbp_subres" \
    bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 7000 30:8 "$HERE/pen_sub.input" -- --seats $SP | grep "summary: \|PASS\|FAIL" | tr '\n' ' '; echo "[subres]"
