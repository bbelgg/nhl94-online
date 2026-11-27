/*
 * nhl94_seats.c - per-skater human control ("player lock") for NHL Hockey
 * '94 (Genesis, retail ROM).
 *
 * MODEL. A seat is one logical player of the simulation input
 * (GenesisSimInput.pad[i], i.e. genesis_sim_pad(i); netplay session slot i
 * by default). Seats 0..3 are bound to the game's own controllers 1..4
 * (cont1team..cont4team, c1playernum..c4playernum, PadRaw1..4, lj1..4, the
 * B-hold timers); seats 4..11 to virtual pads in unused guest RAM that are
 * swapped through pad 3's / pad 4's slots (see "virtual pads" below), so up
 * to 12 seats = 6v6. Each is locked to ONE role of ONE team for the whole
 * game:
 *
 *     NHL94_SEATS="home:C,away:G"     (or --seats home:C,away:G)
 *
 * Every frame the seat's player is re-found by role (object +$34), so line
 * changes and penalty re-slotting hand the next player at that position to
 * the same human. Every other player is AI. Nothing here keeps simulation
 * state outside guest RAM: the hooks read the guest and the tick's sealed
 * input only, so rollback replays and every netplay peer compute the same.
 *
 * HOOKS (game.toml game_hook sites; see docs/PLAYER_LOCK.md for why each):
 *   $01A370 ReadJoyData   REPLACED while seats are configured: PadRaw1..4 are
 *                         filled from genesis_sim_pad(0..3) (the game's own
 *                         active-low byte, opposite directions resolved with
 *                         its PadDirTable). Seats 2/3 never touch port wiring
 *                         or the 4 Way Play protocol.
 *   $00A9D6 updateplayers ENFORCE (continues into the routine): cont1..4team
 *                         and c1..4playernum := the seats' teams and role
 *                         objects; pfjoycon only on those objects. Objects
 *                         that lose it are released exactly as restorepl does
 *                         (bclr pfjoycon, bset pfna unless pflags3 bit 3).
 *   $00C0BC setc1player   REFUSED (rts) while the lock is active: every pad
 *   $00C0DA setc2player   switch the game makes (B switch, pass receiver,
 *                         one-timer, loose puck, faceoff reselection, penalty
 *                         clear) funnels through these two. Pads 3/4 reach
 *                         them through the game's own 3->1 / 4->2 swaps.
 *   $00B0E8 doinput       REMAP (continues): for a pad player whose pfgoal is
 *                         clear (attacks the bottom goal) the direction code
 *                         d0 is turned 180 degrees ((d+4)&7) and the raw U/D,
 *                         L/R bits of d1-d3 swapped, so "up" is always toward
 *                         the opponent's goal (the per-player views rotate
 *                         that team's screen 180 degrees).
 *   $00AD42 updateplayers .t1 / $00AE7C .t1cont (continue): a seat 4..11's
 *                         virtual pad is swapped into pad 3's (home) or pad
 *                         4's (away) slots for its own object's pad block, so
 *                         the game's own pad-3/4 code runs doinput for it.
 *   $00D462 assgoaliectrl .cont2 (continues): pf3gctrl cleared on a seat's
 *                         goalie, cancelling the camera-distance rule (the
 *                         shared camera must not decide who controls him).
 *                         The game's other goalie rules (out-of-range whistle,
 *                         animation locks, checkanim) are untouched.
 *
 * "Lock active" = seats configured, not in a penalty shot / shootout
 * (BA_PS_flags bit 2: the game's own shooter/goalie hand-over runs), not in
 * intermission highlights (gmhl), not in the horizontal scoreboard rink.
 *
 * DIAGNOSTICS (host-side only, never read by the simulation):
 *   NHL94_SEATS_TRACE=<n>  every n ticks print each seat's object, position,
 *                          velocity, facing, pfjoycon mask; and counters:
 *                          refused switches, stray pfjoycon, goalie rule.
 *   NHL94_SEATS_KEEP_CAMERA_RULE=1  TEST ONLY (changes the simulation): keep
 *                          the camera-distance goalie rule, as a negative
 *                          control for the goalie-seat test.
 *   NHL94_PASSASSIST=off   TEST ONLY (changes the simulation): no aimed
 *                          passing and no B mask for a non-carrier, i.e. the
 *                          pre-"EASHL pass" behaviour (reproduces the old
 *                          baseline hashes). Never set it in a netplay session.
 *
 * EASHL-style passing ($00BD5E dopass, doinput; docs/PLAYER_LOCK.md "Aimed
 * passing"): a human passer's receiver is the teammate nearest the held d-pad
 * direction (nhl94_aim.c), and B is a call, never a sweep check, for a
 * non-carrier whose teammate has the puck.
 */
#include "nhl94_seats.h"
#include "nhl94_aim.h"
#include "nhl94_format.h"
#include "nhl94_builds.h"
#include "nhl94_ram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_runtime.h"
#include "genesis_host_mem.h"
#include "multiview.h"
#include "sim_step.h"
#if GENESIS_HAS_RECOMP_NET
#include "netplay/genesis_netplay.h"
#endif

typedef struct { int team, role; } Seat;

static Seat     s_seat[NHL94_SEATS_MAX];
static int      s_nseat;
static int      s_parsed;
static char     s_canon[160];
static char     s_cli[160];
static int      s_have_cli;
static unsigned s_trace;            /* NHL94_SEATS_TRACE period, 0 = off */
static int      s_no_autolc;        /* NHL94_SEATS_NO_AUTOLC=1: TEST ONLY negative control (changes the simulation) */
static int      s_keep_camrule;     /* NHL94_SEATS_KEEP_CAMERA_RULE=1: negative control */
static int      s_noassist;         /* NHL94_PASSASSIST=off: TEST ONLY, the pre-aimed-pass sim */
/* The three raw env flags above act only through these (ignored in netplay,
 * nhl94_sim_knob_allowed). */
#define KNOB(raw, name) ((raw) && nhl94_sim_knob_allowed(name))
#define NO_AUTOLC()     KNOB(s_no_autolc, "NHL94_SEATS_NO_AUTOLC")
#define KEEP_CAMRULE()  KNOB(s_keep_camrule, "NHL94_SEATS_KEEP_CAMERA_RULE")
#define NO_ASSIST()     KNOB(s_noassist, "NHL94_PASSASSIST")

/* Diagnostics (host statics; logging only). */
static unsigned long s_refused, s_stray, s_moved, s_reset, s_gctrl, s_remaps, s_frames;
static unsigned long s_vacant[NHL94_SEATS_MAX], s_handover[NHL94_SEATS_MAX];
static int s_last_rost[NHL94_SEATS_MAX] = { [0 ... NHL94_SEATS_MAX - 1] = -1 };
static unsigned long s_calls, s_call_passes, s_call_dropped, s_call_human, s_call_busy, s_call_opp, s_call_loose;
static unsigned s_dbg_gate, s_dbg_pick;   /* chk4pass visits of the pending call (log only) */
static int s_last_target[NHL94_SEATS_MAX] = { [0 ... NHL94_SEATS_MAX - 1] = -1 };
static unsigned long s_vswaps[NHL94_SEATS_MAX];   /* virtual-pad doinput passes per seat (log only) */
static unsigned long s_aim_passes, s_aim_changed, s_aim_human, s_aim_none;   /* aim_pass_hook (log only) */
static int s_atexit;
static unsigned long s_ps_frames, s_lcm_seen, s_sub_frames, s_sub_takes, s_vacant_nosub, s_pending_released, s_midplay_handover;
static int s_was_sub[NHL94_SEATS_MAX], s_last_prim[NHL94_SEATS_MAX] = { [0 ... NHL94_SEATS_MAX - 1] = -1 };
/* setpersonel callers (observation, $015788): return address -> total / mid-play calls. A mid-play
 * caller other than the goalie pulls would decide the "setpersonel pin" (docs/PLAYER_LOCK.md). */
#define SPC_MAX 16
static struct { uint32_t ret; unsigned long n, mid; } s_spc[SPC_MAX];

static const char *const s_role_name[] = { "G", "LD", "RD", "LW", "C", "RW" };

int nhl94_sim_knob_allowed(const char *name)
{
#if GENESIS_HAS_RECOMP_NET
    if (genesis_netplay_active()) {
        static const char *seen[8];
        int i = 0;
        for (; i < 8 && seen[i]; i++) if (!strcmp(seen[i], name)) return 0;
        if (i < 8) seen[i] = name;
        fprintf(stderr, "nhl94_seats: %s is a TEST-ONLY sim knob and is IGNORED while netplay is active "
                        "(it is not part of the sealed session)\n", name);
        return 0;
    }
#endif
    (void)name;
    return 1;
}

/* ---- configuration ---- */

/* PASS-LOGIC MARKER (EASHL aimed passing, docs/PLAYER_LOCK.md "Session
 * marker"). Aimed passing and the B mask change the simulation, so a build
 * without them must REFUSE a session that has them (and a build with them a
 * session that lacks them): the match identity does not refuse a different
 * build on its own. A seats value that carries seats is led by "p<N>:", N the
 * pass-logic version (SEATS_PASS_VER), in front of every other form:
 *
 *     value := [ "p" N ":" ] body [ "+B" ... ]        ("+B..." = the builds
 *                                       suffix, parsed by whoever owns it)
 *     body  := "4v4" | "4v4," list | "*4" letters | "*" letters | list
 *
 * An older parse_seats rejects the marked forms by itself: "p<N>:4v4,..." and
 * "p<N>:home:C,..." fail parse_seats_list (the first item's role is not a role
 * name / its team is neither home nor away) and "p<N>:*eg" starts with 'p', so
 * it is read as a list whose item "p<N>:*eg" fails the same way. A value
 * without seats ("4v4" alone: Classic control, no lock) carries no marker:
 * nothing in the sim differs then.
 *
 * Versions: 1 = aimed passing + the B mask. 2 = also the later sim changes
 * (join at the whistle $FFE302, pause-orphan release, the penalty-shot
 * attempt latch $FFE304, joiner role-body reservation): a "p1" host runs a
 * different drop / join / penalty-shot sim, so it is refused. Bump again for
 * any further change that alters the sim for the same seats value. */
#define SEATS_PASS_VER 2

/* "p<digits>:" at the start of v -> *ver and the rest; else *ver = 0, v. */
static const char *strip_marker(const char *v, int *ver)
{
    *ver = 0;
    if (v[0] != 'p' || v[1] < '0' || v[1] > '9') return v;
    const char *q = v + 1;
    int n = 0;
    while (*q >= '0' && *q <= '9' && n < 1000) n = n * 10 + (*q++ - '0');
    if (*q != ':' || n < 1) return v;
    *ver = n;
    return q + 1;
}

/* Compact form for the session image (GenesisSessionConfig.game is small):
 * "*" then one letter per seat, 'a' + team * 6 + role (a..f = home G..RW,
 * g..l = away G..RW). "*eg" = home:C,away:G. */
static int parse_compact(const char *list, Seat *out, int *n_out)
{
    int n = 0;
    for (const char *p = list + 1; *p; p++) {
        if (*p < 'a' || *p > 'l' || n >= NHL94_SEATS_MAX) return 0;
        Seat s = { (*p - 'a') / 6, (*p - 'a') % 6 };
        for (int i = 0; i < n; i++)
            if (out[i].team == s.team && out[i].role == s.role) return 0;
        out[n++] = s;
    }
    *n_out = n;
    return n > 0;
}

static int parse_seats_list(const char *list, Seat *out, int *n_out)
{
    int n = 0;
    if (list && list[0] == '*') return parse_compact(list, out, n_out);
    const char *p = list;
    while (p && *p) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char item[32];
        if (len == 0 || len >= sizeof item || n >= NHL94_SEATS_MAX) return 0;
        memcpy(item, p, len); item[len] = 0;
        char *r = strchr(item, ':');
        if (!r) return 0;
        *r++ = 0;
        Seat s = { -1, -1 };
        if (!strcmp(item, "home")) s.team = 0;
        else if (!strcmp(item, "away")) s.team = 1;
        for (int i = 0; i < 6; i++) if (!strcmp(r, s_role_name[i])) s.role = i;
        if (s.team < 0 || s.role < 0) return 0;
        for (int i = 0; i < n; i++)
            if (out[i].team == s.team && out[i].role == s.role) return 0;  /* one human per role */
        out[n++] = s;
        p = e ? e + 1 : NULL;
    }
    *n_out = n;
    return n > 0;
}

