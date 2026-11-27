/* multiview.c -- speculative views (see multiview.h). */
#include "multiview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "game_spec.h"
#include "png_write.h"
#include "rb_state.h"
#include "sim_step.h"
#include "video/genesis_machine.h"

/* rb_state.c: the snapshot pair that leaves no feedback-replay checkpoint
 * rows and no presentation reset behind (speculation is invisible). */
extern size_t genesis_rb_save_untracked(void *dst, size_t cap);

static int  s_read;               /* env read */
static int  s_count;              /* 0 = off */
static int  s_count_cfg;          /* as configured, for the summary */
static int  s_speculating;
static GenesisViewPrepareFn s_prepare;
static void *s_prepare_user;
static GenesisViewPrepareFn s_override;
static void *s_override_user;
static int  s_has_override;
static int  s_shot_every;
static const char *s_shot_dir;
static GenesisViewImage s_img[GENESIS_MULTIVIEW_MAX];
static uint32_t s_last_tick;
static int  s_last_rendered;
static int  s_ticks_per_view = 1; /* speculative ticks per view, grab after the last */
static int  s_view_tick;          /* 0-based sub-tick while speculating */
static int  (*s_ready)(int, void *);  /* GameSpec.view_ready with the game's prepare */
static int  s_extra_max = 3;      /* extra ticks a view may wait for view_ready */
static unsigned long long s_views_total, s_views_extra, s_views_unready;

static uint8_t *s_snap;
static size_t   s_snap_cap;

/* Cost per presented tick (all views, save+loads+ticks+copies). */
#define MV_TIMES 16384
static double   s_ms[MV_TIMES];
static unsigned s_ms_n;
static double   s_ms_sum;
static unsigned long long s_ticks;

static void prep_noop(int view, void *user) { (void)view; (void)user; }

/* Visible, game-agnostic proof that prepare reaches the clone: shift every
 * vertical-scroll entry by 16*k lines. Only the clone is touched. */
static void prep_vscroll(int view, void *user)
{
    (void)user;
    for (int i = 0; i < GVDP_VSRAM_ENTRIES; i++)
        g_machine.vdp.vsram[i] = (uint16_t)((g_machine.vdp.vsram[i] + 16 * view) & 0x7FFu);
}

static int  s_cli_views = -1;
static char s_cli_debug[16];
static char s_game_arg[512];

int genesis_multiview_configure(const char *spec)
{
    if (!spec || !*spec || s_read) return 0;
    /* <n>[:<debug>] | [<n>]@<game-arg> */
    const char *at = strchr(spec, '@');
    if (at) {
        if (!at[1] || strlen(at + 1) >= sizeof s_game_arg) return 0;
        snprintf(s_game_arg, sizeof s_game_arg, "%s", at + 1);
    }
    if (at == spec) { s_cli_views = -1; return 1; }   /* count from the game */
    char *end = NULL;
    long n = strtol(spec, &end, 10);
    if (end == spec || n < 0) return 0;
    if (*end == ':' && !at) {
        snprintf(s_cli_debug, sizeof s_cli_debug, "%s", end + 1);
    } else if (*end && end != at) {
        return 0;
    }
    s_cli_views = (int)n;
    return 1;
}

const char *genesis_multiview_game_arg(void) { return s_game_arg[0] ? s_game_arg : NULL; }

