# NHL 94 per-player view research

RAM and behaviour reference for per-player camera views. Addresses are
68000 addresses (work RAM `$FF0000`-`$FFFFFF`). C defines are in
`game/nhl94_ram.h`.

Each item is tagged:

- **VERIFIED**: seen in a run of the stable build
  (`~/nhl94-online-build/stable`), with how it was checked.
- **FROM SOURCE**: read from the disassembly (`NHL94Genesis/src`) but not
  seen in a run.
- **UNKNOWN**: not established.

Paths like `logic94.asm:426` are relative to `NHL94Genesis/src/engine/`
unless another directory is given. `ram94.asm` and `tables94.asm` are in
`NHL94Genesis/src/`.

## How it was verified

All runs were headless, for example
`./NHL94Recomp NHL94.bin --no-launcher --benchmark N --input-script S`, using
`DUMP_RAM` (a 64 KB dump of `$FF0000`), `SCREENSHOT` and `WRITE_RAM16`.
The scripts, dumps and the parser (`parse.py`, `cmp.py`, `rsaddr.py`) are in
`~/nhl94-online-build/research/`.

`rsaddr.py` works out the address of every `rs` variable in `ram94.asm`
(from `varstart` `$FFFFB000`). Its results match the dumps, for example the
VBint vector is `$00015D9A` in game, SortCords is at `$FFB04A`, and Hscroll
equals `-64-Hpos`.

The runs:

| run | what it does |
|---|---|
| `p1/` | one-player game. Holds UP, checks pfjoycon, the camera and facing. |
| `demo2/`, `cam*/`, `per/`, `lc/` | Players = **Demo** game, chosen in the setup menu. |
| `rep/` | the in-game Instant Replay with **Reverse Angle**. |
| `ai/` | a one-player game switched to all-CPU partway through. |

> **Flag bit convention.** The asm tests flag bits with `btst #n,(word).w`,
> which is a byte operation on the **high** byte of the word. For example,
> `gmdir equ 1` on `gmode` is word mask `$0200`.

---

## 1. Player objects

### Layout

- **Table:** `SortCords` = `$FFB04A`, stride `$80` (`SCstruct`), 16 objects
  (`Sortobjs`). Source: `ram94.asm:449`. VERIFIED.

| SCnum | object |
|---|---|
| 0-5 | home team (`pfteam` 0) |
| 6-11 | away team (`pfteam` 1) |
| 12 | top net, at (0, +268) |
| 13 | bottom net, at (0, -268) |
| 14 | puck |
| 15 | puck shadow |

The positions above are VERIFIED from the dumps. The bench is not stored
as objects. Players who are not on the ice are only roster entries in the
team struct (`tmpdst`: -2 bench, -1 ice, 0 or more penalty box). Source:
`ram94.asm:742`.

### Fields

Offsets are from `ram94.asm:365-448`. A tag on its own means the field was
seen in the dumps.

