# Player lock: one human per on-ice player

Every human (a *seat*) controls exactly one on-ice player of NHL Hockey '94
for the whole game: a skater role or the goalie. Every player no seat claims
is the game's own AI. Code: `game/nhl94_seats.c`; hook sites: `game/game.toml`
(end of file); camera: `game/nhl94_view.c`.

## Configuration

**Launcher (normal use):** Mods -> Gameplay -> **Control mode**
(`game/nhl94_mode.c`, a feature appended to the engine's launcher mod
provider through `GameSpec.mods`; this game builds recomp-ui with
`RECOMP_UI_ENABLE_MODS=ON` for it). Options: **Mode** = Classic (cartridge) |
Position lock, **Format** = 6v6 (cartridge) | 4v4 (G, C, LW, LD per team; see
"4v4 format" below), **Preset** = 1v1 (home C vs away C) | Full 6v6 (12 seats) |
Full 4v4 (8 seats) | Custom, and **Seat 1..12 position** (Home/Away Goalie, Left D, Right D,
Left Wing, Center, Right Wing, or None for seats 2-12: the list stops at the
first None; a position already taken by another seat is refused with
"home:C is already seat 1"). Choosing a preset fills the seats and turns
Position lock on; Preset shows Custom as soon as a seat differs. Full 6v6 is
home:C, away:C, home:LW, away:LW, home:RW, away:RW, home:LD, away:LD,
home:RD, away:RD, home:G, away:G: seat k is lobby seat k online. Joiners
fill the first open seats (seat 1 the host, seat 2 the first guest, ...), so
alternating the teams centre-first keeps any number of joiners as even as
possible. Full 4v4 is home:C, away:C, home:LW, away:LW, home:LD, away:LD,
home:G, away:G and also sets Format = 4v4; Full 6v6 sets it back to 6v6.

**Picking a position in the waiting room.** The waiting room labels every
player seat with its role under the host's list ("Home C", "Away G"), open
seats included, and an open seat has a **Take** button (dragging your own row
onto it does the same). Taking a seat is picking that position; open seats
between players stay CPU. For a free choice of every position, host with the
Full 6v6 list and a 12-seat room. The labels come from the room's published
caps (`GameSpec.netplay_seat_label` = `nhl94_seats_label`, presentation only):
the seat list itself is still the host's sealed session image, adopted by
every peer before tick 0, so picking a seat changes who sits where, never
the list.
Saved in `nhl94-control.ini` next to `settings.ini` (the engine rewrites
`settings.ini` wholesale, so game keys cannot live there); read at start-up
through `GameSpec.load_settings`. Every change is applied and saved at once,
because the lobby captures the room's session image when the room is
created, before PLAY. Defaults: Classic, 6v6; seats home:C, away:C, none, none.

**Precedence** (logged as `nhl94_seats: control mode ... (source: ...)`):
`NHL94_SEATS` environment (when set; `off` = Classic) > `--seats <list>` >
launcher setting > Classic. Online the HOST's choice is the session's: the
image (`nhl94.seats=<list>`, nothing for Classic; when the readable list does
not fit the engine's 48-byte `GenesisSessionConfig.game`, i.e. beyond ~4
seats, the compact form `nhl94.seats=*<letters>`, one letter per seat,
`a + team*6 + role`: a..f = home G..RW, g..l = away G..RW, e.g. the 6v6
preset is `*ekdjflbhciag`) is adopted by the guest
through `GameSpec.netplay_config_adopt` (source `host's session config`)
and wins over the guest's own setting for that session; the guest's own
choice is restored afterwards. The format rides in the same value: 4v4 is
`nhl94.seats=4v4` (Classic), `nhl94.seats=4v4,<list>` or compact
`*4<letters>` (e.g. Full 4v4 = `*4ekdjbhag`), so a build without 4v4 refuses
the host instead of playing 6v6 (see "4v4 format").

- Seat *i* is logical player *i* of the simulation input
  (`GenesisSimInput.pad[i]`, `genesis_sim_pad(i)`) and netplay session slot
  *i* (default slot->player map). Seats 0-3 are the game's own controllers
  1-4; seats 4-11 are virtual pads (see "6 and 12 seats"). A seat at or
  beyond the engine's `GENESIS_SIM_MAX_PLAYERS` gets no input and stays AI
  (logged once: "this engine carries N pads per tick").
- A seat is human only while the sealed input's `human_mask` has its player
  (a device or script offline, an occupied slot online); otherwise its player
  stays AI. Offline Position lock with the default seats therefore means: you
  are seat 1 (home C), everyone else AI, until a 2nd controller is assigned.
- Roles: `G LD RD LW C RW`. Up to 12 seats (6v6, goalies included). A role
  may be claimed once per team.
- Offline scripts drive seat *i* with `PLAYER i+1`. In rollback netplay each
  process samples its own device (`PLAYER 1` in its script) into its own seat.

Menu setting: **Players = Two - Head to Head** is the recommended (and
tested) one. The lock rewrites `cont1team..cont4team` every frame, so control
does not depend on the menu, but Head to Head is the game's own human-vs-human
mode: `demoread` never treats it as a demo (in Demo, any non-START press on
pad 1/2 exits to the title until the first locked frame sets the pad teams),
records and pause work per pad, and both teams get the human line-change box
at faceoffs. Demo + seats also works once the puck is on the ice.

## What the hooks do

All the sites are `game_hook` pre-instruction sites. They act only when a
seat list is configured, and read nothing but guest RAM, the tick's sealed
input and the constant seat list, so every rollback replay and every peer
computes the same thing. No simulation state lives outside guest RAM (no
`rb_state_save` needed).

| site | routine | action | why |
|---|---|---|---|
| `$01A370` | `ReadJoyData` (sound94.asm, vblank) | **replaced** | Writes `PadRaw1..4` from `genesis_sim_pad(0..3)`: the game's active-low byte, opposite directions resolved with its `PadDirTable`. Seats 3/4 reach the game without port wiring or the 4 Way Play protocol; seats 1/2 produce the same bytes the port read did. Leaves `d0` = PadRaw2 as the original. |
| `$00A9D6` | `updateplayers` entry | enforce, then continue | `contNteam` := seat team (1 home, 2 away; 0 for no seat), `cNplayernum` := the seat's role object, `pfjoycon` only on those objects. An object losing `pfjoycon` is released as `restorepl` does (clear `pfjoycon`, set `pfna` unless pflags3 bit 3). The game then reads each pad and runs `doinput` for its player itself, including its own pad 3→1 / 4→2 swaps. |
| `$00C0BC`, `$00C0DA` | `setc1player`, `setc2player` | **refused** (rts) | Every pad switch funnels through these two: B switch (`chgplayer`), pass receiver (`passto`), one-timer, B-hold "take the goalie", penalty clear (`.clrplayer`), faceoff reselection (`ResetAndSelectPlayers`), penalty-shot setup. Pads 3/4 reach them through the game's own swaps. So the game never moves a human and never hands a pad an AI player. |
| `$00B0E8` | `doinput` entry | remap, then continue | If the pad's player has `pfgoal` clear (attacks the bottom goal), the direction code `d0` becomes `(d+4)&7` (180°, the same idiom the game uses for its horizontal rink, `addq #2`) and the raw U/D and L/R bits of `d1..d3` are swapped. "Up" is then always toward the opponent's goal, matching the per-player views, which turn that team's screen 180°. Computed from game RAM per player per frame, so it flips at each period. |
| `$00AD42`, `$00AE7C` | `updateplayers` `.t1` / `.t1cont` (pad 3/4 block) | swap a virtual pad in / out, continue | Seats 5-12: see "6 and 12 seats". |
| `$00D462` | `assgoaliectrl` `.cont2` | clear `pf3gctrl`, continue | The camera-distance rule (goalie more than 116/100 px from Hpos/Vpos → AI) is cancelled for a seat's goalie: the shared camera no longer decides who controls him. The out-of-range whistle, animation locks and `checkanim` stay. |

| `$015788` | `setpersonel` entry | **observation only** | Logs the caller's return address (and whether play is running) for the "setpersonel pin" decision; writes nothing. |

The control mode (`ctl_mode`) is OFF during intermission highlights of other
games (`gmhl`) and the horizontal scoreboard rink; PLAY otherwise; **PS** while
a penalty-shot / shootout attempt is live (`BA_PS_flags` bit 2). The lock stays
on in PS (the setc1player/setc2player refusal and virtual pads keep working);
only the targets change (see "Penalty shots and shootouts").

### The seat's player each frame

(Since the "always a player" work this is the seat's **primary** body,
`seat_primary`; a seat whose primary is missing may take a substitute, see
"Substitutes". `nhl94_seats_control_object(seat)` returns what the seat controls
this frame.) Scan the team's six objects for `position` (+$34) == role. Nobody (the seat
controls nothing, the player is AI) while:

