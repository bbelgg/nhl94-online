/*
 * nhl94_format.c - game format for NHL Hockey '94: 6v6 (the cartridge) or
 * 4v4 = four bodies per team, G + C + LW + LD (docs/FOUR_ON_FOUR_PLAN.md,
 * approach A).
 *
 * HOW. The game counts bodies on the ice per team in tmap (4..6) and fills
 * them from the current line in priolist order (SetPlList). 4v4 starts every
 * game with tmap = 4 and points the three priolist readers at our own list
 * G, LD, C, LW, RD, RW, X (PRIO4), and the faceoff placement at our own .apl
 * rows (APL4). Penalties follow the NHL 3-on-3 overtime rule: a penalty adds
 * a body to the OTHER team,
 *
 *     tmap(T) = 4 + clamp(boxed(other) - boxed(T), 0, 2)     (cap 6 - min(boxed(T), 2))
 *
 * (boxed = in the box, not coincidental, InProgress_ap's own test; the cap
 * keeps a free object for every player releasepl may bring back).
 *
 * HOOKS (game.toml game_hook sites). All are no-ops while the guest word
 * NHL94_FMT != 4, so 6v6 runs exactly the cartridge's code.
 *   $007806 restoreteams .r  LATCH: FMT := the effective format (host-side,
 *                            constant per session: the host's sealed image
 *                            online). 4v4: tables written, tmap(a2) = 4.
 *                            The ONLY reader of host state.
 *   $015842 SetPlList        a0 = PRIO4 (after movea.l #priolist,a0)
 *   $012A06 releasepl        a0 = PRIO4
 *   $012ECA getlinee         a0 = PRIO4
 *   $00FA7A puckfaceoff2 .t0 a1 = APL4 (after movea.l #.apl,a1)
 *   $0123DE InProgress_ap    REPLACED (rts): bclr #5 on a2's boxed entries
 *                            as the original, tmap of both teams by the rule
 *   $01284E PenGoalStuff     PenBuf cleared when the scored-on team has
 *                            nobody boxed (the stock "tmap == 6" test)
 *   $012A50 GetLowestPen     rts with d0 when the power-play team has nobody
 *                            boxed (the stock "tmap == 6" exit)
 *   $00F9D8 puckfaceoff2     after ResetBench: the rule re-applied to both
 *                            teams (faceoff normalisation: a mid-play
 *                            release's 5v5 goes back to 4v4)
 *   $01211A PenShotChk       REPLACED (rts): every minor called in a hit
 *                            becomes a penalty shot for the fouled team (no
 *                            breakaway needed, nobody boxed); see penshotchk
 *   $0157E4 setpersonel      second pass done here in C, then the stock pass
 *                            finds PlList empty. The stock pass picks a free
 *                            object with `dbpl d3` and UNDERFLOWS when the
 *                            team's last object has a request but no
 *                            position yet: the player lands on the other
 *                            team's object and is lost (a 4v4 team with two
 *                            boxed forwards took a faceoff with 3 bodies).
 *                            Same choice otherwise: the first unrequested
 *                            on-ice object, else the last unrequested one.
 * Every hook writes the table bytes before pointing a register at them
 * (idempotent), so snapshots / resets need no init path.
 *
 * DIAGNOSTICS (host-side only, never read by the simulation):
 *   NHL94_FORMAT_TRACE=<n>  every n ticks: FMT, tmap and on-ice positions per
 *                           team, PBnum; summary histogram at exit.
 *   NHL94_FORMAT_TEST_PENALTY=T:team:role|T:puck[,...]  TEST ONLY (changes the
 *                           simulation): from tick T on, at the first game
 *                           frame with the clock running and that player on
 *                           the ice, queue a Tripping minor on him exactly as
 *                           AddPenalty2 does (PenBuf entry, DelayedPen bit,
 *                           pf2pen). "puck" = the skater carrying the puck
 *                           (his team has it: called at once). Done-bits live in guest RAM
 *                           (NHL94_FMT_TESTPEN), so rollback replays agree.
 */
#include "nhl94_format.h"
#include "nhl94_ram.h"
#include "nhl94_seats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_runtime.h"
#include "genesis_host_mem.h"
#include "multiview.h"
#include "sim_step.h"

