# Player builds (real NHL 94 players for the position lock)

Each human chooses ANY player of ANY team's NHL 94 roster as the build for his seat: the seat plays with that
player's real ratings, jersey number and name. A player has a **skater build** and a **goalie build**; the one
used follows the seat's role (a G seat uses the goalie build and lists only goalies, every other role the
skater build and lists only skaters). **Default (no pick) = the team's 1st-line player for that role** (ROM
line set 1), resolved at game start from the actual teams, so even after line changes the human's player plays
with the 1st-line player's attributes and name. The same real player may be picked by any number of seats
("every skater is Gretzky").

Files: `game/nhl94_builds.c/.h` (everything), `game/nhl94_roster_table.c/.h` (GENERATED, checked in),
`tools/builds/gen_roster.py` (generator + `--check`), `tools/builds/check_builds.py`, `tools/seats/builds_*`,
`tools/seats/test_roster_table.c`. Engine side: `GameSpec.lobby_pick_*` + `genesis_host_lobby.c`
(segagenesisrecomp), `seat_pick_*` callbacks and the popup in recomp-ui, `rnet_lobby_set_member_data` in
recomp-net. Design record: `r4.out`; this file is the as-built description.

## 1. What the ROM holds (verified by `gen_roster.py`, which asserts all of it on the real ROM)

| Item | Value |
|---|---|
| Team table | `$00030E`, 28 x u32 (`$30A` is a zero long). ANH BOS BUF CGY CHI DAL DET EDM FLA HFD LA MTL NJ NYI NYR OTW PHI PIT QUE SJ STL TB TOR VAN WSH WPG ASE ASW |
| Team block | six u16 offsets from its base: Playerdata, Palettedata, Teamname, LineSets, ScoutReport, GoalieOdds |
| Roster | records until a length word of 2. **28 teams, 684 players, 65 goalie-class**, 18..25 per team (max 26) |
| Record | `u16 len` (counts itself, the name and its even pad), name ("First Last", NUL padded), **8 rating bytes**; next record at `p+len+8`. `len <= 40` for every record |
| Line sets | 8 lines x 8 bytes at base+LineSets, 1-based roster numbers: `0 G, 1 LD, 2 RD, 3 LW, 4 C, 5 RW, 6 extra attacker, 7 zero`. Line 1 is "NLC". Every team's line 1 names 6 real players |
| Team name strings | length-prefixed: city, abbreviation, nickname, arena |

**Goalie class = rating byte b5 == 0.** The design guessed `b4 == 0 && (b5&F) <= 1`; the real ROM disproved it
(goalies carry the handedness bit in b4, e.g. Fuhr `...0100...`). `gen_roster.py` checks the rule against the line
sets: every player on a G line has b5 == 0, every player on a skater line b5 != 0, nobody is on both.

Rating bytes (b0..b7) and what `setplayer` ($015AA4) derives (object fields +$67..+$76; `AC(n) = clamp(5n +
trunc(hotcold/3), 0..30)`, hotcold = the on-ice roster slot's `tmHotCold` byte; **all verified against traced
objects by `check_builds.py`**):

| byte | field(s) |
|---|---|
| b0 | jersey (BCD) -> `rostnum` +$6F |
| b1 | hi*8 -> `weight`; lo -> `legstr` AC |
| b2 | hi -> `legspd` AC; lo -> `aioff` = fold(clamp(AC + PP + PK + home/away + 3rd period)) |
| b3 | hi -> `aidef` = fold(clamp(AC + home/away)); lo -> `shotspd` AC |
| b4 | hi -> `checking` clamp(AC + 3rd); bit0 -> `handed` = bit0^1; bits1-3 -> `fighting` |
| b5 | hi -> `stickhand`, lo -> `shotacc` (both + PP + PK + home/away) |
| b6 | hi -> `endurance` AC; lo -> `spodds` clamp(AC + 2*3rd) |
| b7 | hi -> `passacc` clamp(AC + PP + home/away); lo -> `aggress` AC & F |

`fold(c) = (((c>>1)^F)+F)>>1`. Goalies: b6/b7 are the four save ratings (glove L/R, stick L/R); b4/b5 are 0. A goalie
(position 0) gets none of the PP / PK / home-away / 3rd-period bonuses (`setplayer` skips them). Those bonuses and
the hot/cold value belong to the **on-ice slot and team**, not to the build: a build plays with its team's bonuses.

