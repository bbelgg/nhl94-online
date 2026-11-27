# Zooming out the position-lock view

## Status (2026-11-24): implemented (A, B1, B2, B3)

Built and checked in `~/nhl94-online-build/wider-view/`.
Screenshots and an index are in `wider-view/shots/`. The sections after this
one are the original research and plan, kept as written. Where the
implementation differs from the plan, this section says so.

| Stage | What shipped | Where |
|---|---|---|
| **A, framing** | The own player sits 0.20 H below the centre (goalies 0.33 H). The camera leans toward the puck by 0.45 (goalies 0.30, only up to the own blue line) and leads by Yvel/256 (±40 px). It keeps the player in a band 18-90 % from the top. Dead zone 8 px (x 24 px), follow 0.20 per frame, snap on a flip or a jump past 160 px. The clamps narrow with the zoom; `Hpos` is 0 at 1.5x and wider. | `game/nhl94_view.c` `frame_camera` |
| **B1, columns** | Engine: `GameSpec.view_geometry` (appended; NULL = upstream) is asked before every view sub-tick. While speculating, `widescreen_update_for_frame` arms the VDP margin (`ws_extra`) with no 16:9 canvas and `g_ws_margin` = 0. `mv_host_begin` / `end` save and restore the margin. The window grid sizes its cells from the images (other shapes are fitted). Game: `find3d` ($016920) is replaced in zoomed view ticks with the same projection and a wider cull. Plane A columns 32-63 are blanked in the clone. The width is capped at the 48-column map (384 px, reached at 1.5x). | engine `multiview.c`, `main.c`, `game_spec.h`; game `nhl94_view.c`, `game.toml` |
| **B2, rows** | Engine: `gvdp_set_virtual_rows` (present-time parameter, outside the VDP struct). The frame loop renders the rows above the screen at line 0 and the rows below after the last active row, before the vblank DMA. Plane A and the window are not drawn on those rows. Sprite X/Y in $300-$3FF are read as negative. Guest sprite status is saved and restored around them. `gvdp_set_virtual_hud_anchor(split)` keeps plane A's bottom band (the clock and score boxes) at the picture's bottom corners; plane A above the band stays at the native position, so the faceoff close-up box keeps lining up with its sprites. Game: a taller clone switches to 64x64 planes and moves Satt from $DC00 to $F000 (VSPRITES + register 5). It fills plane B with every map row the window shows (map row m goes to plane row (m+32)&63) and skips `checksso`'s arrows ($016480). | engine `genesis_vdp.c/h`, `genesis_machine.c`; game `nhl94_view.c`, `game.toml` |
| **B3, presentation** | A launcher option, **Mods > Control mode > View zoom** (1x / 1.25x / 1.5x / 2x, default 1.5x; display only; saved in `nhl94-control.ini`, not in the session image). Keyboard **F** or controller **R3** (without Back) cycles it in game. `NHL94_VIEW_ZOOM` overrides it. Spectators and test view lists default to 1x. | `game/nhl94_mode.c`, `nhl94_view.c` |

**Not done:**

- **Name tags at window resolution.** The tags are drawn into the view image,
  in picture pixels (see below), not at window resolution. No separate
  marker ring was added; the game's own star and label stay.
- **Integer or sharp scaling.** The grid still scales its logical size with
  nearest neighbour, as before.
- **More than 64 sprites** was not needed: no dropped sprites were seen in
  the screenshots.

**Measured cost on this Mac** (one view, `multiview: summary`, quiet machine,
3600 ticks):

| Zoom | mean | p99 |
|---|---|---|
| 1x | 1.36 ms | 2.1 ms |
| 1.5x | 1.56 ms | 2.6 ms |
| 2x | 1.76 ms | 3.0 ms |

Six views in a CPU game, at 1x or 1.5x: mean 8.4 ms, p99 13-14 ms.

**Determinism** (zoom 1.5x is the position-lock default, so the battery
runs it):

- `tools/seats/run_checks.sh` and `run_checks12.sh`: the state hashes equal
  `tools/upstream-sync/baseline/checks1-4.hashes` and `checks6-12.hashes`.
- rb_probe: 5 runs PASS, 0 divergences.
- Localhost rollback TIMELINE digests: identical.
- An extra rb_probe at 2x: PASS (99 passes).
- A 2-process rollback match with one client at 1x and the other at 2x:
  66/66 TIMELINE digests identical.
- `lock2`, 3600 frames: the same `state_fnv1a64` at every zoom level and
  with framing on and off (6C1958BFACC51194).
- No dispatch misses.
- The Windows cross build (`wider-view-win64`) compiles with no warnings in
  the changed files.

**Known issues:**

- 2x was first 512x448, and the stands art past the 384-px map repeated
  at the sides (two benches). It is now capped at the map width: 2x is
  384x448, the same width as 1.5x with twice the ice ahead and behind.
