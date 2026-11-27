/*
 * nhl94_mode.c - the launcher's "Control mode" setting for NHL Hockey '94.
 *
 *   Classic        the cartridge: a pad's player switches to the best
 *                  skater (B, passes, loose pucks).
 *   Position lock  each human controls ONE position for the whole game
 *                  (nhl94_seats.c, docs/PLAYER_LOCK.md); seat 1..12 roles
 *                  (12 = 6v6, goalies included).
 *   Format         6v6 (the cartridge) or 4v4 = G, C, LW, LD per team
 *                  (nhl94_format.c, docs/FOUR_ON_FOUR_PLAN.md). Independent
 *                  of the control mode; in 4v4 only G / LD / LW / C seats.
 *
 * WHERE. A game-owned feature appended to the engine's launcher mod provider
 * (GameSpec.mods, the same seam as the engine's Widescreen / Diagnostics
 * entries): one feature "Control mode" on the launcher's Mods page with a
 * Mode choice, a Preset choice (1v1 centres / full 6v6) and twelve Seat
 * choices. Persisted in nhl94-control.ini next to
 * settings.ini (the engine rewrites settings.ini wholesale, so it cannot hold
 * game keys). Read at start-up through GameSpec.load_settings; every change
 * in the launcher is applied and saved at once, so a room hosted right after
 * the change already advertises it (the lobby captures the session image
 * when the room is created, before PLAY).
 *
 * PRECEDENCE (nhl94_seats.c): NHL94_SEATS env > --seats > this setting;
 * NHL94_FORMAT env > --format > this setting (nhl94_format.c).
 * Online the host's choice is the session's; the guest adopts it.
 */
#include "nhl94_mode.h"
#include "nhl94_seats.h"
#include "nhl94_format.h"
#include "nhl94_tags.h"
#include "nhl94_builds.h"
#include "nhl94_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RECOMP_LAUNCHER
#include "recomp_launcher.h"
#endif