## 2. How it is applied (all simulation state is guest RAM / ROM / the sealed image)

Every attribute of an on-ice player comes from `setplayer`, which runs at each faceoff (`forcepldata`), line change,
penalty release and penalty-shot set-up. Nothing else re-reads ratings during play. Hook sites (`game/game.toml`,
dispatched from `nhl94_spec.c`):

| Site | Hook | What |
|---|---|---|
| `$007806` restoreteams `.r` | `nhl94_builds_latch()` (called BEFORE the 4v4 format hook, which returns first for this pc) | copies each seat's record from the ROM into the guest block; idempotent (fires twice: home, away) |
| `$015BA6` setplayer, `move.b (a0),rostnum(a3)` after `subq.w #pldsize,a0` | H1 (pre-instruction, continues) | `a0` := the seat's build bytes when the object's (team side from pflags bit 6, `position` +$34) is a seat role; maintains `slotmap` |
| `$018BC8` GetPlayerNamePointer | H2 (REPLACED, `rts`) | for a roster slot that last played as a build: `a0` = the build's name record, `d0 = -1` (the `dbf` exit); covers the goal / penalty / injury / stars boxes (FormatPlayerName & co. all go through it) |

**Keyed to the role, not the slot.** The seat is locked to a role, so a line change gives the role to another roster
slot; `setplayer` re-runs for him with the same `position` and the build re-applies (test 4). H2 keeps a per-slot
map so the banner names follow.

**Guest-RAM block `$FFE000..$FFE28F`** (the unused stack bottom: `NHL94_SEATREQ` starts at `$FFDF00`, the format
block ends `$FFDFE4`, the deepest stack seen in play is `$FFFEC9`; I own `$FFE000-$FFE2FF`). In every snapshot and digest.

```
+0x00 u16 magic $B1D0 (0 = no builds)
+0x02 u8  rolemap[2][8]   seat+1 by [side][role], 0 none
+0x12 u8  slotmap[2][26]  seat+1 by [side][pnum]  (banner names; zeroed by every latch of the side, on purpose:
                          restoreteams puts the whole roster on the bench, so "last played as a build" restarts
                          with it and H1 refills each slot as the player re-enters. Keeping the map across latches
                          carries a finished game's mapping into a rematch with the same teams: the 100000-frame
                          `builds_ps150k.input` run then differs from the clearing one after the 2nd game starts)
+0x48 "NHLBLD!!"          canary
+0x50 entries[12] x 48 B  verbatim ROM records [u16 len][name][8 attr], entry = seat index
```

**Latch details.** For each seat of the side in `a2`: the pick's record (class checked on ROM bytes: a goalie only
on a G seat; a misfit falls back to the default, logged) or, with no pick, line-1 slot of the role from the team's
own ROM block (`tmdata(a2)`). Highlights call `restoreteams` for OTHER games' teams (`scoreboard94.asm StartHL2`,
before `gmhl` is set: **gmhl is NOT set at that call**): the latch refuses unless `tmteam(a2) == HomeTeam/VisTeam`
and `tmdata(a2)` is that team's ROM block, so the block keeps the real game's builds. With builds off / no seats, a
non-zero magic from an earlier game is cleared; otherwise nothing is ever written.

