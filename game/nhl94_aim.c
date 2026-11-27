/*
 * nhl94_aim.c - the aimed-pass receiver picker. Integer only, so the result
 * is the same on every peer and every rollback replay. See nhl94_aim.h.
 */
#include "nhl94_aim.h"

/* Unit vectors (not normalised) of the game's 8 directions: dirtab order of
 * vtoa / passdir, 0 = up the screen (+y), clockwise. */
static const int8_t AX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int8_t AY[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };

static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

int nhl94_aim_error(int dir, int vx, int vy)
{
    int ax = AX[dir & 7], ay = AY[dir & 7];
    int a2 = ax * ax + ay * ay;                              /* 1 straight, 2 diagonal */
    int32_t d2 = vx * vx + vy * vy;
    if (d2 == 0) return 0;                                   /* puck on the receiver: aligned */
    int dot = ax * vx + ay * vy;
    if (dot <= 0) return -1;                                 /* 90 degrees or more */
    int64_t cq = ((int64_t)dot * dot * 1024) / ((int64_t)a2 * d2);   /* cos^2 * 1024 */
    return 1024 - (int)cq;
}

int nhl94_aim_pick(int dir, int px, int py, const AimCand *c, int n, int *score_out)
{
    int best = -1, bs = 0;
    for (int i = 0; i < n; i++) {
        int vx = c[i].x - px, vy = c[i].y - py;
        int err = nhl94_aim_error(dir, vx, vy);
        if (err < 0 || err > AIM_ERR_MAX) continue;
        uint32_t d = isqrt32((uint32_t)(vx * vx + vy * vy));
        if (d > AIM_DIST_CAP) d = AIM_DIST_CAP;
        int s = err + (int)(d >> 2) - (c[i].human ? AIM_HUMAN_BONUS : 0)
                    - (c[i].human && c[i].calling ? AIM_CALL_BONUS : 0);
        if (best < 0 || s < bs) { best = i; bs = s; }        /* strict: ties keep the first */
    }
    if (score_out) *score_out = bs;
    return best;
}
