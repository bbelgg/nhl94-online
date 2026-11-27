#!/bin/bash
# plan_mismatch.sh -- two LAN peers whose seat plan differs (peer 1 is given
# GENESIS_NET_OCCUPIED=3, peer 0 the default 0): the plan is sealed into the
# session config image (genesis_netplay_set_seat_plan, docs/DROPIN.md "Review
# fixes"), so both must refuse with MOD SETS DIFFER and never tick.
#   tools/seats/plan_mismatch.sh <build dir> <out dir> [base port]
set -u
B=$(cd "${1:?build dir}" && pwd); OUT="${2:?out dir}"; PORT=${3:-18808}
HERE=$(cd "$(dirname "$0")" && pwd); ROM="$HERE/../../game/NHL94.bin"
mkdir -p "$OUT"; PIDS=""
for s in 0 1; do
    extra=""; [ $s = 1 ] && extra="GENESIS_NET_OCCUPIED=3"
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 GENESIS_NETPLAY=1 GENESIS_NET_MODE=rollback \
        GENESIS_NET_TRANSPORT=lan GENESIS_NET_SLOTS=2 GENESIS_NET_SLOT=$s $extra \
        GENESIS_NET_BIND=127.0.0.1:$((PORT+s)) GENESIS_NET_PEER=127.0.0.1:$((PORT+1-s)) NHL94_SEATS=home:C,away:C \
        "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark 1500 --input-script "$HERE/net_slot$s.input" \
        > "$OUT/p$s.out" 2> "$OUT/p$s.err" &
    PIDS="$PIDS $!"
done
sleep 40
n=$(grep -h "RB match refused (mod_set_mismatch)" "$OUT/p0.err" "$OUT/p1.err" | wc -l | tr -d ' ')
for p in $PIDS; do kill -9 "$p" 2>/dev/null; done   # only the PIDs started here
if [ "$n" = 2 ]; then echo "plan_mismatch: PASS (both peers refused: mod_set_mismatch)"; else echo "plan_mismatch: FAIL (refusals=$n)"; exit 1; fi