#define SEATS NHL94_SEATS_MAX               /* 12 */
#define OPT_TAGS 1                          /* Name tags: On / Off (nhl94_tags.c), right under Mode */
#define OPT_CALLICON 2                      /* Call icon: On / Off (nhl94_tags.c), next to Name tags */
#define OPT_ZOOM 3                          /* View zoom (nhl94_view.c), position lock's own view */
#define OPT_FORMAT 4                        /* Format: 6v6 / 4v4 (nhl94_format.c) */
#define OPT_PRESET 5
#define OPT_SEAT0 6                         /* options: 0 mode, 1 tags, 2 call icon, 3 zoom, 4 format, 5 preset, 6.. seat 1..
                                               (a new option goes BEFORE the seats and bumps OPT_SEAT0: the seat
                                               options are addressed through it, p_option's `i -= OPT_SEAT0 - 1`) */
#define NOPTS (OPT_SEAT0 + SEATS)
static int  s_lock;                         /* 0 Classic, 1 Position lock */
static int  s_format = NHL94_FORMAT_6V6;    /* 6v6 / 4v4 */
static char s_seat[SEATS][16] = { "home:C", "away:C", "none", "none", "none", "none",
                                  "none", "none", "none", "none", "none", "none" };
static char s_file[700];
static char s_error[160];

static const char *const k_roles[] = {
    "home:G", "home:LD", "home:RD", "home:LW", "home:C", "home:RW",
    "away:G", "away:LD", "away:RD", "away:LW", "away:C", "away:RW" };
#define NROLES 12
/* 4v4: G, LD, LW, C only (RD / RW are not on the ice at even strength). */
static const char *const k_roles4[] = {
    "home:G", "home:LD", "home:LW", "home:C",
    "away:G", "away:LD", "away:LW", "away:C" };
#define NROLES4 8
#define NROLES_NOW (s_format == NHL94_FORMAT_4V4 ? NROLES4 : NROLES)
#define ROLE_NOW(i) (s_format == NHL94_FORMAT_4V4 ? k_roles4[i] : k_roles[i])

static int role_ok_now(const char *v)
{
    for (int i = 0; i < NROLES_NOW; i++) if (!strcmp(v, ROLE_NOW(i))) return 1;
    return 0;
}

static int valid_role(const char *v)
{
    for (int i = 0; i < NROLES; i++) if (!strcmp(v, k_roles[i])) return 1;
    return 0;
}

/* Presets. Seat order = join order online (seat 1 the host, seat 2 the
 * first guest, ...), so 6v6 alternates the teams centre-first: any number
 * of players who join is as even as it can be, and every on-ice player of
 * both teams (home G..RW, away G..RW) has a seat. */
static const char *const k_preset_1v1[SEATS] = { "home:C", "away:C", "none", "none", "none", "none",
                                                 "none", "none", "none", "none", "none", "none" };
static const char *const k_preset_6v6[SEATS] = { "home:C", "away:C", "home:LW", "away:LW", "home:RW",
                                                 "away:RW", "home:LD", "away:LD", "home:RD", "away:RD",
                                                 "home:G", "away:G" };
/* Full 4v4: centres first, alternating teams, goalies last (as 6v6). */
static const char *const k_preset_4v4[SEATS] = { "home:C", "away:C", "home:LW", "away:LW", "home:LD",
                                                 "away:LD", "home:G", "away:G", "none", "none",
                                                 "none", "none" };

static int is_preset(const char *const *p)
{
    for (int i = 0; i < SEATS; i++) if (strcmp(s_seat[i], p[i])) return 0;
    return 1;
}

static const char *preset_value(void)
{
    return is_preset(k_preset_6v6) ? "6v6" : is_preset(k_preset_4v4) ? "4v4"
         : is_preset(k_preset_1v1) ? "1v1" : "custom";
}

/* "seatN" (N = 1..SEATS) -> N-1, else -1. */
static int seat_key(const char *k)
{
    if (strncmp(k, "seat", 4) || !k[4]) return -1;
    char *end = NULL;
    long n = strtol(k + 4, &end, 10);
    if (*end || k[4] == '0' || n < 1 || n > SEATS) return -1;
    return (int)n - 1;
}

/* Seats 1.. up to the first "none". */
static void seat_list(char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < SEATS && strcmp(s_seat[i], "none"); i++) {
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%s%s", i ? "," : "", s_seat[i]);
    }
}

/* 4v4 has no RD / RW seats: drop them, keeping the order of the rest (the
 * list ends at the first "none"). Seat 1 falls back to home:C. */
static void scrub_seats(void)
{
    if (s_format != NHL94_FORMAT_4V4) return;
    char keep[SEATS][16];
    int n = 0;
    for (int i = 0; i < SEATS && strcmp(s_seat[i], "none"); i++)
        if (role_ok_now(s_seat[i])) snprintf(keep[n++], sizeof keep[0], "%s", s_seat[i]);
        else fprintf(stderr, "nhl94_mode: seat%d = %s has no player in 4v4 -- removed\n", i + 1, s_seat[i]);
    for (int i = 0; i < SEATS; i++) snprintf(s_seat[i], sizeof s_seat[0], "%s", i < n ? keep[i] : "none");
    if (!n) snprintf(s_seat[0], sizeof s_seat[0], "home:C");
}

static void apply(void)
{
    char list[160];
    seat_list(list, sizeof list);
    nhl94_format_set_launcher(s_format);    /* before the seats: their roles depend on it */
    nhl94_seats_set_launcher(s_lock && list[0] ? list : "");
}

static const char *zoom_value(void)
{
    int q = nhl94_view_zoom_setting();
    return q == 4 ? "1" : q == 5 ? "1.25" : q == 8 ? "2" : "1.5";
}
static int zoom_parse(const char *v)
{
    return !strcmp(v, "1") ? 4 : !strcmp(v, "1.25") ? 5 : !strcmp(v, "1.5") ? 6 : !strcmp(v, "2") ? 8 : -1;
}

