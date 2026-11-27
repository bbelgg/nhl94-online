/*
 * nhl94_tags.c - name tags for the humans on the ice, drawn onto the
 * PRESENTED frame (main window and every view). Presentation only.
 *
 * WHAT. Every on-ice object a human controls right now gets a small
 * triangle in the seat's colour (fixed 12-colour palette) and the human's
 * name (upper case, <= 10 chars) above the skater. AI players get nothing.
 * The local seat's tag is drawn filled (seat colour, dark/white text) next
 * to the game's own star; the others are a dark glass box with a seat-colour
 * border. A human outside the picture is shown at the nearest edge with the
 * triangle pointing toward him (the camera follows the puck or one player, so
 * a teammate off-screen is exactly when "who is where" is most useful; a
 * tag that would hide is information lost).
 *
 * WHO. nhl94_seats_human_objects (nhl94_seats.c): Position lock = each seat's
 * object as the lock resolves it this frame; Classic = the objects the pads
 * control (c1playernum.. with cont1team..). Names: nhl94_tagnames.c from the
 * lobby's live member table (online) or "P<seat>" (offline / LAN).
 *
 * WHERE THE DATA COMES FROM (the rollback / determinism rule). Nothing here
 * writes guest RAM, the VDP or any snapshotted state. Capture reads guest RAM
 * at the DoGameFrame -> setvideo site ($007998), the same place the views
 * apply their camera, into host statics:
 *   - authoritative tick: the list setvideo builds is uploaded by that
 *     tick's vblank and shown by the NEXT tick (docs/VIEW_RESEARCH.md
 *     section 4), so the record is held for one tick: after tick T+1's hook
 *     the picture presented for T+1 is tick T's. A tick whose hook did not
 *     run (menus, a lag frame) reuses the held record for at most 2 ticks.
 *     Rollback replays run the hook too; the last two hooks before a
 *     presented frame are always the final (corrected) ones.
 *   - view k: captured in the view's speculative tick right after
 *     apply_view (so camera and 180-degree mirror are the view's own); the
 *     view's image is drawn after it was grabbed.
 * The record is camera-relative (X-Hpos, Y-Vpos+1.5Z; the flipped view has
 * both negated in RAM already), so the projection below is the game's own
 * find3d: screen x = X - Hpos + w/2, screen y = h/2 - (Y - Vpos + 1.5 Z).
 *
 * CALL ICON (nhl94_seats_call_flags). A human on the local seat's team who
 * is CALLING for the puck (B / X held while a teammate carries) or QUEUED (an
 * AI carrier was asked for him) gets a pulsing ring in his seat colour at his
 * feet and a "!" bubble beside his name tag; spectators see every team. It is
 * part of the tag record (so it needs name tags on): the same capture, the
 * same camera-relative position, so it is right in the rotated period-2 view
 * and the zoomed picture (drawn in picture pixels, shrinks with the skaters).
 * The pulse reads the tick counter and nothing is written back.
 *
 * ENV  NHL94_NAMETAGS=0/1   override the launcher setting
 *      NHL94_CALLICON=0/1/all  override the launcher's call icon setting
 *                           (all = opponents' calls too)
 *      NHL94_NAMETAGS_FAKE="Alice,Bob,..."  TEST: fake lobby table, entry i
 *                           names seat i (slot i)
 *      NHL94_NAMETAGS_DEBUG=1   log records, draw the projection origin
 */
#include "nhl94_tags.h"
#include "nhl94_ram.h"
#include "nhl94_seats.h"
#include "nhl94_tagnames.h"
#include "nhl94_builds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "genesis_runtime.h"
#include "multiview.h"
#include "sim_step.h"
#if GENESIS_HAS_RECOMP_NET
#include "netplay/genesis_netplay.h"
#include <recomp_net/lobby_client.h>
#endif

/* ---- setting ---- */
static int s_setting = 1;
static int s_env = -2;          /* -2 unread, -1 unset, 0/1 */
static int s_debug = -1;
static char s_fake[NHL94_SEATS_MAX][NHL94_TAGNAME_MAX * 4 + 4];
static int s_nfake = -1;