static void mv_read_env(void)
{
    if (s_read) return;
    s_read = 1;
    const char *v = getenv("GENESIS_VIEWS");
    int n = s_cli_views >= 0 ? s_cli_views : (v && *v ? atoi(v) : 0);
    /* Neither says how many: the game may (its own view list). */
    if (s_cli_views < 0 && !(v && *v) && g_game_spec.view_default_count)
        n = g_game_spec.view_default_count();
    if (n <= 0) return;
    if (n > GENESIS_MULTIVIEW_MAX) {
        fprintf(stderr, "multiview: GENESIS_VIEWS=%d clamped to %d\n", n, GENESIS_MULTIVIEW_MAX);
        n = GENESIS_MULTIVIEW_MAX;
    }
    const char *dbg = s_cli_debug[0] ? s_cli_debug : getenv("GENESIS_VIEW_DEBUG");
    if (dbg && *dbg) {
        if (!strcmp(dbg, "noop")) s_prepare = prep_noop;
        else if (!strcmp(dbg, "vscroll")) s_prepare = prep_vscroll;
        else {
            fprintf(stderr, "multiview: unknown GENESIS_VIEW_DEBUG=%s (noop|vscroll) -- views off\n", dbg);
            return;
        }
        s_prepare_user = NULL;
    } else if (g_game_spec.view_prepare) {
        s_prepare = g_game_spec.view_prepare;
        s_prepare_user = g_game_spec.view_user;
        s_ready = g_game_spec.view_ready;
    } else {
        /* A late genesis_multiview_set_prepare can still supply one. */
        s_prepare = NULL;
    }
    s_count = s_count_cfg = n;
    {
        /* Ticks per view: the game's need (GameSpec.view_ticks, e.g. a game
         * that builds its display list one frame and shows it the next),
         * GENESIS_VIEW_TICKS overrides. Debug prepares are about the
         * current frame, so they keep 1 unless overridden. */
        const char *t = getenv("GENESIS_VIEW_TICKS");
        int tv = (t && *t) ? atoi(t) : ((dbg && *dbg) ? 1 : g_game_spec.view_ticks);
        if (tv < 1) tv = 1;
        if (tv > 8) tv = 8;
        s_ticks_per_view = tv;
        const char *x = getenv("GENESIS_VIEW_TICKS_EXTRA");
        if (x && *x) s_extra_max = atoi(x) < 0 ? 0 : (atoi(x) > 8 ? 8 : atoi(x));
    }
    const char *e = getenv("GENESIS_VIEW_SCREENSHOT_EVERY");
    s_shot_every = e && *e ? atoi(e) : 0;
    s_shot_dir = getenv("GENESIS_VIEW_SCREENSHOT_DIR");
    if (!s_shot_dir || !*s_shot_dir) s_shot_dir = ".";
    fprintf(stderr, "multiview: %d views, prepare=%s, ticks/view=%d, screenshot every=%d dir=%s\n", n,
            dbg && *dbg ? dbg : (g_game_spec.view_prepare ? "game" : "registered-later"),
            s_ticks_per_view, s_shot_every, s_shot_dir);
}

int genesis_multiview_enabled(void)
{
    mv_read_env();
    return s_count > 0;
}

int genesis_multiview_count(void) { mv_read_env(); return s_count; }

void genesis_multiview_disable(const char *why)
{
    mv_read_env();
    if (s_count > 0)
        fprintf(stderr, "multiview: views disabled (%s)\n", why ? why : "host request");
    s_count = 0;
}

void genesis_multiview_set_prepare(GenesisViewPrepareFn fn, void *user)
{
    s_override = fn;
    s_override_user = user;
    s_has_override = fn != NULL;
}

int genesis_multiview_speculating(void) { return s_speculating; }
static int s_geom_cols, s_geom_rows;   /* the running view's GameSpec.view_geometry */
int genesis_multiview_extra_cols(void) { return s_speculating ? s_geom_cols : 0; }
int genesis_multiview_extra_rows(void) { return s_speculating ? s_geom_rows : 0; }
int genesis_multiview_view_tick(void) { return s_speculating ? s_view_tick : -1; }
int genesis_multiview_ticks_per_view(void) { mv_read_env(); return s_ticks_per_view; }

static char s_label[GENESIS_MULTIVIEW_MAX][32];

void genesis_multiview_set_label(int view, const char *label)
{
    if (view < 0 || view >= GENESIS_MULTIVIEW_MAX) return;
    snprintf(s_label[view], sizeof s_label[view], "%s", label ? label : "");
}

const char *genesis_multiview_label(int view)
{
    return (view >= 0 && view < GENESIS_MULTIVIEW_MAX) ? s_label[view] : "";
}

const GenesisViewImage *genesis_multiview_image(int view)
{
    return (view >= 0 && view < s_count) ? &s_img[view] : NULL;
}

int genesis_multiview_image_reserve(GenesisViewImage *dst, int stride, int height)
{
    if (!dst || stride <= 0 || height <= 0) return 0;
    size_t need = (size_t)stride * (size_t)height;
    if (dst->capacity < need) {
        uint32_t *p = (uint32_t *)realloc(dst->pixels, need * sizeof *p);
        if (!p) return 0;
        dst->pixels = p;
        dst->capacity = need;
    }
    dst->stride = stride;
    return 1;
}