static int s_launcher = NHL94_FORMAT_6V6;
static int s_cli;            /* 0 = not given */
static int s_adopted;        /* 0 = not adopted */

int nhl94_format_parse(const char *v)
{
    if (!v) return 0;
    if (!strcmp(v, "6v6") || !strcmp(v, "6") || !strcmp(v, "off")) return NHL94_FORMAT_6V6;
    if (!strcmp(v, "4v4") || !strcmp(v, "4")) return NHL94_FORMAT_4V4;
    return 0;
}

const char *nhl94_format_name(int fmt) { return fmt == NHL94_FORMAT_4V4 ? "4v4" : "6v6"; }

int nhl94_format_local(void)
{
    const char *env = getenv("NHL94_FORMAT");
    if (env && *env) {
        int f = nhl94_format_parse(env);
        if (f) return f;
        static int warned;
        if (!warned) { warned = 1; fprintf(stderr, "nhl94_format: NHL94_FORMAT=\"%s\" unknown (6v6 | 4v4) -- ignored\n", env); }
    }
    if (s_cli) return s_cli;
    return s_launcher;
}

int nhl94_format_effective(void) { return s_adopted ? s_adopted : nhl94_format_local(); }

void nhl94_format_set_launcher(int fmt) { if (fmt) s_launcher = fmt; }

void nhl94_format_adopt(int fmt)
{
    s_adopted = fmt && fmt != nhl94_format_local() ? fmt : 0;
}

int nhl94_format_role_ok(int fmt, int role)
{
    if (fmt != NHL94_FORMAT_4V4) return role >= 0 && role <= 5;
    return role == NHL94_POS_G || role == NHL94_POS_LD || role == NHL94_POS_LW || role == NHL94_POS_C;
}

int nhl94_format_handle_arg(const char *key, const char *val)
{
    if (!key || strcmp(key, "--format") != 0) return 0;
    int f = nhl94_format_parse(val);
    if (!f) { fprintf(stderr, "--format expects 6v6 or 4v4\n"); return val ? 2 : 1; }
    s_cli = f;
    return 2;
}

/* ---- guest ---- */

static inline uint16_t rd16(uint32_t a) { return glue_peek16(a); }
static inline void     wr16(uint32_t a, uint16_t v) { glue_poke16(a, v); }
static inline void     wr8(uint32_t a, uint8_t v)   { glue_poke8(a, v); }

static int fmt4(void) { return rd16(NHL94_FMT) == NHL94_FMT_4V4; }

static void write_tables(void)
{
    static const uint8_t prio4[8] = { 0, 1, 4, 3, 2, 5, 6, 0xFF };
    /* position -> .ptab slot (0 G, 1 LD, 2 RD, 3 LW, 4 C, 5 RW, 6 X (0,-60)).
     * tmap 6: the stock row. tmap 5: the stock row (same body set as the
     * stock 5: G, LD, C, LW, RD). tmap 4: LD at the centre point, LW and C
     * on their own spots, RD on his (pulled-goalie set LD, C, LW, RD). */
    static const uint8_t apl4[24] = { 0, 1, 2, 3, 4, 5, 6, 0,
                                      0, 1, 5, 3, 4, 2, 0, 0,
                                      0, 6, 2, 3, 4, 5, 0, 0 };
    for (unsigned i = 0; i < sizeof prio4; i++) wr8(NHL94_PRIO4 + i, prio4[i]);
    for (unsigned i = 0; i < sizeof apl4; i++) wr8(NHL94_APL4 + i, apl4[i]);
}

/* Players of team struct t keeping the team short: InProgress_ap's test. */
static int boxed(uint32_t t)
{
    int n = 0;
    for (unsigned r = 0; r < NHL94_MAXROS; r++) {
        int16_t v = (int16_t)rd16(t + NHL94_TM_PDST + 2u * r);
        if (v <= 0) continue;
        if ((uint16_t)v & (NHL94_PDST_COINC | NHL94_PDST_BIT4)) continue;
        n++;
    }
    return n;
}

