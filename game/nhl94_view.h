/*
 * nhl94_view.h - per-player camera views for NHL Hockey '94 on top of the
 * engine's speculative views (runner/multiview.h). See nhl94_view.c.
 */
#ifndef NHL94_VIEW_H
#define NHL94_VIEW_H
#include <stdint.h>

/* GameSpec.view_prepare */
void nhl94_view_prepare(int view_index, void *user);
/* GameSpec.view_ready: the last view tick displayed the view's frame. */
int  nhl94_view_ready(int view_index, void *user);
/* GameSpec.view_default_count: number of entries in the view list. */
int  nhl94_view_default_count(void);
/* GameSpec.view_geometry: the zoomed view's extra columns / rows. */
void nhl94_view_geometry(int view_index, void *user, int *extra_cols, int *extra_rows);
/* GameSpec.present_overlay: a view's held picture through a stoppage, then
 * the name tags (nhl94_tags_present_overlay). */
void nhl94_view_present_overlay(uint32_t *pixels, int w, int h, int stride, int view_index);
/* GameSpec.instruction_hook (game.toml game_hook sites). Returns 1 only for
 * find3d in a zoomed view's speculative tick (replaced by a wider cull). */
int  nhl94_view_instruction_hook(uint32_t pc);

/* Launcher "View zoom" (nhl94_mode.c): quarter steps 4 (1x), 5 (1.25x),
 * 6 (1.5x), 8 (2x); 0 = default (1.5x for position lock, else 1x). */
void nhl94_view_set_zoom_setting(int q);
int  nhl94_view_zoom_setting(void);

#endif /* NHL94_VIEW_H */