static double now_ms(void)
{
    return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

int genesis_multiview_render(const GenesisSimInput *in, const GenesisMultiviewHost *host)
{
    s_last_rendered = 0;
    if (!genesis_multiview_enabled() || !host || !host->run_tick || !host->grab) return 0;
    GenesisViewPrepareFn prep = s_has_override ? s_override : s_prepare;
    void *user = s_has_override ? s_override_user : s_prepare_user;
    if (!prep) {
        genesis_multiview_disable("no view_prepare: set GameSpec.view_prepare, "
                                  "genesis_multiview_set_prepare or GENESIS_VIEW_DEBUG");
        return 0;
    }
    double t0 = now_ms();
    size_t need = genesis_rb_bound();
    if (s_snap_cap < need) {
        uint8_t *nb = (uint8_t *)realloc(s_snap, need);
        if (!nb) { genesis_multiview_disable("snapshot allocation failed"); return 0; }
        s_snap = nb; s_snap_cap = need;
    }
    size_t len = genesis_rb_save_untracked(s_snap, s_snap_cap);
    if (!len) { genesis_multiview_disable("snapshot save failed"); return 0; }
    uint32_t tick = genesis_sim_tick_count();
    if (host->begin) host->begin(host->ctx);
    int done = 0;
    for (int k = 0; k < s_count; k++) {
        if (k > 0 && !genesis_rb_load_untracked(s_snap, len)) break;
        s_speculating = 1;
        s_view_tick = 0;
        prep(k, user);
        /* Every sub-tick reuses tick T's sealed input: the later ticks only
         * carry the view's frame to the screen and are discarded. */
        /* At least P ticks; then, if the game says the last tick did not
         * yet display the view's frame (a lag frame delayed it), up to
         * s_extra_max more. */
        int ok = 1, j = 0, ready = 0;
        for (; ok && j < s_ticks_per_view + s_extra_max; j++) {
            s_view_tick = j;
            /* The game may decide the picture size only once its hook has
             * seen the frame (e.g. a canonical menu frame stays native), so
             * ask before every sub-tick; the grabbed frame is rendered with
             * the last answer. */
            s_geom_cols = s_geom_rows = 0;
            if (!s_has_override && g_game_spec.view_geometry) {
                g_game_spec.view_geometry(k, user, &s_geom_cols, &s_geom_rows);
                if (s_geom_cols < 0) s_geom_cols = 0;
                if (s_geom_rows < 0) s_geom_rows = 0;
            }
            ok = host->run_tick(host->ctx, in);
            if (ok && j + 1 >= s_ticks_per_view) {
                int (*rf)(int, void *) = s_has_override ? NULL : s_ready;
                ready = !rf || rf(k, user);
                if (ready) { j++; break; }
            }
        }
        if (ok) {
            s_views_total++;
            if (j > s_ticks_per_view) s_views_extra++;
            if (!ready) s_views_unready++;
        }
        s_speculating = 0;
        s_view_tick = 0;
        s_geom_cols = s_geom_rows = 0;
        s_img[k].valid = 0;
        if (!ok) break;
        host->grab(host->ctx, &s_img[k]);
        s_img[k].tick = tick;
        s_img[k].valid = s_img[k].pixels && s_img[k].width > 0 && s_img[k].height > 0;
        if (s_img[k].valid && g_game_spec.present_overlay)
            g_game_spec.present_overlay(s_img[k].pixels, s_img[k].width, s_img[k].height,
                                        s_img[k].stride, k);
        done++;
    }
    if (!genesis_rb_load_untracked(s_snap, len)) {
        /* Cannot happen for a blob this process just wrote; the machine is
         * no longer trustworthy, so stop rather than present a wrong tick. */
        fprintf(stderr, "multiview: FATAL restore failed at tick %u\n", tick);
        abort();
    }
    if (host->end) host->end(host->ctx);
    double ms = now_ms() - t0;
    if (s_ms_n < MV_TIMES) s_ms[s_ms_n++] = ms;
    s_ms_sum += ms;
    s_ticks++;
    s_last_tick = tick;
    s_last_rendered = done;
    return done;
}

static void write_png(const char *name, int idx, uint32_t tick, const GenesisViewImage *img)
{
    char path[1024];
    if (idx >= 0) snprintf(path, sizeof path, "%s/%s%d_t%u.png", s_shot_dir, name, idx, tick);
    else snprintf(path, sizeof path, "%s/%s_t%u.png", s_shot_dir, name, tick);
    static unsigned failures;
    if (png_write_argb(path, img->pixels, img->width, img->height, img->stride) != 0 &&
        failures++ < 4)
        fprintf(stderr, "multiview: failed to write %s (does the directory exist?)\n", path);
}

void genesis_multiview_post_tick(const GenesisViewImage *main_frame)
{
    if (s_count <= 0 || !s_last_rendered || s_shot_every <= 0) return;
    if (s_last_tick % (uint32_t)s_shot_every) return;
    for (int k = 0; k < s_last_rendered; k++)
        if (s_img[k].valid) write_png("view", k, s_last_tick, &s_img[k]);
    if (main_frame && main_frame->pixels) write_png("main", -1, s_last_tick, main_frame);
}

static int dcmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

void genesis_multiview_summary(void)
{
    if (!s_ticks) return;
    qsort(s_ms, s_ms_n, sizeof s_ms[0], dcmp);
    fprintf(stderr, "multiview: summary views=%d ticks/view=%d ticks=%llu ms_per_tick mean=%.3f p50=%.3f p99=%.3f max=%.3f"
            " view_frames=%llu late=%llu unready=%llu\n",
            s_count_cfg, s_ticks_per_view, s_ticks, s_ms_sum / (double)s_ticks, s_ms[s_ms_n / 2],
            s_ms[(s_ms_n * 99) / 100], s_ms[s_ms_n - 1], s_views_total, s_views_extra, s_views_unready);
}