static int save(void)
{
    if (!s_file[0]) return 1;
    FILE *f = fopen(s_file, "wb");
    if (!f) { snprintf(s_error, sizeof s_error, "cannot write %s", s_file); return 0; }
    fprintf(f, "# NHL 94 control mode (launcher: Mods > Control mode).\n"
               "# mode = classic | lock; seatN = home|away:G|LD|RD|LW|C|RW or none\n"
               "# nametags = on | off (name tags above the human-controlled players)\n"
               "# callicon = on | off (ring + ! for a teammate calling for the puck; needs name tags)\n"
               "# viewzoom = 1 | 1.25 | 1.5 | 2 (position lock's own view; display only)\n"
               "# format = 6v6 | 4v4 (4v4 = G, C, LW, LD per team; seats G|LD|LW|C only)\n"
               "[control]\nmode = %s\nnametags = %s\ncallicon = %s\nviewzoom = %s\nformat = %s\n",
            s_lock ? "lock" : "classic", nhl94_tags_setting() ? "on" : "off",
            nhl94_tags_callicon_setting() ? "on" : "off", zoom_value(), nhl94_format_name(s_format));
    for (int i = 0; i < SEATS; i++) fprintf(f, "seat%d = %s\n", i + 1, s_seat[i]);
    nhl94_builds_ini_write(f);
    int ok = fclose(f) == 0;
    return ok;
}

void nhl94_mode_load(const char *settings_ini)
{
    /* nhl94-control.ini beside settings.ini */
    const char *slash = settings_ini ? strrchr(settings_ini, '/') : NULL;
#ifdef _WIN32
    const char *bs = settings_ini ? strrchr(settings_ini, '\\') : NULL;
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    if (slash) snprintf(s_file, sizeof s_file, "%.*snhl94-control.ini", (int)(slash - settings_ini + 1), settings_ini);
    else snprintf(s_file, sizeof s_file, "nhl94-control.ini");
    FILE *f = fopen(s_file, "rb");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char key[32], val[64];
            if (sscanf(line, " %31[a-z0-9_] = %63s", key, val) != 2) continue;
            if (!strcmp(key, "mode")) s_lock = !strcmp(val, "lock");
            else if (!strcmp(key, "nametags")) nhl94_tags_set_setting(strcmp(val, "off") && strcmp(val, "0"));
            else if (!strcmp(key, "callicon")) nhl94_tags_set_callicon(strcmp(val, "off") && strcmp(val, "0"));
            else if (!strcmp(key, "viewzoom") && zoom_parse(val) > 0) nhl94_view_set_zoom_setting(zoom_parse(val));
            else if (!strcmp(key, "format") && nhl94_format_parse(val)) s_format = nhl94_format_parse(val);
            else if (seat_key(key) >= 0 && (valid_role(val) || !strcmp(val, "none")))
                snprintf(s_seat[seat_key(key)], sizeof s_seat[0], "%s", val);
            else nhl94_builds_ini_key(key, val);      /* build_skater / build_goalie (nhl94_builds.c) */
        }
        fclose(f);
    }
    if (!valid_role(s_seat[0])) snprintf(s_seat[0], sizeof s_seat[0], "home:C");
    /* A hand-edited file with one position twice: keep the first. */
    for (int i = 1; i < SEATS; i++)
        for (int j = 0; j < i; j++)
            if (valid_role(s_seat[i]) && !strcmp(s_seat[i], s_seat[j])) {
                fprintf(stderr, "nhl94_mode: seat%d = %s is already seat %d -- none\n", i + 1, s_seat[i], j + 1);
                snprintf(s_seat[i], sizeof s_seat[0], "none");
            }
    scrub_seats();
    fprintf(stderr, "nhl94_mode: launcher Control mode = %s, format %s (%s)\n", s_lock ? "Position lock" : "Classic",
            nhl94_format_name(s_format), f ? s_file : "no nhl94-control.ini yet, default");
    apply();
    nhl94_builds_announce();     /* the lobby member data: our picks, for the host's session image */
}

/* The waiting room's picker (nhl94_builds.c) changed the picks: persist them. */
void nhl94_mode_save(void) { save(); }