- the role is not on the ice (boxed, short-handed role dropped, goalie
  pulled: `position` negative or missing);
- the object is `pf2unav` (skating to/from the bench or the box);
- a real line change is pending on it (`newpnum` != `pnum` or `newpos` !=
  role), unless he carries the puck or has a pending penalty. `check4bench`
  skips `pfjoycon` players, so a held player would never change lines; the
  release lets the game's own AI send him off.

When the role is back (next player of the line, or the same player back from
the box), the seat takes him the same frame. This is "lock to position".

## Call for pass

While an **AI teammate** carries the puck, pressing the seat's **B** (or a
6-button **X**; edge-triggered) asks him to pass to the seat's player.

- **Request state** lives in unused guest RAM at `$FFDF00` (bottom of the
  stack area `PALflag+2`..`Stack`: Begin's RAM clear stops at PALflag and the
  deepest stack seen in play is `$FFFEC9`): `+0` receiver SC+1 (0 = none),
  `+2` carrier SC, `+4` seat, `+6` frames left, `+8` u16[12] previous B|X per
  seat for edge detection (`$FFDF08..$FFDF1F`; seats 5-12 use `+$10..`). It is created in the `updateplayers` hook from the
  tick's sealed pads, so snapshots, digests and replays cover it.
- **Created** only when the puck carrier is on the seat's team, is not the
  seat's own player and is not human. A human carrier: nothing (counted,
  logged with `NHL94_SEATS_TRACE`). Opponent's or loose puck: B does what it
  did before (sweep check / nothing). One request at a time: the first press
  wins, seats are scanned in index order within a tick.
- **Dropped** when the carrier loses the puck, the seat's player leaves the
  ice, or after 60 game frames (~1 s).
- While a request is pending, B is masked out of the caller's `doinput` (it
  must not also sweep-check).
- **Carried out by the game's own code.** The AI carrier's assignment calls
  `chk4pass` each time its reaction timer (`aioff`) runs out. Hooks:

| site | instruction | action |
|---|---|---|
| `$00E456` | chk4pass `cmp.w #$C,d0` | the random "consider a pass" roll passes (d0 = 0; the RNG is still drawn) |
| `$00E464` | chk4pass `cmpi.w #6,SCnum(a3)` | the random candidate teammate is the caller |
| `$00E4F8` | chk4pass `eor.w d0,d1` | waive "no pass across the offensive blue line" (offside risk is the caller's) |
| `$00E502` | chk4pass `.oko cmp.w #$58,d0` | waive "no backward pass outside the attacking zone" |
| `$00BD5E` | dopass `tst.l d4` after the receiver loop | receiver a0 = the caller (dopass's score could pick a nearer teammate in the same direction); request cleared |

  Kept: the receiver must be on the ice, free and not animation-locked; the
  lane check (an opponent nearer than the receiver in the same 8-way
  direction = not now, retry at the next decision); the slot rule (no pass
  with the puck in front of the net); and all of `dopass`/`passto`: speed
  from the passer's rating, lead to the moving receiver, pass animation and
  sound, `passReceiverPlayerNum` (one-timer eligibility), interceptions.

**Verified** (`callpass1.input`, 1 seat home:C standing still, B every 45
frames; `callpass2.input`, home:C + home:RD):

- 22 calls → 13 passes, median 12 frames after the press (1..59); 7 of
  the 13 reached the caller (puckc = his object), the others were
  intercepted or picked up by another player in the lane. 9 drops: 6
  windows expired with a goalie carrier (he reaches `chk4pass` only when
  pressured), 1 expired with a skater whose 24 tries all failed the lane /
  slot checks, 2 carriers lost the puck first.
- 29 presses while the opponent had the puck and 33 with a loose puck made
  no request; with two home seats, 6 presses while the other seat carried
  the puck made none (`human_carrier=6`), and presses while a call was
  pending were ignored (`busy`).
- Same script twice: `callpass1` FAA72A99530568E6 both times, `callpass2`
  560C3425A02A9A91. `rb_probe_seats.sh` with `callpass1.input --seats
  home:C`: passes=209 divergences=0 asymmetric=0 PASS. Lock-only hashes
  (`lock2`, `lock4`, Demo) are unchanged.
- Netplay, 2 processes, rollback, seat 0 calling (`net_call_slot0.input`):
  66/66 TIMELINE digests identical, desyncs=0, 3 call-passes. With 60±20 ms
  simulated latency: 11 rollback episodes, 130/130 shared TIMELINE digests
  identical, the same 3 call-passes on both peers.

**Limits.** A goalie carrier rarely runs `chk4pass` (only when an opponent
is close), so a call to him usually expires. Timing follows the carrier's
reaction timer (typically 3..40 frames). No on-screen cue yet (the game has
no pass-target highlight; a view-side marker would be presentation-only).

## Aimed passing

EASHL-style passing for a human passer in Position lock. Code:
`game/nhl94_aim.c/.h` (pure integer picker, unit-tested by
`tools/seats/test_aimpass.c`) and `aim_pass_hook` in `game/nhl94_seats.c`.

**What the cartridge does.** `dopass` (`logic94.asm`, the `.findplayer` loop,
ROM `$BCD8..$BD56`) picks the teammate with the lowest `dx^2 + dy^2 +
anglePenalty`, where only `vtoa` octant offsets 0 and +-1 from `passdir` survive
(+-1 adds a flat 65536). So it is distance first, blind to humans, and the octants are
lopsided. `$BD5E` (`tst.l d4` after the loop) leaves `a0` = receiver, `d4` = its
score (-1 = nobody: `.nopp`, the puck goes into empty ice along `passdir`).
`passto` does all the physics (pass speed from the rating, lead on the receiver's
velocity, animation, `passReceiverPlayerNum`); we never touch that.

**What we do.** The existing `$00BD5E` site (`NHL94_ROM_DOPASS_PICKED`, no new
`game.toml` entry) now also calls `aim_pass_hook()` after `call_hook` (the AI
carrier asked by a seat: unchanged). For a passer that is `pfjoycon` with the lock active:

- **Aim** = `passdir` (`$FFBEDC`, `NHL94_PASSDIR`): `setpassmode` sets it to
  `facedir & 7`, `passmode` sets `d0 & 7` while a d-pad direction is held with B
  down. It is read after `remap_input` rotated `d0` for a `pfgoal`-clear skater, so
  it is already in rink coordinates (0 = +y, 1 = +x+y, 2 = +x, ...); views never matter.
- **Candidates**: teammates of the passer, not the passer, `position > 0` (goalies
  and parked 4v4 objects are excluded, as in the game), not `pf2unav`.
- **Score** (`nhl94_aim_pick`, lowest wins): `err + min(dist,320)/4 - 150 if human
  - 300 if that human is calling`. `err = 1024 sin^2(angle)` (true geometry, not
  octants): 10 deg = 30, 20 = 120, 30 = 256, 45 = 512. **Cone**: err <= 768 (60
  deg); 90 deg or more is out. A human within ~22 deg beats an AI dead on the stick;
  a human whose seat holds B / X (calling) within ~41 deg does. Ties: lowest object number.
- **Calling** = the seat's B / X in `NHL94_SEATREQ_PREV` (the sealed pad, written
  in-sim by `pass_calls` in `updateplayers`, before any `doinput` of the frame), the
  same word the call icon reads.
- **Fallback**: nobody in the cone -> `a0` / `d4` stay the game's (its pick or
  `.nopp`): never worse than the cartridge. Otherwise `a0` = the object (as
  `movea.w`), `d4` = 0.
- AI passers (`chk4pass`, `chk4shot`, line-change auto-pass), penalty shots /
  shootouts (`lock_active()` = 0), the one-timer path (taken before the loop) and a
  stick left alone (`passdir = facedir`) are as before.

**B for a non-carrier.** `remap_input` masks B (bit 4 of `d0..d3`, as for a pending
call) for a pad player whose teammate (human **or** AI) carries the puck: B is a
call for the puck, never a sweep check / player switch (`chgplayer` ->
`Sweepcheck` locks the skater, `pfalock`). Call-for-pass presses are unaffected
(`pass_calls` reads the sealed pad). A loose puck, an own puck and an opponent's
puck leave B as before.

**RAM.** None new: reads `passdir`, the puck, objects, `SEATREQ_PREV`. No host
state feeds the sim; logging only when `!genesis_multiview_speculating()`.

**Log.** `nhl94_seats: t=T aim pass seat K SC p dir D game SC g ours SC o score S
human H call C [CHANGED]` (or `ours none (cone empty)`), summary line
`aim_passes= aim_changed= aim_human= aim_none=`.

