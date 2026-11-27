# 4v4 mode (C, LW, LD, G) — plan and implementation

Status: **implemented 2026-11-24** (approach A, steps 1-6 and 8; phase 7 AI
centring skipped by decision). Code: `game/nhl94_format.c/.h`, with
`nhl94_seats.c` (format in the seats value, RD/RW refusal), `nhl94_mode.c`
(launcher Format option, Full 4v4 preset), `nhl94_spec.c`, `nhl94_ram.h`,
`game.toml` (10 sites). Measured results and the deviations from this plan
are in **§10** at the end; the plan text below is kept as written, with
"→ §10" notes where the implementation differs. Not yet play-tested by a
human. **§11 (2026-11-25):** in 4v4, penalties called in a hit are penalty
shots instead of minors. Tags: **[S]** read from source, **[R]** checked against the ROM bytes,
**[U]** unverified (needs a run or a play test), **[M]** measured in a run.

## 0. Naming

"4v4" here means **four bodies per team: G + C + LW + LD**, like EASHL. The game
counts bodies *including the goalie* (`tmap`, "active players on ice 4-6",
`ram94.asm:719`). So our 4v4 is `tmap = 4` per team. In NHL rules terms this is
3-on-3 skaters, which the cartridge can already produce for one team at a time
in a 5-on-3 (its lower limit, see §1.2).

## 1. How the game puts players on the ice and takes them off

### 1.1 Objects, positions, lines

- **12 player objects** (SortCords). SCnum 0-5 are home and 6-11 are away, each
  `$80` bytes (`nhl94_ram.h:28-38`). A team **always has 6 objects**. Fewer
  players means objects with `position < 0`, never fewer objects. Hard-coded
  `moveq #5,dN` loops run over them everywhere (e.g. `setpersonel`
  `collide94.asm:2520`, `forcepldata` `collide94.asm:2708`, `SetupPenaltyShot`
  `collide94.asm:2669`).
- `position` (object `+$34`): 0 G, 1 LD, 2 RD, 3 LW, 4 C, 5 RW, 6 extra
  attacker. Negative means off the ice, bench or box (`nhl94_ram.h:58`).
- The team struct holds:
  - `tmlines`: 7 lines × 8 bytes. Lines 0-2 are even strength, 3-4 power play,
    5-6 penalty kill. Each is a roster number per position slot
    (`ram94.asm:749`).
  - `tmline`: the current line.
  - `tmap`: bodies on ice, 4-6.
  - `tmgoalie`: negative means the goalie is pulled.
  - `tmpdst[26]`: -2 bench, -1 ice, -3/-4 injured, >0 seconds in the box with
    flag bits 4/5/6 (coincidental is bit 6).
  - `tmboxlist`: the penalty box queue (`ram94.asm:715-760`).

**Who goes on is decided by one routine, `SetPlList`** (`collide94.asm:2577`,
ROM `$015832`). It takes the **first `tmap` entries of `priolist`**
(`tables94.asm:352`, ROM `$019286` [R]):

```
priolist  dc.b 0,1,2,4,3,5,6     ; G, LD, RD, C, LW, RW, X
```

and fills each slot from the current line, substituting via `sublist` or the
whole roster for boxed or injured players. With the goalie pulled it starts one
entry later (`addq.w #1,a0`), giving LD, RD, C, LW, RW, X.

- `setpersonel` (`collide94.asm:2520`) turns that list into per-object
  `newpos`/`newpnum` requests.
- At a faceoff, `forcepldata` (`collide94.asm:2708`) applies them at once.
  Objects with no request get `position = -1` and are parked at x = -240
  (`puckfaceoff2 .10`, `logic94.asm:6077-6082`).
- During play, `check4bench` (`logic94.asm:2212`) skates players on and off.

### 1.2 How penalties take players off

1. `AddPenalty` (`penalty94.asm:5`) queues the call in `PenBuf`. Penalties off
   (`OptPen = 0`) drops every call except icing.
2. `InProgress` (`penalty94.asm:225`) logs it and writes the box time into
   `tmpdst`. It appends to `tmboxlist` and assigns `asspenalty`, so the player
   skates to the box with `pf2unav`. `assdopen` then sets `position = $FFFF`
   (`logic94.asm:2480-2490`).
3. When `PenBuf` is empty, **`InProgress_ap`** (`penalty94.asm:367`, ROM
   `$0123DE`, IDA name `ai_CountAvailableSkaters`) recomputes
   `tmap = 6 − (boxed, non-coincidental players)` with a hard floor
   (`cmp.w #4,d1 / beq .nap`: "never below 4"). It does this for both teams.
4. At the faceoff, `SetPlList` uses the smaller `tmap`. With `priolist`,
   **5 bodies drops RW and 4 drops LW too**: a short-handed team keeps
   G, LD, RD, C. This matches the "drops RW, then LW" note in
   `PLAYER_LOCK.md` "Known limits".
5. Each second, `updatepentime` / `updatepentime_ut` (`penalty94.asm:766-858`)
   count down the first two boxed players. At 0, **`releasepl`**
   (`penalty94.asm:865`) puts one player back on mid-play:
   - it does `addq.w #1,tmap(a2)` (ROM `$0129F0`);
   - it uses `position = priolist[old tmap]`;
   - it sets `tmflcc` on both teams.
6. On a power-play goal, **`PenGoalStuff`** (`penalty94.asm:729`) acts:
   - If the scored-on team has exactly 6 (`cmpi.w #6,tmap(a1)`, ROM `$01284E`),
     pending calls are cleared (`ClearPenaltyBuffer`).
   - If the scorer had more bodies, the scored-on team's first boxed player is
     released and that team's `tmap` goes up by 1 (ROM `$01288A`).
7. Power-play UI and time:
   - `updatepwrplay` (`scoreboard94.asm:6`) shows a power play when the two
     teams' `tmap` differ.
   - `GetLowestPen` (`penalty94.asm:892`) computes the power-play time and
     tests `cmpi.w #6,tmap(a3)` (ROM `$012A50`).
8. `restoreteams` (`hockey94.asm:82`) starts each game with
   `move.w #6,tmap(a2)` (ROM `$007800`).

### 1.3 Can 4v4 reuse the penalty mechanism?

**Yes, and it is the best route. But not as "both teams permanently
short-handed by RW and RD".** Three things in the stock mechanism are wrong
for us:

