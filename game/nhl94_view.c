/*
 * nhl94_view.c - per-player camera views for NHL Hockey '94.
 *
 * One deterministic simulation, up to 8 cameras. Each view follows one
 * player (team + role) or the puck, and is turned so that player's team
 * attacks UP the screen (own goalie at the bottom), using the game's own
 * Instant Replay "Reverse Angle" renderer for the 180-degree turn.
 *
 * WHERE IT RUNS. Only inside the engine's speculative view ticks
 * (runner/multiview.h): the engine snapshots the machine before each
 * authoritative tick, and per view restores it, calls nhl94_view_prepare,
 * runs GameSpec.view_ticks (= 2) ticks, grabs the frame and finally restores
 * the snapshot. Nothing here ever runs in (or leaks into) the authoritative
 * tick: the camera feeds back into the simulation (lag frames, HV-counter
 * randomness, camera-relative AI targets; docs/VIEW_RESEARCH.md section 5),
 * so it may only change on the discarded clone.
 *
 * HOW. game.toml declares a game_hook site, $007998 = `jmp setvideo` at
 * the end of DoGameFrame. When a view tick reaches it, this frame's logic,
 * checkwindow and updatereplay are done and setvideo is about to build the
 * sprite table and queue rink rows for the next vblank. The hook (first hit
 * per view only):
 *   1. finds the target object in the clone (role scan, puck fallback),
 *   2. if the target's team attacks down this period, applies the Reverse
 *      Angle transform exactly as RestoreReplayFrame does (replay94.asm:456):
 *      negate X/Y of all 16 objects, puck shadow +2, frames through
 *      revframetbl, glass/fight frame fix-ups, sflags4 bit 4, OOlist re-sort
 *      (SprSort), and the RevRinkTiles upload ReloadRinkGraphics would do,
 *   3. sets Hpos/Vpos to the view's camera (game clamps) and redraws the
 *      32-row plane B ring for it (what updatescroll does after Oldrow =
 *      $7D0, written directly; see redraw_rink_plane for why).
 * setvideo then builds that frame with the game's own code (culling, sprite
 * tile DMA for frames != oldframe, crowd/arena-anim suppression in reverse),
 * the vblank of that tick uploads it and the NEXT tick displays it, which is
 * why a view needs two ticks (measured: see NHL94_VIEW_TRACE). Two more
 * sites ($015F28 setvideo `bset #dfok`, $015DB2 VBlank `bsr DumpSprites`)
 * record when the view's list was completed and uploaded; view_ready asks
 * the engine for more ticks when a lag frame delayed the upload.
 *
 * Logic is never run on a mirrored world before the grabbed frame: the
 * mirror is applied after this frame's logic, and the second tick's logic
 * only affects the frame after the one that is grabbed.
 *
 * Host-side state (camera smoothing, fallback log) lives here, not in the
 * machine: it must survive the snapshot restore and never reach the guest
 * except through the clone.
 *
 * CONFIG
 *   NHL94_VIEWS="home:G,away:G,home:C,away:C,home:RD,away:RD"  (default)
 *       team = home|away, role = G|LD|RD|LW|C|RW|X|puck, optional suffix
 *       ":up" (default, auto-orient so the team attacks up) or ":fixed"
 *       (never turn). The entry count is the view count when GENESIS_VIEWS
 *       is unset. --views [<n>]@<list> passes the same list on the command
 *       line (wins over the environment; needed where the env is cleared).
 *   NHL94_VIEW_TRACE=1   log hook timing (tick, scanline, sub-tick) and
 *                        every view decision.
 *   NHL94_VIEW_LOOKAHEAD=<px>  camera offset toward the attacked goal (16).
 *   NHL94_VIEW_MARKER=0  no active-player marker (default on: the game's own
 *                        pad-1 star + position/number label under the
 *                        view's player, via PadControlBits, see apply_view).
 *   NHL94_VIEW_DMA_REDRAW=1    use the game's Oldrow=$7D0 DMA redraw instead
 *                        of the direct plane write (diagnostic).
 *   NHL94_VIEW_SMOOTH=<0..1>   fraction of the remaining distance (past
 *                        the dead zone) the camera moves per frame (0.20;
 *                        0.35 without framing; 1 = locked).
 *   NHL94_VIEW_FRAMING=0  the old centred camera (target + LOOKAHEAD).
 *   NHL94_VIEW_ANCHOR=<f>  own player f x view height below the centre
 *                        (0.20); goalies 0.33.
 *   NHL94_VIEW_PUCKBIAS=<f> lean toward the puck (0.45; goalies 0.30, only
 *                        up to their blue line).
 *   NHL94_VIEW_DEADZONE=<px> vertical dead zone (8; horizontal 3x).
 *   NHL94_VIEW_ZOOM=1|1.25|1.5|2  picture size (docs/WIDER_VIEW_PLAN.md):
 *                        1.5 = 384x336, the whole rink width. Default: the
 *                        launcher's "View zoom" (1.5) for position lock's
 *                        own view, 1 for test view lists and spectators.
 *                        Keyboard F / controller R3 cycles it in game.
 *
 * ZOOM. GameSpec.view_geometry asks the engine for extra columns (its
 * per-view widescreen margin) and extra rows (the VDP's virtual rows) for
 * the view's ticks. In the clone: find3d ($016920) is replaced by the same
 * projection with a wider cull, checksso's off-screen arrows ($016480) are
 * skipped, a taller view switches the planes to 64x64 with the sprite
 * table moved from $DC00 to $F000 and fills plane B with every map row it
 * shows; the engine keeps the plane A HUD band (clock, score) at the
 * picture's bottom corners. Never in the authoritative tick.
 *
 * STOPPAGES. From the whistle to the faceoff the game's logic does not reach
 * setvideo for ~32 ticks (forceblack's fade, then the faceoff set-up); the
 * views then present their last complete picture, faded with the live CRAM
 * (HOLD, before nhl94_view_geometry), instead of the game's native frame.
 *
 * NAME TAGS (nhl94_tags.c). Two read-only captures hang off the same
 * $007998 site: after apply_view in a view's speculative tick (the view's
 * own camera and mirror), and on the authoritative tick (host statics only,
 * nothing written to the guest). The tags are drawn onto the presented image
 * by GameSpec.present_overlay.
 *
 * PLAYER LOCK (nhl94_seats.c). With a seat list and none of NHL94_VIEWS /
 * NHL94_VIEW / --views @list given, the process shows ONE view: its own
 * seat's player (nhl94_seats_local_seat: the netplay local player online,
 * NHL94_LOCAL_SEAT offline, default seat 0); NHL94_VIEW=off disables it.
 * Host toggle, presentation only (never an input, never on the wire):
 * keyboard G or a controller's left-stick click (L3, without Back) switches
 * every view between following its player and following the puck (the
 * orientation stays the team's). NHL94_VIEW_FOLLOW=puck starts in puck mode.
 */
#include "nhl94_view.h"
#include "nhl94_ram.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "genesis_runtime.h"
#include "multiview.h"
#include "nhl94_seats.h"
#include "nhl94_tags.h"
#include "video/genesis_dac.h"
#include "video/genesis_machine.h"

/* ---- ROM / RAM facts used here (see nhl94_ram.h / VIEW_RESEARCH.md) ---- */
#define HOOK_SETVIDEO_JMP   0x007998u   /* DoGameFrame: jmp setvideo */
#define HOOK_SETVIDEO_DFOK  0x015F28u   /* setvideo: bset #dfok (list complete) */
#define HOOK_VBL_DUMP       0x015DB2u   /* VBlank: bsr DumpSprites (dfok was set) */
#define HOOK_FIND3D         0x016920u   /* find3d (video94.asm:987): REPLACED in zoomed views */
#define HOOK_CHECKSSO       0x016480u   /* checksso (video94.asm:595): SKIPPED in zoomed views */
#define OSFLAG              0x4E20
#define HUD_SPLIT           184         /* plane A rows from here on: clock / score boxes */      /* find3d: off screen (ram94.asm:73) */
#define SFLAGS_HOR_B        0x80        /* sfhor, high byte of sflags */
#define RINKVRCSET          0xFFB018u   /* rinkvrcset (ram94.asm:303) */
#define VMMAP2              0xFFB008u   /* VmMAP2: plane B name table ($C000) */
#define ROM_RINKTILELIST    0x05605Au   /* Rinktilelist (jim: +4 = map offset) */
#define ROM_REVRINKTILELIST 0x0B5180u   /* RevRinkTilelist */
#define ROM_RINKTILES       0x056062u   /* Rinktilelist+8 (setupice) */
#define ROM_REVRINKTILES    0x0B5188u   /* RevRinkTilelist+8 (graphics94.asm:124) */
#define LOGO_TILES          0x18        /* LoadHomeTeamGfx: d4 - $18 */
#define OLDROW_REDRAW       0x07D0
#define SFLAGS_HOR_W        0x8000      /* sfhor: horizontal scoreboard rink */
#define SFLAGS_PAUSE_W      0x0100
#define SFLAGS2_REPLAY_W    0x0800
#define SFLAGS4_REV_W       0x1000      /* bit 4 of the high byte */
#define SPF_PUCK            394         /* frames94.asm:40 */
#define SPF_FIGHT           354
#define SPF_PEN             377
#define SPF_BGLASS          653
#define FRAME_LIMIT         0x34E
#define ROLE_PUCK           (-1)