**Test-only** `NHL94_PASSASSIST=off`: no aimed passing and no B mask (the old sim).
It changes the simulation, so it is **ignored while netplay is active** (log line
`... is a TEST-ONLY sim knob and is IGNORED while netplay is active`): the host environment is not
part of the sealed session, and one peer with it left in its shell would desync the room. The same
holds for every test knob that changes the sim: `NHL94_SEATS_NO_AUTOLC`,
`NHL94_SEATS_KEEP_CAMERA_RULE`, `NHL94_SEATS_TEST_PS`, `NHL94_FORMAT_TEST_PENALTY`
(`nhl94_sim_knob_allowed()`, `genesis_netplay_active()`, evaluated at use time; offline tests and
`rb_probe` are unaffected). Config inputs (`NHL94_SEATS`, `NHL94_FORMAT`, `NHL94_BUILDS`,
`NHL94_MEMBER_BUILD`) only feed the host's session image, which is sealed, and the guests adopt it.

### Session marker (a build without aimed passing refuses the match)

The engine does not refuse a different build on its own, and aimed passing changes
the simulation. A seats value that carries seats therefore starts with `p<N>:`
(`SEATS_PASS_VER` = 2 in `nhl94_seats.c`), in front of every other form:

```
value := [ "p" N ":" ] body [ "+B" ... ]    e.g.  nhl94.seats=p2:home:C,away:G
body  := "4v4" | "4v4," list | "*4" letters | "*" letters | list
```

- `config_image` always writes `p2:` when there are seats (readable list, 4v4 list
  or the compact one-letter form: `p2:*ekdjflbhciag` = 16 bytes for 12 seats).
  `"4v4"` alone (Classic control in 4v4, no lock, no passing) has no marker.
- `parse_seats` (adopt and waiting-room labels) strips the marker first. N greater
  than this build's: refused. Seats without a marker (an older build's host): refused.
  A marker on a seatless value is harmless.
- An older build refuses a marked value by itself: `p2:4v4,...` and `p2:home:C,...`
  fail `parse_seats_list` (the first item's role is not a role / its team is neither
  home nor away), and `p2:*eg` is read as a list whose item `p2:*eg` fails the same way.
- The `+B...` builds suffix (another feature) is not parsed here: whoever owns it must
  strip it before `parse_seats` sees the value; the marker is independent of it.
- `GenesisSessionConfig.game` is 48 bytes: the marker costs 3.
- Bump `SEATS_PASS_VER` for any later change of the pass sim (aim constants in
  `nhl94_aim.h`, the B mask) **or of any other sim behaviour for the same seats value**.
  Version 2 = version 1 + the drop-in / penalty-shot work that followed it (join at the whistle,
  pause-orphan release, the penalty-shot attempt latch, joiner role-body reservation): a `p1`
  host is refused.

**Verification** (build `~/nhl94-online-build/eashl/build-pass`,
`tools/seats/run_checks_pass.sh <build> <out>`):

- Unit test `test_aimpass.c`: 118 checks, 0 failures (`cc -I game -o /tmp/t
  tools/seats/test_aimpass.c game/nhl94_aim.c -lm`). `test_format_image.c` extended for
  the marker (all OK).
- `aimpass1.input` (seats `home:C,home:RD`; the C wins the faceoff, `WAIT_RAM16
  FFB7AA 0002`): three passes. t=1818 C aims down-right: game would pick SC 0, ours the
  human RD (SC 3) `CHANGED`; t=2764 RD aims up-left while the C calls: the C (call 1); t=2866
  C aims up: SC 4 (the AI, same as the game's). Hash **A84A0C5E44F9364D** (twice, and
  with the icon off). `NHL94_PASSASSIST=off`: F1551B3F2E7D1544.
- New baselines with the feature on: callpass1 (`home:C`) **47F6F5603E67C676**
  (the B mask); callpass2, lock2 3EF834BA226E6E28 and lock4 A94E00C87F121117 are
  unchanged. With `NHL94_PASSASSIST=off`: callpass1 FAA72A99530568E6, callpass2
  560C3425A02A9A91 as documented. Demo (no seats) 80CE87C28143ADBF.
- `rb_probe_seats.sh ... 6300 30:8 aimpass1.input -- --seats home:C,home:RD`: passes=209
  divergences=0 asymmetric=0 PASS.
- LAN lobby selftest (`lobby_format_test.sh`, 6v6/6v6): the guest adopts the marked
  image, 43 TIMELINE ticks identical.

**Not done / unverified.** Aimed passing over real netplay with latency
(`net_aim_slot*.input`) was not added; the rollback probe and the LAN lobby run are
the netplay evidence. Whether a goalie teammate could receive (excluded, as in the
game). `aimpass1` depends on the C winning the faceoff (verified, deterministic).

## Call icon

A pulsing ring at the feet and a `!` bubble beside the name tag of a teammate who
is calling for the puck. Presentation only (`game/nhl94_tags.c`).

- **Source**: `nhl94_seats_call_flags(uint8_t[12])` (guest RAM only), per object:
  bit 0 HELD = the seat's B / X (`SEATREQ_PREV`) is down while a teammate carries
  (the condition `aim_pass_hook` uses); bit 1 QUEUED = an AI carrier was asked for
  him (`SEATREQ_TARGET == obj + 1`, `SEATREQ_SEAT == seat`). Needs `pfjoycon` on the object.
- **Who sees it**: the local seat's team (own icon included); every team for a
  spectator or `NHL94_CALLICON=all`.