static void read_env(void)
{
    if (s_env != -2) return;
    const char *e = getenv("NHL94_NAMETAGS");
    s_env = (e && *e) ? (atoi(e) != 0) : -1;
    const char *d = getenv("NHL94_NAMETAGS_DEBUG");
    s_debug = d && atoi(d) > 0;
    s_nfake = 0;
    const char *f = getenv("NHL94_NAMETAGS_FAKE");
    for (const char *p = f; p && *p && s_nfake < NHL94_SEATS_MAX; ) {
        const char *c = strchr(p, ',');
        size_t n = c ? (size_t)(c - p) : strlen(p);
        if (n >= sizeof s_fake[0]) n = sizeof s_fake[0] - 1;
        memcpy(s_fake[s_nfake], p, n);
        s_fake[s_nfake++][n] = 0;
        if (!c) break;
        p = c + 1;
    }
}

/* Call icon setting: launcher on/off; env NHL94_CALLICON 0 / 1 / all (2). */
static int s_icon_setting = 1;
static int s_icon_env = -2;     /* -2 unread, -1 unset, 0, 1, 2 = all */

static void read_icon_env(void)
{
    if (s_icon_env != -2) return;
    const char *e = getenv("NHL94_CALLICON");
    s_icon_env = !e || !*e ? -1 : !strcmp(e, "all") ? 2 : (atoi(e) != 0);
}

void nhl94_tags_set_callicon(int on) { s_icon_setting = on != 0; }
int  nhl94_tags_callicon_setting(void) { return s_icon_setting; }
int  nhl94_tags_callicon_env_override(void) { read_icon_env(); return s_icon_env; }
/* 0 off, 1 own team's calls, 2 every team's. */
int  nhl94_tags_callicon_mode(void)
{
    read_icon_env();
    return s_icon_env >= 0 ? s_icon_env : s_icon_setting;
}

void nhl94_tags_set_setting(int on) { s_setting = on != 0; }
int  nhl94_tags_setting(void) { return s_setting; }
int  nhl94_tags_env_override(void) { read_env(); return s_env; }
int  nhl94_tags_enabled(void)
{
    read_env();
    return s_env >= 0 ? s_env : s_setting;
}

/* ---- records ---- */
#define TE_CALL   1     /* the human is calling for the puck (nhl94_seats_call_flags bit 0) */
#define TE_QUEUED 2     /* an AI carrier was asked for him (bit 1) */
typedef struct { int8_t seat; uint8_t team; uint8_t flags; int16_t dx, dy; } TagEnt;
typedef struct { int valid, n; TagEnt e[12]; } TagRec;

static TagRec  s_pend, s_shown;
static TagRec  s_view[GENESIS_MULTIVIEW_MAX];
static int     s_fresh;          /* the hook ran since the last main overlay */
static int     s_stale;          /* ticks the held record has been reused */

static inline uint16_t rd16(uint32_t a) { return glue_peek16(a); }
static inline int16_t  rds16(uint32_t a) { return (int16_t)glue_peek16(a); }

#define SFLAGS_HOR_W    0x8000
#define SFLAGS_PAUSE_W  0x0100
#define SFLAGS2_REPLAY_W 0x0800