- Fixed (zoompolish): the faceoff close-up box is now centred
  horizontally in a zoomed picture. Vertically, its centre keeps the 1x
  fraction of the picture height (the upper third). The engine call
  `gvdp_set_virtual_plane_a_box(x0,y0,w,h,dx,dy)` (HUD-anchor mode only)
  draws the plane A rectangle shifted and blanks its old place. The game
  sets it from `nhl94_view_geometry` while `dffaceoff` is set. The rectangle
  is the print box `(fodropx-46)/8, (fodropy-68)/8`, 12x13 cells. At
  setvideo's `bset #dfok` hook, the clone's Satt entries that `checkfo`
  wrote (the first N; N is counted the way checkfo counts) get the same
  shift. At 1x it is never armed (output is byte-identical). Other plane A
  pop-ups (the referee "Period Over" box) still sit at the native position.
- Name tags in zoomed views are drawn in picture pixels (scale 1), so they
  shrink with the skaters and sit just above the sprite. (zoompolish first
  scaled them by height / 224 to keep their 1x window size; that was too
  distracting zoomed out and was reverted to scale 1 on 2026-11-24.)

---

Research and plan (2026-11-23), kept as written:

**The problem.** In position lock, each player's view follows their own
skater and is turned so their team attacks UP the screen
(`game/nhl94_view.c`). That view shows too little ice.

**The goal** (revised 2026-11-23): a **zoom-out**, that is, more ice in
every direction (both sides, ahead and behind):

- **1.5x** today's area in each axis (2.25x the area);
- what **2x** would take;
- the result scaled down to fit the window.

**Tags used in this doc:**

- **FROM SOURCE**: read from the disassembly
  (`NHL94Genesis/src/engine/*.asm`), the engine, or our own code.
- **MEASURED**: from existing logs in `~/nhl94-online-build/`.
- **ESTIMATE**: not measured.

No new runs were made for this research.

---

## 0. Recommendation in brief

1. **Camera framing first (step A, small).** Lower anchor, lean toward the
   puck, velocity lead, dead zone. It needs no new pixels. With zoom on, it
   keeps doing the vertical framing inside the bigger window (§6).