/* A seats VALUE, with the game format in it (docs/FOUR_ON_FOUR_PLAN.md
 * section 5): "4v4" alone (Classic control in 4v4), "4v4,<list>" or the
 * compact "*4<letters>" = 4v4; anything else is a 6v6 list. The format rides
 * in the seats value, not a key of its own, because a build without 4v4
 * must REFUSE a 4v4 host (its parse_seats rejects these forms) instead of
 * adopting the seats and playing 6v6: the match identity does not refuse a
 * different build on its own (rnet_rb_driver: "BUILD differs -- not
 * refused"). In 4v4 only G / LD / LW / C seats exist. *fmt_out = 6 or 4;
 * Classic 6v6 ("") is not a value (no key). */
static int parse_seats(const char *v, Seat *out, int *n_out, int *fmt_out)
{
    int fmt = NHL94_FORMAT_6V6, ok, ver;
    *n_out = 0;
    if (!v) return 0;
    v = strip_marker(v, &ver);
    if (ver > SEATS_PASS_VER) return 0;          /* a newer pass logic than this build knows */
    if (!strcmp(v, "4v4")) { *fmt_out = NHL94_FORMAT_4V4; return 1; }
    if (!strncmp(v, "4v4,", 4)) { fmt = NHL94_FORMAT_4V4; ok = parse_seats_list(v + 4, out, n_out); }
    else if (v[0] == '*' && v[1] == '4') {
        char tmp[NHL94_SEATS_MAX + 2];
        if (strlen(v + 2) > NHL94_SEATS_MAX) return 0;
        snprintf(tmp, sizeof tmp, "*%s", v + 2);
        fmt = NHL94_FORMAT_4V4;
        ok = parse_compact(tmp, out, n_out);
    } else ok = parse_seats_list(v, out, n_out);
    if (!ok) return 0;
    if (*n_out > 0 && ver != SEATS_PASS_VER) return 0;   /* seats without our marker: an older build's session */
    for (int i = 0; i < *n_out; i++)
        if (!nhl94_format_role_ok(fmt, out[i].role)) return 0;
    *fmt_out = fmt;
    return 1;
}

/* The per-player builds ride in the seats value as a "+B..." segment
 * (nhl94_builds.c: "+B" <4 hex digits> <2 chars per seat>), after the list
 * ("*ekdjfl+B1A2B3C..."). A segment starts at "+B" and ends at the next '+'
 * or the end of the value, so another feature's "+X" marker may sit before or
 * after it. Older parsers reject the '+' and refuse the launch (as for 4v4).
 * Removes the segment from val and copies it to seg ("" when none). */
static void split_builds(char *val, char *seg, size_t cap)
{
    seg[0] = 0;
    char *b = strstr(val, "+B");
    if (!b) return;
    size_t len = 2 + strcspn(b + 2, "+");
    snprintf(seg, cap, "%.*s", (int)len, b);
    memmove(b, b + len, strlen(b + len) + 1);
}

/* A host "game" image's seat list, format and builds segment. 0 = none /
 * unparsable (Classic image, past the list, ...). */
static int image_seats(const char *image, Seat *out, int *n, int *fmt, char *seg, size_t segcap)
{
    const char *k = image ? strstr(image, "nhl94.seats=") : NULL;
    char list[160];
    if (!k) return 0;
    k += strlen("nhl94.seats=");
    size_t len = strcspn(k, ";\n");
    if (len == 0 || len >= sizeof list) return 0;
    memcpy(list, k, len); list[len] = 0;
    split_builds(list, seg, segcap);
    return parse_seats(list, out, n, fmt);
}

/* Where the seat list comes from. The LOCAL choice (what this process
 * offers, the netplay session image) is, by precedence: NHL94_SEATS (env,
 * when set; "off" = Classic) > --seats > the launcher's Control mode setting
 * (nhl94_mode.c) > Classic. Online the HOST's image is adopted for the
 * session (GameSpec.netplay_config_adopt) and wins over all of them. */
static char s_launcher[160];
static int  s_have_launcher;
static char s_adopted[160];
static int  s_have_adopt;
static char s_local_canon[160];
static char s_last_log[256];
static int  s_adopt_fmt;

static int canon_of(const Seat *seat, int n, char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%s%s:%s", i ? "," : "",
                 seat[i].team ? "away" : "home", s_role_name[seat[i].role]);
    }
    return n;
}

/* The local list and its source name; "" = Classic. */
static const char *local_list(const char **src)
{
    const char *env = getenv("NHL94_SEATS");
    if (env && *env) { *src = "NHL94_SEATS environment"; return env; }
    if (s_have_cli) { *src = "--seats"; return s_cli; }
    if (s_have_launcher) { *src = "launcher setting"; return s_launcher; }
    *src = "default";
    return "";
}

/* A plain seat list (local sources: env, --seats, launcher; or the canon
 * of an adopted value) validated for format fmt. */
static int parse_or_classic(const char *list, Seat *out, int *n, const char *src, int fmt)
{
    *n = 0;
    if (!list || !*list || !strcmp(list, "off") || !strcmp(list, "classic")) return 1;
    int ok = parse_seats_list(list, out, n);
    for (int i = 0; ok && i < *n; i++)
        if (!nhl94_format_role_ok(fmt, out[i].role)) ok = 0;
    if (!ok) {
        fprintf(stderr, "nhl94_seats: malformed seat list \"%s\" from %s (want up to %d of "
                        "home|away:%s, no role twice) -- Classic\n",
                list, src, NHL94_SEATS_MAX, fmt == NHL94_FORMAT_4V4 ? "G|LD|LW|C (4v4)" : "G|LD|RD|LW|C|RW");
        *n = 0;
        return 0;
    }
    return 1;
}

static void load(void)
{
    if (s_parsed) return;
    s_parsed = 1;
    const char *t = getenv("NHL94_SEATS_TRACE");
    s_trace = t ? (unsigned)atoi(t) : 0u;
    const char *kc = getenv("NHL94_SEATS_KEEP_CAMERA_RULE");
    s_keep_camrule = kc && atoi(kc) > 0;
    const char *na = getenv("NHL94_SEATS_NO_AUTOLC");
    s_no_autolc = na && atoi(na) > 0;
    const char *pa = getenv("NHL94_PASSASSIST");
    s_noassist = pa && !strcmp(pa, "off");
    const char *src = NULL;
    const char *list = local_list(&src);
    Seat tmp[NHL94_SEATS_MAX];
    int n = 0;
    int fmt = nhl94_format_local();
    parse_or_classic(list, tmp, &n, src, fmt);
    canon_of(tmp, n, s_local_canon, sizeof s_local_canon);
    if (s_have_adopt) {
        src = "host's session config";
        fmt = s_adopt_fmt;
        parse_or_classic(s_adopted, tmp, &n, src, fmt);
    }
    memcpy(s_seat, tmp, sizeof tmp[0] * (size_t)n);
    s_nseat = n;
    canon_of(tmp, n, s_canon, sizeof s_canon);
    char line[256];
    snprintf(line, sizeof line, "%s%s (source: %s), format %s", n ? "Position lock " : "Classic (cartridge control)",
             s_canon, src, nhl94_format_name(fmt));
    if (strcmp(line, s_last_log)) {   /* log each change of the effective mode once */
        snprintf(s_last_log, sizeof s_last_log, "%s", line);
        fprintf(stderr, "nhl94_seats: control mode %s%s\n", line,
                n ? " -- seat i = logical player i (seats 1-4 = game pads 1-4, 5-12 = virtual "
                    "pads); a seat with no player (no device offline, empty slot online) stays AI" : "");
        if (n > GENESIS_SIM_MAX_PLAYERS)
            fprintf(stderr, "nhl94_seats: this engine carries %d pads per tick: seats %d-%d get no "
                            "input and stay AI\n", GENESIS_SIM_MAX_PLAYERS, GENESIS_SIM_MAX_PLAYERS + 1, n);
    }
}

void nhl94_seats_set_launcher(const char *list)
{
    snprintf(s_launcher, sizeof s_launcher, "%s", list ? list : "");
    s_have_launcher = 1;
    s_parsed = 0;
    load();
}

/* GameSpec.netplay_config_adopt: the session's "game" image. The guest
 * adopts the host's; restoring the local image ends the adoption. */
int nhl94_seats_adopt(const char *image)
{
    load();
    const char *k = image ? strstr(image, "nhl94.seats=") : NULL;
    char list[160] = "", seg[96] = "";
    int fmt = NHL94_FORMAT_6V6;                      /* no key: Classic 6v6 (and old images) */
    if (k) {
        k += strlen("nhl94.seats=");
        size_t len = strcspn(k, ";\n");
        if (len >= sizeof list) return 1;
        memcpy(list, k, len); list[len] = 0;
        split_builds(list, seg, sizeof seg);
        Seat tmp[NHL94_SEATS_MAX];
        int n;
        if (!parse_seats(list, tmp, &n, &fmt)) return 1;   /* cannot run it (or RD/RW in 4v4): refuse */
        canon_of(tmp, n, list, sizeof list);         /* the compact form names the same list */
    }
    if (nhl94_builds_adopt(seg)) return 1;           /* another roster table, or a bad token: refuse */
    nhl94_format_adopt(fmt);
    s_adopt_fmt = fmt != nhl94_format_local() ? fmt : 0;
    if (!strcmp(list, s_local_canon) && !s_adopt_fmt) s_have_adopt = 0;
    else { snprintf(s_adopted, sizeof s_adopted, "%s", list); s_have_adopt = 1; s_adopt_fmt = fmt; }
    s_parsed = 0;
    load();
    return 0;
}

void nhl94_seats_reload(void)
{
    s_parsed = 0;
    load();
}

/* GameSpec.netplay_seat_label: seat k's role under a host's "game" image, for
 * the waiting room ("Home C"). Reads the image only; adopts nothing. */
int nhl94_seats_label(const char *image, int seat, char *out, size_t cap)
{
    Seat tmp[NHL94_SEATS_MAX];
    char seg[96], who[64];
    int n = 0, fmt;
    if (!out || !cap || seat < 0 || !image_seats(image, tmp, &n, &fmt, seg, sizeof seg)) return 0;
    if (seat >= n) {
        /* A room with more seats than the host's Control mode has positions:
         * whoever sits here controls nobody. Say so rather than show nothing. */
        snprintf(out, cap, "No position");
        return 2;   /* a label, but no role (GameSpec.netplay_seat_label) */
    }
    if (nhl94_builds_seat_text(seg, seat, who, sizeof who))
        snprintf(out, cap, "%s %s: %s", tmp[seat].team ? "Away" : "Home", s_role_name[tmp[seat].role], who);
    else
        snprintf(out, cap, "%s %s", tmp[seat].team ? "Away" : "Home", s_role_name[tmp[seat].role]);
    return 1;
}

/* The seat's team side / role under a host image (the waiting-room picker
 * needs to know skater from goalie). */
int nhl94_seats_image_role(const char *image, int seat, int *team, int *role)
{
    Seat tmp[NHL94_SEATS_MAX];
    char seg[96];
    int n = 0, fmt;
    if (seat < 0 || !image_seats(image, tmp, &n, &fmt, seg, sizeof seg) || seat >= n) return 0;
    if (team) *team = tmp[seat].team;
    if (role) *role = tmp[seat].role;
    return 1;
}

int nhl94_seats_count(void) { load(); return s_nseat; }

int nhl94_seats_get(int i, int *team, int *role)
{
    load();
    if (i < 0 || i >= s_nseat) return 0;
    if (team) *team = s_seat[i].team;
    if (role) *role = s_seat[i].role;
    return 1;
}

int nhl94_seats_local_seat(void)
{
#if GENESIS_HAS_RECOMP_NET
    if (genesis_netplay_active()) return genesis_netplay_local_player();
#endif
    const char *v = getenv("NHL94_LOCAL_SEAT");
    return v && *v ? atoi(v) : 0;
}

int nhl94_seats_handle_arg(const char *key, const char *val)
{
    if (!key || strcmp(key, "--seats") != 0) return 0;
    if (!val) { fprintf(stderr, "--seats expects a list, e.g. home:C,away:G\n"); return 1; }
    snprintf(s_cli, sizeof s_cli, "%s", val);
    s_have_cli = 1;
    s_parsed = 0;
    load();
    return 2;
}

