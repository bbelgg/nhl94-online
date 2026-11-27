#ifndef GENESIS_MULTIVIEW_H
#define GENESIS_MULTIVIEW_H
/*
 * multiview.h -- speculative views: one deterministic simulation shown from
 * several cameras at once, without ever changing the simulation.
 *
 * Per presented tick T (offline, or the live tick online), BEFORE the
 * authoritative tick runs:
 *
 *   S = snapshot(machine)                       (rb_state, untracked)
 *   for view k in 0..N-1:
 *       restore(S)                              (view 0 runs on S as is)
 *       prepare(k, user)                        game hook: may mutate the CLONE
 *       run P ticks with T's sealed input, rendering normally (P = ticks
 *       per view, default 1)
 *       copy the framebuffer of the last one -> view k's image
 *   restore(S)
 *   (the host restores its framebuffer and runs tick T normally)
 *
 * A view whose prepare is a no-op therefore shows exactly the frame the
 * authoritative tick produces, and the authoritative state is bit-identical
 * to a run with views disabled (the rollback property rb_probe verifies).
 *
 * A speculative tick is genesis_sim_step with the rollback-replay contract
 * (scratch audio, no post-drain observation hooks) plus the suppressions
 * keyed on genesis_multiview_speculating(): no feedback-replay rows
 * (sim_step.c), no presentation-history capture (main.c). Snapshots use the
 * untracked save/load (no replay checkpoint rows, no presentation reset).
 * Everything else the host does per tick (audio output, SRAM persistence,
 * netplay staging, input-script advance, screenshots, rewind, frame rings)
 * runs outside genesis_sim_step and only for the authoritative tick.
 *
 * CONFIG (environment, read once):
 *   GENESIS_VIEWS=<n>                  enable n views (1..8); 0/unset = off
 *   GENESIS_VIEW_DEBUG=noop|vscroll    built-in prepare instead of the
 *                                      game's: noop = identity (test);
 *                                      vscroll = view k shifts every VSRAM
 *                                      entry by 16*k lines (visible proof)
 *   GENESIS_VIEW_SCREENSHOT_EVERY=<n>  every n ticks write
 *   GENESIS_VIEW_SCREENSHOT_DIR=<dir>    <dir>/view<k>_t<tick>.png and the
 *                                      authoritative <dir>/main_t<tick>.png
 *   GENESIS_VIEW_TICKS=<p>             speculative ticks per view (1..8),
 *                                      overriding GameSpec.view_ticks. A game
 *                                      whose display lags its logic by a
 *                                      frame (display list built in frame t,
 *                                      uploaded in its vblank, shown in t+1)
 *                                      needs p >= 2 to show what prepare (or
 *                                      its own hooks) changed. View images are
 *                                      then p-1 frames ahead of the main one.
 *   GENESIS_VIEW_TICKS_EXTRA=<x>       when GameSpec.view_ready says the
 *                                      P-th tick did not yet display the
 *                                      view's frame (e.g. a lag frame), run
 *                                      up to x more ticks (default 3); the
 *                                      summary counts late / unready views
 *   GENESIS_VIEW_GRID_MAIN=1           window grid also shows the main view
 *                                      (tile 0)
 *   --views <n>[:<debug>]              command-line equivalent of
 *                                      GENESIS_VIEWS / GENESIS_VIEW_DEBUG
 *                                      (wins over the environment)
 *   --views [<n>]@<game-arg>           pass <game-arg> to the game's views
 *                                      (genesis_multiview_game_arg); without
 *                                      <n> (and no GENESIS_VIEWS) the count
 *                                      is GameSpec.view_default_count()
 *
 * GAME HOOK: GameSpec.view_prepare(view_index, GameSpec.view_user), or a
 * runtime registration via genesis_multiview_set_prepare (wins over the
 * GameSpec field). Without either and without GENESIS_VIEW_DEBUG, views
 * stay off (a log line says why). A prepare may write guest RAM, VDP
 * registers/VRAM/CRAM/VSRAM, or adapter state covered by rb_state_save --
 * anything inside the rollback snapshot. It must not touch host-side state
 * that outlives the restore.
 *
 * BIGGER PICTURES: GameSpec.view_geometry (optional) is asked before every
 * sub-tick of a view for extra columns (pixels per side: the widescreen
 * margin, no 16:9 canvas) and extra rows (per side: the VDP's virtual rows)
 * that the view's ticks render; the grabbed image is that much bigger and
 * the window grid sizes its cells from the images. Present-time parameters
 * only (outside the VDP struct and the snapshot); restored after the views,
 * so the authoritative tick renders the native screen.
 */
