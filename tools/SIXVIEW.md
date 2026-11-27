# Six-view localhost harness

`tools/sixview_localhost.sh` runs several NHL94Recomp processes in **one**
rollback-netplay session on 127.0.0.1. Each process takes tick-aligned
screenshots at the same ticks. The script checks that every process's confirmed
`TIMELINE` digests agree, then `tools/sixview_composite.py` builds a labelled
3x2 grid PNG for each tick and an MP4 of the grids.

```sh
tools/sixview_localhost.sh                 # 2 seats + 4 spectators, Demo game, t=1800..4800 every 60
tools/sixview_localhost.sh --help          # every knob
SIXVIEW_SEATS=6 SIXVIEW_SPECTATORS=0 tools/sixview_localhost.sh      # six player seats instead
SIXVIEW_LATENCY_MS=40 SIXVIEW_JITTER_MS=10 SIXVIEW_LOSS_PCT=1 \
  SIXVIEW_SHOT_FROM=1800 SIXVIEW_SHOT_TO=3600 SIXVIEW_SHOT_EVERY=120 tools/sixview_localhost.sh
SIXVIEW_SCRIPT=none SIXVIEW_FRAMES=34000 SIXVIEW_SHOT_FROM=32400 SIXVIEW_SHOT_TO=33300 \
  tools/sixview_localhost.sh               # the input-free attract demo instead
python3 tools/sixview_composite.py <shots-dir> --out <dir> [--mode procs|views] [--fps 10]
```

The script runs under macOS `/bin/bash` 3.2. It uses no associative arrays,
no `mapfile` and no `wait -n`. On exit, Ctrl-C or TERM it sends TERM to every
child process, then KILL. Output goes to
`~/nhl94-online-build/harness/sixview_<timestamp>/` by default
(`SIXVIEW_OUT`):

| Path | Contents |
|---|---|
| `run.txt` | each process's role, port and `NHL94_VIEW` |
| `logs/pass<k>/<name>.log` | stderr and stdout of each process |
| `logs/pass<k>/timeline.tsv` | tick, digest (or `MISMATCH`), number of processes that reported it |
| `logs/pass<k>/shots.tsv` | `name tick live confirmed`: the digest shown in the screenshot vs the confirmed digest |
| `shots/<name>.png.t<T>.png` | raw engine PNGs, about 215 KB each and uncompressed. `SIXVIEW_KEEP_RAW=0` deletes them after compositing |
| `shots/tiles.tsv` | grid order and labels (home views on the top row, away views on the bottom) |
| `composite/grid_t<T>.png`, `composite/sixview.mp4`, `composite/index.tsv` | the grids, the video and the list of ticks |

## Session layout (verified)

Process *i* binds `127.0.0.1:BASE_PORT+i`. All processes get the same
`GENESIS_NET_SESSION_ID` (random for each pass) and these settings:
`GENESIS_NETPLAY=1 GENESIS_NET_TRANSPORT=hub GENESIS_NET_SLOTS=<seats>
GENESIS_NET_TIMELINE_EVERY=60 SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1`,
plus `--no-launcher --benchmark N`.

| Process | Additional environment |
|---|---|
| seat 0 (runs the LAN hub, sim authority, gets the input script) | `GENESIS_NET_SLOT=0 GENESIS_NET_BIND=127.0.0.1:P0 GENESIS_NET_PEER=` |
| seat s (1..SEATS-1) | `GENESIS_NET_SLOT=s GENESIS_NET_BIND=127.0.0.1:Ps GENESIS_NET_PEER=127.0.0.1:P0` |
| spectator at wire slot w (SEATS..7) | `GENESIS_NET_SPECTATOR=1 GENESIS_NET_SPECTATOR_WIRE_SLOT=w GENESIS_NET_BIND=127.0.0.1:Pw GENESIS_NET_PEER=127.0.0.1:P0` |
| every process | `NHL94_VIEW=<home\|away>:<G\|C\|LW\|RW\|LD\|RD>` (passed through unchanged) plus `GENESIS_SCREENSHOT_AT_TICK=T1,..,T16 GENESIS_SCREENSHOT_AT_EXIT=<out>/shots/<name>.png` |

