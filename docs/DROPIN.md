# Drop-in / drop-out (EASHL-style play) for Position-lock rooms

Research design: `eashl/r2.out` (read-only). This file says what is built, how
it is tested, and what is left. Stages: **B** a dropped seat becomes CPU and the
game continues (done), **D** waiting-room Ready / Swap (see the D section),
**C** join a game in progress at the next whistle (see the C section).

## B. A dropped guest seat becomes CPU, the game continues

### What happens

Before: any seat's BYE set `peer_gone` and `main.c` ended the match for every
peer; a silent seat in a 3+ seat room stalled everybody at `tip + P`
(`pcap_freeze`) with nothing recovering it.

Now, in a **rollback room of 3 or more seats** (the drop policy):

* The authority (seat 0) decides. A guest's **BYE** drops its seat at once; a
  guest **silent** for `GENESIS_NET_DROP_MS` (default 2000) drops it *only while
  some other seat is heard* (if everybody is quiet the authority's own link is
  the suspect and nobody is dropped).
* The drop is an event in an append-only **occupancy table**
  `{wire, seat, present, gen}` ("seat S is absent from wire tick W"), carried by
  new wire-v6 packets `OCC_EVENT` (authority -> all, the whole table, re-sent
  every 100 ms until every present seat acked, then every second so spectators
  catch up) and `OCC_ACK`. `W` = the authority's contiguous ack of that seat + 1,
  so every row below `W` is real and held by the authority.
* Every peer applies the same table to the same wire ticks. From `W` on,
  `rnet_session_peek_remote_input` returns the **zero row flagged `absent`**
  for that seat; `remote_tip`, session stats, input-bundle acks and liveness
  ignore it, so nothing waits on it (this is what lifts the stall).
* The row travels the existing rollback path: the runner's `h_decode` turns
  `absent` into `RNET_RB_ROW_ABSENT` (bit 15 of `RNetRbFrame.buttons`), the
  driver's reconcile sees the difference to a predicted human row and runs the
  normal mispredict -> episode -> replay. Rows a peer had already consumed as
  *real* at wire >= W are marked predicted again when the table grows
  (`rb_occ_sync`) so they are corrected the same way. `rb_expect_mask` and the
  `RNetRbSession` seal set subtract dropped seats (episodes, hash chains,
  baselines, quiesce never wait on a gone peer).
* `h_publish` passes an `absent_mask` to `genesis_netplay`, which clears that
  seat's `human_mask` bit and zeroes its pad in the **sealed `GenesisSimInput`**
  (snapshotted, replayed exactly). NHL94's `enforce()` already releases seats
  not in `human_mask`: `contNteam = 0`, target -1, `release()` -> the AI takes
  the player (goalie too, via the existing `pf3gctrl` rule).
* The authority re-serves the dropped seat's last 48 rows (in the seat's name,
  legal on the authority's own egress) for 3 s, so a peer that lost one below
  `W` still gets the real row instead of keeping a wrong prediction.
* A dropped-but-alive peer (frozen window, SIGSTOP) learns it from the table
  (`rnet_session_dropped_self`), leaves the match and lands in the lobby with
  "You were removed from the match: your connection to the host timed out. Your
  player is now played by the CPU." Its remaining packets are ignored
  ("ghost": INPUT rows still stored, everything else dropped, no liveness).
* **The match ends** (`genesis_netplay_match_should_end`, asked by `main.c`)
  only if the host is gone (BYE or 3 s silence from seat 0), this peer was
  dropped, every remaining guest is silent at once (our own link), or fewer than
  `GENESIS_NET_MIN_HUMANS` (default 1) humans remain. A host alone vs the CPU
  plays on. **2-seat rooms keep the old rule** (policy off), as do delay-sync
  rooms and `GENESIS_NET_DROP_MS=0`.

Deviation from the research text: events are per seat (`{wire, seat, present}`)
rather than a whole mask per event, so two drops whose wires are not in
arrival order stay correct. Wire v6 reserves ids 32/33 for `SEAT_CLAIM` /
`SEAT_GRANT` (stage C).

### NHL94 side (`game/nhl94_seats.c`)

* `drop_pause_orphan()` (called at the end of `read_joy_data`): the pause menu
  reads ONE pad (`getpzjoy`: pad 2 when `sfpj`, else pad 1; pad 3/4 with 4 Way
  Play and `pausepad`). If that pad's seat is not in `human_mask` for
  `NHL94_DROP_PAUSE_FRAMES` (300) game frames, START is held on that pad for two
  frames (`HandleMenuInput`: START leaves the menu). Counter: guest RAM
  `$FFE300` (u16).
* Penalty shot / shootout: no extra code. The lock now stays on during an
  attempt (`ctl_mode` CM_PS, docs/PLAYER_LOCK.md) and `ps_targets` only picks
  seats in `human_mask`, so a seat that left has no target and its pad goes
  to the AI through the normal `enforce` path. (The stage-B branch had a
  `drop_penalty_shot_cleanup()` for the old "lock off in PS" behaviour; it
  was dropped when the branches were merged.) The attempt is latched
  (`$FFE304-$FFE306`, docs/PLAYER_LOCK.md "Penalty shots"): no join admission while a
  penalty shot is pending / live or in a shootout, and the shooter / goalie seats stay
  those chosen at the start of the attempt.