#include <stddef.h>
#include <stdint.h>

#include "sim_step.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GENESIS_MULTIVIEW_MAX 8

typedef void (*GenesisViewPrepareFn)(int view_index, void *user);

typedef struct GenesisViewImage {
    uint32_t *pixels;     /* ARGB8888, row stride `stride` pixels */
    int       stride;
    size_t    capacity;   /* pixels allocated */
    int       width, height;
    uint32_t  tick;       /* sim tick this frame shows (tick count before it ran) */
    int       valid;
} GenesisViewImage;

/* The host side of a speculative tick (main.c). */
typedef struct GenesisMultiviewHost {
    /* Once per presented tick, before the first view: preserve whatever host
     * presentation state run_tick overwrites (the framebuffer). Optional. */
    void (*begin)(void *ctx);
    /* Run ONE tick with `in` and the replay contract (scratch audio, no
     * observation), rendering through the host's normal pipeline. 1 = done. */
    int  (*run_tick)(void *ctx, const GenesisSimInput *in);
    /* Copy the frame run_tick just rendered into dst (resize via
     * genesis_multiview_image_reserve). */
    void (*grab)(void *ctx, GenesisViewImage *dst);
    /* After the last view and the final restore. Optional. */
    void (*end)(void *ctx);
    void *ctx;
} GenesisMultiviewHost;

/* --views <n>[:<debug>] | [<n>]@<game-arg>; must precede the first use.
 * 0 = malformed. */
int  genesis_multiview_configure(const char *spec);
/* The <game-arg> of --views, or NULL. */
const char *genesis_multiview_game_arg(void);
/* Reads the environment on first use. 1 when views are on. */
int  genesis_multiview_enabled(void);
int  genesis_multiview_count(void);
/* Turn views off for the rest of the process (with a reason line). */
void genesis_multiview_disable(const char *why);
/* Runtime override of GameSpec.view_prepare (NULL restores the spec's). */
void genesis_multiview_set_prepare(GenesisViewPrepareFn fn, void *user);
/* 1 while a speculative tick (or its prepare) is running. */
int  genesis_multiview_speculating(void);
/* While speculating: which of the view's ticks is running (0..P-1);
 * -1 otherwise. prepare runs with 0. */
int  genesis_multiview_view_tick(void);
int  genesis_multiview_ticks_per_view(void);

/* While speculating: the extra columns (pixels per side) / rows (per side)
 * GameSpec.view_geometry asked for the running view; 0 otherwise. */
int  genesis_multiview_extra_cols(void);
int  genesis_multiview_extra_rows(void);

/* Render every view for the tick about to run with input `in`. Must be
 * called between ticks on the scheduler fiber, immediately before the
 * authoritative genesis_sim_step(in, ...). Leaves the machine exactly as it
 * found it. Returns the number of views rendered. */
int  genesis_multiview_render(const GenesisSimInput *in, const GenesisMultiviewHost *host);

/* After the authoritative tick: PNG dump (GENESIS_VIEW_SCREENSHOT_*) of the
 * views and of `main` (the authoritative frame of the same tick). */
void genesis_multiview_post_tick(const GenesisViewImage *main_frame);

const GenesisViewImage *genesis_multiview_image(int view);
/* Optional per-view caption the host may draw over the view (the window grid
 * does, top-left), e.g. "HOME C". Set by the game; NULL/"" clears it.
 * Presentation only: never part of machine state. */
void        genesis_multiview_set_label(int view, const char *label);
const char *genesis_multiview_label(int view);   /* "" when unset */
/* Ensure dst holds stride*height pixels; 1 on success. */
int  genesis_multiview_image_reserve(GenesisViewImage *dst, int stride, int height);

/* Exit line: "multiview: summary views=.. ticks=.. ms/tick p50 p99 mean". */
void genesis_multiview_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* GENESIS_MULTIVIEW_H */
