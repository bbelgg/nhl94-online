/* Unit test of the aimed-pass picker (game/nhl94_aim.c).
 *   cc -I game -o /tmp/test_aimpass tools/seats/test_aimpass.c game/nhl94_aim.c && /tmp/test_aimpass */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "nhl94_aim.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* A candidate at `deg` degrees clockwise from the aim `dir`, `dist` px away from the puck at (px, py). */
static AimCand at(int dir, double deg, int dist, int px, int py, int human, int calling, int sc)
{
    double base = dir * 45.0, a = (base + deg) * M_PI / 180.0;   /* 0 = +y, clockwise: x = sin, y = cos */
    AimCand c = { (int16_t)lround(px + dist * sin(a)), (int16_t)lround(py + dist * cos(a)),
                  (uint8_t)human, (uint8_t)calling, (int8_t)sc };
    return c;
}

int main(void)
{
    int s;
    /* aim up: teammates at 5, 30 and 70 degrees (same distance): 5 wins */
    {
        AimCand c[3] = { at(0, 30, 100, 0, 0, 0, 0, 0), at(0, 5, 100, 0, 0, 0, 0, 1), at(0, 70, 100, 0, 0, 0, 0, 2) };
        CHECK(nhl94_aim_pick(0, 0, 0, c, 3, &s) == 1);
    }
    /* only a 70-degree teammate: outside the cone */
    {
        AimCand c[1] = { at(0, 70, 100, 0, 0, 0, 0, 0) };
        CHECK(nhl94_aim_pick(0, 0, 0, c, 1, &s) == -1);
    }
    /* a human at 20 degrees beats an AI dead on; an AI dead on beats a human at 35 */
    {
        AimCand c[2] = { at(0, 0, 100, 0, 0, 0, 0, 0), at(0, 20, 100, 0, 0, 1, 0, 1) };
        CHECK(nhl94_aim_pick(0, 0, 0, c, 2, &s) == 1);
        AimCand d[2] = { at(0, 0, 100, 0, 0, 0, 0, 0), at(0, 35, 100, 0, 0, 1, 0, 1) };
        CHECK(nhl94_aim_pick(0, 0, 0, d, 2, &s) == 0);
    }
    /* a calling human at 30 degrees beats an AI dead on; the same human not calling does not (sin^2 30 = 256 > 150) */
    {
        AimCand c[2] = { at(0, 0, 100, 0, 0, 0, 0, 0), at(0, 30, 100, 0, 0, 1, 1, 1) };
        CHECK(nhl94_aim_pick(0, 0, 0, c, 2, &s) == 1);
        c[1].calling = 0;
        CHECK(nhl94_aim_pick(0, 0, 0, c, 2, &s) == 0);
    }
    /* "calling" without "human" counts for nothing */
    {
        AimCand c[2] = { at(0, 0, 100, 0, 0, 0, 0, 0), at(0, 30, 100, 0, 0, 0, 1, 1) };
        CHECK(nhl94_aim_pick(0, 0, 0, c, 2, &s) == 0);
    }
    /* equal scores: the lowest index */
    {
        AimCand c[3] = { { 0, 100, 0, 0, 0 }, { 0, 100, 0, 0, 1 }, { 0, 100, 0, 0, 2 } };
        CHECK(nhl94_aim_pick(0, 0, 0, c, 3, &s) == 0);
    }
    /* mild distance term: two AIs dead on, the nearer wins; beyond the cap equal */
    {
        AimCand c[2] = { { 0, 200, 0, 0, 0 }, { 0, 100, 0, 0, 1 } };
        CHECK(nhl94_aim_pick(0, 0, 0, c, 2, &s) == 1);
        AimCand d[2] = { { 0, 400, 0, 0, 0 }, { 0, 330, 0, 0, 1 } };
        CHECK(nhl94_aim_pick(0, 0, 0, d, 2, &s) == 0);
    }
    /* all 8 directions, straight and 20 degrees off, with a decoy opposite and one at 90 degrees */
    for (int dir = 0; dir < 8; dir++) {
        for (int off = -20; off <= 20; off += 40) {
            AimCand c[3] = { at(dir, 180, 80, 10, -20, 0, 0, 0), at(dir, off, 120, 10, -20, 0, 0, 1),
                             at(dir, 90, 60, 10, -20, 0, 0, 2) };
            CHECK(nhl94_aim_pick(dir, 10, -20, c, 3, &s) == 1);
        }
        /* nobody ahead: the cone is empty even though people are around */
        AimCand e[2] = { at(dir, 180, 80, 0, 0, 1, 1, 0), at(dir, -90, 80, 0, 0, 1, 1, 1) };
        CHECK(nhl94_aim_pick(dir, 0, 0, e, 2, &s) == -1);
        /* the error scale along the dir is 0 and monotonic with the angle */
        int e0 = nhl94_aim_error(dir, 0, 0);
        CHECK(e0 == 0);
        int prev = -1;
        for (int deg = 0; deg <= 60; deg += 10) {
            AimCand k = at(dir, deg, 300, 0, 0, 0, 0, 0);
            int er = nhl94_aim_error(dir, k.x, k.y);
            CHECK(er >= prev);
            prev = er;
        }
        AimCand k45 = at(dir, 45, 300, 0, 0, 0, 0, 0), k61 = at(dir, 61, 300, 0, 0, 0, 0, 0);
        int e45 = nhl94_aim_error(dir, k45.x, k45.y), e61 = nhl94_aim_error(dir, k61.x, k61.y);
        CHECK(e45 > 480 && e45 < 540);              /* 1024 * sin^2 45 = 512 */
        CHECK(e61 > AIM_ERR_MAX);
    }
    /* puck on the receiver (d2 = 0): aligned */
    {
        AimCand c[1] = { { 5, 5, 0, 0, 0 } };
        CHECK(nhl94_aim_pick(3, 5, 5, c, 1, &s) == 0 && s == 0);
    }
    /* empty list */
    CHECK(nhl94_aim_pick(0, 0, 0, NULL, 0, &s) == -1);
    /* dir is taken modulo 8 */
    {
        AimCand c[1] = { { 0, 100, 0, 0, 0 } };
        CHECK(nhl94_aim_pick(8, 0, 0, c, 1, &s) == 0);
    }
    /* versus the game's octant rule (vtoa bucket +-1 octant, nearest wins): the picker differs where the cartridge
     * prefers a near 44-degree-off teammate over a far dead-on one */
    {
        AimCand c[2] = { at(0, 44, 60, 0, 0, 0, 0, 0), at(0, 2, 200, 0, 0, 0, 0, 1) };
        int j = nhl94_aim_pick(0, 0, 0, c, 2, &s);
        CHECK(j == 1);       /* the dead-on far one: 2 deg err + 50 vs 44 deg err (~480) + 15 */
    }
    printf("test_aimpass: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
