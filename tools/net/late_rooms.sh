#!/bin/bash
# late_rooms.sh -- join in progress through a LOCAL lobby server (never the
# production one): rooms with open seats, a late peer that joins a running match,
# replays it from tick 0 (no render), claims a seat and plays from the next
# whistle; graded by late_grade.py (every peer's TIMELINE digests identical
# over everything they share, the join events on every peer, nobody refused).
#
#   EXE=<NHL94Recomp> LOBBY=ws://127.0.0.1:<port> tools/net/late_rooms.sh [scenario ...]
#   scenarios: late4 faceoff jdj lag whistle two noroles [oldserver]   (default: late4 faceoff jdj lag)
#     late4     3 players + 1 open seat, a late joiner after ~40 s (tick >600), mid-play
#     faceoff   same, joiner early enough that its seat is claimed before the opening
#               faceoff (the takeover is immediate)
#     jdj       join, the joiner leaves (BYE), a second late peer joins the freed seat
#     lag       late4 with RNET_SIM_LATENCY_MS=60 JITTER=20 LOSS and the forced-mispredict
#               injector on guest1
#     whistle   a mid-play join in a long match: the seat is taken over only at a whistle
#               (--expect-takeover-after: no human-mask growth before the join wire, growth later)
#     two       5 seats / 3 players, TWO late peers at once: the second claim is answered
#               BUSY while the first join is agreed, then takes the other free seat
#     noroles   an open seat past the end of the host's seat list has no role: the claim is
#               refused (NO_SEAT), the late peer keeps watching, nothing else changes
#     oldserver pass an UNCHANGED server's URL: the room must still run, late join unavailable
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
EXE=${EXE:?set EXE}; : "${LOBBY:?set LOBBY (ws://127.0.0.1:port)}"
export EXE LOBBY OUT_ROOT=${OUT_ROOT:-${TMPDIR:-/tmp}/nhl94-rooms}
export HOST_SCRIPT=$HERE/../seats/drop_host.input GUEST_SCRIPT=$HERE/../seats/drop_guest.input
SEATS="home:C,home:LD,away:C,away:LD"
PAD="GENESIS_NET_TEST_PAD=40:20:0008"
scen=("$@"); [ ${#scen[@]} -eq 0 ] && scen=(late4 faceoff jdj lag)
rc=0
run() {  # name frames late_at  [grade args]
    local name=$1 frames=$2 late_at=$3; shift 3
    echo "=== $name"
    LATE_AT=$late_at "$HERE/room.sh" "$name" 3 0 "$frames" >/dev/null 2>&1
    python3 "$HERE/late_grade.py" "$OUT_ROOT/$name" "$@" || rc=1
}
for s in "${scen[@]}"; do case $s in
 late4)   ROOM_SEATS=4 LATE=1 EXTRA_ENV="NHL94_SEATS=$SEATS NHL94_SEATS_TRACE=600" LATE_ENV="$PAD" \
          run late4 5000 40 --late=late1 --expect-seat=3;;
 faceoff) ROOM_SEATS=4 LATE=1 EXTRA_ENV="NHL94_SEATS=$SEATS NHL94_SEATS_TRACE=300" LATE_ENV="$PAD" \
          run faceoff 4200 24 --late=late1 --expect-seat=3 --expect-takeover-by=2400;;
 jdj)     ROOM_SEATS=4 LATE=2 EXTRA_ENV="NHL94_SEATS=$SEATS NHL94_SEATS_TRACE=600" \
          LATE_ENV_1="$PAD GENESIS_NET_TEST_LEAVE=3300" LATE_ENV_2="$PAD" \
          LATE_AT=40 LATE_AT_2=80 run jdj 6500 40 --late=late1,late2 --leaver=late1 --expect-seat=3 --expect-events=4;;
 lag)     ROOM_SEATS=4 LATE=1 EXTRA_ENV="NHL94_SEATS=$SEATS RNET_SIM_LATENCY_MS=60 RNET_SIM_JITTER_MS=20 RNET_SIM_LOSS_PCT=1 RNET_SIM_SEED=7" \
          LATE_ENV="$PAD" run lag 5000 40 --late=late1 --expect-seat=3;;
 whistle) ROOM_SEATS=4 LATE=1 EXTRA_ENV="NHL94_SEATS=$SEATS NHL94_SEATS_TRACE=120" LATE_ENV="$PAD" WALL=420 \
          run whistle 17000 40 --late=late1 --expect-seat=3 --expect-whistle-takeover;;
 two)     ROOM_SEATS=5 LATE=2 EXTRA_ENV="NHL94_SEATS=$SEATS,home:RD NHL94_SEATS_TRACE=600" LATE_ENV="$PAD" \
          run two 5000 40 --late=late1,late2 --expect-seat-set=3,4 --expect-events=4;;
 noroles) ROOM_SEATS=4 LATE=1 EXTRA_ENV="NHL94_SEATS=home:C,home:LD,away:C" LATE_ENV="$PAD" \
          run noroles 3600 40 --late=late1 --expect-refused;;
 oldserver) ROOM_SEATS=4 LATE=1 EXTRA_ENV="NHL94_SEATS=$SEATS" LATE_ENV="$PAD" WALL=170 \
          run oldserver 3000 40 --old-server;;
esac; done
exit $rc