void nhl94_seats_config_image(char *out, size_t cap)
{
    load();
    if (!out || !cap) return;
    size_t l = strnlen(out, cap);
    /* The LOCAL choice (never an adopted host image): what this process
     * offers when it hosts. Classic writes nothing. The readable list when it
     * fits the image (up to ~4 seats in GenesisSessionConfig.game), else the
     * compact one-letter-per-seat form (parse_compact). */
    /* 4v4 rides in the value (parse_seats): "4v4", "4v4,<list>", "*4<letters>". */
    int f4 = nhl94_format_local() == NHL94_FORMAT_4V4;
    if (!s_local_canon[0]) {
        if (f4) snprintf(out + l, cap - l, "%snhl94.seats=4v4", l ? ";" : "");
        return;
    }
    /* every value with seats leads with the pass-logic marker "p<N>:" (SEATS_PASS_VER);
     * the builds segment "+B..." (nhl94_builds.c) trails it */
    char mark[8];
    snprintf(mark, sizeof mark, "p%d:", SEATS_PASS_VER);
    Seat tmp[NHL94_SEATS_MAX];
    int n = 0;
    if (!parse_seats_list(s_local_canon, tmp, &n)) return;
    /* The builds segment (nhl94_builds.c): the local tokens per seat. */
    int bt[NHL94_SEATS_MAX], br[NHL94_SEATS_MAX];
    char seg[96];
    for (int i = 0; i < n; i++) { bt[i] = tmp[i].team; br[i] = tmp[i].role; }
    nhl94_builds_suffix(bt, br, n, seg, sizeof seg);
    size_t sl = strlen(seg);
    if (l + (l ? 1u : 0u) + strlen("nhl94.seats=") + strlen(mark) + (f4 ? 4u : 0u) + strlen(s_local_canon) + sl < cap) {
        snprintf(out + l, cap - l, "%snhl94.seats=%s%s%s%s", l ? ";" : "", mark, f4 ? "4v4," : "", s_local_canon, seg);
        return;
    }
    char compact[NHL94_SEATS_MAX + 8];
    snprintf(compact, sizeof compact, "%s*", mark);
    int o = (int)strlen(compact);
    if (f4) compact[o++] = '4';
    for (int i = 0; i < n; i++) compact[o++] = (char)('a' + tmp[i].team * 6 + tmp[i].role);
    compact[o] = 0;
    snprintf(out + l, cap - l, "%snhl94.seats=%s%s", l ? ";" : "", compact, seg);
}

/* ---- guest helpers ---- */

static inline uint16_t rd16(uint32_t a) { return glue_peek16(a); }
static inline uint8_t  rd8(uint32_t a)  { return glue_peek8(a); }
static inline void     wr16(uint32_t a, uint16_t v) { glue_poke16(a, v); }
static inline void     wr8(uint32_t a, uint8_t v)   { glue_poke8(a, v); }

/* Seat k's pad player word: c1playernum.. for the game's pads, the virtual
 * pad's copy for seats 5..12. */
static uint32_t seat_player_addr(int k)
{
    return k < NHL94_GAME_PADS ? NHL94_C1PLAYERNUM + 2u * (unsigned)k
                               : NHL94_VPAD_ADDR(k) + NHL94_VPAD_PLAYER;
}

/* Control mode. CM_PLAY: every human seat plays its role (or a substitute).
 * CM_PS: a penalty shot / shootout attempt is live -- the lock stays on, but
 * the targets are the shooter and the defending goalie (ps_targets) instead
 * of the roles; before this the game's own shooter/goalie hand-over ran and
 * seats on virtual pads lost control. */
enum { CM_OFF, CM_PLAY, CM_PS };

static int ctl_mode(void)
{
    if (!nhl94_seats_count()) return CM_OFF;
    if (rd16(NHL94_GMODE) & NHL94_GMODE_HL_W) return CM_OFF;          /* other games' highlights */
    if (rd16(NHL94_SFLAGS) & NHL94_SFLAGS_HOR_W) return CM_OFF;       /* scoreboard rink */
    if (rd16(NHL94_BA_PS_FLAGS) & NHL94_BA_PS_ACTIVE_W) return CM_PS; /* attempt live */
    return CM_PLAY;
}

static int lock_active(void) { return ctl_mode() != CM_OFF; }

/* For the builds hooks (nhl94_builds.c): only regular play (CM_PLAY), never a
 * penalty shot / shootout, highlights or the scoreboard rink -- builds are
 * applied by role, and those paths re-run setplayer with parked / stale
 * positions (docs/BUILDS.md). */
int nhl94_seats_lock_active(void) { return ctl_mode() == CM_PLAY; }

/* The object (SCnum 0..11) a3 points at, or -1. */
static int a3_object(void)
{
    uint32_t a = g_cpu.A[3] & 0xFFFFFFu;
    if (a < NHL94_SORTCORDS) return -1;
    uint32_t off = a - NHL94_SORTCORDS;
    if (off % NHL94_SC_STRIDE) return -1;
    off /= NHL94_SC_STRIDE;
    return off < 12 ? (int)off : -1;
}

static inline int pos_of(int obj) { return (int16_t)rd16(NHL94_SC_ADDR((unsigned)obj) + NHL94_OFF_POSITION); }

/* Object i has a line change / bench trip pending: newpos/newpnum set and
 * different from what he is (check4bench skips pfjoycon players, so a held
 * player would never change). A pending change waits while he has the puck
 * or a penalty is pending on him (check4bench waits too). setpersonel marks
 * EVERY object of the team (newpos/newpnum); for a player who stays (same
 * roster index, same position) that is not a change. */
static int obj_pending(int i, int role)
{
    uint32_t b = NHL94_SC_ADDR((unsigned)i);
    int8_t np = (int8_t)rd8(b + NHL94_OFF_NEWPOS), nn = (int8_t)rd8(b + NHL94_OFF_NEWPNUM);
    int pending = (nn >= 0 && (uint8_t)nn != rd8(b + NHL94_OFF_PNUM)) || (np >= 0 && np != role);
    return pending && (int16_t)rd16(NHL94_PUCKC) != (int16_t)i && !(rd8(b + NHL94_OFF_PFLAGS2) & NHL94_PF2_PEN);
}

/* The seat's PRIMARY player this frame: the team's object whose position is
 * the seat's role, unless it is skating to/from the bench or box (pf2unav)
 * or a line change is pending for it -- then nobody until the role is back
 * on the ice. Once released, the game's own check4bench (AI assignment code)
 * takes him to the bench or applies the new position. *role_obj = the object
 * holding the role even when unavailable (-1: nobody has it), so another
 * seat's substitute never takes it. */
static int seat_primary(const Seat *s, int *role_obj)
{
    unsigned first = s->team ? NHL94_SC_AWAY_FIRST : NHL94_SC_HOME_FIRST;
    if (role_obj) *role_obj = -1;
    for (unsigned i = first; i < first + 6; i++) {
        uint32_t b = NHL94_SC_ADDR(i);
        if ((int16_t)rd16(b + NHL94_OFF_POSITION) != s->role) continue;
        if (role_obj) *role_obj = (int)i;
        if (rd8(b + NHL94_OFF_PFLAGS2) & NHL94_PF2_UNAV) return -1;
        if (obj_pending((int)i, s->role)) return -1;
        return (int)i;
    }
    return -1;
}

/* Substitute bodies for a seat whose role has no body (short-handed drop,
 * boxed player, pulled goalie): the role -> positions in preference order,
 * -1 ends; 6 = the extra attacker (a pulled goalie's replacement). Nearest
 * line-mate first, so a dropped RW gets C/LW rather than a defenseman. */
static const int8_t s_pref[6][6] = {
    /* G  */ { 6, 4, 3, 5, 2, 1 },
    /* LD */ { 2, 3, 4, 5, 6, -1 },
    /* RD */ { 1, 5, 4, 3, 6, -1 },
    /* LW */ { 4, 5, 1, 2, 6, -1 },
    /* C  */ { 3, 5, 1, 2, 6, -1 },
    /* RW */ { 4, 3, 2, 1, 6, -1 },
};
#define VAC_MIN 6   /* enforce calls (frames) a role is missing before a substitute is taken: debounce */

/* Object s may be seat k's substitute: on its team, a skater (never a
 * goalie body), available, not changing, and neither another seat's
 * primary (claimed) nor the body of another human's role (reserved). */
static int sub_ok(int k, int s, uint32_t claimed, uint32_t reserved)
{
    unsigned first = s_seat[k].team ? NHL94_SC_AWAY_FIRST : NHL94_SC_HOME_FIRST;
    if (s < (int)first || s >= (int)first + 6) return 0;
    if ((claimed | reserved) & (1u << s)) return 0;
    int p = pos_of(s);
    if (p < 1 || p > 6) return 0;
    if (rd8(NHL94_SC_ADDR((unsigned)s) + NHL94_OFF_PFLAGS2) & NHL94_PF2_UNAV) return 0;
    return !obj_pending(s, p);
}

/* First usable object of seat k's team at position p (lowest SC), or -1. */
static int find_sub_at(int k, int p, uint32_t claimed, uint32_t reserved)
{
    unsigned first = s_seat[k].team ? NHL94_SC_AWAY_FIRST : NHL94_SC_HOME_FIRST;
    for (unsigned i = first; i < first + 6; i++)
        if (pos_of((int)i) == p && sub_ok(k, (int)i, claimed, reserved)) return (int)i;
    return -1;
}

static int pick_sub(int k, uint32_t claimed, uint32_t reserved)
{
    for (int j = 0; j < 6 && s_pref[s_seat[k].role][j] >= 0; j++) {
        int s = find_sub_at(k, s_pref[s_seat[k].role][j], claimed, reserved);
        if (s >= 0) return s;
    }
    return -1;
}

static int play_can_resume(void)
{
    /* a substitute is only TAKEN when play can resume: the clock runs, or
     * the faceoff line-up (a whistle / penalty-shot clean-up has parked
     * bodies that look vacant for a moment) */
    return !(rd16(NHL94_GMODE) & NHL94_GMODE_CLOCK_W) || (rd16(NHL94_SFLAGS2) & NHL94_SFLAGS2_FACEOFF_W);
}

/* Seat k's target for CM_PLAY: its primary body; a vacant human seat takes a
 * sticky substitute (SEATSUB, guest RAM). All primaries are resolved first
 * so role-holders reserve their bodies; vacant seats then choose in seat
 * order, objects in index order, positions by s_pref -- deterministic.
 * prim[]/robj[] are returned for the diagnostics. */
static void role_targets(int tgt[], uint32_t humans, int prim[], int robj[])
{
    uint32_t reserved = 0, claimed = 0;
    for (int k = 0; k < NHL94_SEATS_MAX; k++) {
        int hum = k < s_nseat && k < GENESIS_SIM_MAX_PLAYERS && (humans & (1u << k));
        robj[k] = -1;
        prim[k] = hum ? seat_primary(&s_seat[k], &robj[k]) : -1;
        if (robj[k] >= 0) reserved |= 1u << robj[k];
        if (prim[k] >= 0) claimed |= 1u << prim[k];
    }
    /* A seat the session says is human but drop_join_filter holds back (it
     * joined during play / a penalty shot and takes its player at the next
     * whistle) has no body yet, but its role body is spoken for: a vacant
     * seat's sticky substitute must not sit on it, or the joiner's admission
     * would invalidate that pick at a stoppage where no new pick is allowed
     * (play_can_resume() false), leaving the substitute seat without a player. */
    uint32_t held = genesis_sim_human_mask() & ~humans;
    for (int k = 0; k < NHL94_SEATS_MAX && k < s_nseat && k < GENESIS_SIM_MAX_PLAYERS; k++) {
        int ro = -1;
        if (!(held & (1u << k))) continue;
        (void)seat_primary(&s_seat[k], &ro);
        if (ro >= 0) reserved |= 1u << ro;
    }
    int allow = play_can_resume();
    for (int k = 0; k < NHL94_SEATS_MAX; k++) {
        tgt[k] = prim[k];
        int hum = k < s_nseat && k < GENESIS_SIM_MAX_PLAYERS && (humans & (1u << k));
        if (k >= s_nseat) continue;
        if (!hum || prim[k] >= 0) {
            if (rd8(NHL94_SEATSUB(k))) wr8(NHL94_SEATSUB(k), 0);
            if (rd8(NHL94_SEATVAC(k))) wr8(NHL94_SEATVAC(k), 0);
            continue;
        }
        uint8_t v = rd8(NHL94_SEATVAC(k));
        if (v < 255) wr8(NHL94_SEATVAC(k), (uint8_t)++v);
        if (v < VAC_MIN) { wr8(NHL94_SEATSUB(k), 0); continue; }
        int s = (int)rd8(NHL94_SEATSUB(k)) - 1;
        /* sticky: the pick is kept while it is valid. A new pick only when
         * play can resume. The goalie seat always prefers the extra
         * attacker once he exists (the pulled goalie's replacement). */
        if (s >= 0 && !sub_ok(k, s, claimed, reserved)) s = -1;
        if (s_seat[k].role == NHL94_POS_G && (s < 0 || pos_of(s) != NHL94_POS_X)) {
            int x = find_sub_at(k, NHL94_POS_X, claimed, reserved);   /* available + not changing: valid at any time */
            if (x >= 0) s = x;
        }
        if (s < 0 && allow) s = pick_sub(k, claimed, reserved);
        wr8(NHL94_SEATSUB(k), (uint8_t)(s + 1));
        if (s >= 0) { tgt[k] = s; claimed |= 1u << s; }
    }
}