| off | size | name | meaning | tag |
|---|---|---|---|---|
| +00 | s16 (+2 fraction) | Xpos | x in rink pixels. 0 is centre, +x is right. | VERIFIED |
| +04 | u16 | attribute | VDP attribute xor: `$0800` hflip, `$1000` vflip, bit 0 away palette. A right-handed skater has `$0800` (`collide94.asm:2940`), so his art is the mirrored left-handed set. | VERIFIED |
| +06 | u16 | frame | sprite frame. Bits 0-10 are the frame number; bits 11-15 are xor'ed into the attribute. | VERIFIED |
| +08 | u16 | oldframe | render state: the last frame whose tiles were DMA'd | VERIFIED |
| +0A | u8[8] | VRoffs | render state | FROM SOURCE |
| +12 | u16 | VRchar | render state: fixed VRAM tile base of the object | FROM SOURCE |
| +14 | s16 (+2 fraction) | Ypos | y. +y is up the screen. | VERIFIED |
| +18 | s16 | Zpos | height. Negative means not drawn. | FROM SOURCE |
| +1C, +20, +24 | s32 | OldXpos, OldYpos, OldZpos | positions before this frame's move | FROM SOURCE |
| +28, +2A, +2C | s16 | Xvel, Yvel, Zvel | velocity. Each frame the game adds `vel * 17 * d7` to the 16.16 position (22 instead of 17 on PAL; `logic94.asm` updateplayers). So 1 px/frame is about 3855. Holding UP gave Yvel +7212 while Y rose. | VERIFIED |
| +34 | s16 | position | 0 G, 1 LD, 2 RD, 3 LW, 4 C, 5 RW, 6 extra attacker. Negative (`$FFxx`, from `st position`) means in the box or off the ice. | VERIFIED |
| +36 | u16 | assnum | AI assignment index | FROM SOURCE |
| +38 | u8[8] | asslist | AI assignment queue (asstab) | FROM SOURCE |
| +52 | u16 | SCnum | object index | VERIFIED |
| +54 | u16 | facedir | facing, 0-7 clockwise with 0 = up: 1 up-right, 2 right, 3 down-right, 4 down, 5 down-left, 6 left, 7 up-left. See `vtoa`, `logic94.asm:7065`. Holding UP gave facedir 0. | VERIFIED |
| +58, +5A, +5C | u16 | SPA, SPAnum, SPAcnt | animation id, index and timer | SPA VERIFIED |
| +60, +61 | u8 | newpos, newpnum | requested position and roster index for a change (`$FF` = none) | FROM SOURCE |
| +62 | u8 | pflags | see below | VERIFIED |
| +63 | u8 | pflags2 | bit 2 `pf2unav` is set while a player skates to or from the bench. | VERIFIED |
| +64 | u8 | pflags3 | bit 2 `pf3gctrl` | FROM SOURCE |
| +66 | u8 | pnum | roster index. It changes on line changes. | VERIFIED |
| +6F | u8 | rostnum | jersey number (BCD) | VERIFIED |

`pflags` (+62) bits:

| bit | mask | name | meaning |
|---|---|---|---|
| 3 | `$08` | `pfjoycon` | human controlled |
| 6 | `$40` | `pfteam` | 0 home, 1 away |
| 7 | `$80` | `pfgoal` | 0 = shoots at the bottom goal (-y), 1 = shoots at the top goal |

The other bits: `$01` deceleration off, `$02` new assignment, `$04` no
collision, `$10` skating backwards, `$20` animation lock.

`pflags` was VERIFIED in the dumps: home `$80` and away `$40` in period 1;
in period 2 they became `$00` and `$C0`. The pad-1 player was `$88`.

### Finding a role (home C, home RD, goalie)

1. Scan objects 0-5 (home) or 6-11 (away) for the wanted `position`. Use
   `pfteam` as a cross-check.
2. Skip objects with a negative `position`.

The slot is **not** fixed to a role. VERIFIED in the `lc/` run with line
changes and penalties on:

- **Start:** the mapping was (slot 0-5) = (RW, LW, C, RD, LD, G).
- **After a home penalty:** slot 4 went to the box (position -1). The other
  skaters were re-slotted to (C, RD, LW, LD, -, G).
- **When he came back:** slot 4 returned as RW.
- **Away penalty:** an away slot showed position `-254` (`$FF02`) while in
  the box.

When a team is short-handed, a role is simply missing.
`SetPlList`/`priolist` (`tables94.asm:352`, order 0,1,2,4,3,5,6) drops RW
first, then LW. FROM SOURCE.

When the goalie is pulled, no object has position 0 and one skater is
position 6 (`collide94.asm:2577`). FROM SOURCE.

Line changes keep the slot's position but change `pnum`/`rostnum`. Watch
`pf2unav`, which is set while a player is leaving or entering.

**Human control:** VERIFIED in a one-player game.

- `cont1team`..`cont4team` (`$FFC328`..`$FFC32E`): 0 none, 1 home, 2 away.
- `c1playernum`..`c4playernum` (`$FFC320`..`$FFC326`): the SCnum each pad
  controls, or negative.
- The controlled object also has `pfjoycon` set.
- `PadControlBits` (`$FFBE78`) holds the nibbles `[3:0]` puck carrier,
  `[7:4]` pad 1 player, `[11:8]` pad 2 player, `[15:12]` replay. `$F` means
  none. Observed `$FF00`: carrier 0, pad 1 on 0.

The pad's player is switched by `changeplayer`/`chgplayer`
(`chgplayer94.asm:5`), which picks the nearest free skater to the puck, and
by `setc1player`/`restorepl` (`logic94.asm:1745`).

## 2. Puck

The puck is object 14 at `$FFB74A`, with the same layout as the players.
VERIFIED.