static void apply_rule(void)
{
    int bh = boxed(NHL94_TMSTRUCT), ba = boxed(NHL94_AWTMSTRUCT);
    for (int k = 0; k < 2; k++) {
        int bt = k ? ba : bh, bo = k ? bh : ba;
        int d = bo - bt;
        if (d < 0) d = 0;
        if (d > 2) d = 2;
        int ap = 4 + d, cap = 6 - (bt > 2 ? 2 : bt);
        if (ap > cap) ap = cap;
        if (ap < 4) ap = 4;
        wr16((k ? NHL94_AWTMSTRUCT : NHL94_TMSTRUCT) + NHL94_TM_AP, (uint16_t)ap);
    }
}

static uint32_t reg_addr(uint32_t r) { return r & 0xFFFFFFu; }

/* ---- diagnostics ---- */
static unsigned s_trace = (unsigned)-1;
static unsigned long s_hist[3][3], s_lines, s_rdrw_even, s_rdrw_at4, s_mismatch_fo;
static int s_atexit;
/* 4v4 hook visits outside view speculation (log only): restoreteams latch,
 * priolist x3, apl, InProgress_ap, PenGoalStuff, GetLowestPen (+ rts taken),
 * faceoff normalisation, setpersonel pass 2 (+ players placed). */
enum { H_LATCH, H_SETPL, H_RELEASE, H_GETLINEE, H_APL, H_INPROG, H_PENGOAL, H_PENGOAL_CLR, H_LOWPEN,
       H_LOWPEN_RTS, H_NORM, H_PASS2, H_PASS2_PLACED, H_PENSHOT, H_PENSHOT_SET, H_N };
static unsigned long s_hits[H_N];
static const char *const s_hit_name[H_N] = { "restoreteams_latch4", "setpllist", "releasepl", "getlinee",
    "faceoff_apl", "inprogress_ap", "pengoal", "pengoal_cleared", "lowestpen", "lowestpen_rts",
    "faceoff_norm", "setpersonel_pass2", "pass2_placed", "penshotchk", "penshot_awarded" };
static void hit(int h) { if (!genesis_multiview_speculating()) s_hits[h]++; }

static void summary(void)
{
    fprintf(stderr, "nhl94_format: summary traced=%lu rdrw_at_even_strength=%lu rdrw_on_team_at_4=%lu "
                    "onice_ne_tmap_at_faceoff=%lu tmap(home,away):", s_lines, s_rdrw_even, s_rdrw_at4, s_mismatch_fo);
    for (int h = 0; h < 3; h++)
        for (int a = 0; a < 3; a++)
            if (s_hist[h][a]) fprintf(stderr, " %dv%d=%lu", h + 4, a + 4, s_hist[h][a]);
    fprintf(stderr, "\nnhl94_format: summary hooks:");
    for (int h = 0; h < H_N; h++) fprintf(stderr, " %s=%lu", s_hit_name[h], s_hits[h]);
    fprintf(stderr, "\n");
}

