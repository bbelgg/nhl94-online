#!/usr/bin/env bash
#
# rb_probe_seats.sh -- segagenesisrecomp/tools/rb_probe.sh for the player lock,
# WITHOUT copying the ROM: the executable runs in its build directory and the
# ROM is passed by absolute path (rb_probe.sh copies both into its output
# directory; this project's rule is that NHL94.bin is never copied). Same
# probe (GENESIS_RB_PROBE=<period:depth>), same cleared environment, same
# grading (passes > 0, no divergence, no asymmetric restore, no dispatch
# misses).
#
#   tools/seats/rb_probe_seats.sh <exe> <rom> [frames] [period:depth] [scenario] [-- exe args]
#
#   scenario: gameplay  rb_probe.sh's own: 12 START taps, then HOLD RIGHT + C
#             <file>    an input script (e.g. tools/seats/lock4.input)
#
# Env: RB_PROBE_OUT (default ./rb_probe_out) for logs and the scenario;
#      RB_PROBE_ENV="K=V ..." extra variables for the cleared environment
#      (e.g. the test-only NHL94_FORMAT_TEST_PENALTY, docs/FOUR_ON_FOUR_PLAN.md).
# Seat config goes on the command line (the environment is cleared), e.g.
#   tools/seats/rb_probe_seats.sh build/NHL94Recomp game/NHL94.bin 3000 30:8 \
#       gameplay -- --seats home:C,away:G
set -u
EXE="${1:?usage: rb_probe_seats.sh <exe> <rom> [frames] [period:depth] [scenario] [-- args]}"
ROM="${2:?usage}"
FRAMES="${3:-3000}"
PD="${4:-30:8}"
SCEN="${5:-gameplay}"
shift 5 2>/dev/null || shift $#
[ "${1:-}" = "--" ] && shift
OUT="${RB_PROBE_OUT:-$PWD/rb_probe_out}"
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
EXEDIR=$(dirname "$EXE")
rm -rf "$OUT"; mkdir -p "$OUT"
script="$OUT/scenario.input"
case "$SCEN" in
gameplay)
    {
        echo "# rb_probe.sh scenario: gameplay"
        echo "WAIT 240"
        for _ in $(seq 12); do echo "PRESS START 4"; echo "WAIT 150"; done
        echo "HOLD RIGHT"
        for _ in $(seq 60); do echo "PRESS C 12"; echo "WAIT 90"; done
    } > "$script" ;;
*)  [ -f "$SCEN" ] || { echo "rb_probe_seats: no scenario $SCEN" >&2; exit 2; }
    cp "$SCEN" "$script" ;;   # the scenario's own WRITE_RAM lines are inputs of the test
esac
# dispatch_misses.toml is written next to the executable (it accumulates per
# build): start from none so only this run is graded.
rm -f "$EXEDIR/NHL94.srm" "$EXEDIR/dispatch_misses.toml"
cd "$OUT" || exit 2
env -i PATH="$PATH" HOME="$HOME" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    GENESIS_NO_LAUNCHER=1 GENESIS_RB_PROBE="$PD" ${RB_PROBE_ENV:-} \
    "$EXE" "$ROM" --benchmark "$FRAMES" --input-script "$script" "$@" \
    > stdout.log 2> stderr.log
rc=$?
summary=$(grep -m1 'rb_probe: summary' stderr.log)
passes=$(echo "$summary" | grep -oE 'passes=[0-9]+' | cut -d= -f2)
div=$(echo "$summary" | grep -oE 'divergences=[0-9]+' | cut -d= -f2)
asym=$(echo "$summary" | grep -oE 'asymmetric=[0-9]+' | cut -d= -f2)
misses=0
for f in "$EXEDIR/dispatch_misses.toml"; do
    [ -f "$f" ] || continue
    misses=$(python3 - "$f" <<'PY'
import re,sys
t=open(sys.argv[1]).read()
m=re.search(r"extra\s*=\s*\[(.*?)\]",t,re.S)
print(0 if not m else len([x for x in re.split(r"[,\n]",m[1]) if x.strip() and not x.strip().startswith("#")]))
PY
)
done
echo "exe=$(basename "$EXE") scenario=$SCEN frames=$FRAMES probe=$PD args=$* rc=$rc"
echo "summary: ${summary:-<none>}"
grep -m5 'rb_probe: DIVERGE\|rb_probe: ASYMMETRIC' stderr.log
grep -m2 'nhl94_seats: [0-9]* seat\|nhl94_seats: summary' stderr.log
echo "dispatch_misses extra: $misses"
if [ "$rc" -eq 0 ] && [ -n "$passes" ] && [ "$passes" -gt 0 ] && [ "$div" = 0 ] && [ "$asym" = 0 ] \
   && [ "$misses" = 0 ]; then
    echo "PASS"; exit 0
fi
echo "FAIL (logs in $OUT)"; exit 1
