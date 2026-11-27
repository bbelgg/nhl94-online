#!/bin/bash
# drop_rooms.sh -- drop-in / drop-out battery: N-seat online rooms through a
# LOCAL lobby server where a guest leaves mid-match (graceful BYE or SIGKILL),
# graded by drop_grade.py: the survivors' TIMELINE digests identical over the
# whole shared range, the game goes on, an occupancy event on every survivor.
#
#   EXE=<NHL94Recomp> LOBBY=ws://127.0.0.1:<port> tools/net/drop_rooms.sh [scenario ...]
#   scenarios: grace4 crash4 crash3 lag4 two6 spect host2 seats2 lone3 (default: all but lone3)
#     lone3: 3 seats, guest 2 leaves, then guest 1 (the last remote human) crashes: the host's lobby
#            link is fine, so the host drops it to the CPU and plays on alone (without that evidence
#            it ends the match: "Lost the connection to every player")
# Start the lobby yourself (recomp-net-server, BIND_ADDR=127.0.0.1:<port>, never
# the production one). OUT_ROOT = where the logs go.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
EXE=${EXE:?set EXE}; : "${LOBBY:?set LOBBY (ws://127.0.0.1:port)}"
export EXE LOBBY OUT_ROOT=${OUT_ROOT:-${TMPDIR:-/tmp}/nhl94-rooms}
export HOST_SCRIPT=$HERE/../seats/drop_host.input GUEST_SCRIPT=$HERE/../seats/drop_guest.input
SEATS="home:C,home:LD,away:C,away:G,home:RD,away:LD"
scen=("$@"); [ ${#scen[@]} -eq 0 ] && scen=(grace4 crash4 crash3 lag4 two6 spect host2 seats2)
rc=0
run() {  # name players spect frames survivors leavers [grade args]
    local name=$1 players=$2 spect=$3 frames=$4 surv=$5 leav=$6; shift 6
    echo "=== $name"
    tools_room="$HERE/room.sh"
    "$tools_room" "$name" "$players" "$spect" "$frames" >/dev/null 2>&1
    python3 "$HERE/drop_grade.py" "$OUT_ROOT/$name" "$leav" "$surv" "$@" || rc=1
}
for s in "${scen[@]}"; do case $s in
 grace4) EXTRA_ENV="NHL94_SEATS=$SEATS NHL94_SEATS_TRACE=600" GUEST_ENV_2="GENESIS_NET_TEST_LEAVE=3300" \
         run grace4 4 0 5500 host,guest1,guest3 guest2;;
 crash4) EXTRA_ENV="NHL94_SEATS=$SEATS NHL94_SEATS_TRACE=600" GUEST_ENV_2="GENESIS_NET_TEST_LEAVE=3300:crash" \
         run crash4 4 0 5500 host,guest1,guest3 guest2;;
 crash3) EXTRA_ENV="NHL94_SEATS=$SEATS" GUEST_ENV_1="GENESIS_NET_TEST_LEAVE=3000:crash" \
         run crash3 3 0 5000 host,guest2 guest1;;
 lag4)   EXTRA_ENV="NHL94_SEATS=$SEATS RNET_SIM_LATENCY_MS=60 RNET_SIM_JITTER_MS=20 RNET_SIM_LOSS_PCT=1 RNET_SIM_SEED=7" \
         GUEST_ENV_3="GENESIS_NET_TEST_LEAVE=3300:crash" run lag4 4 0 5500 host,guest1,guest2 guest3;;
 two6)   EXTRA_ENV="NHL94_SEATS=$SEATS" GUEST_ENV_2="GENESIS_NET_TEST_LEAVE=3000" GUEST_ENV_4="GENESIS_NET_TEST_LEAVE=3600:crash" \
         run two6 6 0 5500 host,guest1,guest3,guest5 guest2,guest4;;
 spect)  EXTRA_ENV="NHL94_SEATS=$SEATS" GUEST_ENV_2="GENESIS_NET_TEST_LEAVE=3300:crash" \
         run spect 4 1 5500 host,guest1,guest3,spect1 guest2;;
 lone3)  EXTRA_ENV="NHL94_SEATS=$SEATS" GUEST_ENV_2="GENESIS_NET_TEST_LEAVE=3000" GUEST_ENV_1="GENESIS_NET_TEST_LEAVE=4200:crash" \
         run lone3 3 0 6500 host guest1,guest2;;
 host2)  # negative control: the HOST leaving still ends the match for everyone
         EXTRA_ENV="NHL94_SEATS=$SEATS" HOST_ENV="GENESIS_NET_TEST_LEAVE=3300" \
         run host2 4 0 5500 guest1,guest2,guest3 host --ends=guest1,guest2,guest3;;
 seats2) # negative control: a 2-seat room is unchanged (a guest leaving ends the match)
         EXTRA_ENV="NHL94_SEATS=home:C,away:C" GUEST_ENV_1="GENESIS_NET_TEST_LEAVE=3300" \
         run seats2 2 0 5000 host guest1 --ends=host;;
esac; done
exit $rc