| name | address |
|---|---|
| puckx | `$FFB74A` |
| pucky | `$FFB75E` |
| puckz | `$FFB762` |
| puckvx, puckvy, puckvz | `$FFB772`, `$FFB774`, `$FFB776` |
| `puckc` | `$FFB7AA` |

`puckc` is a word: the carrier's SCnum, or negative when the puck is
loose. It equalled the carrying object in the dumps.

At a faceoff the puck is placed at (`fox`, `foy`) = `$FFBF94`/`$FFBF96`
(0,0 at period start). VERIFIED.

The puck shadow is object 15. Its `Zpos` is set separately (replay record).

## 3. Coordinate system

All the measurements below are in rink pixels. One unit is one screen
pixel; the game screen is H32, 256x224.

**VERIFIED:**

- **Origin:** centre ice is (0,0) (period-start faceoff, centre logo).
- **Axes:** +x is right and +y is up the screen.
- **Nets:** (0, +268) and (0, -268).
- **Goalies:** about ±245 to ±249.
- **Goals:** the puck reached y = ±272 when a goal was scored.

**FROM SOURCE:**

- **Blue lines:** ±88 (`Blueline $58`, `ram94.asm:485`).
- **Goal lines:** ±264 (`Blueline+goalline`, `logic94.asm:6699`).
- **Side boards:** x ±136 (`Sideline $88`). Fall/wall code uses x ±130.
- **End boards:** y ±296 (`$128`, SetShotMode). The replay manual camera
  limits are x ±136 and y ±294.
- **Corners:** rounded (wall collision in `collide94.asm`).

**Symmetry:** the game logic is 180° symmetric in coordinates (goals at
±268, `pfgoal` and `gmdir` flip, and the AI uses `pfgoal`-relative
targets). The rink *art* is not symmetric, because the 3/4 view has a
perspective. The game keeps a separate reversed rink tile set and map
(`RevRinkTiles`/`RevRinkTilelist`, `graphics94.asm:121`). VERIFIED in the
reverse-angle replay (§7).

**Camera clamps:** Hpos -60..60 and Vpos -200..256. They are deliberately
asymmetric in y.

## 4. Which team attacks up

- **`gmode` bit `gmdir`:** word mask `$0200` at `$FFC2EA`. Clear means the
  home team attacks up (+y, the top net).
- **Per period:** `PeriodOver` toggles it every period
  (`setup94.asm:742`).
- **Per player:** each period `setupice` rebuilds the objects from the
  defaults (home `$80`, away `$40`) and flips `pfgoal` for the 12 players
  when `gmdir` is set (`setup94.asm:67`).

VERIFIED:

| period | gmode | home pflags | away pflags | home goalie y |
|---|---|---|---|---|
| 1 | `$0000` | `$80` | `$40` | -247 |
| 2 | `$0200` | `$00` | `$C0` | +245 |

Per-player truth is `pflags` bit 7.

> **Warning.** Between periods the game plays "Hockey Night" highlights of
> *other* games in the same SortCords. This shows as `gmode` `$1000`
> (`gmhl`) with random clock and score. Ignore the object state while that
> bit is set. VERIFIED.

## 5. Camera

### State

The camera state is VERIFIED.

| variable | address | meaning |
|---|---|---|
| `Vpos` | `$FFBD18` | camera centre y |
| `Hpos` | `$FFBD1C` | camera centre x |
| `Hscroll` | `$FFBD1E` | `-64 - Hpos`, written to the VRAM hscroll table |
| `Vscroll` | `$FFBD20` | `-Vpos`, written to VSRAM |
| `Oldrow` | `$FFBD1A` | last streamed map row. `$7D0` forces a full redraw. |
| `xc1`, `yc1` | `$FFBF8E`, `$FFBF90` | target, or lock point |
| `yleader` | `$FFBF92` | look-ahead toward the carrier's attack goal |

The flag `sflags` `sfslock` (word mask `$4000`) locks the camera to
`xc1`/`yc1`.

### Update routine

The update is `checkwindow`, `logic94.asm:426` (ROM `$AFCA`). It is called
once per game frame from `DoGameFrame`, after `updateplayers`.

1. **Target.** If the puck has a carrier, the target is the carrier: x =
   carrier X, and y = carrier Y + Yvel/128 + `yleader`. Otherwise the
   target is the puck (`yleader` is still added). `yleader` moves 2·d7 per
   frame toward ±50, following the carrier's `pfgoal`. When `sfslock` is
   set, the target is `xc1`/`yc1`.
