#!/bin/bash
# sixview_localhost.sh -- N processes of NHL94Recomp in ONE rollback-netplay
# session on 127.0.0.1 (default: 2 player seats + 4 spectators over the LAN
# hub), each capturing tick-aligned screenshots at the same ticks; checks that
# every process's confirmed TIMELINE digests agree, then builds a labelled 3x2
# grid PNG per tick and an MP4 (tools/sixview_composite.py).
#
# Runs under macOS /bin/bash 3.2 (no associative arrays, no mapfile, no wait -n).
# See tools/SIXVIEW.md for what was verified and the limits.
#
# Usage:  tools/sixview_localhost.sh            (all settings via env, below)
#         tools/sixview_localhost.sh --help
#
# Settings (environment):
#   SIXVIEW_EXE=path        game binary (default: the stable build below)
#   SIXVIEW_ROM=path        ROM (default: <repo>/game/NHL94.bin; never copied)
#   SIXVIEW_OUT=dir         output dir (default: $HOME/nhl94-online-build/
#                           harness/sixview_<timestamp>)
#   SIXVIEW_MODE=procs      procs (N processes, one netplay session) | single (ONE
#                           process rendering GENESIS_VIEWS views itself; see below)
#   SIXVIEW_SEATS=2         player seats (1 process each; seat 0 is the hub)
#   SIXVIEW_SPECTATORS=4    spectator processes (wire slots SEATS..SEATS+n-1, <= 7)
#   SIXVIEW_BASE_PORT=47300 process i binds 127.0.0.1:BASE_PORT+i
#   SIXVIEW_FRAMES=5000     frames each process runs in the FIRST pass (--benchmark N,
#                           uncapped; sim ticks ~= frames). Later passes stop just
#                           after their last screenshot tick.
#   SIXVIEW_SHOT_TICKS=     explicit comma list of screenshot ticks, or else
#   SIXVIEW_SHOT_FROM=1800 SIXVIEW_SHOT_TO=4800 SIXVIEW_SHOT_EVERY=60
#                           The engine takes at most 16 ticks per process, so the
#                           list is split into passes of 16; every pass re-runs the
#                           whole (deterministic) session; passes are cross-checked.
#   SIXVIEW_SCRIPT=demo     seat 0's input script: demo (menu path to Players=Demo,
#                           CPU vs CPU, on ice ~t1600) | none (no input: attract
#                           demo, on ice only from ~t32400) | <path to a script>
#   SIXVIEW_VIEWS=home:C,away:C,home:G,away:G,home:LW,away:RW
#                           NHL94_VIEW for process 0,1,2,... (passed through as is)
#   SIXVIEW_LATENCY_MS= SIXVIEW_JITTER_MS= SIXVIEW_LOSS_PCT= SIXVIEW_SIM_SEED=
#                           -> RNET_SIM_LATENCY_MS / _JITTER_MS / _LOSS_PCT / _SEED
#   SIXVIEW_TIMELINE_EVERY=60   GENESIS_NET_TIMELINE_EVERY
#   SIXVIEW_PASS_TIMEOUT=300    seconds before a pass is killed
#   SIXVIEW_ORDER=staged    launch order: staged (hub, spectators, other seats; waits
#                           for each) | together (all at once) | random (shuffled,
#                           SIXVIEW_ORDER_GAP_MS=300 max gap) | late (seats first,
#                           spectators once seat 0 confirmed SIXVIEW_LATE_TICK=600)
#   SIXVIEW_ROSTER=1        1 = seat 0 gets GENESIS_NET_SPECTATORS=<spectators> (START
#                           waits for them, bounded); 0 = START on seats alone
#                           (the default for ORDER=late)
#   SIXVIEW_EXTRA_ENV="A=1 B=2" extra env for every process (word-split)
#   SIXVIEW_PROC_VIEWS=0    1 = each process renders its own NHL94_VIEW view and
#                           the grid is built from those (OUT/pviews, OUT/pviews_grid)
#   SIXVIEW_COMPOSITE=1     0 = skip the grid/MP4 step
#   SIXVIEW_FPS=10          MP4 frames per second (one frame per screenshot tick)
#   SIXVIEW_KEEP_RAW=1      0 = delete the raw (uncompressed, ~215 KB) engine PNGs
#                           after compositing
#   Single mode only: SIXVIEW_VIEW_COUNT=6 (GENESIS_VIEWS), SIXVIEW_VIEW_EVERY=60
#                           (GENESIS_VIEW_SCREENSHOT_EVERY); writes into OUT/views/.
#
# Exit status: 0 = every process reached the end, confirmed TIMELINE digests
# agree on all shared ticks (and across passes), every screenshot was taken.