#if RECOMP_LAUNCHER
/* ---- the launcher feature, appended to the engine's provider ---- */

#define PKG  "nhl94-control"
#define FEAT "control-mode"
static const RecompLauncherCModProvider *s_base;
static RecompLauncherCModProvider s_prov;

static int base_count(void) { return s_base && s_base->feature_count ? s_base->feature_count(s_base->ctx) : 0; }
static int mine(const char *pkg, const char *feat) { return pkg && !strcmp(pkg, PKG) && (!feat || !strcmp(feat, FEAT)); }
#define CPY(d, v) snprintf(d, sizeof(d), "%s", v)

static void status(char *out, size_t cap)
{
    char list[160];
    seat_list(list, sizeof list);
    int n = 0;
    while (n < SEATS && strcmp(s_seat[n], "none")) n++;
    if (s_lock && is_preset(k_preset_6v6))
        snprintf(out, cap, "Position lock: full 6v6, 12 seats (seat 1 = you offline, the host online)");
    else if (s_lock && is_preset(k_preset_4v4))
        snprintf(out, cap, "Position lock: full 4v4, 8 seats (seat 1 = you offline, the host online)");
    else if (s_lock) snprintf(out, cap, "Position lock, %d seat%s: %s (seat 1 = you offline, the host online)",
                              n, n == 1 ? "" : "s", list);
    else snprintf(out, cap, "Classic: control switches to the best player");
    size_t l = strlen(out);
    snprintf(out + l, cap - l, " | %s", s_format == NHL94_FORMAT_4V4 ? "4v4 (G, C, LW, LD)" : "6v6");
    l = strlen(out);
    snprintf(out + l, cap - l, " | name tags %s", nhl94_tags_enabled() ? "on" : "off");
    l = strlen(out);
    if (s_lock) snprintf(out + l, cap - l, " | call icon %s", nhl94_tags_callicon_mode() ? "on" : "off");
    l = strlen(out);
    if (s_lock) snprintf(out + l, cap - l, " | view zoom %sx", zoom_value());
}

static int p_count(void *ctx) { (void)ctx; return base_count() + 1; }

static int p_package(void *ctx, int i, RecompLauncherCModPackage *out)
{
    int n = s_base && s_base->package_count ? s_base->package_count(s_base->ctx) : 0;
    if (i < n) return s_base->package_get ? s_base->package_get(ctx, i, out) : 0;
    if (i != n || !out) return 0;
    memset(out, 0, sizeof *out);
    CPY(out->id, PKG); CPY(out->name, "Control mode"); CPY(out->version, "1");
    CPY(out->description, "Classic cartridge control or per-position lock.");
    out->enabled = 1; out->option_count = NOPTS;
    return 1;
}
static int p_pkgcount(void *ctx) { return (s_base && s_base->package_count ? s_base->package_count(ctx) : 0) + 1; }

static int p_feature(void *ctx, int i, RecompLauncherCModFeature *out)
{
    int n = base_count();
    if (i < n) return s_base->feature_get(ctx, i, out);
    if (i != n || !out) return 0;
    memset(out, 0, sizeof *out);
    CPY(out->id, FEAT); CPY(out->package_id, PKG); CPY(out->package_name, "Control mode");
    CPY(out->package_version, "1"); CPY(out->name, "Control mode"); CPY(out->group, "Gameplay");
    CPY(out->description,
        "Classic: the original cartridge - your control switches to the best player (B, passes, "
        "loose pucks). Position lock: every human plays ONE position all game (seat 1 = you offline / "
        "the room host, seat 2 = the first guest...), up to 12 seats = 6v6 with goalies; the AI plays "
        "the rest; B calls for a pass. Format 4v4: goalie, centre, left wing and left D per team. "
        "Online the host's mode and format are used for every player.");
    status(out->status, sizeof out->status);
    out->enabled = 1;   /* always on: the Mode option is the switch */
    out->option_count = NOPTS;
    out->quick = 1;
    return 1;
}