static void capture(TagRec *r)
{
    memset(r, 0, sizeof *r);
    if (!nhl94_tags_enabled()) return;
    uint16_t gmode = rd16(NHL94_GMODE), sflags = rd16(NHL94_SFLAGS), sflags2 = rd16(NHL94_SFLAGS2);
    /* Intermission highlights of other games, the scoreboard rink, pause and
     * replay: nothing is "on the ice" for the humans. */
    if ((gmode & NHL94_GMODE_HL_W) || (sflags & (SFLAGS_HOR_W | SFLAGS_PAUSE_W)) ||
        (sflags2 & SFLAGS2_REPLAY_W))
        return;
    int8_t os[12];
    uint8_t cf[12];
    nhl94_seats_human_objects(os);
    nhl94_seats_call_flags(cf);
    int hpos = rds16(NHL94_HPOS), vpos = rds16(NHL94_VPOS);
    r->valid = 1;
    for (int i = 0; i < 12; i++) {
        if (os[i] < 0) continue;
        uint32_t b = NHL94_SC_ADDR((unsigned)i);
        if (rds16(b + NHL94_OFF_POSITION) < 0) continue;       /* off the ice */
        int x = rds16(b + NHL94_OFF_XPOS), y = rds16(b + NHL94_OFF_YPOS), z = rds16(b + NHL94_OFF_ZPOS);
        if (z < 0) z = 0;
        TagEnt *e = &r->e[r->n++];
        e->seat = os[i];
        e->team = i >= 6;
        e->flags = cf[i];
        e->dx = (int16_t)(x - hpos);
        e->dy = (int16_t)(y - vpos + (3 * z) / 2);
    }
}

static void trace(const char *what, int idx, const TagRec *r)
{
    if (!s_debug) return;
    fprintf(stderr, "nhl94_tags: %s %d tick=%u valid=%d n=%d", what, idx, genesis_sim_tick_count(), r->valid, r->n);
    for (int i = 0; i < r->n; i++)
        fprintf(stderr, " [seat %d dx=%d dy=%d f=%d]", r->e[i].seat, r->e[i].dx, r->e[i].dy, r->e[i].flags);
    fprintf(stderr, "\n");
}

void nhl94_tags_main_hook(void)
{
    if (genesis_multiview_speculating() || !nhl94_tags_enabled()) return;
    s_shown = s_pend;
    capture(&s_pend);
    s_fresh = 1;
}

void nhl94_tags_view_begin(int view)
{
    if (view >= 0 && view < GENESIS_MULTIVIEW_MAX) memset(&s_view[view], 0, sizeof s_view[view]);
}

void nhl94_tags_view_capture(int view)
{
    if (view < 0 || view >= GENESIS_MULTIVIEW_MAX) return;
    capture(&s_view[view]);
}

/* ---- names ---- */
static void member_table(NhlTagMember *m, int *n, char *local, size_t lcap)
{
    *n = 0;
    local[0] = 0;
    read_env();
    if (s_nfake > 0) {                                   /* test table */
        for (int i = 0; i < s_nfake; i++) {
            m[i].slot = i; m[i].spectator = 0;
            snprintf(m[i].name, sizeof m[i].name, "%s", s_fake[i]);
        }
        *n = s_nfake;
        return;
    }
#if GENESIS_HAS_RECOMP_NET
    if (!genesis_netplay_active()) return;
    int c = rnet_lobby_member_count();
    for (int i = 0; i < c && *n < 16; i++) {
        RNetLobbyMember lm;
        if (!rnet_lobby_member_get(i, &lm)) continue;
        m[*n].slot = lm.slot;
        m[*n].spectator = lm.is_spectator;
        snprintf(m[*n].name, sizeof m[*n].name, "%s", lm.display_name);
        (*n)++;
    }
    const char *ln = rnet_lobby_display_name();
    if (ln) snprintf(local, lcap, "%s", ln);
#else
    (void)lcap;
#endif
}

/* ---- drawing ---- */
#include "mv_font5x7.h"

typedef struct { uint32_t *px; int w, h, stride; } Img;

static const uint32_t k_palette[NHL94_SEATS_MAX] = {
    0xFFFF3B30, 0xFF2F6BFF, 0xFFFFD60A, 0xFF30D158, 0xFFFF9F0A, 0xFFBF5AF2,
    0xFF32D4FF, 0xFFFF5CC8, 0xFFA6E22E, 0xFF20A090, 0xFFC8A06A, 0xFFFFFFFF };

static inline void put(const Img *im, int x, int y, uint32_t c)
{
    if (x >= 0 && y >= 0 && x < im->w && y < im->h) im->px[(size_t)y * (size_t)im->stride + (size_t)x] = c;
}

static void fill(const Img *im, int x0, int y0, int x1, int y1, uint32_t c)   /* inclusive */
{
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) put(im, x, y, c);
}

