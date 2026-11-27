/*
 * nhl94_builds.c - per-player builds for NHL Hockey '94 (docs/BUILDS.md).
 *
 * MODEL. A build is a (team, roster slot) token naming a real record of the
 * ROM's rosters. It belongs to a SEAT (the seat is locked to a role of a team
 * side, nhl94_seats.c), so it follows the role through line changes. With no
 * pick the seat plays as the team's 1st-line player for that role (ROM line
 * set 1), resolved at game start from the actual teams. The same real player
 * may be picked by any number of seats.
 *
 * SIMULATION (all of it reads guest RAM, the ROM and the session's tokens):
 *   $007806 restoreteams .r   LATCH (nhl94_builds_latch, called from
 *                             nhl94_spec.c before the 4v4 format hook): for
 *                             the team side in a2, each seat's record is
 *                             copied from the ROM into the guest block
 *                             NHL94_BLD, and rolemap[side][role] = seat + 1.
 *   $015BA6 setplayer         H1: a0 -> the build's 8 rating bytes when the
 *                             object's (team side, position) is a seat role.
 *                             setplayer then derives every attribute from
 *                             them exactly as for the ROM record (hot/cold
 *                             and the PP/PK, home/away and 3rd-period bonuses
 *                             stay those of the on-ice slot and team).
 *   $018BC8 GetPlayerNamePointer  H2 (REPLACED): banners print the build's
 *                             name for a roster slot that last played as a
 *                             build (slotmap, maintained by H1).
 * Both hooks are gated by nhl94_seats_lock_active() (no builds in the
 * intermission highlights, penalty shots / shootouts, the scoreboard rink).
 * With builds off (or no seats) nothing is written and the hooks fall through.
 *
 * Where the tokens come from (nhl94_builds_suffix, the host's image):
 *   NHL94_BUILDS env > --builds > the lobby's member table (guests' picks,
 *   rnet_lobby_set_member_data) > this machine's ini picks for the local seat.
 * Online every peer adopts the host's tokens from the session image.
 *
 *   NHL94_BUILDS=off | <item>[,<item>...]   item = <key>=<pick>
 *     key  = home:C | away:G | ... (that seat) | S (every skater seat) | G (every goalie seat)
 *     pick = default | <TEAM>:<part of the name> | <TEAM>:#<jersey>
 *     e.g. NHL94_BUILDS="home:C=LA:Gretzky,away:G=TOR:Potvin,S=ASE:Lemieux"
 *   NHL94_MEMBER_BUILD="S=BOS:Bourque;G=TOR:Potvin"  TEST: this process's lobby
 *                          member data (the picks it tells the host)
 *   NHL94_BUILDS_TRACE=1   log every seat object's rating bytes when they change
 */
#include "nhl94_builds.h"
#include "nhl94_roster_table.h"
#include "nhl94_seats.h"
#include "nhl94_mode.h"
#include "nhl94_ram.h"
#include "game_spec.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_runtime.h"
#include "genesis_host_mem.h"
#include "multiview.h"
#include "sim_step.h"
#if GENESIS_HAS_RECOMP_NET
#include "netplay/genesis_netplay.h"
#include <recomp_net/lobby_client.h>
#endif

/* ---- guest RAM block (the unused stack bottom; NHL94_SEATREQ..FMT end at
 * $FFDFE4, the deepest stack seen in play is $FFFEC9) ---- */
#define BLD            0xFFE000u
#define BLD_MAGIC      0xB1D0u
#define BLD_ROLEMAP    (BLD + 0x02)     /* u8[2][8]  seat+1 by [side][role], 0 none */
#define BLD_SLOTMAP    (BLD + 0x12)     /* u8[2][26] seat+1 by [side][pnum]: banner names */
#define BLD_CANARY     (BLD + 0x48)     /* "NHLBLD!!" */
#define BLD_ENT        (BLD + 0x50)     /* entries[12] x 48 B: [u16 len][name][8 attr] as in the ROM */
#define BLD_ENT_SIZE   48u
#define BLD_END        (BLD_ENT + 12u * BLD_ENT_SIZE)   /* $FFE290 < $FFE300 */
#define ROM_TEAMTAB    0x30Eu
/* hook sites (game.toml) */
#define NHL94_ROM_SETPLAYER_REC   0x015BA6u   /* setplayer: move.b (a0),rostnum(a3) after subq.w #pldsize,a0 */
#define NHL94_ROM_GETPLAYERNAMEPTR 0x018BC8u  /* GetPlayerNamePointer entry: movea.l tmdata(a2),a0 */
#define TM_TEAM        0x28u                  /* tmteam */
#define TM_DATA        0x1Eu                  /* tmdata */
#define TM_HOTCOLD     0x1A2u                 /* tmHotCold, 16 bytes per roster slot */

#define NH_SEATS NHL94_SEATS_MAX

static inline uint16_t rd16(uint32_t a) { return glue_peek16(a); }
static inline uint8_t  rd8(uint32_t a)  { return glue_peek8(a); }
static inline void     wr16(uint32_t a, uint16_t v) { if (glue_peek16(a) != v) glue_poke16(a, v); }
static inline void     wr8(uint32_t a, uint8_t v)   { if (glue_peek8(a) != v) glue_poke8(a, v); }
static uint32_t reg_addr(uint32_t r) { return r & 0xFFFFFFu; }