- **Drawing**: part of the tag record (`TagEnt.team`, `.flags`), so it uses the same
  capture and camera-relative position as the tags: right in the rotated period-2
  view (the clone's mirror turns the picture, the overlay is upright) and in the zoomed
  picture (drawn in picture pixels, shrinks with the skaters). Ring: 2 px elliptical band
  (11:5) in the seat colour with a black outline, HELD pulses 11 / 13 px every 8 ticks
  (`genesis_sim_tick_count() >> 3`, read-only); QUEUED is steady and filled at half
  alpha. Bubble: 11 px wide, after the name box (before it when there is no room /
  the off-screen arrow is on that side), reserved in `placed[]`. An off-screen human gets the
  bubble at his edge tag and no ring. `!` is a local glyph (the 5x7 font has none).
- **Settings**: launcher Mods > Control mode > **Call icon** (On/Off, default On, right
  after Name tags), `callicon = on|off` in `nhl94-control.ini`, `NHL94_CALLICON=0|1|all`
  overrides. It needs name tags on. Adding the option shifted the launcher option indices:
  `OPT_SEAT0` is now 6 (zoom 3, format 4, preset 5).
- **Hash invariance** (`icons2.input`, 6000 frames, screenshots taken): icon on, off
  (`NHL94_CALLICON=0`), `all`, and tags off all give **FD341A48C4682E1D**.
- **Images** (`run_checks_pass.sh` writes `icons_on_*.png`; zoomed seat-view frames
  with `GENESIS_VIEW_SCREENSHOT_EVERY=4 GENESIS_VIEW_SCREENSHOT_DIR=...`): HELD in
  period 1, QUEUED (RD taps B after the C passed up to an AI mate), and HELD in the flipped
  period 2, all correct in the 1.5x seat view.
- **Limits**: on a rollback misprediction the icon flickers until the corrected tick (as
  the tags); off-screen players get no ring.

## Camera (presentation only)

(Position lock: the seat's own view follows `nhl94_seats_control_object(seat)` first -- a substitute
body, the penalty-shot shooter / goalie -- and the role scan only when that is -1; explicit
`NHL94_VIEWS` role lists keep the role scan.)

With seats and none of `NHL94_VIEWS` / `NHL94_VIEW` / `--views @list`, the
process shows one view: its own seat's player (the netplay local player
online, `NHL94_LOCAL_SEAT` offline, default 0; spectators follow the puck).
`NHL94_VIEW=off` disables it. Keyboard **G** or a controller's **left-stick
click (L3, without Back)** toggles every view between its player and the puck
(orientation stays the team's). Both are free in the runner and the recomp-ui
defaults (F10 / Back+L3 open the runtime menu, shoulders are quicksave/load
and Y/Z). The toggle is polled in `view_prepare`, a host static, never an
input and never on the wire. `NHL94_VIEW_FOLLOW=puck` starts in puck mode.

**Framing.** The camera keeps your player in the lower part of the view (20 %
of its height below the centre; goalies 33 %). It leans toward the puck
(45 %; goalies 30 %, only up to their own blue line), leads your skating
direction, and never lets you leave the band 18-90 % from the top. A small
dead zone keeps it calm. `NHL94_VIEW_FRAMING=0` gives the old centred camera;
`NHL94_VIEW_ANCHOR`, `NHL94_VIEW_PUCKBIAS`, `NHL94_VIEW_DEADZONE` and
`NHL94_VIEW_SMOOTH` tune it.

**Zoom (how much ice you see).** Pick it in the launcher: **Mods > Control
mode > View zoom**: 1x (the original 256x224), 1.25x, **1.5x (default,
recommended)** or 2x. In game, keyboard **F** or a controller's
**right-stick click (R3, without Back)** cycles 1x, 1.25x, 1.5x, 2x.

| Zoom | Picture | What you see |
|---|---|---|
| 1x | 256x224 | the cartridge view |
| 1.25x | 320x280 | a bit more each way |
| 1.5x | 384x336 | the whole rink width (no sideways scrolling) and half again as much ice ahead and behind |
| 2x | 384x448 | the whole rink width and twice the ice ahead and behind; players are smaller on screen |

The clock and score boxes stay in the picture's bottom corners. Off-screen
arrows are not drawn while zoomed, because your own player is always in
view. `NHL94_VIEW_ZOOM=1|1.25|1.5|2` overrides the setting (test view lists
and spectators default to 1x unless it is set or changed in game).

Zoom is display only. Each player picks their own; it is not in the session
image, the lobby or the input stream. The bigger picture is rendered only in
the view's discarded clone, as described in `docs/WIDER_VIEW_PLAN.md`.

## Verification (build `~/nhl94-online-build/seats`)

`tools/seats/run_checks.sh <build> <out> [old build]` reruns all of this.
Scripts are in `tools/seats/`; `boot_h2h*.inc` is the menu path (Head to
Head; the `_30s` variant writes `Optperlen=3` for 30-second periods, offline
only). `NHL94_SEATS_TRACE=<n>` prints each seat's object, position, velocity,
facing and the `pfjoycon` mask every n ticks plus an exit summary.

**2 seats (home:C, away:G), `lock2.input`.** `joy=804` (objects 2 and 11
only) on every traced frame; summary `stray_joycon=0 pad_moved=0`,
16 refused game switches, 2 faceoff resets restored.

| period | seat input | result |
|---|---|---|
| 1 (home up) | home C UP | vy +8786, y −15 → 283, facing 0 |
| 1 | home C RIGHT | facing 0 → 2, vx up to +1426 (against the top boards) |
| 1 | away G LEFT (remapped) | vx +3817, x 0 → +39 |
| 1 | away G RIGHT (remapped) | vx −7530, x → −38 |
| 2 (home down) | home C UP (remapped) | vy −8670, y 15 → −176, facing 4 |
| 2 | home C RIGHT (remapped) | vx −8421, facing 6 |
| 2 | away G LEFT | vx −6255, x 0 → −39 |
| 2 | away G RIGHT | vx +7580, x → +38 |

**4 seats (home:C, home:RD, away:C, away:G), `lock4.input`**, all four at
once: `joy=90C` (objects 2, 3, 8, 11) throughout, `stray_joycon=0
pad_moved=0`.

| period | s0 home C UP | s1 home RD RIGHT | s2 away C UP | s3 away G LEFT |
|---|---|---|---|---|
| 1 | vy +8648 | vx +7818 | vy −8401 | vx +5197 |
| 2 | vy −8510 | vx −7767 | vy +8480 | vx −7470 |

Composite of the four seats' own views (2x2, labelled):
`~/nhl94-online-build/seats-work/v4comp/grid_t00{1800,1880,1920,3800,3880,3920}.png`.

**Goalie seat, camera rule.** In `lock2.input` the camera-distance rule
would have handed the away goalie to the AI on 1388 of 2233 frames (counter
`goalie_cam_rule_cancelled`); he stays `pfjoycon` throughout and follows the
pad (x 0 → +39 → −38, |vx| up to 7530). Negative control
`NHL94_SEATS_KEEP_CAMERA_RULE=1` (test only, changes the simulation): same
inputs, the goalie barely moves (x 0 → +15 → −11, |vx| ≤ 3242): the AI drives
him.

**Determinism.** Same script twice: `lock2` 3EF834BA226E6E28 (also with the
own view off), `lock4` A94E00C87F121117 (also with 4 views on). No seats,
Demo game: 80CE87C28143ADBF with this build, with 6 views, and with the build
before this change (`views`). Rollback probe (`tools/seats/rb_probe_seats.sh`,
the engine probe with the ROM passed in place instead of copied):
gameplay + `--seats home:C,away:G` passes=99 divergences=0 asymmetric=0 PASS;
`lock4.input` + 4 seats passes=139 divergences=0 asymmetric=0 PASS.

**Netplay** (2 processes, LAN, rollback, `NHL94_SEATS=home:C,away:C`,
`net_slot0.input` / `net_slot1.input`): 66/66 confirmed TIMELINE digests
identical, final state 2C4B8FF1E23D0B26 on both, desyncs=0. Home C UP vy
+8783; away C (other process) UP vy −8401 (remapped), LEFT vx +8488. With
simulated 60±20 ms latency: 4 rollback episodes, 65 shared confirmed
TIMELINE digests identical, desyncs=0.

**Control mode setting** (build `~/nhl94-online-build/mode`):
the launcher's Mods page shows the feature (scripted with recomp-ui's
`LNG_SCRIPT`: `view:mods;mod-select:2;click:...;shot:...`); choosing Position
lock wrote `mode = lock` to `nhl94-control.ini` and logged `control mode
Position lock home:C,away:C (source: launcher setting)`; a duplicate position
was refused on screen. Precedence checked headless: no file -> Classic; file
lock -> launcher; `--seats` over the file; `NHL94_SEATS=off` over `--seats`.
Real lobby (`tools/seats/lobby_mode_test.sh`, engine lobby selftest): host
Classic + guest Position lock -> the guest logs `Classic (source: host's
session config)`, 43/43 TIMELINE digests identical, desyncs=0; host Position
lock + guest Classic -> the guest adopts `home:C,away:C`, both peers show
objects 2 and 8 human (`joy=104`), 43/43 identical, desyncs=0.

## Name tags

Presentation only: every on-ice player a human controls gets a small
triangle in the seat's colour and the human's name above the skater, in
every picture (the main window and each view). AI players get nothing.
Code: `game/nhl94_tags.c` (capture + drawing), `game/nhl94_tagnames.c`
(seat -> name, pure), `nhl94_seats_human_objects` in `nhl94_seats.c` (who is
human), the engine hook `GameSpec.present_overlay` (below).

**Who is human.** Position lock: seat k's object as `seat_target` resolves it
this frame, if it carries `pfjoycon` (a seat with no device / empty slot
has none, so no tag). Classic (and the lock suspended for a penalty shot /
highlights): the objects the pads control (`c1playernum..c4playernum` with
`cont1team..cont4team` set, and `pfjoycon`); pad i = seat i. Nothing is
drawn during menus, intermission highlights of other games, the scoreboard
rink, pause and replay (`gmhl`, `sfhor`, `sfpz`, `sf2replay`), nor for
objects off the ice (`position` < 0).

**Names.** Online: the lobby client's live member table
(`rnet_lobby_member_get`): the non-spectator member whose `slot` equals the
seat. The launch's `slot_port` is the identity or maps session slot -> lobby
seat -> logical player with the same number (`genesis_host_lobby.c
config_from_launch`: `slot_port[i] = lobby_seat[i] = p`), so lobby slot ==
seat. A seat the table does not know (or a blank name) shows `P<seat+1>`;
the local seat falls back to `rnet_lobby_display_name()`. Offline and LAN:
`P1`..`P12`. Display form: upper case, A-Z 0-9 space `-:._` (anything else
`?`, one per UTF-8 character), at most 10 characters.
`tools/seats/test_tagnames.c` checks the mapping with a fake member table.
`NHL94_NAMETAGS_FAKE="Alice,Bob,..."` (test) feeds the same fake table to a
running game (entry i names seat i).