2. **Dead zone.** ±10 px vertically and ±40 px horizontally. Outside it,
   the camera aims at the target minus or plus the zone, clamped to Vpos
   -200..256 and Hpos -60..60.
3. **Smoothing.** Each frame the camera moves 1/16 of the remaining
   distance, at least 1 px.

VERIFIED from the dumps. For example, the target equalled the carrier X and
Y + Yvel/128 + 50, and Vpos stayed put while |target - Vpos| < 10.

`freezewindow` (`logic94.asm:421`) locks the camera at the current
position. The game uses it at period end and in the shootout.

Faceoffs and period start reset the camera to 0, then run `checkwindow`
101 times so it settles (`logic94.asm:6040-6069`, `5372-5401`). FROM
SOURCE.

The replay uses its own camera: `UpdateCameraPos`, `replay94.asm:420`
(`$A616`), and `ClampReplayView`, `replay94.asm:621` (`$A88C`).

### Game logic that reads or writes the camera

All found by searching the non-Sega-CD sources for
`Hpos/Vpos/Hscroll/Vscroll/xc1/yc1/yleader/CameraPosStruct`. FROM SOURCE.

| where | what | effect on the simulation |
|---|---|---|
| `assgoaliectrl`, `logic94.asm:3243` (`$D3C6`), lines 3265-3290 | Applies only to a human-controlled goalie (`pfjoycon`). If he is more than 116 px in x or 100 px in y from Hpos/Vpos, it sets `pf3gctrl` and the goalie AI takes over (see also `logic94.asm:3463`). | Logic depends on the camera. |
| `assscore`, `logic94.asm:2645` (lines 2653-2657) | On a goal, celebrating skaters get a skate-to target of x = ±90 (sign of Hpos) and y = Vpos. | Logic depends on the camera. |
| `assstanley`, `logic94.asm:2620` (lines 2626-2630) | Same as `assscore`, for the Stanley Cup skate. | Logic depends on the camera. |
| `EndShootout`, `period94.asm:4` (lines 28-32) | The winning skaters are placed at y = Vpos ±240. | Logic depends on the camera. |
| `skateto`, `logic94.asm:6866` | When the puck has `pf2fight` set (`puck_pflags2`), a still player faces `xc1`/`yc1` instead of the puck. 94 has no fights; `ShowInjuryBox` uses the bit only for text. Probably dead. | Probably none. |
| writers: `setInjuryType` (`collide94.asm:964`), `CheckBump` (`collide94.asm:1525`), `collide94.asm:1562`, `logic94.asm:5714`, `freezewindow` | They set `xc1`/`yc1` and `sfslock`. These are camera writes, not reads. | None directly. |
| `updatereplay`, `replay94.asm:725` | Records Hpos/Vpos into the replay buffer. | Presentation only. |

The rest are rendering and presentation only: `updatescroll`, `find3d`,
`checksso` (off-screen arrows), `showcrowd`, `AddArenaAnimSprite`, and
replay. No sound panning reads the camera; `updatesound` and `Crowd_Noise`
use `Crowdlevel`.

### The camera also changes the simulation through timing

**VERIFIED.** Two identical Demo runs gave byte-identical RAM after 900
and 2100 frames. The runner is deterministic.

A third run forced Vpos=256 and Hpos=-60 for 900 frames (`cam_c`). It
diverged: positions, the RNG seed and the scores were all different.

A per-frame run (`cam2/`) forced the camera for 100 frames:

- Before frame 170, only render state, sound, crowd counters and the stack
  differed.
- At frame 170 the puck diverged, then everything did.
- The lag-frame pattern (frames where `d7 = Vcount - OldVcount > 1`)
  differed between the runs (frames 177/187 against 182/191).

A control run (`cam3/`) touched no camera at all. It only forced
`oldframe = $FFFF` on the 12 players for 100 frames, which makes the
game re-DMA every player's sprite tiles. It also diverged, at frame 182.

The mechanism, FROM SOURCE plus the runs above:

1. **Rendering cost changes timing.** The camera position decides how many
   map rows are queued and which sprites are drawn and DMA'd (culling and
   tile caching per object). That changes the time spent in `setvideo` and
   the vblank DMA.
2. **Lag frames.** `DoGameFrame` runs the simulation with `d7` = vblanks
   elapsed (`hockey94.asm:171`). One step with `d7 = 2` is not the same as
   two steps with `d7 = 1`.