static void trace(void)
{
    if (s_trace == (unsigned)-1) {
        const char *t = getenv("NHL94_FORMAT_TRACE");
        s_trace = t ? (unsigned)atoi(t) : 0u;
    }
    if (!s_trace || genesis_multiview_speculating() || genesis_sim_tick_count() % s_trace) return;
    if (rd16(NHL94_GMODE) & NHL94_GMODE_HL_W) return;
    if (!s_atexit) { s_atexit = 1; atexit(summary); }
    static const char *const nm[] = { "G", "LD", "RD", "LW", "C", "RW", "X" };
    char buf[400];
    int ap[2] = { (int16_t)rd16(NHL94_TMSTRUCT + NHL94_TM_AP), (int16_t)rd16(NHL94_AWTMSTRUCT + NHL94_TM_AP) };
    int l = snprintf(buf, sizeof buf, "nhl94_format: t=%u fmt=%u per=%u clk=%u fo=%d tmap=%d/%d pb=%02X ba=%04X gm=%04X",
                     genesis_sim_tick_count(), rd16(NHL94_FMT), rd16(NHL94_GSP), rd16(NHL94_GAMECLOCK),
                     !!(rd16(NHL94_SFLAGS2) & NHL94_SFLAGS2_FACEOFF_W), ap[0], ap[1], glue_peek8(NHL94_PBNUM), rd16(NHL94_BA_PS_FLAGS), rd16(NHL94_GMODE));
    int rdrw[2] = { 0, 0 }, cnt[2] = { 0, 0 }, hasg[2] = { 0, 0 };
    for (int t = 0; t < 2; t++) {
        l += snprintf(buf + l, sizeof buf - (size_t)l, " %s=", t ? "away" : "home");
        static const int order[] = { 0, 1, 4, 3, 2, 5, 6 };
        for (int oi = 0; oi < 7; oi++)
            for (unsigned i = 6u * (unsigned)t; i < 6u * (unsigned)t + 6u; i++) {
                int16_t p = (int16_t)rd16(NHL94_SC_ADDR(i) + NHL94_OFF_POSITION);
                if (p != order[oi]) continue;
                cnt[t]++;
                if (p == NHL94_POS_G) hasg[t] = 1;
                if (p == NHL94_POS_RD || p == NHL94_POS_RW) rdrw[t]++;
                l += snprintf(buf + l, sizeof buf - (size_t)l, "%s%s", cnt[t] > 1 ? "," : "", nm[p]);
            }
    }
    fprintf(stderr, "%s\n", buf);
    s_lines++;
    if (ap[0] >= 4 && ap[0] <= 6 && ap[1] >= 4 && ap[1] <= 6) s_hist[ap[0] - 4][ap[1] - 4]++;
    if (fmt4()) {
        for (int t = 0; t < 2; t++) {
            /* a pulled goalie's 4 are LD, C, LW, RD (PRIO4 + 1): expected */
            if ((int16_t)rd16((t ? NHL94_AWTMSTRUCT : NHL94_TMSTRUCT) + NHL94_TM_GOALIE) < 0) continue;
            /* a penalty shot (puckpenshot) benches the shooter's goalie and
             * every other skater; they skate off, any position, with the
             * clock stopped until the next faceoff: expected */
            if (!hasg[t] && (rd16(NHL94_GMODE) & NHL94_GMODE_CLOCK_W)) continue;
            if (rdrw[t] && ap[0] == ap[1] && ap[0] == 4) s_rdrw_even++;
            if (rdrw[t] && ap[t] == 4) s_rdrw_at4++;
        }
        if ((rd16(NHL94_SFLAGS2) & NHL94_SFLAGS2_FACEOFF_W) && (cnt[0] != ap[0] || cnt[1] != ap[1])) {
            s_mismatch_fo++;
            fprintf(stderr, "nhl94_format: t=%u ASSERT on-ice %d/%d != tmap %d/%d at a faceoff\n",
                    genesis_sim_tick_count(), cnt[0], cnt[1], ap[0], ap[1]);
            for (int t = 0; t < 2; t++) {
                if (cnt[t] == ap[t]) continue;
                uint32_t ts = t ? NHL94_AWTMSTRUCT : NHL94_TMSTRUCT;
                char d[512];
                int dl = snprintf(d, sizeof d, "nhl94_format:   %s objects (pos/pnum/newpos/newpnum/pf2):", t ? "away" : "home");
                for (unsigned i = 6u * (unsigned)t; i < 6u * (unsigned)t + 6u; i++) {
                    uint32_t ob = NHL94_SC_ADDR(i);
                    dl += snprintf(d + dl, sizeof d - (size_t)dl, " %d/%d/%d/%d/%02X", (int16_t)rd16(ob + NHL94_OFF_POSITION),
                                   (int8_t)glue_peek8(ob + NHL94_OFF_PNUM), (int8_t)glue_peek8(ob + NHL94_OFF_NEWPOS),
                                   (int8_t)glue_peek8(ob + NHL94_OFF_NEWPNUM), glue_peek8(ob + NHL94_OFF_PFLAGS2));
                }
                dl += snprintf(d + dl, sizeof d - (size_t)dl, " line=%u goalie=%d pdst:", rd16(ts + NHL94_TM_LINE),
                               (int16_t)rd16(ts + NHL94_TM_GOALIE));
                for (unsigned r = 0; r < NHL94_MAXROS && dl < (int)sizeof d - 8; r++) {
                    int16_t v = (int16_t)rd16(ts + NHL94_TM_PDST + 2u * r);
                    if (v != -2) dl += snprintf(d + dl, sizeof d - (size_t)dl, " %u:%X", r + 1, (uint16_t)v);
                }
                dl += snprintf(d + dl, sizeof d - (size_t)dl, " lines0-2:");
                for (unsigned j = 0; j < 24 && dl < (int)sizeof d - 4; j++)
                    dl += snprintf(d + dl, sizeof d - (size_t)dl, "%s%02X", j % 8 ? "" : " ", glue_peek8(ts + 0x16Au + j));
                fprintf(stderr, "%s\n", d);
            }
            fprintf(stderr, "nhl94_format:   last PlList %02X %02X %02X %02X %02X %02X plpos %02X %02X %02X %02X %02X %02X\n",
                    glue_peek8(0xFFCF14u), glue_peek8(0xFFCF15u), glue_peek8(0xFFCF16u), glue_peek8(0xFFCF17u),
                    glue_peek8(0xFFCF18u), glue_peek8(0xFFCF19u), glue_peek8(0xFFCF1Au), glue_peek8(0xFFCF1Bu),
                    glue_peek8(0xFFCF1Cu), glue_peek8(0xFFCF1Du), glue_peek8(0xFFCF1Eu), glue_peek8(0xFFCF1Fu));
        }
    }
}