typedef struct {
    int team;       /* 0 home, 1 away */
    int role;       /* NHL94_POS_* or ROLE_PUCK */
    int fixed;      /* 1 = never turn */
    int useseat;    /* the seat-derived view: follow the seat's CONTROLLED object (substitute, PS) */
    int seat;
} ViewCfg;

typedef struct {
    int    have;            /* camera initialised */
    double cx, cy;          /* camera centre in the view's (oriented) coords */
    int    flip;            /* orientation of the last frame */
    int    fallback;        /* last frame used the puck fallback */
    unsigned fallback_logs;
    int    shown;           /* the previous round presented the view's own picture (live or held) */
} ViewState;

/* The view's last complete picture (see HOLD below). */
typedef struct {
    uint32_t *px;
    size_t    cap;
    int       w, h;
    uint16_t  cram[64];     /* CRAM it was drawn with */
    int       valid;
    int       n;            /* rounds held since */
} HeldImage;

static ViewCfg   s_cfg[GENESIS_MULTIVIEW_MAX];
static int       s_ncfg;
static int       s_parsed;
static int       s_listed;       /* the list came from NHL94_VIEWS / --views */
static ViewState s_st[GENESIS_MULTIVIEW_MAX];
static int       s_cur = -1;     /* view being rendered */
static int       s_applied;      /* hook already fired for s_cur */
static int       s_changed;      /* ... and changed the clone (camera/mirror) */
static int       s_built_sub;    /* sub-tick whose setvideo completed the view's list */
static int       s_upload_sub;   /* sub-tick whose vblank uploaded it (DumpSprites) */
static int       s_round_good;   /* this round's grabbed frame is the view's own, complete */
static int       s_hold;         /* this round presents the held picture (HOLD) */
/* What apply_view put in the clone, re-applied every time the game reaches
 * setvideo again inside the same view's speculative ticks (see hook). */
static int       s_app_hpos, s_app_vpos, s_app_flip;
static uint16_t  s_app_pcb;
static int       s_trace = -1;
static int       s_lookahead = 16;
static int       s_dma_redraw;
static int       s_marker = 1;   /* NHL94_VIEW_MARKER=0: no active-player star */   /* NHL94_VIEW_DMA_REDRAW=1: Oldrow=$7D0 instead */
static double    s_smooth = 0.35;
static int       s_framing = 1;   /* NHL94_VIEW_FRAMING=0: the old centred camera */
static double    s_anchor = 0.20; /* NHL94_VIEW_ANCHOR: own player this fraction of H below centre */
static double    s_puckbias = 0.45; /* NHL94_VIEW_PUCKBIAS: lean toward the puck (0..1) */
static double    s_deadzone = 8;  /* NHL94_VIEW_DEADZONE: px (x uses 3x) */
static int       s_extra_cols;    /* zoom: extra columns per side (B1) */
static int       s_zoom_cfg = -1; /* NHL94_VIEW_ZOOM in quarter steps (4 = 1x); -1 = default */
static int       s_zoom_q = 4;    /* current zoom, quarter steps: 4, 5, 6, 8 */
static int       s_zoom_env;      /* NHL94_VIEW_ZOOM given */
static int       s_zoom_setting;  /* launcher "View zoom", quarter steps; 0 = default */
static int       s_extra_rows;    /* zoom: extra rows per side (B2) */
static int       s_seatview;     /* the one view follows this process's seat */
static int       s_follow_puck;  /* host toggle: views follow the puck */
static uint16_t  s_revtbl[0x400];
static int       s_revtbl_ok;

static const char *s_role_names[] = { "G", "LD", "RD", "LW", "C", "RW", "X" };

static const ViewCfg s_default_cfg[] = {
    { 0, NHL94_POS_G, 0 }, { 1, NHL94_POS_G, 0 },
    { 0, NHL94_POS_C, 0 }, { 1, NHL94_POS_C, 0 },
    { 0, NHL94_POS_RD, 0 }, { 1, NHL94_POS_RD, 0 },
};

static int parse_role(const char *s, size_t n)
{
    if (n == 4 && !strncmp(s, "puck", 4)) return ROLE_PUCK;
    for (int i = 0; i < 7; i++)
        if (strlen(s_role_names[i]) == n && !strncmp(s, s_role_names[i], n)) return i;
    return -2;
}

/* "home:G,away:C:fixed" -> s_cfg. 0 entries on a malformed list. */
static int parse_list(const char *list)
{
    ViewCfg tmp[GENESIS_MULTIVIEW_MAX];
    int n = 0;
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char item[64];
        if (len == 0 || len >= sizeof item || n >= GENESIS_MULTIVIEW_MAX) return 0;
        memcpy(item, p, len); item[len] = 0;
        char *f1 = item, *f2 = strchr(f1, ':');
        if (!f2) return 0;
        *f2++ = 0;
        char *f3 = strchr(f2, ':');
        if (f3) *f3++ = 0;
        ViewCfg c = { 0, 0, 0 };
        if (!strcmp(f1, "home")) c.team = 0;
        else if (!strcmp(f1, "away")) c.team = 1;
        else return 0;
        c.role = parse_role(f2, strlen(f2));
        if (c.role < ROLE_PUCK) return 0;
        if (f3) {
            if (!strcmp(f3, "fixed")) c.fixed = 1;
            else if (strcmp(f3, "up")) return 0;
        }
        tmp[n++] = c;
        if (!e) break;
        p = e + 1;
    }
    memcpy(s_cfg, tmp, sizeof tmp[0] * (size_t)n);
    return n;
}

