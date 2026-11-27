#!/bin/bash
# lobby_format_test.sh -- (4v4 variant of lobby_mode_test.sh) 2 processes through the real lobby (the build's
# NHL94_LOBBY_URL) with the engine's lobby selftest: the host's and the
# guest's launcher Control mode (nhl94-control.ini) set independently; checks
# that both run the HOST's mode and stay in sync.
#   tools/seats/lobby_format_test.sh <peer-root> <host-format> <guest-format> [frames]
# Both peers Position lock home:C / away:C; format = 6v6 | 4v4 per peer. The
# guest must adopt the HOST format from the session image (nhl94.seats=4v4,...).
# LAN=1 (default): the engine lobby selftest over LAN discovery on localhost
# (no lobby server); LAN=0 = the build's lobby URL.
# <peer-root>/host and <peer-root>/guest each hold their OWN copy of the exe
# (+assets); the ROM is passed by path, never copied. mode = classic | lock.
set -u
ROOT=$(cd "${1:?peer root}" && pwd); HM=${2:?host format}; GM=${3:?guest format}; FRAMES=${4:-2600}
HERE=$(cd "$(dirname "$0")" && pwd); ROM="$HERE/../../game/NHL94.bin"
LOBBY="nhl94-format-test-$$-$RANDOM"
for p in host guest; do
    m=$HM; [ $p = guest ] && m=$GM
    printf '[control]\nmode = lock\nformat = %s\nseat1 = home:C\nseat2 = away:C\nseat3 = none\nseat4 = none\n' "$m" > "$ROOT/$p/nhl94-control.ini"
    rm -f "$ROOT/$p/NHL94.srm"
done
run() {  # role script
    (cd "$ROOT/$1" && env -u NHL94_SEATS -u NHL94_FORMAT SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 \
        GENESIS_LOBBY_SELFTEST=$1 GENESIS_LOBBY_SELFTEST_LOBBY="$LOBBY" GENESIS_LOBBY_SELFTEST_FRAMES=$FRAMES \
        GENESIS_NET_TIMELINE_EVERY=60 NHL94_SEATS_TRACE=300 NHL94_FORMAT_TRACE=300 \
        GENESIS_LOBBY_SELFTEST_LAN=${LAN:-1} GENESIS_LOBBY_SELFTEST_LAN_PORT=${LAN_PORT:-17791} \
        ./NHL94Recomp "$ROM" --no-launcher --input-script "$2" > "$ROOT/$1.out" 2> "$ROOT/$1.err")
}
run host "$HERE/lobby_host.input" &
sleep 2
run guest "$HERE/lobby_guest.input" &
wait
for p in host guest; do grep '^TIMELINE' "$ROOT/$p.err" | sort -u > "$ROOT/$p.tl"; done
echo "lobby=$LOBBY host=$HM guest=$GM"
for p in host guest; do
    echo "-- $p: $(grep -c . "$ROOT/$p.tl") TIMELINE lines"
    grep "nhl94_mode:\|control mode" "$ROOT/$p.err" | sed 's/ -- seat i.*//'
    grep -m1 "game=" "$ROOT/$p.err"
    grep "^NETPLAY_DRIVER" "$ROOT/$p.err" | sed 's/ rtt_ms.*//'
    grep "nhl94_format: t=" "$ROOT/$p.err" | tail -2 | cut -c1-160
    grep "nhl94_format: summary" "$ROOT/$p.err"
done
shared=$(comm -12 "$ROOT/host.tl" "$ROOT/guest.tl" | wc -l | tr -d ' ')
diff=$(comm -3 <(sed 's/ d=.*//' "$ROOT/host.tl") <(sed 's/ d=.*//' "$ROOT/guest.tl") >/dev/null; \
       join <(sed 's/TIMELINE t=//; s/ d=/ /' "$ROOT/host.tl" | sort) <(sed 's/TIMELINE t=//; s/ d=/ /' "$ROOT/guest.tl" | sort) | awk '$2!=$3' | wc -l | tr -d ' ')
echo "shared TIMELINE ticks identical: $shared, same tick different digest: $diff"