static void dim(const Img *im, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || y < 0 || x >= im->w || y >= im->h) continue;
            uint32_t *p = &im->px[(size_t)y * (size_t)im->stride + (size_t)x];
            uint32_t v = *p;
            uint32_t r = ((v >> 16) & 0xFF) * 3 / 10, g = ((v >> 8) & 0xFF) * 3 / 10, b = (v & 0xFF) * 3 / 10;
            *p = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
}

static const MvGlyph *glyph(char ch)
{
    static const MvGlyph dot = { '.', { 0, 0, 0, 0, 0, 0x0C, 0x0C } };
    static const MvGlyph und = { '_', { 0, 0, 0, 0, 0, 0, 0x1F } };
    static const MvGlyph bang = { '!', { 0x04, 0x04, 0x04, 0x04, 0x04, 0, 0x04 } };   /* the font has none */
    if (ch == '!') return &bang;
    if (ch == '.') return &dot;
    if (ch == '_') return &und;
    const MvGlyph *q = NULL;
    for (size_t k = 0; k < sizeof k_mv_font / sizeof k_mv_font[0]; k++) {
        if (k_mv_font[k].ch == ch) return &k_mv_font[k];
        if (k_mv_font[k].ch == '?') q = &k_mv_font[k];
    }
    return q;
}

/* Tag scale. A zoomed view's picture is bigger in image pixels and then
 * scaled down to the window. Tags used to be multiplied by that factor to
 * keep a constant window size, but next to skaters that shrink with the zoom
 * they dominated the picture; they are now drawn in picture pixels (scale 1,
 * crisp 1:1 font) so they shrink with the skaters. The sprite-height offset
 * (HEAD) is in picture pixels at every zoom and is never scaled. Font pixels
 * become blocks [floor(i*s), floor((i+1)*s)), nearest neighbour. */
static double s_sc = 1.0;
static int S(int v) { return (int)(v * s_sc + 0.5); }
static int Sf(int v) { return (int)(v * s_sc); }

static void text(const Img *im, int x, int y, const char *s, uint32_t c)
{
    for (int i = 0; s[i]; i++) {
        const MvGlyph *g = glyph(s[i]);
        if (!g) continue;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (g->row[row] & (0x10 >> col))
                    fill(im, x + Sf(i * 6 + col), y + Sf(row), x + Sf(i * 6 + col + 1) - 1, y + Sf(row + 1) - 1, c);
    }
}

/* Triangle of half-width `hw` pointing dir (0 up, 1 down, 2 left, 3 right),
 * tip at (tx, ty), outlined in black. */
static void tri(const Img *im, int tx, int ty, int hw, int dir, uint32_t c)
{
    for (int pass = 0; pass < 2; pass++) {              /* pass 0: outline (grown by 1) */
        int g = pass == 0;
        for (int k = 0; k <= hw + g; k++) {              /* k = distance from the tip */
            int half = k;                                /* width grows 1 px per step */
            for (int s = -half - g; s <= half + g; s++) {
                int x, y;
                switch (dir) {
                case 0:  x = tx + s; y = ty + k; break;
                case 1:  x = tx + s; y = ty - k; break;
                case 2:  x = tx + k; y = ty + s; break;
                default: x = tx - k; y = ty + s; break;
                }
                put(im, x, y, g ? 0xFF000000u : c);
            }
        }
    }
}

static int lum(uint32_t c) { return (int)(((c >> 16) & 0xFF) * 3 + ((c >> 8) & 0xFF) * 6 + (c & 0xFF)) / 10; }
static int clampi(int v, int lo, int hi) { return hi < lo ? lo : v < lo ? lo : v > hi ? hi : v; }

#define TRI_H   4     /* triangle height */
#define HEAD    26    /* origin (feet) -> top of the skater sprite, measured */
#define HUD_Y   188   /* scoreboard panels start here (main frame, 224 high) */

typedef struct { int x0, y0, x1, y1; } Rect;