3. **The H/V counter.** Game logic reads it (`$C00008`) as a random
   source, so the result depends on the beam position when the code runs.
   Readers include `logic94.asm` 3041, 3198, 3495, 4143, 4266, 4307, 4352,
   5038, 6405 and 7609; `collide94.asm` 432, 443, 682, 1493 and 2253; and
   `hockey94.asm` 287 and 294. The recomp models this
   (`genesis_bus.c:376`, `gvdp_read_hv_counter_at(line_cycle)`).

**Consequence for netplay:** rendering a different camera per client
inside the simulating machine **will desync** the clients. Possible
approaches:

- Run the simulation with a canonical camera, or with timing that does not
  depend on rendering, and draw each client's view in a separate pass that
  does not feed back.
- Make `d7` and the HV reads deterministic.

The camera-dependent AI targets above also need a canonical Hpos/Vpos
during logic.

## 6. Frame loop

Everything here is FROM SOURCE, unless marked VERIFIED.

### Main loop

`Gameloop` is at `hockey94.asm:163` (`$7938`). It runs `DoGameFrame`,
then `demoread` (the demo/no-pad check, `hockey94.asm:375`), then the pause
check.

`DoGameFrame` (`hockey94.asm:171`, `$794E`) does this each frame:

1. Busy-waits until `Vcount` changes, and sets `d7` = elapsed vblanks.
2. `periodicevents` (`$799E`): penalties, crowd animation, arena animation,
   crowd sound, and `clockcont`. Once a second it also runs ChkGoalies,
   excitement, the late-game song, bench energy, injuries and the power
   play.
3. `updateplayers` (`logic94.asm:8`, `$A9D6`). For each of the 16 objects:
   - saves the old position;
   - runs `updateanim` (frame from SPA and facedir);
   - applies friction, velocity and gravity;
   - reads the pad for a pad-controlled puck carrier (`doinput`);
   - runs the AI assignment (`asstab`, `tables94.asm:23`);
   - runs collisions (`checkcoll`).
4. The song delay, then `setSlotBit`.
5. `checkwindow`: the camera.
6. `updatereplay` (`$A8CA`): records the replay frame.
7. `setvideo` (`video94.asm:98`, `$15EC0`). This is the sprite list build,
   not run in vblank:
   - waits for `dfok` to clear;
   - `updatescroll` (`$15F34`) sets Hscroll/Vscroll and queues the rink
     rows on `DMAList`;
   - builds `Satt` with `checkfo` (faceoff overlay), `checksso` (arrows),
     `setsortcords` (`$165FC`, the 16 objects in `OOlist` order), `setffo`
     (pad stars, labels, gloves), `showclock`, `showzam`,
     `AddArenaAnimSprite`, `showcrowd` and `showref`;
   - calls `Crowd_Noise`;
   - sets `dfok`.

### Vblank

The vblank handler is `VBlank`, `video94.asm:5` (`$15D9A`; VBint = this
in game, VERIFIED). It runs:

1. If `dfok` is set, `DumpSprites`:
   - `SetScroll2` writes Hscroll to VRAM `$FC00` and Vscroll to VSRAM;
   - DMA of `Satt` to VRAM `$DC00`;
   - `dodmalist`: every queued tile and map-row DMA.
2. `cramfade`.
3. The game clock tick (the clock lives in vblank).
4. `Vcount++`.
5. Music.

So the order is: logic, then camera, then sprite list and DMA queue (main
thread), then DMA (next vblank). The picture shown is one frame behind the
logic.

### Culling

Off-camera objects are culled. `addframe` (`video94.asm:847`) calls
`find3d` (`video94.asm:987`, `$16920`):

- screen x = X - Hpos + 128;
- screen y = 112 - (Y - Vpos + 1.5·Z).

An object is drawn only if `|X-Hpos| <= 144` and
`|Y-Vpos+1.5Z| <= 144`. Culled objects get no sprites and no tile DMA.
FROM SOURCE; it fits the divergence result above.

The draw order is `OOlist`: SCnum·2, ascending Y (nearest the camera
first, which is the highest VDP priority). VERIFIED: the `OOlist` dump
starts with the bottom net and the bottom goalie and ends with the top
net. The order is kept by `SprSort` (`setup94.asm:561`) and by
incremental swaps in `checkcoll`.

### Sprite tiles