static void load_config(void)
{
    if (s_parsed) return;
    s_parsed = 1;
    const char *t = getenv("NHL94_VIEW_TRACE");
    s_trace = t && atoi(t) > 0;
    const char *la = getenv("NHL94_VIEW_LOOKAHEAD");
    if (la && *la) s_lookahead = atoi(la);
    const char *mk = getenv("NHL94_VIEW_MARKER");
    if (mk && *mk) s_marker = atoi(mk) != 0;
    const char *dr = getenv("NHL94_VIEW_DMA_REDRAW");
    s_dma_redraw = dr && atoi(dr) > 0;
    const char *sm = getenv("NHL94_VIEW_SMOOTH");
    if (sm && *sm) { s_smooth = atof(sm); if (s_smooth <= 0 || s_smooth > 1) s_smooth = 1; }
    else s_smooth = 0.20;   /* with the dead zone (framing); the old default was 0.35 */
    const char *fr = getenv("NHL94_VIEW_FRAMING");
    if (fr && *fr) s_framing = atoi(fr) != 0;
    if (!s_framing && !(sm && *sm)) s_smooth = 0.35;
    const char *an = getenv("NHL94_VIEW_ANCHOR");
    if (an && *an) s_anchor = atof(an);
    const char *pbz = getenv("NHL94_VIEW_PUCKBIAS");
    if (pbz && *pbz) s_puckbias = atof(pbz);
    const char *zm = getenv("NHL94_VIEW_ZOOM");
    s_zoom_env = zm && *zm;
    if (zm && *zm) { double z = atof(zm); s_zoom_cfg = z >= 1.0 && z <= 2.0 ? (int)lround(z * 4) : 4; }
    const char *dz = getenv("NHL94_VIEW_DEADZONE");
    if (dz && *dz) s_deadzone = atof(dz);
    const char *list = genesis_multiview_game_arg();
    const char *src = "--views";
    if (!list) { list = getenv("NHL94_VIEWS"); src = "NHL94_VIEWS"; }
    /* NHL94_VIEW=<team>:<role>: one client's own view (a one-entry list),
     * as tools/sixview_localhost.sh passes it to each process. */
    if (!list || !*list) { list = getenv("NHL94_VIEW"); src = "NHL94_VIEW"; }
    const char *fol = getenv("NHL94_VIEW_FOLLOW");
    s_follow_puck = fol && !strcmp(fol, "puck");
    int off = list && !strcmp(list, "off");
    if (off) {
        list = NULL;   /* no views, also not the seat's */
    } else if ((!list || !*list) && nhl94_seats_count() > 0) {
        /* Player lock: this process's own view follows its seat (resolved
         * per frame, see seat_cfg). */
        s_seatview = 1;
        s_listed = 1;
        s_ncfg = 1;
        s_cfg[0] = s_default_cfg[2];
    }
    if (!s_seatview && list && *list) {
        s_ncfg = parse_list(list);
        s_listed = s_ncfg > 0;
        if (!s_ncfg) fprintf(stderr, "nhl94_view: malformed %s=\"%s\" -- using the default list\n", src, list);
    }
    if (!s_ncfg) {
        s_ncfg = (int)(sizeof s_default_cfg / sizeof s_default_cfg[0]);
        memcpy(s_cfg, s_default_cfg, sizeof s_default_cfg);
    }
    /* Zoom (B1/B2): position lock's own view defaults to 1.5x; test view
     * lists and spectators stay native unless NHL94_VIEW_ZOOM says so. */
    s_zoom_q = s_zoom_cfg >= 0 ? s_zoom_cfg : s_zoom_setting ? s_zoom_setting : (s_seatview ? 6 : 4);
    for (int i = 0; i < 0x400; i++)
        s_revtbl[i] = (uint16_t)((g_rom[NHL94_ROM_REVFRAMETBL + 2u * i] << 8) |
                                 g_rom[NHL94_ROM_REVFRAMETBL + 2u * i + 1]);
    s_revtbl_ok = 1;
    if (off)
        fprintf(stderr, "nhl94_view: views off (NHL94_VIEW=off)\n");
    else if (s_seatview)
        fprintf(stderr, "nhl94_view: 1 view: own seat (local seat %d), toggle player/puck: G or L3\n",
                nhl94_seats_local_seat());
    else if (!s_listed && !getenv("GENESIS_VIEWS"))
        /* Nothing asked for views: the default list below is only what
         * GENESIS_VIEWS=n would show. Don't print it as if it were on. */
        fprintf(stderr, "nhl94_view: no views (normal screen); GENESIS_VIEWS=n or "
                        "NHL94_VIEWS=... turn on the per-player test views\n");
    else {
        fprintf(stderr, "nhl94_view: %d view(s):", s_ncfg);
        for (int i = 0; i < s_ncfg; i++)
            fprintf(stderr, " %s:%s%s", s_cfg[i].team ? "away" : "home",
                    s_cfg[i].role == ROLE_PUCK ? "puck" : s_role_names[s_cfg[i].role],
                    s_cfg[i].fixed ? ":fixed" : "");
        fprintf(stderr, "\n");
    }
    for (int i = 0; i < s_ncfg; i++) {   /* window-grid captions, e.g. "HOME C" */
        char lbl[32];
        snprintf(lbl, sizeof lbl, "%s %s", s_cfg[i].team ? "AWAY" : "HOME",
                 s_cfg[i].role == ROLE_PUCK ? "PUCK" : s_role_names[s_cfg[i].role]);
        genesis_multiview_set_label(i, lbl);
    }
}

/* Views are on only when asked for: GENESIS_VIEWS / --views <n> (default
 * list), or an explicit list (its length). */
int nhl94_view_default_count(void)
{
    load_config();
    return s_listed ? s_ncfg : 0;
}

/* The seat-derived view's config this frame (the local seat can change when
 * a netplay session starts after the views were configured). */
static void seat_cfg(void)
{
    int team = 0, role = NHL94_POS_C;
    int seat = nhl94_seats_local_seat();
    ViewCfg c = { 0, ROLE_PUCK, 0 };              /* spectator: the puck */
    if (nhl94_seats_get(seat, &team, &role)) { c.team = team; c.role = role; c.useseat = 1; c.seat = seat; }
    s_cfg[0] = c;
}

static void set_labels(void)
{
    for (int i = 0; i < s_ncfg; i++) {
        char lbl[40];
        snprintf(lbl, sizeof lbl, "%s %s%s", s_cfg[i].team ? "AWAY" : "HOME",
                 s_cfg[i].role == ROLE_PUCK ? "PUCK" : s_role_names[s_cfg[i].role],
                 s_follow_puck && s_cfg[i].role != ROLE_PUCK ? " (PUCK)" : "");
        genesis_multiview_set_label(i, lbl);
    }
}

/* Host-only camera toggle: keyboard G or L3 on any open controller (L3
 * without Back: Back+L3 belongs to the runtime menu). Edge-triggered, polled
 * once per presented tick. Never reaches the simulation. */
static void poll_follow_toggle(void)
{
    static int prev = 1;   /* swallow a key held at start-up */
    int down = 0;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    if (ks && ks[SDL_SCANCODE_G]) down = 1;
    for (int j = 0, n = SDL_NumJoysticks(); j < n && !down; j++) {
        if (!SDL_IsGameController(j)) continue;
        SDL_GameController *gc = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(j));
        if (gc && SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK) &&
            !SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))
            down = 1;
    }
    if (down && !prev) {
        s_follow_puck = !s_follow_puck;
        fprintf(stderr, "nhl94_view: camera follows %s\n", s_follow_puck ? "the puck" : "the player");
        set_labels();
    }
    prev = down;
}

/* Host-only zoom cycle: keyboard F or a controller's right-stick click (R3,
 * without Back) steps 1x -> 1.25x -> 1.5x -> 2x -> 1x. Edge-triggered,
 * polled once per presented tick; never an input, never on the wire. The
 * spectator rule (native unless asked) ends once the player picks a zoom. */
static int view_zoom(void);
static void poll_zoom_toggle(void)
{
    static int prev = 1;
    int down = 0;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    if (ks && ks[SDL_SCANCODE_F]) down = 1;
    for (int j = 0, n = SDL_NumJoysticks(); j < n && !down; j++) {
        if (!SDL_IsGameController(j)) continue;
        SDL_GameController *gc = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(j));
        if (gc && SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK) &&
            !SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))
            down = 1;
    }
    /* the launcher / runtime menu changed "View zoom" (no env override) */
    static int seen;
    if (!s_zoom_env && s_zoom_setting && s_zoom_setting != seen) {
        if (seen) { s_zoom_q = s_zoom_setting; s_zoom_cfg = s_zoom_q; }
        seen = s_zoom_setting;
    }
    if (down && !prev) {
        int q = view_zoom();
        s_zoom_q = q == 4 ? 5 : q == 5 ? 6 : q == 6 ? 8 : 4;
        s_zoom_cfg = s_zoom_q;   /* an explicit choice: spectators too */
        fprintf(stderr, "nhl94_view: zoom %s\n", s_zoom_q == 4 ? "1x" : s_zoom_q == 5 ? "1.25x" :
                s_zoom_q == 6 ? "1.5x" : "2x");
    }
    prev = down;
}

void nhl94_view_prepare(int view_index, void *user)
{
    (void)user;
    load_config();
    if (view_index == 0) {
        poll_follow_toggle();
        poll_zoom_toggle();
        if (s_seatview) {
            ViewCfg before = s_cfg[0];
            seat_cfg();
            if (before.team != s_cfg[0].team || before.role != s_cfg[0].role) set_labels();
        }
    }
    s_cur = view_index;
    s_round_good = 0;
    s_hold = 0;
    nhl94_tags_view_begin(view_index);
    s_applied = 0;
    s_changed = 0;
    s_built_sub = -1;
    s_upload_sub = -1;
    /* Nothing is mutated here: the clone is changed at the hook, after this
     * frame's logic, so the logic never sees a view's camera or mirror. */
}

/* Plane A (HUD: clock, score, ref) is not scrolled and only its columns
 * 0-31 are on a 256-wide screen; a wider view also samples columns 32-63,
 * which hold leftovers. Blank them in the clone (tile $7FF, the blank char
 * the game fills plane A with). */
static uint16_t rd16_early(uint32_t a);
/* Extra rows (B2): the 32-row plane B ring covers only 256 lines, so a
 * taller view's clone switches both planes to 64x64 cells (VDP register 16)
 * and moves the sprite table out of plane B's second half: Satt from $DC00
 * to $F000 (the window name table; the window is off in game), through
 * VSPRITES (DumpSprites' DMA target) and register 5. Plane A's rows 32-63
 * then alias $F000-$FFFF, but the engine does not draw plane A on the
 * virtual rows and the screen's rows use plane A rows 0-27 only.
 * 0 when the layout is not the in-game one (then no extra rows). */