**Gating.** H1 and H2 run only while `nhl94_seats_lock_active()` (new export of the lock's own gate: seats
configured, not in highlights (`gmhl`), not in a penalty shot / shootout (`BA_PS_flags` bit 2), not on the
scoreboard rink). **Penalty shots (verified by reading the code):** `BA_PS_flags` bit 2 is set at `$EDB0`
(`puckshootout .selectskater`) *before* `assreplace` starts `puckpenshot`, whose `setpersonel` /
`SetupPenaltyShot` run `setplayer`; and `SetupPenaltyShot` only calls it for an object with `position < 0`, which H1
ignores anyway. So the shooter chosen by `PickBestAvailableSkater` and the defending goalie keep their ROM ratings;
play afterwards re-applies the builds at the next faceoff. Highlights: `.setteam` (the highlight's `setplayer`s)
runs after `gmhl` is set (`scoreboard94.asm:886`).

Not covered (shown as in the ROM, like the in-game scouting screens do today): player cards, stats pages, scouting,
`PickBestAvailableSkater`'s rating of the whole roster, the penalty-box jersey print (`PrintPenaltyRow`). Stats
(goals, assists, shots) are credited to the roster slot that played; the banners show the build's name for that slot.

## 3. Where the tokens come from, and the session image

Token = `team*32 + roster slot` as two base-32 characters `0-9A-V`; `--` = default.

The builds ride in the **seats value** of the session image as a `+B` segment (grammar independent of the 4v4 /
pass-assist markers: a segment starts at `+B` and ends at the next `+` or the end of the value):

```
nhl94.seats=*ekdjflbhciag+B<T4><tok per seat in seat order>      e.g. nhl94.seats=home:C,away:G+BE261H2M0
```

`T4` = low 16 bits of the roster-table CRC (4 hex digits); adoption refuses a different table or a bad token. An
older build rejects the `+` and REFUSES the launch (same reasoning as 4v4). **Presence rules:** with seats configured
the segment is always written (all `--` = defaults on); no segment = builds OFF for the session (`NHL94_BUILDS=off`,
the pre-builds behaviour and baselines); no seats = Classic, never builds. `GenesisSessionConfig.game` grows
48 -> 96 bytes (12 compact seats + builds = 55), `RNET_LOBBY_CAPS_EXT_BYTES` 64 -> 128 (the caps JSON limit on the
server is 4096 bytes total; `gen_game` is otherwise unconstrained -- verified in `sanitize_match_caps`).

Sources, per seat, on the machine that builds the image (the host online): `NHL94_BUILDS` env > `--builds` > the
lobby member table (every seated member's picks, the host's own row included) > this machine's ini picks for the local
seat (offline / LAN only: in a room the member table is the whole truth, so an empty seat has NO pick). While netplay
runs every peer latches the ADOPTED tokens; offline the local sources are read live.

**Which seat number.** The image's seat i, the sim's seat i (`human_mask` bit i, `nhl94_seats_local_seat`) and the
waiting room's seat `wire` are all the LOBBY seat: the host-first plan (`plan_session_slots`) makes the host session
slot 0 whatever seat it holds, but `slot_port[]` maps every session slot back to its lobby seat and the engine drives
pad / `human_mask` bit `slot_port[slot]` (`genesis_netplay_sim_input`, `genesis_netplay_local_player`). So the member
`slot` (lobby seat) IS the seat here and no lobby-seat -> session-slot mapping is needed. What did go wrong: with the host
in lobby seat 1, the "local seat" fallback (seat 0 before netplay starts) gave the host's skater pick to the image's
seat 0 while nobody sat there (a CPU seat played with a human's build). Fixed in `local_token` (`rnet_lobby_in_lobby()`
skips the fallback); drill: `ROOM_SEATS=3 HOST_SEAT=1 GUEST_SEAT=2 SEAT3=away:C EXPECT=... tools/seats/builds_lobby_test.sh`.

Offline / test controls:

```
NHL94_BUILDS=off | key=pick[,key=pick...]   key: home:C | away:G | ... (that seat) | S (all skater seats) | G (all goalie seats)
                                            pick: default | TEAM:<part of the name> | TEAM:#<jersey>   (ambiguous / unknown = reported, ignored)
--builds <same>                              e.g. --builds "home:C=PIT:Mario Lemieux,away:G=TOR:Potvin,S=ASE:Messier"
NHL94_MEMBER_BUILD="S=BOS:Bourque;G=TOR:Potvin"   TEST: this process's lobby member data
NHL94_BUILDS_TRACE=1                         latch lines (with the record bytes), every seat object's +$67..+$76 when they change,
                                             name-hook hits, setplayer-in-a-penalty-shot lines, a summary at exit
```

Persistence: `nhl94-control.ini` keys `build_skater` / `build_goalie` (`TEAM:First_Last` or `default`), written by
the picker, read by `nhl94_mode_load` (which also announces them to the lobby).

## 4. Lobby data path and waiting-room UI

* **recomp-net** (`rnet_lobby_client.c`): `rnet_lobby_set_member_data(text)` (printable ASCII < 40 chars). A guest
  sends it to the host as a `signal` of type **140** (`RNET_LOBBY_SIG_MEMBER_DATA`; the server relays signals
  verbatim and stamps `from_player_id`, so a guest can only speak for itself; verified against
  `ws_lobby.rs handle_signal`), at once and every ~3 s while seated. The host stores it per player id and returns it
  as `RNetLobbyMember.game_data` (every peer sees its own row). Game string: `1;S=<tok>;G=<tok>`, both picks, so
  moving between a skater and the goalie seat needs no resend.
* **Host fold-in:** `nhl94_builds_suffix()` (via `nhl94_seats_config_image`) walks the member table (`slot` = seat) and
  takes each seat's skater or goalie token by its role; the host re-validates every token against the ROM.
* **Republish:** `host_game_watch_step` (recomp-ui `recomp_netplay_host.c`) rebuilds the caps at most twice a
  second and republishes when the game's ext bytes change. Guests auto-ready again (the existing behaviour on any
  republish); every client, late joiners included, then sees every seat's build in the caps.