usage() { sed -n '2,59p' "$0" | sed 's/^# \{0,1\}//'; }
case "${1:-}" in -h|--help) usage; exit 0 ;; esac

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
EXE="${SIXVIEW_EXE:-$HOME/nhl94-online-build/stable/NHL94Recomp}"
ROM="${SIXVIEW_ROM:-$REPO/game/NHL94.bin}"
OUT="${SIXVIEW_OUT:-$HOME/nhl94-online-build/harness/sixview_$(date +%Y%m%d_%H%M%S)}"
MODE="${SIXVIEW_MODE:-procs}"
SEATS="${SIXVIEW_SEATS:-2}"
SPECT="${SIXVIEW_SPECTATORS:-4}"
BASE_PORT="${SIXVIEW_BASE_PORT:-47300}"
FRAMES="${SIXVIEW_FRAMES:-5000}"
SHOT_FROM="${SIXVIEW_SHOT_FROM:-1800}"
SHOT_TO="${SIXVIEW_SHOT_TO:-4800}"
SHOT_EVERY="${SIXVIEW_SHOT_EVERY:-60}"
SCRIPT="${SIXVIEW_SCRIPT:-demo}"
VIEWS="${SIXVIEW_VIEWS:-home:C,away:C,home:G,away:G,home:LW,away:RW}"
TL_EVERY="${SIXVIEW_TIMELINE_EVERY:-60}"
PASS_TIMEOUT="${SIXVIEW_PASS_TIMEOUT:-300}"
ORDER="${SIXVIEW_ORDER:-staged}"
ROSTER="${SIXVIEW_ROSTER:-1}"; [ "$ORDER" = late ] && ROSTER="${SIXVIEW_ROSTER:-0}"
ORDER_GAP_MS="${SIXVIEW_ORDER_GAP_MS:-300}"
LATE_TICK="${SIXVIEW_LATE_TICK:-600}"
FPS="${SIXVIEW_FPS:-10}"
FFMPEG="${FFMPEG:-$(command -v ffmpeg || echo /opt/homebrew/bin/ffmpeg)}"
PY="${PYTHON:-python3}"

die() { echo "sixview: $*" >&2; exit 2; }
[ -x "$EXE" ] || die "no executable at $EXE (SIXVIEW_EXE)"
[ -f "$ROM" ] || die "no ROM at $ROM (SIXVIEW_ROM)"
case "$MODE" in procs|single) ;; *) die "SIXVIEW_MODE must be procs or single" ;; esac
case "$ORDER" in staged|together|random|late) ;; *) die "SIXVIEW_ORDER must be staged, together, random or late" ;; esac
NPROC=$((SEATS + SPECT))
if [ "$MODE" = procs ]; then
    [ "$SEATS" -ge 1 ] && [ "$SEATS" -le 6 ] || die "SIXVIEW_SEATS must be 1..6 (GENESIS_SIM_MAX_PLAYERS)"
    [ "$NPROC" -ge 2 ] || die "need at least 2 processes"
    [ "$NPROC" -le 8 ] || die "seats + spectators must be <= 8 (the hub learns wire slots 0..7)"
fi
mkdir -p "$OUT/logs" "$OUT/shots" || die "cannot create $OUT"

# ------------------------------------------------------------------ processes
PIDS=""
cleanup() {
    local p
    [ -n "$PIDS" ] || return 0
    for p in $PIDS; do kill -TERM "$p" 2>/dev/null; done
    sleep 1
    for p in $PIDS; do kill -KILL "$p" 2>/dev/null; done
    PIDS=""
}
trap 'cleanup; srm_restore' EXIT
trap 'echo "sixview: interrupted" >&2; cleanup; exit 130' INT TERM HUP