static int p_option(void *ctx, const char *pkg, const char *feat, int i, RecompLauncherCModOption *out)
{
    if (!mine(pkg, feat))
        return s_base && s_base->feature_option_get ? s_base->feature_option_get(ctx, pkg, feat, i, out) : 0;
    if (!out || i < 0 || i >= NOPTS) return 0;
    memset(out, 0, sizeof *out);
    out->type = RECOMP_MOD_OPTION_CHOICE; out->step = 1;
    if (i == 0) {
        CPY(out->id, "mode"); CPY(out->label, "Mode");
        CPY(out->description, "Classic = cartridge control. Position lock = one position per human.");
        CPY(out->value, s_lock ? "lock" : "classic"); CPY(out->default_value, "classic");
        out->choice_count = 2;
        return 1;
    }
    if (i == OPT_FORMAT) {
        CPY(out->id, "format"); CPY(out->label, "Format");
        CPY(out->description, "6v6 = the cartridge. 4v4 = goalie, centre, left wing and left D per team; a "
                              "penalty gives the other team an extra (AI) skater, as in NHL 3-on-3 overtime. "
                              "Starts with the next game. Online the host's format is used.");
        CPY(out->value, nhl94_format_name(s_format)); CPY(out->default_value, "6v6");
        out->choice_count = 2;
        return 1;
    }
    if (i == OPT_PRESET) {
        CPY(out->id, "preset"); CPY(out->label, "Preset");
        CPY(out->description, "Fill the seats: 1v1 = home C vs away C; Full 6v6 = all 12 on-ice players; "
                              "Full 4v4 = all 8 players of the 4v4 format (sets Format to 4v4). Seats "
                              "alternate home / away, centres first, goalies last. "
                              "Custom = the seat list below.");
        CPY(out->group, "Position lock");
        CPY(out->value, preset_value()); CPY(out->default_value, "1v1");
        out->choice_count = 4;
        out->disabled = !s_lock;
        return 1;
    }
    if (i == OPT_ZOOM) {
        CPY(out->id, "viewzoom"); CPY(out->label, "View zoom");
        CPY(out->description, "Position lock: how much ice your own view shows. 1.5x = the whole rink width "
                              "and half again as much ice ahead / behind; 2x = the same width and twice the ice ahead / behind "
                              "(smaller players). Display only (each player picks their own); F or R3 "
                              "cycles it in game; NHL94_VIEW_ZOOM overrides it.");
        CPY(out->value, zoom_value()); CPY(out->default_value, "1.5");
        out->choice_count = 4;
        out->disabled = !s_lock;
        return 1;
    }
    if (i == OPT_TAGS) {
        CPY(out->id, "nametags"); CPY(out->label, "Name tags");
        CPY(out->description, "Mark every human-controlled player on the ice with a coloured triangle and "
                              "the player's name (AI players get none). Display only; the "
                              "NHL94_NAMETAGS=0/1 environment overrides this.");
        CPY(out->value, nhl94_tags_setting() ? "on" : "off"); CPY(out->default_value, "on");
        out->choice_count = 2;
        return 1;
    }
    if (i == OPT_CALLICON) {
        CPY(out->id, "callicon"); CPY(out->label, "Call icon");
        CPY(out->description, "Position lock: a pulsing ring at the feet and a ! beside the name tag of a "
                              "teammate who is calling for the puck (holding B or X while a teammate has it). "
                              "Needs Name tags. Display only; the NHL94_CALLICON=0/1/all environment overrides this.");
        CPY(out->value, nhl94_tags_callicon_setting() ? "on" : "off"); CPY(out->default_value, "on");
        out->choice_count = 2;
        out->disabled = !s_lock;
        return 1;
    }
    i -= OPT_SEAT0 - 1;                     /* seat number 1..SEATS */
    snprintf(out->id, sizeof out->id, "seat%d", i);
    snprintf(out->label, sizeof out->label, "Seat %d position", i);
    snprintf(out->description, sizeof out->description,
             i == 1 ? "Seat 1: you offline, the room host online."
             : i == 2 ? "Seat 2: the first guest online (or a 2nd controller offline). None = stop the list here."
             : "Seat %d: the next guest online / a further local controller. None = stop the list here.", i);
    CPY(out->group, "Position lock");
    CPY(out->value, s_seat[i - 1]);
    CPY(out->default_value, i == 1 ? "home:C" : i == 2 ? "away:C" : "none");
    out->choice_count = NROLES_NOW + (i > 1);
    out->disabled = !s_lock;
    return 1;
}

