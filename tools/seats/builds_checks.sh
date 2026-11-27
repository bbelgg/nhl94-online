#!/bin/bash
# builds_checks.sh -- per-player builds verification battery (docs/BUILDS.md section "Tests").
#   tools/seats/builds_checks.sh <build dir> <out dir> [peer root for the lobby test]
# Regression (builds off = the pre-builds hashes), default builds, real override checked against the ROM
# (tools/builds/check_builds.py), line-change handover, determinism, rb_probe, 2-process rollback netplay
# (same builds: TIMELINE identical; different builds: REFUSED), the lobby signal path (needs a local lobby
# server on RNET_LOBBY_URL, default ws://127.0.0.1:18741), unit tests.
set -u
B=$(cd "${1:?build dir}" && pwd); OUT="${2:?out dir}"; PEERS="${3:-}"
HERE=$(cd "$(dirname "$0")" && pwd); ROOT="$HERE/../.."; ROM="$ROOT/game/NHL94.bin"
mkdir -p "$OUT"
run() {  # name seats frames script [env...]
    local name=$1 seats=$2 frames=$3 script=$4; shift 4
    rm -f "$B/NHL94.srm"
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 NHL94_SEATS="$seats" "$@" \
        "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark "$frames" --input-script "$script" \
        > "$OUT/$name.out" 2> "$OUT/$name.err"
    printf '%-16s %s %s\n' "$name" "$(grep -o '"state_fnv1a64":"[0-9A-F]*"' "$OUT/$name.out")" \
        "$(grep -o '"complete":[a-z]*' "$OUT/$name.out")"
}
OV="home:C=PIT:Mario Lemieux,away:G=TOR:Potvin"
echo "== 1. regression: no seats (Demo 80CE87C28143ADBF), builds off = the old seat hashes (lock2 3EF834BA226E6E28, lock4 A94E00C87F121117)"
run demo "" 4000 "$HERE/demo.input"
run lock2_off "home:C,away:G" 4800 "$HERE/lock2.input" NHL94_BUILDS=off
run lock4_off "home:C,home:RD,away:C,away:G" 4200 "$HERE/lock4.input" NHL94_BUILDS=off
echo "== 2. default builds (team's 1st-line player per role) + determinism (a, b, own view off, 2 views)"
run lock2_def_a "home:C,away:G" 4800 "$HERE/lock2.input" NHL94_BUILDS_TRACE=1
run lock2_def_b "home:C,away:G" 4800 "$HERE/lock2.input"
grep -h "plays as" "$OUT/lock2_def_a.err"
echo "== 2b. identity: the default build is the on-ice player's own record, so the seat trace (positions, velocities, pads) must equal the builds-off run"
run id_off "home:C,away:G" 4800 "$HERE/lock2.input" NHL94_BUILDS=off NHL94_SEATS_TRACE=10
run id_def "home:C,away:G" 4800 "$HERE/lock2.input" NHL94_SEATS_TRACE=10
for n in id_off id_def; do grep "nhl94_seats: t=" "$OUT/$n.err" | sed 's/^nhl94_seats: //' > "$OUT/$n.tr"; done
cmp -s "$OUT/id_off.tr" "$OUT/id_def.tr" && echo "identity: $(wc -l < "$OUT/id_def.tr") trace lines identical (state hashes differ: the latch block is in WRAM)" || echo "identity: trace DIFFERS"
echo "== 3. real override, fields checked against the ROM"
run lock2_ov_a "home:C,away:G" 4800 "$HERE/lock2.input" NHL94_BUILDS_TRACE=1 "NHL94_BUILDS=$OV"
run lock2_ov_b "home:C,away:G" 4800 "$HERE/lock2.input" "NHL94_BUILDS=$OV"
run lock2_ov_noview "home:C,away:G" 4800 "$HERE/lock2.input" NHL94_VIEW=off "NHL94_BUILDS=$OV"
run lock2_ov_views "home:C,away:G" 4800 "$HERE/lock2.input" NHL94_VIEWS=home:C,away:G "NHL94_BUILDS=$OV"
python3 "$ROOT/tools/builds/check_builds.py" "$OUT/lock2_ov_a.err" --rom "$ROM"
echo "== 4. line change hands the build to the next player of the role"
run lc_ov "home:C" 4800 "$HERE/forced_linechange.input" NHL94_BUILDS_TRACE=1 "NHL94_BUILDS=home:C=PIT:Mario Lemieux"
grep -h "obj=2 side=0.*pos=4" "$OUT/lc_ov.err" | cut -c1-140
echo "== 5. same real player on several seats (S= every skater seat)"
run multi "home:C,away:C,home:LW,away:LW" 4800 "$HERE/lock4.input" NHL94_BUILDS_TRACE=1 "NHL94_BUILDS=S=ASE:Lemieux"
grep -h "plays as" "$OUT/multi.err"
echo "== 6. rb_probe (no ROM copy)"
RB_PROBE_OUT="$OUT/rbp_ov" bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 4200 30:8 "$HERE/lock4.input" -- \
    --seats home:C,home:RD,away:C,away:G --builds "home:C=PIT:Mario Lemieux,away:G=TOR:Potvin,S=ASE:Messier" | grep "summary: \|PASS\|FAIL"