static int overlaps(const Rect *placed, int n, int x0, int y0, int x1, int y1)
{
    for (int i = 0; i < n; i++)
        if (x0 <= placed[i].x1 && x1 >= placed[i].x0 && y0 <= placed[i].y1 && y1 >= placed[i].y0) return 1;
    return 0;
}

/* Call icon. The ring is an elliptical band 2 px thick (a circle on the ice
 * seen from the broadcast angle, 11:5) around the feet (sx, sy), outlined in
 * black outside so it reads on the white ice and on the boards. A HELD call
 * pulses between two sizes with the tick counter; a QUEUED one (the AI carrier
 * was asked, not passed yet) is steady and filled at half alpha. */
static void blend(const Img *im, int x, int y, uint32_t c, int a256)
{
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) return;
    uint32_t *p = &im->px[(size_t)y * (size_t)im->stride + (size_t)x];
    uint32_t v = *p;
    uint32_t r = (((c >> 16) & 0xFF) * (uint32_t)a256 + ((v >> 16) & 0xFF) * (uint32_t)(256 - a256)) >> 8;
    uint32_t g = (((c >> 8) & 0xFF) * (uint32_t)a256 + ((v >> 8) & 0xFF) * (uint32_t)(256 - a256)) >> 8;
    uint32_t b = ((c & 0xFF) * (uint32_t)a256 + (v & 0xFF) * (uint32_t)(256 - a256)) >> 8;
    *p = 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* inside the ellipse of radii (rx, ry)? (integer: (x/rx)^2 + (y/ry)^2 <= 1) */
static int in_ell(int x, int y, int rx, int ry)
{
    return rx > 0 && ry > 0 && x * x * ry * ry + y * y * rx * rx <= rx * rx * ry * ry;
}

static void call_ring(const Img *im, int sx, int sy, uint32_t col, int flags)
{
    int pulse = (flags & TE_CALL) ? (int)((genesis_sim_tick_count() >> 3) & 1u) : 0;
    int rx = 11 + 2 * pulse, ry = (rx * 5 + 5) / 11;
    int cy = sy + 1;
    for (int y = -ry - 1; y <= ry + 1; y++)
        for (int x = -rx - 1; x <= rx + 1; x++) {
            int in_out = in_ell(x, y, rx, ry), in_in = in_ell(x, y, rx - 2, ry - 2);
            if (in_out && !in_in) put(im, sx + x, cy + y, col);
            else if (!in_out && in_ell(x, y, rx + 1, ry + 1)) put(im, sx + x, cy + y, 0xFF000000u);
            else if (in_in && (flags & TE_QUEUED)) blend(im, sx + x, cy + y, col, 128);
        }
}

/* "!" bubble: colour fill, black outline, 11 x bh, left edge bx. */
static void call_bubble(const Img *im, int bx, int top, int bh, uint32_t col)
{
    fill(im, bx, top, bx + 10, top + bh - 1, 0xFF000000u);
    fill(im, bx + 1, top + 1, bx + 9, top + bh - 2, col);
    text(im, bx + 3, top + (bh - 7) / 2, "!", lum(col) > 140 ? 0xFF101010u : 0xFFFFFFFFu);
}
#define BUBBLE_W 11

/* Lays out and draws one tag. `placed` holds the rectangles (box plus its
 * triangle) already used in this picture; a tag that would land on one is
 * stacked away from it (above/below the skater; along the edge when he is
 * off-screen) so two humans standing together (a faceoff) both stay readable.
 * A stacked tag keeps a 1-pixel leader line down to the skater. */
