#!/bin/bash
# lobby_mode_test.sh -- 2 processes through the real lobby (the build's
# NHL94_LOBBY_URL) with the engine's lobby selftest: the host's and the
# guest's launcher Control mode (nhl94-control.ini) set independently; checks
# that both run the HOST's mode and stay in sync.
#   tools/seats/lobby_mode_test.sh <peer-root> <host-mode> <guest-mode> [frames]
# <peer-root>/host and <peer-root>/guest each hold their OWN copy of the exe
# (+assets); the ROM is passed by path, never copied. mode = classic | lock.
set -u
ROOT=$(cd "${1:?peer root}" && pwd); HM=${2:?host mode}; GM=${3:?guest mode}; FRAMES=${4:-2600}
HERE=$(cd "$(dirname "$0")" && pwd); ROM="$HERE/../../game/NHL94.bin"
LOBBY="nhl94-mode-test-$$-$RANDOM"
for p in host guest; do
    m=$HM; [ $p = guest ] && m=$GM
    printf '[control]\nmode = %s\nseat1 = home:C\nseat2 = away:C\nseat3 = none\nseat4 = none\n' "$m" > "$ROOT/$p/nhl94-control.ini"
    rm -f "$ROOT/$p/NHL94.srm"
done
run() {  # role script
    (cd "$ROOT/$1" && env -u NHL94_SEATS SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 \
        GENESIS_LOBBY_SELFTEST=$1 GENESIS_LOBBY_SELFTEST_LOBBY="$LOBBY" GENESIS_LOBBY_SELFTEST_FRAMES=$FRAMES \
        GENESIS_NET_TIMELINE_EVERY=60 NHL94_SEATS_TRACE=300 \
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
    grep "nhl94_seats: t=" "$ROOT/$p.err" | tail -1 | grep -o "t=[0-9]* .* joy=[0-9A-F]*" | sed 's/ roles=.*//'
done
shared=$(comm -12 "$ROOT/host.tl" "$ROOT/guest.tl" | wc -l | tr -d ' ')
diff=$(comm -3 <(sed 's/ d=.*//' "$ROOT/host.tl") <(sed 's/ d=.*//' "$ROOT/guest.tl") >/dev/null; \
       join <(sed 's/TIMELINE t=//; s/ d=/ /' "$ROOT/host.tl" | sort) <(sed 's/TIMELINE t=//; s/ d=/ /' "$ROOT/guest.tl" | sort) | awk '$2!=$3' | wc -l | tr -d ' ')
echo "shared TIMELINE ticks identical: $shared, same tick different digest: $diff"