/* CM_PS targets: the shooter and the defending goalie only; every other
 * seat has nobody (all other bodies are parked). Chosen from guest RAM:
 *  - penalty shot: the seat on the shooting team whose role is the fouled
 *    skater's (NHL94_PSROLE, latched while the shot was pending: SetupPenaltyShot
 *    then rewrites position to C), else the C seat, else the first skater seat;
 *  - shootout (gmode2 bit 0): the shooting team's skater seats in turn,
 *    by shootoutround;
 *  - a team with only a G seat: he shoots;
 *  - the defending goalie goes to the G seat; without one, the first skater
 *    seat of that team takes him (a CPU goalie against a lone human is no
 *    fun, and the 1v1 preset used to hand the human the goalie here). */
static void ps_targets(int tgt[], uint32_t humans)
{
    for (int k = 0; k < NHL94_SEATS_MAX; k++) tgt[k] = -1;
    int bat = rd16(NHL94_BA_TEAM) & 1;
    int sk = (int16_t)rd16(NHL94_BA_SKTR_SCNUM), gk = (int16_t)rd16(NHL94_BA_GOALIE_SCNUM);
    /* The attempt's identity: shooting team, shooter object, shootout round.
     * Nonzero (bit 0), so 0 = "no latch". The seats are chosen ONCE per
     * attempt (below) and kept: re-picking every frame from the current
     * `humans` moved the shooter whenever a seat left (and, before the join
     * gate held joiners, joined) mid-attempt -- the pad of the seat that was
     * shooting was released and another took it over in the breakaway. */
    uint16_t key = (uint16_t)(1u | (unsigned)bat << 1 | (unsigned)(sk & 15) << 2 |
                              (unsigned)((rd8(NHL94_GMODE2) & 1) ? (rd16(NHL94_SHOOTOUTROUND) & 0xFF) : 0u) << 8);
    int sseat = -1, gseat = -1;
    if (rd16(NHL94_PS_LATCH_KEY) == key) {
        sseat = (int)rd8(NHL94_PS_LATCH_SHOOTER) - 1;
        gseat = (int)rd8(NHL94_PS_LATCH_GOALIE) - 1;
        /* a latched seat that has since left is nobody's: the AI finishes the attempt */
        if (sseat >= 0 && !(sseat < GENESIS_SIM_MAX_PLAYERS && (humans & (1u << sseat)))) sseat = -1;
        if (gseat >= 0 && !(gseat < GENESIS_SIM_MAX_PLAYERS && (humans & (1u << gseat)))) gseat = -1;
    } else {
        int cand[NHL94_SEATS_MAX], n = 0, fseat = -1, gfallback = -1;
        for (int k = 0; k < s_nseat; k++) {
            if (!(k < GENESIS_SIM_MAX_PLAYERS && (humans & (1u << k)))) continue;
            if (s_seat[k].team == bat) {
                if (s_seat[k].role != NHL94_POS_G) cand[n++] = k;
                else fseat = k;
            } else if (s_seat[k].role == NHL94_POS_G) {
                if (gseat < 0) gseat = k;
            } else if (gfallback < 0) gfallback = k;
        }
        if (n) {
            sseat = cand[0];
            if (rd8(NHL94_GMODE2) & 1) {                      /* shootout: rotate (round = both teams' attempt: ClearShootout / CountShootoutGoals) */
                int r = (int)rd16(NHL94_SHOOTOUTROUND);
                sseat = cand[(r > 0 ? r - 1 : 0) % n];
            } else {
                int want = (int)rd16(NHL94_PSROLE) - 1, got = -1;
                for (int i = 0; i < n && got < 0; i++) if (s_seat[cand[i]].role == want) got = cand[i];
                for (int i = 0; i < n && got < 0; i++) if (s_seat[cand[i]].role == NHL94_POS_C) got = cand[i];
                if (got >= 0) sseat = got;
            }
        } else sseat = fseat;
        if (gseat < 0) gseat = gfallback;
        wr8(NHL94_PS_LATCH_SHOOTER, (uint8_t)(sseat + 1));
        wr8(NHL94_PS_LATCH_GOALIE, (uint8_t)(gseat + 1));
        wr16(NHL94_PS_LATCH_KEY, key);
    }
    if (sseat >= 0 && sk >= 0 && sk <= 11 && pos_of(sk) >= 0) tgt[sseat] = sk;
    if (gseat >= 0 && gk >= 0 && gk <= 11 && gseat != sseat) tgt[gseat] = gk;
}

/* The attempt is over (or the lock is off): forget the latch so the next one
 * is chosen afresh. Written only on a change (Classic never writes). */
static void clear_ps_latch(void)
{
    if (rd16(NHL94_PS_LATCH_KEY)) wr16(NHL94_PS_LATCH_KEY, 0);
    if (rd8(NHL94_PS_LATCH_SHOOTER)) wr8(NHL94_PS_LATCH_SHOOTER, 0);
    if (rd8(NHL94_PS_LATCH_GOALIE)) wr8(NHL94_PS_LATCH_GOALIE, 0);
}

/* Latch the fouled skater's role while a penalty shot is pending (BA_PS_flags
 * bit 3): getBAplayerInfo has recorded the shooter at foul time and
 * SetupPenaltyShot (collide94.asm:2689) has not yet rewritten position to C.
 * enforce runs every frame through the whistle, before the attempt starts. */
static void latch_psrole(void)
{
    uint16_t f = rd16(NHL94_BA_PS_FLAGS);
    if (f & NHL94_BA_PS_ACTIVE_W) return;              /* CM_PS: keep the latch for the attempt */
    if (!(f & 0x0800u)) { if (rd16(NHL94_PSROLE)) wr16(NHL94_PSROLE, 0); return; }
    if (rd16(NHL94_PSROLE)) return;
    int sk = (int16_t)rd16(NHL94_BA_SKTR_SCNUM);
    if (sk < 0 || sk > 11) return;
    int p = pos_of(sk);
    if (p > 0 && p <= 5) wr16(NHL94_PSROLE, (uint16_t)(p + 1));
}

/* Position lock forces the cartridge's automatic line-change mode (sflags7
 * bit 4, set by GameSetUp $0F8474 when OptLine = 2): SetLCmode ($00B8F2)
 * never opens the faceoff box (for any pad), SetFaceoffComputerLine ($00F590)
 * changes every team's line at the faceoff whatever cont1team/cont2team say
 * (seats 3-12 included), and chk4lc ($00E1F4) makes no mid-play computer
 * change. A human's role body is then swapped only at faceoffs (forcepldata,
 * instantly), and the seat follows the role. Side effect of the ROM mode:
 * makepde returns full energy (no fatigue). OptLine != 0 (menu "off") is left
 * alone: then there are no line changes at all. Per frame and idempotent, so
 * snapshots, rollbacks and late session starts self-heal. */
static void force_auto_linechange(void)
{
    if (NO_AUTOLC() || rd16(NHL94_OPTLINE) != 0) return;
    uint8_t f = rd8(NHL94_SFLAGS7);
    if (!(f & NHL94_SFLAGS7_AUTOLC)) wr8(NHL94_SFLAGS7, (uint8_t)(f | NHL94_SFLAGS7_AUTOLC));
}

int nhl94_seats_control_object(int seat)
{
    load();
    if (seat < 0 || seat >= s_nseat || ctl_mode() == CM_OFF) return -1;
    return (int)rd8(NHL94_SEATTGT(seat)) - 1;
}

/* Seat k's controlled object (SCnum) or -1: the ONE accessor the pass code
 * below (aim_pass_hook, nhl94_seats_call_flags) uses for it: the lock's
 * resolved target (role body, substitute, PS shooter / goalie). */
static int seat_obj(int k)
{
    return nhl94_seats_control_object(k);
}

void nhl94_seats_human_objects(int8_t obj_seat[12])
{
    for (int i = 0; i < 12; i++) obj_seat[i] = -1;
    load();
    if (s_nseat && lock_active()) {
        /* NHL94_NAMETAGS_CPU=1 (presentation only, for demos / recordings): a
         * seat the CPU plays (offline GENESIS_SIM_TEST_ABSENT, a dropped seat)
         * is tagged on its role's player as if a human held it. Reads guest
         * RAM only, like the rest of this function. */
        static int cpu_tags = -1;
        if (cpu_tags < 0) { const char *e = getenv("NHL94_NAMETAGS_CPU"); cpu_tags = e && atoi(e) > 0; }
        for (int k = 0; k < s_nseat; k++) {
            int t = (int)rd8(NHL94_SEATTGT(k)) - 1;      /* enforce's final target: roles, substitutes, PS */
            if (t < 0 && cpu_tags) {
                t = seat_primary(&s_seat[k], NULL);
                if (t >= 0 && t <= 11 && obj_seat[t] < 0) obj_seat[t] = (int8_t)k;
                continue;
            }
            if (t < 0 || t > 11 || obj_seat[t] >= 0) continue;
            if (rd8(NHL94_SC_ADDR((unsigned)t) + NHL94_OFF_PFLAGS) & NHL94_PF_JOYCON) obj_seat[t] = (int8_t)k;
        }
        return;
    }
    for (int i = 0; i < NHL94_GAME_PADS; i++) {
        uint16_t team = rd16(NHL94_CONT1TEAM + 2u * (unsigned)i);
        int p = (int16_t)rd16(NHL94_C1PLAYERNUM + 2u * (unsigned)i);
        if ((team != 1 && team != 2) || p < 0 || p > 11 || obj_seat[p] >= 0) continue;
        if (rd8(NHL94_SC_ADDR((unsigned)p) + NHL94_OFF_PFLAGS) & NHL94_PF_JOYCON) obj_seat[p] = (int8_t)i;
    }
}

/* restorepl's release of the old pad player (logic94.asm restorepl .cont). */
static void release(unsigned i)
{
    uint32_t b = NHL94_SC_ADDR(i);
    uint8_t f = rd8(b + NHL94_OFF_PFLAGS) & (uint8_t)~NHL94_PF_JOYCON;
    if (!(rd8(b + NHL94_OFF_PFLAGS3) & NHL94_PF3_KEEPNA)) f |= NHL94_PF_NA;
    wr8(b + NHL94_OFF_PFLAGS, f);
}

static void summary(void)
{
    if (!s_nseat) return;
    fprintf(stderr, "nhl94_seats: summary frames=%lu refused_switches=%lu stray_joycon=%lu "
                    "pad_moved=%lu pad_reset=%lu goalie_cam_rule_cancelled=%lu remapped_inputs=%lu\n",
            s_frames, s_refused, s_stray, s_moved, s_reset, s_gctrl, s_remaps);
    fprintf(stderr, "nhl94_seats: summary pass_calls=%lu passed=%lu dropped=%lu "
                    "human_carrier=%lu busy=%lu b_presses_opponent_puck=%lu b_presses_loose_puck=%lu\n",
            s_calls, s_call_passes, s_call_dropped, s_call_human, s_call_busy, s_call_opp, s_call_loose);
    fprintf(stderr, "nhl94_seats: summary lines lcm_seen=%lu midplay_handover=%lu pending_released=%lu sub_frames=%lu "
                    "sub_takes=%lu vacant_nosub=%lu ps_frames=%lu sf7=%02X\n",
            s_lcm_seen, s_midplay_handover, s_pending_released, s_sub_frames, s_sub_takes, s_vacant_nosub, s_ps_frames,
            rd8(NHL94_SFLAGS7));
    for (int i = 0; i < SPC_MAX && s_spc[i].n; i++)
        fprintf(stderr, "nhl94_seats: summary setpersonel caller $%06X calls=%lu midplay=%lu\n", s_spc[i].ret, s_spc[i].n,
                s_spc[i].mid);
    for (int k = 0; k < s_nseat; k++)
        fprintf(stderr, "nhl94_seats: summary seat %d %s:%s vacant_frames=%lu player_handovers=%lu "
                        "virtual_pad_inputs=%lu\n",
                k, s_seat[k].team ? "away" : "home", s_role_name[s_seat[k].role], s_vacant[k], s_handover[k],
                s_vswaps[k]);
    fprintf(stderr, "nhl94_seats: summary aim_passes=%lu aim_changed=%lu aim_human=%lu aim_none=%lu%s\n",
            s_aim_passes, s_aim_changed, s_aim_human, s_aim_none, NO_ASSIST() ? " (NHL94_PASSASSIST=off)" : "");

}