* Joiner role body: `role_targets` reserves the role body of a seat that is in
  `human_mask` but still held by `drop_join_gate`, so another seat's sticky
  substitute never sits on it (its admission would invalidate that pick at a
  stoppage where no new pick is allowed).

RAM owned: `$FFE300-$FFE33F` (`$FFE300` pause-orphan counter, `$FFE302` join-at-whistle waiting set, stage C; `$FFE304-$FFE30F` penalty-shot attempt latch: `$FFE304` shooter seat+1, `$FFE305` goalie seat+1, `$FFE306` u16 key).
No new `game.toml` site.

### Knobs

| Knob | Meaning |
|---|---|
| `GENESIS_NET_DROP_MS` | silence before a guest is dropped (default 2000; 0 = policy off) |
| `GENESIS_NET_MIN_HUMANS` | end the match when fewer humans remain (default 1) |
| `GENESIS_NET_TEST_LEAVE=<tick>[:crash][@seat]` | this peer (or only seat `seat`) leaves at sim tick `tick`: BYE + back to the lobby, or `:crash` = SIGKILL without a word |
| `GENESIS_SIM_TEST_ABSENT="k@tick,..."` | offline: seat k is no human from sim tick `tick` (human_mask bit cleared, pad zero) -- what the online schedule seals |

### Tests (all run on this build; local lobby server, never the production one)

Offline (`tools/seats/drop_checks.sh <build> <out>`):

