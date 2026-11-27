#!/bin/bash
# run_checks_pass.sh -- EASHL passing verification (docs/PLAYER_LOCK.md "Aimed passing", "Call icon").
#   tools/seats/run_checks_pass.sh <build dir> <out dir>
# Unit test, aimpass1 hashes (twice; icon off; PASSASSIST=off), the old
# baselines under NHL94_PASSASSIST=off, Demo, the icon screenshots (icon on /
# off: same state hash), rb_probe.
set -u
B=$(cd "${1:?build dir}" && pwd); OUT="${2:?out dir}"
HERE=$(cd "$(dirname "$0")" && pwd); ROM="$HERE/../../game/NHL94.bin"
mkdir -p "$OUT"
echo "== unit test"
cc -Wall -I "$HERE/../../game" -o "$OUT/test_aimpass" "$HERE/test_aimpass.c" "$HERE/../../game/nhl94_aim.c" -lm && "$OUT/test_aimpass"
run() {  # name seats frames script [env...]
    local name=$1 seats=$2 frames=$3 script=$4; shift 4
    mkdir -p "$OUT/w_$name"
    ( cd "$OUT/w_$name" && env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 NHL94_SEATS="$seats" "$@" \
        "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark "$frames" --input-script "$script" \
        > "$OUT/$name.out" 2> "$OUT/$name.err" )
    printf '%-16s %s\n' "$name" "$(grep -o '"state_fnv1a64":"[0-9A-F]*"' "$OUT/$name.out")"
}
echo "== aimpass1 (home:C,home:RD): determinism, icon on/off, assist off"
run aim_a "home:C,home:RD" 4500 "$HERE/aimpass1.input" &
run aim_b "home:C,home:RD" 4500 "$HERE/aimpass1.input" &
run aim_noicon "home:C,home:RD" 4500 "$HERE/aimpass1.input" NHL94_CALLICON=0 &
run aim_off "home:C,home:RD" 4500 "$HERE/aimpass1.input" NHL94_PASSASSIST=off &
wait
grep -h "aim pass\|summary aim" "$OUT/aim_a.err"
echo "== old baselines with NHL94_PASSASSIST=off (callpass1 FAA72A99530568E6, callpass2 560C3425A02A9A91)"
run cp1_off "home:C" 6300 "$HERE/callpass1.input" NHL94_PASSASSIST=off &
run cp2_off "home:C,home:RD" 6300 "$HERE/callpass2.input" NHL94_PASSASSIST=off &
run cp1_on "home:C" 6300 "$HERE/callpass1.input" &
run cp2_on "home:C,home:RD" 6300 "$HERE/callpass2.input" &
run demo "" 4000 "$HERE/demo.input" &
wait
echo "(Demo must be 80CE87C28143ADBF)"
echo "== call icon: screenshots (icon on) and the same run with the icon off / views"
for v in on off; do
    sed -e "s|@OUT@|$OUT|g" -e "s|@NAME@|icons_$v|g" "$HERE/icons2.input" > "$OUT/icons_$v.input"
done
run icons_on "home:C,home:RD" 6000 "$OUT/icons_on.input" &
run icons_off "home:C,home:RD" 6000 "$OUT/icons_off.input" NHL94_CALLICON=0 &
run icons_notags "home:C,home:RD" 6000 "$OUT/icons_off.input" NHL94_NAMETAGS=0 &
run icons_all "home:C,home:RD" 6000 "$OUT/icons_on.input" NHL94_CALLICON=all &
wait
ls "$OUT"/icons_on_*.png 2>/dev/null
echo "== rb_probe (no ROM copy)"
RB_PROBE_OUT="$OUT/rbp_aim" bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 6300 30:8 "$HERE/aimpass1.input" -- --seats home:C,home:RD | grep "summary: \|PASS\|FAIL"
