/* Link-aware input delay: D from RTT and jitter (pure function). */
#include "recomp_net/sched.h"

#include <stdio.h>

static int failures;
#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            printf("FAIL: %s\n", msg);                     \
            failures++;                                    \
        }                                                  \
    } while (0)

int main(void)
{
    /* 0 ms link: 4 ms slack -> 1 tick + margin 1 */
    CHECK(rnet_sched_link_delay_ticks(0, 0, 1) == 2, "idle link, margin 1 -> 2");
    CHECK(rnet_sched_link_delay_ticks(0, 0, 0) == 2, "floor is 2");
    /* 24 ms RTT, 4 ms jitter: 12 + 4 + 4 = 20 ms -> 2 ticks + 1 */
    CHECK(rnet_sched_link_delay_ticks(24, 4, 1) == 3, "LAN 24/4 -> 3");
    /* 100 ms RTT, 10 ms jitter: 50 + 10 + 4 = 64 ms -> 4 ticks + 1 */
    CHECK(rnet_sched_link_delay_ticks(100, 10, 1) == 5, "WAN 100/10 -> 5");
    CHECK(rnet_sched_link_delay_ticks(100, 10, 0) == 4, "WAN 100/10 margin 0 -> 4");
    /* exact tick boundary rounds up: need 16.683 ms exactly would be 1 tick;
     * 1 ms over is 2 */
    CHECK(rnet_sched_link_delay_ticks(25, 0, 0) == 2, "16.5 ms -> 1 tick, floored 2");
    CHECK(rnet_sched_link_delay_ticks(60, 0, 0) == 3, "34 ms -> 3 ticks");
    /* monotonic in rtt and jitter */
    {
        int prev = 0;
        unsigned r;
        for (r = 0; r <= 600; r += 10) {
            int d = rnet_sched_link_delay_ticks(r, 5, 1);
            CHECK(d >= prev, "monotonic in rtt");
            prev = d;
        }
    }
    CHECK(rnet_sched_link_delay_ticks(2000, 500, 8) == 20, "clamped at 20");
    if (failures)
        return 1;
    printf("ALL PASS\n");
    return 0;
}