* **UI** (`launcher_imgui.cpp`, function `np_seat_pick_ui`, one call from the seat row): on the LOCAL seat's row a
  button `Build: <pick> v` opens a popup: `Team default` button (id -1), a search box (focused; case-insensitive AND
  of words over name, team abbreviation, city, nickname and `#jersey`), the matches grouped under a header per team
  (headers collapsed while the box is empty), name + `#NN ROLE  Speed n  Off n  Def n  Shot n  Check n  (of 6)` (goalies: `Glove L n R n  Stick L n R n`) per row,
  **Enter picks the first match** (only once something is typed: an empty box lists the whole roster). Other members' builds show in the role label of
  their seat (`Home C: Gretzky (LA)`; nothing extra for the default), from the caps image.
* **Callbacks:** `RecompLauncherCNetplayCallbacks.seat_pick_current / seat_pick_query / seat_pick_set`
  (`RecompPickRow`), adapters in `recomp_netplay_host.c` over `RecompNetplayHostHooks.seat_pick_*`, implemented by
  `genesis_host_lobby.c` over `GameSpec.lobby_pick_current / _query / _set` (`game_spec.h`, `GamePickRow`).
* Online rooms only; a LAN / direct room has no caps, so there `NHL94_BUILDS` on both sides is the control and the
  rollback handshake refuses a mismatch (test 8).
* **Known limit:** at lobby time the real teams are unknown (chosen in the game's own menus after launch), so
  "Team default" says "the team's own player", not his name.

## 5. Name tags

`nhl94_tags.c` asks `nhl94_builds_seat_name(seat)` (guest RAM: the latched entry of the seat) and passes it to
`nhl94_tagname_for_seat_build` (`nhl94_tagnames.c`): the tag shows the build's LAST name (e.g. `LEMIEUX`,
sanitized, max 10) instead of the lobby name; Classic / builds off / not yet latched fall back to the lobby name /
`P<n>`. Two humans on the same build show the same tag.

## 6. Tests and baselines

`tools/seats/builds_checks.sh <build dir> <out dir> [peer root]` runs all of it; individual results of the last run:

| # | Check | Result |
|---|---|---|
| 1 | Regression: Demo (no seats) `80CE87C28143ADBF`; builds off `lock2` `3EF834BA226E6E28`, `lock4` `A94E00C87F121117` | identical |
| 2 | Default builds (1st-line player per role), `lock2` 4800 | `FBA0B8150BB0C3EE`, a = b. **Identity:** the seat trace (positions, velocities, pad state; 223 lines) is identical to the builds-off run (the state hash differs: the latch block is in WRAM) |
| 3 | Real override `home:C=PIT:Mario Lemieux,away:G=TOR:Potvin`, `lock2` | `99482F6AD5CBDC8B`; same with own view off and `NHL94_VIEWS`; `check_builds.py`: 2 of 2 objects match the ROM formulas |
| 4 | Line change (`forced_linechange.input`): seat's new object (pnum 2 -> 10) keeps Lemieux's fields and jersey `66` | OK |
| 5 | The same real player on several seats (`S=`) | OK |
| 6 | `rb_probe_seats.sh` 30:8 with builds (lock4, 4 seats) and default | PASS, divergences=0 (139 passes each) |
| 7 | Names: `demo_penalties` 4v4 + injected minors: H2 replaced the slot's name with the build's (trace `name request ... -> Wayne Gretzky`); tags show `LEMIEUX` (`builds_names.input` screenshots) | OK |
| 8 | 2-process LAN rollback (`NHL94_SEATS=home:C,away:C`): same builds `CF3660142176A68E` and defaults `A9831B866D66407D`, 66 TIMELINE lines identical, desyncs=0; negative control (different `NHL94_BUILDS`) REFUSED at sim=0 (`mod_set_mismatch`) | OK |
| 9 | Lobby signal path: real local lobby server, host picks a skater in its ini, guest a goalie; image `+BE261H2M0` published after "game configuration changed"; both latch Lemieux + Potvin; 43 TIMELINE lines identical | OK |
| 10 | Tables: `gen_roster.py --check`; `test_roster_table.c` (28 teams / 684 players / 65 goalies, token round trip, filter, picker rows grouped, set / revert, class refusal, image + label + adopt incl. bad table id / bad token, 12-seat image 55 bytes); `test_format_image.c`, `test_tagnames.c` still pass | OK |