2. **The zoom-out itself: (b), a larger virtual raster rendered in the view
   clone, in one pass.** It is built in three stages, and each stage ships
   on its own:
   - **B1, horizontal.** A per-view engine margin up to 128 px per side.
     This mechanism exists already (`ws_extra`); a per-view seam is needed.
     Plus a `find3d` cull hook, so players are not hidden near the old
     edges (the NHL '98 / GPGX Wide failure, §11).
     At 1.5x (384 px) this shows **the full rink width** with `Hpos`
     pinned to 0.
   - **B2, vertical.** Extra virtual rows above and below the 224 lines.
     Plane B is read from a host-side copy of the whole rink map, not the
     32-row ring. Sprite Y is read with more bits.
   - **B3, presentation.** Integer or sharp scaling to the window, HUD and
     name tags at window resolution, and a zoom picker (launcher setting +
     a hotkey or R3 cycle).
3. **Fallback: (c), the hybrid.** B1 plus **two stitched vertical passes**
   instead of B2. Use it only if the engine raster change (B2) is not
   wanted. It costs about 2x per view and adds seam handling.
4. **(a), a mosaic of 4 to 6 camera passes per view tick**, needs no engine
   raster work. It is rejected as the product: 4-6x cost, seams in both
   axes, HUD and arrow duplication. It is only useful as a quick throwaway
   prototype to judge readability.

**Sizes** (H32 pixels):

| Zoom | Size | Notes |
|---|---|---|
| 1x | 256x224 | today |
| 1.5x | 384x336 | full rink width |
| 2x | 512x448 | the outer 64 px on each side are beyond the rink art (backdrop) |

All of it is presentation only. Nothing reaches the authoritative
simulation, the session caps or the sealed session image (§8).

## 1. Facts that limit every option

| Fact | Value | Evidence |
|---|---|---|
| In-game screen mode | **H32 x V28 = 256x224**. `setupice` sets `df32c`; register 12 stays `$8C00`; register 1 is `$64`, so V30 is off. | `setup94.asm:12`, `middle94.asm:730-737` |
| **Sprite limits in H32** | **64 sprites total, 16 per line, and a 256-px dot budget per line.** The 20/80 figures belong to H40 (320-wide), which NHL does not use in game. In the engine, `max_sprites` is 64, `max_per_line` is 16, and `pixels_budget` = output width (`genesis_vdp.c:695-837`). `gvdp_set_unlimited_sprites` lifts the per-line and dot limits, **but not the 64 total**. | `genesis_vdp.c` |
| Game sprite table | `Satt` is 360 words = **90 entries** in RAM (`ram94.asm:611`). The game can build more than 64, but the VDP walks only 64 links. | FROM SOURCE |
| Plane size and VRAM | 64x32 cells (512x256 px). Plane B (rink) at `$C000`, Satt at `$DC00`, plane A (HUD) at `$E000`, window at `$F000` (off), hscroll at `$FC00`. | `setup94.asm:13-20`, `middle94.asm:738` |
| Rink map | **48x113 cells = 384x904 px**, plus a reversed copy (`$5605A`, `$B5180`). It is a static ROM map. | `VIEW_RESEARCH.md` §6 |
| Plane B streaming | A 32-row ring (256 px) selected by `Vpos>>3`, so only **32 px** beyond 224 lines. The 48 map columns sit at plane columns 0-47. `Hscroll = -64 - Hpos`. | `video94.asm:133-195` |
| Horizontal extent of the art | The whole map is 384 px wide: boards at x ±136 plus 56 px of glass and crowd art each side. **Anything wider than 384 is backdrop.** At `Hpos = 0` with 64 px margins, the view covers exactly plane px 0..383. | derived |
| Plane A (HUD) | Not scrolled; `SetScroll2` writes only plane B. The clock is at row 24, column 3 (bottom-left). The ref overlay is at the top. | `video94.asm:90-95`, `204-215`, `566-575` |
| Sprite projection and cull (`find3d`, `$16920`) | sx = dx + 256, sy = 240 - (dy + 1.5z) (SAT coordinates). Drawn only if **\|dx\| <= 144 and \|dy+1.5z\| <= 144**: 16 px past the side edges and 32 px past the top and bottom. | `video94.asm:987-1016` |
| How the engine reads sprite positions | Y is read with 10 bits (`& 0x3FF`, then -128), and X with 10 bits in widescreen. So lines -128 to about +895 and columns -128 to about +895 are addressable once the game writes those values. | `genesis_vdp.c:714, 734` |
| Engine output limits | `GVDP_MAX_WIDTH` 512, so H32 can take 128 px margin per side. `GVDP_MAX_HEIGHT` 240 (V30). `MAX_SCREEN_HEIGHT` 480 (framebuffer). | `genesis_vdp.h:32-33`, `main.c:284` |
| Camera clamps | Hpos -60..60 and Vpos -200..256, so the camera reaches world y from -312 to +368 (680 px). | `VIEW_RESEARCH.md` §3 |
| Cost of one view (2 ticks per view) | mean **1.0-1.9 ms**, p99 1.7-4.5 ms per presented tick. 6 views: mean 5.8-7.8 ms, p99 9-11 ms. | `multiview: summary` log lines (MEASURED) |
| NHL widescreen today | **None.** There is no `[widescreen]` table (`ws_capable = 0`) and no `GameSpec.video`. The NHL `[[widescreen_site]]` entries are only `game_hook` carriers. The launcher toggle and the `gen_ws` / `gen_cells` / `gen_video` caps cannot change an NHL frame. | `game_config.c:521`, `nhl94_spec.c` |

## 2. Mechanisms available

**Horizontal:**

- **H1. Per-view engine margin (`ws_extra`).** The VDP renders extra
  columns (planes wrap-sampled, sprites with 10-bit X). It must be armed
  only for a view's speculative ticks, through a new engine seam. **Not**
  through the session `gen_ws` path, which is session configuration.
- **H2. `find3d` cull hook.** A replacing C hook at `$16920`, active only
  while speculating with zoom > 1. It runs the same projection with limits
  |dx| <= 144 + margin. It writes exactly the routine's outputs (d0/d1, or
  `osflag` in d1).

**Vertical:**

- **V1. Stitched passes.** Render the view again with `Vpos` shifted by D
  and stack the pictures (math in §3a).
- **V2. Engine extra rows.** The renderer draws virtual rows -my..-1 and
  224..223+my from the same VDP state. Plane B comes from a host-side
  plane, sprites from Y with more bits, and plane A is suppressed (§5).
- **V3. V30 in the clone.** Only +16 lines, and it shifts vblank timing.
  Not worth it.

**Presentation:**

- **P1. Scale the larger image down to the window.** Integer scale where
  possible, otherwise sharp bilinear.
- **P2. HUD, name tags and marker drawn at window resolution.**

A pure downscale of today's 256x224 frame adds no ice, so zoom always
means "more pixels, then scale".

## 3. The candidates for the zoom-out

### 3a. (a) Mosaic of camera passes

**How it works.** For each view tick, run N speculative passes from the
same snapshot, each with its own `Hpos` / `Vpos`, then compose the tiles.
It reuses the existing view hook unchanged (camera + `redraw_rink_plane`
per pass).

**Tile layout:**

| Zoom | Image | Layout |
|---|---|---|
| 1.5x | 384x336 | **2x2 passes**: `Hpos` = ±64 (together they cover the 384-px map exactly) and `Vpos` = Vc ± 56. Overlap: 128 px horizontally and 112 px vertically. |
| 2x | 512x448 | 2x2 with no overlap: `Hpos` = ±128 (each outer 64 px is backdrop), `Vpos` = Vc ± 112. |
| more than 2x | | 3x2 or 3x3 |

**Maximum area.** Unlimited in principle. The camera clamps and the map
limit the useful area (horizontal 384 px; vertical about 680-900 px).

**Artifacts:**

- **Seams.** Each pass is a full native frame, so seams are clean only if
  all passes show the same world tick. The guard is to compare
  `s_built_sub` across passes and fall back to one pass if they differ.