/* ---- tokens ---- */
static const char k_b32[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
static const char *const k_role[] = { "G", "LD", "RD", "LW", "C", "RW" };

int nhl94_builds_token_encode(int team, int slot, char out[3])
{
    if (team < 0 || team >= NHL_NTEAMS || slot < 0 || slot >= g_nhl_teams[team].count) return 0;
    int v = team * 32 + slot;
    out[0] = k_b32[v >> 5];
    out[1] = k_b32[v & 31];
    out[2] = 0;
    return 1;
}

static int b32(char c)
{
    const char *p = c ? strchr(k_b32, c) : NULL;
    return p ? (int)(p - k_b32) : -1;
}

/* "--" = default (-1, -1); 0 = valid token or "--", -1 = invalid. */
int nhl94_builds_token_decode(const char *two, int *team, int *slot)
{
    if (!two || !two[0] || !two[1]) return -1;
    if (two[0] == '-' && two[1] == '-') { *team = *slot = -1; return 0; }
    int a = b32(two[0]), b = b32(two[1]);
    if (a < 0 || b < 0) return -1;
    int t = a * 32 + b;
    *team = t >> 5;
    *slot = t & 31;
    if (*team >= NHL_NTEAMS || *slot >= g_nhl_teams[*team].count) return -1;
    return 0;
}

static int pidx(int team, int slot) { return (int)g_nhl_teams[team].first + slot; }
static int tok_of(int team, int slot) { return team * 32 + slot; }     /* the picker's row id */
static int is_goalie(const NhlRosterPlayer *p) { return p->flags & 1; }

static uint16_t table_id(void) { return (uint16_t)(g_nhl_table_crc & 0xFFFFu); }

/* ---- the ROM walk (what the latch does; verify_rom checks the table against it) ---- */
static uint32_t rom_team_base(int team) { return glue_peek32(ROM_TEAMTAB + 4u * (unsigned)team); }

/* Record `slot` of the roster at `base`: start (the len word) and length of
 * the record incl. its 8 rating bytes; 0 when there is no such record. */
static uint32_t rom_record(uint32_t base, int slot, unsigned *total)
{
    if (base < ROM_TEAMTAB + 4u * NHL_NTEAMS || base >= 0x100000u || slot < 0 || slot >= 26) return 0;
    uint32_t p = base + rd16(base);
    for (int i = 0;; i++) {
        unsigned ln = rd16(p);
        if (ln == 2 || ln < 4 || ln > 40) return 0;
        if (i == slot) { *total = ln + 8u; return p; }
        p += ln + 8u;
    }
}

static int rom_is_goalie(uint32_t rec) { return rd8(rec + rd16(rec) + 5) == 0; }

/* ROM line set 1: the roster slot of role 0..5 (G LD RD LW C RW), -1 if none. */
static int rom_line1_slot(uint32_t base, int role)
{
    int v = rd8(base + rd16(base + 6) + (unsigned)role);
    return v > 0 ? v - 1 : -1;
}

/* The picker lists the C table, the simulation reads the ROM: check that the
 * table is this ROM's roster (gen_roster.py emitted it from game/NHL94.bin) before the
 * waiting room offers names. A mismatch (a modded ROM, a stale table) only disables
 * the picker -- tokens resolve against the ROM in the latch whatever the table says. */
static int s_table_ok = -1;
static int table_ok(void)
{
    if (s_table_ok >= 0) return s_table_ok;
    /* The pre-boot launcher (the waiting room) runs before the cartridge is mapped: nothing to compare
     * with. The launcher has verified the ROM's CRC32 (GameSpec.expected_rom_crc32) and gen_roster.py
     * generated the table from a ROM of that CRC, so the table is right; the walk below runs in game. */
    if (rom_team_base(0) == 0) return 1;
    int ok = 1;
    for (int t = 0; t < NHL_NTEAMS && ok; t++) {
        uint32_t base = rom_team_base(t);
        for (int s = 0; s < g_nhl_teams[t].count && ok; s++) {
            const NhlRosterPlayer *p = &g_nhl_players[pidx(t, s)];
            unsigned total = 0;
            uint32_t rec = rom_record(base, s, &total);
            unsigned ln = rec ? rd16(rec) : 0;
            if (!rec || p->team != t || p->slot != s) { ok = 0; break; }
            for (unsigned i = 0; i < 8; i++) if (rd8(rec + ln + i) != p->attr[i]) ok = 0;
            for (unsigned i = 2; i < ln && rd8(rec + i); i++)
                if (rd8(rec + i) != (uint8_t)p->name[i - 2]) ok = 0;
        }
        if (ok && rom_record(base, g_nhl_teams[t].count, &(unsigned){0})) ok = 0;   /* roster ends where the table says */
    }
    s_table_ok = ok;
    if (!ok) fprintf(stderr, "nhl94_builds: the roster table does not match this ROM -- build picker disabled\n");
    return ok;
}

/* ---- name search ---- */
static int ci_eq(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
    return !*a && !*b;
}

static int ci_find(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return 1;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

static void jersey(const NhlRosterPlayer *p, char out[4])
{
    snprintf(out, 4, "%X%X", (p->attr[0] >> 4) & 0xF, p->attr[0] & 0xF);
    if (out[0] == '0') { out[0] = out[1]; out[1] = 0; }
}

/* Case-insensitive AND over words: each must be a substring of name, team
 * abbreviation, city, nickname or "#jersey". */
int nhl94_builds_filter_match(const char *filter, int pi)
{
    const NhlRosterPlayer *p = &g_nhl_players[pi];
    const NhlRosterTeam *t = &g_nhl_teams[p->team];
    char hay[200], j[4];
    jersey(p, j);
    snprintf(hay, sizeof hay, "%s|%s|%s|%s|#%s", p->name, t->abbr, t->city, t->nick, j);
    const char *f = filter ? filter : "";
    for (;;) {
        while (*f == ' ') f++;
        if (!*f) return 1;
        char w[64];
        size_t n = 0;
        while (f[n] && f[n] != ' ' && n < sizeof w - 1) n++;
        memcpy(w, f, n);
        w[n] = 0;
        f += n;
        if (!ci_find(hay, w)) return 0;
    }
}

/* ---- the picks of this machine (ini / UI) and the offline sources ---- */
static int s_pick[2] = { -1, -1 };          /* [0] skater, [1] goalie: token team*32+slot, -1 = default */

/* NHL94_BUILDS / --builds: per-seat and per-class overrides. */
typedef struct { int set, tok; } Over;
static Over s_over_seat[2][6];              /* [team][role] */
static Over s_over_class[2];                /* [0] S, [1] G */
static int  s_spec_off;
static char s_spec_cli[256];
static int  s_have_cli;
static int  s_spec_parsed;
static char s_spec_src[24];

/* "TEAM:part" / "TEAM:#NN" -> token; class: 0 skater, 1 goalie, -1 either. 0 = ok, -1 = error (reported). */
static int resolve_pick(const char *pick, int cls, int *tok)
{
    char tm[32];
    const char *c = strchr(pick, ':');
    if (!c || (size_t)(c - pick) >= sizeof tm) return -1;
    memcpy(tm, pick, (size_t)(c - pick));
    tm[c - pick] = 0;
    const char *want = c + 1;
    int team = -1;
    for (int t = 0; t < NHL_NTEAMS && team < 0; t++)
        if (ci_eq(tm, g_nhl_teams[t].abbr)) team = t;
    for (int t = 0; t < NHL_NTEAMS && team < 0; t++)
        if (ci_eq(tm, g_nhl_teams[t].city) || ci_eq(tm, g_nhl_teams[t].nick)) team = t;
    if (team < 0) { fprintf(stderr, "nhl94_builds: unknown team \"%s\"\n", tm); return -1; }
    int hit = -1, nhit = 0, exact = -1;
    for (int s = 0; s < g_nhl_teams[team].count; s++) {
        const NhlRosterPlayer *p = &g_nhl_players[pidx(team, s)];
        if (cls >= 0 && is_goalie(p) != cls) continue;
        char j[4];
        jersey(p, j);
        int ok = want[0] == '#' ? !strcmp(want + 1, j) : ci_find(p->name, want);
        if (!ok) continue;
        if (want[0] != '#' && ci_eq(p->name, want)) exact = s;
        hit = s;
        nhit++;
    }
    if (exact >= 0) { hit = exact; nhit = 1; }
    if (nhit != 1) {
        fprintf(stderr, "nhl94_builds: \"%s\" %s on %s (%s)\n", pick, nhit ? "is ambiguous" : "matches nobody",
                g_nhl_teams[team].abbr, cls == 1 ? "goalies" : cls == 0 ? "skaters" : "all");
        return -1;
    }
    *tok = tok_of(team, hit);
    return 0;
}

static void parse_spec(const char *spec, const char *src)
{
    snprintf(s_spec_src, sizeof s_spec_src, "%s", src);
    memset(s_over_seat, 0, sizeof s_over_seat);
    memset(s_over_class, 0, sizeof s_over_class);
    s_spec_off = 0;
    if (!spec || !*spec) return;
    if (!strcmp(spec, "off") || !strcmp(spec, "none") || !strcmp(spec, "0")) { s_spec_off = 1; return; }
    char buf[256];
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *item = strtok(buf, ","); item; item = strtok(NULL, ",")) {
        char *eq = strchr(item, '=');
        if (!eq) { fprintf(stderr, "nhl94_builds: malformed item \"%s\" in %s (want key=TEAM:name)\n", item, src); continue; }
        *eq++ = 0;
        int cls = -1, team = -1, role = -1;
        if (!strcmp(item, "S")) cls = 0;
        else if (!strcmp(item, "G")) cls = 1;
        else {
            char *c = strchr(item, ':');
            if (c) {
                *c++ = 0;
                team = !strcmp(item, "home") ? 0 : !strcmp(item, "away") ? 1 : -1;
                for (int r = 0; r < 6; r++) if (!strcmp(c, k_role[r])) role = r;
            }
            if (team < 0 || role < 0) { fprintf(stderr, "nhl94_builds: bad key \"%s\" in %s\n", item, src); continue; }
        }
        int tok = -1;
        Over o = { 1, -1 };
        if (strcmp(eq, "default") != 0) {
            int want = cls >= 0 ? cls : (role == 0);
            if (resolve_pick(eq, want, &tok) != 0) continue;
            o.tok = tok;
        }
        if (cls >= 0) s_over_class[cls] = o; else s_over_seat[team][role] = o;
    }
}

/* The offline sources are read when first needed and after --builds. The env
 * wins over --builds. */
static void spec_load(void)
{
    const char *env = getenv("NHL94_BUILDS");
    static char seen[256];                       /* the env is re-read when it changes (tests) */
    if (s_spec_parsed && !strcmp(seen, env ? env : "")) return;
    snprintf(seen, sizeof seen, "%s", env ? env : "");
    s_spec_parsed = 1;
    if (env && *env) parse_spec(env, "NHL94_BUILDS environment");
    else if (s_have_cli) parse_spec(s_spec_cli, "--builds");
    else parse_spec(NULL, "default");
}

int nhl94_builds_handle_arg(const char *key, const char *val)
{
    if (!key || strcmp(key, "--builds") != 0) return 0;
    if (!val) { fprintf(stderr, "--builds expects off or home:C=TEAM:name,...\n"); return 1; }
    snprintf(s_spec_cli, sizeof s_spec_cli, "%s", val);
    s_have_cli = 1;
    s_spec_parsed = 0;
    return 2;
}

/* ---- ini ---- */
static void tok_to_ini(int tok, char *out, size_t cap)
{
    if (tok < 0) { snprintf(out, cap, "default"); return; }
    const NhlRosterPlayer *p = &g_nhl_players[pidx(tok >> 5, tok & 31)];
    char nm[48];
    snprintf(nm, sizeof nm, "%s", p->name);
    for (char *c = nm; *c; c++) if (*c == ' ') *c = '_';
    snprintf(out, cap, "%s:%s", g_nhl_teams[p->team].abbr, nm);
}

int nhl94_builds_ini_key(const char *key, const char *val)
{
    int cls = !strcmp(key, "build_skater") ? 0 : !strcmp(key, "build_goalie") ? 1 : -1;
    if (cls < 0) return 0;
    char v[64];
    snprintf(v, sizeof v, "%s", val);
    for (char *c = v; *c; c++) if (*c == '_') *c = ' ';
    int tok = -1;
    if (strcmp(v, "default") != 0 && resolve_pick(v, cls, &tok) != 0) tok = -1;
    s_pick[cls] = tok;
    return 1;
}

void nhl94_builds_ini_write(FILE *f)
{
    char a[64], b[64];
    tok_to_ini(s_pick[0], a, sizeof a);
    tok_to_ini(s_pick[1], b, sizeof b);
    fprintf(f, "# per-player builds (the waiting room's picker): TEAM:First_Last or default; one for the\n"
               "# skater seats, one for the goalie seat (docs/BUILDS.md)\n"
               "build_skater = %s\nbuild_goalie = %s\n", a, b);
}

/* ---- lobby member data: what this process tells the host ---- */
static int s_announced_valid;
static char s_announced[40];

static void member_text(char *out, size_t cap)
{
    char a[3] = "--", b[3] = "--";
    int sp = s_pick[0], gp = s_pick[1];
    const char *env = getenv("NHL94_MEMBER_BUILD");       /* TEST: "S=BOS:Bourque;G=TOR:Potvin" */
    if (env && *env) {
        char buf[96];
        snprintf(buf, sizeof buf, "%s", env);
        for (char *it = strtok(buf, ";"); it; it = strtok(NULL, ";")) {
            int tok;
            if (it[0] == 'S' && it[1] == '=' && resolve_pick(it + 2, 0, &tok) == 0) sp = tok;
            if (it[0] == 'G' && it[1] == '=' && resolve_pick(it + 2, 1, &tok) == 0) gp = tok;
        }
    }
    if (sp >= 0) nhl94_builds_token_encode(sp >> 5, sp & 31, a);
    if (gp >= 0) nhl94_builds_token_encode(gp >> 5, gp & 31, b);
    snprintf(out, cap, "1;S=%s;G=%s", a, b);
}

/* Tell the lobby (and so the host) our picks; idempotent. */
void nhl94_builds_announce(void)
{
#if GENESIS_HAS_RECOMP_NET
    char t[40];
    member_text(t, sizeof t);
    if (s_announced_valid && !strcmp(t, s_announced)) return;
    s_announced_valid = 1;
    snprintf(s_announced, sizeof s_announced, "%s", t);
    (void)rnet_lobby_set_member_data(t);
#endif
}

/* "1;S=xx;G=xx" -> tokens (-1 default / absent). */
static void parse_member(const char *text, int tok[2])
{
    tok[0] = tok[1] = -1;
    if (!text || strncmp(text, "1;", 2)) return;
    for (int c = 0; c < 2; c++) {
        const char *k = strstr(text, c ? ";G=" : ";S=");
        int t, s;
        if (k && nhl94_builds_token_decode(k + 3, &t, &s) == 0 && t >= 0) tok[c] = tok_of(t, s);
    }
}

/* ---- this process's choice for a seat ---- */
static int local_token(int seat, int team, int role)
{
    spec_load();
    int cls = role == NHL94_POS_G;
    if (s_over_seat[team][role].set) return s_over_seat[team][role].tok;
    if (s_over_class[cls].set) return s_over_class[cls].tok;
#if GENESIS_HAS_RECOMP_NET
    int nm = rnet_lobby_member_count();
    for (int i = 0; i < nm; i++) {
        RNetLobbyMember m;
        if (!rnet_lobby_member_get(i, &m) || m.is_spectator || m.slot != seat) continue;
        int tok[2];
        parse_member(m.game_data, tok);
        return tok[cls];
    }
    /* In a room the member table is the whole truth, and the seat is the LOBBY
     * seat (== the sim seat: GenesisNetplayConfig.slot_port maps session slot
     * -> lobby seat, genesis_netplay_local_player returns it). A seat nobody
     * sits in (open: the CPU's) has no pick. The "local seat" fallback below
     * is for offline / LAN runs: before netplay starts it is seat 0 whatever
     * lobby seat this host holds, and handed a host in seat 1 the skater pick
     * of the empty seat 0 (or, in the gallery, of seat 0 too). */
    if (rnet_lobby_in_lobby()) return -1;
#endif
    if (seat == nhl94_seats_local_seat()) return s_pick[cls];
    return -1;
}

int nhl94_builds_enabled(void)
{
    spec_load();
    return !s_spec_off;
}

void nhl94_builds_suffix(const int *team, const int *role, int n, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = 0;
    if (n <= 0 || !nhl94_builds_enabled() || cap < 8u + 2u * (unsigned)n) return;
    int l = snprintf(out, cap, "+B%04X", (unsigned)table_id());
    for (int i = 0; i < n; i++) {
        char t[3] = "--";
        int tok = local_token(i, team[i], role[i]);
        if (tok >= 0) nhl94_builds_token_encode(tok >> 5, tok & 31, t);
        l += snprintf(out + l, cap - (size_t)l, "%s", t);
    }
}

int nhl94_builds_parse(const char *seg, int *tok, int max, int *n)
{
    *n = 0;
    if (!seg || !*seg) return 0;
    if (seg[0] != '+' || seg[1] != 'B') return 1;
    unsigned id = 0;
    for (int i = 0; i < 4; i++) {
        int d = seg[2 + i] ? (isxdigit((unsigned char)seg[2 + i]) ? (isdigit((unsigned char)seg[2 + i]) ? seg[2 + i] - '0' : toupper(seg[2 + i]) - 'A' + 10) : -1) : -1;
        if (d < 0) return 1;
        id = id * 16 + (unsigned)d;
    }
    if (id != table_id()) return 1;              /* a different roster table: refuse */
    const char *p = seg + 6;
    int k = 0;
    while (*p && *p != '+') {
        int t, s;
        if (k >= max || nhl94_builds_token_decode(p, &t, &s) != 0) return 1;
        tok[k++] = t < 0 ? -1 : tok_of(t, s);
        p += 2;
    }
    *n = k;
    return 0;
}

static int s_ad_valid, s_ad_on, s_ad_tok[NH_SEATS];

int nhl94_builds_adopt(const char *seg)
{
    int tok[NH_SEATS], n;
    if (nhl94_builds_parse(seg, tok, NH_SEATS, &n)) return 1;
    s_ad_valid = 1;
    s_ad_on = seg && *seg;
    for (int i = 0; i < NH_SEATS; i++) s_ad_tok[i] = i < n ? tok[i] : -1;
    return 0;
}

/* Effective source for the simulation: the adopted session while netplay runs. */
static int session_adopted(void)
{
#if GENESIS_HAS_RECOMP_NET
    return s_ad_valid && genesis_netplay_active();
#else
    return 0;
#endif
}

static int effective_on(void) { return session_adopted() ? s_ad_on : nhl94_builds_enabled(); }

static int effective_token(int seat, int team, int role)
{
    if (session_adopted()) return seat < NH_SEATS ? s_ad_tok[seat] : -1;
    return local_token(seat, team, role);
}

static void tok_text(int tok, char *out, size_t cap)
{
    const NhlRosterPlayer *p = &g_nhl_players[pidx(tok >> 5, tok & 31)];
    snprintf(out, cap, "%s (%s)", p->name, g_nhl_teams[p->team].abbr);
}

int nhl94_builds_seat_text(const char *seg, int seat, char *out, size_t cap)
{
    int tok[NH_SEATS], n;
    if (out && cap) out[0] = 0;
    if (nhl94_builds_parse(seg, tok, NH_SEATS, &n) || seat < 0 || seat >= n || tok[seat] < 0) return 0;
    const NhlRosterPlayer *p = &g_nhl_players[pidx(tok[seat] >> 5, tok[seat] & 31)];
    const char *last = strrchr(p->name, ' ');
    snprintf(out, cap, "%s (%s)", last ? last + 1 : p->name, g_nhl_teams[p->team].abbr);
    return 1;
}

/* ================================================================== */
/* The simulation side                                                */
/* ================================================================== */

/* Latch one seat's entry; returns 1 when a build was written. */
static int latch_seat(int seat, int side, int role, uint32_t base, int tok)
{
    unsigned total = 0;
    uint32_t rec = 0;
    int slot = -1, from_default = 0;
    if (tok >= 0) {
        int t = tok >> 5;
        slot = tok & 31;
        uint32_t tb = rom_team_base(t);
        rec = rom_record(tb, slot, &total);
        /* The class must fit the role (a goalie only in goal): evaluated on ROM bytes, so
         * every peer decides alike. A misfit falls back to the team's own player. */
        if (!rec || rom_is_goalie(rec) != (role == NHL94_POS_G)) {
            if (rec) fprintf(stderr, "nhl94_builds: seat %d %s:%s: %s is not a %s -- team default\n", seat,
                             side ? "away" : "home", k_role[role], g_nhl_players[pidx(t, slot)].name,
                             role == NHL94_POS_G ? "goalie" : "skater");
            rec = 0;
        }
    }
    if (!rec) {
        slot = rom_line1_slot(base, role);
        rec = rom_record(base, slot, &total);
        from_default = 1;
    }
    if (!rec || total > BLD_ENT_SIZE) return 0;
    uint32_t ent = BLD_ENT + BLD_ENT_SIZE * (unsigned)seat;
    for (unsigned i = 0; i < BLD_ENT_SIZE; i++) wr8(ent + i, i < total ? rd8(rec + i) : 0);
    wr8(BLD_ROLEMAP + (unsigned)side * 8u + (unsigned)role, (uint8_t)(seat + 1));
    if (!genesis_multiview_speculating()) {
        char nm[48];
        unsigned ln = rd16(rec), n = 0;
        for (unsigned i = 2; i < ln && n < sizeof nm - 1 && rd8(rec + i); i++) nm[n++] = (char)rd8(rec + i);
        nm[n] = 0;
        fprintf(stderr, "nhl94_builds: t=%u seat %d %s:%s plays as %s (roster slot %d%s) attr=", genesis_sim_tick_count(),
                seat, side ? "away" : "home", k_role[role], nm, slot, from_default ? ", team default" : "");
        for (unsigned i = 0; i < 8; i++) fprintf(stderr, "%02X", rd8(rec + ln + i));
        fprintf(stderr, "\n");
    }
    return 1;
}

static void clear_block(void)
{
    for (uint32_t a = BLD; a < BLD_END; a += 2) wr16(a, 0);
}

void nhl94_builds_latch(void)
{
    uint32_t a2 = reg_addr(g_cpu.A[2]);
    int side = a2 == NHL94_TMSTRUCT ? 0 : a2 == NHL94_AWTMSTRUCT ? 1 : -1;
    if (side < 0) return;
    int n = nhl94_seats_count();
    if (!n || !effective_on()) {
        if (rd16(BLD) == BLD_MAGIC) clear_block();        /* an earlier game's block: never leak into a new one */
        return;
    }
    /* The intermission highlights call restoreteams for OTHER games' teams
     * before gmhl is set (scoreboard94.asm StartHL2: HomeTeam / VisTeam are
     * already the highlight's, tmdata is still the real game's): the block
     * must keep the real game's builds. At the game's own start tmteam matches. */
    uint16_t want = rd16(side ? NHL94_VISTEAM : NHL94_HOMETEAM);
    if (want >= NHL_NTEAMS || rd16(a2 + TM_TEAM) != want) return;   /* tmteam */
    uint32_t base = glue_peek32(a2 + TM_DATA);               /* tmdata */
    if (base != rom_team_base(want)) return;
    wr16(BLD, BLD_MAGIC);
    static const char canary[8] = "NHLBLD!!";
    for (int i = 0; i < 8; i++) wr8(BLD_CANARY + (unsigned)i, (uint8_t)canary[i]);
    for (int r = 0; r < 8; r++) wr8(BLD_ROLEMAP + (unsigned)side * 8u + (unsigned)r, 0);
    for (int pn = 0; pn < 26; pn++) wr8(BLD_SLOTMAP + (unsigned)side * 26u + (unsigned)pn, 0);
    for (int k = 0; k < n; k++) {
        int team, role;
        if (!nhl94_seats_get(k, &team, &role) || team != side) continue;
        latch_seat(k, side, role, base, effective_token(k, team, role));
    }
}

/* ---- trace (NHL94_BUILDS_TRACE) ---- */
static int s_trace = -1;        /* NHL94_BUILDS_TRACE; read on first use (trace_objects / spec) */
static uint8_t s_last_obj[12][0x12];
static unsigned long s_h1, s_h1_applied, s_h2, s_h2_applied;
static int s_atexit;

static void summary(void)
{
    if (!s_h1 && !s_h2) return;
    fprintf(stderr, "nhl94_builds: summary setplayer_visits=%lu applied=%lu name_visits=%lu name_replaced=%lu\n",
            s_h1, s_h1_applied, s_h2, s_h2_applied);
}

static void trace_objects(void)
{
    if (s_trace < 0) {
        const char *t = getenv("NHL94_BUILDS_TRACE");
        s_trace = t && *t ? atoi(t) : 0;
    }
    if (!s_trace || genesis_multiview_speculating() || rd16(BLD) != BLD_MAGIC) return;
    for (unsigned i = 0; i < 12; i++) {
        uint32_t b = NHL94_SC_ADDR(i);
        uint8_t cur[0x12];
        cur[0] = rd8(b + NHL94_OFF_PNUM);
        cur[1] = (uint8_t)rd16(b + NHL94_OFF_POSITION);
        for (unsigned k = 0; k < 16; k++) cur[2 + k] = rd8(b + 0x66 + 1 + k);   /* +$67..+$76 */
        if (!memcmp(cur, s_last_obj[i], sizeof cur)) continue;
        memcpy(s_last_obj[i], cur, sizeof cur);
        int side = i >= 6;
        unsigned pn = cur[0];
        int hc = pn < 26 ? (int8_t)rd8((side ? NHL94_AWTMSTRUCT : NHL94_TMSTRUCT) + TM_HOTCOLD + pn * 16u) : 0;
        fprintf(stderr, "nhl94_builds: t=%u obj=%u side=%d team=%u pnum=%u pos=%d jersey=%02X hc=%d gsp=%u fields=",
                genesis_sim_tick_count(), i, side, rd16((side ? NHL94_AWTMSTRUCT : NHL94_TMSTRUCT) + TM_TEAM), pn,
                (int16_t)rd16(b + NHL94_OFF_POSITION), rd8(b + NHL94_OFF_ROSTNUM), hc, rd16(NHL94_GSP));
        for (unsigned k = 0; k < 16; k++) fprintf(stderr, "%02X", cur[2 + k]);
        fprintf(stderr, "\n");
    }
}

int nhl94_builds_instruction_hook(uint32_t pc, int *mine)
{
    *mine = 0;
    switch (pc) {
    case NHL94_ROM_SETPLAYER_REC: {
        if (rd16(BLD) != BLD_MAGIC) return 0;
        if (s_trace > 0 && !genesis_multiview_speculating() &&
            (rd16(NHL94_BA_PS_FLAGS) & (NHL94_BA_PS_ACTIVE_W | (NHL94_BA_PS_PENDING_B << 8))))
            /* docs/BUILDS.md "penalty shots": every setplayer while a penalty shot is pending or running */
            fprintf(stderr, "nhl94_builds: t=%u setplayer in a penalty shot: ba_ps=%04X pos=%d lock_active=%d\n",
                    genesis_sim_tick_count(), rd16(NHL94_BA_PS_FLAGS),
                    (int16_t)rd16(reg_addr(g_cpu.A[3]) + NHL94_OFF_POSITION), nhl94_seats_lock_active());
        if (!nhl94_seats_lock_active()) return 0;
        uint32_t a3 = reg_addr(g_cpu.A[3]);
        int role = (int16_t)rd16(a3 + NHL94_OFF_POSITION);
        int side = (rd8(a3 + NHL94_OFF_PFLAGS) & NHL94_PF_TEAM) ? 1 : 0;
        unsigned pn = rd8(a3 + NHL94_OFF_PNUM);
        unsigned e = (role >= 0 && role <= 5) ? rd8(BLD_ROLEMAP + (unsigned)side * 8u + (unsigned)role) : 0;
        if (!genesis_multiview_speculating()) {
            s_h1++;
            if (e) s_h1_applied++;
            if (!s_atexit) { s_atexit = 1; atexit(summary); }
        }
        if (pn < 26) wr8(BLD_SLOTMAP + (unsigned)side * 26u + pn, (uint8_t)e);   /* 0 clears a stale mapping */
        if (!e) return 0;
        uint32_t ent = BLD_ENT + BLD_ENT_SIZE * (e - 1u);
        g_cpu.A[0] = 0xFF000000u | (ent + rd16(ent));     /* the build's 8 rating bytes */
        *mine = 1;
        return 0;
    }
    case NHL94_ROM_GETPLAYERNAMEPTR: {
        if (rd16(BLD) != BLD_MAGIC || !nhl94_seats_lock_active()) return 0;
        uint32_t a2 = reg_addr(g_cpu.A[2]);
        int side = a2 == NHL94_TMSTRUCT ? 0 : a2 == NHL94_AWTMSTRUCT ? 1 : -1;
        unsigned idx = g_cpu.D[0] & 0xFFFFu;
        if (side < 0 || idx >= 26) return 0;
        if (!genesis_multiview_speculating()) s_h2++;
        unsigned e = rd8(BLD_SLOTMAP + (unsigned)side * 26u + idx);
        if (!e) return 0;                                  /* run the original */
        if (!genesis_multiview_speculating()) {
            s_h2_applied++;
            if (s_trace > 0) {
                char nm[48];
                unsigned ent = BLD_ENT + BLD_ENT_SIZE * (e - 1u), ln = rd16(ent), n = 0;
                for (unsigned i = 2; i < ln && n < sizeof nm - 1 && rd8(ent + i); i++) nm[n++] = (char)rd8(ent + i);
                nm[n] = 0;
                fprintf(stderr, "nhl94_builds: t=%u name request side=%d slot=%u -> %s\n", genesis_sim_tick_count(),
                        side, idx, nm);
            }
        }
        g_cpu.A[0] = 0xFF000000u | (BLD_ENT + BLD_ENT_SIZE * (e - 1u));   /* record start (len word) */
        g_cpu.D[0] = (g_cpu.D[0] & 0xFFFF0000u) | 0xFFFFu; /* the dbf loop's exit value */
        *mine = 1;
        return 1;                                          /* rts */
    }
    case NHL94_ROM_UPDATEPLAYERS:
        trace_objects();
        return 0;
    default:
        return 0;
    }
}

void nhl94_builds_seat_name(int seat, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = 0;
    int team, role;
    if (seat < 0 || seat >= NH_SEATS || rd16(BLD) != BLD_MAGIC || !nhl94_seats_get(seat, &team, &role)) return;
    if (rd8(BLD_ROLEMAP + (unsigned)team * 8u + (unsigned)role) != seat + 1) return;
    uint32_t ent = BLD_ENT + BLD_ENT_SIZE * (unsigned)seat;
    unsigned ln = rd16(ent);
    if (ln < 4 || ln > 40) return;
    char nm[48];
    unsigned n = 0, last = 0;
    for (unsigned i = 2; i < ln && n < sizeof nm - 1; i++) {
        uint8_t c = rd8(ent + i);
        if (!c) break;
        if (c == ' ') last = n + 1;
        nm[n++] = (char)c;
    }
    nm[n] = 0;
    snprintf(out, cap, "%s", nm + last);
}

/* ================================================================== */
/* Waiting-room picker                                                */
/* ================================================================== */

static int image_role(const char *image, int seat, int *team, int *role)
{
    return image && strstr(image, "+B") && table_ok() && nhl94_seats_image_role(image, seat, team, role);
}

int nhl94_builds_lobby_current(const char *image, int seat, char *out, size_t cap)
{
    int team, role;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!image_role(image, seat, &team, &role)) return 0;
    nhl94_builds_announce();
    int tok = s_pick[role == NHL94_POS_G];
    if (tok < 0) snprintf(out, cap, "Team default");
    else tok_text(tok, out, cap);
    return 1;
}