/* ---- test-only penalty injection ---- */
typedef struct { unsigned tick; int team, role; } TestPen;
static TestPen s_tp[16];
static int s_ntp = -1;

static void test_penalties(void)
{
    if (s_ntp == 0) return;                                       /* env parsed, nothing injected */
    if (s_ntp > 0 || getenv("NHL94_FORMAT_TEST_PENALTY"))
        if (!nhl94_sim_knob_allowed("NHL94_FORMAT_TEST_PENALTY")) return;   /* sim-changing: offline only */
    if (s_ntp < 0) {
        s_ntp = 0;
        const char *e = getenv("NHL94_FORMAT_TEST_PENALTY");
        static const char *const rn[] = { "G", "LD", "RD", "LW", "C", "RW" };
        for (const char *p = e; p && *p && s_ntp < 16; ) {
            char team[8], role[4];
            unsigned t;
            if (sscanf(p, "%u:%7[a-z]", &t, team) == 2 && !strcmp(team, "puck")) {
                TestPen tp = { t, -1, -1 };
                s_tp[s_ntp++] = tp;
            } else if (sscanf(p, "%u:%7[a-z]:%3[A-Z]", &t, team, role) == 3) {
                TestPen tp = { t, !strcmp(team, "away"), -1 };
                for (int i = 0; i < 6; i++) if (!strcmp(role, rn[i])) tp.role = i;
                if (tp.role >= 0) s_tp[s_ntp++] = tp;
            }
            p = strchr(p, ',');
            if (p) p++;
        }
        if (s_ntp) fprintf(stderr, "nhl94_format: TEST ONLY: %d injected penalties (NHL94_FORMAT_TEST_PENALTY)\n", s_ntp);
    }
    if (!s_ntp || (rd16(NHL94_GMODE) & (NHL94_GMODE_CLOCK_W | NHL94_GMODE_HL_W))) return;
    uint16_t done = rd16(NHL94_FMT_TESTPEN);
    for (int k = 0; k < s_ntp; k++) {
        if ((done & (1u << k)) || genesis_sim_tick_count() < s_tp[k].tick) continue;
        int team = s_tp[k].team, carrier = -1;
        if (team < 0) {
            carrier = (int16_t)rd16(NHL94_PUCKC);
            if (carrier < 0 || carrier > 11 || (int16_t)rd16(NHL94_SC_ADDR(carrier) + NHL94_OFF_POSITION) <= 0) continue;
            team = carrier >= 6;
        }
        for (unsigned i = 6u * (unsigned)team; i < 6u * (unsigned)team + 6u; i++) {
            uint32_t b = NHL94_SC_ADDR(i);
            if (carrier >= 0 ? (int)i != carrier : (int16_t)rd16(b + NHL94_OFF_POSITION) != s_tp[k].role) continue;
            uint8_t f2 = glue_peek8(b + NHL94_OFF_PFLAGS2);
            if (f2 & (NHL94_PF2_UNAV | NHL94_PF2_PEN)) break;
            unsigned slot = 0;
            while (slot < NHL94_PENBUF_CLEAR_WORDS && rd16(NHL94_PENBUF + 2u * slot)) slot++;
            if (slot >= NHL94_PENBUF_CLEAR_WORDS) break;
            wr8(NHL94_PENBUF + 2u * slot, NHL94_PEN_TRIPPING);
            wr8(NHL94_PENBUF + 2u * slot + 1u, (uint8_t)i);
            wr8(NHL94_DELAYEDPEN, (uint8_t)(glue_peek8(NHL94_DELAYEDPEN) | (team ? 4u : 2u)));
            wr8(b + NHL94_OFF_PFLAGS2, (uint8_t)(f2 | NHL94_PF2_PEN));
            done |= (uint16_t)(1u << k);
            wr16(NHL94_FMT_TESTPEN, done);
            if (!genesis_multiview_speculating())
                fprintf(stderr, "nhl94_format: t=%u TEST penalty %d: Tripping on %s SC %u\n",
                        genesis_sim_tick_count(), k, team ? "away" : "home", i);
            break;
        }
    }
}