RB_PROBE_OUT="$OUT/rbp_def" bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 4200 30:8 "$HERE/lock4.input" -- \
    --seats home:C,home:RD,away:C,away:G | grep "summary: \|PASS\|FAIL"
netplay() {  # tag env0 env1
local tag=$1 e0=$2 e1=$3
echo "== netplay ($tag): 2 processes, rollback, seat 0 home:C, seat 1 away:C"
rm -f "$B/NHL94.srm"
for s in 0 1; do
    e=$e0; [ $s = 1 ] && e=$e1
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 GENESIS_NETPLAY=1 GENESIS_NET_MODE=rollback \
        GENESIS_NET_TRANSPORT=lan GENESIS_NET_SLOTS=2 GENESIS_NET_TIMELINE_EVERY=60 GENESIS_NET_SLOT=$s \
        GENESIS_NET_BIND=127.0.0.1:$((47411+s)) GENESIS_NET_PEER=127.0.0.1:$((47412-s)) \
        NHL94_SEATS=home:C,away:C NHL94_BUILDS_TRACE=1 "NHL94_BUILDS=$e" \
        "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark 4000 --input-script "$HERE/net_slot$s.input" \
        > "$OUT/net$tag$s.out" 2> "$OUT/net$tag$s.err" &
done
wait
for s in 0 1; do grep '^TIMELINE' "$OUT/net$tag$s.err" | sort -u > "$OUT/net$tag$s.tl"; done
echo "slot0 $(grep -o '"state_fnv1a64":"[0-9A-F]*"' "$OUT/net${tag}0.out") timelines=$(wc -l < "$OUT/net${tag}0.tl")"
echo "slot1 $(grep -o '"state_fnv1a64":"[0-9A-F]*"' "$OUT/net${tag}1.out") timelines=$(wc -l < "$OUT/net${tag}1.tl")"
cmp -s "$OUT/net${tag}0.tl" "$OUT/net${tag}1.tl" && echo "TIMELINE digests identical" || echo "TIMELINE digests DIFFER"
grep -h "^NETPLAY_DRIVER\|refus\|REFUS\|differs" "$OUT/net${tag}0.err" "$OUT/net${tag}1.err" | sed 's/ rtt_ms.*//' | head -4
grep -h "plays as" "$OUT/net${tag}0.err" | head -2
}
netplay same "home:C=PIT:Mario Lemieux,away:C=LA:Gretzky" "home:C=PIT:Mario Lemieux,away:C=LA:Gretzky"
netplay defaults "" ""
netplay NEGATIVE "home:C=PIT:Mario Lemieux" "home:C=LA:Gretzky"
echo "== lobby signal path"
if [ -n "$PEERS" ]; then bash "$HERE/builds_lobby_test.sh" "$PEERS" 2600 | tail -14; else echo "(skipped: pass a peer root)"; fi
echo "== unit tests"
F=$(cd "$B" && ninja -t commands NHL94Recomp | grep -m1 nhl94_seats.c | tr ' ' '\n' | grep '^-[ID]' | sed 's/RECOMP_NET=1/RECOMP_NET=0/' | tr '\n' ' ')
G="$ROOT/game"
eval cc -w $F -o "$OUT/test_roster_table" "$HERE/test_roster_table.c" $G/nhl94_seats.c $G/nhl94_format.c $G/nhl94_builds.c $B/nhl94_roster/nhl94_roster_table.c $G/nhl94_aim.c \
    && "$OUT/test_roster_table" "$ROM" 2>&1 | grep -v "control mode" | tail -4
eval cc -w $F -o "$OUT/test_format_image" "$HERE/test_format_image.c" $G/nhl94_seats.c $G/nhl94_format.c $G/nhl94_builds.c $B/nhl94_roster/nhl94_roster_table.c $G/nhl94_aim.c \
    && "$OUT/test_format_image" 2>&1 | tail -1
cc -I"$G" -o "$OUT/test_tagnames" "$HERE/test_tagnames.c" $G/nhl94_tagnames.c && "$OUT/test_tagnames" | tail -1
python3 "$ROOT/tools/builds/gen_roster.py" --check --rom "$ROM"