**Look.** Fixed 12-colour seat palette (seat 1 red, 2 blue, 3 yellow, 4
green, 5 orange, 6 purple, 7 cyan, 8 pink, 9 lime, 10 teal, 11 tan, 12
white). The LOCAL seat's tag (`nhl94_seats_local_seat`) is drawn filled in
its colour next to the game's own star; the others are a dark glass box with a
colour border and white text. Tags that collide (two humans at a faceoff) are
stacked, with a 1-pixel leader line to the skater; no room above = below
the feet. **Off-screen humans are clamped to the nearest picture edge** with
a bigger triangle pointing toward them (the camera follows the puck or one
player, so a teammate out of sight is exactly when "who is where" helps; the
game's own off-screen arrow for the pad players stays too). Hiding them would
lose that.

**Zoomed views.** In a view picture taller than 224 lines (the zoomed
position-lock view) tags are drawn in picture pixels, unscaled, like the
sprites: they shrink with the skaters when the picture is scaled down to the
window, and the 26 px head offset stays right above the sprite. (Scaling them
by h/224 to keep their 1x window size made them dominate the zoomed-out
picture.) The projection is unscaled too.

**Projection** (the game's own `find3d`, VIEW_RESEARCH.md section 4):
screen x = X - Hpos + w/2, screen y = h/2 - (Y - Vpos + 1.5 Z), tag above
the feet by 26 px. The record stores X-Hpos and Y-Vpos+1.5Z, so the flipped
views (X, Y, Hpos, Vpos all negated in the clone) need no special case.
Measured: the projection origin sits at the skater's feet (cross-check with
the game's own star: offset (1.0, -2.5) px constant while stationary and
while moving 2 px/frame, so there is no one-frame lag).

**Timing.** The sprite list `setvideo` builds in tick T is shown by tick T+1
(VIEW_RESEARCH.md section 4). Authoritative picture: the capture at the
`$007998` hook (read-only, host statics) is held one tick, so tick T+1's
picture gets tick T's record; a tick without the hook (menus, a lag frame)
reuses it for at most 2 ticks. A view records at its own hook, right after
`apply_view`, and is drawn after the engine grabbed its image.

**Determinism.** No guest RAM, VDP or snapshot state is written; the capture
only reads RAM into host statics and the drawing touches the already
presented pixels (the main frame is drawn in place after the tick, before
screenshots; the renderer rewrites every pixel of the next frame). The hook
runs on rollback replays too, harmlessly (the last two hooks before a
presented frame are the final ones).

**Setting.** Launcher Mods -> Gameplay -> Control mode -> **Name tags**
(On/Off, default On, right under Mode), saved as `nametags = on|off` in
`nhl94-control.ini`; `NHL94_NAMETAGS=0/1` overrides it. `NHL94_NAMETAGS_DEBUG=1`
logs the records and draws the projection origin as a magenta cross.

**Engine hook.** `GameSpec.present_overlay(uint32_t *pixels, int w, int h,
int stride, int view_index)` (last field of `GameSpec`, NULL = none): called
by `runner/main.c` after the authoritative tick (view_index -1, on
`s_framebuf` before the screenshot / post-tick dumps) and by
`runner/multiview.c` after each view image is grabbed (view_index k). The
runner knows nothing about hockey.

**Verification** (build `~/nhl94-online-build/tags`, work
dir `~/nhl94-online-build/tags-work`; scripts
`tools/seats/tags2.input`, `tags4.input`, `tagsclassic.input`; helper
`pngzoom.py`, `montage.py`, `tagprobe.py`):

- State hash (`--benchmark N`, same script) with tags on / off / again / with
  4 views / own view off: `tags2` 6CFD2D9C3295CFAF (7 runs, all equal),
  `tags4` B3A1764CD7E863BA (4 runs), `tagsclassic` 430229F98A8267FF (5 runs).
- `rb_probe_seats.sh` (tags default on): `lock4.input` + 4 seats passes=139
  divergences=0 asymmetric=0 PASS; gameplay + `--seats home:C,away:G` passes=99
  PASS; gameplay Classic passes=99 PASS.
- Images (`tags-work/final/`): `final_a_lock2_main.png` (2 seats, main
  window: period 1 faceoff, period 1 away from the faceoff, period 2 flipped
  faceoff, period 2), `final_b_lock4_main.png` (4 seats P1-P4),
  `final_c_classic.png` (Classic Head to Head: the tags follow the pad's
  skater as control switches), `final_d_views2_p1_flip.png` /
  `final_d_views2_p2.png` (views home:C, away:C and the main frame, the away
  view is turned 180 degrees in period 1), `final_e_views4_p2.png` (4
  views). Names: `tags-work/m_names.png` (fake table `alice`, `Bob the
  Builder`, `Zoe..`), `tags-work/test_tagnames.txt` (unit test output).
  Launcher page: `tags-work/mods_b.png`, `mods_d_crop.png` (Off, written to the
  ini).

**Limits.** Names online were checked against a fake member table, not a
live lobby (the mapping assumes lobby slot == seat). The collision handling
is simple (stacking / sliding along the edge): twelve humans bunched in one
spot will still crowd. The 26 px head offset is the skater's; a goalie
crouched in his crease is a few pixels shorter. NHL94 has no widescreen
mode or frame interpolation here (`video->interpolate` is NULL), so the
overlay never meets a differently sized or re-composited frame; a game
that adds one has to draw the overlay on that image too. Tags are in the
frame the engine writes screenshots from, so screenshots include them.

## 4v4 format

**Format = 4v4** (launcher, `format = 6v6|4v4` in `nhl94-control.ini`;
`--format 4v4`; `NHL94_FORMAT=4v4`; precedence env > CLI > launcher, the
host's online) puts four bodies per team on the ice: **G, LD, C, LW**. Code:
`game/nhl94_format.c`; design, hook list and measurements:
`docs/FOUR_ON_FOUR_PLAN.md`. It is independent of the control mode (4v4 with
Classic control is valid).

- **Seats.** Only G / LD / LW / C exist in 4v4. The launcher offers only
  those (switching to 4v4 drops RD/RW seats and closes the gap); an
  `NHL94_SEATS` / `--seats` list with RD or RW under 4v4 is refused
  (Classic, logged as malformed); a host image combining 4v4 with RD/RW is
  refused by the guest.
- **Penalties** follow the NHL 3-on-3 overtime rule: a penalty adds a body to
  the OTHER team (5v4, 6v4 bodies). The extra skaters are RD then RW, always
  AI. A penalty expiring mid-play gives the penalized team its player back
  at once (5v5 / 6v6 bodies until the next whistle); every faceoff re-applies
  the rule.
- **Applied at game start.** The format is latched into guest RAM (`FMT`,
  `$FFDFC0`) when the game resets its teams; changing it in the launcher
  mid-game takes effect at the next game.
- **Views.** `GENESIS_VIEWS=6`'s default list still names home:RD/away:RD,
  which follow the puck in 4v4 (`nhl94_view.c`; a per-seat own view is
  fine). Parked objects (x = -240) are outside every view, the 2x zoom
  included.

## Line changes, substitutes, pulled goalie, penalty shots

Goal (EASHL): a human always has a player on the ice. Design: `eashl/r1.out`;
code: `game/nhl94_seats.c` (`force_auto_linechange`, `seat_primary`,
`role_targets`, `ps_targets`, `latch_psrole`); presentation:
`nhl94_seats_control_object`, `nhl94_view.c find_target`.

### RAM (guest, only written with seats configured)

| address | meaning |
|---|---|
| `$FFDFB4-BF` `NHL94_SEATTGT(k)` | u8[12] SC+1 of the object seat *k* controls this frame (0 none). Written by `enforce` for every seat; read by tags (`nhl94_seats_human_objects`) and the camera (`nhl94_seats_control_object`). |
| `$FFDFE4-EF` `NHL94_SEATSUB(k)` | u8[12] sticky substitute SC+1 (0 none). |
| `$FFDFF0-FB` `NHL94_SEATVAC(k)` | u8[12] frames the role has had no body (saturating; debounce). |
| `$FFDFFC` `NHL94_PSROLE` | u16 role+1 of the fouled skater, latched while a penalty shot is pending (`BA_PS_flags` bit 3). |
| `$FFDFFE` | u16 TEST ONLY: done bits of `NHL94_SEATS_TEST_PS`. |

(`$FFDFB4-BF` and `$FFDFE4-FF` were the only free gaps of the stack-bottom block;
`$FFE000+` belongs to other features.)

### Line changes: Option B, no mid-play change ever touches a human

Position lock forces the cartridge's own **automatic line-change mode**
(`sflags7` byte `$FFC2FC` bit 4, normally set by `GameSetUp` `$0F8474` when the
menu's line-change option is *Auto*) every frame in `enforce`, **only when
`OptLine == 0`** (menu "On"). Effects, all from ROM code that already exists:
`SetLCmode` ($00B8F2) never opens the faceoff box (any pad); `SetFaceoffComputerLine`
($00F590) changes **every** team's line at the faceoff whatever `cont1team`/`cont2team`
say, so teams of seats 3-12 get computer changes (before, only pads 1/2 decided
"human"); `chk4lc` ($00E1F4) makes no mid-play computer change. The seat follows
its role at the faceoff, where `forcepldata` swaps players instantly. Side effect
of the ROM mode: `makepde` returns full energy (**no fatigue**). The menu default
in these scripts is `OptLine = 1` (line changes **Off**): nothing is changed or
forced then (no changes happen at all); the test scripts write `OptLine = 0`
(`--optline 0`). `NHL94_SEATS_NO_AUTOLC=1` skips the forcing (TEST ONLY negative control).

Observation, `setpersonel` callers (`$015788`, host log + summary): in every run of
`lc_auto`, `pen_sub`, `pull_g`, `ps_4v4`, `four_on_four`... the only **mid-play**
callers were the goalie pulls (`ChkGoalies`'s two `bsr`s: return addresses `$0079D4`
and `$00F6C2`, delayed penalty `st tmgoalie; bra setpersonel`); faceoff callers were
`$00F9E0`/`$00F9EC` (puckfaceoff) and `$00F5BA` (SetFaceoffComputerLine .dochange).
`pending_released` (a skater seat whose role body was pending a mid-play change)
stayed **0**. Decision: **the `setpersonel` pin is not shipped.**

### Substitutes (penalties, short-handed dropped roles, pulled goalie)

A human seat whose primary body is missing for `VAC_MIN` = 6 frames takes a
**sticky substitute** (`SEATSUB`): a skater body of its own team that is on the
ice, not `pf2unav`, no pending change, not another seat's body and not another
human's role body. Candidates by `s_pref[role]` (nearest line-mate first; the G seat
prefers position 6, the extra attacker, then C, LW, RW, RD, LD); seats choose in
index order, objects in index order. A **new** pick is only made when play can resume
(`gmclock` clear, or the faceoff line-up); the existing pick is kept while valid, so the
seat keeps skating through a whistle. The G seat switches to the extra attacker (position
6) as soon as that body exists (not `pf2unav`). When the primary body returns the
seat switches back at once (`releasepl` bodies are `pf2unav` until out of the door) and the
substitute is released as `restorepl` would (AI resumes). A seat with no candidate (all other
skaters belong to other seats) idles (`vacant_nosub`). A pulled goalie (delayed penalty): his
object carries `newpos=6`, so the G seat is released from him at once (a pending change on a
goalie is the one that must leave mid-play), takes a skater, then the extra attacker; at the
next faceoff `ReturnGoalies` brings the goalie back and the seat returns to him.
Idle gaps remain only where no body exists: the ~0.5 s between a faceoff re-slot and the new
bodies, and the post-penalty-shot clean-up.

### Penalty shots and shootouts (`CM_PS`)

`ctl_mode()` = PS while `BA_PS_flags` bit 2 is set. `ps_targets` gives the shooter body
(`BA_Sktr_SCnum`) to **one** seat of the shooting team and the defending goalie
(`BA_Goalie_SCnum`) to the **G seat** of the defending team, else (fallback) to the first
skater seat of that team (a CPU goalie against a lone human is not fun; the 1v1 preset used
to hand the human the goalie in the game's own hand-over). Every other seat has nobody (all
other bodies are parked). Shooter seat: penalty shot = the seat whose role equals the fouled
skater's (`NHL94_PSROLE`, latched during the whistle, because `SetupPenaltyShot` rewrites his
position to C), else the C seat, else the first skater seat; shootout (`gmode2` bit 0) =
the shooting team's skater seats in turn by `shootoutround` (`$D578`, 1-based, advances once per both teams' attempts: read from
`shootout94.asm`, not observed running); a team with only a G seat: he shoots.
**Attempt latch** (`$FFE304` shooter seat+1, `$FFE305` goalie seat+1, `$FFE306` u16 key = shooting
team, shooter object, shootout round; cleared whenever the mode is not PS): `ps_targets` chooses the
seats once per attempt and keeps them. A latched seat that leaves is nobody's (the AI finishes the
attempt); nobody is re-dealt the shot. `drop_join_gate` admits no joiner while `BA_PS_flags` has the
pending (`$0800`) or live (`$0400`) bit or in a shootout (`gmode2` bit 0): a stopped clock is not a
whistle there; the joiner takes its player at the first whistle / faceoff after the attempt.
`shootoutround` (`$D578`) is 1-based and advances once per BOTH teams' attempts
(`shootout94.asm` ClearShootout / CountShootoutGoals), so `cand[(round-1) % n]` is each team's own
candidate list, not a repeat across teams.
Pad identity is unchanged (seat *k* keeps pad *k*; the game's `doinput` runs for whichever
object carries `pfjoycon`). No call-for-pass in PS. Before this the lock was suspended and
seats on virtual pads lost control.

