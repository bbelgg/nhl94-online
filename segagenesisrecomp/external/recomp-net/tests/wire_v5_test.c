/* Wire version 4: 16-bit per-seat masks and the 4-bit rollback epoch seat. */
#include "protocol/rnet_protocol.h"
#include "recomp_net/rb_driver.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void expect_true(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_failures++;
    }
}

#define MAGIC 0x524E4554u

int main(void)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    RNetDecodedPacket dec;
    rnet_u32 need[12];
    int n, i;
    uint32_t seat;

    expect_true(RNET_WIRE_VERSION >= 5u, "wire version is at least 5 (v6 adds the occupancy schedule)");
    expect_true(RNET_MAX_SLOTS == 16, "16 seats");

    /* STATE_PROBE replied mask with seats >= 8 */
    n = rnet_proto_encode_state_probe_ex(buf, sizeof(buf), MAGIC, 0x1234, 0, 1, 0, 4096u, 0xCAFEu,
                                         (rnet_u16)((1u << 11) | (1u << 9) | (1u << 2)));
    expect_true(n > 0, "probe encodes");
    expect_true(rnet_proto_decode(buf, (size_t)n, MAGIC, &dec) == 0, "probe decodes");
    expect_true(dec.type == RNET_PKT_STATE_PROBE, "probe type");
    expect_true(dec.state_probe_replied == (rnet_u16)((1u << 11) | (1u << 9) | (1u << 2)),
                "probe replied mask keeps seats 2, 9, 11");
    expect_true(dec.state_total_size == 4096u && dec.state_payload_crc == 0xCAFEu,
                "probe size/crc unharmed by the trailing byte");
    n = rnet_proto_encode_state_probe_ex(buf, sizeof(buf), MAGIC, 0x1234, 0, 1, 0, 8u, 1u, 0xFFFFu);
    expect_true(n > 0 && rnet_proto_decode(buf, (size_t)n, MAGIC, &dec) == 0 &&
                    dec.state_probe_replied == 0xFFFFu,
                "probe replied mask all 16 seats");

    /* OBS_CATCHUP commit-0 mask with seat 11 */
    for (i = 0; i < 12; ++i)
        need[i] = (rnet_u32)(100 + i);
    n = rnet_proto_encode_obs_catchup(buf, sizeof(buf), MAGIC, 0x1234, 13, 7, RNET_OBS_FLAG_RUNNING,
                                      55u, (rnet_u16)((1u << 11) | 1u), need, 12);
    expect_true(n > 0, "obs_catchup encodes");
    expect_true(rnet_proto_decode(buf, (size_t)n, MAGIC, &dec) == 0, "obs_catchup decodes");
    expect_true(dec.type == RNET_PKT_OBS_CATCHUP, "obs_catchup type");
    expect_true(dec.obs_commit0_mask == (rnet_u16)((1u << 11) | 1u), "commit0 mask keeps seat 11");
    expect_true(dec.local_slot == 13 && dec.input_epoch == 7 && dec.obs_host_xfer_done == 55u,
                "obs_catchup header fields");
    expect_true(dec.ack_count == 12 && dec.acks[0] == 100u && dec.acks[11] == 111u,
                "obs_catchup need[] of 12 seats");

    /* Rollback epoch: initiator seat 0..15 survives, sequence is not clobbered. */
    expect_true(RNET_RB_EPOCH_SLOT_BITS == 4u, "epoch seat field is 4 bits");
    for (seat = 0; seat < 16; ++seat)
    {
        uint32_t e = rnet_rb_epoch_make(5u, seat);
        expect_true(rnet_rb_epoch_initiator(e) == seat, "epoch initiator round-trips");
        expect_true((e >> RNET_RB_EPOCH_SLOT_BITS) == 5u, "epoch sequence intact");
    }
    expect_true(rnet_rb_epoch_initiator(rnet_rb_epoch_make(3u, 9u)) == 9u, "seat 9 is not seat 1");
    expect_true(rnet_rb_epoch_initiator(rnet_rb_epoch_make(3u, 15u)) == 15u, "seat 15");

    if (g_failures)
        return 1;
    printf("ALL PASS\n");
    return 0;
}