# ------------------------------------------------------------------ views/labels
VIEW_LIST=()
OLDIFS=$IFS; IFS=','; for v in $VIEWS; do VIEW_LIST+=("$v"); done; IFS=$OLDIFS
view_of() { # index -> team:role (validated; the game reads NHL94_VIEW later)
    local v="${VIEW_LIST[$1]:-}"
    case "$v" in
        home:G|home:C|home:LW|home:RW|home:LD|home:RD|away:G|away:C|away:LW|away:RW|away:LD|away:RD) echo "$v" ;;
        "") echo "" ;;
        *) echo "sixview: warning: NHL94_VIEW '$v' is not <home|away>:<G|C|LW|RW|LD|RD>; passed as is" >&2; echo "$v" ;;
    esac
}
upper() { echo "$1" | tr '[:lower:]' '[:upper:]'; }

# ------------------------------------------------------------------ input script
case "$SCRIPT" in
    none|"") SCRIPT_FILE="" ;;
    demo)
        # Verified 2026-11-21 on the stable build: title -> setup menu, Players row
        # (2nd) cycled RIGHT x4: One-Home, One-Visitor, Two-Teammates,
        # Two-Head to Head, Demo (Goalies switch to Auto Control), then START
        # through matchups and the intermission's "Start Game": faceoff on ice at
        # ~t1600, clock running from ~t1850, CPU vs CPU (LA at MTL).
        SCRIPT_FILE="$OUT/demo_menu.txt"
        cat > "$SCRIPT_FILE" <<'EOS'
# sixview: menu path to Players = Demo (CPU vs CPU, goalies auto)
WAIT 600
PRESS START 5
WAIT 120
PRESS START 5
WAIT 120
PRESS DOWN 5
WAIT 30
PRESS RIGHT 5
WAIT 30
PRESS RIGHT 5
WAIT 30
PRESS RIGHT 5
WAIT 30
PRESS RIGHT 5
WAIT 60
PRESS START 5
WAIT 240
PRESS START 5
WAIT 240
PRESS START 5
EOS
        ;;
    *) [ -f "$SCRIPT" ] || die "SIXVIEW_SCRIPT: no file $SCRIPT"; SCRIPT_FILE="$SCRIPT" ;;
esac

# ------------------------------------------------------------------ shot ticks
if [ -n "${SIXVIEW_SHOT_TICKS:-}" ]; then
    ALL_TICKS=$(echo "$SIXVIEW_SHOT_TICKS" | tr ',' '\n' | sed '/^$/d' | sort -n | uniq | tr '\n' ' ')
else
    ALL_TICKS=$(seq "$SHOT_FROM" "$SHOT_EVERY" "$SHOT_TO" | tr '\n' ' ')
fi
PASSES=()    # each element: comma list of <= 16 ticks
chunk=""; n=0
for t in $ALL_TICKS; do
    chunk="${chunk:+$chunk,}$t"; n=$((n + 1))
    if [ "$n" -eq 16 ]; then PASSES+=("$chunk"); chunk=""; n=0; fi
done
[ -n "$chunk" ] && PASSES+=("$chunk")

common_env() { # env words every process gets
    echo "SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1"
    [ -n "${SIXVIEW_LATENCY_MS:-}" ] && echo "RNET_SIM_LATENCY_MS=$SIXVIEW_LATENCY_MS"
    [ -n "${SIXVIEW_JITTER_MS:-}" ] && echo "RNET_SIM_JITTER_MS=$SIXVIEW_JITTER_MS"
    [ -n "${SIXVIEW_LOSS_PCT:-}" ] && echo "RNET_SIM_LOSS_PCT=$SIXVIEW_LOSS_PCT"
    [ -n "${SIXVIEW_SIM_SEED:-}" ] && echo "RNET_SIM_SEED=$SIXVIEW_SIM_SEED"
    [ -n "${SIXVIEW_EXTRA_ENV:-}" ] && echo "$SIXVIEW_EXTRA_ENV"
    return 0
}

