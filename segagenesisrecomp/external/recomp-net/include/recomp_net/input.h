#ifndef RECOMP_NET_INPUT_H
#define RECOMP_NET_INPUT_H

#include "recomp_net/config.h"
#include "recomp_net/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Host-defined pad layout lives in `bytes`. The library never interprets the
 * payload — only tick ownership and presence matter for delay-sync admission.
 */
typedef struct RNetInputSample
{
    rnet_u32 tick;
    rnet_u16 size;
    rnet_u8 bytes[RNET_INPUT_MAX];
    rnet_u8 valid;
    /* In-process only, never on the wire: 1 on the zero row the session
     * synthesizes for a seat the authority's occupancy table says is absent at
     * that wire tick (a dropped peer -- the host plays it as CPU from then on).
     * Distinct from a neutral pad: the game decides human vs CPU from it. */
    rnet_u8 absent;
} RNetInputSample;

static inline rnet_u32 rnet_wire_tick_from_sim(rnet_u32 sim_tick, rnet_u8 delay)
{
    return sim_tick + (rnet_u32)delay;
}

static inline rnet_u32 rnet_sim_tick_from_wire(rnet_u32 wire_tick, rnet_u8 delay)
{
    if (wire_tick < (rnet_u32)delay)
    {
        return 0;
    }
    return wire_tick - (rnet_u32)delay;
}

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_NET_INPUT_H */