static void trace_line(const int *target, int mode)
{
    char buf[2048];
    int l = snprintf(buf, sizeof buf, "nhl94_seats: t=%u per=%u clk=%u gmode=%04X puckc=%d",
                     genesis_sim_tick_count(), rd16(NHL94_GSP), rd16(NHL94_GAMECLOCK),
                     rd16(NHL94_GMODE), (int16_t)rd16(NHL94_PUCKC));
    l += snprintf(buf + l, sizeof buf - (size_t)l, " mode=%d optline=%u sf7=%02X line=%u/%u tmg=%d/%d ap=%u/%u",
                  mode, rd16(NHL94_OPTLINE), rd8(NHL94_SFLAGS7), rd16(NHL94_TMSTRUCT + NHL94_TM_LINE), rd16(NHL94_AWTMSTRUCT + NHL94_TM_LINE),
                  (int16_t)rd16(NHL94_TMSTRUCT + NHL94_TM_GOALIE), (int16_t)rd16(NHL94_AWTMSTRUCT + NHL94_TM_GOALIE),
                  rd16(NHL94_TMSTRUCT + NHL94_TM_AP), rd16(NHL94_AWTMSTRUCT + NHL94_TM_AP));
    unsigned joy = 0;
    for (unsigned i = 0; i < 12; i++)
        if (rd8(NHL94_SC_ADDR(i) + NHL94_OFF_PFLAGS) & NHL94_PF_JOYCON) joy |= 1u << i;
    l += snprintf(buf + l, sizeof buf - (size_t)l, " joy=%03X roles=", joy);
    for (unsigned i = 0; i < 12; i++) {   /* position digit + jersey per object, '-' off ice */
        int16_t pos = (int16_t)rd16(NHL94_SC_ADDR(i) + NHL94_OFF_POSITION);
        l += snprintf(buf + l, sizeof buf - (size_t)l, "%s%c%02X", i == 6 ? "/" : (i ? "," : ""),
                      pos < 0 ? '-' : (char)('0' + (pos > 9 ? 9 : pos)),
                      rd8(NHL94_SC_ADDR(i) + NHL94_OFF_ROSTNUM));
    }
    for (int k = 0; k < s_nseat && l < (int)sizeof buf - 80; k++) {
        int sc = target[k];
        if (sc < 0) {
            l += snprintf(buf + l, sizeof buf - (size_t)l, " | s%d %s:%s none", k,
                          s_seat[k].team ? "away" : "home", s_role_name[s_seat[k].role]);
            continue;
        }
        uint32_t b = NHL94_SC_ADDR(sc);
        l += snprintf(buf + l, sizeof buf - (size_t)l,
                      " | s%d %s:%s%s sc=%d c=%d pad=%03X x=%d y=%d vx=%d vy=%d f=%u pf=%02X #%02X",
                      k, s_seat[k].team ? "away" : "home", s_role_name[s_seat[k].role],
                      rd8(NHL94_SEATSUB(k)) ? "(sub)" : "", sc,
                      (int16_t)rd16(seat_player_addr(k)), genesis_sim_pad(k) & 0xFFF,
                      (int16_t)rd16(b + NHL94_OFF_XPOS), (int16_t)rd16(b + NHL94_OFF_YPOS),
                      (int16_t)rd16(b + NHL94_OFF_XVEL), (int16_t)rd16(b + NHL94_OFF_YVEL),
                      rd16(b + NHL94_OFF_FACEDIR), rd8(b + NHL94_OFF_PFLAGS), rd8(b + NHL94_OFF_ROSTNUM));
    }
    fprintf(stderr, "%s\n", buf);
}

/* ---- the hooks ---- */

/* Seat k's sealed pad as the game's active-low pad byte (bit 7 S, 6 A,
 * 5 C, 4 B, 3 R, 2 L, 1 D, 0 U), opposite directions resolved with the
 * game's PadDirTable. */
static uint8_t pad_raw(int k)
{
    static const uint8_t pad_dir_table[16] = { 0, 1, 2, 1, 4, 5, 6, 6, 8, 9, 0xA, 0xA, 8, 9, 0xA, 0 };
    uint8_t b = (uint8_t)(genesis_sim_pad(k) & 0xFFu);   /* GPAD_* low byte = game layout */
    return (uint8_t)~((b & 0xF0u) | pad_dir_table[b & 0x0Fu]);
}

/* A virtual pad's byte: START is masked (a pause from seat 5+ would be
 * pause 3/4, whose resume reads the real pad 3/4). */
static uint8_t vpad_raw(int k) { return (uint8_t)(pad_raw(k) | 0x80u); }

/* ---- drop-in / drop-out (docs/DROPIN.md) ----
 * A seat that left mid-match is a cleared human_mask bit in the sealed input
 * (enforce() already hands its player to the AI). Two game states still wait
 * on a human who is gone; both are guest RAM + sealed input only, so every
 * peer and every replay decides the same.
 *
 * PAUSE ORPHAN. The pause menu (Pausemode $7D00, getpzjoy) reads ONE pad: pad
 * 2 when sfpj, pad 1 otherwise, or pad 3 / 4 when pausepad says so on a 4 Way
 * Play board. START on that pad resumes (HandleMenuInput: START leaves). If
 * its seat is gone nobody can resume and the whole room sits in the menu.
 * After NHL94_DROP_PAUSE_FRAMES game frames, hold START on that pad for two
 * frames. The frame counter is guest RAM (NHL94_DROP_PAUSE_CTR). */
static void drop_pause_orphan(void)
{
    uint16_t sf = rd16(NHL94_SFLAGS), n;
    if (!(sf & NHL94_SFLAGS_PAUSE_W)) {
        if (rd16(NHL94_DROP_PAUSE_CTR)) wr16(NHL94_DROP_PAUSE_CTR, 0);
        return;
    }
    uint16_t pp = rd16(NHL94_PAUSEPAD);
    int pad = (rd16(NHL94_FOURWAYPLAY) && pp >= 3) ? (int)pp - 1 : (sf & NHL94_SFLAGS_PAUSEJ_W) ? 1 : 0;
    if (pad >= NHL94_GAME_PADS || (genesis_sim_human_mask() & (1u << pad))) {
        if (rd16(NHL94_DROP_PAUSE_CTR)) wr16(NHL94_DROP_PAUSE_CTR, 0);
        return;
    }
    n = (uint16_t)(rd16(NHL94_DROP_PAUSE_CTR) + 1u);
    if (n >= NHL94_DROP_PAUSE_FRAMES) {
        wr8(NHL94_PADRAW1 + (uint32_t)pad, (uint8_t)(rd8(NHL94_PADRAW1 + (uint32_t)pad) & ~0x80u));   /* START, active low */
        if (n >= NHL94_DROP_PAUSE_FRAMES + 2u) n = 0;
        if (n == NHL94_DROP_PAUSE_FRAMES && !genesis_multiview_speculating())
            fprintf(stderr, "nhl94_seats: t=%u pause menu orphaned (pad %d's seat left) -- resuming\n",
                    genesis_sim_tick_count(), pad + 1);
    }
    wr16(NHL94_DROP_PAUSE_CTR, n);
}

/* ReadJoyData replacement: the seats' sealed pads into PadRaw1..4. */
static void read_joy_data(void)
{
    uint16_t vseat = rd16(NHL94_VSWAP_SEAT);
    unsigned vslot = rd16(NHL94_VSWAP_SLOT);
    for (int k = 0; k < NHL94_GAME_PADS; k++) {
        uint8_t raw = pad_raw(k);
        if (vseat && k == 2 + (int)vslot) {
            /* V-blank inside a virtual-pad swap: the real pad's byte goes to
             * the parked copy, the slot keeps the virtual seat's. */
            wr8(NHL94_VSWAP_RAW, raw);
            raw = vpad_raw(vseat - 1);
        }
        wr8(NHL94_PADRAW1 + (uint32_t)k, raw);
    }
    drop_pause_orphan();
    /* ReadJoyData leaves the last pad byte read (pad 2 without 4 Way Play)
     * in d0 (ReadPad3Button: moveq #0,d0 ... not.b d0) and restores d1/a0. */
    g_cpu.D[0] = rd8(NHL94_PADRAW1 + 1u);
}

/* ---- virtual pads (seats 5..12) ----
 * updateplayers runs, for each object d6 = SCnum, pad 1's and pad 2's doinput
 * when the object is c1/c2playernum, then (.t1) pad 3's and pad 4's: each
 * swapped into pad 1 / pad 2 (team, player, B/A hold timers, pad word,
 * PadControlBits) around ReadJoy3/4 + doinput, and (.t1cont) the AI
 * assignment. A seat k >= 4 reuses that code: at .t1, when d6 is seat k's
 * player, seat k's virtual pad is put into pad 3's slots (home seat) or pad
 * 4's (away seat) -- cont3/4team, c3/4playernum, bholdtimer34,
 * aholdtimer34, padword3/4, PadControlBits34, lj3/4 and PadRaw3/4 -- with
 * the real pad 3/4 values parked in NHL94_VSWAP; at .t1cont everything is
 * swapped back. The game's own pad-3/4 block then reads that pad (ReadJoy3/4)
 * and runs doinput for the seat's player with pad 1 / pad 2's slots, exactly
 * as for a 4 Way Play pad 3/4, so the doinput hooks (remap, call for pass
 * mask) and the goalie rule apply unchanged. One object per pass and one
 * seat per object, so the real pad 3/4 is never wanted while parked. All of
 * it is guest RAM: snapshots, digests and replays carry it. */
static void vpad_swap_out(void);

static void vpad_swap_in(void)
{
    if (rd16(NHL94_VSWAP_SEAT)) vpad_swap_out();   /* a pass that skipped .t1cont: undo it first */
    if (!lock_active() || s_nseat <= NHL94_VPAD_FIRST) return;
    int16_t d6 = (int16_t)(g_cpu.D[6] & 0xFFFFu);
    if (d6 < 0 || d6 > 11) return;
    int k = NHL94_VPAD_FIRST;
    for (; k < s_nseat; k++) {
        uint32_t v = NHL94_VPAD_ADDR(k);
        if (rd16(v + NHL94_VPAD_TEAM) && (int16_t)rd16(v + NHL94_VPAD_PLAYER) == d6) break;
    }
    if (k >= s_nseat) return;
    uint32_t v = NHL94_VPAD_ADDR(k);
    unsigned slot = rd16(v + NHL94_VPAD_TEAM) == 1 ? 0u : 1u;   /* home: pad 3, away: pad 4 */
    uint32_t o = 2u * slot;
    wr16(NHL94_VSWAP_SLOT, (uint16_t)slot);
    wr16(NHL94_VSWAP_TEAM, rd16(NHL94_CONT3TEAM + o));
    wr16(NHL94_VSWAP_PLAYER, rd16(NHL94_C3PLAYERNUM + o));
    wr16(NHL94_VSWAP_BHOLD, rd16(NHL94_BHOLDTIMER34));
    wr16(NHL94_VSWAP_AHOLD, rd16(NHL94_AHOLDTIMER34));
    wr16(NHL94_VSWAP_PADWORD, rd16(NHL94_PADWORD3 + o));
    wr16(NHL94_VSWAP_PCB, rd16(NHL94_PADCONTROLBITS34));
    wr16(NHL94_VSWAP_LJ, rd16(NHL94_LJ3 + o));
    wr8(NHL94_VSWAP_RAW, rd8(NHL94_PADRAW1 + 2u + slot));
    wr16(NHL94_CONT3TEAM + o, rd16(v + NHL94_VPAD_TEAM));
    wr16(NHL94_C3PLAYERNUM + o, rd16(v + NHL94_VPAD_PLAYER));
    wr16(NHL94_BHOLDTIMER34, rd16(v + NHL94_VPAD_BHOLD));
    wr16(NHL94_AHOLDTIMER34, rd16(v + NHL94_VPAD_AHOLD));
    wr16(NHL94_PADWORD3 + o, rd16(v + NHL94_VPAD_PADWORD));
    wr16(NHL94_PADCONTROLBITS34, rd16(v + NHL94_VPAD_PCB));
    wr16(NHL94_LJ3 + o, rd16(v + NHL94_VPAD_LJ));
    wr8(NHL94_PADRAW1 + 2u + slot, vpad_raw(k));
    wr16(NHL94_VSWAP_SEAT, (uint16_t)(k + 1));
    if (!genesis_multiview_speculating()) s_vswaps[k]++;
}