- **HUD duplication.** Plane A (clock, ref) appears in every tile. Each
  pass must blank the plane A cells it does not own.
- **Off-screen arrows** (`checksso`). They fall on inner tile edges when
  there is no overlap, as at 2x. Suppress them.
- **Faceoff overlay.** It uses fixed screen positions, so it repeats per
  tile.
- **Sprite limits.** These are per pass, so the 64 total is not a problem
  (each pass sees a quarter of the area). Per-line limits are the same as
  today.

**Cost.** N times a view: about 4-8 ms mean and up to about 18 ms p99 for
2x2 (ESTIMATE = 4x measured). That is the whole frame budget at p99.

- **Online** (1 view per player): too heavy on slow machines.
- **Local 12-view test:** 48 passes is not real time.

**Effort.** Small engine part (N passes per view and a compose hook), but
the compose and seam handling grows with N.

### 3b. (b) Larger virtual raster in the view clone (recommended)

**How it works.** One pass. The VDP renders a W x H virtual screen centred
on the native one:

- **Columns:** -mx..255+mx (H1, exists).
- **Rows:** -my..223+my (V2, new).

**Maximum area.**

- **Horizontal:** 512 (`GVDP_MAX_WIDTH`). Past 384 it is backdrop anyway.
- **Vertical:** the engine reads sprite Y over 10 bits (lines -128 to
  about +895). With a host-side plane B, the full 904-px map is available,
  so 2x (448) and beyond is possible.
- The framebuffer (`MAX_SCREEN_HEIGHT` 480) caps the height at about 2.1x
  unless it is raised.

**Artifacts:**

- **No seams.**
- **The 64-sprite total.** This is the real limit, because the whole
  visible area shares one sprite table. Rough per-frame use: skaters
  (2-4 hardware sprites each), goalies, nets (large), puck and shadow, pad
  stars and labels, and the ref. With all 12 skaters visible, it could pass
  64. **Measure first** (Satt size per view, §9 T0).
  - If it does pass 64, the virtual mode can let the engine walk up to 80
    links. The link field has 7 bits, and the game's Satt holds 90.
  - This is a presentation-only engine flag, like unlimited sprites.
- **Per-line limits.** 16 per line plus the dot budget. Wider lines make
  the dot budget scale with the width automatically, but 16 per line can
  drop sprites when players line up horizontally. Turn on
  `gvdp_set_unlimited_sprites` for zoom views.
- **HUD.** Plane A would stay at its native window position (the clock
  would sit mid-left). Fix: suppress plane A in zoom views and draw the
  clock (from `gameclock`) in the overlay at a window corner (P2).
- **Arrows** (`checksso`). They would be drawn at the native edges, inside
  the picture. Skip them while zoomed: the own skater is always visible.
- **Crowd and arena sprites** (`showcrowd`, `AddArenaAnimSprite`). They are
  placed for the native window and would show only near its edges. Skip
  them while zoomed (the game already turns them off in reverse angle, so
  half the views lack them today).
- **Faceoff close-up overlay.** It stays at its native-window position,
  near the centre. Acceptable; check it on screenshots.

**Cost.** The guest work is the same as today's single view (same ticks).
Added cost is raster only: 2.25x the pixels at 1.5x and 4x at 2x.

- Engine raster time per frame is not measured yet. ESTIMATE: +0.3-1.5 ms
  per view, so about 1.5-3.5 ms total for one zoomed view.
- The extra rows only need drawing on the grabbed sub-tick, because they
  are never guest-visible.
- **Local 12-view test:** roughly 12 x (view + extra raster), likely 15-25
  ms (ESTIMATE). That means fewer views or zoom 1x locally. Online it is 1
  view per player.

**Effort.** Larger, and most of it is in the shared engine (the upstream
patch series). See §5.

### 3c. (c) Hybrid: engine margins for width, stitched passes for height

**How it works.** Margin `mx` = 64 (1.5x) or 128 (2x) with `Hpos` = 0, plus
the `find3d` hook, plus **2 vertical passes** at `Vpos` = Vc ± D/2,
stitched.

**Maximum area.** 512 wide by 448 tall (2 passes of 224), or more with 3
passes.

**Artifacts.** One horizontal seam (same-tick guard, plane A ownership).
Per pass the sprite totals are about half of (b), so the 64-sprite total
is less of a risk. Unlimited sprites is still needed per line on the wide
lines. Arrows and crowd as in (b).

**Cost.** About 2x a view: 2-4 ms mean, p99 about 9 ms (ESTIMATE).
Local 12-view: about 24 passes, not real time.

**Effort.** Medium. B1 plus a pass/compose seam in the engine. No change to
how rows are rendered.

### Comparison