static int p_choice(void *ctx, const char *pkg, const char *feat, const char *opt, int i,
                    RecompLauncherCModChoice *out)
{
    if (!mine(pkg, feat))
        return s_base && s_base->feature_choice_get ? s_base->feature_choice_get(ctx, pkg, feat, opt, i, out) : 0;
    if (!out || !opt || i < 0) return 0;
    memset(out, 0, sizeof *out);
    if (!strcmp(opt, "mode")) {
        if (i > 1) return 0;
        CPY(out->value, i ? "lock" : "classic");
        CPY(out->label, i ? "Position lock" : "Classic (cartridge)");
        return 1;
    }
    if (!strcmp(opt, "format")) {
        if (i > 1) return 0;
        CPY(out->value, i ? "4v4" : "6v6");
        CPY(out->label, i ? "4v4 (G, C, LW, LD)" : "6v6 (cartridge)");
        return 1;
    }
    if (!strcmp(opt, "preset")) {
        static const char *const v[] = { "1v1", "6v6", "4v4", "custom" };
        static const char *const l[] = { "1v1 (home C vs away C)", "Full 6v6 (12 seats)", "Full 4v4 (8 seats)",
                                         "Custom" };
        if (i > 3) return 0;
        CPY(out->value, v[i]); CPY(out->label, l[i]);
        return 1;
    }
    if (!strcmp(opt, "viewzoom")) {
        static const char *const v[] = { "1", "1.25", "1.5", "2" };
        static const char *const l[] = { "1x (original)", "1.25x", "1.5x (recommended)", "2x" };
        if (i > 3) return 0;
        CPY(out->value, v[i]); CPY(out->label, l[i]);
        return 1;
    }
    if (!strcmp(opt, "nametags") || !strcmp(opt, "callicon")) {
        if (i > 1) return 0;
        CPY(out->value, i ? "off" : "on"); CPY(out->label, i ? "Off" : "On");
        return 1;
    }
    if (seat_key(opt) < 0) return 0;
    int seat1 = !strcmp(opt, "seat1");
    int nr = NROLES_NOW;
    if (i >= nr + !seat1) return 0;
    if (i == nr) { CPY(out->value, "none"); CPY(out->label, "None"); return 1; }
    static const char *const names[] = { "Goalie", "Left D", "Right D", "Left Wing", "Center", "Right Wing" };
    const char *v = ROLE_NOW(i);
    int r = 0;
    while (r < NROLES && strcmp(k_roles[r], v)) r++;
    CPY(out->value, v);
    snprintf(out->label, sizeof out->label, "%s %s", r < 6 ? "Home" : "Away", names[r % 6]);
    return 1;
}