static void vpad_swap_out(void)
{
    uint16_t a = rd16(NHL94_VSWAP_SEAT);
    if (!a) return;
    int k = a - 1;
    unsigned slot = rd16(NHL94_VSWAP_SLOT) & 1u;
    uint32_t o = 2u * slot;
    if (k >= NHL94_VPAD_FIRST && k < NHL94_SEATS_MAX) {
        uint32_t v = NHL94_VPAD_ADDR(k);
        wr16(v + NHL94_VPAD_TEAM, rd16(NHL94_CONT3TEAM + o));
        wr16(v + NHL94_VPAD_PLAYER, rd16(NHL94_C3PLAYERNUM + o));
        wr16(v + NHL94_VPAD_BHOLD, rd16(NHL94_BHOLDTIMER34));
        wr16(v + NHL94_VPAD_AHOLD, rd16(NHL94_AHOLDTIMER34));
        wr16(v + NHL94_VPAD_PADWORD, rd16(NHL94_PADWORD3 + o));
        wr16(v + NHL94_VPAD_PCB, rd16(NHL94_PADCONTROLBITS34));
        wr16(v + NHL94_VPAD_LJ, rd16(NHL94_LJ3 + o));
    }
    wr16(NHL94_CONT3TEAM + o, rd16(NHL94_VSWAP_TEAM));
    wr16(NHL94_C3PLAYERNUM + o, rd16(NHL94_VSWAP_PLAYER));
    wr16(NHL94_BHOLDTIMER34, rd16(NHL94_VSWAP_BHOLD));
    wr16(NHL94_AHOLDTIMER34, rd16(NHL94_VSWAP_AHOLD));
    wr16(NHL94_PADWORD3 + o, rd16(NHL94_VSWAP_PADWORD));
    wr16(NHL94_PADCONTROLBITS34, rd16(NHL94_VSWAP_PCB));
    wr16(NHL94_LJ3 + o, rd16(NHL94_VSWAP_LJ));
    wr8(NHL94_PADRAW1 + 2u + slot, rd8(NHL94_VSWAP_RAW));
    wr16(NHL94_VSWAP_SEAT, 0);
}

/* ---- call for pass ----
 * A seat presses B (or 6-button X) while an AI teammate carries the puck:
 * the request (guest RAM, NHL94_SEATREQ) asks that carrier to pass to the
 * seat's player. It is carried out by the game's own AI pass decision
 * (chk4pass, run by the carrier's assignment each time its reaction timer
 * runs out) and the game's own pass (dopass -> passto): the hooks only open
 * chk4pass's random "consider a pass" roll, make its candidate the seat's
 * player, and make dopass's receiver the seat's player. chk4pass's own
 * checks stay -- receiver on the ice, free and not in an animation lock;
 * an opponent nearer than the receiver in the pass direction (the lane
 * check), or the puck in the slot = not now, try again at the next decision
 * -- and so does passto's physics (speed from the passer's rating, lead to
 * the receiver, interceptions, one-timer set-up). Two AI caution rules are
 * waived for a call (the human asked): no pass across the offensive blue
 * line (offside risk) and no pass backward outside the attacking zone. */
#define CALL_BUTTONS  (0x10u | 0x100u)   /* GPAD_B | GPAD_X */
#define CALL_WINDOW   60                 /* game frames (~1 s) */

static int call_active(int *target, int *carrier)
{
    uint16_t t = rd16(NHL94_SEATREQ_TARGET);
    if (!t || t > 12) return 0;
    if (target) *target = t - 1;
    if (carrier) *carrier = rd16(NHL94_SEATREQ_CARRIER);
    return 1;
}

static void call_clear(void)
{
    wr16(NHL94_SEATREQ_TARGET, 0);
    wr16(NHL94_SEATREQ_CARRIER, 0);
    wr16(NHL94_SEATREQ_SEAT, 0);
    wr16(NHL94_SEATREQ_LEFT, 0);
}

static void pass_calls(const int *target, int spec)
{
    unsigned elapsed = g_cpu.D[7] & 0xFFFFu;      /* updateplayers: d7 = elapsed frames */
    if (elapsed < 1) elapsed = 1;
    if (elapsed > 8) elapsed = 8;
    int16_t puckc = (int16_t)rd16(NHL94_PUCKC);
    int tgt, car;
    if (call_active(&tgt, &car)) {
        int seat = rd16(NHL94_SEATREQ_SEAT);
        int16_t left = (int16_t)(rd16(NHL94_SEATREQ_LEFT) - elapsed);
        const char *why = NULL;
        if (seat >= s_nseat || target[seat] != tgt) why = "receiver not on the ice";
        else if (puckc != car) why = "carrier lost the puck";
        else if (left <= 0) why = "window expired";
        if (why) {
            call_clear();
            if (!spec) {
                s_call_dropped++;
                fprintf(stderr, "nhl94_seats: t=%u pass call seat %d dropped: %s "
                                "(chk4pass rolls=%u picks=%u, carrier position %d)\n",
                        genesis_sim_tick_count(), seat, why, s_dbg_gate, s_dbg_pick,
                        (int16_t)rd16(NHL94_SC_ADDR(car) + NHL94_OFF_POSITION));
            }
        } else {
            wr16(NHL94_SEATREQ_LEFT, (uint16_t)left);
        }
    }
    for (int k = 0; k < s_nseat; k++) {
        uint16_t now = (uint16_t)(genesis_sim_pad(k) & CALL_BUTTONS);
        uint16_t prev = rd16(NHL94_SEATREQ_PREV + 2u * (unsigned)k);
        wr16(NHL94_SEATREQ_PREV + 2u * (unsigned)k, now);
        if (!(now & ~prev) || target[k] < 0) continue;        /* edge only */
        if (puckc < 0 || puckc > 11 || puckc == target[k]) {   /* loose puck / own puck: B as before */
            if (!spec && puckc != target[k]) s_call_loose++;
            continue;
        }
        if ((puckc >= 6) != (s_seat[k].team == 1)) {           /* opponent's puck: B as before */
            if (!spec) s_call_opp++;
            continue;
        }
        if (call_active(NULL, NULL)) {                         /* first press wins */
            if (!spec) {
                s_call_busy++;
                if (s_trace) fprintf(stderr, "nhl94_seats: t=%u pass call seat %d ignored (another call pending)\n",
                                     genesis_sim_tick_count(), k);
            }
            continue;
        }
        if (rd8(NHL94_SC_ADDR(puckc) + NHL94_OFF_PFLAGS) & NHL94_PF_JOYCON) {
            if (!spec) {
                s_call_human++;
                if (s_trace) fprintf(stderr, "nhl94_seats: t=%u pass call seat %d: carrier SC %d is human -- nothing\n",
                                     genesis_sim_tick_count(), k, puckc);
            }
            continue;
        }
        wr16(NHL94_SEATREQ_TARGET, (uint16_t)(target[k] + 1));
        wr16(NHL94_SEATREQ_CARRIER, (uint16_t)puckc);
        wr16(NHL94_SEATREQ_SEAT, (uint16_t)k);
        wr16(NHL94_SEATREQ_LEFT, CALL_WINDOW);
        s_dbg_gate = s_dbg_pick = 0;
        if (!spec) {
            s_calls++;
            fprintf(stderr, "nhl94_seats: t=%u pass call seat %d: SC %d asks carrier SC %d\n",
                    genesis_sim_tick_count(), k, target[k], puckc);
        }
    }
}

/* chk4pass gate / pick and dopass receiver (see above). */
static void call_hook(uint32_t pc)
{
    int tgt, car;
    if (!lock_active() || !call_active(&tgt, &car) || a3_object() != car) return;
    switch (pc) {
    case NHL94_ROM_CHK4PASS_GATE:      /* the "consider a pass" roll always passes */
        g_cpu.D[0] = 0;
        s_dbg_gate++;
        break;
    case NHL94_ROM_CHK4PASS_PICK:      /* the random teammate is the caller */
        g_cpu.D[0] = (uint32_t)(tgt - (car >= 6 ? 6 : 0));
        s_dbg_pick++;
        break;
    case NHL94_ROM_CHK4PASS_BLUE:      /* the caller asked: an offside risk is his call */
        g_cpu.D[1] = (g_cpu.D[1] & 0xFFFF0000u) | (g_cpu.D[0] & 0xFFFFu);
        break;
    case NHL94_ROM_CHK4PASS_BACK:      /* ... and so is a pass back to him */
        g_cpu.D[0] = (g_cpu.D[0] & 0xFFFF0000u) | 0x59u;
        break;
    case NHL94_ROM_DOPASS_PICKED: {    /* dopass's receiver is the caller */
        uint32_t b = NHL94_SC_ADDR(tgt);
        if ((int16_t)rd16(b + NHL94_OFF_POSITION) <= 0 || (rd8(b + NHL94_OFF_PFLAGS2) & NHL94_PF2_UNAV))
            break;
        g_cpu.A[0] = (uint32_t)(int32_t)(int16_t)(uint16_t)(b & 0xFFFFu);  /* as movea.w */
        g_cpu.D[4] = 0;
        int seat = rd16(NHL94_SEATREQ_SEAT);
        call_clear();
        if (!genesis_multiview_speculating()) {
            s_call_passes++;
            fprintf(stderr, "nhl94_seats: t=%u pass call seat %d: carrier SC %d passes to SC %d "
                            "(puck %d,%d -> %d,%d)\n", genesis_sim_tick_count(), seat, car, tgt,
                    (int16_t)rd16(NHL94_PUCKX), (int16_t)rd16(NHL94_PUCKY),
                    (int16_t)rd16(b + NHL94_OFF_XPOS), (int16_t)rd16(b + NHL94_OFF_YPOS));
        }
        break;
    }
    default:
        break;
    }
}

/* TEST ONLY (NHL94_SEATS_TEST_PS=tick[:team[:role]],...): stage the state getBAplayerInfo
 * (penalty94.asm:174) leaves for a penalty shot -- BA_PS_flags bit 3, BA_Team, BA_Sktr_SCnum,
 * BA_Goalie_SCnum, the three pnum offsets -- plus a delayed Tripping call on a defending skater,
 * so the next whistle runs InProgress -> puckshootout like a real breakaway foul. A real one needs
 * a breakaway player hit with penalties on (rare in scripted play). The shooter is `team`'s
 * `role` (default C). Done bits in guest RAM ($FFDFFE) so replays and snapshots agree; it only
 * exists to exercise ps_targets and the PS control path. */
#define NHL94_PS_TESTDONE 0xFFDFFEu
typedef struct { unsigned tick; int team, role; } TestPS;
static TestPS s_tps[8];
static int s_ntps = -1;

