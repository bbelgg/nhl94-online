#!/bin/bash
# room.sh -- N headless peers in ONE online room through a lobby server
# (GENESIS_LOBBY_SELFTEST), graded from their logs. The ROM is passed by
# absolute path and never copied; each peer runs a private copy of the
# executable (7 MB) so files written beside it are not shared.
#
#   room.sh <name> <players> [spectators] [frames]
# Env: EXE (required), OUT_ROOT (default $TMPDIR/nhl94-rooms), ROM, LOBBY (ws URL; default the build's), EXTRA_ENV="K=V ..." for every peer,
#      HOST_ENV / GUEST_ENV extra env, WALL seconds, MISPREDICT (guest1, 0 off),
#      HOST_SCRIPT / GUEST_SCRIPT input scripts (--input-script) for the host / every guest,
#      GUEST_ENV_<n> extra env for guest <n> only (e.g. GUEST_ENV_2="GENESIS_NET_TEST_LEAVE=3000:crash")
#      ROOM_SEATS=<n> seats the room has (> players leaves open seats; join in progress then lets a
#      late peer in), LATE=<n> late joiners (roles late1..), LATE_AT=<s> seconds after the first
#      peers were spawned (default 45; LATE_AT_<n> per joiner), LATE_ENV / LATE_ENV_<n> extra env for them
set -u
NAME=$1; PLAYERS=$2; SPECT=${3:-0}; FRAMES=${4:-1800}
EXE=${EXE:?set EXE to the NHL94Recomp executable}
HERE=$(cd "$(dirname "$0")" && pwd); ROM=${ROM:-$HERE/../../game/NHL94.bin}
OUT=${OUT_ROOT:-${TMPDIR:-/tmp}/nhl94-rooms}/$NAME
rm -rf "$OUT"; mkdir -p "$OUT"
WALL=${WALL:-$((150 + FRAMES / 30))}
LOBBYNAME="nhlroom$RANDOM"
ROLES=(host); for ((g=1; g<PLAYERS; g++)); do ROLES+=("guest$g"); done
for ((g=1; g<=SPECT; g++)); do ROLES+=("spect$g"); done
for ((g=1; g<=${LATE:-0}; g++)); do ROLES+=("late$g"); done
common=(PATH="$PATH" HOME="$HOME" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1
  GENESIS_NET_TIMELINE_EVERY=60 GENESIS_LOBBY_SELFTEST_ROUNDS=1 GENESIS_LOBBY_SELFTEST_FRAMES=$FRAMES
  GENESIS_LOBBY_SELFTEST_PLAYERS=$PLAYERS GENESIS_LOBBY_SELFTEST_SPECTATORS=$SPECT
  GENESIS_LOBBY_SELFTEST_LOBBY=$LOBBYNAME GENESIS_RUN_DONE=1)
[ -n "${ROOM_SEATS:-}" ] && common+=(GENESIS_LOBBY_SELFTEST_ROOM_SEATS=$ROOM_SEATS)
[ -n "${LOBBY:-}" ] && common+=(RNET_LOBBY_URL=$LOBBY)
read -r -a extra <<<"${EXTRA_ENV:-}"
t0=$SECONDS
PID=()
for i in "${!ROLES[@]}"; do role=${ROLES[$i]}
  if [ "${role#late}" != "$role" ]; then   # join in progress: after the match runs
    la=LATE_AT_${role#late}; la=${!la:-${LATE_AT:-45}}
    while [ $((SECONDS-t0)) -lt "$la" ]; do sleep 1; done
  fi
  mkdir -p "$OUT/$role"; cp "$EXE" "$OUT/$role/"
  [ -d "$(dirname "$EXE")/assets" ] && ln -s "$(dirname "$EXE")/assets" "$OUT/$role/assets"
  case $role in
    host) k=(GENESIS_LOBBY_SELFTEST=host GENESIS_RB_FORCE_MISPREDICT=0); read -r -a r <<<"${HOST_ENV:-}";;
    guest1) k=(GENESIS_LOBBY_SELFTEST=guest GENESIS_RB_FORCE_MISPREDICT=${MISPREDICT:-45}); read -r -a r <<<"${GUEST_ENV:-}";;
    late*) k=(GENESIS_LOBBY_SELFTEST=latejoin GENESIS_RB_FORCE_MISPREDICT=0); read -r -a r <<<"${LATE_ENV:-}";;
    *) k=(GENESIS_LOBBY_SELFTEST=guest GENESIS_RB_FORCE_MISPREDICT=0); read -r -a r <<<"${GUEST_ENV:-}";;
  esac
  sc=""; [ "$role" = host ] && sc=${HOST_SCRIPT:-} || sc=${GUEST_SCRIPT:-}
  [ "${role#spect}" != "$role" ] && sc=""
  pe=GUEST_ENV_${role#guest}; [ "${role#guest}" != "$role" ] && read -r -a r2 <<<"${!pe:-}" || r2=()
  pl=LATE_ENV_${role#late}; [ "${role#late}" != "$role" ] && read -r -a r2 <<<"${!pl:-}"
  (cd "$OUT/$role" && exec env -i "${common[@]}" ${extra[@]+"${extra[@]}"} "${k[@]}" ${r[@]+"${r[@]}"} ${r2[@]+"${r2[@]}"} \
     GENESIS_LOBBY_SELFTEST_NAME="$role" ./NHL94Recomp "$ROM" ${sc:+--input-script "$sc"}) >"$OUT/$role.log" 2>&1 &
  PID[$i]=$!
  [ "$role" = host ] && sleep 2
  [ "$role" = "guest$((PLAYERS-1))" ] && [ "$SPECT" -gt 0 ] && sleep 5
  sleep 0.3
done
killed=""
alive() { for p in "${PID[@]}"; do kill -0 "$p" 2>/dev/null && return 0; done; return 1; }
while alive; do
  if [ $((SECONDS-t0)) -ge "$WALL" ]; then
    for i in "${!ROLES[@]}"; do kill -0 "${PID[$i]}" 2>/dev/null && { killed="$killed ${ROLES[$i]}"; kill -9 "${PID[$i]}"; }; done; break
  fi; sleep 1
done
for p in "${PID[@]}"; do wait "$p" 2>/dev/null; done
for role in "${ROLES[@]}"; do rm -f "$OUT/$role/NHL94Recomp"; done
echo "took=$((SECONDS-t0))s killed=[$killed]" > "$OUT/summary.txt"
python3 "$HERE/grade.py" "$OUT" "${ROLES[@]}" | tee -a "$OUT/summary.txt"
