#!/bin/bash
# latejoin_hub.sh -- join in progress over the LAN hub (no lobby server):
# seats 0..2 play, seat 3 is OPEN at launch (GENESIS_NET_OPEN=8), a spectator
# process starts JOIN_AT_S seconds later (wall clock), replays the match from
# tick 0 without rendering, claims the open seat and plays from the next whistle.
#
#   EXE=<NHL94Recomp> OUT=<dir> tools/net/latejoin_hub.sh
# Env: FRAMES (host/guest frame budget, default 7200), JOIN_AT_S (default 40),
#      PORT (hub base port, default 47700), EXTRA="K=V ..." for every peer,
#      LATE_EXTRA for the joiner only, LEAVE_LATE=<tick>[:crash] to make the
#      joiner leave again (join + drop), SECOND_JOIN_AT_S (a second joiner
#      process after the first left), SIM_NET=1 for 60 ms / 20 ms jitter + the
#      forced-mispredict injector on guest 1.
set -u
EXE=${EXE:?set EXE}; OUT=${OUT:?set OUT}
HERE=$(cd "$(dirname "$0")" && pwd); ROM=${ROM:-$HERE/../../game/NHL94.bin}
FRAMES=${FRAMES:-7200}; JOIN_AT_S=${JOIN_AT_S:-40}; PORT=${PORT:-47700}
SEATS=${SEATS:-home:C,home:LD,away:C,away:LD}
SID=${SID:-$((RANDOM + 100))}
rm -rf "$OUT"; mkdir -p "$OUT"
common=(SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 GENESIS_NETPLAY=1
  GENESIS_NET_TRANSPORT=hub GENESIS_NET_SLOTS=4 GENESIS_NET_SESSION_ID=$SID GENESIS_NET_TIMELINE_EVERY=60
  GENESIS_NET_LATE_JOIN=1 GENESIS_NET_OPEN=8 NHL94_SEATS="$SEATS" NHL94_SEATS_TRACE=600)
[ "${SIM_NET:-0}" = 1 ] && common+=(RNET_SIM_LATENCY_MS=60 RNET_SIM_JITTER_MS=20 RNET_SIM_SEED=7)
read -r -a extra <<<"${EXTRA:-}"
PIDS=()
launch() { # name slot args...
  local name=$1 slot=$2; shift 2
  mkdir -p "$OUT/$name"; cp "$EXE" "$OUT/$name/"
  [ -d "$(dirname "$EXE")/assets" ] && ln -sf "$(dirname "$EXE")/assets" "$OUT/$name/assets"
  (cd "$OUT/$name" && exec env -i PATH="$PATH" HOME="$HOME" "${common[@]}" ${extra[@]+"${extra[@]}"} "$@" \
     ./NHL94Recomp "$ROM" --no-launcher --max-frames "$FRAMES" ${SCRIPT:+--input-script "$SCRIPT"}) > "$OUT/$name.log" 2>&1 &
  PIDS+=($!)
  echo "$name pid=$!"
}
SCRIPT=$HERE/../seats/drop_host.input launch host 0 GENESIS_NET_SLOT=0 GENESIS_NET_BIND=127.0.0.1:$PORT GENESIS_RB_FORCE_MISPREDICT=0
sleep 1
SCRIPT=$HERE/../seats/drop_guest.input launch guest1 1 GENESIS_NET_SLOT=1 GENESIS_NET_BIND=127.0.0.1:$((PORT+1)) GENESIS_NET_PEER=127.0.0.1:$PORT \
   GENESIS_RB_FORCE_MISPREDICT=${MISPREDICT:-0}
SCRIPT=$HERE/../seats/drop_guest.input launch guest2 2 GENESIS_NET_SLOT=2 GENESIS_NET_BIND=127.0.0.1:$((PORT+2)) GENESIS_NET_PEER=127.0.0.1:$PORT
sleep "$JOIN_AT_S"
SCRIPT= launch late 4 GENESIS_NET_SPECTATOR=1 GENESIS_NET_SPECTATOR_WIRE_SLOT=4 GENESIS_NET_LATE_LAUNCH=1 \
   GENESIS_NET_BIND=127.0.0.1:$((PORT+4)) GENESIS_NET_PEER=127.0.0.1:$PORT GENESIS_NET_TEST_PAD=${LATE_PAD:-40:20:0008} \
   ${LEAVE_LATE:+GENESIS_NET_TEST_LEAVE=$LEAVE_LATE} ${LATE_EXTRA:-}
for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null; done
echo done