static int p_set(void *ctx, const char *pkg, const char *feat, const char *opt, const char *val)
{
    if (!mine(pkg, feat))
        return s_base && s_base->feature_set_option ? s_base->feature_set_option(ctx, pkg, feat, opt, val) : 0;
    s_error[0] = 0;
    if (!opt || !val) return 0;
    if (!strcmp(opt, "mode")) {
        if (strcmp(val, "lock") && strcmp(val, "classic")) return 0;
        s_lock = !strcmp(val, "lock");
    } else if (!strcmp(opt, "format")) {
        int f = nhl94_format_parse(val);
        if (!f) return 0;
        s_format = f;
        scrub_seats();
    } else if (!strcmp(opt, "preset")) {
        const char *const *p = !strcmp(val, "6v6") ? k_preset_6v6 : !strcmp(val, "4v4") ? k_preset_4v4
                             : !strcmp(val, "1v1") ? k_preset_1v1 : NULL;
        if (!p && strcmp(val, "custom")) return 0;
        if (p) {                                /* a preset is a Position-lock seat list */
            for (int j = 0; j < SEATS; j++) snprintf(s_seat[j], sizeof s_seat[0], "%s", p[j]);
            s_lock = 1;
            if (p == k_preset_4v4) s_format = NHL94_FORMAT_4V4;     /* Full 4v4 is the 4v4 format */
            else if (p == k_preset_6v6) s_format = NHL94_FORMAT_6V6;
        }
    } else if (!strcmp(opt, "nametags")) {
        if (strcmp(val, "on") && strcmp(val, "off")) return 0;
        nhl94_tags_set_setting(!strcmp(val, "on"));
    } else if (!strcmp(opt, "callicon")) {
        if (strcmp(val, "on") && strcmp(val, "off")) return 0;
        nhl94_tags_set_callicon(!strcmp(val, "on"));
        return save();                      /* display only: nothing for the session */
    } else if (!strcmp(opt, "viewzoom")) {
        if (zoom_parse(val) < 0) return 0;
        nhl94_view_set_zoom_setting(zoom_parse(val));
        return save();                      /* display only: nothing for the session */
    } else if (seat_key(opt) >= 0) {
        int k = seat_key(opt);
        if (!(valid_role(val) || (k > 0 && !strcmp(val, "none")))) return 0;
        if (valid_role(val) && !role_ok_now(val)) {
            snprintf(s_error, sizeof s_error, "%s is not on the ice in 4v4 (G, LD, LW, C)", val);
            return 0;
        }
        for (int j = 0; j < SEATS; j++)          /* one human per position */
            if (j != k && valid_role(val) && !strcmp(s_seat[j], val)) {
                snprintf(s_error, sizeof s_error, "%s is already seat %d", val, j + 1);
                return 0;
            }
        snprintf(s_seat[k], sizeof s_seat[0], "%s", val);
    } else {
        return 0;
    }
    apply();          /* the lobby may capture the session image before PLAY */
    return save();
}

static int p_enable(void *ctx, const char *pkg, const char *feat, int on)
{
    if (!mine(pkg, feat))
        return s_base && s_base->feature_enable ? s_base->feature_enable(ctx, pkg, feat, on) : 0;
    return 1;         /* always enabled; the Mode option switches */
}

static int p_commit(void *ctx, const char *image)
{
    int ok = s_base && s_base->commit ? s_base->commit(ctx, image) : 1;
    apply();
    return save() && ok;
}

static int p_commit_netplay(void *ctx, const char *image)
{
    /* The engine's own entries stay vanilla online (its commit_netplay, or
     * none); the control mode is the host's session config. */
    int ok = s_base && s_base->commit_netplay ? s_base->commit_netplay(ctx, image) : 1;
    apply();
    return save() && ok;
}

static const char *p_error(void *ctx)
{
    if (s_error[0]) return s_error;
    return s_base && s_base->last_error ? s_base->last_error(ctx) : "";
}

const struct RecompLauncherCModProvider *nhl94_mode_mods(const struct RecompLauncherCModProvider *base)
{
    s_base = base;
    if (base) s_prov = *base;              /* keeps base ctx + everything not overridden */
    else memset(&s_prov, 0, sizeof s_prov);
    s_prov.package_count = p_pkgcount;  s_prov.package_get = p_package;
    s_prov.feature_count = p_count;     s_prov.feature_get = p_feature;
    s_prov.feature_option_get = p_option; s_prov.feature_choice_get = p_choice;
    s_prov.feature_set_option = p_set;  s_prov.feature_enable = p_enable;
    s_prov.commit = p_commit;           s_prov.commit_netplay = p_commit_netplay;
    s_prov.last_error = p_error;
    return &s_prov;
}
#else
const struct RecompLauncherCModProvider *nhl94_mode_mods(const struct RecompLauncherCModProvider *base)
{
    return base;
}
#endif