static void test_ps(void)
{
    if (s_ntps == 0) return;                                       /* env parsed, nothing staged */
    if (s_ntps > 0 || getenv("NHL94_SEATS_TEST_PS"))
        if (!nhl94_sim_knob_allowed("NHL94_SEATS_TEST_PS")) return;   /* staged shots are sim-changing: offline only */
    if (s_ntps < 0) {
        s_ntps = 0;
        const char *e = getenv("NHL94_SEATS_TEST_PS");
        for (const char *p = e; p && *p && s_ntps < 8; ) {
            unsigned t; int team = 0, role = NHL94_POS_C;
            int n = sscanf(p, "%u:%d:%d", &t, &team, &role);
            if (n >= 1) { TestPS tp = { t, team & 1, role }; s_tps[s_ntps++] = tp; }
            p = strchr(p, ',');
            if (p) p++;
        }
        if (s_ntps) fprintf(stderr, "nhl94_seats: TEST ONLY: %d staged penalty shots (NHL94_SEATS_TEST_PS)\n", s_ntps);
    }
    if (!s_ntps || (rd16(NHL94_GMODE) & (NHL94_GMODE_CLOCK_W | NHL94_GMODE_HL_W)) || (rd16(NHL94_BA_PS_FLAGS) & 0x0C00)) return;
    uint16_t done = rd16(NHL94_PS_TESTDONE);
    for (int k = 0; k < s_ntps; k++) {
        if ((done & (1u << k)) || genesis_sim_tick_count() < s_tps[k].tick) continue;
        int sk = -1, gk = -1, ck = -1, team = s_tps[k].team;
        for (unsigned i = 0; i < 12; i++) {
            int p = pos_of((int)i);
            if ((int)(i / 6) == team && p == s_tps[k].role && sk < 0) sk = (int)i;
            if ((int)(i / 6) != team && p == 0 && gk < 0) gk = (int)i;
            if ((int)(i / 6) != team && p > 0 && ck < 0 && !(rd8(NHL94_SC_ADDR(i) + NHL94_OFF_PFLAGS2) & (NHL94_PF2_UNAV | NHL94_PF2_PEN))) ck = (int)i;
        }
        if (sk < 0 || gk < 0 || ck < 0) continue;
        unsigned slot = 0;
        while (slot < NHL94_PENBUF_CLEAR_WORDS && rd16(NHL94_PENBUF + 2u * slot)) slot++;
        if (slot >= NHL94_PENBUF_CLEAR_WORDS) continue;
        wr16(NHL94_BA_TEAM, (uint16_t)team);
        wr16(NHL94_BA_SKTR_SCNUM, (uint16_t)sk);
        wr16(NHL94_BA_GOALIE_SCNUM, (uint16_t)gk);
        wr16(NHL94_BA_SKATER_OFF, rd8(NHL94_SC_ADDR((unsigned)sk) + NHL94_OFF_PNUM));
        wr16(NHL94_BA_GOALIE_OFF, rd8(NHL94_SC_ADDR((unsigned)gk) + NHL94_OFF_PNUM));
        wr16(NHL94_BA_CHECKER_OFF, rd8(NHL94_SC_ADDR((unsigned)ck) + NHL94_OFF_PNUM));
        wr16(NHL94_PSPENALTY, NHL94_PEN_TRIPPING);
        wr16(NHL94_BA_PS_FLAGS, (uint16_t)(rd16(NHL94_BA_PS_FLAGS) | 0x0800));
        wr8(NHL94_PENBUF + 2u * slot, NHL94_PEN_TRIPPING);
        wr8(NHL94_PENBUF + 2u * slot + 1u, (uint8_t)ck);
        wr8(NHL94_DELAYEDPEN, (uint8_t)(rd8(NHL94_DELAYEDPEN) | (team ? 2u : 4u)));   /* the defending team's call */
        wr8(NHL94_SC_ADDR((unsigned)ck) + NHL94_OFF_PFLAGS2, (uint8_t)(rd8(NHL94_SC_ADDR((unsigned)ck) + NHL94_OFF_PFLAGS2) | NHL94_PF2_PEN));
        wr16(NHL94_PS_TESTDONE, (uint16_t)(done | (1u << k)));
        if (!genesis_multiview_speculating())
            fprintf(stderr, "nhl94_seats: t=%u TEST penalty shot %d: shooter SC %d (%s %s), goalie SC %d, offender SC %d\n",
                    genesis_sim_tick_count(), k, sk, team ? "away" : "home", s_role_name[s_tps[k].role], gk, ck);
        break;
    }
}

/* ---- aimed passing ($00BD5E, dopass after the receiver loop) ----
 * The game picks the receiver by distance with a lopsided octant penalty
 * (logic94.asm dopass .findplayer: a0 = best teammate, d4 = its score, -1 =
 * none -> .nopp, the puck goes into empty ice along passdir). For a HUMAN
 * passer (pfjoycon, lock active) nhl94_aim_pick replaces a0 / d4 with the
 * teammate nearest the held d-pad direction. passdir ($FFBEDC) is the last
 * direction held while B was down (setpassmode: facedir; passmode: d0 & 7,
 * AFTER remap_input rotated it), so it is already in rink coordinates and
 * views never matter. Nobody inside the cone: a0 / d4 stay the game's. The
 * pass physics (passto: speed, lead, one-timer, animation) is untouched.
 * Inputs: guest RAM + the seat list; no host state. */
static int seat_calling(int k)
{
    return (rd16(NHL94_SEATREQ_PREV + 2u * (unsigned)k) & CALL_BUTTONS) != 0;
}

static void aim_pass_hook(void)
{
    if (NO_ASSIST() || !lock_active()) return;
    int p = a3_object();
    if (p < 0 || !(rd8(NHL94_SC_ADDR((unsigned)p) + NHL94_OFF_PFLAGS) & NHL94_PF_JOYCON)) return;  /* AI: the game's */
    int dir = rd16(NHL94_PASSDIR) & 7;
    int px = (int16_t)rd16(NHL94_PUCKX), py = (int16_t)rd16(NHL94_PUCKY);
    unsigned first = p >= 6 ? 6u : 0u;
    uint8_t calling[12] = { 0 };
    int pseat = -1;
    for (int k = 0; k < s_nseat && k < NHL94_SEATS_MAX; k++) {
        int t = seat_obj(k);
        if (t < 0 || t > 11) continue;
        if (t == p) pseat = k;
        if (seat_calling(k)) calling[t] = 1;
    }
    AimCand c[6];
    int n = 0;
    for (unsigned i = first; i < first + 6; i++) {
        uint32_t b = NHL94_SC_ADDR(i);
        if ((int)i == p || (int16_t)rd16(b + NHL94_OFF_POSITION) <= 0) continue;   /* goalies / parked: as the game */
        if (rd8(b + NHL94_OFF_PFLAGS2) & NHL94_PF2_UNAV) continue;
        int hum = (rd8(b + NHL94_OFF_PFLAGS) & NHL94_PF_JOYCON) != 0;
        c[n++] = (AimCand){ (int16_t)rd16(b + NHL94_OFF_XPOS), (int16_t)rd16(b + NHL94_OFF_YPOS),
                            (uint8_t)hum, (uint8_t)(hum && calling[i]), (int8_t)i };
    }
    int score = 0;
    int j = nhl94_aim_pick(dir, px, py, c, n, &score);
    int spec = genesis_multiview_speculating();
    uint32_t ga = g_cpu.A[0] & 0xFFFFFFu;                 /* the game's pick, for the log */
    int gsc = (int32_t)g_cpu.D[4] >= 0 && ga >= NHL94_SORTCORDS && (ga - NHL94_SORTCORDS) % NHL94_SC_STRIDE == 0
              ? (int)((ga - NHL94_SORTCORDS) / NHL94_SC_STRIDE) : -1;
    if (j < 0) {
        if (!spec) {
            s_aim_passes++; s_aim_none++;
            fprintf(stderr, "nhl94_seats: t=%u aim pass seat %d SC %d dir %d game SC %d ours none (cone empty)\n",
                    genesis_sim_tick_count(), pseat, p, dir, gsc);
        }
        return;                                           /* the game's own result stands */
    }
    int o = c[j].sc;
    g_cpu.A[0] = (uint32_t)(int32_t)(int16_t)(uint16_t)(NHL94_SC_ADDR((unsigned)o) & 0xFFFFu);   /* as movea.w */
    g_cpu.D[4] = 0;
    if (!spec) {
        s_aim_passes++;
        if (o != gsc) s_aim_changed++;
        if (c[j].human) s_aim_human++;
        fprintf(stderr, "nhl94_seats: t=%u aim pass seat %d SC %d dir %d game SC %d ours SC %d score %d "
                        "human %d call %d%s\n", genesis_sim_tick_count(), pseat, p, dir, gsc, o, score,
                c[j].human, c[j].calling, o != gsc ? " CHANGED" : "");
    }
}

/* Presentation (nhl94_tags.c call icon): who is calling for the puck. Guest
 * RAM only; the same two conditions the passing code acts on:
 *   bit0 HELD   the seat's B / X is down while a teammate carries the puck
 *               (aim_pass_hook's `calling`, and the B mask in remap_input);
 *   bit1 QUEUED an AI carrier has been asked for this player (SEATREQ). */
void nhl94_seats_call_flags(uint8_t obj_flags[12])
{
    memset(obj_flags, 0, 12);
    if (!nhl94_seats_count() || !lock_active()) return;
    int puckc = (int16_t)rd16(NHL94_PUCKC);
    for (int k = 0; k < s_nseat && k < NHL94_SEATS_MAX; k++) {
        int t = seat_obj(k);
        if (t < 0 || t > 11) continue;
        if (!(rd8(NHL94_SC_ADDR((unsigned)t) + NHL94_OFF_PFLAGS) & NHL94_PF_JOYCON)) continue;   /* no human here now */
        if (seat_calling(k) && puckc >= 0 && puckc <= 11 && puckc != t && ((puckc >= 6) == (t >= 6)))
            obj_flags[t] |= 1;
        if (rd16(NHL94_SEATREQ_TARGET) == (uint16_t)(t + 1) && rd16(NHL94_SEATREQ_SEAT) == (uint16_t)k)
            obj_flags[t] |= 2;
    }
}

/* JOIN AT THE WHISTLE (stage C, NHL94 half). A seat that becomes a human while
 * play is live (a player taking over a CPU seat in a running game) must not be
 * handed its player mid-play: it would drop into a rush with no warning. It
 * takes the player at the next whistle -- the clock stopped (gmclock, sflags2
 * faceoff). "Newly human" = in human_mask but its pad has no team yet
 * (contNteam / the virtual pad's team; enforce() writes that team the frame it
 * takes the player, so a seat already playing never reads as new). Pending
 * seats are a bit in guest RAM ($FFE302) written only on a change, so games
 * where every human sits from the first faceoff (all baselines) never touch it.
 * Returns 1 when seat k may have its player this frame. */
static int drop_join_gate(int k, uint32_t humans)
{
    uint16_t pend = rd16(NHL94_DROP_JOIN_PEND), bit = (uint16_t)(1u << k);
    if (!(humans & bit)) {
        if (pend & bit) wr16(NHL94_DROP_JOIN_PEND, (uint16_t)(pend & ~bit));
        return 0;
    }
    uint32_t team_a = k < NHL94_GAME_PADS ? NHL94_CONT1TEAM + 2u * (unsigned)k
                                          : NHL94_VPAD_ADDR(k) + NHL94_VPAD_TEAM;
    if (rd16(team_a)) return 1;                                   /* already playing */
    /* Never admit a joiner while a penalty shot is pending / live or in a
     * shootout: a stopped clock is not a whistle then, and a new human would
     * re-deal the shooter / goalie (ps_targets) mid-attempt. It stays held
     * (no pending bit needed) until the attempt ends and play resumes. */
    if ((rd16(NHL94_BA_PS_FLAGS) & (NHL94_BA_PS_ACTIVE_W | 0x0800u)) || (rd8(NHL94_GMODE2) & 1)) return 0;
    if ((rd16(NHL94_GMODE) & NHL94_GMODE_CLOCK_W) || (rd16(NHL94_SFLAGS2) & NHL94_SFLAGS2_FACEOFF_W)) {
        if (pend & bit) wr16(NHL94_DROP_JOIN_PEND, (uint16_t)(pend & ~bit));
        return 1;                                                 /* whistle: take it now */
    }
    if (!(pend & bit)) {
        wr16(NHL94_DROP_JOIN_PEND, (uint16_t)(pend | bit));
        if (!genesis_multiview_speculating())
            fprintf(stderr, "nhl94_seats: t=%u seat %d joined during play -- takes its player at the next whistle\n",
                    genesis_sim_tick_count(), k);
    }
    return 0;
}

/* humans minus the seats still waiting for a whistle (the rest of enforce()
 * then treats them as nobody's: no team, no player, the AI keeps it). */
static uint32_t drop_join_filter(uint32_t humans)
{
    uint32_t out = humans;
    for (int k = 0; k < NHL94_SEATS_MAX && k < s_nseat && k < GENESIS_SIM_MAX_PLAYERS; k++)
        if (!drop_join_gate(k, humans)) out &= ~(1u << k);
    return out;
}