static void draw_tag(const Img *im, const TagEnt *e, int flags, int local_seat, const NhlTagMember *mem, int nmem,
                     const char *local_name, Rect *placed, int *nplaced)
{
    char name[NHL94_TAGNAME_MAX + 4];
    int seat = e->seat;
    char bname[48];
    nhl94_builds_seat_name(seat, bname, sizeof bname);       /* the seat plays as this real player */
    nhl94_tagname_for_seat_build(seat, local_seat, local_name, mem, nmem, bname, name, sizeof name);
    uint32_t col = k_palette[clampi(seat, 0, NHL94_SEATS_MAX - 1)];
    int is_local = seat == local_seat;
    int len = (int)strlen(name);
    const int PAD = S(2), BRD = S(1) > 0 ? S(1) : 1, head = HEAD;
    int bw = Sf(len * 6 - 1) + 2 * PAD, bh = Sf(7) + 2 * PAD;   /* text + 2 px padding */
    int unit = bh + S(TRI_H) + 2;
    int ox = im->w / 2, oy = im->h / 2;
    int sx = ox + e->dx, sy = oy - e->dy;
    int hud = im->h >= 224 ? im->h - (224 - HUD_Y) : im->h;
    int off = sx < 0 || sx >= im->w || sy < 0 || sy >= im->h;
    const int TH = S(off ? TRI_H + 3 : TRI_H);            /* off-screen: a bigger "that way" triangle */
    int dir = 1;                                         /* triangle: 1 down (tag above), 0 up (below), 2 left, 3 right */
    int cx, top, stacked = 0;
    if (!off) {
        cx = clampi(sx, bw / 2 + 1, im->w - bw / 2 - 2);
        top = sy - head - TH - 1 - bh;
        int below = 0;
        for (int tries = 0; tries < 6; tries++) {
            if (!below && top < 0) {                      /* no room above: below the feet */
                below = 1; dir = 0; top = sy + TH + 3;
                if (tries) stacked = 1;
            }
            int ty0 = dir == 1 ? top : top - TH - 1, ty1 = dir == 1 ? top + bh + TH : top + bh - 1;
            if (!overlaps(placed, *nplaced, cx - bw / 2, ty0, cx + bw / 2, ty1)) break;
            stacked = 1;
            top += below ? unit : -unit;
        }
        if (top < 0) top = 0;
        if (top + bh > im->h) top = im->h - bh;
    } else {
        /* nearest edge, triangle pointing toward the player */
        double fx = (double)abs(sx - ox) / (double)(im->w / 2), fy = (double)abs(sy - oy) / (double)(im->h / 2);
        int lo_x = bw / 2 + 1 + TH + 2, hi_x = im->w - bw / 2 - 2 - TH - 2;
        int lo_y = TH + 2, hi_y = hud - bh - TH - 3;
        int px, py;
        if (fx > fy) {
            dir = sx < ox ? 2 : 3;
            cx = dir == 2 ? lo_x : hi_x;
            top = clampi(sy - bh / 2, lo_y, hi_y);
        } else {
            dir = sy < oy ? 0 : 1;
            cx = clampi(sx, lo_x, hi_x);
            top = dir == 0 ? lo_y : hi_y;
        }
        for (int tries = 0; tries < 8; tries++) {         /* slide along the edge */
            px = cx; py = top;
            int x0 = px - bw / 2 - (dir == 2 ? TH + 1 : 0), x1 = px + bw / 2 + (dir == 3 ? TH + 1 : 0);
            int y0 = py - (dir == 0 ? TH + 1 : 0), y1 = py + bh - 1 + (dir == 1 ? TH + 1 : 0);
            if (!overlaps(placed, *nplaced, x0, y0, x1, y1)) break;
            if (dir >= 2) top = clampi(top + ((tries & 1) ? -1 : 1) * unit * (tries / 2 + 1), lo_y, hi_y);
            else cx = clampi(cx + ((tries & 1) ? -1 : 1) * (bw + TH + 3) * (tries / 2 + 1), lo_x, hi_x);
        }
    }
    int x0 = cx - bw / 2, x1 = x0 + bw - 1, y1 = top + bh - 1;
    /* the "!" bubble: right of the name box (left when that side has the
     * off-screen arrow or no room) */
    int bubble_left = 0, bubble_x = 0;
    if (flags) {
        bubble_left = dir == 3 || x1 + 3 + BUBBLE_W > im->w;
        bubble_x = bubble_left ? x0 - 3 - BUBBLE_W : x1 + 3;
    }
    {   /* reserve box + triangle (+ bubble) */
        Rect r = { x0 - (dir == 2 ? TH + 1 : 0), top - (dir == 0 ? TH + 1 : 0),
                   x1 + (dir == 3 ? TH + 1 : 0), y1 + (dir == 1 ? TH + 1 : 0) };
        if (flags) { if (bubble_left) r.x0 = bubble_x - 1; else r.x1 = bubble_x + BUBBLE_W; }
        if (*nplaced < 16) placed[(*nplaced)++] = r;
    }
    if (flags && !off) call_ring(im, sx, sy, col, flags);   /* under the leader and the box */
    if (stacked && !off) {                               /* leader to the skater */
        int ly0 = dir == 1 ? y1 + TH + 1 : sy, ly1 = dir == 1 ? sy - head : top - TH - 1;
        for (int y = ly0; y <= ly1; y++) {
            fill(im, cx - BRD, y, cx - BRD, y, 0xFF000000u);
            fill(im, cx, y, cx + BRD - 1, y, col);
            fill(im, cx + BRD, y, cx + BRD, y, 0xFF000000u);
        }
    }
    if (is_local) fill(im, x0, top, x1, y1, col);
    else {
        dim(im, x0, top, x1, y1);
        fill(im, x0, top, x1, top + BRD - 1, col); fill(im, x0, y1 - BRD + 1, x1, y1, col);
        fill(im, x0, top, x0 + BRD - 1, y1, col); fill(im, x1 - BRD + 1, top, x1, y1, col);
    }
    uint32_t tc = is_local ? (lum(col) > 140 ? 0xFF101010u : 0xFFFFFFFFu) : 0xFFFFFFFFu;
    text(im, x0 + PAD, top + PAD, name, tc);
    /* marker triangle between the box and the player (or toward him) */
    switch (dir) {
    case 1: tri(im, cx, y1 + TH + 1, TH, 1, col); break;           /* tip down, base on the box */
    case 0: tri(im, cx, top - TH - 1, TH, 0, col); break;          /* tip up */
    case 2: tri(im, x0 - TH - 1, top + bh / 2, TH, 2, col); break;
    default: tri(im, x1 + TH + 1, top + bh / 2, TH, 3, col); break;
    }
    if (flags) call_bubble(im, bubble_x, top, bh, col);
    if (s_debug && !off) {                              /* the projection origin */
        for (int d = -3; d <= 3; d++) { put(im, sx + d, sy, 0xFFFF00FFu); put(im, sx, sy + d, 0xFFFF00FFu); }
    }
}

