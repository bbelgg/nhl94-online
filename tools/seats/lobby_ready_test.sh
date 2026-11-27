#!/bin/bash
# lobby_ready_test.sh -- the waiting room's explicit Ready and seat Swap, headless
# (3-seat room through a LOCAL lobby server; the engine's lobby selftest drills
# GENESIS_LOBBY_SELFTEST_READY / _SWAP / _ACCEPT_SWAP / _HOLD_MS, see
# genesis_host_lobby.c). docs/DROPIN.md, stage D.
#   EXE=<NHL94Recomp> LOBBY=ws://127.0.0.1:<port> tools/seats/lobby_ready_test.sh
# Expect: ready_late = Play blocked by name, then the match starts; ready_never =
# blocked the whole time (no launch); swap_ok = request with both roles, accepted,
# slots exchanged, everybody re-readied (the server cleared ready), match starts;
# swap_no = declined, nobody moves.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOM="$HERE/../net/room.sh"; export EXE LOBBY OUT_ROOT=${OUT_ROOT:-${TMPDIR:-/tmp}/nhl94-rooms}
: "${EXE:?set EXE}" "${LOBBY:?set LOBBY}"
SEATS="NHL94_SEATS=home:C,away:C,away:G"
rc=0
chk() { if eval "$2"; then echo "  ok   $1"; else echo "  FAIL $1"; rc=1; fi; }
L() { cat "$OUT_ROOT/$1"/*.log; }
echo "== ready_late: guest 2 readies 9 s after joining"
GUEST_ENV_2="GENESIS_LOBBY_SELFTEST_READY=9" "$ROOM" ready_late 3 0 600 >/dev/null 2>&1
chk "host named the unready guest" 'L ready_late | grep -q "Play blocked: guest2 is not ready"'
chk "then the match launched" 'L ready_late | grep -q "host round=1 start=1"'
chk "digests identical" 'grep -q "VERDICT PASS" "$OUT_ROOT/ready_late/summary.txt"'
echo "== ready_never: guest 2 never readies"
GUEST_ENV_2="GENESIS_LOBBY_SELFTEST_READY=0" WALL=45 "$ROOM" ready_never 3 0 600 >/dev/null 2>&1
chk "Play blocked by name" 'L ready_never | grep -q "Play blocked: guest2 is not ready"'
chk "no launch" '! L ready_never | grep -q "host round=1 start=1"'
echo "== swap_ok: guest 1 asks for guest 2's seat (Away G)"
HOST_ENV="GENESIS_LOBBY_SELFTEST_HOLD_MS=9000" EXTRA_ENV="$SEATS" GUEST_ENV_1="GENESIS_LOBBY_SELFTEST_SWAP=2" \
    "$ROOM" swap_ok 3 0 600 >/dev/null 2>&1
chk "request shows both roles" 'L swap_ok | grep -q "swap request from \"guest1\" (Away C) for my Away G -> accept"'
chk "accepted" 'L swap_ok | grep -q "swap ACCEPTED"'
chk "guest 1 now in seat 2, guest 2 in seat 1" '[ "$(grep -h "fill_launch" "$OUT_ROOT/swap_ok/guest1.log" | grep -o "slot=[0-9]")" = "slot=2" ] && [ "$(grep -h "fill_launch" "$OUT_ROOT/swap_ok/guest2.log" | grep -o "slot=[0-9]")" = "slot=1" ]'
chk "ready was cleared by the swap and given again" '[ "$(grep -c "ready (#" "$OUT_ROOT/swap_ok/guest1.log")" -ge 3 ]'
chk "match launched, digests identical" 'grep -q "VERDICT PASS" "$OUT_ROOT/swap_ok/summary.txt"'
echo "== swap_no: the other player keeps the seat"
HOST_ENV="GENESIS_LOBBY_SELFTEST_HOLD_MS=9000" EXTRA_ENV="$SEATS" GUEST_ENV_1="GENESIS_LOBBY_SELFTEST_SWAP=2" \
    GUEST_ENV_2="GENESIS_LOBBY_SELFTEST_ACCEPT_SWAP=0" "$ROOM" swap_no 3 0 600 >/dev/null 2>&1
chk "declined" 'L swap_no | grep -q "swap DECLINED"'
chk "nobody moved" '[ "$(grep -h "fill_launch" "$OUT_ROOT/swap_no/guest1.log" | grep -o "slot=[0-9]")" = "slot=1" ]'
exit $rc
