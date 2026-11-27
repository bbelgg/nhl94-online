#!/bin/bash
# builds_lobby_test.sh -- the builds' lobby signal path (docs/BUILDS.md section "Lobby data path"): 2 processes
# through a REAL lobby server (LAN has no caps / member data), engine lobby selftest. The picks are the
# peers' own nhl94-control.ini (build_skater / build_goalie): the host (seat 1, home:C) picks a skater, the guest
# (seat 2, away:G) picks a goalie. The guest tells the host with rnet_lobby_set_member_data, the host folds
# both into the image it publishes, the guest adopts it: both must latch the same builds and stay in sync.
#   tools/seats/builds_lobby_test.sh <peer-root> [frames] [guest-env...]
# <peer-root>/host and /guest each hold their OWN copy of the exe (+assets); the ROM is passed by path.
# Env: RNET_LOBBY_URL (default ws://127.0.0.1:18741: a local recomp-net-server, never the real one),
#      HOST_PICK / GUEST_PICK ("skater|goalie" ini values),
#      HOST_SEAT=<k>: the host moves to lobby seat k before the guest joins (the guest takes seat 0), so the
#      host is NOT in seat 0 while it stays session slot 0 (host-first plan). With k=1 the host is the
#      away:G seat: give it a goalie pick, the guest a skater pick (see the HOST_SEAT line of docs/BUILDS.md).
#      ROOM_SEATS=3 GUEST_SEAT=2 SEAT3=away:C (with HOST_SEAT=1): the host in seat 1, the guest in seat 2, seat 0
#      (home:C) EMPTY = CPU: no human's pick may land on it (the host's skater pick used to, through the
#      "local seat is 0" fallback).
set -u
ROOT=$(cd "${1:?peer root}" && pwd); FRAMES=${2:-2600}
HERE=$(cd "$(dirname "$0")" && pwd); ROM="$HERE/../../game/NHL94.bin"
LOBBYURL=${RNET_LOBBY_URL:-ws://127.0.0.1:18741}
LOBBY="nhl94-builds-$$-$RANDOM"
HP=${HOST_PICK:-PIT:Mario_Lemieux|default}; GP=${GUEST_PICK:-default|TOR:Felix_Potvin}
for p in host guest; do
    pick=$HP; [ $p = guest ] && pick=$GP
    printf '[control]\nmode = lock\nseat1 = home:C\nseat2 = away:G\nseat3 = %s\nseat4 = none\nbuild_skater = %s\nbuild_goalie = %s\n' \
        "${SEAT3:-none}" "${pick%%|*}" "${pick##*|}" > "$ROOT/$p/nhl94-control.ini"
    rm -f "$ROOT/$p/NHL94.srm"
done
run() {  # role script
    local x=()
    [ -n "${ROOM_SEATS:-}" ] && x+=(GENESIS_LOBBY_SELFTEST_ROOM_SEATS=$ROOM_SEATS)
    [ $1 = host ] && [ -n "${HOST_SEAT:-}" ] && x+=(GENESIS_LOBBY_SELFTEST_HOST_SEAT=$HOST_SEAT)
    [ $1 = guest ] && [ -n "${GUEST_SEAT:-}" ] && x+=(GENESIS_LOBBY_SELFTEST_SEAT=$GUEST_SEAT)
    (cd "$ROOT/$1" && env -u NHL94_SEATS -u NHL94_BUILDS SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 \
        RNET_LOBBY_URL="$LOBBYURL" ${x[@]+"${x[@]}"} GENESIS_LOBBY_SELFTEST=$1 GENESIS_LOBBY_SELFTEST_LOBBY="$LOBBY" \
        GENESIS_LOBBY_SELFTEST_FRAMES=$FRAMES GENESIS_NET_TIMELINE_EVERY=60 NHL94_BUILDS_TRACE=1 \
        GENESIS_LOBBY_SELFTEST_LAN=0 ./NHL94Recomp "$ROM" --no-launcher --input-script "$2" \
        > "$ROOT/$1.out" 2> "$ROOT/$1.err")
}
run host "$HERE/lobby_host.input" &
sleep 2
run guest "$HERE/lobby_guest.input" &
wait
for p in host guest; do grep '^TIMELINE' "$ROOT/$p.err" | sort -u > "$ROOT/$p.tl"; done
echo "lobby=$LOBBY url=$LOBBYURL host=[$HP] guest=[$GP]"
for p in host guest; do
    echo "-- $p: $(grep -c . "$ROOT/$p.tl") TIMELINE lines"
    grep -m1 "nhl94.seats=" "$ROOT/$p.err" | cut -c1-200
    grep "game configuration changed" "$ROOT/$p.err" | head -3
    grep "nhl94_builds: t=.* plays as" "$ROOT/$p.err"
    grep "\\[lobby-selftest\\] $p seat . label" "$ROOT/$p.err"
    grep "^NETPLAY_DRIVER" "$ROOT/$p.err" | sed 's/ rtt_ms.*//'
done
shared=$(comm -12 "$ROOT/host.tl" "$ROOT/guest.tl" | wc -l | tr -d ' ')
diff=$(join <(sed 's/TIMELINE t=//; s/ d=/ /' "$ROOT/host.tl" | sort) <(sed 's/TIMELINE t=//; s/ d=/ /' "$ROOT/guest.tl" | sort) | awk '$2!=$3' | wc -l | tr -d ' ')
echo "shared TIMELINE ticks identical: $shared, same tick different digest: $diff"
# EXPECT="0=none;1=Ed Belfour;2=Mario Lemieux": the build each LOBBY seat must latch on BOTH peers ("none": the seat
# keeps the team default, its "plays as" line says so), and no waiting-room label may ever carry a pick for a seat nobody sits in.
rc=0
if [ -n "${EXPECT:-}" ]; then
    IFS=';' read -r -a want <<<"$EXPECT"
    for p in host guest; do
        for w in "${want[@]}"; do
            seat=${w%%=*}; name=${w#*=}
            got=$(grep "nhl94_builds: t=.* seat $seat .* plays as" "$ROOT/$p.err" | head -1)
            if [ "$name" = none ]; then
                case $got in *"team default)"*) echo "  ok   $p seat $seat: team default";; *) echo "  FAIL $p seat $seat should be the team default: ${got:-no latch}"; rc=1;; esac
            else
                case $got in *"plays as $name "*) echo "  ok   $p seat $seat: $name";; *) echo "  FAIL $p seat $seat should be $name: ${got:-no latch}"; rc=1;; esac
            fi
        done
    done
    [ "$diff" = 0 ] || { echo "  FAIL digests differ"; rc=1; }
    [ $rc = 0 ] && echo "VERDICT PASS" || echo "VERDICT FAIL"
fi
exit $rc