| | (a) mosaic 2x2 | (b) virtual raster | (c) hybrid |
|---|---|---|---|
| Area at 1.5x / 2x | yes / yes | yes / yes (2x+ needs a taller framebuffer) | yes / yes |
| Seams | 2 axes | **none** | 1 horizontal |
| 64-sprite total | per tile, fine | **shared; measure, may need 80** | per half, mostly fine |
| 16 per line | as today | unlimited flag | unlimited flag |
| `find3d` cull | not needed | needed (both axes) | needed (horizontal) |
| HUD duplication | in every tile | none (plane A off, overlay clock) | 2 copies, ownership |
| CPU per view (ESTIMATE) | **4x** (4-8 ms) | **about 1.2-1.8x** (1.5-3.5 ms) | 2x (2-4 ms) |
| 12 local views | no | marginal | no |
| Engine work | small | **large** (raster rows, plane override, sprite Y) | medium |
| Risk | seams, cost | engine, shared renderer | seam, cost |

**Ranking for the zoom-out goal:**

1. **(b)**, staged as B1, then B2 and B3: best image and cost, and it
   scales to 2x.
2. **(c)**, the fallback if B2 is refused or slips. B1 is shared, so no
   work is lost.
3. **(a)**, a prototype only.
4. V30: no.

## 4. Readability: how big things are on a 1080p window

The engine shows H32 with a 1.25 pixel aspect (a 256-wide image in a
320-wide logical cell). A skater sprite is roughly 24-32 px tall in native
pixels (ESTIMATE from the frame art). An 8x8 font character is 8 px.

| Zoom | Native image | Logical (x1.25 w) | Scale on 1080p | Skater on screen | 8x8 text on screen |
|---|---|---|---|---|---|
| 1x (today) | 256x224 | 320x224 | 4x (896 tall), 4.8 to fill | 96-128 px | 32 px |
| 1.25x | 320x280 | 400x280 | 3x (840), 3.86 to fill | 72-96 px | 24 px |
| **1.5x** | 384x336 | 480x336 | **3x (1008, integer)** = 1440x1008 | **72-96 px** | **24 px** |
| 2x | 512x448 | 640x448 | 2x (896), 2.41 to fill | 48-64 px | 16 px |

**What this means:**

- **1.5x is the sweet spot on 1080p.** It is an exact 3x scale with no
  shimmer, players are three-quarters of today's size, and the game's own
  text is still large.
- **2x** halves the sprites. Still readable at 1080p; tight at 720p.
- On 1440p and 4K, 2x becomes comfortable (3x and 4.8x scale).

**Marker and name tags:**

- The game's own **marker** (pad star and position/number label) is a
  sprite, so it shrinks with the zoom. At 2x, consider an overlay ring or
  arrow drawn at window resolution under the own player, using the
  position `nhl94_tags` already captures.
- **Name tags** (`nhl94_tags.c`) are drawn into the view image at image
  resolution, so they also shrink. Move the tag pass to after the scaling
  (window resolution), or scale the tag font with the zoom, so tags keep a
  constant on-screen size.

## 5. Plan (b), step by step

The order puts the cheap, independently useful pieces first.

**Step A: camera framing.** `nhl94_view.c` only. Details in §6.

**Step B1: horizontal zoom.**

1. Engine: a per-view margin seam. For example,
   `GameSpec.view_margin(view)`, or `genesis_multiview_set_margin(k, px)`
   called from prepare.
   - `widescreen_update_for_frame` / `widescreen_pre_raster` use it
     **only while `genesis_multiview_speculating()`**, and reset it to 0
     for the authoritative tick.
   - `mv_host_grab` already takes `gvdp_active_width`.
   - Do not touch `s_ws_user_on`, `ws_capable` or the netplay caps.
2. Game: a `find3d` replacing hook (`$16920`, a new `game_hook` site). If
   not speculating, or zoom is 1x, return 0. Otherwise compute the result
   with limits `144 + mx` (x) and `144 + my` (y), and keep the `sfhor` path
   as is.
3. Game: in zoom views, skip `checksso` (arrows) and `showcrowd` /
   `AddArenaAnimSprite`. These are replacing hooks that return through
   `rts` only while speculating with zoom on.