### Tests and baselines (build `~/nhl94-online-build/eashl/build-lock`)

`tools/seats/run_checks_lock.sh <build> <out> [pre-change build]` runs all of this. Scripts
are generated by `gen_input.py` (seeded; the menu default is `OptLine = 1`, so the scripts
write `OptLine = 0`; period sync for 30-second periods) and committed: `lc_auto.input`
(3 x 30 s periods, 4 seats), `pen_sub.input` (5-minute periods, penalties on, `C` presses),
`ps_4v4.input` (8 seats), `ps_12.input` (12 seats), `obs_long.input` (8 seats, 17k frames).
`pull_g` is `pen_sub.input` with G seats. Test-only env: `NHL94_FORMAT_TEST_PENALTY` (existing),
`NHL94_SEATS_TEST_PS=tick[:team[:role]],...` (stages the state `getBAplayerInfo` leaves for a
penalty shot -- a real one needs a breakaway foul), `NHL94_SEATS_NO_AUTOLC=1`.
Trace (`NHL94_SEATS_TRACE=n`) gains `mode=` (1 play, 2 PS), `optline=`, `sf7=`, `line=` (both
teams), `tmg=`, `ap=` and `(sub)` after a seat's role; summary: `lcm_seen`, `midplay_handover`,
`pending_released`, `sub_frames`, `sub_takes`, `vacant_nosub`, `ps_frames`, and the `setpersonel`
caller table.

| check | result |
|---|---|
| `lc_auto` 9000 frames, seats `home:C,home:LW,away:C,away:LW` | `sf7` bit 4 set from the first frame (`sf7=10/11`), `lcm_seen=0`, `midplay_handover=0`, `pending_released=0`, home `tmline` 0 -> 2 at a faceoff, seat vacancy 3 frames each (the faceoff re-slot). Hash `2538466F51D14C61` twice. |
| negative control `NHL94_SEATS_NO_AUTOLC=1` | `lcm_seen=660`: the pad 1/2 line-change box opens (hash `F2DB69454933E863`). |
| `pen_sub` 11000 frames, `home:C,home:LW,home:RW,away:C`, `NHL94_FORMAT_TEST_PENALTY=2400:home:LD` | After the faceoff the RW seat takes SC 2 (RD, the first free after C and LW are reserved); the substitute follows him across whistles and faceoffs; at t=7009 (penalty over) he is back on the RW body (SC 4). `vacant_nosub=2` (frames). Hash `470FC06199363F0E` twice. With the penalty on the seat's own role (`:home:LW`): same, the seat controls the roster substitute as primary. |
| `pull_g` 9000 frames, `home:G,home:C,away:G,away:C`, 8 injected penalties | Away pull at t=2408 -> G seat takes SC 7 (LW) 5 frames later, back on the goalie at the next faceoff (t=2810); home pull at t=6740 -> seat takes SC 3, back on the goalie (SC 5) at t=7630. Hash `BBEA01E472EFEB39` twice. The extra attacker (position 6) only exists when the pulled goalie has reached the bench; in these runs the G seat had taken a skater before and the next faceoff came first (`xbetter` path **not** observed taking position 6). |
| `ps_4v4` 8000 frames, Full 4v4 seats, `NHL94_SEATS_TEST_PS=2500:0:4,4000:1:3` | `mode=2` for 715 frames: exactly two seats have a target: the shooter (home C seat 0, then away LW seat 3 = PSROLE LW) and the defending G seat (away G s7, then home G s6); the shooter's pad moves him; after the attempt `mode=1` and the seats are back on their roles. Hash `76EF0AFADBBD77AC` twice (views on/off). |
| `ps_12` 12 seats (virtual pads) | shooter away:RW (seat 5, virtual pad) and goalie home:G (seat 10, virtual pad) both move with their pads in PS; then home:LW / away:G; home:C / away:G. Hash `BEF649113B3C3E5C` twice. |
| `rb_probe_seats.sh ... 30:8` | `lc_auto` passes=299, `pen_sub` 299, `pull_g` 299, `ps_4v4` 266, `ps_12` 233, `four_on_four` (`--format 4v4`, Full 4v4) 666: all `divergences=0 asymmetric=0 PASS`. (Use an ABSOLUTE `RB_PROBE_OUT`: the script `cd`s into it.) |
| Demo (no seats) | `80CE87C28143ADBF`, also with 6 views and against the pre-change build: **unchanged**. |
| neutrality: pre-change build vs this build with `NHL94_SEATS_NO_AUTOLC=1`, `NHL94_SEATS_TRACE=10` | `lock2` (223 lines), `lock4` (246), `callpass1` (527): trace identical (hashes differ: `SEATTGT` is new RAM). |
| `setpersonel` observation (`lc_auto`, `pen_sub`, `pull_g`, `ps_4v4`, `ps_12`, `obs_long` 18000 frames 6v6 and 4v4 with 8 injected penalties) | callers: faceoff `$00F9E0`, `$00F9EC` (puckfaceoff), `$00F5BA` (SetFaceoffComputerLine); mid-play only `$0079D4` / `$00F6C2` = `ChkGoalies` goalie pulls. **No other mid-play caller: the pin is not shipped.** |
| 2-process rollback netplay (`net_slot*.input`, `net_call_slot0.input`) | 66/66 TIMELINE digests identical, desyncs=0 (`run_checks.sh`). |