| Stock behaviour | Problem for 4v4 | Fix |
|---|---|---|
| `priolist` keeps G, LD, RD, C at `tmap = 4` | Wrong set: we want G, LD, C, LW | Point the three `priolist` readers at our own table: G, LD, C, LW, RD, RW, X |
| `.apl` faceoff row for `tmap = 4` maps LD→LW spot, RD→RW spot, LW→LD spot (`logic94.asm:6282`) | With our set, LW lines up at the LD spot and LD on the wing | Our own `.apl` rows (§3) |
| Floor of 4 in `InProgress_ap` | A penalty in 4v4 cannot reduce the team. Today the boxed player would just be replaced by a sub, and `releasepl` would later add a **5th** body | Our own strength rule (§1.4) |

What already works with a permanent `tmap = 4` on both sides, because the code
compares the two teams' `tmap` rather than testing for 6 [S]:

- **Power-play display:** `updatepwrplay`, `scoreboard94.asm:14`. Equal
  strength means no power play.
- **AI power-play / penalty-kill switch:** `chkpk` via `sf2pwrplay`
  (`logic94.asm:4325`).
- **Line picks:** `CompLine` PP/PK lines (`logic94.asm:5897`) and `AvgCline`
  (`scoreboard94.asm:485`).
- **Line-change menu rows:** `getlchoice` (`logic94.asm:1196`).
- **Short-handed icing rule:** `logic94.asm:6682`.
- **Score-summary PP/SH byte:** `collide94.asm:1330`.
- **Lead songs:** `logos94.asm:49`, `ui94.asm:266`.

The places that assume **6 = full strength** are the four literal sites in
§1.2 steps 3, 6, 7 and 8. Those are the hook list.

### 1.4 Penalties in 4v4: the NHL 3-on-3 overtime rule

A 4v4 side cannot go lower (the game's AI and faceoff tables stop at 4, and
2 skaters is not hockey). So use the rule the NHL uses in 3-on-3 overtime: **a
penalty adds a skater to the other team.**

```
boxed(T) = T's boxed players without a coincidental penalty  (InProgress_ap's own test)
tmap(T)  = 4 + clamp(boxed(other) − boxed(T), 0, 2)
```

Examples:

- 1 minor: 5 bodies vs 4 (4-on-3 skaters).
- 2 minors on the same team: 6 vs 4.
- One minor on each team, not coincidental: 4 vs 4.

This fits every stock consumer:

- The side with more bodies is the power-play side.
- The shorthanded side is the one with boxed players, which is what
  `GetLowestPen` and `PenGoalStuff` expect.
- The extra skaters come from our list's tail (RD, then RW), all AI.

Mid-play changes:

- **Penalty expires** (`releasepl`): the stock code gives the penalized team +1
  (4→5) while the opponent still has 5. That is 5v5 (4 skaters each) until the
  next whistle, which is also what happens in NHL overtime. It needs no hook.
- **Power-play goal** (`PenGoalStuff`): the boxed player is released (+1) and
  the faceoff follows anyway.
- **At every faceoff**, a normalisation hook re-applies the formula. Then
  `setpersonel` + `forcepldata` drop the extra bodies with no skating
  (`puckfaceoff2`, `logic94.asm:6069-6073`).

**Ship phase 1 with this rule already in place** (it is a small amount of
code). Also offer "Penalties off" in the game's own options menu as the
low-risk way to play until it has been play-tested.

## 2. AI with fewer skaters

The AI was researched by a read-only agent and spot-checked. Summary with
citations:

- **No per-formation positioning tables.** Targets are computed from the
  puck, the zone and the position number. Defaults come from `Setplass .alist`
  (`collide94.asm:2786`): LD/RD use `assdefd`, LW/RW use `asswingd`, C uses
  `asscenterd`.
- **Short-handed awareness is only the power-play/penalty-kill flag**
  (`chkpk`). It is driven by the `tmap` difference, so it does not fire at
  equal strength. Even-strength 4v4 plays the even-strength AI, which is what
  we want.
- **Absent objects are skipped** (`position < 0`, `pf2unav`) in:
  - the assignment dispatch (`logic94.asm:121`)
  - `chgplayer`
  - `assnearest` (`logic94.asm:4753-4770`)
  - the CPU pass pick (`logic94.asm:4496-4526`)
  - offside tests
  - `assdefd`'s opponent scan (`logic94.asm:2828-2877`)
  - `resetplstuff`

  This is the same state as during a 5-on-3, so it is exercised code.
- **Side asymmetry [S].** RD and RW are the "special case" sides:
  - `assdefd`/`assdefo` put RD at +x and **every other D at −x**
    (`cmpi.w #2,position(a3)`, `logic94.asm:2768ff`).
  - `asswingd`/`asswingo` flip x only for RW (`logic94.asm:2944`, `3077`).

  So with RD and RW gone, **the lone LD guards the left side and the lone LW
  works the left wing**. The right side is covered only by C and by
  `assnearest`. This is how the stock 5-on-3 behaves too (G, LD, RD, C there).
  It is playable but lopsided **[U]**. A polish hook could centre the lone D
  (x = 0) and mirror the winger to the puck's side (phase 3).
- **Risks to check in a run [U]:**
  - Some loops only skip the goalie (`position == 0`), not absent objects:
    - the pass-lane / shot-line test (`logic94.asm:4412-4430`)
    - the breakaway scan over all 12 objects (`logic94.asm:4052-4075`)
    - possibly the check-target loop (`logic94.asm:4999-5030`)
  - They see parked objects at x = −240, y = 0 (the faceoff parks them there).
    y = 0 is centre ice, so they should never look "behind" a carrier past the
    blue line. But in 4v4 two objects per side are parked *all game* instead of
    for two minutes. Verify that a breakaway is still detected and that no pass
    is blocked by a ghost.
  - Prior art (§3a) lowers this risk. The community's 2on2/3on3 ROMs have
    left the same objects parked all game for years. Still verify it in our
    build.
  - Also confirm that parked objects are never drawn: x = −240 is outside the
    ±(60 + 160) visible span, but check our wider per-player views and the
    widescreen margin.

## 3. Alternatives, ranked