#define VSPRITES_RAM 0xFFB002u
#define SATT_GAME    0xDC00u
#define SATT_ZOOM    0xF000u
static int set_tall_planes(void)
{
    uint16_t vs = rd16_early(VSPRITES_RAM);
    if (vs != SATT_GAME && vs != SATT_ZOOM) return 0;
    if (rd16_early(0xFFB008u) != 0xC000u) return 0;   /* VmMAP2: plane B */
    glue_poke16(VSPRITES_RAM, SATT_ZOOM);
    g_machine.vdp.reg[0x05] = (uint8_t)(SATT_ZOOM >> 9);
    g_machine.vdp.reg[0x10] = 0x11;                    /* 64x64 cells */
    return 1;
}
#define VMMAP1 0xFFB004u   /* VmMAP1: plane A name table ($E000) */
static void blank_plane_a_margins(void)
{
    uint8_t *vram = g_machine.vdp.vram;
    uint16_t plane = rd16_early(VMMAP1);
    for (unsigned r = 0; r < 32; r++)
        for (unsigned c = 32; c < 64; c++) {
            uint32_t a = (uint32_t)(plane + r * 128u + c * 2u) & 0xFFFEu;
            vram[a] = 0x07; vram[a + 1] = 0xFF;
        }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- guest access (host-side: never a bus access) ---- */
static inline uint16_t rd16(uint32_t a) { return glue_peek16(a); }
static inline int16_t  rds16(uint32_t a) { return (int16_t)glue_peek16(a); }
static inline void     wr16(uint32_t a, uint16_t v) { glue_poke16(a, v); }
static uint16_t rd16_early(uint32_t a) { return glue_peek16(a); }

static int find_target(const ViewCfg *c, int *fell_back)
{
    *fell_back = 0;
    if (c->role == ROLE_PUCK) return NHL94_SC_PUCK;
    /* Position lock: the camera follows the body the seat controls right now (a substitute while
     * its role has no body, the shooter / goalie of a penalty shot), then the role as before. */
    if (c->useseat) {
        int o = nhl94_seats_control_object(c->seat);
        if (o >= 0 && o < 12) return o;
    }
    unsigned first = c->team ? NHL94_SC_AWAY_FIRST : NHL94_SC_HOME_FIRST;
    for (unsigned i = first; i < first + 6; i++) {
        int16_t pos = rds16(NHL94_SC_ADDR(i) + NHL94_OFF_POSITION);
        if (pos == c->role) return (int)i;
    }
    /* 4v4 (guest FMT, nhl94_format.c): no RD / RW at even strength, so an
     * RD / RW view (e.g. the GENESIS_VIEWS=6 default) follows LD / LW. */
    if (rd16(NHL94_FMT) == NHL94_FMT_4V4 && (c->role == NHL94_POS_RD || c->role == NHL94_POS_RW)) {
        int alt = c->role == NHL94_POS_RD ? NHL94_POS_LD : NHL94_POS_LW;
        for (unsigned i = first; i < first + 6; i++)
            if (rds16(NHL94_SC_ADDR(i) + NHL94_OFF_POSITION) == alt) return (int)i;
    }
    *fell_back = 1;
    return NHL94_SC_PUCK;
}

/* 1 when `team` shoots at the top goal this period: pflags bit 7 (pfgoal) of
 * any of its on-ice objects, else gmode gmdir (clear = home attacks up). */
static int team_attacks_up(int team)
{
    unsigned first = team ? NHL94_SC_AWAY_FIRST : NHL94_SC_HOME_FIRST;
    for (unsigned i = first; i < first + 6; i++) {
        uint32_t b = NHL94_SC_ADDR(i);
        if (rds16(b + NHL94_OFF_POSITION) < 0) continue;
        return (glue_peek8(b + NHL94_OFF_PFLAGS) & NHL94_PF_GOAL) != 0;
    }
    int home_up = !(rd16(NHL94_GMODE) & NHL94_GMODE_DIR_W);
    return team ? !home_up : home_up;
}

/* SprSort (setup94.asm:561): bubble passes over OOlist keyed on Ypos,
 * keeping OOlistpos in step. Byte-for-byte the game's algorithm. */
static void spr_sort(void)
{
    int16_t key[16];
    for (unsigned i = 0; i < 16; i++) {
        key[i] = rds16(NHL94_SC_ADDR(i) + NHL94_OFF_YPOS);
        wr16(NHL94_YLIST + 2u * i, (uint16_t)key[i]);
    }
    int swapped;
    do {
        swapped = 0;
        for (unsigned j = 0; j < 15; j++) {
            uint8_t a = glue_peek8(NHL94_OOLIST + j), b = glue_peek8(NHL94_OOLIST + j + 1);
            if (key[(a >> 1) & 15] > key[(b >> 1) & 15]) {
                glue_poke8(NHL94_OOLIST + j + 1, a);
                glue_poke8(NHL94_OOLIST + j, b);
                wr16(NHL94_OOLISTPOS + a, (uint16_t)(j + 1));
                wr16(NHL94_OOLISTPOS + b, (uint16_t)j);
                swapped = 1;
            }
        }
    } while (swapped);
}

static uint16_t rom16(uint32_t a) { return (uint16_t)((g_rom[a] << 8) | g_rom[a + 1]); }

/* What ReloadRinkGraphics does for a reverse angle, straight into the
 * clone's VRAM: RevRinkTiles at rinkvrcset, then the home logo (which
 * LoadHomeTeamGfx puts $18 tiles before the end of the rink set) moved to
 * its reverse-set place. Both tile sets are stored unpacked in this ROM
 * (count word without bit 15), so this is a copy. 0 if the data is not as
 * expected (then the view stays unmirrored). */
static int load_reverse_rink_tiles(void)
{
    uint16_t nn = rom16(ROM_RINKTILES), nr = rom16(ROM_REVRINKTILES);
    if ((nn & 0x8000u) || (nr & 0x8000u) || nn < LOGO_TILES || nr < LOGO_TILES) return 0;
    uint32_t base = (uint32_t)rd16(RINKVRCSET) * 32u;
    uint32_t logo_src = base + (uint32_t)(nn - LOGO_TILES) * 32u;
    uint32_t logo_dst = base + (uint32_t)(nr - LOGO_TILES) * 32u;
    uint32_t end = base + (uint32_t)(nn > nr ? nn : nr) * 32u;
    if (end > GVDP_VRAM_SIZE) return 0;
    uint8_t logo[LOGO_TILES * 32];
    uint8_t *vram = g_machine.vdp.vram;
    memcpy(logo, vram + logo_src, sizeof logo);
    memcpy(vram + base, g_rom + ROM_REVRINKTILES + 2u, (size_t)nr * 32u);
    memcpy(vram + logo_dst, logo, sizeof logo);
    return 1;
}

/* RestoreReplayFrame's reverse-angle transform (replay94.asm:456-618) on the
 * live objects. Velocities and the AI state are left alone: nothing that
 * reads them runs before the grabbed frame is displayed. */
static void mirror_world(void)
{
    for (unsigned n = 0; n < NHL94_SC_COUNT; n++) {
        uint32_t b = NHL94_SC_ADDR(n);
        wr16(b + NHL94_OFF_XPOS, (uint16_t)-rds16(b + NHL94_OFF_XPOS));
        int16_t y = (int16_t)-rds16(b + NHL94_OFF_YPOS);
        if (n == NHL94_SC_PUCK_SHADOW) y = (int16_t)(y + 2);
        wr16(b + NHL94_OFF_YPOS, (uint16_t)y);
        uint16_t fw = rd16(b + NHL94_OFF_FRAME);
        unsigned num = fw & 0x7FFu;
        if (num < 0x400u) {
            unsigned r = s_revtbl[num];
            if (n == NHL94_SC_PUCK_SHADOW && r != SPF_PUCK)
                wr16(b + NHL94_OFF_YPOS, 0xFC00u);
            if (r >= 1 && r < FRAME_LIMIT) num = r;
        }
        wr16(b + NHL94_OFF_FRAME, (uint16_t)((fw & 0xF800u) | num));
        if (num >= SPF_BGLASS && num < SPF_BGLASS + 5)
            wr16(b + NHL94_OFF_YPOS, 0x0190u);
        if (num >= SPF_FIGHT && num < SPF_PEN)
            wr16(b + NHL94_OFF_ATTRIBUTE, (uint16_t)(rd16(b + NHL94_OFF_ATTRIBUTE) ^ 0x0800u));
    }
    wr16(NHL94_SFLAGS4, (uint16_t)(rd16(NHL94_SFLAGS4) | SFLAGS4_REV_W));
    spr_sort();
}

/* updatescroll's full redraw (video94.asm:133, the .su0 path taken with
 * Oldrow = $7D0), done straight into the clone's plane B: the 32 map rows
 * around Vpos into the 32-row ring, from Rinktilelist or RevRinkTilelist.
 * Then Oldrow = Vpos>>3, so setvideo queues no rows. Why not let the game
 * queue them: 32 rows are 1536 DMA words on top of a mirrored view's sprite
 * tiles; the vblank handler (DMA stalls charged to the 68K) then reaches
 * the last row transfers after the next frame's raster has passed the top
 * rows, which showed stale rows at the top of the view. Same words, same
 * VRAM addresses as the game's own redraw. */
/* The tall plane (64x64): every map row the virtual window shows. Screen
 * line L shows map row m = 32 + ((L - Vpos) >> 3) (the game's ring puts map
 * row m at ring row m & 31; VSRAM = -Vpos), so in a 64-row plane map row m
 * goes to plane row (m + 32) & 63. Lines -rows .. 223 + rows. */
static void redraw_rink_plane_tall(int vpos, int rev, int rows)
{
    uint32_t list = rev ? ROM_REVRINKTILELIST : ROM_RINKTILELIST;
    uint32_t map = list + (((uint32_t)rom16(list + 4) << 16) | rom16(list + 6));
    unsigned cols = rom16(map), nrows = rom16(map + 2);
    uint32_t data = map + 4;
    uint16_t plane = rd16(VMMAP2);
    uint8_t *vram = g_machine.vdp.vram;
    if (cols > 64) return;
    int m0 = 32 + ((-rows - 8 - vpos) >> 3), m1 = 32 + ((224 + rows + 8 - vpos) >> 3);
    for (int m = m0; m <= m1; m++) {
        uint32_t dst = (uint16_t)(plane + (((unsigned)(m + 32) & 63u) << 7));
        int inmap = m >= 0 && (unsigned)m < nrows;
        uint32_t src = data + (uint32_t)(inmap ? m : 0) * cols * 2u;
        for (unsigned c = 0; c < 64; c++) {
            uint32_t a = (dst + 2u * c) & 0xFFFEu;
            if (inmap && c < cols) { vram[a] = g_rom[src + 2u * c]; vram[a + 1] = g_rom[src + 2u * c + 1]; }
            else { vram[a] = 0x07; vram[a + 1] = 0xFF; }
        }
    }
    wr16(NHL94_OLDROW, (uint16_t)(int16_t)((int16_t)vpos >> 3));
}

static void redraw_rink_plane(int vpos, int rev)
{
    if (s_extra_rows) { redraw_rink_plane_tall(vpos, rev, s_extra_rows); return; }
    uint32_t list = rev ? ROM_REVRINKTILELIST : ROM_RINKTILELIST;
    uint32_t map = list + (((uint32_t)rom16(list + 4) << 16) | rom16(list + 6));
    unsigned cols = rom16(map), rows = rom16(map + 2);
    uint32_t data = map + 4;
    uint16_t plane = rd16(VMMAP2);
    int d1 = (int16_t)vpos >> 3;
    uint8_t *vram = g_machine.vdp.vram;
    for (int i = 0; i < 32; i++, d1++) {
        int row = 0x3D - d1;
        if (row < 0 || (unsigned)row >= rows || cols > 64) continue;
        uint32_t dst = (uint16_t)(((((-d1) + 0x1D) & 0x1F) << 7) + plane);
        uint32_t src = data + (uint32_t)row * cols * 2u;
        for (unsigned c = 0; c < cols; c++) {
            uint32_t a = (dst + 2u * c) & 0xFFFEu;
            vram[a] = g_rom[src + 2u * c];
            vram[a + 1] = g_rom[src + 2u * c + 1];
        }

    }
    wr16(NHL94_OLDROW, (uint16_t)(int16_t)((int16_t)vpos >> 3));
}


/* ---- zoom (presentation only) ----
 * Quarter steps: 4 = 1x (256x224), 5 = 1.25x (320x280), 6 = 1.5x (384x336),
 * 8 = 2x (384x448). Columns are the engine's per-view margin
 * (GameSpec.view_geometry); the game's find3d cull is widened to match in
 * the view's clone only (HOOK_FIND3D). 384 px is the whole rink map. */
/* Columns stop at the rink map's width (48 cells = 384 px, reached at
 * 1.5x): past it there is no arena art, only the plane's wrapped copy. 2x
 * therefore widens no further and only adds rows (384x448). */
static int zoom_cols(int q) { int c = q <= 4 ? 0 : (256 * q / 4 - 256) / 2; return c > 64 ? 64 : c; }
static int zoom_rows(int q) { return q <= 4 ? 0 : (224 * q / 4 - 224) / 2; }

/* Launcher setting (nhl94_mode.c "View zoom"), quarter steps; 0 = the
 * default (1.5x for position lock). NHL94_VIEW_ZOOM wins over it. */
void nhl94_view_set_zoom_setting(int q) { s_zoom_setting = (q == 4 || q == 5 || q == 6 || q == 8) ? q : 0; }
int  nhl94_view_zoom_setting(void) { return s_zoom_setting; }

/* The zoom of the running view: spectators of a seat list (no own player)
 * stay native unless NHL94_VIEW_ZOOM was given. */
static int view_zoom(void)
{
    if (s_zoom_cfg < 0 && s_seatview && s_cfg[0].role == ROLE_PUCK) return 4;
    return s_zoom_q;
}

/* ---- faceoff close-up box in a zoomed picture (presentation only) ----
 * puckfaceoff2 (logic94.asm:6176) prints the box on plane A at print
 * column (fodropx - 46) / 8, row (fodropy - 68) / 8 (12 x 13 cells with
 * the line names; the eraser's size), beside the drop spot of the native
 * 256x224 screen. checkfo (video94.asm:230) puts the big faceoff sprites at
 * fodropx / fodropy as the FIRST entries of Satt. In a zoomed picture the
 * box is moved as one piece: the engine draws that plane A rectangle
 * shifted (gvdp_set_virtual_plane_a_box) and the view's own sprite list
 * gets the same shift (faceoff_sprites_shift at setvideo's bset #dfok, in
 * the clone only). Target: centred horizontally; vertically the box keeps
 * its centre at the same fraction of the picture height as at 1x. */
#define FODROPX       0xFFB032u   /* checkfo: add.w $B032.w */
#define FODROPY       0xFFB034u
#define FOFDATA2      0xFFBDA8u   /* 3 entries: word frame, word attr */
#define ROM_FACEOFFSPRITES 0x0A78AEu
#define DISFLAGS_FACEOFF_W 0x1000 /* dffaceoff: bit 4 of the high byte */
#define FOBOX_W       (12 * 8)
#define FOBOX_H       (13 * 8)
static int s_fobox_on, s_fobox_x0, s_fobox_y0, s_fobox_dx, s_fobox_dy;

static int faceoff_box_compute(void)
{
    s_fobox_on = 0;
    if (!s_changed || !s_extra_rows) return 0;   /* zoom always has rows; 1x: never */
    if (!(rd16(NHL94_DISFLAGS) & DISFLAGS_FACEOFF_W) || (rd16(NHL94_SFLAGS) & NHL94_SFLAGS_PAUSE_W)) return 0;
    int fx = (int16_t)rd16(FODROPX), fy = (int16_t)rd16(FODROPY);
    int x0 = ((fx - 46) >> 3) * 8, y0 = ((fy - 68) >> 3) * 8;
    if (x0 < 0 || y0 < 0 || x0 + FOBOX_W > 256 || y0 + FOBOX_H > HUD_SPLIT) return 0;
    int W = 256 + 2 * s_extra_cols, H = 224 + 2 * s_extra_rows;
    int tx = (W - FOBOX_W) / 2 - s_extra_cols;                           /* native coords */
    int ty = (y0 + FOBOX_H / 2) * H / 224 - FOBOX_H / 2 - s_extra_rows;
    s_fobox_x0 = x0; s_fobox_y0 = y0; s_fobox_dx = tx - x0; s_fobox_dy = ty - y0;
    s_fobox_on = 1;
    return 1;
}

/* The faceoff sprites checkfo just wrote: the first entries of Satt. Same
 * count as its loop (frame table of FaceOffSprites). */
static void faceoff_sprites_shift(void)
{
    if (!faceoff_box_compute()) return;
    uint32_t tab = ROM_FACEOFFSPRITES + (((uint32_t)rom16(ROM_FACEOFFSPRITES + 4) << 16) |
                                         rom16(ROM_FACEOFFSPRITES + 6));
    unsigned n = 0;
    for (unsigned i = 0; i < 3; i++) {
        int16_t f = (int16_t)rd16(FOFDATA2 + i * 4u);
        if (f <= 0) continue;
        n += (unsigned)(rom16(tab + 2u * (unsigned)f + 2u) - rom16(tab + 2u * (unsigned)f)) / 8u;
    }
    unsigned total = rd16(NHL94_SATTSIZE) / 4u;
    if (n > total) n = total;
    for (unsigned i = 0; i < n; i++) {
        uint32_t e = NHL94_SATT + i * 8u;
        wr16(e, (uint16_t)((rd16(e) + s_fobox_dy) & 0x3FF));
        wr16(e + 6, (uint16_t)((rd16(e + 6) + s_fobox_dx) & 0x3FF));
    }
}

/* ---- HOLD: the picture through a stoppage (presentation only) ----
 * From the whistle to the faceoff the game's frame logic does not reach
 * setvideo for about 32 ticks: puckfaceoff2's first call (logic94.asm:6005)
 * runs forceblack, a palette fade whose vblank handler (vb2) does no DMA,
 * then sets the faceoff up with interrupts masked. A view round in that
 * window has no picture of its own (the hook never runs, or ran but vb2
 * never uploaded the list): grabbing would present the game's native frame
 * (256x224, its camera, never turned), the "zoom snaps back" before every
 * faceoff. The real screen meanwhile is a frozen picture whose palette fades
 * to black. So such a round presents the view's last complete picture with
 * every colour mapped from the CRAM it was drawn with to the live CRAM:
 * the view fades exactly as the screen does. Only while vb2 runs the fade or
 * the screen is black under the game-play VBlank, and for at most HOLD_MAX
 * rounds; a canonical frame (replay, pause, menu) ends it. */
#define VBINT_RAM    0xFFB03Au   /* VBint (u32), nhl94_ram.h NHL94_VBINT */
#define VBL_GAME     0x015D9Au   /* VBlank (video94.asm:5) */
#define VBL_FADE     0x015E4Cu   /* vb2 (video94.asm:56): palette fade only, no DMA */
#define HOLD_MAX     90
#define HOLD_SHORT   4           /* rounds held under VBlank with a lit palette */
static HeldImage s_held[GENESIS_MULTIVIEW_MAX];
static uint16_t  s_cram_frame[64];   /* CRAM as the last sub-tick began: the grabbed frame's palette
                                       * (its vblank may already step a fade) */

static uint32_t vbint(void) { return ((uint32_t)rd16(VBINT_RAM) << 16) | rd16(VBINT_RAM + 2u); }

static int hold_candidate(int k)
{
    if (k < 0 || k >= GENESIS_MULTIVIEW_MAX) return 0;
    const HeldImage *hd = &s_held[k];
    if (!hd->valid || !s_st[k].shown || hd->n >= HOLD_MAX) return 0;
    /* vb2: the fade itself, the screen frozen. Under the game-play VBlank
     * nothing new reaches the screen either while setvideo is not reached
     * (no dfok, no DumpSprites): hold for a few rounds right after a live
     * picture (puckfaceoff2's short stall with interrupts masked, 1-2
     * ticks), and after that only while the screen is black (the faceoff
     * set-up after the fade). Any other screen that lights its palette (the
     * intermission menu) is shown as the game draws it. */
    uint32_t vb = vbint();
    if (vb == VBL_FADE) return 1;
    if (vb != VBL_GAME) return 0;
    if (hd->n < HOLD_SHORT) return 1;
    for (int i = 0; i < 64; i++)
        if (g_machine.vdp.cram[i] & 0x0EEEu) return 0;
    return 1;
}

static void hold_store(HeldImage *hd, const uint32_t *px, int w, int h, int stride)
{
    size_t need = (size_t)w * (size_t)h;
    if (hd->cap < need) {
        uint32_t *nb = (uint32_t *)realloc(hd->px, need * sizeof *nb);
        if (!nb) { hd->valid = 0; return; }
        hd->px = nb; hd->cap = need;
    }
    for (int y = 0; y < h; y++) memcpy(hd->px + (size_t)y * (size_t)w, px + (size_t)y * (size_t)stride, (size_t)w * sizeof *px);
    memcpy(hd->cram, s_cram_frame, sizeof hd->cram);
    hd->w = w; hd->h = h; hd->valid = 1; hd->n = 0;
    if (s_trace > 0) {
        static unsigned logged;
        if (logged++ < 4) {
            uint32_t pal[64];
            for (int i = 0; i < 64; i++) pal[i] = genesis_dac_cram_to_argb(hd->cram[i], 0);
            size_t miss = 0; uint32_t ex = 0;
            for (size_t i = 0; i < need; i++) {
                int f = 0;
                for (int j = 0; j < 64 && !f; j++) f = pal[j] == hd->px[i];
                if (!f) { miss++; ex = hd->px[i]; }
            }
            fprintf(stderr, "nhl94_view: hold store %dx%d unmatched=%zu/%zu e.g. %08X pal0=%08X pal1=%08X pal17=%08X\n",
                    w, h, miss, need, ex, pal[0], pal[1], pal[17]);
        }
    }
}

static void hold_paste(const HeldImage *hd, uint32_t *px, int stride)
{
    uint32_t from[3 * 64], to[3 * 64];
    for (int l = 0; l < 3; l++)
        for (int i = 0; i < 64; i++) {
            from[l * 64 + i] = genesis_dac_cram_to_argb(hd->cram[i], l);
            to[l * 64 + i] = genesis_dac_cram_to_argb(g_machine.vdp.cram[i], l);
        }
    uint32_t key = 0, val = 0;
    int have = 0;
    for (int y = 0; y < hd->h; y++) {
        const uint32_t *s = hd->px + (size_t)y * (size_t)hd->w;
        uint32_t *d = px + (size_t)y * (size_t)stride;
        for (int x = 0; x < hd->w; x++) {
            uint32_t p = s[x];
            if (!have || p != key) {
                key = p; val = p; have = 1;
                for (int j = 0; j < 3 * 64; j++) if (from[j] == p) { val = to[j]; break; }
            }
            d[x] = val;
        }
    }
}

/* GameSpec.present_overlay: the views' HOLD, then the name tags. */
void nhl94_view_present_overlay(uint32_t *pixels, int w, int h, int stride, int view_index)
{
    if (pixels && view_index >= 0 && view_index < GENESIS_MULTIVIEW_MAX && view_index == s_cur) {
        HeldImage *hd = &s_held[view_index];
        ViewState *st = &s_st[view_index];
        if (s_round_good) {
            hold_store(hd, pixels, w, h, stride);     /* before the tags are drawn */
            st->shown = 1;
        } else if ((s_hold || (s_changed && hold_candidate(view_index))) && hd->w == w && hd->h == h) {
            hold_paste(hd, pixels, stride);
            hd->n++;
            if (s_trace)
                fprintf(stderr, "nhl94_view: v%d hold %d (tick %u)\n", view_index, hd->n, genesis_sim_tick_count());
            return;   /* no tags on a held picture */
        } else {
            st->shown = 0;
        }
    }
    nhl94_tags_present_overlay(pixels, w, h, stride, view_index);
}

/* GameSpec.view_geometry: asked before every sub-tick of a view. Only a
 * frame the hook turned into the view's own picture is bigger, or one that
 * may present the held picture (its size). */
void nhl94_view_geometry(int view_index, void *user, int *extra_cols, int *extra_rows)
{
    (void)user;
    memcpy(s_cram_frame, g_machine.vdp.cram, sizeof s_cram_frame);
    *extra_cols = s_changed ? s_extra_cols : 0;
    *extra_rows = s_changed ? s_extra_rows : 0;
    if (!s_applied && genesis_multiview_view_tick() >= genesis_multiview_ticks_per_view() - 1 &&
        hold_candidate(view_index)) {
        *extra_cols = (s_held[view_index].w - 256) / 2;
        *extra_rows = (s_held[view_index].h - 224) / 2;
        if (*extra_cols < 0) *extra_cols = 0;
        if (*extra_rows < 0) *extra_rows = 0;
        gvdp_set_virtual_hud_anchor(0);
        gvdp_set_virtual_plane_a_box(0, 0, 0, 0, 0, 0);
        return;
    }
    /* plane A is the screen-fixed HUD: clock and score boxes to the corners */
    gvdp_set_virtual_hud_anchor(*extra_cols || *extra_rows ? HUD_SPLIT : 0);
    if (*extra_rows && faceoff_box_compute())
        gvdp_set_virtual_plane_a_box(s_fobox_x0, s_fobox_y0, FOBOX_W, FOBOX_H, s_fobox_dx, s_fobox_dy);
    else
        gvdp_set_virtual_plane_a_box(0, 0, 0, 0, 0, 0);
}

/* find3d (video94.asm:987) with the cull limits widened by the view's extra
 * columns / rows. Same arithmetic and register results as the routine:
 * word ops on d0/d1/d2 (upper words kept), d1 = OSFLAG when off screen. Its
 * only caller (addframe) saves d0-d2 and tests d1 only. The horizontal
 * scoreboard rink (sfhor) is left to the game. */
static int find3d_zoomed(void)
{
    if (glue_peek8(NHL94_SFLAGS) & SFLAGS_HOR_B) return 0;
    uint32_t D0 = g_cpu.D[0], D1 = g_cpu.D[1], D2 = g_cpu.D[2];
    int16_t d0 = (int16_t)((int16_t)D0 - rds16(NHL94_HPOS));
    int16_t d1 = (int16_t)((int16_t)D1 - rds16(NHL94_VPOS));
    int16_t d2 = (int16_t)D2;
    int lx = 0x90 + s_extra_cols, ly = 0x90 + s_extra_rows;
    if (d0 > lx || d0 < -lx) {
        g_cpu.D[0] = (D0 & 0xFFFF0000u) | (uint16_t)d0;
        g_cpu.D[1] = (D1 & 0xFFFF0000u) | (uint16_t)OSFLAG;
        return 1;
    }
    d0 = (int16_t)(d0 + 0x100);
    d1 = (int16_t)(d1 + d2);
    d2 = (int16_t)(d2 >> 1);
    d1 = (int16_t)(d1 + d2);
    g_cpu.D[0] = (D0 & 0xFFFF0000u) | (uint16_t)d0;
    g_cpu.D[2] = (D2 & 0xFFFF0000u) | (uint16_t)d2;
    if (d1 > ly || d1 < -ly) {
        g_cpu.D[1] = (D1 & 0xFFFF0000u) | (uint16_t)OSFLAG;
        return 1;
    }
    d1 = (int16_t)(0xF0 - d1);
    g_cpu.D[1] = (D1 & 0xFFFF0000u) | (uint16_t)d1;
    return 1;
}

/* ---- camera framing (presentation only: host statics + the clone's
 * Hpos/Vpos). Oriented coordinates: +y = the view team's attack direction.
 * The view's own player sits `anchor` below the centre (more ice ahead), the
 * camera leans toward the puck by `puckbias`, leads the skating direction by
 * Yvel/256, and never lets the own player leave the band [18%, 90%] of the
 * view height from the top. A dead zone and an exponential follow keep it
 * calm; it snaps on a flip or a jump past 160 px. Screen row of a world y:
 * H/2 + (camera_y - y), so the band is camera_y - own_y in
 * [-0.32 H, +0.40 H]. */
static int view_height(void) { return 224 + 2 * s_extra_rows; }
static int view_width(void)  { return 256 + 2 * s_extra_cols; }

static void frame_camera(ViewState *st, const ViewCfg *c, const ViewCfg *follow, int sc,
                         int flip, int sgn, int *hpos_out, int *vpos_out)
{
    const int H = view_height(), W = view_width();
    uint32_t b = NHL94_SC_ADDR(sc);
    double ox = sgn * rds16(b + NHL94_OFF_XPOS), oy = sgn * rds16(b + NHL94_OFF_YPOS);
    double tx = ox, ty = oy;
    if (!c->fixed && s_framing) {
        uint32_t pb = NHL94_SC_ADDR(NHL94_SC_PUCK);
        double px = sgn * rds16(pb + NHL94_OFF_XPOS), py = sgn * rds16(pb + NHL94_OFF_YPOS);
        int puckcam = follow->role == ROLE_PUCK;
        int goalie = !puckcam && c->role == NHL94_POS_G;
        double anchor = (goalie ? 0.33 : s_anchor) * H;
        double w = puckcam ? 0.0 : (goalie ? 0.30 : s_puckbias);
        double lead = puckcam ? 0.0 : sgn * rds16(b + NHL94_OFF_YVEL) / 256.0;
        if (lead > 40) lead = 40; else if (lead < -40) lead = -40;
        double dpy = py - oy;
        if (goalie && py > -88) dpy = -88 - oy > dpy ? -88 - oy : dpy;  /* goalie: bias stops at own blue line */
        ty = oy + anchor + lead + w * dpy;
        double lo = -0.32 * H, hi = 0.40 * H;           /* band for camera_y - own_y */
        if (ty - oy < lo) ty = oy + lo;
        if (ty - oy > hi) ty = oy + hi;
        tx = ox + (puckcam ? 0.0 : 0.30 * (px - ox));
    } else if (!c->fixed) {
        ty = oy + s_lookahead;
    }
    double ex = tx - st->cx, ey = ty - st->cy;
    if (!st->have || st->flip != flip || ex * ex + ey * ey > 160.0 * 160.0) {
        st->cx = tx; st->cy = ty;
    } else if (s_framing) {
        double dzx = s_deadzone * 3, dzy = s_deadzone;
        if (ex > dzx) st->cx += (ex - dzx) * s_smooth; else if (ex < -dzx) st->cx += (ex + dzx) * s_smooth;
        if (ey > dzy) st->cy += (ey - dzy) * s_smooth; else if (ey < -dzy) st->cy += (ey + dzy) * s_smooth;
    } else {
        st->cx += ex * s_smooth; st->cy += ey * s_smooth;
    }
    st->have = 1;
    st->flip = flip;
    /* Clamps: the game's for a 256x224 window, narrowed by the extra rows /
     * columns so the bigger window stays on the rink map. A window as wide as
     * the map (384) has no horizontal freedom: Hpos = 0. */
    int hmax = NHL94_HPOS_MAX - s_extra_cols;
    if (hmax < 0) hmax = 0;
    int vmin = NHL94_VPOS_MIN + s_extra_rows, vmax = NHL94_VPOS_MAX - s_extra_rows;
    if (vmin > vmax) vmin = vmax = (NHL94_VPOS_MIN + NHL94_VPOS_MAX) / 2;
    (void)W;
    int hpos = clampi((int)lround(st->cx), -hmax, hmax);
    int vpos = clampi((int)lround(st->cy), vmin, vmax);
    st->cx = hpos; st->cy = vpos;   /* no wind-up against the clamps */
    *hpos_out = hpos; *vpos_out = vpos;
}

static int apply_view(int k)
{
    ViewState *st = &s_st[k];
    const ViewCfg *c = &s_cfg[k % s_ncfg];
    uint16_t gmode = rd16(NHL94_GMODE), sflags = rd16(NHL94_SFLAGS), sflags2 = rd16(NHL94_SFLAGS2);
    /* Own-seat view in a Classic session (the launcher mode or the host's
     * changed after the view count was fixed): the canonical image. */
    if (s_seatview && nhl94_seats_count() == 0) { st->have = 0; s_extra_cols = s_extra_rows = 0; return 0; }
    /* Highlights of other games, the horizontal scoreboard rink, replay and
     * pause: show the canonical image. */
    if ((gmode & NHL94_GMODE_HL_W) || (sflags & (SFLAGS_HOR_W | SFLAGS_PAUSE_W)) ||
        (sflags2 & SFLAGS2_REPLAY_W) || (rd16(NHL94_SFLAGS4) & SFLAGS4_REV_W)) {
        if (s_trace) fprintf(stderr, "nhl94_view: v%d canonical (gmode=%04X sflags=%04X sflags2=%04X)\n",
                             k, gmode, sflags, sflags2);
        st->have = 0;
        s_extra_cols = s_extra_rows = 0;
        return 0;
    }
    int fell_back = 0;
    ViewCfg follow = *c;
    if (s_follow_puck) follow.role = ROLE_PUCK;   /* toggle: same team orientation */
    int sc = find_target(&follow, &fell_back);
    if (fell_back && !st->fallback && st->fallback_logs++ < 8)
        fprintf(stderr, "nhl94_view: view %d (%s:%s) not on the ice -- following the puck\n", k,
                c->team ? "away" : "home", s_role_names[c->role]);
    if (!fell_back && st->fallback && s_trace)
        fprintf(stderr, "nhl94_view: view %d target back on the ice (SC %d)\n", k, sc);
    st->fallback = fell_back;

    int flip = !c->fixed && !team_attacks_up(c->team);
    int sgn = flip ? -1 : 1;
    int hpos, vpos;
    s_extra_cols = zoom_cols(view_zoom());
    s_extra_rows = zoom_rows(view_zoom());
    if (s_extra_rows && !set_tall_planes()) s_extra_rows = 0;
    frame_camera(st, c, &follow, sc, flip, sgn, &hpos, &vpos);
    if (flip && !load_reverse_rink_tiles()) {
        static int warned;
        if (!warned++) fprintf(stderr, "nhl94_view: rink tile sets not as expected -- no mirror\n");
        flip = 0;
    }
    if (flip) mirror_world();
    /* Active-player marker: the game's own pad-1 star and label (uppads /
     * setffo, video94.asm:719), which setvideo draws under the object in
     * PadControlBits nibble [7:4]. Pure presentation: nothing in the logic
     * reads the nibble for the star, and the logic of this frame has
     * already run. Pad 2's star is hidden so only the view's player is
     * marked; the puck-carrier nibble [3:0] is left as the game set it. */
    if (s_marker) {
        uint16_t pcb = rd16(NHL94_PADCONTROLBITS);
        int marked = sc;
        if (s_follow_puck && c->role != ROLE_PUCK) marked = find_target(c, &fell_back);
        unsigned nib = (c->role != ROLE_PUCK && !fell_back) ? (unsigned)marked : 0xFu;
        wr16(NHL94_PADCONTROLBITS, (uint16_t)((pcb & 0xF00Fu) | 0x0F00u | (nib << 4)));
    }
    wr16(NHL94_HPOS, (uint16_t)hpos);
    wr16(NHL94_VPOS, (uint16_t)vpos);
    if (s_dma_redraw) wr16(NHL94_OLDROW, OLDROW_REDRAW);   /* the game's own redraw */
    else redraw_rink_plane(vpos, flip);
    if (s_extra_cols) blank_plane_a_margins();
    s_app_hpos = hpos; s_app_vpos = vpos; s_app_flip = flip;
    s_app_pcb = rd16(NHL94_PADCONTROLBITS);
    if (s_trace)
        fprintf(stderr, "nhl94_view: v%d sc=%d%s flip=%d cam=(%d,%d) gmode=%04X\n", k, sc,
                fell_back ? "(fallback)" : "", flip, hpos, vpos, gmode);
    return 1;
}

/* setffo appends the pad stars after every player, net and puck sprite,
 * i.e. at the lowest sprite priority, so a goalie's star is hidden behind
 * his net. With the list complete (setvideo's `bset #dfok`), move the
 * sprites of pad object 1 (the view's marker: tiles VRchar.. up to the next
 * pad's VRchar) to the head of the link list. Satt is a sequentially linked
 * array (link = index+1, last 0), so it is re-linked in the new order. */
#define PADS        0xFFBDB4u   /* pads: 7 ffo objects of ffosize ($1C) */
#define FFOSIZE     0x1Cu
#define SATT_MAX    80
static void marker_to_front(void)
{
    unsigned words = rd16(NHL94_SATTSIZE), n = words / 4u;
    if (n < 2 || n > SATT_MAX) return;
    unsigned lo = rd16(PADS + FFOSIZE + NHL94_OFF_VRCHAR) & 0x7FFu, hi = lo + 64u;
    for (unsigned p = 0; p < 7; p++) {
        unsigned v = rd16(PADS + p * FFOSIZE + NHL94_OFF_VRCHAR) & 0x7FFu;
        if (p != 1 && v > lo && v < hi) hi = v;
    }
    uint8_t sp[SATT_MAX][8], out[SATT_MAX][8];
    for (unsigned i = 0; i < n; i++)
        for (unsigned b = 0; b < 8; b++) sp[i][b] = glue_peek8(NHL94_SATT + i * 8u + b);
    unsigned m = 0;
    for (int pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < n; i++) {
            unsigned tile = (((unsigned)sp[i][4] << 8) | sp[i][5]) & 0x7FFu;
            int mine = tile >= lo && tile < hi;
            if (mine == (pass == 0)) memcpy(out[m++], sp[i], 8);
        }
    for (unsigned i = 0; i < n; i++) {
        out[i][3] = (uint8_t)(i + 1 < n ? i + 1 : 0);
        for (unsigned b = 0; b < 8; b++) glue_poke8(NHL94_SATT + i * 8u + b, out[i][b]);
    }
}

int nhl94_view_instruction_hook(uint32_t pc)
{
    if (!genesis_multiview_speculating() || s_cur < 0) {
        if (pc == HOOK_SETVIDEO_JMP) {
            if (s_trace > 0)
                fprintf(stderr, "nhl94_view: hook tick=%u line=%u main\n",
                        genesis_sim_tick_count(), (unsigned)g_machine.vdp.scanline);
            nhl94_tags_main_hook();   /* presentation only: reads RAM into host statics */
        }
        return 0;
    }
    int sub = genesis_multiview_view_tick();
    switch (pc) {
    case HOOK_SETVIDEO_JMP:
        if (s_trace)
            fprintf(stderr, "nhl94_view: hook tick=%u line=%u sub=%d view=%d applied=%d\n",
                    genesis_sim_tick_count(), (unsigned)g_machine.vdp.scanline, sub, s_cur, s_applied);
        if (s_applied) {
            /* The game reached setvideo again in a later sub-tick (a lag frame
             * made the view need extra ticks). Its logic has run once more in
             * the clone: checkwindow pulled the camera back toward its own
             * target and the marker nibble may be rewritten. Left alone,
             * setvideo would build that frame with the drifted camera and
             * stream ONE new rink row into the ring, so the displayed frame
             * showed a band of stale/garbage rows (seen as one-frame
             * horizontal lines). Re-pin the camera, the marker and the full
             * plane redraw; the world mirror stays applied from the first hit
             * (applying it again would undo it). */
            if (s_changed) {
                if (s_marker) wr16(NHL94_PADCONTROLBITS, s_app_pcb);
                wr16(NHL94_HPOS, (uint16_t)s_app_hpos);
                wr16(NHL94_VPOS, (uint16_t)s_app_vpos);
                if (s_dma_redraw) wr16(NHL94_OLDROW, OLDROW_REDRAW);
                if (s_extra_rows) set_tall_planes();
                if (!s_dma_redraw) redraw_rink_plane(s_app_vpos, s_app_flip);
                if (s_extra_cols) blank_plane_a_margins();
            }
            break;
        }
        s_applied = 1;
        s_changed = apply_view(s_cur);
        nhl94_tags_view_capture(s_cur);   /* name tags: the view's own camera / mirror */
        break;
    case HOOK_SETVIDEO_DFOK:
        if (s_changed) {
            if (s_built_sub < 0) s_built_sub = sub;
            faceoff_sprites_shift();           /* before the marker re-orders the list */
            if (s_marker) marker_to_front();   /* every list this view completes */
        }
        break;
    case HOOK_CHECKSSO:
        /* Off-screen arrows point past the 256x224 window's edges, which are
         * inside a zoomed picture; the own player is always in view. */
        if (s_changed && (s_extra_cols || s_extra_rows)) return 1;
        break;
    case HOOK_FIND3D:
        if (s_changed && (s_extra_cols || s_extra_rows)) return find3d_zoomed();
        break;
    case HOOK_VBL_DUMP:
        if (s_built_sub >= 0 && s_upload_sub < 0) s_upload_sub = sub;
        break;
    default:
        break;
    }
    return 0;
}

/* The tick that just ran displayed the view's frame: its list was uploaded
 * by the vblank of an earlier sub-tick. Views that changed nothing (menus,
 * highlights, pause, replay) are always ready. A round with no picture of
 * its own inside a stoppage (setvideo not reached, or vb2 installed so the
 * list will not be uploaded) is ready at once and presents the held
 * picture (HOLD). */
int nhl94_view_ready(int view_index, void *user)
{
    (void)user;
    if (!s_applied) {
        s_hold = hold_candidate(view_index);
        return 1;
    }
    if (!s_changed) return 1;
    int sub = genesis_multiview_view_tick();
    int ready = s_upload_sub >= 0 && s_upload_sub < sub;
    s_round_good = ready;
    if (!ready && vbint() == VBL_FADE && hold_candidate(view_index)) {
        s_hold = 1;
        ready = 1;
    }
    if (s_trace)
        fprintf(stderr, "nhl94_view: ready? view=%d sub=%d built=%d upload=%d -> %d\n",
                view_index, sub, s_built_sub, s_upload_sub, ready);
    return ready;
}