/* setpersonel's second pass (see the header), within the team's 6 objects. */
static void setpersonel_pass2(void)
{
    uint32_t first = 0xFF0000u | rd16(reg_addr(g_cpu.A[2]) + NHL94_TM_SORT);
    if (first < NHL94_SORTCORDS || first > NHL94_SC_ADDR(6)) return;   /* not a team: leave it to the ROM */
    for (int k = 5; k >= 0; k--) {
        uint8_t want = glue_peek8(NHL94_PLLIST + (uint32_t)k);
        if (!want) continue;
        uint32_t pick = 0;
        for (unsigned i = 0; i < 6; i++) {
            uint32_t b = first + i * NHL94_SC_STRIDE;
            if ((int8_t)glue_peek8(b + NHL94_OFF_NEWPNUM) >= 0) continue;
            pick = b;
            if ((int16_t)rd16(b + NHL94_OFF_POSITION) >= 0) break;
        }
        if (!pick) continue;                  /* no object: the ROM pass would not place him either */
        wr8(pick + NHL94_OFF_NEWPOS, glue_peek8(NHL94_PLLIST + 6u + (uint32_t)k));
        wr8(pick + NHL94_OFF_NEWPNUM, (uint8_t)(want - 1u));
        wr8(NHL94_PLLIST + (uint32_t)k, 0);
        hit(H_PASS2_PLACED);
    }
}

/* ---- 4v4 penalties: a penalty shot instead of a minor ----
 *
 * PenShotChk (penalty94.asm, ROM $01211A) runs before AddPenalty at every
 * hit that draws a penalty (checks, hooks, holds, trips): a2 = the fouled
 * player, a3 = the offender, d0 = the penalty (Penaltylist offset). Stock, it
 * swaps d0 for the penalty-shot form (minutes $FF: nobody boxed) only when a2
 * is on a breakaway. In 4v4 this replacement drops the breakaway test, so
 * every such minor is a penalty shot; the rest is the stock path
 * (getBAplayerInfo below, then AddPenalty, puckpenshot, SetupPenaltyShot).
 *
 * The stock lookup table PenShotPenalties ($01913A) is shifted one entry
 * (Charging gives Slashing's shot, ..., Interference and Holding none); 4v4
 * uses the intended pairs and gives Holding the Hooking shot. 6v6 keeps the
 * cartridge's table.
 *
 * Still a stock penalty (the 3-on-3 OT rule): penalties off (getBAplayerInfo
 * fails), a penalty shot already pending, the other team's goalie pulled.
 * A fouled goalie hands the shot to his C, else his LW. Fights and other
 * calls never pass through PenShotChk. */
static int ps_penalty4(unsigned pen)
{
    switch (pen) {
    case 0x16: return 0x38;   /* Charging      -> ChargingPS */
    case 0x18: return 0x3C;   /* Slashing      -> SlashingPS */
    case 0x12:                /* Roughing (long slow-down) */
    case 0x1A: return 0x36;   /* Roughing      -> RoughingPS */
    case 0x1C: return 0x3A;   /* Cross-check   -> CrossChkPS */
    case 0x1E: return 0x2E;   /* Hooking       -> HookingPS */
    case 0x20: return 0x30;   /* Tripping      -> TrippingPS */
    case 0x22: return 0x34;   /* Interference  -> InterferencePS */
    case 0x24: return 0x2E;   /* Holding       -> HookingPS (no form of its own) */
    default:   return -1;
    }
}