| # | Approach | Effort | Risk | Verdict |
|---|---|---|---|---|
| **A** | **Strength via `tmap = 4` + our own `priolist` / `.apl` tables + the OT penalty rule.** About 8 small instruction hooks. | Small (≈150-250 lines of C) | Low. Uses the game's own 5-on-3 machinery, which has shipped. Every on-ice change still goes through `SetPlList` / `forcepldata` / `check4bench`. | **Do this** |
| B | A but keep the stock `priolist` (G, LD, RD, C) and present it as "1 F + 2 D". | Smallest | Low | Not what was asked (no LW). Useful as a 10-minute smoke test of the `tmap` hooks alone. |
| C | **Blank the RW/RD line slots** (`tmlines` byte = 0) in RAM at game start. This is the community's proven "2on2/3on3 ROM" method (§3a), done on the team data with NOSE. | Smallest (1 hook + the Edit Lines fixes) | Medium. `tmap` stays 6, so the game's strength maths is wrong. **The first penalty costs nothing** (5 bodies = G, LD, RD-blank, C, LW), a second one drops LW, an expiring penalty puts an RW on until the faceoff, `getlinee` reads `tmflags` as the energy of the blank slots, and the in-game Edit Lines / Team Rosters screens crash without smozoma's fixes. | Fallback / prototype only. It also shows the engine copes with 4 bodies all game |
| C′ | The same, but with ghost or duplicate players in the RW/RD slots instead of blanks. | Medium | High. Duplicates break `TryAddPlayerToList`, and stats and energy go wrong. | No |
| D | Park RW/RD objects off-ice every frame (force `position = -1`, x off-rink) while `tmap` stays 6. | Medium | High. Fights `check4bench` / `setpersonel` every frame, the game believes it has 6 (power-play maths, faceoff rows, line energy), and it is a constant source of divergence between what the game thinks and what is on the ice. | No |
| E | Hook object spawning (`setplayer`) to refuse RW/RD. | Medium | High. Same desync of `tmap` versus reality as D, and `releasepl` would loop looking for free objects. | No |
| F | Rewrite `SetPlList` in C. | Medium | Medium. It duplicates a working routine and its sub/injury paths. A has the same effect through the table pointer. | Only if A's table hooks turn out impossible |

## 3a. Prior art (web search, 2026-11-23)

**Short version.** Nothing exists that does a 4v4 mode with penalty rules, a
launcher switch, or online play. But the NHL'94 community **has made reduced-
player Genesis ROMs (1on1, 2on2, 3on3, 4on4) since at least 2007**, all by
editing line data. No 4v4 or 3v3 mod for any later NHL 9x game turned up, and
neither did any SNES "fewer skaters" hack.

The forum (forum.nhl94.com) answered most fetches with HTTP 429, so the
details below come from search snippets and one full read of thread 11608.
Treat the hex values as theirs, not checked against our ROM **[U]**.