Baselines for the seat batteries are the **builds-off** ones (`run_checks*.sh` export `NHL94_BUILDS=off`):
`tools/upstream-sync/baseline/*.hashes` are unchanged. With builds ON (default) the seat hashes change by design.

**Waiting-room UI** (not scriptable in CI): driven with the launcher's `LNG_SCRIPT` automation against a local
lobby server (`LNG_TEST_HIDDEN=1`, click-through netplay -> host -> create): the seat row shows `Home C: Gretzky (LA)`
from the ini pick and a `Pick build` button; the popup shows `Team default`, the focused search box and 28 collapsed
team headers; expanding a team lists name, `#jersey ROLE` and ratings; clicking a row sets it (`Home C: King (ANH)`),
writes `build_skater = ANH:Steven_King` to the ini and logs "game configuration changed - republishing". The launcher's
`text:` command crashes in SDL (not ours), so typing into the search box was exercised by `test_roster_table.c`
(`nhl94_builds_lobby_query`) and not by script; Enter-picks-first is by code reading.

## 7. UNVERIFIED items of the design, resolved

| Item | Resolution |
|---|---|
| ROM layout, 684 players, goalie rule | Verified by `gen_roster.py` on the real ROM. The goalie rule of the design was WRONG; b5 == 0 is right (section 1) |
| `BA_PS_flags` bit 2 before the penalty-shot `setplayer` | Yes: set at `$EDB0` in `puckshootout` before `assreplace` starts `puckpenshot`; and `SetupPenaltyShot` only calls `setplayer` for `position < 0`, which H1 ignores. No natural penalty shot happened in two 150000-tick demo games with seats (AI never fouled on a breakaway), so this is by code reading plus `NHL94_BUILDS_TRACE` instrumentation ("setplayer in a penalty shot" lines) that stayed silent |
| `gmhl` set at `restoreteams` from highlights | NOT set (`scoreboard94.asm:872` runs before `:886`): the latch is guarded by `tmteam == HomeTeam/VisTeam` and `tmdata` (section 2) |
| `AttributeCalc` per-attribute indexing | The attribute number is ignored: hot/cold is per roster slot (`tmHotCold + pnum*16`); the trace shows `hc` per object and `check_builds.py` reproduces every field exactly |
| Stack headroom for `$FFE000` outside gameplay | The block survived menus -> game, every game start and new Demo games in 150000-tick runs; the canary is written at each latch (no clobber detector beyond that) |
| Server limit on `gen_game`, signal 140 relayed | The caps JSON limit is 4096 bytes total (`sanitize_match_caps`); `handle_signal` relays any type to seated members (rate-limited); verified by the lobby test |
| Engine lobby selftest member naming | The host's and guest's member `slot` is the seat; the selftest needs no change (`builds_lobby_test.sh`) |

## 8. Limits / gaps

* Cards, scouting, stats pages keep the original roster; `PickBestAvailableSkater` rates the ROM roster.
* Hot/cold, PP/PK, home/away and 3rd-period bonuses are the slot's / team's.
* The roster table is only for the picker; a table / ROM mismatch (`table_ok`) disables the picker names, never
  desyncs (the latch reads the ROM).
* 4v4: roles are G/LD/LW/C only (RD/RW seats are refused by the seats layer already).
* Penalty shots, shootouts, highlights, the scoreboard rink use ROM ratings (lock inactive).
* A pick made after the host pressed START misses that match (every peer adopts the launch's caps).