4. Game: when the zoom width >= 384, pin `Hpos = 0`. For 2x, fill columns
   beyond the map with the backdrop (step B2's plane override, or write
   blank cells into the clone's plane B columns 48-63).
5. Present: size the view cell from the image (`mv_present_grid` uses a
   fixed 320x224 cell today). Use nearest-neighbour integer scale where it
   fits, otherwise sharp bilinear.

The result is a 384x224 full-width view, shippable on its own.

**Step B2: vertical zoom (engine raster rows).**

1. **Engine: virtual rows.** A present-time parameter `my` (rows per side),
   kept outside the `GVDP` struct like `s_ws_extra`.
   - The frame producer renders rows -my..-1 before line 0 and rows
     224..223+my after line 223, from the VDP state at those moments. NHL
     DMAs only in vblank, so VRAM is stable over the active display.
   - Output rows are offset by `my`. Guest-visible status (sprite overflow
     and collision flags, HV counter) comes from native rows only.
   - Raise the framebuffer or grab height as needed (448 + margins needs
     `MAX_SCREEN_HEIGHT` >= 448; it is 480).
2. **Engine: plane override.** A host-side, present-time API, for example
   `gvdp_set_plane_source(PLANE_B, cells, w, h, x0, y0)`. In virtual mode
   it samples plane B from a game-supplied name-table buffer instead of the
   VRAM ring. Tiles still come from VRAM.
   - Game side: at the view hook, fill the buffer once from the ROM map
     (`Rinktilelist` or `RevRinkTilelist`, 48x113), with the same mapping
     `redraw_rink_plane` uses.
   - This removes the 32-row ring limit without moving Satt or changing
     plane sizes in the clone.
3. **Engine: sprite Y in virtual mode.** Treat SAT Y as signed above the
   10-bit range, so sprites partly above row -my clip instead of wrapping.
   Optionally walk up to 80 sprite links (presentation flag).
4. **Engine: plane A off** in virtual mode (flag). The game draws the clock
   in the overlay (B3).
5. **Game:** the view hook sets `my` from the zoom, and `find3d` uses
   `144 + my`. Clamp the camera centre so the virtual window stays within
   the map. With the host plane, the window can go past the old Vpos clamp
   range as far as the map has rows; verify the exact world-y to map-row
   mapping.

**Step B3: presentation and zoom picker.**

1. Scaling as in B1. Name tags and an optional marker ring at window
   resolution. The clock (and optionally the score) in the overlay corner,
   read from guest RAM at the existing authoritative `$007998` capture,
   which is read-only.
2. **Zoom picker:**
   - **Launcher setting:** Mods > View > "Zoom: 1x / 1.25x / 1.5x / 2x",
     default 1.5x once proven. Saved in the local ini.
   - **In-game cycle:** keyboard **Z** or controller **R3** (L3 is already
     the player/puck toggle; R3 without Back is free). Edge-triggered and
     polled per presented tick, exactly like `poll_follow_toggle`. It
     changes the scale, not the window size.
   - **Environment:** `NHL94_VIEW_ZOOM=1|1.25|1.5|2` for tests and the
     localhost scripts.
   - The zoom is a host static. It is never an input, never on the wire,
     and never in caps.

**Hybrid (c) instead of B2.** Replace B2.1-B2.3 with:

- N vertical passes per view (`GameSpec.view_passes`) and a compose hook;
- plane A ownership per pass (blank the clock in the top pass, the ref in
  the bottom one);
- the `s_built_sub` same-tick guard.

B1 and B3 are unchanged.

## 6. Camera framing (step A) and how it fits the zoom-out

**Today:** centre = target + 16 px look-ahead, 0.35 lerp. The own skater
sits only 16 px below the centre, so nearly half the view is behind them.

**Proposed** (oriented coordinates, presentation only; it writes only the
clone's `Hpos` / `Vpos` and the `Oldrow` / plane redraw, as today):

1. **Anchor.** Keep the own skater about `0.2 x H` below the centre, where
   H is the view height: 48 px at 1x, 67 px at 1.5x, 90 px at 2x.
2. **Puck bias.** Aim at own + w·(puck - own), w about 0.45. Clamp so the
   own skater stays in the band [0.18 H, 0.9 H] from the top.
3. **Velocity lead:** Yvel/128, as the game does.
4. **Smoothing:** a ±8 px dead zone and a critically damped follow. Keep
   the snaps (on a flip, and past 160 px).
5. **Horizontal:**
   - 1x: a ±40 px dead zone, as the game.
   - **Zoom width >= 384: `Hpos` pinned to 0** (the whole width is
     visible, so no sideways camera motion at all).
6. **Goalie:** the own net near the bottom edge; the puck bias is capped
   at the own blue line.

**How it fits the zoom-out.** At 1x the framing alone gives about 30-40 px
more ice ahead. With zoom, the horizontal axis is solved by width, and the
same vertical framing places the larger window. The anchor scales with H,
so the extra rows go mostly ahead of the player. `xc1` / `yc1` /
`yleader` / `sfslock` (authoritative camera state) are never touched.

**Settings:** `NHL94_VIEW_ANCHOR`, `NHL94_VIEW_PUCKBIAS`,
`NHL94_VIEW_DEADZONE`, plus the existing `NHL94_VIEW_SMOOTH`. Later these
can be launcher settings.

## 7. Option list (all directions)

| # | Option | Gain | Effort | Risk | Rank |
|---|---|---|---|---|---|
| A | Camera framing | +30-40 px ahead (1x) | S | very low | **1, now** |
| B1 | Per-view margin + `find3d` hook (+ Hpos = 0) | full rink width (1.5x horizontal) | M | low-medium | **2** |
| B2+B3 | Engine virtual rows + host plane B + presentation | 1.5x / 2x vertical, one pass | L | medium (shared engine) | **3**, the target |
| C | Hybrid: B1 + 2 stitched vertical passes | 1.5x / 2x vertical at 2x cost | M | medium (seam) | fallback for 3 |
| a | 2x2 / 3x2 mosaic | any | M | seams, 4-6x cost | prototype only |
| F | V30 in the clone | +16 px | S | timing | no |

## 8. Determinism, netplay and the session image

Every piece changes only the **speculative clone** or **host
presentation**:

- **Clone RAM/VRAM writes:** `Hpos`, `Vpos`, `Oldrow`, plane B redraw,
  marker nibble.
- **Hooks active only in the clone:** `find3d`, `checksso` and
  `showcrowd` / arena replacements. Each is active only when
  `genesis_multiview_speculating()` and zoom > 1. Otherwise it returns 0,
  so the generated code runs untouched in every authoritative and rollback
  tick.
- **Host-only state:** the camera filter, zoom level, margin and virtual
  rows, plane override buffer, presentation scale, and overlay clock and
  tags. The engine parameters stay outside the `GVDP` struct (as
  `s_ws_extra` does), so they never enter `rb_state`, save states, replays
  or digests.
- **The authoritative tick always renders native 256x224.** The per-view
  margin and rows are reset in `mv_host_end`. The speculation suppressions
  in `multiview.h` already keep view ticks out of audio, replay rows and
  presentation history.

**Not part of the session.** Nothing goes into the sealed session image,
the lobby caps (`gen_ws` / `gen_cells` / `gen_video` stay unused for NHL),
or the input stream. Each client picks its own zoom and framing.

**Proof.** `rb_probe` with views, state-hash equality with views off and
on, and a two-client netplay run where the clients use different zoom
levels (§9).

**Camera-dependent game logic** (`assgoaliectrl`, `assscore`,
`EndShootout` read `Hpos` / `Vpos`) keeps seeing the authoritative camera,
because the clone is discarded.

## 9. Test plan

Run everything in `~/nhl94-online-build/wider-view/`, with
the ROM passed by absolute path and `-DNHL94_COPY_ROM_TO_BUILD=OFF`.

**T0, before building anything (measurement).** With today's code:

- log `Sattsize/4` per view frame;
- log Satt entries per view in a "zoomed" view using `GENESIS_VIEW_DEBUG`,
  or a temporary trace in the `find3d` hook build;
- measure engine raster time per frame (time inside
  `genesis_sim_step` with the sink vs without).

This decides whether 80 sprite links are needed and confirms the (b) cost
estimate.

**Determinism tests (every step):**

1. `rb_probe` with 1 and 6 views, at zoom 1x, 1.5x and 2x: PASS.
2. State hash at 5,000 ticks: identical between views off, zoom 1x and
   zoom 2x.
3. Localhost netplay with 2 clients at different zoom levels and framing
   settings: identical sim hashes, no desync, for 10 minutes of play.
4. **Hook inertness.** With views off, the `find3d` / `checksso` /
   `showcrowd` hooks never return 1. Count the calls in a debug build.

**Visual tests** (`GENESIS_VIEW_SCREENSHOT_EVERY`, both orientations, both
periods). Scenes: breakout, own-zone defence, goalie view, a faceoff with
close-up, a goal celebration, line change, power play, and every player
bunched in one corner (the sprite-limit stress).

- No missing players near the old edges (the NHL '98 GPGX Wide failure).
- No sprite wrap at the virtual top and bottom.
- No duplicate HUD.
- No plane B garbage past the map.
- Arrows and crowd absent while zoomed.
- The clock overlay is correct.

**Pixel tests:**

- At zoom 1x, the view image is byte-identical to today's view.
- The native 256x224 centre of a zoomed image matches a 1x view at the
  same camera, except the plane A HUD and suppressed sprites.
- (c) only: rows S-4..S+4 at the seam show no tear; log `s_built_sub` for
  both passes and count fallbacks.

**Performance:** `multiview: summary` for 1 zoomed view.

- Target mean under 3 ms and p99 under 6 ms at 1.5x on the Mac.
- Same run on the slowest Windows client during a 12-player online
  session.
- 12-view local: record the cost and keep zoom 1x as the default there if
  it is over budget.

**Usability:** 1080p and 1440p windows at 1.5x and 2x. Check that the
integer scale is chosen, the tags keep their on-screen size, R3 / Z cycles
the zoom, and the launcher setting persists.

## 10. Open questions

1. **Default zoom.** 1.5x (integer 3x on 1080p) or 2x on large screens?
   Should goalies get a different default?
2. Is the shared-engine work for B2 (virtual rows, plane override, signed
   sprite Y) acceptable in the upstream patch series, or should the
   project stop at B1 + (c)?
3. Does the whole rink at 2x exceed 64 hardware sprites in practice (T0)?
4. **Overlay HUD:** clock only, or also the score and period (the game
   shows the score only on the horizontal scoreboard rink)?
5. Exactly how far does the map reach past the old camera clamps (world y
   to map row)? It decides how close to the end boards a 2x view can
   centre.

## 11. Prior art

A web search on 2026-11-23 covered nhl94.com, libretro, RetroPie, Sonic
Retro, GitHub, GameTech Wiki and news sites. The search engine returned no
YouTube or Reddit results. Some pages could not be opened (the heyjoeway
GitHub repos returned 404 to the fetch tool; the PC Gamer article body did
not load). Those entries are based on search snippets.

**Summary:**

- **Nothing exists for NHL '94 itself.** No widescreen, zoom-out,
  taller-view or camera mod was found for the Genesis game. The nhl94.com
  community's hacks are rosters, graphics, fights, multitap and speed.
- **No known project renders more lines vertically** on Genesis or SNES.
  Every widescreen project found extends only horizontally. The vertical
  half of the zoom (virtual rows or stitched passes) appears to be new.

| Work | What it does | How | Limits / artifacts | What we take from it |
|---|---|---|---|---|
| **"Tech Talk (MD/Super 32X): probability of obtaining a proper resolution?"**, nhl94.com forum, Royameadow, June 2015 ([thread](https://forum.nhl94.com/index.php/topic/16877-tech-talk-mdsuper-032x-probability-of-obtaining-a-proper-resolution/)) | A wish for anamorphic widescreen NHL '94. Mockups show the 256x224 image with sides pieced together from Instant Replay frames. | Speculative only (a wider MD mode or 32X software rendering). Nothing was built. The admin (kingraph) doubted anyone was working on it. | None (never built) | It confirms nobody has done this. The mockup (wider context from other camera positions of the same frame) is the mosaic idea (a), done offline. |
| **Genesis Plus GX Wide**, heyjoeway (fork of ekeeke's Genesis Plus GX). Libretro core since 2021 ([libretro post](https://www.libretro.com/index.php/genesis-plus-gx-wide-now-available/), [libretro repo](https://github.com/libretro/Genesis-Plus-GX-Wide), [heyjoeway fork](https://github.com/heyjoeway/Genesis-Plus-GX)) | The VDP draws N extra 8-px columns per side ("Extra columns to draw in H40") | Emulator-side rendering of planes and sprites in the margins, with no game changes. Games that cull objects need ROM patches: Sonic 1 through heyjoeway's autopatcher, Sonic 2 through his [s2disasm hack](https://github.com/heyjoeway/s2disasm). | Pop-in at the edges, plane-size limits (Sonic 1 capped at 12 columns), many games warp. Horizontal only. | The same model as our engine's `ws_extra` plus game-side fixes, which is B1. |
| **NHL '98 on GPGX Wide**, libretro forum, Mr.Curious, Nov 2024 ([thread](https://forums.libretro.com/t/genesis-plus-gx-wide-issues-nhl-98/46475)) | Ran the EA hockey engine (NHL '98) with 12 extra columns | Emulator margins only, no game patch | "Players sometimes become invisible", only in the widened edge areas. No fix was found. | **Independent evidence** of the `find3d` cull problem. Emulator margins alone are not enough for this engine, so B1 must include the cull hook. |
| **bsnes-hd**, DerKoun ([GitHub](https://github.com/DerKoun/bsnes-hd), [libretro docs](https://docs.libretro.com/library/bsnes_hd_beta/)) | SNES widescreen (Mode 7 or all scenes), HD Mode 7, per-scene enable | Emulator renders the extra columns. Sprites have safe / unsafe / clip / disable modes ("safe" draws only sprites partly in the 4:3 area). Per-game configuration files. | Sprites in the margins need ROM hacks. Many artifacts, so widescreen is off by default outside Mode 7. | Keep a native fallback for scenes that do not support zoom (we already go canonical in menus, replay and highlights). Sprite handling at the old edges is the main risk. |
| **wide-snes / SMW Widescreen**, Vitor Vilela and contributors ([GitHub](https://github.com/VitorVilela7/wide-snes)) | Super Mario World at 352x224 or 384x224 | BPS ROM patch (ASM changes) plus a bsnes-hd configuration file | bsnes-hd only. Horizontal only. | Emulator margins plus game-side fixes. For us the game side must be C hooks (never ROM patches), and only in the view clone. |
| **GameTech Wiki "Widescreen hacks"** ([page](https://emulation.gametechwiki.com/index.php/Widescreen_hacks)) | An overview across systems | Mostly per-game camera-size hacks | Warns of per-game glitches | General context |
| **"High-res Sonic hack crams entire stages onto a single screen"**, PC Gamer, 2021 ([article](https://www.pcgamer.com/this-high-res-sonic-hack-crams-entire-stages-on-a-single-screen/)) | Sonic 2 at a very large width. From search snippets: stretched over two monitors. | Not verified (body not loaded). Most likely heyjoeway's fork at a large column count. | Unknown | Shows that an extreme zoom-out is technically possible when the game cooperates |

**What we reuse:** no code. Everything above is horizontal, emulator-side
and for other games, and our engine already has the margin mechanism.

**What we learn:**

1. **The EA hockey engine needs the cull fix** (NHL '98 evidence).
2. **Keep a native fallback** for scenes that are not supported.
3. **Expect artifacts at the old edges** and screenshot-test them.
4. **Vertical has no precedent,** so prove it with T0 and the pixel tests
   before making it the default.

Credit: Royameadow and kingraph (nhl94.com), heyjoeway and ekeeke
(Genesis Plus GX / GX Wide), Mr.Curious (libretro forum), DerKoun
(bsnes-hd), Vitor Vilela and contributors (wide-snes).