Each of the 16 objects owns a fixed VRAM tile area (`VRchar`, allocated
by `defaultsprites_0`, `setup94.asm:361`). `addframe2`
(`video94.asm:863`, `$167AA`) queues a DMA of that frame's tiles from ROM
(`Spritetiles`) only when `frame != oldframe`, and only for visible
objects. So the tiles are per object, sent on frame change; there are no
fixed tile sets. The frame list is `Sprites` (jim `.anim`), 8 bytes per
hardware sprite piece. FROM SOURCE.

### Rink background

- **Planes:** H32 mode, 64x32-cell planes (`setVram`,
  `middle94.asm:716`). The rink is on plane B at `VmMAP2` = `$C000`. Plane
  A at `$E000` holds the HUD, clock and ref. The window plane is at
  `$F000`.
- **Map:** the rink map is 48x113 cells (384x904 px). This was read from
  the ROM map header at `$5605A`; the reversed map at `$B5180` is also
  48x113.
- **Streaming:** `updatescroll` (`video94.asm:133`) streams whole 48-word
  map rows into a 32-row ring (`row & 31`). It sends up to 32 rows a frame
  when `Vpos>>3` changes (`Oldrow`).
- **Scroll mode:** full-screen scroll. Horizontal scroll is done only with
  `Hscroll`; the plane is 64 cells wide, wider than the 48-cell map.

## 7. Sprite direction and 180° rotation

### How a frame is picked

`updateanim` (`logic94.asm:359`) picks the frame as follows:

```
SPAlist[SPA] has 8 per-direction offsets + attr
d = facedir
if attribute & $0800 (right-handed, hflip): d = (8 - d) & 7
frame = table[d][SPAnum]
```

There is art for all 8 directions (for left-handed players). Right-handed
players use the hardware hflip of the mirror direction. `addframe2`
applies the hflip and vflip offsets per piece. FROM SOURCE.

### 180° rotation through the game's own renderer

**VERIFIED: it works, and the game already does it.**

The Instant Replay *Reverse Angle* (pause, Instant Replay, START, C) draws
a correct 180° view. The rotated rink is drawn from `RevRinkTiles`, and
the players face the right way (screenshot `rep/r2.png`).

The RAM comparison between the normal and reverse replay frames
(`rep/r1.bin` and `rep/r2.bin`) shows:

- the X and Y of all 16 objects negated, nets included (they swap ends);
- Hpos and Vpos negated;
- `sflags4` = `$1000`;
- frames remapped through `revframetbl` (ROM `$F600E`, for example
  11→31, 21→1, 145→153, goalie 539↔551, nets 405↔406);
- attribute flip bits unchanged.

A 180° rotation keeps handedness, so no hflip change is needed.

The code is `RestoreReplayFrame`/`RevReplayAdj` (`replay94.asm:456-618`,
`$A67C`), `updatescroll`, `setupice` and `ReloadRinkGraphics`
(`front/stats94.asm:1951`).

### What to transform for a mirrored view

**Per object (all 16):**

| item | transform |
|---|---|
| Xpos, Ypos | `x→-x`, `y→-y` |
| puck shadow (15) | Y gets +2 after negation. If its frame is not `SPFpuck`, Y = `$FC00` (hidden). |
| frame | via `revframetbl`. Keep frames outside 1..`$34D` as they are. |
| glass-shatter frames | `SPFbglass`..+4 get Y = `$190` |
| old fight frames | `SPFfight`..`SPFPen` toggle hflip |

Using facedir+4 instead of `revframetbl` would also pick the right art in
principle. But the frame is only re-derived when the animation advances
(and `glitch` limits switching), so remapping `frame` is the reliable way.
FROM SOURCE.

**Camera:**

- Hpos and Vpos negated, then clamped (`ClampReplayView`).
- `Oldrow = $7D0` to force a full redraw.

**Background:**

- Set `sflags4` bit 4 (`$1000`). `updatescroll` then reads
  `RevRinkTilelist`.
- `RevRinkTiles` must be in VRAM at `rinkvrcset` (reload).

**Draw order:**

- Re-sort `OOlist` (`SprSort`) on the negated Y.

**Turned off in reverse by the game:**

- crowd sprites (`showcrowd`);
- arena animation;
- penalty-box crowd frames.

**Not transformed by the game:**

- **Faceoff close-up sprites:** fixed screen positions `fodropx`/`fodropy`
  and `fofdata`.