void nhl94_tags_present_overlay(uint32_t *pixels, int w, int h, int stride, int view_index)
{
    if (!pixels || w <= 0 || h <= 0 || !nhl94_tags_enabled()) return;
    const TagRec *r = NULL;
    if (view_index < 0) {
        if (s_fresh) { s_stale = 0; s_fresh = 0; }
        else if (++s_stale > 2) { s_shown.valid = 0; s_pend.valid = 0; }
        r = &s_shown;
        trace("main", -1, r);
    } else if (view_index < GENESIS_MULTIVIEW_MAX) {
        r = &s_view[view_index];
        trace("view", view_index, r);
    }
    if (!r || !r->valid || r->n == 0) return;
    NhlTagMember mem[16];
    int nmem;
    char local_name[64];
    member_table(mem, &nmem, local_name, sizeof local_name);
    int local = nhl94_seats_local_seat();
    /* Call icon: the local seat's team (every team for a spectator or
     * NHL94_CALLICON=all). */
    int imode = nhl94_tags_callicon_mode(), lteam = -1;
    if (!nhl94_seats_get(local, &lteam, NULL)) lteam = -1;
    Img im = { pixels, w, h, stride };
    s_sc = 1.0;
    /* the local seat first: it gets the place nearest its skater */
    Rect placed[16];
    int nplaced = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < r->n; i++)
            if ((r->e[i].seat == local) == (pass == 0))
                draw_tag(&im, &r->e[i],
                         imode && (imode == 2 || lteam < 0 || r->e[i].team == lteam) ? r->e[i].flags : 0,
                         local, mem, nmem, local_name, placed, &nplaced);
}