| What | Who / when | How it works | Online? | What we take from it |
|---|---|---|---|---|
| **"Fewer players on the ice (1on1 ROMs etc.)"**, an entry in the [Gens "Editing the Game" thread guide](https://forum.nhl94.com/index.php?/topic/8090-gens-editing-the-game-thread-guide-newbs-start-here/) | **wboy**, 2007 (the guide gives Oct 22 or May 2007) | Not readable (429). Probably the same line-data method as below. | Offline ROMs (emulator netplay at best) | It is the earliest community work on this. Credit wboy. |
| **2on2 / 3on3 ROMs** and [**"HOW TO: Prevent 2on2 roms from crashing in Edit Lines"**](https://forum.nhl94.com/index.php?/topic/11608-how-to-prevent-2on2-roms-from-crashing-in-edit-lines/) (How-To & Reference, from March 2010) | First 2on2 ROM by **clockwise**. Crash fix by **smozoma**. Bug narrowing by **jesusplaysnhl94**. | Use the NOSE editor to **clear the line positions you don't want on the ice**. The empty line slot means `SetPlList` puts nobody there (see below). The menus then crash, because an empty slot leaves the name and number print pointers stale. smozoma's ROM patches fix that: the position range, player count and position-name strings of Edit Lines and Team Rosters (e.g. `$8375`, `$837B`, `$879D`, `$8C76`, `$86DC-$86F4`, `$191D2`). The thread says the fix was written for 3on3 and needs transposing for 2on2. | Offline ROMs. The 2on2 community plays them on emulators. | (1) **Evidence that the engine and AI run a whole game with 2-4 bodies a side.** That lowers the risk of the "ghost object" loops in §2. (2) If we ever let players use the in-game Edit Lines / Team Rosters in 4v4, smozoma's analysis shows **why it would crash**: it crashes only with blank slots (approach C). Approach A keeps RW/RD in the line data, so those screens stay intact **[U]**. (3) The slot order G, LD, RD, LW, C, RW, X matches our `tmlines` reading. |
| [SNES NHL '94 ROM mapping project](https://forum.nhl94.com/index.php/topic/13150-snes-nhl94-rom-mapping-project/) | nhl94.com community | Data mapping only. It confirms the same per-line slot order on the SNES. Search snippets mention that people built 1on1, 3on3 and 4on4 SNES ROMs from it with an editor. | – | Only confirms the data model. |
| [5-player SNES hockey hack](https://retrohandhelds.gg/new-snes-hack-adds-5-player-mayhem-to-16-bit-hockey/) (via romhacking.net, Feb 2026) | Author not named in the article | It uses the multitap and **maps extra controllers to skaters on the ice**. This is closer to our position lock than to 4v4. The article does not say whether it changes the number of skaters. | No (hardware multitap) | Not 4v4. Worth a look later for position-lock UX ideas. |
| [StaticEck2026/NHL94Genesis-C-Port](https://github.com/StaticEck2026/NHL94Genesis-C-Port) | StaticEck | A native C port of the Genesis game with 2-4 machine rollback netplay, custom rosters and adjustable rule presets. The README mentions no reduced-player or 3v3/4v4 mode. | Yes (rollback) | No 4v4 to reuse. It is a peer project to watch, in case it adds one. |
| Adam Catalyst's yearly editions (e.g. [romhacking.net 8267](https://www.romhacking.net/hacks/8267/)), NHL'94 Fight Edition | Adam Catalyst; others | Roster and gameplay updates. They have no 3v3 or 4v4. | No | Nothing to reuse. |

**Searched with no relevant result:**

- general web search;
- romhacking.net;
- GitHub (our own NHL94Genesis and NHL94Recomp included: no 2on2/3on3/4on4 notes in either);
- Reddit and YouTube (no hits for 3-on-3 / 4-on-4 NHL '94);
- nothing further from smozoma beyond the 2on2/3on3 menu fix above (the thread implies a 3on3 ROM existed, but I found no separate page for it);
- nothing from kingraph, McMarkis/Mhopkinsinc, Drezz or chaos (their mapping, editor and roster work is the basis for the data, but none of it is a skater-count mod).

**Why the community method works, and why we still prefer approach A**
**[S]**. In `SetPlList` (`collide94.asm:2591-2600`), the line byte for a
skater position is copied into `PlList` as-is. A blank byte (0) is then skipped
both by the availability loop (`beq .next1`) and by `setpersonel`
(`beq .next`). So no object receives that position. The cost is that `tmap`
still says 6, so:

- a team's first penalty takes off the blank RD and changes nothing;
- the second penalty removes LW;
- `releasepl` returns a player as RW until the faceoff;
- `getlinee` averages a bogus energy for the blank slots.

Approach A makes `tmap` equal to the real number of bodies and keeps line data
and menus intact. That is what makes the game's own power-play, line-change
and penalty logic correct.

## 4. Hook list for approach A (ROM addresses read from the retail bytes [R])

A game hook runs **before** the instruction at its address. It can change
registers or RAM and return 0 to run that instruction, or return 1 to `rts`
out of the routine (`game_spec.h:259-264`, `nhl94_seats.c:876`). It cannot
skip one instruction. So each "replace an immediate" hook sits on the
**instruction after** the one that loads the immediate.

| Site | ROM | Instruction there | Hook does (only when 4v4 is on) |
|---|---|---|---|
| `restoreteams .r` | `$007806` | `moveq #50,d0` (after `move.w #6,tmap(a2)` at `$007800`) | `tmap(a2) = 4`. Write the mode marker and tables (below). |
| `SetPlList` | `$015842` | `tst.w tmgoalie(a2)` (after `movea.l #priolist,a0` at `$01583C`) | `a0 = PRIO4` |
| `releasepl` | `$012A06` | `tst.w tmgoalie(a2)` (after the movea at `$012A00`) | `a0 = PRIO4` |
| `getlinee` | `$012ECA` | `move.w tmap(a2),d4` (after the movea at `$012EC4`) | `a0 = PRIO4` (line energy and CPU line choice count the right players) |
| `puckfaceoff2 .t0` | `$00FA7A` | `adda.w d4,a1` (after `movea.l #.apl,a1` at `$00FA74`) | `a1 = APL4` |
| `InProgress_ap` entry | `$0123DE` | `moveq #6,d1` | **Replace** (return 1): for each roster slot with `tmpdst > 0`, `bclr #5` as the original does, then `tmap` for both teams by the §1.4 rule. Called once per team. Computing both each time is idempotent. |
| `PenGoalStuff` | `$01284E` | `cmpi.w #6,tmap(a1)` | If `boxed(a1) == 0`, clear PenBuf (32 words, what `ClearPenaltyBuffer` does). Then continue. The stock compare never matches in 4v4 unless 6, so there is no double action. |
| `GetLowestPen .endoflist` | `$012A50` | `cmpi.w #6,tmap(a3)` | If `boxed(a3) == 0`, return 1 (`rts` with d0 = the power-play time), the same as the stock "6 on ice" exit. **[M]** works (§10). |
| `puckfaceoff2` after `ResetBench` | `$00F9D8` | `movea.w #tmstruct,a2` (after `bsr.w ResetBench` at `$00F9D4`, `logic94.asm:6068-6069`) | Re-apply the §1.4 formula to both teams' `tmap` (the faceoff normalisation). |

Notes on the table:

- `releasepl` indexes `priolist[old tmap]`: in 4v4, 4 → RD and 5 → RW. With the
  goalie pulled it reads from `PRIO4 + 1`.
- `GetLowestPen`'s routine pushes nothing before `.endoflist`, so a mid-routine
  `rts` is safe **[S]**. Confirm that the generator honours return 1 at an
  interior site **[U]**.
- In `getlinee`, the `bra .next` / `dbf` loop reads entries `tmap-1 .. 0`
  (`scoreboard94.asm:335-356`).
- **Faceoff `d4`.** In the faceoff hook, `d4 = (6 − tmap) × 8`
  (`logic94.asm:6106-6113`), so APL4 has 3 rows for `tmap` 6/5/4.
- **Sub-word granularity.** `restoreteams` writes `tmap` again for team 2 at
  the same pc: the hook fires once per team (`a2` differs).
- **Dead code.** A byte-identical `.apl` copy at ROM `$00F2C0`
  (`logic94.asm:5559`) has no `movea`/`lea` reference [R]. Leave it alone.
- → §10: a 10th site was needed, `setpersonel`'s second pass (`$0157E4`).

### Guest-RAM tables (unused block, next to the seat block)

Put the tables in the unused work RAM after `NHL94_VSWAP` (`$FFDFA0..$FFDFB3`),
for example `$FFDFC0..$FFDFE7` (the stack never goes below `$FFFEC9`, as
measured for the seat block, `nhl94_ram.h:269-272`):

```
$FFDFC0  u16 FMT     0 = 6v6 (stock), 4 = 4v4. Written by the restoreteams hook
                     so the state hash differs between modes.
$FFDFC2  u8[8] PRIO4 0,1,4,3,2,5,6,$FF      ; G, LD, C, LW, RD, RW, X
                     goalie pulled (+1):    LD, C, LW, RD, RW, X
$FFDFCA  u8[24] APL4 position -> .ptab slot, one row per tmap 6/5/4:
                     tmap 6: 0,1,2,3,4,5,6,0   (stock row)
                     tmap 5: 0,1,2,3,4,0,6,0   (G, LD, RD, LW, C: everyone on his own spot)
                     tmap 4: 0,6,2,3,4,5,0,0   (LD -> slot 6 (0,-60) "centre point",
                                                LW -> slot 3, C -> 4; RD -> 2 for the
                                                pulled-goalie set LD, C, LW, RD)
```

`.ptab` slots (`logic94.asm:6285`): 0 (0,−250) G, 1 (−35,−50) LD,
2 (35,−50) RD, 3 (−50,−10) LW, 4 (0,−15) C, 5 (50,−10) RW, 6 (0,−60) X.
Slots above 2 skip the x pull toward the spot (`cmp.w #8,d4`). The slot choices
are a first guess for a play test **[U]**.

Every hook writes the table bytes before pointing a register at them. Writing
identical bytes is idempotent, and it means the tables are present after any
snapshot restore or reset with no separate init path.

## 5. Determinism and netplay

- **All simulation state stays in guest RAM:** `tmap`, the tables and the FMT
  marker. Snapshots and digests cover it.
- **The only host-side input is the mode flag.** It is constant for the
  session and identical on every peer because it comes from the host's sealed
  session image, the same way as `nhl94.seats=`. The hooks read guest RAM and
  that flag only. The 3-on-3 rule is a pure function of `tmpdst` / `tmboxlist`.
- **Selection:**
  - a new launcher option "Format: 6v6 (cartridge) / 4v4" in the Control mode
    feature (`nhl94_mode.c`), persisted as `format = 6v6|4v4` in
    `nhl94-control.ini`;
  - `--format 4v4` and `NHL94_FORMAT` for scripts, with the same precedence as
    seats.
  - Keep the format separate from the control mode: 4v4 with Classic control
    is valid (and the easiest first test).
- **Session image.** (→ §10: the format went into the seats VALUE instead,
  because the identity does not refuse an old build.) `nhl94_spec.c` `nhl94_netplay_config_image` writes
  `nhl94.fmt=4` **before** the seats key, then calls
  `nhl94_seats_config_image`. That function measures the remaining space
  (`l`) and already falls back to the compact form. Budget, `game[48]`
  (`genesis_netplay.h:162`): `nhl94.fmt=4;` is 12 bytes, and 12 compact seats
  is `nhl94.seats=*` + 12 = 25 bytes, 37 in total.
  - Adoption: `nhl94_netplay_config_adopt` hands the line to both parsers.
  - A missing key means 6v6 (old images).
  - An unknown value is refused (return 1), as with the seats.
- **Old builds [U].** The image text is folded into the match identity, so
  peers whose images differ refuse the match (`genesis_netplay.h:150-157`).
  But an **old build** would adopt `nhl94.fmt=4`, ignore it, and play 6v6.
  - Confirm that the identity's content fingerprint also covers the build
    (`genesis_host_lobby.c:145-187` publishes version + content sha), so an
    old client cannot join a new host.
  - If it does not, carry the format inside the seats value, which old builds'
    `parse_seats` refuses.
- **Mid-session changes.** The mode is applied at `restoreteams`, the
  game-start reset. Changing it in the launcher mid-game must not take effect
  until the next game. This holds automatically if the flag is only latched in
  that hook (into FMT) and **every other hook reads FMT from guest RAM, not
  the host flag**. Recommended: rollback then never depends on host state at
  all.

## 6. Position lock, presets, labels, tags, views

- **Seat roles.** In 4v4, RD and RW are not on the ice at even strength. A seat
  with either role controls nobody (`seat_target`, `nhl94_seats.c:383`,
  returns -1, counted as vacant). The OT rule can put an AI RD or RW on for a
  power play. If a seat owned that role it would suddenly control a player for
  the duration.
  - Recommendation: in 4v4 the **launcher offers only G / LD / LW / C**.
  - `parse_seats` + format refuse RD/RW seats. The adopt path refuses a host
    image that combines `fmt=4` with RD/RW seats.
  - Power-play extra skaters are always AI.
- **Presets.** Add "Full 4v4 (8 seats)", centres first, alternating teams,
  goalies last (same reasoning as `k_preset_6v6`, `nhl94_mode.c:59-66`):
  `home:C, away:C, home:LW, away:LW, home:LD, away:LD, home:G, away:G`.
  - Choosing it also sets the format to 4v4.
  - Choosing 6v6 sets it back.
  - 1v1 works in both formats.
  - `status()` text: "Position lock: full 4v4, 8 seats".
- **Waiting room.** `nhl94_seats_label` (`nhl94_seats.c:271`) needs no change:
  labels come from the seat list. Optionally prefix the room or format name
  ("4v4 · Home LW").
- **Name tags** (`nhl94_tags.c`) follow the seats' objects. No change.
- **Views.**
  - `nhl94_view.c:161-163` defaults (`GENESIS_VIEWS=6`) include `home:RD` and
    `away:RD`, which in 4v4 fall back to the puck. Use LD instead when FMT = 4.
  - A per-seat own view (`nhl94_view.c:293-296`) takes the seat's role, so it
    is fine once RD/RW seats are refused.
- **Pulled goalie:** unchanged from 6v6. The G seat controls nobody while
  pulled (`PLAYER_LOCK.md` "Known limits").
- **Line changes.** Fewer roles on ice means a seat's role changes less often.
  The existing release-while-changing logic (`seat_target` pending test) is
  unchanged.

## 7. Implementation steps

Each step builds in its own build directory on `~` and runs the
existing checks before the next step.

1. **Format setting, no behaviour.** Add `nhl94_format.c/.h`:
   - `format` from env / CLI / launcher;
   - `nhl94_format_config_image` / `_adopt`;
   - wire it into `nhl94_spec.c` (image order: format first).

   Unit-test the image round trip and the 48-byte budget with 12 compact
   seats, in the style of `tools/seats/test_tagnames.c`.
2. **RAM map.** Add `NHL94_FMT`, `NHL94_PRIO4` and `NHL94_APL4` to
   `nhl94_ram.h`. Add `TM_PDST` / `TM_BOXLIST` / `PENBUF` addresses where
   missing (`PenBuf` from `ram94.asm`).
3. **Smoke test (approach B).** Hook `restoreteams` (`$007806`) and
   `InProgress_ap` (`$0123DE`) only, with Penalties off. Expect G, LD, RD, C on
   the ice for both teams, the stock 5-on-3 set. This proves the `tmap` path.
4. **Our set.**
   - Add the `priolist` hooks (`$015842`, `$012A06`, `$012ECA`) and the
     faceoff hook (`$00FA7A`).
   - Expect G, LD, C, LW, and faceoff spots per APL4.
   - Add the 4 sites to `game.toml` as `[[widescreen_site]] kind="game_hook"`
     with a comment block like the player-lock one.
   - Dispatch from `nhl94_instruction_hook` (`nhl94_spec.c:51`) before
     seats/view.
5. **Penalties (§1.4).**
   - Full `InProgress_ap` replacement, the `PenGoalStuff` (`$01284E`) and
     `GetLowestPen` (`$012A50`) hooks, and the faceoff normalisation hook
     (`$00F9D8`, right after `bsr ResetBench`).
   - Scripted penalty run: `tools/seats/long_penalties.input` with
     `NHL94_FORMAT=4v4`. Check:
     - `tmap` on the power play is 5v4 and 6v4;
     - the power-play banner and time;
     - release mid-play gives 5v5, then 4v4 at the next faceoff;
     - a power-play goal frees the boxed player.
6. **Position lock integration.**
   - Seat validation for RD/RW.
   - The "Full 4v4" preset.
   - The launcher's Format option, with seat choices filtered.
   - Default views.
   - `PLAYER_LOCK.md` "Known limits" updated.
7. **Polish (optional, after a play test).** Centre the lone D and mirror the
   lone winger toward the puck side, with hooks in `assdefd` / `assdefo` /
   `asswingd` / `asswingo` after x is chosen. AI tuning is a play-test
   question.
8. **Docs.** Add this mode to `PLAYER_LOCK.md` and `VIEW_RESEARCH.md`
   (parked-object visibility). Move this plan to "implemented" with the
   measured results.

## 8. Test plan

All tests run headless with the ROM by absolute path, never copied
(`tools/seats/rb_probe_seats.sh` already does this).

- **No regression in 6v6.** With the format unset, `tools/seats/run_checks.sh`
  must give **identical** state hashes to the previous build. That covers the
  Demo A/B, lock2 / lock4 / callpass, rb_probe, and the 2-process netplay
  timeline. Every hook must be a no-op while FMT ≠ 4.
- **Determinism in 4v4.**
  - Run each script twice per configuration and compare `state_fnv1a64`:
    `lock2.input`, `lock4.input` (with seats changed to C/LW/LD/G),
    `long_penalties.input`, and a new `four_on_four.input`. That script is a
    Head to Head with 8 seats, the Full 4v4 preset, penalties on and line
    changes on, for about 20k frames.
  - Run once with `NHL94_VIEW=off` and once with views on: the hash must be
    equal.
- **Rollback.** `rb_probe_seats.sh … 30:8 four_on_four.input -- --seats
  <8-seat list> --format 4v4` must report no divergence and no asymmetric
  restore.
- **Netplay.**
  - A 2-process localhost rollback run with the host on 4v4 and the guest on
    6v6 locally: the guest must adopt the format and the TIMELINE hashes must
    match.
  - A run with a guest whose image is forced to differ must be refused.
  - A 12-seat hub run (`tools/seats/run_checks12.sh` style) with the 8-seat
    preset.
- **Game-state asserts (trace env, host-side logging only).** Every N frames,
  log per team:
  - `tmap`;
  - the on-ice positions (they must equal the first `tmap` of PRIO4, except
    between a mid-play release and the next faceoff);
  - the number of objects with `position >= 0`;
  - `PBnum`.

  Flag any frame where an RW/RD is on ice without a power play, or where on-ice
  objects ≠ `tmap` after a faceoff settles.
- **Penalty cases** (scripted or forced through a debug env that queues a
  penalty, test only):
  - a single minor;
  - two minors on one team;
  - one minor each;
  - coincidental majors;
  - expiry mid-play;
  - a power-play goal;
  - a delayed penalty plus a goal by the non-offending team (PenBuf cleared);
  - a pulled goalie at 4v4 and at 5v4.
- **Visual check** (screenshots via `tools/seats/montage.py`):
  - faceoff alignment at centre ice, in both zones, and at both end-zone dots
    on each side;
  - no ghost sprites at x = −240 in the per-player views and widescreen.

### Needs a real play test

- Does 4v4 feel right? The lone LD hugs the left, the lone LW plays only the
  left wing, and the right side is left to C and `assnearest`.
- Faceoff spots for the 4-body row (LD at the centre point?).
- CPU line changes. Even-strength lines still roll lines 0-2, and the RW/RD of
  each line sit unused. Energy and fatigue use only the 4 placed bodies
  (`getlinee` with PRIO4).
- Whether power plays (5v4 / 6v4 bodies) are too strong in a game this
  open.
- Goalie pulls (the CPU pulls late in the game, giving LD, C, LW, RD).

## 9. Open questions

1. **Penalties.** Use the 3-on-3 OT rule (recommended), keep "penalties off
   only" for 4v4, or let a penalty reduce to G + 2 skaters? The last one needs
   a 4th `.apl` row and goes below the game's tested floor. Not recommended.
2. **Should RD/RW seats be refused in 4v4,** or allowed as "power-play
   specialist" seats that are vacant at even strength?
3. **Lone-D / lone-W AI centring (phase 7):** in scope or not?
4. **Format option placement:** in the Control mode feature (simplest), or its
   own launcher feature "Game format"?
5. **Does the generator support a `return 1` (rts) hook at an interior,
   non-entry address** (the `GetLowestPen` site)? If not, replace
   `GetLowestPen` at its entry in C (about 25 lines).
6. **Are the parked objects visible anywhere in our views?** Check
   `nhl94_view.c` and the widescreen margin.
7. **Does an old build refuse a 4v4 host?** See §5, "Old builds".

## 10. Implementation and measured results (2026-11-24)

Build: `~/nhl94-online-build/fourvfour` (Release, netplay + ICE,
mods UI). ROM passed by absolute path, never copied. `dispatch_misses.toml`
and `interior_label_misses.toml` stayed empty after every run.

### 10.1 Decisions (§9)

1. Penalties: the 3-on-3 OT rule from phase 1, with one extra cap (§10.2).
2. RD/RW seats are refused in 4v4 (launcher offers G/LD/LW/C only; `NHL94_SEATS`
   / `--seats` with RD/RW under 4v4 → Classic, logged as malformed; a host
   image combining 4v4 with RD/RW → the guest refuses it).
3. Phase 7 (AI centring) skipped.
4. Format lives in the Control mode feature: option **Format** = 6v6 | 4v4,
   persisted as `format = 6v6|4v4` in `nhl94-control.ini`; preset **Full 4v4
   (8 seats)** = home:C, away:C, home:LW, away:LW, home:LD, away:LD, home:G,
   away:G, and choosing it sets 4v4 (Full 6v6 sets 6v6). `--format` and
   `NHL94_FORMAT`, precedence env > CLI > launcher, the host's online.
5. **The generator honours return 1 at an interior site.** It emits
   `if (genesis_game_instruction_hook(pc)) return;` at the site, inside the
   routine's own C function (`func_012A2E` = GetLowestPen for `$012A50`), and
   a caller pops its own return slot after the native call, so a `return`
   with nothing pushed is exactly `rts` (the tier-3 interpreter pops the
   return address itself). **[M]** `lowestpen_rts` = 99 in the penalty run,
   rollback-probed without divergence. No C replacement of GetLowestPen needed.
6. Parked objects (x = −240, y = 0) are not visible: the widest view (2x zoom,
   384×448 = the whole rink map) ends at x ≈ −192, the boards at ±136.
   Screenshots below show no ghost sprite (1x views, 2x zoom, widescreen on).
7. **An old build would NOT be refused** by the identity: the build
   fingerprint is compared but explicitly "not refused on its own"
   (`rnet_rb_driver.c`, "peer BUILD differs ... The boot digest is the
   arbiter"), and the content fingerprint folds the adopted image text, which
   is identical on both sides. The boot digest would match too (FMT is latched
   only at game start). So, as planned, **the format rides in the seats
   value**: `nhl94.seats=4v4` (Classic), `nhl94.seats=4v4,<list>`, or compact
   `*4<letters>`. An old build's `parse_seats` rejects all three and refuses
   the host. No `nhl94.fmt=` key; a missing key or a plain list = 6v6.
   Budget: the 12-letter compact 6v6 list is 25 bytes; Full 4v4 is
   `nhl94.seats=*4ekdjbhag` = 22 bytes (`tools/seats/test_format_image.c`).

### 10.2 Deviations from §4

- **APL4 tmap-5 row = the stock row** (0,1,5,3,4,2,0,0): the 5-body set G, LD,
  C, LW, RD is the same set as the stock 5, so its proven placement is kept.
  Rows 6 and 4 as planned (4: LD on the centre point, slot 6).
- **Cap** in the rule: `tmap(T) = clamp(4 + clamp(bo − bt, 0, 2), 4, 6 − min(bt, 2))`.
  A team with a boxed player keeps a free object for the player releasepl
  brings back (releasepl scans for the first object with position < 0 and
  would run past the team's six otherwise).
- **10th site, `setpersonel` second pass `$0157E4` (4v4 only).** Found by the
  trace asserts: with two forwards boxed, a faceoff put **3** home bodies on
  for `tmap = 4`. The stock free-object scan (`dbmi d3` / `tst.w position`
  / `dbpl d3`) underflows `d3` when the team's last object already has a
  request but no position yet, walks on into the **other team's** objects,
  and writes the substitute there (then cleared by that team's own
  setpersonel: the player is lost). 6v6 does not reach it (there is always an
  unwanted on-ice object to swap). The hook does the pass in C with the same
  choice (first unrequested on-ice object, else the last unrequested one)
  within the team, then the stock pass finds `PlList` empty. Also used by
  mid-play line changes (check4bench → setpersonel).
- **Test-only penalty injection** `NHL94_FORMAT_TEST_PENALTY=T:team:role|T:puck,...`
  (changes the simulation; done-bits in guest RAM `$FFDFE2`): no scripted run
  produced natural penalties, so minors are queued as AddPenalty2 does.
- **Host-side trace** `NHL94_FORMAT_TRACE=<n>`: tmap, PBnum and on-ice
  positions per team, asserts (RD/RW on a team at 4 with its goalie in; on-ice
  ≠ tmap during a faceoff), a strength histogram and per-hook visit counts.

### 10.3 Hooks and verification

| Site | Hook | Verified |
|---|---|---|
| `$007806` restoreteams .r | FMT latch, tmap = 4 | [M] every 4v4 game starts G, LD, C, LW both teams; 6v6 FMT stays 0 |
| `$015842` SetPlList | a0 = PRIO4 | [M] on-ice set G,LD,C,LW / +RD / +RW at tmap 4/5/6 |
| `$012A06` releasepl | a0 = PRIO4 | [M] mid-play release adds RD (4→5) / RW (5→6) |
| `$012ECA` getlinee | a0 = PRIO4 | [M] 36 visits in the CPU-line-change run (Demo, line changes on) |
| `$00FA7A` puckfaceoff2 .t0 | a1 = APL4 | [M] screenshots: centre and both end zones, 4v4 / 5v4 / 4v5 |
| `$0123DE` InProgress_ap | replaced: OT rule | [M] 5v4, 6v4, 4v4 with one each, 3+3 boxed = 4v4, 4 boxed = 4v6 |
| `$01284E` PenGoalStuff | PenBuf cleared when nobody boxed | [M] visited on every goal; clear path taken 2× (pen4) |
| `$012A50` GetLowestPen | rts when PP team has nobody boxed | [M] 99 rts taken; the power-play banner shows the time ("MTL Pwr Play 2:00") |
| `$00F9D8` puckfaceoff2 | rule re-applied | [M] 5v5 / 6v6 / 6v5 after mid-play releases back to the rule at the faceoff |
| `$0157E4` setpersonel pass 2 | C pass within the team | [M] fixes the 3-body faceoff; 0 faceoff asserts in all runs since |

Not exercised: coincidental majors, a power-play goal that frees a boxed
player (two PenGoalStuff visits with boxed players happened, but the trace
does not show which side scored), a delayed penalty with a goal by the
non-offending team, a goalie pulled at 5v4 (a pulled goalie at 4v4 was seen:
LD, C, LW, RD as designed).

### 10.4 Test results

**6v6 no-regression (format unset), mandatory.** `tools/seats/run_checks.sh`
state hashes (non-netplay lines) = `tools/upstream-sync/baseline/checks1-4.hashes`
exactly (3× 3EF834BA226E6E28, 1× 560C3425A02A9A91, 2× 80CE87C28143ADBF,
3× A94E00C87F121117, 2× FAA72A99530568E6); rb_probe 3× PASS; netplay
TIMELINE identical (2C4B8FF1E23D0B26, F86F74D7CB8F65E8). `run_checks12.sh` =
`checks6-12.hashes` exactly (3× 3EBD5E5F3A64349D, 1× AD65C10AEF8F93A7,
2× FBD494B2680E850C); rb_probe 2× PASS. Every hook's visit counter is 0 in
6v6 runs.

**4v4** (`tools/seats/run_checks4v4.sh`; same hash twice in every row):

| Run | Frames | state_fnv1a64 |
|---|---|---|
| `four_on_four.input`, 8 seats Full 4v4 (a, b, `NHL94_VIEW=off`, 4 views) | 20000 | 45B46C2EC1015982 (all four) |
| `long_penalties.input`, Classic | 30000 | F93F170296FE4667 |
| `demo_penalties.input` + 8 injected minors (`pentest4`) | 18000 | 09BB33481726BEC7 |
| same, 6v6 control | 18000 | 74282DC9A5319467 |
| `lock2.input` home:C,away:G | 4800 | FCAE20883C58762B |
| `lock4.input` home:C,home:LW,away:C,away:G | 4200 | C13F283F54E9D86C |

- Trace asserts: 0 RD/RW on a team at 4 with its goalie in, 0 faceoffs with
  on-ice ≠ tmap, in every run. Strength histogram of the penalty run
  (home v away, traced frames): 4v4 287, 4v5 85, 4v6 56, 5v4 81, 6v4 15.
- rb_probe 30:8 with `--format 4v4`: `four_on_four` 199 passes, `pentest4`
  (injected penalties) 599 passes, `long_penalties` 399 passes; 0
  divergences, 0 asymmetric restores, 0 dispatch misses.
- **Netplay adoption** (`tools/seats/lobby_format_test.sh`, two processes,
  each with its own exe copy, through a local `recomp-net-server` on
  127.0.0.1 — a LAN room carries no match caps, so a guest adopts nothing
  there): host 4v4 / guest 6v6 → guest logs `control mode Position lock
  home:C,away:C (source: host's session config), format 4v4`, both trace
  fmt=4 with G,LD,C,LW, 66/66 TIMELINE ticks identical, 0 desyncs; host 6v6
  / guest 4v4 → guest plays 6v6, 50/50 identical. Waiting-room labels
  "Home C" / "Away C" from the 4v4 image.
- **Refusal**: a 2-process LAN run with host 4v4 vs guest 6v6 (no lobby, no
  adoption) is refused before tick 0 (`refusal=mod_set_mismatch` on both).
- Image unit test (`tools/seats/test_format_image.c`): images, the 48-byte
  budget, adopt + label round trip, RD/RW refusals: all OK.
- Screenshots (`~/nhl94-online-build/fourvfour-shots/`):
  `faceoffs_main.png`, `views.png`, `a/main_t1650.png` (centre 4v4),
  `a/main_t3000.png` (5v4 with the banner), `b/view0_t1800.png` (2x zoom, 8
  tags). Faceoffs line up (C on the dot, LW on the wing spot, LD on the
  centre point, the PP team's RD on the far wing), no ghost sprites.

### 10.5 Open issues

- **Play test**: lone LD / LW hug the left side (no phase 7); faceoff spots
  (LD on the centre point); power-play strength.
- **Mid-play expiries** give 5v5 or 6v6 bodies until the next whistle (by
  design, §1.4); with two expiring that can be a long 6v6 stretch.
- **Duplicate positions mid-play**: a team showed RW twice (no LW) for ~400
  ticks after a CPU line change; the 6v6 control shows the same cartridge
  artifact (LD twice, no RD, ~750 ticks). Not introduced by 4v4; not chased.
- `GENESIS_VIEWS=6`'s defaults (`nhl94_view.c`, not part of this change) name
  home:RD / away:RD, which follow the puck in 4v4: use LD when guest
  `FMT` (`$FFDFC0`, u16) == 4.
- The room list does not show the format; the waiting-room labels do not
  prefix it either.

## 11. Penalty shots instead of minors (2026-11-25)

**Rule.** In 4v4, a penalty called in a hit (check, slash, rough, cross-check,
hook, hold, trip, interference) is a **penalty shot for the fouled team**.
Nobody goes to the box, so there is no power play. This replaces the 3-on-3
OT rule (§1.4) for those calls. The OT rule still applies when no shot can be
set up (below).

**How [S/R].** Every one of those calls goes through `PenShotChk`
(`penalty94.asm:143`, ROM `$01211A`) right before `AddPenalty`. On a
breakaway (`pf3break` on the fouled player `a2`), the cartridge swaps the
penalty for its penalty-shot form: minutes `$FF`, nobody boxed. Then
`AddPenalty` → `puckpenshot` → `SetupPenaltyShot` runs the shot. The new
`game_hook` at `$01211A` replaces the routine in 4v4 (`nhl94_format.c`
`penshotchk`, returns with `rts`). It is the same logic without the
breakaway test, and `getBAplayerInfo` (`$012174`) is done in C (it writes
`BA_Team`, `BA_Sktr_SCnum`, `BA_Goalie_SCnum` and the `*_Offset` words at
`$FFD404-$FFD40E`, `pspenalty` at `$FFD410`, and bit 3 of the high byte of
`BA_PS_flags`). 6v6 runs the cartridge's routine unchanged.

**Cartridge table bug [R].** `PenShotPenalties` (`$01913A`) is shifted one
word: Charging gets Slashing's shot, Slashing gets Roughing's, ...,
Tripping gets Interference's, and Interference and Holding get none.
4v4 uses the intended pairs in C:

| Call | Shot |
|---|---|
| Charging `$16` | `$38` |
| Slashing `$18` | `$3C` |
| Roughing `$1A` / `$12` | `$36` |
| Cross-checking `$1C` | `$3A` |
| Hooking `$1E` | `$2E` |
| Tripping `$20` | `$30` |
| Interference `$22` | `$34` |
| Holding `$24` | `$2E` (Hooking's; Holding has no shot form) |

6v6 keeps the cartridge's table.

**Still a normal penalty (OT rule):**

- penalties off;
- a penalty shot already pending (`BA_PS_flags` bit 3);
- the defending goalie is pulled.

**Who shoots:** the fouled player. If that player is the goalie, his team's
C shoots, else his LW. Fights and other calls never go through
`PenShotChk`.

**Trace assert.** While a shot is being set up, `puckpenshot` benches the
shooter's goalie and every other skater, and they skate off in any position
until the next faceoff. So the RD/RW-at-even-strength counter now skips a
team with no goalie on the ice while the clock is stopped.

**Measured [M].** All-AI Demo game with penalties on
(`demo_penalties.input`, 150000 ticks, `NHL94_FORMAT=4v4`):

- 8 calls, 8 penalty shots: 3 Holding, 3 Cross-checking, 1 Slashing,
  1 Hooking. The call is delayed until the offending team touches the puck,
  as stock.
- State hash `823CCEE9515C3B5F`, identical in three runs.
- 0 RD/RW at even strength, 0 faceoffs with on-ice ≠ tmap.
- The test-only `NHL94_FORMAT_TEST_PENALTY` writes `PenBuf` directly, so
  those injected calls stay box minors (they exercise the OT rule).

- Batteries: `checks1-4` and `checks6-12` hashes match the baseline (6v6
  is unchanged). In `checks4v4`, only `pentest4` changed (`3F08743E6FCB2AD2`,
  same in both runs): 2 natural calls in that game are now penalty shots.
  `rb_probe` passes, `pentest4` included. Baseline
  `tools/upstream-sync/baseline/checks4v4.hashes` refreshed.

**Open:** play test (shooter choice, how often shots happen, the stock
penalty-shot UI with a defenceman as the shooter).
