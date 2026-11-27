#!/bin/bash
# run_checks4v4.sh -- 4v4 format checks (docs/FOUR_ON_FOUR_PLAN.md section 8).
#   tools/seats/run_checks4v4.sh <build dir> <out dir>
# The 6v6 no-regression check is run_checks.sh / run_checks12.sh against
# tools/upstream-sync/baseline/*.hashes (format unset). This script: state
# hashes twice per 4v4 configuration (+ views on/off), the host-side format
# trace asserts (NHL94_FORMAT_TRACE), the test-only injected penalties
# (NHL94_FORMAT_TEST_PENALTY), rb_probe with --format 4v4, and a 2-process LAN
# run that must be REFUSED (host 4v4, guest 6v6: a LAN room carries no match
# caps, so nothing is adopted). Adoption through a lobby: lobby_format_test.sh.
set -u
# Per-player builds (docs/BUILDS.md) are ON by default and change the seat hashes; these batteries are the
# pre-builds baselines, so they run with builds off. tools/seats/builds_checks.sh covers builds on.
export NHL94_BUILDS=off
B=$(cd "${1:?build dir}" && pwd); OUT="${2:?out dir}"
HERE=$(cd "$(dirname "$0")" && pwd); ROM="$HERE/../../game/NHL94.bin"
mkdir -p "$OUT"
S8="home:C,away:C,home:LW,away:LW,home:LD,away:LD,home:G,away:G"
run() {  # name seats frames script [env...]
    local name=$1 seats=$2 frames=$3 script=$4; shift 4
    rm -f "$B/NHL94.srm"
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 NHL94_SEATS="$seats" "$@" \
        "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark "$frames" --input-script "$script" \
        > "$OUT/$name.out" 2> "$OUT/$name.err"
    printf '%-16s %s %s misses=%s\n' "$name" "$(grep -o '"state_fnv1a64":"[0-9A-F]*"' "$OUT/$name.out")" \
        "$(grep -o '"complete":[a-z]*' "$OUT/$name.out")" "$(grep -o '"dispatch_misses":[^,}]*' "$OUT/$name.out")"
}
echo "== 4v4 determinism (same script twice)"
run f4_a "$S8" 20000 "$HERE/four_on_four.input" NHL94_FORMAT=4v4 NHL94_FORMAT_TRACE=30
run f4_b "$S8" 20000 "$HERE/four_on_four.input" NHL94_FORMAT=4v4
run f4_noview "$S8" 20000 "$HERE/four_on_four.input" NHL94_FORMAT=4v4 NHL94_VIEW=off
run f4_4views "$S8" 20000 "$HERE/four_on_four.input" NHL94_FORMAT=4v4 NHL94_VIEWS=home:C,away:C,home:LD,away:LD
run pen4_a "" 30000 "$HERE/long_penalties.input" NHL94_FORMAT=4v4 NHL94_FORMAT_TRACE=30
run pen4_b "" 30000 "$HERE/long_penalties.input" NHL94_FORMAT=4v4
TP="2500:puck,2600:puck,5500:puck,8500:puck,8600:puck,11500:puck,14500:puck,14600:puck"
run pentest4_a "" 18000 "$HERE/demo_penalties.input" NHL94_FORMAT=4v4 NHL94_FORMAT_TRACE=30 NHL94_FORMAT_TEST_PENALTY=$TP
run pentest4_b "" 18000 "$HERE/demo_penalties.input" NHL94_FORMAT=4v4 NHL94_FORMAT_TEST_PENALTY=$TP
run pentest6 "" 18000 "$HERE/demo_penalties.input" NHL94_FORMAT_TRACE=30 NHL94_FORMAT_TEST_PENALTY=$TP
run lock2_4v4_a "home:C,away:G" 4800 "$HERE/lock2.input" NHL94_FORMAT=4v4
run lock2_4v4_b "home:C,away:G" 4800 "$HERE/lock2.input" NHL94_FORMAT=4v4
run lock4_4v4_a "home:C,home:LW,away:C,away:G" 4200 "$HERE/lock4.input" NHL94_FORMAT=4v4
run lock4_4v4_b "home:C,home:LW,away:C,away:G" 4200 "$HERE/lock4.input" NHL94_FORMAT=4v4
echo "== an RD seat in 4v4 is refused (-> Classic)"
grep -h "nhl94_seats: malformed\|nhl94_seats: control mode" "$OUT/f4_a.err" | head -2
rm -f "$B/NHL94.srm"
env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 NHL94_SEATS=home:C,home:RD NHL94_FORMAT=4v4 \
    "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark 10 2>&1 | grep "nhl94_seats: malformed\|control mode" | head -2
echo "== format trace summaries (asserts: RD/RW at even strength, on-ice != tmap at faceoffs)"
for f in f4_a pen4_a pentest4_a pentest6; do echo "-- $f"; grep -h "nhl94_format: summary\|ASSERT" "$OUT/$f.err" | head -6; done
echo "-- pentest4_a strength changes (tmap home/away, PBnum, on-ice positions)"
grep -h "nhl94_format: t=" "$OUT/pentest4_a.err" | grep -v TEST | awk '{k=$6" "$7" "$8" "$9" "$10; if(k!=last){print; last=k}}' | cut -c15-160
grep -h "nhl94_seats: summary frames\|nhl94_seats: summary seat" "$OUT/f4_a.err"
echo "== rb_probe --format 4v4 (no ROM copy)"
RB_PROBE_OUT="$OUT/rbp_f4" bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 6000 30:8 "$HERE/four_on_four.input" -- --format 4v4 --seats "$S8" | grep "summary: \|PASS\|FAIL"
RB_PROBE_ENV="NHL94_FORMAT_TEST_PENALTY=$TP" RB_PROBE_OUT="$OUT/rbp_pentest4" bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 18000 30:8 "$HERE/demo_penalties.input" -- --format 4v4 | grep "summary: \|PASS\|FAIL"
RB_PROBE_OUT="$OUT/rbp_pen4" bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 12000 30:8 "$HERE/long_penalties.input" -- --format 4v4 | grep "summary: \|PASS\|FAIL"
echo "== netplay (LAN, no lobby: no adoption): host 4v4 vs guest 6v6 must be REFUSED"
rm -f "$B/NHL94.srm"
for s in 0 1; do
    script="$HERE/net_slot$s.input"; fmt=""; [ $s = 0 ] && fmt=4v4
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 GENESIS_NETPLAY=1 GENESIS_NET_MODE=rollback \
        GENESIS_NET_TRANSPORT=lan GENESIS_NET_SLOTS=2 GENESIS_NET_TIMELINE_EVERY=60 GENESIS_NET_SLOT=$s \
        GENESIS_NET_BIND=127.0.0.1:$((47421+s)) GENESIS_NET_PEER=127.0.0.1:$((47422-s)) \
        NHL94_SEATS=home:C,away:C NHL94_FORMAT=$fmt \
        "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark 3000 --input-script "$script" \
        > "$OUT/netrefuse$s.out" 2> "$OUT/netrefuse$s.err" &
done
wait
grep -h "^NETPLAY_DRIVER" "$OUT/netrefuse0.err" "$OUT/netrefuse1.err" | grep -o "sim=[0-9]*\|refusal=[a-z_]*" | paste -sd' ' -
echo "== netplay adoption (lobby): tools/seats/lobby_format_test.sh <peer-root> 4v4 6v6 (needs a lobby;"
echo "   LAN=0 RNET_LOBBY_URL=ws://127.0.0.1:<port> with a local recomp-net-server)"