/* getGoalieSCnum: the goalie (position 0) among the 6 objects ending at sc. */
static int goalie_scnum(unsigned last)
{
    for (int i = (int)last; i > (int)last - 6; i--)
        if (rd16(NHL94_SC_ADDR(i) + NHL94_OFF_POSITION) == 0)
            return (int16_t)rd16(NHL94_SC_ADDR(i) + NHL94_OFF_SCNUM);
    return -1;
}

/* getBAplayerInfo ($012174) for shooter object sc, offender object off.
 * 1 = penalty shot set up. */
static int ba_player_info(unsigned sc, uint32_t off)
{
    if (!rd16(NHL94_OPTPEN)) return 0;
    wr8(NHL94_BA_PS_FLAGS, (uint8_t)(glue_peek8(NHL94_BA_PS_FLAGS) | NHL94_BA_PS_PENDING_B));
    int g = goalie_scnum(sc > 5 ? 5u : 11u);
    if (g < 0) {
        wr8(NHL94_BA_PS_FLAGS, (uint8_t)(glue_peek8(NHL94_BA_PS_FLAGS) & ~NHL94_BA_PS_PENDING_B));
        return 0;
    }
    uint32_t b = NHL94_SC_ADDR(sc);
    wr16(NHL94_BA_SKTR_SCNUM, (uint16_t)sc);
    wr16(NHL94_BA_TEAM, (glue_peek8(b + NHL94_OFF_PFLAGS) & NHL94_PF_TEAM) ? 1u : 0u);
    wr16(NHL94_BA_SKATER_OFF, glue_peek8(b + NHL94_OFF_PNUM));
    wr16(NHL94_BA_GOALIE_SCNUM, (uint16_t)g);
    wr16(NHL94_BA_GOALIE_OFF, glue_peek8(NHL94_SC_ADDR(g) + NHL94_OFF_PNUM));
    wr16(NHL94_BA_CHECKER_OFF, glue_peek8(off + NHL94_OFF_PNUM));
    return 1;
}

/* The shooter for a foul on object sc: sc itself if a skater, else (a
 * goalie) his team's C, else LW on the ice; -1 none. */
static int shooter_for(unsigned sc)
{
    if ((int16_t)rd16(NHL94_SC_ADDR(sc) + NHL94_OFF_POSITION) > 0) return (int)sc;
    static const int16_t want[] = { NHL94_POS_C, NHL94_POS_LW };
    unsigned first = sc < 6 ? 0u : 6u;
    for (unsigned w = 0; w < 2; w++)
        for (unsigned i = first; i < first + 6u; i++)
            if ((int16_t)rd16(NHL94_SC_ADDR(i) + NHL94_OFF_POSITION) == want[w]) return (int)i;
    return -1;
}

/* PenShotChk, 4v4. Returns d0 (the penalty to add). Preserves d1-d3/a0 like
 * the original (C touches no guest register but d0). */
static uint16_t penshotchk(uint16_t pen)
{
    hit(H_PENSHOT);
    if (glue_peek8(NHL94_BA_PS_FLAGS) & NHL94_BA_PS_PENDING_B) return pen;
    int ps = ps_penalty4(pen);
    if (ps < 0) return pen;
    uint32_t a2 = reg_addr(g_cpu.A[2]), a3 = reg_addr(g_cpu.A[3]);
    unsigned sc = rd16(a2 + NHL94_OFF_SCNUM);
    if (sc > 11) return pen;
    int sh = shooter_for(sc);
    if (sh < 0 || !ba_player_info((unsigned)sh, a3)) return pen;
    wr16(NHL94_PSPENALTY, (uint16_t)ps);
    hit(H_PENSHOT_SET);
    if (!genesis_multiview_speculating())
        fprintf(stderr, "nhl94_format: t=%u 4v4 penalty $%02X on SC %u -> penalty shot $%02X by SC %d\n",
                genesis_sim_tick_count(), pen, (unsigned)rd16(a3 + NHL94_OFF_SCNUM), ps, sh);
    return (uint16_t)ps;
}