static void enforce(void)
{
    int mode = ctl_mode();
    if (mode != CM_PS) clear_ps_latch();
    if (mode == CM_OFF) return;
    force_auto_linechange();
    if (mode == CM_PLAY) { test_ps(); latch_psrole(); }
    int target[NHL94_SEATS_MAX], prim[NHL94_SEATS_MAX], robj[NHL94_SEATS_MAX];
    /* A seat nobody plays (no device offline, empty slot online: the
     * sealed input's human_mask) controls nobody; its player stays AI. A
     * seat beyond the engine's pads (GENESIS_SIM_MAX_PLAYERS) never has a
     * human_mask bit. */
    uint32_t humans = drop_join_filter(genesis_sim_human_mask());   /* joiners wait for a whistle */
    if (mode == CM_PS) {
        ps_targets(target, humans);
        for (int k = 0; k < NHL94_SEATS_MAX; k++) { prim[k] = target[k]; robj[k] = -1; }
    } else {
        role_targets(target, humans, prim, robj);
    }
    for (int k = 0; k < s_nseat; k++) wr8(NHL94_SEATTGT(k), (uint8_t)(target[k] + 1));

    /* What did the game do since the last enforcement? (diagnostics only) */
    int spec = genesis_multiview_speculating();
    if (!spec) {
        int first = !s_atexit;
        if (!s_atexit) { s_atexit = 1; atexit(summary); }
        s_frames++;
        for (unsigned i = 0; !first && i < 12; i++) {
            if (!(rd8(NHL94_SC_ADDR(i) + NHL94_OFF_PFLAGS) & NHL94_PF_JOYCON)) continue;
            int known = 0;
            for (int k = 0; k < s_nseat; k++)
                if ((int)i == target[k] || (int)i == s_last_target[k]) known = 1;
            if (!known) {
                s_stray++;
                fprintf(stderr, "nhl94_seats: t=%u stray pfjoycon on SC %u (released)\n",
                        genesis_sim_tick_count(), i);
            }
        }
        for (int k = 0; !first && k < s_nseat; k++) {
            int c = (int16_t)rd16(seat_player_addr(k));
            if (c == target[k] || c == s_last_target[k]) continue;
            if (c < 0) s_reset++;
            else {
                s_moved++;
                fprintf(stderr, "nhl94_seats: t=%u pad %d found on SC %d (seat target %d) -- restored\n",
                        genesis_sim_tick_count(), k + 1, c, target[k]);
            }
        }
        for (int k = 0; k < s_nseat; k++) {
            /* role vacant (bench/box trip, pulled goalie) and hand-overs to
             * the next player at the role (line change, re-slotting) */
            int rost = target[k] >= 0 ? rd8(NHL94_SC_ADDR(target[k]) + NHL94_OFF_ROSTNUM) : -1;
            if (target[k] < 0) s_vacant[k]++;
            else if (!first && rost != s_last_rost[k] && s_last_rost[k] >= 0) {
                s_handover[k]++;
                fprintf(stderr, "nhl94_seats: t=%u seat %d now on SC %d #%02X (was #%02X)\n",
                        genesis_sim_tick_count(), k, target[k], rost, s_last_rost[k]);
            }
            if (rost >= 0) s_last_rost[k] = rost;
            s_last_target[k] = target[k];
        }
        /* the line-change / substitute / penalty-shot counters (host only) */
        if (mode == CM_PS) s_ps_frames++;
        int midplay = !(rd16(NHL94_GMODE) & NHL94_GMODE_CLOCK_W) && !(rd16(NHL94_SFLAGS2) & NHL94_SFLAGS2_FACEOFF_W);
        for (unsigned i = 0; i < 12; i++)
            if (rd8(NHL94_SC_ADDR(i) + NHL94_OFF_PFLAGS2) & NHL94_PF2_LCM) { s_lcm_seen++; break; }
        for (int k = 0; mode == CM_PLAY && k < s_nseat; k++) {
            int onsub = target[k] >= 0 && target[k] != prim[k];
            if (onsub) { s_sub_frames++; if (!s_was_sub[k]) { s_sub_takes++; if (s_trace) fprintf(stderr,
                "nhl94_seats: t=%u seat %d takes substitute SC %d (pos %d, role body %d)\n",
                genesis_sim_tick_count(), k, target[k], pos_of(target[k]), robj[k]); } }
            else if (s_was_sub[k] && s_trace) fprintf(stderr, "nhl94_seats: t=%u seat %d leaves substitute -> %d\n",
                genesis_sim_tick_count(), k, target[k]);
            if (target[k] < 0 && rd8(NHL94_SEATVAC(k)) >= VAC_MIN && (humans & (1u << k))) s_vacant_nosub++;
            /* a skater seat whose role body exists, is on the ice and is
             * pending a mid-play change = a bench trip the pin would avoid */
            if (prim[k] < 0 && robj[k] >= 0 && s_seat[k].role != NHL94_POS_G && midplay &&
                !(rd8(NHL94_SC_ADDR((unsigned)robj[k]) + NHL94_OFF_PFLAGS2) & NHL94_PF2_UNAV)) s_pending_released++;
            if (midplay && !onsub && !s_was_sub[k] && prim[k] >= 0 && s_last_prim[k] >= 0 && prim[k] != s_last_prim[k] &&
                rd8(NHL94_SC_ADDR((unsigned)prim[k]) + NHL94_OFF_ROSTNUM) != rd8(NHL94_SC_ADDR((unsigned)s_last_prim[k]) + NHL94_OFF_ROSTNUM))
                s_midplay_handover++;
            s_was_sub[k] = onsub;
            s_last_prim[k] = prim[k];
        }
    }

    /* Pads 1-4 always (no seat = no team), virtual pads only for seats
     * that exist (nothing in their RAM block is touched otherwise). */
    for (int k = 0; k < NHL94_SEATS_MAX && (k < NHL94_GAME_PADS || k < s_nseat); k++) {
        uint16_t team = k < s_nseat && (humans & (1u << k)) ? (uint16_t)(s_seat[k].team + 1) : 0u;
        if (k < NHL94_GAME_PADS) {
            wr16(NHL94_CONT1TEAM + 2u * (unsigned)k, team);
        } else {
            uint32_t v = NHL94_VPAD_ADDR(k);
            /* A seat's first frame (or a team change): the B/A hold timers
             * start as the game's own reset leaves them. */
            if (rd16(v + NHL94_VPAD_TEAM) != team) {
                wr16(v + NHL94_VPAD_BHOLD, 0x1111);
                wr16(v + NHL94_VPAD_AHOLD, 0);
                wr16(v + NHL94_VPAD_PCB, 0xFFFF);
            }
            wr16(v + NHL94_VPAD_TEAM, team);
        }
        wr16(seat_player_addr(k), (uint16_t)(int16_t)target[k]);
    }
    for (unsigned i = 0; i < 12; i++) {
        int want = 0;
        for (int k = 0; k < s_nseat; k++) if (target[k] == (int)i) want = 1;
        uint32_t b = NHL94_SC_ADDR(i);
        uint8_t f = rd8(b + NHL94_OFF_PFLAGS);
        if ((f & NHL94_PF_JOYCON) && !want) release(i);
        else if (!(f & NHL94_PF_JOYCON) && want) wr8(b + NHL94_OFF_PFLAGS, (uint8_t)(f | NHL94_PF_JOYCON));
    }
    if (mode == CM_PS) {
        /* no calls for the pass during an attempt; keep the edge detector current */
        call_clear();
        for (int k = 0; k < s_nseat; k++)
            wr16(NHL94_SEATREQ_PREV + 2u * (unsigned)k, (uint16_t)(genesis_sim_pad(k) & CALL_BUTTONS));
    } else {
        pass_calls(target, spec);
    }
    if (!spec && s_trace && genesis_sim_tick_count() % s_trace == 0) trace_line(target, mode);
}

/* doinput: d0 = direction code (bits 0-3, 8 = none) | new buttons (4-7),
 * d1 = new presses, d2 = changed, d3 = held (U=0 D=1 L=2 R=3 B C A S). */
static void remap_input(void)
{
    if (!lock_active()) return;
    int i = a3_object();
    if (i < 0) return;
    int tgt;
    int mask = call_active(&tgt, NULL) && tgt == i;   /* B called for the pass: not also a B action */
    if (!mask && !NO_ASSIST()) {
        /* A teammate (human or AI) carries: B is a call for the puck, never a
         * sweep check / player switch (chgplayer -> Sweepcheck locks the
         * skater in an animation, pfalock). Only a call-for-pass press is
         * made of it: pass_calls reads the sealed pad, not these registers. */
        int puckc = (int16_t)rd16(NHL94_PUCKC);
        mask = puckc >= 0 && puckc <= 11 && puckc != i && ((puckc >= 6) == (i >= 6));
    }
    if (mask)
        for (int r = 0; r <= 3; r++) g_cpu.D[r] &= ~0x10u;
    uint8_t f = rd8(NHL94_SC_ADDR(i) + NHL94_OFF_PFLAGS);
    if (!(f & NHL94_PF_JOYCON) || (f & NHL94_PF_GOAL)) return;   /* attacks up: as is */
    uint32_t d0 = g_cpu.D[0];
    if (!(d0 & 8u)) d0 = (d0 & ~7u) | ((d0 + 4u) & 7u);
    g_cpu.D[0] = d0;
    for (int r = 1; r <= 3; r++) {
        uint32_t v = g_cpu.D[r], n = v & 0xFu;
        uint32_t m = ((n & 1u) << 1) | ((n & 2u) >> 1) | ((n & 4u) << 1) | ((n & 8u) >> 1);
        g_cpu.D[r] = (v & ~0xFu) | m;
    }
    if (!genesis_multiview_speculating()) s_remaps++;
}

static void goalie_camera_rule(void)
{
    if (!lock_active() || KEEP_CAMRULE()) return;
    int i = a3_object();
    if (i < 0) return;
    uint32_t b = NHL94_SC_ADDR(i);
    if (!(rd8(b + NHL94_OFF_PFLAGS) & NHL94_PF_JOYCON)) return;
    uint8_t f3 = rd8(b + NHL94_OFF_PFLAGS3);
    if (f3 & NHL94_PF3_GCTRL) {
        wr8(b + NHL94_OFF_PFLAGS3, (uint8_t)(f3 & ~NHL94_PF3_GCTRL));
        if (!genesis_multiview_speculating()) s_gctrl++;
    }
}

/* setpersonel entry: log who called it (return address on the guest stack).
 * Observation only -- nothing is written. */
static void setpersonel_observe(void)
{
    if (!nhl94_seats_count() || genesis_multiview_speculating()) return;
    uint32_t sp = g_cpu.A[7] & 0xFFFFFFu;
    uint32_t ret = ((uint32_t)rd16(sp) << 16 | rd16(sp + 2u)) & 0xFFFFFFu;
    int mid = !(rd16(NHL94_GMODE) & NHL94_GMODE_CLOCK_W) && !(rd16(NHL94_SFLAGS2) & NHL94_SFLAGS2_FACEOFF_W);
    for (int i = 0; i < SPC_MAX; i++) {
        if (s_spc[i].n && s_spc[i].ret != ret) continue;
        s_spc[i].ret = ret; s_spc[i].n++;
        if (mid) {
            if (!s_spc[i].mid++ || s_trace)
                fprintf(stderr, "nhl94_seats: t=%u setpersonel called MID-PLAY from $%06X (team A2=$%06X, tmgoalie h/a %d/%d)\n",
                        genesis_sim_tick_count(), ret, g_cpu.A[2] & 0xFFFFFFu,
                        (int16_t)rd16(NHL94_TMSTRUCT + NHL94_TM_GOALIE), (int16_t)rd16(NHL94_AWTMSTRUCT + NHL94_TM_GOALIE));
        }
        return;
    }
}

int nhl94_seats_instruction_hook(uint32_t pc, int *mine)
{
    *mine = 1;
    switch (pc) {
    case NHL94_ROM_READJOYDATA:
        if (!nhl94_seats_count()) return 0;
        read_joy_data();
        return 1;                                   /* replaces the routine (rts) */
    case NHL94_ROM_UPDATEPLAYERS:
        enforce();
        return 0;
    case NHL94_ROM_SETC1PLAYER:
    case NHL94_ROM_SETC2PLAYER:
        if (!lock_active()) return 0;
        if (!genesis_multiview_speculating()) s_refused++;
        return 1;                                   /* the pad keeps its player */
    case NHL94_ROM_DOINPUT:
        remap_input();
        return 0;
    case NHL94_ROM_UPD_T1:
        vpad_swap_in();
        return 0;
    case NHL94_ROM_UPD_T1CONT:
        vpad_swap_out();
        return 0;
    case NHL94_ROM_GOALIECTRL_CONT2:
        goalie_camera_rule();
        return 0;
    case NHL94_ROM_SETPERSONEL:
        setpersonel_observe();
        return 0;
    case NHL94_ROM_CHK4PASS_GATE:
    case NHL94_ROM_CHK4PASS_PICK:
    case NHL94_ROM_CHK4PASS_BLUE:
    case NHL94_ROM_CHK4PASS_BACK:
        call_hook(pc);
        return 0;
    case NHL94_ROM_DOPASS_PICKED:
        call_hook(pc);          /* an AI carrier asked by a seat: unchanged */
        aim_pass_hook();        /* a human passer: the teammate at the stick */
        return 0;
    default:
        *mine = 0;
        return 0;
    }
}