| Check | Result |
|---|---|
| `lock4.input`, no knob | `A94E00C87F121117` (the existing lock4 baseline) |
| `lock4.input`, `GENESIS_SIM_TEST_ABSENT=2@2600,3@3400` twice | `DFB3450E0C17DA07` both; `joy=` 90C -> 80C -> 00C (the dropped seats' players lose human control) |
| `rb_probe_seats.sh` same script + knob | passes=139 divergences=0 asymmetric=0 PASS |
| `pause_orphan.input`, `GENESIS_SIM_TEST_ABSENT=0@2380`, 3500 frames | `97EEF55A4F202882`; "pause menu orphaned" at t=2679, the clock runs again |
| `run_checks.sh` battery | unchanged: lock2 3EF834BA226E6E28, lock4 A94E00C87F121117, callpass FAA72A99530568E6, callpass2 560C3425A02A9A91, **Demo 80CE87C28143ADBF**, rb_probe 99 / 209 / 139 PASS, netplay TIMELINE 66/66 identical |

Online (`tools/net/drop_rooms.sh`, headless peers through a local
`recomp-net-server`, `drop_grade.py`: survivors' TIMELINE digests identical over
the whole shared range, an occupancy event on every survivor, nobody ended):

| Scenario | Result |
|---|---|
| `grace4`: 4 seats, guest 2 leaves gracefully at tick 3300 | PASS: 91 shared TIMELINE ticks (t=5460), 0 mismatching, event `seat 2 ABSENT from wire 3306`, `joy=914 -> 814` |
| `crash4`: same, `:crash` (SIGKILL) | PASS: 91/91, event "silent (connection lost)" wire 3306 |
| `crash3`: 3 seats, guest 1 crashes | PASS: 83 ticks |
| `lag4`: 4 seats, `RNET_SIM_LATENCY_MS=60 JITTER=20 LOSS=1%`, guest 3 crashes, guest 1 with the forced-mispredict injector | PASS: 91 ticks, 365 episodes, 0 desyncs |
| `two6`: 6 seats, guest 2 leaves (3000), guest 4 crashes (3600) | PASS: both events on all 4 survivors, 91 ticks |
| ghost: guest 2 `SIGSTOP`ed 4 s, then resumed | survivors PASS (98 ticks), the ghost leaves with "You were removed ..." and is back in the room |
| `host2`: the host leaves | every guest ends the match "The host left the match." (unchanged) |
| `seats2`: 2-seat room, the guest leaves | the host ends the match (unchanged) |
| `spect`: 4 seats + a spectator | **not run**: the local lobby server (and an older build) never completes the spectator's join here, unrelated to this work; the observer's adoption of the table is covered by `occ_schedule_test` |

recomp-net unit tests (`ctest`, own build dir, `-DRNET_ENABLE_ICE=OFF`): new
`occ_schedule_test` passes (5/5 runs); `wire_v5_test` updated to accept v6; the
only failures are the known upstream ones (`session_state_test`,
`lobby_ws_backlog_test`, `lobby_client_test` does not link on this macOS:
`u_short` in `rnet_host_relay.c`, same on the base) and the flaky
`state_replay_test`. `rb_driver_test` passes alone (3/3, also on the base); it
timed out / failed once when run with `ctest -j` next to other heavy jobs.

### Known limits (stage B)

* Rollback rooms only; 2-seat rooms keep the old rule; no host migration.
* A drop within `P` (<= 32) ticks of the authority's tip is the supported
  shape; the correction is bounded by the 64-row seal mask like any late input.
* Rows below the drop wire that a peer lacks AND the authority lacks (a hole in
  the authority's ring) are never filled: that peer keeps a prediction (the
  existing "correction lost" logging reports it).
* A spectator that missed every `OCC_EVENT` stalls at the drop wire (it
  predicts nothing) until the once-a-second re-send reaches it.

## D. Waiting room: a real Ready toggle and an explicit Swap

### Behaviour

* **Ready.** In an online room of **3 or more seats** (`manual_ready` hook set by
  `genesis_host_lobby.c`; 2-seat rooms keep the old auto-ready;
  `GENESIS_NET_MANUAL_READY=0|1` overrides, 1 even for 2 seats) a guest joins
  *not ready* and presses **Ready** itself (a `Ready` / `Unready` button on its
  own row, status text `Ready` in green / `Not ready` in amber). The lobby client no longer re-arms
  it after the server clears it: **the server clears everyone's ready on a join,
  kick, seat move and swap**, so nobody launches into a seat they did not
  confirm (recomp-net `rnet_lobby_set_manual_ready`; the host keeps arming
  itself, its Play is its ready).
* **Host Play** is disabled until every guest is ready; the tooltip says
  "Waiting for <names> to press Ready" and the same line is drawn beside PLAY.
  The lobby client's own gate (`rnet_lobby_match_blocked_by_unready`, the
  server's `not_all_ready`) is unchanged, so a launch cannot slip past it.
  "Not ready" for a launch gate (no game image / mods missing, set through
  `rnet_lobby_set_launch_blocked`) still wins over a user's Ready.
* **Swap.** Occupied seats carry a **Swap** button next to the role label (where
  Take sits on open seats); it calls `seat_swap_request`. The target gets the
  modal prompt, now with both roles: "Bob (Home C) wants to swap with your Away G."
  Accept / decline are the existing ones. Nothing moves until they accept; a
  swap clears everyone's ready (server), so both players press Ready again.

### Files

recomp-ui: `recomp_launcher.h` (append-only `manual_ready` callback,
`RECOMP_LAUNCHER_HAS_MANUAL_READY`), `recomp_netplay_host.{h,c}` (hook,
`manual_ready_active()`, pump, rematch), `launcher_imgui.cpp` (row status +
button, Play gate text, Swap button, prompt). recomp-net: `lobby_client.h` /
`rnet_lobby_client.c` (`rnet_lobby_set_manual_ready`: three guarded lines at
the auto-arm sites -- **touches `rnet_lobby_client.c`, watch for conflicts with
the member-build-data signal work**). Engine: `genesis_host_lobby.c`.

### Tests (`tools/seats/lobby_ready_test.sh`, local lobby, headless selftest drills)

New selftest knobs in `genesis_host_lobby.c`: `GENESIS_LOBBY_SELFTEST_READY`
(1 default: ready, and again after each clear; 0 never; N>=2 N seconds after
joining), `_SWAP=<seat>`, `_ACCEPT_SWAP` (default 1), `_HOLD_MS` (host waits
before Play). The selftest guest/host re-ready whenever the server cleared it.

| Drill | Result |
|---|---|
| `ready_late` (guest 2 readies 9 s after joining) | host logs "Play blocked: guest2 is not ready", then launches; digests identical |
| `ready_never` | blocked by name for the whole run, no launch |
| `swap_ok` (guest 1 asks for guest 2's Away G) | prompt text "guest1 (Away C) for my Away G", ACCEPTED, slots 1 <-> 2 exchanged in the launch, ready given again (#3, #4), digests identical |
| `swap_no` | DECLINED, nobody moves |
| 2-seat room, 3-6 seat rooms (all `room.sh` / `drop_rooms.sh` runs) | unchanged: the selftest readies itself |

recomp-ui `ctest`: 79/80 (the known `recomp-ui-launcher-netplay-handoff` does
not link here). recomp-net: 36/36 of the non-excluded tests.

**Not exercised:** the ImGui drawing itself (no display here): the row layout,
the button next to the status text, the line beside PLAY and the prompt text
compile and follow the existing patterns, but nobody has seen them on screen.

## C. Join a game in progress by taking a CPU seat

A player in the lobby sees a room whose match is running as `[in progress]`,
joins it, **catches up** (the match replayed from tick 0, nothing rendered or
played), then **claims a free / CPU seat** and plays from the **next whistle**;
everybody stays in sync. Research: `eashl/r2.out` section C; the plan here is
what it said, in the order it said it (items 1-8), now built.

### Behaviour, end to end

1. **The host asks.** `genesis_host_lobby.c` creates 3+ seat online rooms with
   `late_join` (`GENESIS_NET_LATE_JOIN=0` opts out). A server that does not know
   the option ignores it (see "Unchanged server" below).
2. **The launch carries every room seat** (item 1). With late join the slot plan
   (recomp-ui `plan_session_slots`) gives *every lobby seat* a session slot: the
   players' first (host = slot 0), then the seats nobody held, in seat order, each
   with its own port. `occupied_mask` = all seats, `open_slot_mask` = the empty
   ones. The mask the server sends (`open_slots`) is the LAUNCH-time table: the
   plan treats those seats as open whoever sits in them now, so a late joiner
   (which reads the live member list, where a player may since have taken one)
   builds the same plan as the original peers (`netplay_host_test`,
   `case_host_first_late_open_mask`). Without the mask the plan is built from the
   live table, so a late joiner can only be right from a server that sends it.
   Every peer seeds `ABSENT from wire 0` for the open seats **locally and
   identically** (`RNetConfig.open_mask`): nothing travels, the table's first
   gens are the same everywhere, and the authority's whole-table resend lines up
   with them. A disagreement is logged ("occupancy table DISAGREES").
3. **A join is an ADD event** (item 2). `rnet_session_declare_seat_add(slot)`
   (authority): `PRESENT from wire A`, `A = sealed tip + 112` (`RNET_ADD_LEAD_TICKS`),
   then a **barrier**: the rollback driver stops sealing once its tip + D + P + 2
   could reach `A` until every present seat (the joiner included) acked the
   event's gen (`rnet_session_occ_barrier`). Peers run at most P ahead of the
   authority's tip, so **nobody consumes a row at `A` before knowing**. 6 s
   without all acks cancels the join (an ABSENT event at the same wire) so a
   vanished joiner cannot hold the room.
4. **Time-aware table.** A seat present from a future wire is in the table at once
   (every peer holds the same schedule) but nothing waits on it before `A`:
   `rnet_session_absent_mask_at(wire)` (driver `rb_expect_mask`, the seal set),
   `remote_tip` (floor `A-1`), stats, contiguous watermark (extends over absent
   wires, capped at sim + 64 so it never passes a wire an ADD could still pick).
   A seat entering the episode set starts its **hash chain there**
   (`rnet_hc_prime_after`). The joiner's own seat is the flagged zero row below
   `A` in its own sim too (`rb_local_absent_at`).
5. **Full input log** (item 3). `RNetConfig.late_join`: the authority logs every
   seat's rows (as they arrive), a seat its own (growable, 4 B + size per tick,
   a bit map; 216 000 ticks x 12 seats ~ 13 MB at the authority). START is answered
   at any tick (no `RNET_OBS_JOIN_MAX_TICK` refusal), rows are served from the log
   (`OBS_CATCHUP` now serves every source seat: a seat its own rows, the authority
   also a dropped seat's and an earlier occupant's rows). The spectator's prefetch
   is 88 ticks (the ring accepts ~119 ahead). A seat re-added after a drop starts
   clean (stale BYE / liveness / hub identity cleared).
6. **Catch-up loop** (item 3, runner). `genesis_netplay_catching_up()`: far behind
   live (> 90 ticks) `main.c` runs `genesis_sim_step(no sink, no audio)` in 12 ms
   slices per host iteration (measured ~1000 ticks/s, 2500 ticks in ~2.5 s here),
   presenting nothing, then presents like a spectator below 30 ticks of lag.
7. **Claim / grant** (item 5). Within 48 ticks of live for 300 ms the joiner sends
   `SEAT_CLAIM` (wire id 32; `GENESIS_NET_LATE_SEAT=<session slot>` picks one, default
   any free); the authority appends the ADD, **re-binds the joiner's hub identity
   from its spectator index to the seat** (ICE hub and LAN hub), and answers
   `SEAT_GRANT` (id 33: seat, wire `A`, gen, D; re-sent every 100 ms until acked).
   The grant **promotes the session in place** (observer -> seat: own wire id,
   ring, D) and the facade promotes the rollback driver
   (`rnet_rb_driver_promote_observer`: lockstep released, hash chains restart at the
   current tick). `BUSY` (another join in flight, or the joiner > 160 ticks behind)
   is retried; `NO_SEAT` / `STATE` end the claim and the joiner keeps watching.
8. **ICE during a match** (item 4). The joiner connects an agent to the host while
   it waits in the room (it is a gallery spectator in the room). The lobby client
   **holds the server's late `launch` until that agent is COMPLETED**. The host
   keeps answering agents during its match for spectators that arrive later
   (`host_ice_step` late mode, `rnet_lobby_ice_take_late` -> `rnet_session_hub_add_agent`
   at the spectator's hub index = `spectator_relay_base` + gallery index).
9. **Server** (item 6; repo `recomp-net-server`, branch `eashl-latejoin`, local
   tests only, *not deployed*): `create late_join` (implies a gallery),
   `match_state` op, `in_progress` in the list / room view, a join of a running
   late-join room is seated in the gallery and handed the last `launch` again
   (`late: true`), `launch` carries `open_slots`, a late-join gallery may exchange
   ICE signals and does not block a rematch's host-relay decision. Details in the
   server's `docs/WS_LOBBY.md` ("Join in progress").
10. **NHL94**: nothing new. The sim sees a seat that becomes human mid-play; the
    existing `drop_join_gate` (`game/nhl94_seats.c`, `$FFE302`) holds its player
    for the whistle. No new RAM, no new `game.toml` site. Demo hash unchanged.

**Unchanged server.** The client sends `late_join` in `create` (ignored), reads
`late_join` / `in_progress` from room rows (absent -> 0), sends `match_state`
only if the room view said `late_join`, and plans compactly (no open seats) unless
the room says so. Verified: `late_rooms.sh oldserver` against the production-era
server binary.

### Wire / API summary

* recomp-net wire stays version 6; new ids 32 `SEAT_CLAIM` (`want_slot`, sim tick)
  and 33 `SEAT_GRANT` (`target wire id`, seat or 0xFF + reason, `A`, gen, D).
  A v6 peer without them ignores both.
* `rnet_session_*`: `declare_seat_add`, `occ_barrier`, `request_seat`, `claim_status`,
  `absent_mask_at`, `hub_rebind`, `hub_add_agent`, `observer_live_tick`,
  `late_join_counters`; `rnet_rb_driver_promote_observer`; lobby client
  `rnet_lobby_set_late_join_pref`, `rnet_lobby_room_late_join`,
  `rnet_lobby_ice_take_late`; `RecompLauncherCNetplayLaunch.late_join / late_launch /
  open_slot_mask`; `GenesisNetplayConfig.late_join / late_launch / open_mask`.
* Env: `GENESIS_NET_LATE_JOIN`, `_LATE_LAUNCH`, `_OPEN` (hub tests), `_LATE_SEAT`,
  `GENESIS_NET_LATE_JOIN_OFF` (test: ignore the room's late join).

### Tests

All on this build (macOS arm64, Release, ICE on), a LOCAL `recomp-net-server` on
127.0.0.1 (the branch `eashl-latejoin` build on :18761, the unchanged production-era
binary on :18762); nothing was deployed anywhere.

**recomp-net unit** (`ctest`, own build dir, ICE off): new `latejoin_test` passes (5/5
runs): open seats seeded identically with nothing on the wire; the ADD with nobody
behind it cancelled by the 6 s timeout; a spectator replaying 960 ticks from the
logs (3840 rows, all equal to the closed-form truth, far past the 128-row ring);
claim -> grant -> promotion -> its rows real from wire `A` under the seat id (LAN hub
rebind); the joiner leaves (BYE) and a **second** spectator takes the same seat (the
stale hub identity is replaced, the old occupant's BYE does not drop the newcomer).
Rest of the suite 38/38 of the non-excluded tests (`occ_schedule_test`,
`session_observer_*`, `rb_driver_test` 276 s alone, ...); `session_state_test` fails and
`lobby_ws_backlog_test` / `lobby_client_test` do not run / link, exactly the known
upstream list (`tools/upstream-sync/known-failures.txt`); `state_replay_test` flaky,
passed. recomp-ui `ctest` 79/80 (the known `recomp-ui-launcher-netplay-handoff` does
not link). Server `cargo test --release` 205 passed (202 + 3 new in `ws_lobby.rs`).

**Online, through the local lobby server** (`tools/net/late_rooms.sh`,
`late_grade.py`: every peer's TIMELINE digests identical over everything shared, the
same occupancy table on every peer, nobody ended or refused, desyncs 0). Final run
after the last code change:

| Scenario | Result |
|---|---|
| `late4`: 4-seat room, 3 players, open seat 3, a late peer after ~40 s (tick > 600), claim, mid-play | PASS: 4 peers, 83 shared TIMELINE ticks (t=5000), 0 mismatching; ADD `seat 3 PRESENT from wire 2403`; the joiner replays ~2400 ticks in ~2.5 s |
| `faceoff`: joiner early (claim before the opening faceoff) | PASS: 70 ticks; human mask 4 from the first sample after the faceoff (the takeover is immediate at a faceoff) |
| `whistle`: mid-play join, 17 000-tick match (separate run) | PASS: 283 shared ticks, 0 mismatching; seat present from wire 2415, **human mask 3 -> 4 at the first whistle after it** (trace tick 3360), not at the join |
| `jdj`: join, the joiner leaves (BYE), a second late peer joins the freed seat | PASS: 108 ticks; events seed, ADD@2338, DROP@3307, ADD@4792 identical on every peer |
| `lag`: `RNET_SIM_LATENCY_MS=60 JITTER=20 LOSS=1%`, forced-mispredict injector on guest 1 | PASS: 83 ticks, ~330 episodes, 0 desyncs; ADD@1744 |
| `two`: 5 seats / 3 players, **two** late peers at once | PASS: second claim answered BUSY while the first is agreed, then seat 4; ADD@2465 and @2490; 83 ticks |
| `noroles`: open seat past the end of the host's seat list | PASS: claim refused (NO_SEAT), the late peer keeps watching in sync (18 shared ticks), no PRESENT event |
| `oldserver`: the same room against the UNCHANGED server binary | PASS: the room runs as before (compact plan, 3 slots, no open seat, 50 ticks identical), the late peer finds nothing in progress and never joins |
| LAN hub, no lobby (`tools/net/latejoin_hub.sh`) | PASS: host + 2 guests + a spectator process joining at tick ~2500, 99 shared ticks identical, `joy` unchanged until a whistle |

Existing batteries on the same build: `tools/net/drop_rooms.sh` **all 8 scenarios PASS**
(`grace4` 91 ticks, `crash4`, `crash3`, `lag4`, `two6`, `host2`, `seats2` as before, and
`spect` -- which could not run before -- now completes: a late-join room's gallery is
allowed to negotiate ICE); `tools/seats/lobby_ready_test.sh` all ok.

**Offline** (`tools/seats/run_checks.sh`, `drop_checks.sh`): lock2 `84C62F7EC823E795`, lock4
`51B3D2BD181C4A47`, callpass `E436CAA161B9EFF8`, callpass2 `9360A471FD0CA2C8`,
**Demo `80CE87C28143ADBF`** (= the integration base's `checks-int4/base.txt`: nothing in
the sim moved), rb_probe 99 / 209 / 139 PASS divergences=0, 2-process netplay
TIMELINE 66/66 identical (`E86D38742516C780`, `675410A9241DCDE5`). `drop_checks.sh`:
absent_a == absent_b `A19852F1F2090832` (`joy=90C -> 80C -> 00C`, rb_probe 139 PASS),
join_a == join_b `338F0E05D54A840C` (rb_probe 106 PASS), pause_orphan
`2F8F28F9EBCCD332`. These three are the new baselines of this tree; the figures in
stage B above and the earlier version of this section (`DFB3450E0C17DA07`,
`57A1B206D2DB114A`, `97EEF55A4F202882`) were taken before the integration moved lock4 (`A94E...` ->
`51B3...`); run twice they agree, and the nothing-moved evidence is the Demo / lock /
callpass set above -- **I did not rebuild the previous commit to diff the drop hashes
(UNVERIFIED that they equal the older ones, which was never expected)**.


### Known limits / what is not done

* **Not seen on a screen.** No display here: the `[in progress]` row marker, the
  catch-up (nothing is drawn while catching up -- the window keeps its last frame; the
  state is in the log: "late join: replayed N of M ticks") and the joined player's own
  view / name tag were never looked at. A progress bar / "joining..." overlay, and a
  seat / role picker for the joiner (today: any free seat that has a role, or
  `GENESIS_NET_LATE_SEAT`), are TODO.
* **Rollback rooms of 3+ seats, HOST_FIRST plans, ICE host hub only.** Two-seat rooms
  keep the old rule, delay-sync rooms and `host_spectates` rooms never get open seats,
  the SEAT slot policy and LAN / Direct IP rooms are unchanged. The LAN-hub path is
  tested (script, unit test) but is not reachable from the launcher.
* **Table size.** `RNET_OCC_MAX` 64 events per match: one per open seat at launch plus
  one per drop / add. A match with more than ~50 joins and drops stops scheduling
  (a drop is then not declared; the seat stays silent) -- it would need a compacting
  table. One join at a time (BUSY, retried every 500 ms); a joiner more than 160 ticks
  behind live is told BUSY too.
* **Memory.** The authority's log is 5 bytes per tick per seat (a 216 000-tick match,
  12 seats: ~13 MB); the seats log their own row only. A hard resync frees it.
* **Time.** Catch-up ran ~1000 ticks/s here (0.7-1.0 ms/tick): a full 3 x 5 minute game
  is about a minute. The authority holds sealing for the barrier only while the ADD is
  un-acked (observed 1-33 ms on loopback; up to the 6 s cancel on a vanished joiner).
* **WAN.** Only loopback, simulated latency / jitter / loss, and ICE host candidates
  were exercised; a joiner behind srflx / TURN uses the same agent code as the waiting
  room but was not run that way. The ADD lead (112 ticks ~ 1.9 s) assumes the grant
  round trip fits in ~1.5 s.
* **After the match.** The joiner is still a gallery member in the server's room table
  (the seat was taken inside the match, not in the lobby): back in the waiting room the
  host sees it as a spectator and can move it into a seat with the existing controls.
  `match_state false` is sent when the host's match ends.
* **Roles.** A joiner can only be given a seat whose logical player has a role under the
  host's sealed seat list (`netplay_seat_label`; `noroles` shows the refusal). Hosts that
  want every position claimable must use the Full 6v6 / 4v4 list (plan item 7).
* **Not exercised**: a joiner that dies between GRANT and its ack (unit test: the 6 s
  cancel), leaving during catch-up, the host leaving mid-join, 12-seat rooms, mods
  (the build has none), Windows, a server restart mid-match, spectators of rooms that
  are not late-join (unchanged code path; `drop_rooms.sh spect` passes).
* **Server not deployed.** Everything in `recomp-net-server` is on branch
  `eashl-latejoin` of the private copy, tested locally only. The production server
  keeps running the client (verified with the older binary) with late join simply
  unavailable.
* **Merge notes**: `rnet_lobby_client.c` and `lobby_client.h` are touched again (late
  join fields / ops; the D-stage `manual_ready` lines and the member-build-data work
  live in the same file); `plan_session_slots` gained a parameter (the LAN-room include
  passes 0); `GenesisNetplayConfig`, `RecompLauncherCNetplayLaunch` and
  `RecompLauncherCNetplayLobby` grew append-only fields.


## Review fixes (branch `fixnet`)

Two independent reviews of the drop-in / join-in-progress code (recomp-net session and
the engine runner) were checked finding by finding against the code; real ones were
reproduced with a test where practical and fixed minimally. Reports: `eashl/review/rv2.out`
(recomp-net) and `rv3.out` (runner).

### Fixed

| Finding | Fix |
|---|---|
| rv2#1 a 2-seat room under the drop policy dropped its guest from **wire 0** (the contiguous watermark was kept only for 3+ seats or observers) | `store_remote_frame` keeps the watermark whenever `seat_loss_timeout_ms != 0`; a 2-seat room without the policy keeps the old acks. Test `occ_harden_test` (before: drop wire 0, after: where the guest's rows ended). The runner never turns the policy on for 2 seats (stage B), so this was a library-level hole, not reachable from the engine today |
| rv2#2 any spectator could stall the match and fill the event table with claims | `occ_on_claim`: a replay position (`obs_tick`) ahead of the authority's tip + 64 is refused (STATE); after an abandoned join (not confirmed in 6 s, or the joiner left while it was agreed) claims are answered BUSY for 3 s; after 8 abandoned joins the match takes no more (STATE). `latejoin_test` room 3 forges claims from raw UDP sockets: before, 32 joins were taken and the table filled to 64 events; after, 8 joins, 17 events, a seat can still be dropped |
| rv2#3 a full table made a departed seat un-droppable / the cancel could not be recorded | an ADD is refused unless 8 table entries stay free (`RNET_OCC_RESERVE`), so a drop and the cancel of an unconfirmed ADD always fit; `occ_declare_drop` that cannot record sets `peer_gone` (legacy end of the match) and says so; the runner's host side maps that to "A player left and the match cannot record it" |
| rv2#5 the ADD barrier used the authority's own prediction cap | `rnet_rb_driver.c`: the barrier holds for the maximum cap (32), as `RNET_ADD_LEAD_TICKS` assumes |
| rv2#6 + rv3#4 the silent-seat rule needs a second live guest, so the last remote human crashing ended the match | new `rnet_session_set_link_ok()`; the runner feeds `rnet_lobby_connected()` (the lobby websocket is pumped every frame) from `genesis_netplay_match_should_end`. With that evidence a lone silent guest is dropped to the CPU after `GENESIS_NET_DROP_MS` and the host plays on; "Lost the connection to every player" now fires only without evidence (LAN room, dead websocket), as before. Tests: `occ_harden_test` (negative control without the hint, drop with it), `drop_rooms.sh lone3` online |
| rv2#9 a peer's oversized INPUT frame switched the join log off for everybody | in a late-join room a frame wider than the log row (4 bytes) is ignored in `store_remote_frame` (logged once); only the room's own first rows can disable the log |
| rv2 small: OCC_ACK with `gen > occ_n` released the barrier | ignored |
| rv2 small: a stashed late launch survived host create / room leave | cleared in `rnet_lobby_leave` and on `created` |
| rv3#1 a late joiner's `slot_port` / `occupied_mask` / `open_mask` came from its own launch | `genesis_netplay_set_seat_plan()` seals `slots= ports= occ= open=` into the config image (`genesis-config/3`, line `seats=`), which is the content fingerprint every peer's IDENT is judged on, so a joiner (or guest) whose plan differs refuses the match ("MOD SETS DIFFER") instead of replaying a different one. The lobby server already hands a late joiner the room's *last launch* (`late_launch_json`), so on a conforming server they are equal; this makes a difference detectable |
| rv3#3 `late_promote` ignored a failed `genesis_netplay_rb_promote()` and trusted the granted slot | the slot is bounds-checked (`0 < slot < slot_count`) and a failed promote logs, sets a message and returns to the lobby instead of announcing "JOINED" |
| rv3#5 a refused claim's "no free seat" text stood in for the real end reason | separate buffer (`genesis_netplay_claim_message()`); `genesis_netplay_refusal()` no longer returns it |
| rv3#6 the game's config image could be cut silently (`GenesisSessionConfig.game` is 96 bytes) | `capture_local_session_config` asks the hook into a 512-byte buffer and sets `game_overflow`; hosting (`create_with_relay_policy`) is **refused** with the full image in the log; a guest does not adopt a host line longer than it carries (`caps_parse`). The server's own limit is the whole caps JSON (4096 bytes, `sanitize_match_caps`), not `gen_game` |
| rv3#8 the no-render catch-up left the window blank with no end | window title shows "catching up N%" (twice a second); the join is abandoned with a message when the host sends no live position for 30 s or the replay does not get near live in 300 s (a 20-minute match replays in about a minute) |

### Not a bug / not changed

* **rv2#8 (hub identity re-bound at claim time).** The joiner's seat-named packets must pass
  the hub from the moment of the grant (it plays as the seat at once), so the identity has to
  move then. The spectator-named packets it stops sending are only claim retries (answered by
  the authority's 100 ms grant re-send, which needs no spectator identity) and OBS requests it
  no longer makes. If the ADD is cancelled the cancel event is broadcast to the seat index too
  (`occ_send`), the joiner sees itself absent (`dropped_self`) and leaves for the lobby; a
  stranded address cannot claim again (its packets name the wrong index), and the next claimant's
  rebind clears the stale identity (second-joiner test).
* **rv3#2 (SRAM for a late joiner).** The host's tick-0 SRAM transfer is kept (`served_buf`) and
  re-served to late observers on their STATE_ACK; a late joiner logs "applied host SRAM (16384
  bytes)" (seen in every `late_rooms.sh` run). It is the tick-0 image, not the host's current one.
* **rv3#7 (`h_sanitize` lets `RNET_RB_ROW_ABSENT` through).** Tried: stripping the flag from
  peers' sealed rows. It **forked** rooms whose episode spans a join (`late_rooms.sh jdj`, `lag`,
  `two` all failed with desyncs): a seat's owner-sealed rows below its join wire are flagged
  ("absent") and that is how the peers' replay agrees on the schedule. Reverted, with the reason in
  `rnet_rb_driver.c`. A hostile peer can fork a room by many other means (it owns its rows).
* **rv3#9 (`GENESIS_NET_MANUAL_READY=0` dead).** It is read by recomp-ui
  (`manual_ready_env`, prefix `GENESIS_NET_` from `legacy_env_prefix`); only the comment in
  `genesis_host_lobby.c` is clarified.
* rv2 small: `lobby_update` is dropped while a late launch is pending **on purpose** (the launch's
  seat table must not be replaced by the room as it is now); the spectator-IDENT `ident_owed` is
  paced by the spectator's own IDENT timer.

### Left open

* **rv2#4 observers are not covered by the ADD barrier.** A spectator learns the ADD from the
  authority's 100 ms re-broadcast (>= 18 chances before wire A, which is >= 112 ticks ahead) or its
  own 500 ms request; losing all of them forks it. The proper fix carries the authority's `occ_n`
  in a packet the observer's lockstep must reach: a wire-format change (version bump), out of scope
  for a minimal fix.
* **rv2#7 OCC / SEAT packets carry no era.** Needs an epoch field on the wire (same reason). The
  only symptom today is the "DISAGREES" log line on a stale event after a rematch resync.
* **rv2#2 residual.** The claimant is not checked against the lobby's spectator list inside the
  session (it has no lobby); on a LAN hub any new UDP address can pin a free wire id and claim.
  The abuse is bounded (claim ticks, cooldown, 8 abandoned joins, reserved table entries) but not
  prevented.
* Seat-plan sealing refuses a mismatching joiner; it does not **adopt** the host's plan. A joiner
  whose lobby view differs simply cannot join that match.

### Tests (this branch)

`recomp-net` `ctest` (own build dir, ICE off): `occ_harden_test` (new), `latejoin_test` (new claim-abuse room),
`occ_schedule_test` pass; only the known upstream failures (`session_state_test`, `lobby_ws_backlog_test`,
`lobby_client_test` not built on macOS). `tools/net/drop_rooms.sh`: all 8 scenarios PASS plus the new `lone3`
(guest 2 leaves, then the last remote human crashes: the host logs `seat 1 ABSENT ... silent (connection lost)`
and plays on to sim 6501, no "peer gone"). `tools/net/late_rooms.sh`: late4 faceoff jdj lag two noroles whistle
oldserver PASS. `tools/seats/plan_mismatch.sh` (new: two LAN peers with different seat plans, both refuse with
`mod_set_mismatch`). `tools/seats/lobby_ready_test.sh` all ok. `tools/seats/run_checks.sh`: Demo `80CE87C28143ADBF`,
rb_probe 99 / 209 / 139 divergences=0, 2-process netplay TIMELINE identical (the seat plan line matches on every
peer); lock2 `84C62F7EC823E795`, lock4 `51B3D2BD181C4A47`, callpass `E436CAA161B9EFF8`, callpass2 `9360A471FD0CA2C8`
(the baselines of the integrated tree, above). No simulation code changed.

## On-screen check (2026-11-26)

Three windowed copies (host + 2 guests, each wrapped in its own `.app` so it could be
screenshotted) on a local `eashl-latejoin` lobby server: waiting room, build picker, Ready,
Swap, a dropped guest, and join in progress, looked at on screen. What was wrong, and the fix:

* **Join in progress failed in a room that started with ONE guest** (host + 1 guest, open seats):
  the joiner stalled at tick 0 on `startup_identity_wait` and gave up (`startup_identity_missing`).
  With a single guest the session pins its rollback control to that peer (`rb_peer_slot`) and
  dropped the spectator's IDENT, so the seats never answered it. The headless late-join rooms all
  start with 2+ guests, so they never hit it. `rnet_session.c` now lets a spectator's IDENT (only
  that op) past the pin. Seen fixed on screen: identity from both seats, catch-up, seat 2 taken.
* **Ready was cleared for everyone whenever anyone joined or left** (the server clears all ready
  on a roster change; harmless while ready was automatic). The lobby client now remembers the
  player's own Ready and gives it again after a clear when its own seat is unchanged; a move or
  swap of its own seat still asks for it again (`rnet_lobby_client.c`, `g_ready_intent`).
* **The Ready button did not fit** the Status column (showed "Re"). Your own row is now a
  text-high checkbox with a visible border; other rows still read Ready / Not ready.
* **Seats past the host's position list showed nothing**; a player who sat there controlled nobody
  without being told. They now read "No position" (muted, with a tooltip, never offered to take).
  `GameSpec.netplay_seat_label` returns 2 for that; join in progress still only offers seats that
  return 1.
* **Catch-up was a black window** (progress only in the title). It now draws "Joining the match /
  Catching up NN%" with a bar, ten times a second (`main.c`, 5x7 caption font, `%` glyph added).
* **A player in a "No position" seat had to press Ready** to let the host start, for a seat
  that plays nothing. Its client now readies it itself (`recomp_netplay_host.c`,
  `local_seat_has_no_role`) and the row shows "Ready" with no box to tick. Moving to a seat with
  a position asks for Ready again as usual.
* **The picker's ratings were bare numbers** ("spd 4 off 5 ...", goalies "saves 6 6 5 5"). They
  are now named on the ROM's 0-6 scale: "Speed 4  Off 5  Def 4  Shot 2  Check 2  (of 6)" and
  "Glove L 6 R 6  Stick L 5 R 5  (of 6)" (the order read from the ROM's save table at `$15630`).
  The pick row's `detail` grew from 48 to 80 bytes on both sides (`game_spec.h`,
  `recomp_launcher.h`; `genesis_host_lobby.c` asserts they match).
* A joiner whose claim is refused (no free seat with a position) now says so in the window title;
  `startup_identity_missing` in the lobby reads as a sentence instead of the code.

Looked right as built: the picker (search, team tree, goalie-only list for G, Enter picks the top
hit, Team default), build names on rows for every member, the Swap request dialog (both roles
named), the host's "Waiting for X to press Ready" line, the `[in progress]` row and its tooltip,
the dropped guest becoming CPU, and the version check refusing a joiner on a different build.
Build names also show in the game's own menus (the goalie banner read "33 Patrick Roy").