# Process table (index-aligned arrays; bash 3.2 has no associative arrays).
NAMES=(); ROLES=(); LABELS=(); VIEWV=()
for ((i = 0; i < NPROC; i++)); do
    v=$(view_of "$i"); team=${v%%:*}; role=${v#*:}
    if [ "$i" -lt "$SEATS" ]; then r="seat$i"; else r="spec$i"; fi
    nm="p${i}_${r}"; [ -n "$v" ] && nm="${nm}_${team}_${role}"
    NAMES+=("$nm"); ROLES+=("$r"); VIEWV+=("$v")
    if [ -n "$v" ]; then LABELS+=("$(upper "$team") $(upper "$role") $r"); else LABELS+=("$(upper "$r")"); fi
done

wait_for_line() { # file pattern timeout_s pid -> 0 when seen
    local f=$1 pat=$2 to=$3 pid=$4 t=0
    while [ "$t" -lt $((to * 10)) ]; do
        grep -q "$pat" "$f" 2>/dev/null && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.1; t=$((t + 1))
    done
    return 1
}

# SIXVIEW_PROC_VIEWS=1: every process renders its OWN view (NHL94_VIEW, one
# speculative view; needs a build with runner/multiview + game/nhl94_view) and
# dumps it every SHOT_EVERY ticks into OUT/pviews/<name>/. The grid is then
# built from those views instead of the main screens (which stay identical and
# are still used for the cross-process check above).
PROC_VIEWS="${SIXVIEW_PROC_VIEWS:-0}"
proc_view_env() { # index -> env words (empty unless PROC_VIEWS=1)
    [ "$PROC_VIEWS" = 1 ] || return 0
    mkdir -p "$OUT/pviews/${NAMES[$1]}"
    echo "GENESIS_VIEW_SCREENSHOT_EVERY=$SHOT_EVERY GENESIS_VIEW_SCREENSHOT_DIR=$OUT/pviews/${NAMES[$1]}"
}

launch() { # index pass_dir frames ticks session_id
    local i=$1 pdir=$2 frames=$3 ticks=$4 sid=$5 peer bind extra
    bind="127.0.0.1:$((BASE_PORT + i))"
    peer="127.0.0.1:$BASE_PORT"; [ "$i" -eq 0 ] && peer=""
    if [ "$i" -lt "$SEATS" ]; then extra="GENESIS_NET_SLOT=$i"
    else extra="GENESIS_NET_SPECTATOR=1 GENESIS_NET_SPECTATOR_WIRE_SLOT=$i"; fi
    # The roster: seat 0 holds START (bounded) until this many spectators sent
    # READY, as the lobby does for its room.
    [ "$i" -eq 0 ] && [ "$ROSTER" = 1 ] && extra="$extra GENESIS_NET_SPECTATORS=$SPECT"
    local args=("$ROM" --no-launcher --benchmark "$frames")
    [ "$i" -eq 0 ] && [ -n "$SCRIPT_FILE" ] && args+=(--input-script "$SCRIPT_FILE")
    # shellcheck disable=SC2046
    env $(common_env) GENESIS_NETPLAY=1 GENESIS_NET_TRANSPORT=hub GENESIS_NET_SLOTS="$SEATS" \
        GENESIS_NET_SESSION_ID="$sid" GENESIS_NET_TIMELINE_EVERY="$TL_EVERY" \
        GENESIS_NET_BIND="$bind" GENESIS_NET_PEER="$peer" $extra \
        NHL94_VIEW="${VIEWV[$i]}" \
        $(proc_view_env "$i") \
        GENESIS_SCREENSHOT_AT_TICK="$ticks" \
        GENESIS_SCREENSHOT_AT_EXIT="$OUT/shots/${NAMES[$i]}.png" \
        "$EXE" "${args[@]}" > "$pdir/${NAMES[$i]}.log" 2>&1 &
    PIDS="$PIDS $!"
    eval "PID_$i=$!"
}

# Battery save (<rom-basename>.srm next to the exe). A pass that reaches the
# game writes it, and the next pass would boot from it: a different machine
# state, so passes would no longer replay the same game. Set any existing save
# aside once (restored at exit) and start every pass without one.
SRM="$(dirname "$EXE")/$(basename "$ROM" .bin).srm"
SRM_SAVED=""
srm_set_aside() {
    if [ -f "$SRM" ] && [ -z "$SRM_SAVED" ]; then
        SRM_SAVED="$OUT/$(basename "$SRM").before-sixview"
        mv "$SRM" "$SRM_SAVED" && echo "sixview: set aside existing save $SRM -> $SRM_SAVED (restored at exit)"
    fi
}
srm_restore() { # put the user's save back; otherwise drop the one the passes wrote
    [ -n "${SRM:-}" ] || return 0
    if [ -n "$SRM_SAVED" ] && [ -f "$SRM_SAVED" ]; then mv -f "$SRM_SAVED" "$SRM"
    else rm -f "$SRM"; fi
    return 0
}

run_pass() { # pass_no ticks frames -> 0 ok
    local k=$1 ticks=$2 frames=$3 pdir="$OUT/logs/pass$1" sid i p deadline alive
    mkdir -p "$pdir"; rm -f "$pdir"/*.log
    srm_set_aside; rm -f "$SRM"
    sid=$(( (RANDOM << 8 | RANDOM) % 2000000000 + 1000 ))
    echo "sixview: pass $k: ${NPROC} processes (seats=$SEATS spectators=$SPECT) frames=$frames shots=$ticks session=$sid"
    case "$ORDER" in
    staged)
        # Hub, then every spectator (until each logged its transport start),
        # then the other seats: every spectator is bound at the hub before the
        # session STARTs (the only order that worked before recomp-net
        # answered a spectator's READY with START).
        launch 0 "$pdir" "$frames" "$ticks" "$sid"
        wait_for_line "$pdir/${NAMES[0]}.log" 'started transport' 30 "$PID_0" || { echo "sixview: seat 0 did not start (see $pdir/${NAMES[0]}.log)"; return 1; }
        for ((i = SEATS; i < NPROC; i++)); do launch "$i" "$pdir" "$frames" "$ticks" "$sid"; done
        for ((i = SEATS; i < NPROC; i++)); do
            eval "p=\$PID_$i"
            wait_for_line "$pdir/${NAMES[$i]}.log" 'started transport' 30 "$p" || { echo "sixview: ${NAMES[$i]} did not start"; return 1; }
        done
        sleep 0.5   # let every spectator's HELLO reach the hub
        for ((i = 1; i < SEATS; i++)); do launch "$i" "$pdir" "$frames" "$ticks" "$sid"; done
        ;;
    *)
        # together: every process at once, in process order, no waits.
        # random:   a shuffled order, 0..SIXVIEW_ORDER_GAP_MS between launches.
        # late:     the seats first; the spectators only once seat 0 confirmed
        #           SIXVIEW_LATE_TICK (they join a RUNNING session).
        local order="" j r tmp last="" waited=0 arr=()
        for ((i = 0; i < NPROC; i++)); do arr+=("$i"); done
        if [ "$ORDER" = random ]; then
            for ((i = NPROC - 1; i > 0; i--)); do
                j=$((RANDOM % (i + 1))); tmp=${arr[$i]}; arr[$i]=${arr[$j]}; arr[$j]=$tmp
            done
        fi
        for i in "${arr[@]}"; do
            if [ "$ORDER" = late ] && [ "$i" -ge "$SEATS" ]; then continue; fi
            launch "$i" "$pdir" "$frames" "$ticks" "$sid"; order="$order $i"
            if [ "$ORDER" = random ] && [ "$ORDER_GAP_MS" -gt 0 ]; then
                r=$((RANDOM % (ORDER_GAP_MS + 1)))
                sleep "$(printf '%d.%03d' $((r / 1000)) $((r % 1000)))"
            fi
        done
        if [ "$ORDER" = late ]; then
            while [ "$waited" -lt 1200 ]; do
                last=$(grep -a -o 'TIMELINE t=[0-9]*' "$pdir/${NAMES[0]}.log" 2>/dev/null | tail -1 | sed 's/.*=//')
                [ -n "$last" ] && [ "$last" -ge "$LATE_TICK" ] && break
                kill -0 "$PID_0" 2>/dev/null || break
                sleep 0.1; waited=$((waited + 1))
            done
            echo "sixview: pass $k: seat 0 confirmed t=${last:-?}; launching the spectators now"
            for ((i = SEATS; i < NPROC; i++)); do launch "$i" "$pdir" "$frames" "$ticks" "$sid"; order="$order $i"; done
        fi
        echo "sixview: pass $k: launch order ($ORDER):$order"
        ;;
    esac
    deadline=$(( $(date +%s) + PASS_TIMEOUT ))
    while :; do
        alive=0
        for ((i = 0; i < NPROC; i++)); do eval "p=\$PID_$i"; kill -0 "$p" 2>/dev/null && alive=1; done
        [ "$alive" -eq 0 ] && break
        if [ "$(date +%s)" -ge "$deadline" ]; then echo "sixview: pass $k timed out after ${PASS_TIMEOUT}s; killing"; cleanup; return 1; fi
        sleep 0.5
    done
    for ((i = 0; i < NPROC; i++)); do eval "p=\$PID_$i"; wait "$p" 2>/dev/null; done
    PIDS=""
    return 0
}

# Per pass: TIMELINE agreement across processes. Writes pass<k>/timeline.tsv
# (tick, digest or MISMATCH, processes reporting) and prints a summary.
check_pass() { # pass_no min_last_timeline_tick -> 0 ok
    local k=$1 pdir="$OUT/logs/pass$1"
    "$PY" - "$pdir" "$NPROC" "$2" "$ORDER" "${NAMES[@]}" <<'EOPY'
import re, sys, os
pdir, n, need, order, names = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5:]
tl, shots, drv, ok = {}, [], {}, True
refused = []
for nm in list(names):
    p = os.path.join(pdir, nm + '.log')
    txt = open(p, errors='replace').read() if os.path.exists(p) else ''
    # ORDER=late: a spectator that joins a running match past the join window
    # must be refused loudly (no state transfer); that is the expected result.
    if order == 'late' and 'SPECTATOR' in txt and 'SPECTATOR CANNOT CONTINUE' in txt:
        m = re.search(r'SPECTATOR CANNOT CONTINUE -- (\S+)', txt)
        print('  %-26s refused as expected for ORDER=late (%s)' % (nm, m.group(1) if m else '?'))
        refused.append(nm)
        names.remove(nm)
        continue
    t = dict((int(a), b) for a, b in re.findall(r'TIMELINE t=(\d+) d=([0-9a-f]+)', txt))
    tl[nm] = t
    for a, b in re.findall(r'SHOT t=(\d+) live=([0-9a-f]+)', txt):
        shots.append((nm, int(a), b))
    m = re.search(r'NETPLAY_DRIVER sim=(\d+).*?desyncs=(\d+).*?confirmed=(\d+)', txt)
    drv[nm] = m.groups() if m else ('?', '?', '?')
    started = 'started transport' in txt
    print('  %-26s started=%s timeline=%4d last=%-6s sim=%s confirmed=%s desyncs=%s%s' % (
        nm, 'y' if started else 'N', len(t), max(t) if t else '-', drv[nm][0], drv[nm][2], drv[nm][1],
        '  SPECTATOR' if 'SPECTATOR' in txt else ''))
    # "peer gone" at the very end is normal: the peers finish --benchmark a few
    # ticks apart and the first to leave ends the match for the rest.
    if not t or max(t) < need:
        ok = False
        print('  !! %s: confirmed TIMELINE stops at t=%s, needed >= %d%s' % (
            nm, max(t) if t else '-', need, ' (peer gone)' if 'peer gone' in txt else ''))
allt = sorted(set().union(*[set(v) for v in tl.values()]))
shared = [t for t in allt if all(t in tl[nm] for nm in names)]
mism = [t for t in allt if len(set(tl[nm][t] for nm in names if t in tl[nm])) > 1]
with open(os.path.join(pdir, 'timeline.tsv'), 'w') as f:
    for t in allt:
        ds = set(tl[nm][t] for nm in names if t in tl[nm])
        f.write('%d\t%s\t%d\n' % (t, 'MISMATCH' if len(ds) > 1 else ds.pop(), sum(t in tl[nm] for nm in names)))
n = len(names)
print('  TIMELINE: %d ticks shared by all %d processes (t=%s..%s), %d mismatching tick(s)%s' % (
    len(shared), n, shared[0] if shared else '-', shared[-1] if shared else '-', len(mism),
    (' first at t=%d' % mism[0]) if mism else ''))
# screenshots: live digest vs confirmed timeline digest
with open(os.path.join(pdir, 'shots.tsv'), 'w') as f:
    for nm, t, live in shots:
        conf = tl[nm].get(t, '-')
        f.write('%s %d %s %s\n' % (nm, t, live, conf))
pred = sum(1 for nm, t, live in shots if tl[nm].get(t) and tl[nm][t] != live)
print('  SHOT: %d screenshot(s), %d showed a prediction (live != confirmed)' % (len(shots), pred))
sys.exit(0 if ok and not mism and shared else 1)
EOPY
}

# ------------------------------------------------------------------ single mode
if [ "$MODE" = single ]; then
    VDIR="$OUT/views"; mkdir -p "$VDIR"
    VC="${SIXVIEW_VIEW_COUNT:-6}"; VE="${SIXVIEW_VIEW_EVERY:-60}"
    args=("$ROM" --no-launcher --benchmark "$FRAMES"); [ -n "$SCRIPT_FILE" ] && args+=(--input-script "$SCRIPT_FILE")
    : > "$VDIR/views.tsv"
    for ((i = 0; i < VC; i++)); do
        v=$(view_of "$i"); [ -n "$v" ] && printf '%d\t%s %s\n' "$i" "$(upper "${v%%:*}")" "$(upper "${v#*:}")" >> "$VDIR/views.tsv"
    done
    echo "sixview: single process, GENESIS_VIEWS=$VC every $VE ticks -> $VDIR"
    # shellcheck disable=SC2046
    env $(common_env) GENESIS_VIEWS="$VC" GENESIS_VIEW_SCREENSHOT_EVERY="$VE" GENESIS_VIEW_SCREENSHOT_DIR="$VDIR" \
        NHL94_VIEW="${VIEW_LIST[0]:-}" "$EXE" "${args[@]}" > "$OUT/logs/single.log" 2>&1 &
    PIDS="$!"; wait "$PIDS"; rc=$?; PIDS=""
    echo "sixview: exit $rc, $(ls "$VDIR" | grep -c '^view.*_t.*\.png$') view PNG(s)"
    grep -a -E '^multiview' "$OUT/logs/single.log" | head -5
    if ! ls "$VDIR" | grep -q '^view.*_t.*\.png$'; then
        echo "sixview: FAIL -- no view<k>_t<tick>.png written: this binary does not implement GENESIS_VIEWS"
        echo "         (or GENESIS_VIEW_DEBUG / a view preparer is required); log: $OUT/logs/single.log"
        exit 1
    fi
    if [ "${SIXVIEW_COMPOSITE:-1}" = 1 ]; then
        "$PY" "$HERE/sixview_composite.py" "$VDIR" --mode views --out "$OUT/composite" --fps "$FPS" \
            --ffmpeg "$FFMPEG" --title "single" || exit 1
    fi
    exit 0
fi

# ------------------------------------------------------------------ procs mode
{
    echo "# sixview $(date) exe=$EXE seats=$SEATS spectators=$SPECT script=${SCRIPT_FILE:-none}"
    echo "# latency=${SIXVIEW_LATENCY_MS:-0} jitter=${SIXVIEW_JITTER_MS:-0} loss=${SIXVIEW_LOSS_PCT:-0} seed=${SIXVIEW_SIM_SEED:-} order=$ORDER"
    for ((i = 0; i < NPROC; i++)); do
        echo "# ${NAMES[$i]}: ${ROLES[$i]} bind=127.0.0.1:$((BASE_PORT + i)) NHL94_VIEW=${VIEWV[$i]}"
    done
} > "$OUT/run.txt"
cat "$OUT/run.txt"

rc=0
LAST_TICK_ALL=$(echo $ALL_TICKS | awk '{print $NF}')
for ((k = 0; k < ${#PASSES[@]}; k++)); do
    ticks="${PASSES[$k]}"
    last=${ticks##*,}; fr=$((last + 120))
    [ "$k" -eq 0 ] && [ "$FRAMES" -gt "$fr" ] && fr=$FRAMES
    run_pass "$k" "$ticks" "$fr" || rc=1
    check_pass "$k" $((fr - 3 * TL_EVERY)) || rc=1
done

# Cross-pass determinism: the same confirmed digest at every tick two passes share.
# With the link simulator on, seat 0's input script (stepped per WALL frame)
# lands on different sim ticks from pass to pass, so passes play different
# games: reported as a warning then (each pass is still self-consistent).
NETSIM="${SIXVIEW_LATENCY_MS:-}${SIXVIEW_JITTER_MS:-}${SIXVIEW_LOSS_PCT:-}"
xrc=0
"$PY" - "$OUT/logs" <<'EOPY' || xrc=1
import glob, os, sys
d = {}
for f in sorted(glob.glob(os.path.join(sys.argv[1], 'pass*', 'timeline.tsv'))):
    for line in open(f):
        t, dg, _ = line.split('\t')
        d.setdefault(int(t), {})[os.path.basename(os.path.dirname(f))] = dg
bad = [t for t, v in d.items() if len(set(v.values())) > 1]
multi = [t for t, v in d.items() if len(v) > 1]
print('sixview: cross-pass: %d tick(s) seen by 2+ passes, %d disagree%s' % (
    len(multi), len(bad), (' (first t=%d)' % min(bad)) if bad else ''))
sys.exit(1 if bad else 0)
EOPY
if [ "$xrc" -ne 0 ]; then
    if [ -n "$NETSIM" ] && [ -n "$SCRIPT_FILE" ]; then
        echo "sixview: WARNING: passes disagree under the link simulator with an input script (wall-frame"
        echo "         script timing shifts the game between passes); use one pass (<= 16 shot ticks),"
        echo "         SIXVIEW_SCRIPT=none, or no network simulation for a continuous video"
    else rc=1; fi
fi

# Screenshots: every process at every tick? Byte-identical across processes?
cat "$OUT"/logs/pass*/shots.tsv > "$OUT/shots/shots.tsv" 2>/dev/null
: > "$OUT/shots/tiles.tsv"
# Grid order: home views first (top row), then away, then the rest; process
# order within each group.
for grp in home away other; do
    for ((i = 0; i < NPROC; i++)); do
        case "${VIEWV[$i]%%:*}" in home|away) g="${VIEWV[$i]%%:*}" ;; *) g=other ;; esac
        [ "$g" = "$grp" ] && printf '%s\t%s\n' "${NAMES[$i]}" "${LABELS[$i]}" >> "$OUT/shots/tiles.tsv"
    done