New hash baselines (build before this work in parentheses; they change by design, `SEATTGT` is
guest RAM written every frame and `sflags7` is forced): `lock2` `84C62F7EC823E795` (3EF834BA226E6E28),
`lock4` `51B3D2BD181C4A47` (A94E00C87F121117), `callpass1` `FE8120125849A42E` (FAA72A99530568E6),
`lock6` `1E6B9F7F8FDA89B0`, `lock6_swapped` `CCE85F55D1B833B8`, `lock12` `58488A0A51D3B28F`,
`four_on_four` 8 seats 4v4 20000 frames `B328B6DB8C723E69` (45B46C2EC1015982), netplay `lock`
`E86D38742516C780`, `call` `23DFF88F03926DC8`. `tools/upstream-sync/baseline/*.hashes` were **not**
re-recorded (re-record after the other features are merged).

**UNVERIFIED / not done:** the shootout rotation by `shootoutround` (no shootout script: `so_rotate`
not feasible, see Known limits); `$FFDFE4-FF` zero-init on real hardware (the engine zero-inits, as for
the other blocks); the `xbetter` switch to position 6 in a real pull; the camera substitute follow was
checked only through `find_target`'s code path and the view log (no screenshots).

## Known limits

- **Line changes.** Position lock forces automatic line changes when the menu
  has line changes On (above): no box, no mid-play change, no fatigue (`makepde`); the lines
  change at faceoffs, for every team. OptLine = Off/1: no changes at all. A forced pending
  change on a locked skater (`forced_linechange.input`, written by the test, not the game)
  still releases the seat for the bench trip (nothing in the game does that any more).
- **Penalties.** The penalized human's own body is boxed during the whistle; at the faceoff the
  role gets a roster substitute body (SetPlList `.unavail`) that the seat takes as primary; a role
  dropped by the short-handed rule gets a substitute (above). Which human was penalized is not
  modelled. A seat is idle only while no body exists (whistle trips, ~0.5 s around faceoffs).
  In 4v4 a team never drops below G, LD, C, LW (the other team gains RD/RW, AI, which the
  substitutes may use).
- **Pulled goalie.** The G seat plays a skater / the extra attacker (above) and gets the goalie
  back at the faceoff. When every skater belongs to another seat, the G seat idles.