static void role_text(const NhlRosterPlayer *p, char out[8])
{
    int r = (p->flags >> 1) & 7;
    snprintf(out, 8, "%s", r < 6 ? k_role[r] : "");
}

int nhl94_builds_lobby_query(const char *image, int seat, const char *filter, struct GamePickRow *rows_, int max)
{
    GamePickRow *rows = (GamePickRow *)rows_;
    int team, role, n = 0;
    if (!image_role(image, seat, &team, &role) || !rows || max <= 0) return 0;
    int goalie = role == NHL94_POS_G;
    for (unsigned i = 0; i < g_nhl_nplayers && n < max; i++) {
        const NhlRosterPlayer *p = &g_nhl_players[i];
        if (is_goalie(p) != goalie || !nhl94_builds_filter_match(filter, (int)i)) continue;
        const NhlRosterTeam *t = &g_nhl_teams[p->team];
        GamePickRow *r = &rows[n++];
        char j[4], rl[8];
        jersey(p, j);
        role_text(p, rl);
        r->id = tok_of(p->team, p->slot);
        snprintf(r->group, sizeof r->group, "%s %s (%s)", t->city, t->nick, t->abbr);
        snprintf(r->label, sizeof r->label, "%s", p->name);
        /* Ratings on the ROM's own 0-6 scale, named. Goalies: the save table at
         * $15630 reads glove L/R = b7 lo/hi, stick L/R = b6 lo/hi. */
        if (goalie)
            snprintf(r->detail, sizeof r->detail, "#%s G  Glove L %u R %u  Stick L %u R %u  (of 6)", j,
                     p->attr[7] & 0xF, p->attr[7] >> 4, p->attr[6] & 0xF, p->attr[6] >> 4);
        else
            snprintf(r->detail, sizeof r->detail, "#%s %s  Speed %u  Off %u  Def %u  Shot %u  Check %u  (of 6)",
                     j, rl, p->attr[2] >> 4, p->attr[2] & 0xF, p->attr[3] >> 4, p->attr[3] & 0xF,
                     p->attr[4] >> 4);
    }
    return n;
}

int nhl94_builds_lobby_set(const char *image, int seat, int id)
{
    int team, role;
    if (!image_role(image, seat, &team, &role)) return -1;
    if (id >= 0) {
        int t = id >> 5, s = id & 31;
        char tk[3];
        if (!nhl94_builds_token_encode(t, s, tk)) return -1;
        if (is_goalie(&g_nhl_players[pidx(t, s)]) != (role == NHL94_POS_G)) return -1;
    }
    s_pick[role == NHL94_POS_G] = id < 0 ? -1 : id;
    nhl94_builds_announce();
    nhl94_mode_save();
    return 0;
}