- **Ref overlay:** `RefRamMap`.
- **Zamboni:** `Zamx`, between periods only.
- **Gloves:** `glovecords`, fight leftover.
- **Pad stars, labels and off-screen arrows:** follow from SortCords and
  Hpos/Vpos, so they come out right.

**Velocities:** not used for drawing; the replay leaves them.

**Render state per view:** `oldframe`, `VRoffs` and the VRAM contents
(tile cache, rink rows) belong to one renderer. Each view needs its own,
or a forced full re-DMA.

## 8. All-CPU game

- **Menu option (VERIFIED).** In the setup screen, set Players to
  **"Demo"**: from "One - Home", press LEFT once (`OptNOP` = 0,
  `front/optsetup94.asm:714`). This gives a real game, not the canned
  attract replay:
  - `FigureJoy` sets `cont1team = cont2team = 0` (`setup94.asm:1687`);
  - `c1playernum` and `c2playernum` are -1, and no object ever has
    `pfjoycon`;
  - all 12 players use the AI;
  - the game ran through period 1, the intermission and period 2 with no
    input.

  The idle attract demo (`SetupDemo`, `front/optsetup94.asm:969`, after
  `$5460` frames) is the same mode, with line changes and penalties forced
  on.

- **Caveat.** In this mode `demoread` (`hockey94.asm:375`) reads pads 1 and
  2 every frame. START pauses, and **any other button exits to the title**
  (`EndDemoOnPress` → `ExitToOpening`). Do not feed pad input into such a
  game.

- **Mid-game switch (VERIFIED).** Writing `cont1team = cont2team = 0` and
  `c1playernum = c2playernum = $FFFF` into a running one-player game makes
  it all-CPU, but not at once:
  - the object that was pad-controlled kept `pfjoycon` and was a frozen
    zombie until the next faceoff (after a goal);
  - after that no object had `pfjoycon`, and the game played on (goals,
    clock).

  To switch at once, also clear `pflags` bit 3 on all 12 objects and set
  `pfna` (`$02`) on the old pad player. That is what `restorepl` does
  (`logic94.asm:1745`). FROM SOURCE.

  Also zero `cont3team`/`cont4team`. Leave `PadControlBits` alone; it is
  only used for the pad stars.

## Open items

- **Recomp timing:** how closely the recomp's DMA and cycle timing matches
  hardware for lag frames is UNKNOWN. The divergence was seen in the recomp
  only.
- **H/V counter reads:** individual readers were not classified. Some may
  be in presentation code only.
- **Goalie pulled and short-handed:** the role mapping is FROM SOURCE. It
  was not seen with a pulled goalie.
- **Screen projection:** the formula is FROM SOURCE. It agrees with the
  scroll values but was not checked sprite by sprite.

## Implementation notes (per-player views, game/nhl94_view.c)

- **Where:** only in the engine's speculative view ticks (`runner/multiview.h`).
  A `game_hook` site at `$007998` (`jmp setvideo`, end of `DoGameFrame`)
  applies the view after the frame's logic and `checkwindow`: target lookup,
  Reverse Angle transform (as `RestoreReplayFrame`), RevRinkTiles copied into
  VRAM at `rinkvrcset` (both tile sets are unpacked: $1C6 / $1C4 tiles; the
  home logo moves from tile $1AE to $1AC), camera Hpos/Vpos, and the 32-row
  plane B ring written directly (the game's own `Oldrow=$7D0` DMA redraw
  reached the top rows after the next raster had passed them, because the
  vblank DMA stall pushes the row transfers late in a mirrored frame).
- **Timing (measured):** the tick boundary is at line 0; `DoGameFrame`
  reaches `jmp setvideo` around lines 90-180, the list is uploaded at line
  224 and shown in the NEXT tick, so a view needs 2 ticks. About half of
  the in-game view frames need a 3rd tick (setvideo finishes after line
  224); sites `$015F28` (setvideo `bset #dfok`) and `$015DB2` (VBlank `bsr
  DumpSprites`) let `view_ready` detect that.
- **Marker:** the game's own pad-1 star/label (`uppads`, PadControlBits
  nibble [7:4]) set to the target in the clone; its sprites are moved to the
  head of `Satt` so goalies' stars are not hidden by the net.
- **Verified:** state hash identical views off/on, rb_probe PASS with 6
  views, no-op debug views byte-identical to the main frame.
