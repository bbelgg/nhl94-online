/*
 * nhl94_aim.h - the aimed-pass receiver picker (pure integer, no engine
 * dependencies, unit-tested by tools/seats/test_aimpass.c). Used by
 * nhl94_seats.c aim_pass_hook at dopass ($00BD5E) for a human passer.
 * See docs/PLAYER_LOCK.md, "Aimed passing".
 */
#ifndef NHL94_AIM_H
#define NHL94_AIM_H
#include <stdint.h>

/* error = 1024 * sin^2(angle between the aim and the pass line), so
 * 10 deg = 30, 20 deg = 120, 30 deg = 256, 45 deg = 512, 60 deg = 768. */
#define AIM_ERR_MAX     768   /* the cone: 60 degrees (anything wider is out) */
#define AIM_HUMAN_BONUS 150   /* a human within ~22 deg beats an AI dead on the stick */
#define AIM_CALL_BONUS  300   /* extra for a human calling for the puck (150+300 = 450: ~41 deg off beats an
                                 AI dead on the stick at the same distance) */
#define AIM_DIST_CAP    320   /* px; the distance term is min(dist, cap) / 4 <= 80 */

typedef struct {
    int16_t x, y;       /* receiver position (rink px, +y up the screen) */
    uint8_t human;      /* pfjoycon: a human seat controls him */
    uint8_t calling;    /* ... and that seat holds B / X (calls for the puck) */
    int8_t  sc;         /* caller's id (the object number); not used by the picker */
} AimCand;

/* Aim direction dir 0..7 = the game's passdir / vtoa code (0 = +y, 1 = +x+y,
 * 2 = +x, 3 = +x-y, 4 = -y, 5 = -x-y, 6 = -x, 7 = -x+y), puck at (px, py).
 * Returns the index into c[0..n) of the best candidate, or -1 when nobody is
 * inside the cone. Lowest score wins: error + dist/4 - bonuses; a tie goes to
 * the lower index. *score_out (optional) = the winner's score. */
int nhl94_aim_pick(int dir, int px, int py, const AimCand *c, int n, int *score_out);

/* The angle error (see AIM_ERR_MAX) of the vector (vx, vy) against dir, or -1
 * when it is 90 degrees or more away. (0, 0) = puck on the receiver = 0. */
int nhl94_aim_error(int dir, int vx, int vy);

#endif /* NHL94_AIM_H */