- **Penalty shots / shootouts.** `ctl_mode` PS (above). Shootout rotation by `shootoutround` is
  **UNVERIFIED**: no shootout script (`so_rotate`) could be driven (Play Mode row of the menu not
  found; `OptPlayMode = 4` written to RAM does not start one). Penalty shots are exercised with the
  test-only `NHL94_SEATS_TEST_PS` (stages `getBAplayerInfo`'s state plus a Tripping call).
- **B button.** No switching. With an AI teammate on the puck it calls for
  a pass (above); otherwise it still sweep-checks when the seat's player is
  the nearest to the puck (`chgplayer` → `Sweepcheck`); B-hold "take the
  goalie" does nothing. Pass receivers stay AI unless another seat owns them.
- **Fights.** NHL 94 has none.
- **Camera timing.** The canonical camera still exists and still changes
  timing (lag frames, HV counter; VIEW_RESEARCH.md §5) and the goal
  celebration / shootout placements; only the goalie hand-over rule is gone.
- **Demo.** Without seats nothing changes (hash-identical). With seats on
  the Demo menu setting, see "Menu setting".
- **Netplay with more than 2 seats** needs the hub transport; LAN / direct
  rooms are two seats. 6 and 12 seats are verified offline (scripts,
  rollback probe); not yet in a real multi-process session.
- The room list does not show the host's Control mode; in the waiting room
  the seat labels do (no labels = Classic). The labels reflect the caps the
  host published; change the host's seat list before creating the room. The own-seat camera view is decided at the first game frame of
  the process: if that first session is Classic, a later Position-lock
  session plays without it (restart the program).
- The G / L3 toggle was verified through `NHL94_VIEW_FOLLOW=puck`
  (screens `seats-work/tgcomp/grid_t001500.png`), not by pressing keys.

## 6 and 12 seats

12 seats = 6v6 with goalies: every on-ice player can be a human. Seats 1-4
(index 0-3) are the game's own pads 1-4 as above. Seats 5-12 (index 4-11)
are **virtual pads**; nothing else in the design changes (`seat_target`, the
enforcement, the remap, the goalie rule and call-for-pass are per seat).

**How.** The game time-multiplexes its own pads: in `updateplayers`, for
each object `d6` = SCnum, pads 1/2 run `doinput` when the object is
`c1playernum`/`c2playernum`, then at `.t1` ($AD42) pads 3 and 4 do the
same, each swapped into pad 1's / pad 2's slots (team, player, B/A hold
timers, pad word, `PadControlBits`) around `ReadJoy3/4` + `doinput`, and
`.t1cont` ($AE7C) runs the AI assignment. A seat k ≥ 4 reuses that code:

- `enforce` (updateplayers entry) writes seat k's team and player into its
  virtual pad instead of `contNteam`/`cNplayernum`.
- At `.t1`, when `d6` is seat k's player, its virtual pad goes into pad 3's
  slots (home seat) or pad 4's (away seat): `cont3/4team`, `c3/4playernum`,
  `bholdtimer34`, `aholdtimer34`, `padword3/4`, `PadControlBits34`,
  `lj3/4` and `PadRaw3/4` (its sealed pad, `genesis_sim_pad(k)`, as the
  game's active-low byte like `ReadJoyData`). The real pad 3/4 values are
  parked meanwhile. The game's own pad-3/4 block then reads that pad and
  runs `doinput` for the seat's player in pad 1/2's slots, exactly as for a
  4 Way Play pad, so the `doinput` remap, the call-for-pass B mask and the
  goalie rule apply unchanged.
- At `.t1cont` everything is swapped back (the virtual pad keeps what
  `doinput` changed: hold timers, edge word, pad word).
- One object per pass and one seat per object, so the parked real pad 3/4
  is never wanted meanwhile. A V-blank inside the swap (the replaced
  `ReadJoyData`) writes the real pad's byte to the parked copy and keeps
  the virtual one in the slot. A pass that skipped `.t1cont` would be
  undone at the next `.t1`.

**RAM** (unused stack-bottom block, as `$FFDF00`; Begin's clear stops at
PALflag, deepest stack seen in play `$FFFEC9`; in every snapshot / digest):

| address | contents |
|---|---|
| `$FFDF08..$FFDF1F` | call-for-pass previous B\|X, u16 per seat 0-11 |
| `$FFDF20 + 16*(k-4)` | virtual pad of seat k (k = 4..11, to `$FFDF9F`): `+0` team (0/1 home/2 away), `+2` player SC (-1 none), `+4` B-hold word, `+6` A-hold word, `+8` pad word, `+A` PadControlBits word, `+C` last buttons (edge word) |
| `$FFDFA0..$FFDFB3` | swap state: `+0` seat+1 being swapped (0 none), `+2` slot (0 pad 3, 1 pad 4), `+4..+11` parked cont team, player, bholdtimer34, aholdtimer34, padword, PadControlBits34, lj, `+12` parked PadRaw byte |

Nothing in the block is written for a seat that does not exist, so 1-4
seat runs are hash-identical to the build before.

**Markers.** The game draws pad stars from `PadControlBits` (pads 1/2) and
`PadControlBits34` (pads 3/4) only. Every seat's own view (and every
`NHL94_VIEWS` view) marks its player with the pad-1 star and label (the view
writes the nibble from its target), so a seat 5-12 sees its own star; the
virtual pads' own `PadControlBits` words are not drawn. Other humans' players
show stars only for pads 3/4.

**Verification** (`tools/seats/run_checks12.sh <build> <out>`;
`tools/seats/seat_stats.py` turns `NHL94_SEATS_TRACE=10` into the table:
traced frames per seat / period / held direction, mean vx / vy, distance,
and per-frame checks that the `pfjoycon` mask is exactly the seats' objects
and each seat's pad player word is its object). Builds
`~/nhl94-online-build/seats12` (6-pad engine) and
`seats12-cap12` (12-pad engine work in progress).

6 seats `home:C,away:C,home:RD,away:G,home:LW,away:RW` (`lock6.input`; seat
4 home:LW goes through pad 3's slots while seat 2 home:RD is the real pad
3, seat 5 away:RW through pad 4's while seat 3 away:G is pad 4): 236 traced
frames, pfjoycon mask = the six objects on all, pad player = object on all,
`stray_joycon=0 pad_moved=0`, each virtual pad ran `doinput` on all 2325
game frames. Period 1 = home attacks up, period 2 = down:

| seat | held | period 1 | period 2 |
|---|---|---|---|
| s0 home:C (pad 1) | UP | vy +5930, y +148 | vy −5995, y −161 |
| s1 away:C (pad 2) | UP | vy −6397, y −161 | vy +4493, y +117 |
| s2 home:RD (pad 3) | RIGHT | vx +3139, x +83 | vx −2980, x −81 |
| s3 away:G (pad 4) | LEFT | vx +847, x +39 | vx −1352, x −40 |
| s4 home:LW (virtual) | DOWN | vy −6625, y −184 | vy +4590, y +130 |
| s4 home:LW (virtual), alone | RIGHT | vx +3639, x +48 | vx −5778, x −74 |
| s5 away:RW (virtual) | DOWN | vy +6098, y +166 | vy −4853, y −138 |
| s5 away:RW (virtual), alone | LEFT | vx +4856, x +62 | vx −1841, x −22 |

Equivalence: the same inputs on the same roles with the centres on virtual
pads and the wingers on pads 1/2 (`lock6_swapped.input`) move the same way
(period 1: virtual home C UP vy +5555 / y +157, virtual away C UP vy −5501
/ y −156; real-pad LW DOWN vy −5560, RW DOWN vy +5761). The game differs
afterwards (pad stars, faceoff line-change box are pad 1/2 things), so the
hashes differ.

12 seats, the 6v6 preset order (`lock12.input`: all twelve hold at once,
then seats 5-12 one at a time), 12-pad engine: 294 traced frames, pfjoycon
mask `FFF` (all twelve objects human, AI none) on every one, no vacant seat,
`stray_joycon=0 pad_moved=0`, every virtual pad ran `doinput` on all 2933
game frames. Every seat moves with its pad and flips at period 2, e.g.
s2 home:LW LEFT vx −2720 / +1577, s3 away:LW LEFT +2595 / −1671, s8
home:RD RIGHT +2943 / −2049, s9 away:RD LEFT +5233 / −4720, s6 home:LD
DOWN vy −6951 / +4904, s7 away:LD DOWN +7204 / −6544, s10 home:G LEFT
(period 2) vx +1497, s11 away:G RIGHT −846 / +1296 (centres held UP into
each other at the faceoff dot: |dy| ≤ 6; the LDs' solo RIGHT in period 1
was against the end boards). With the 6-pad engine the same run shows
seats 7-12 vacant on every frame (AI), seats 1-6 human, as designed.

Determinism: `lock6` 3EBD5E5F3A64349D twice and with 6 views (both
engines); `lock12` FBD494B2680E850C twice (12-pad engine); the two 6-view
halves of the 12-seat composite (different `NHL94_VIEWS`) 6B36353B073872DB
both. `rb_probe_seats.sh`: `lock6.input` 6 seats passes=153 divergences=0
asymmetric=0 PASS; `lock12.input` 12 seats passes=173 divergences=0
asymmetric=0 PASS (both engines). The 1-4 seat battery (`run_checks.sh`) is
unchanged on both engines: lock2 3EF834BA226E6E28, lock4 A94E00C87F121117, callpass1
FAA72A99530568E6, callpass2 560C3425A02A9A91, Demo 80CE87C28143ADBF (also
the build before), rb_probe 99 / 209 / 139 passes PASS, netplay TIMELINE
66/66 identical, final 2C4B8FF1E23D0B26 / C334C28EC3B1F508.

Composites of each seat's own view (views mode, the view marks its target):
`~/nhl94-online-build/seats12-work/v6comp/grid_t00{1880,2080,3200,3240}.png`
(6 seats) and `v12comp/grid_t00{1900,2000,3140,3180}.png` (12 seats, two
6-view runs of the same game merged).

**Limits (seats 5-12).**
- START is masked on virtual pads (a pause from them would be the game's
  pause 3/4, whose resume reads the real pad 3/4); pause from seats 1-4.
- The B-hold reset (`holdreset`) only resets the game's two hold words; a
  virtual pad's B-hold timer is not reset by it. B-hold does nothing under
  the lock anyway (switches are refused).
- Faceoff line-change box: pads 1/2 only (as for seats 3/4).
- Other humans' players have no star for seats 5-12 (only the seat's own
  view marks its player).
- Online: rooms larger than 2 need the hub transport and the engine's
  12-slot netplay; not exercised here.

## Drop-in / drop-out

A seat that leaves an online room mid-game becomes CPU for everyone and the game
continues; a player can join a match in progress (replay from tick 0 without
rendering, claim a free / CPU seat, play from the next whistle); the waiting room
has a real Ready toggle and a Swap button. Design, knobs, tests, baselines and the
remaining work: [DROPIN.md](DROPIN.md). Guest RAM `$FFE300-$FFE33F` belongs to it
(join in progress added no RAM and no `game.toml` site).

## Review fixes (game layer): knobs sealed, penalty-shot latch, joiner reservation, marker 2

Independent review findings, each reproduced on the pre-fix build and fixed (no baseline moved:
every offline hash below is identical before and after, `PASSASSIST=off` ones included):

| Finding | Reproduction (pre-fix) | Fix / check (this build) |
|---|---|---|
| Sim-changing test knobs not sealed | 2-process room (`run_checks.sh` call scenario), slot 1 with `NHL94_PASSASSIST=off NHL94_SEATS_NO_AUTOLC=1 NHL94_SEATS_TEST_PS=2500:0:4 NHL94_FORMAT_TEST_PENALTY=2400:home:LD`: slot hashes `675410A9241DCDE5` vs `0B17C4B64542ADB6`, timelines 39 vs 66 (desync). | `nhl94_sim_knob_allowed()`: the five knobs are ignored while `genesis_netplay_active()` (one log line per knob). Same room: both slots `675410A9241DCDE5`, 66/66 timelines, `desyncs=0`, four `IGNORED` lines. Offline scripts / `rb_probe` unchanged. |
| Penalty-shot shooter re-dealt mid-attempt | `drop_checks.sh` section "penalty shot with a join": `ps_4v4.input`, 8 seats, `NHL94_SEATS_TEST_PS=2500:0:4,4000:1:3`, seat 3 (away LW) away 6000..6900: at t=6900 (attempt live) the shot moves from seat 1 (C) to seat 3. Shooter seat leaving (`3@6400`): seat 1 is dealt the shot. | Latch `$FFE304..06` + no join admission during a pending / live attempt or a shootout. `ps_latch_grade.py`: `PS_LATCH PASS` (shooter steady, joiner waits until mode 1); shooter leaving -> the AI finishes (`--leave` PASS). Hashes `psjoin` `ABEF524A72293D5B` x2, `psleave` `7F78667D3F1AC122` x2; rb_probe 266 / 266 PASS divergences=0. Shootout rotation: `shootoutround` is per pair of attempts (`shootout94.asm`), so `cand[(r-1) % n]` was not a per-team repeat; the latch (keyed on team, shooter object, round) covers `n` changing. |
| Joiner's role body reserved | `pen_sub.input`, `home:C,home:LW,home:RW,away:C`, `NHL94_FORMAT_TEST_PENALTY=2400:home:LD`, seat 1 away 2500..3300: seat 2's substitute sits on the LW body; at the joiner's admission (t=3300) `seat 2 leaves substitute -> -1` (t=3398) and he has no player until the faceoff (t=3787). | `role_targets` reserves the role body of every `human_mask` seat that `drop_join_filter` holds back; seat 2 moves to another body at once, during play. `SUB_RESERVE PASS`; `subres` `52E030AF4657B663` x2; rb_probe 233 PASS. |
| Builds banner slot map wiped by every latch | by code reading: `restoreteams` also benches the whole roster, so the map restarts with it (H1 refills each slot at `setplayer`) | **Not a bug, left as is.** Tried keeping the map unless the block changed: the 100000-frame `builds_ps150k.input` run (builds on, 4v4, 3 seats) then differs from the clearing one (`68EEC4994EEB271E` vs `24EFFFEB453DD704`) once the 2nd game of the run starts (a rematch with the same teams inherited the finished game's mapping); clearing = pre-fix hash. Reverted. |
| `SEATS_PASS_VER` not bumped | `p1` image accepted by this build although the drop / join / penalty-shot sim differs | `SEATS_PASS_VER` = 2: `p2:` images; `p1` is refused (`test_format_image.c` checks it), `p3` too. Offline hashes do not carry the marker, so no baseline moves. |