done
missing=0; same=0; differ=0
for t in $ALL_TICKS; do
    sums=""
    for ((i = 0; i < NPROC; i++)); do
        f="$OUT/shots/${NAMES[$i]}.png.t$t.png"
        # ORDER=late: spectators are refused by design (checked per pass above).
        [ "$ORDER" = late ] && [ "$i" -ge "$SEATS" ] && continue
        if [ -f "$f" ]; then sums="$sums $(md5 -q "$f" 2>/dev/null || md5sum "$f" | cut -d' ' -f1)"
        else missing=$((missing + 1)); fi
    done
    nu=$(echo $sums | tr ' ' '\n' | sed '/^$/d' | sort -u | wc -l | tr -d ' ')
    if [ "$nu" -le 1 ]; then same=$((same + 1)); else differ=$((differ + 1)); fi
done
echo "sixview: screenshots: $(echo $ALL_TICKS | wc -w | tr -d ' ') tick(s) x $NPROC processes, $missing missing;" \
     "$same tick(s) byte-identical on every process, $differ differ (expected once NHL94_VIEW is read)"
[ "$missing" -eq 0 ] || rc=1

if [ "$PROC_VIEWS" = 1 ] && [ "${SIXVIEW_COMPOSITE:-1}" = 1 ]; then
    # Gather each process's view0 into one views layout: view<i>_t<tick>.png.
    VG="$OUT/pviews_grid"; mkdir -p "$VG"; : > "$VG/views.tsv"; pv_missing=0
    for ((i = 0; i < NPROC; i++)); do
        printf '%s\t%s\n' "$i" "${LABELS[$i]}" >> "$VG/views.tsv"
        for t in $(seq "$SHOT_FROM" "$SHOT_EVERY" "$SHOT_TO"); do
            f="$OUT/pviews/${NAMES[$i]}/view0_t$t.png"
            if [ -f "$f" ]; then ln -sf "$f" "$VG/view${i}_t$t.png"; else pv_missing=$((pv_missing + 1)); fi
        done
    done
    echo "sixview: per-process views: $pv_missing missing view PNG(s)"
    [ "$pv_missing" -eq 0 ] || rc=1
    "$PY" "$HERE/sixview_composite.py" "$VG" --mode views --out "$OUT/composite" \
        --fps "$FPS" --ffmpeg "$FFMPEG" --title "${SEATS}seat+${SPECT}spec own views" || rc=1
elif [ "${SIXVIEW_COMPOSITE:-1}" = 1 ]; then
    "$PY" "$HERE/sixview_composite.py" "$OUT/shots" --mode procs --out "$OUT/composite" \
        --fps "$FPS" --ffmpeg "$FFMPEG" --title "${SEATS}seat+${SPECT}spec" || rc=1
    [ "${SIXVIEW_KEEP_RAW:-1}" = 0 ] && rm -f "$OUT"/shots/*.png.t*.png
fi
echo "sixview: $( [ $rc -eq 0 ] && echo PASS || echo FAIL ) -- output in $OUT"
exit $rc