/* ---- hooks ---- */

int nhl94_format_instruction_hook(uint32_t pc, int *mine)
{
    *mine = 1;
    switch (pc) {
    case NHL94_ROM_RESTORETEAMS_R: {
        uint16_t want = nhl94_format_effective() == NHL94_FORMAT_4V4 ? NHL94_FMT_4V4 : 0u;
        if (rd16(NHL94_FMT) != want) wr16(NHL94_FMT, want);
        if (want) {
            hit(H_LATCH);
            write_tables();
            wr16(reg_addr(g_cpu.A[2]) + NHL94_TM_AP, 4);
        }
        return 0;
    }
    case NHL94_ROM_SETPLLIST_PRIO:
    case NHL94_ROM_RELEASEPL_PRIO:
    case NHL94_ROM_GETLINEE_PRIO:
        if (!fmt4()) return 0;
        hit(pc == NHL94_ROM_SETPLLIST_PRIO ? H_SETPL : pc == NHL94_ROM_RELEASEPL_PRIO ? H_RELEASE : H_GETLINEE);
        write_tables();
        g_cpu.A[0] = 0xFF000000u | NHL94_PRIO4;
        return 0;
    case NHL94_ROM_FACEOFF_APL:
        if (!fmt4()) return 0;
        hit(H_APL);
        write_tables();
        g_cpu.A[1] = 0xFF000000u | NHL94_APL4;
        return 0;
    case NHL94_ROM_INPROGRESS_AP: {
        if (!fmt4()) return 0;
        hit(H_INPROG);
        uint32_t t = reg_addr(g_cpu.A[2]);
        for (unsigned r = 0; r < NHL94_MAXROS; r++) {
            uint32_t a = t + NHL94_TM_PDST + 2u * r;
            int16_t v = (int16_t)rd16(a);
            if (v > 0 && ((uint16_t)v & NHL94_PDST_JUSTCALLED)) wr16(a, (uint16_t)((uint16_t)v & ~NHL94_PDST_JUSTCALLED));
        }
        apply_rule();
        /* the original leaves d0 = -2 (loop end), d1 = tmap */
        g_cpu.D[0] = (g_cpu.D[0] & 0xFFFF0000u) | 0xFFFEu;
        g_cpu.D[1] = (g_cpu.D[1] & 0xFFFF0000u) | rd16(t + NHL94_TM_AP);
        return 1;
    }
    case NHL94_ROM_PENGOAL_CMP6:
        if (!fmt4()) return 0;
        hit(H_PENGOAL);
        if (boxed(reg_addr(g_cpu.A[1])) == 0) {
            hit(H_PENGOAL_CLR);
            for (unsigned i = 0; i < NHL94_PENBUF_CLEAR_WORDS; i++) wr16(NHL94_PENBUF + 2u * i, 0);
        }
        return 0;
    case NHL94_ROM_LOWESTPEN_CMP6:
        if (!fmt4()) return 0;
        hit(H_LOWPEN);
        if (boxed(reg_addr(g_cpu.A[3]))) return 0;
        hit(H_LOWPEN_RTS);
        return 1;                                   /* rts with d0 = the power-play time */
    case NHL94_ROM_PENSHOTCHK: {
        if (!fmt4()) return 0;
        uint16_t d0 = penshotchk((uint16_t)g_cpu.D[0]);
        g_cpu.D[0] = (g_cpu.D[0] & 0xFFFF0000u) | d0;
        return 1;                                   /* rts */
    }
    case NHL94_ROM_SETPERSONEL_P2:
        if (!fmt4()) return 0;
        hit(H_PASS2);
        setpersonel_pass2();
        return 0;
    case NHL94_ROM_FACEOFF_NORM:
        if (!fmt4()) return 0;
        hit(H_NORM);
        apply_rule();
        return 0;
    case NHL94_ROM_UPDATEPLAYERS:   /* shared with nhl94_seats.c: trace (+ test injection) */
        test_penalties();
        trace();
        *mine = 0;
        return 0;
    default:
        *mine = 0;
        return 0;
    }
}