What the code allows (`runner/netplay/genesis_netplay.c`, recomp-net):

- **Seats:** 1 to 6. `GENESIS_NETPLAY_MAX_SLOTS` = `GENESIS_SIM_MAX_PLAYERS`
  = 6, and the engine refuses more. Two or more seats in `lan` mode switch to
  the hub automatically.
- **Spectators** need the hub or the relay transport, and their wire slot
  must be at least the number of seats. The hub
  (`rnet_transport.c hub_learn_and_forward`) learns a peer from the slot byte
  in each packet and only for slots 0..7 (`RNET_MAX_SLOTS` = 8). So seats plus
  spectators can total at most 8 on the hub.
- **Spectator behavior:** a spectator sends no input, pins lockstep ("remote
  input is never predicted"), and acknowledges seat 0.
- **Occupied mask:** `GENESIS_NET_OCCUPIED` defaults to 0, which means every
  seat is occupied. `GENESIS_NET_SLOT_PORTS` maps seats to logical players.
  The harness does not use either.
- **Start order (fixed 2026-11-22, recomp-net wire version 3).** Before the
  fix the session STARTed as soon as every seat was READY and the authority
  re-sent a lost START only to seats, so a spectator whose first packet
  reached the hub after START never ran (`sim=0`, "RB_SYNC dropped"): 8 of 10
  runs failed with `SIXVIEW_ORDER=together` and 8 of 10 with `random`. Now
  seat 0 gets `GENESIS_NET_SPECTATORS=<spectators>` (the roster, as the lobby
  passes it) and holds START until that many spectators sent READY (at most
  10 s); a spectator whose START was lost is answered with START again, and
  catches up anything else it missed (seat rows, the SRAM transfer, the mod
  set, the tick-0 commit) with `OBS_CATCHUP`. Any order works; the default is
  still `staged`.
- **`SIXVIEW_ORDER`** picks the launch order: `staged` (hub, spectators,
  other seats, waiting for each), `together` (all at once), `random`
  (shuffled, up to `SIXVIEW_ORDER_GAP_MS`=300 between launches) or `late`
  (seats first, spectators once seat 0 confirmed `SIXVIEW_LATE_TICK`=600).
  A spectator can join a running match only until tick 48
  (`RNET_OBS_JOIN_MAX_TICK`: it replays from tick 0 out of the seats' 128-row
  input history); later it is refused and exits with
  `SPECTATOR CANNOT CONTINUE -- spectator_late_join` instead of sitting at
  `sim=0`. With `late` the check treats that refusal as the expected result
  and grades the seats only. `SIXVIEW_ROSTER=0` starts on seats alone (the
  default for `late`).

## Reaching a CPU-vs-CPU game

On the setup menu, the second row ("Players") cycles to the right through
One-Home, One-Visitor, Two-Teammates, Two-Head to Head and **Demo**. Choosing
Demo switches Goalies to "Auto Control", and all 12 players are CPU. The
default `SIXVIEW_SCRIPT=demo` drives seat 0 with this script:

`WAIT 600, START, WAIT 120, START, WAIT 120, DOWN, 4x (RIGHT, WAIT 30), WAIT 30, START, WAIT 240, START, WAIT 240, START`

This gives LA at MTL. The opening faceoff is on screen at about t=1600, the
clock runs from about t=1850 (9:57 at t=1900), and play continues on ice
through at least t=4980. The first period lasts about 13,000 ticks (the game
clock runs at about 22 ticks per game second).

With no input at all (`SIXVIEW_SCRIPT=none`), the game shows the EA and title
screens until about t=5400. It then sits on the setup menu cycling player
cards, shows matchups at about t=27600, the pre-game menu at about t=31200,
and is **on ice from about t=32400** (CPU vs CPU, clock 9:27). It was checked
through t=44400 (a goal at about t=38400). This attract demo is input-free and
therefore deterministic under any network simulation, but each pass has to
run about 33,000 ticks first.

## Verified (2026-11-21, stable binary build 7f36b5a8)

| Run | Result |
|---|---|
| default: 2 seats + 4 spectators, Demo script, frames 5000, shots 1800..4800/60 (51 ticks, 4 passes) | every pass: 6/6 processes, 83/62/78/81 TIMELINE ticks shared by all 6, **0 mismatches**, last confirmed t=4980; 0 desyncs; 306/306 screenshots, none showing a prediction; cross-pass 81 ticks, 0 disagree; all 51 ticks byte-identical across the 6 processes; 51 grids + 5.1 s MP4; about 37 s wall time in total |
| same, `RNET_SIM_LATENCY_MS=40 JITTER=10 LOSS=1% SEED=7`, frames 6000 | 98 shared ticks (t=60..5880), 0 mismatches, 0 desyncs; the seats rolled back (2 episodes, 2149 invents) and the spectators stayed in lockstep |
| 6 seats, 0 spectators, Demo script | 66 shared ticks to t=3960, 0 mismatches. The frame at t=2400 is byte-identical to the 2+4 run |
| 2 + 4, `SIXVIEW_SCRIPT=none`, frames 34000, shots 32400..33300 | 566 shared ticks (t=60..33960), 0 mismatches, attract play on ice |
| `SIXVIEW_MODE=single` on the stable binary | fails clearly: the binary writes no `view<k>_t<T>.png`. The compositor's views mode was tested on synthesized `view<k>_t<T>.png` files, including a missing tile |
| TERM sent to the harness mid-run | all 6 children gone |

## Limits

- **16 screenshot ticks per process per run.** `main.c` keeps a 16-entry
  `GENESIS_SCREENSHOT_AT_TICK` list. The harness splits longer lists into
  passes and re-runs the whole session for each pass. Later passes stop 120
  frames after their last tick. Without the link simulator, the passes replay
  the same game: their digests agreed on every shared tick.
- **The link simulator with an input script makes passes differ.** The input
  script runs once per **wall frame**, not per sim tick. When the network
  simulator stalls a peer, the script's presses land on different ticks. Each
  pass is still self-consistent, but two passes play different games (47 of 62
  shared ticks disagreed at 40 ms). The harness then prints a WARNING instead
  of failing. For continuous video under latency, use one pass (16 ticks or
  fewer), `SIXVIEW_SCRIPT=none`, or no simulation. For the same reason, a
  latency run is not the same game as a run without latency.
- A screenshot is taken when a process's live sim reaches T. It can show a
  prediction that is corrected later, and `shots.tsv` and the tile border
  (orange instead of green) report that. None did so in these runs.
- At the end of a run, "peer gone" is normal: the processes finish
  `--benchmark` a few ticks apart. The check requires each process's last
  confirmed TIMELINE tick to be at least `frames - 180`.
- `NHL94_VIEW` is not read yet, so all six tiles are identical. The harness
  reports how many ticks are "byte-identical" vs "differ" for when it is.
- Engine frames switch between 256 and 320 pixels wide (H32 menus, H40
  title). The compositor scales every tile to 320x224 with nearest-neighbor.
  The installed ffmpeg has no `drawtext`, so labels use a built-in 5x7 font
  (letters are drawn in upper case).
- Single-process mode (`GENESIS_VIEWS`, `GENESIS_VIEW_SCREENSHOT_EVERY`,
  `GENESIS_VIEW_SCREENSHOT_DIR`) depends on the uncommitted
  `runner/multiview.c`, which is not in the stable binary. The compositor
  reads its `view<k>_t<T>.png` files and ignores `main_t<T>.png`. Labels for
  view k come from `SIXVIEW_VIEWS[k]` (written to `views.tsv`).
- `--benchmark` runs uncapped with hidden windows. Six processes need about
  6 to 8 s per 5,000 ticks on this machine and about 45 s per 34,000 ticks.
- Use distinct `SIXVIEW_BASE_PORT` values for runs that happen at the same
  time. Other harnesses on this machine use 9700..9703, 9810..9813, 18965,
  18977 and 17890.
