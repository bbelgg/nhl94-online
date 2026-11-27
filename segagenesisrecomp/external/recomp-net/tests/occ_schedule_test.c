/*
 * Occupancy schedule (wire v6): a seat that leaves a 3-seat room mid-match is
 * scheduled ABSENT by the authority, every peer (and a spectator) applies the
 * same table, and nothing waits on it any more.
 *
 * Real UDP over loopback, fake clock: seat 0 hubs, seats 1 and 2 dial it, an
 * observer watches. Delay-sync stepping is only used to get the rings moving;
 * what is under test is the table, not admission.
 *
 *   1. wire round trip of OCC_EVENT / OCC_ACK, and the version bump to 6;
 *   2. seat 2 says BYE: the authority drops it AT ONCE (no timeout), every
 *      peer holds the same event (same wire), the dropped seat's rows below
 *      the wire stay real and rows from it are the flagged zero row;
 *   3. remote_tip of the dropped seat is "none", its liveness stops counting;
 *   4. NEGATIVE CONTROL: seat 1 then goes silent, but nobody else is heard --
 *      the authority's own link is the suspect, so it is NOT dropped;
 *   5. a silent seat WHILE others talk is dropped after the timeout (second
 *      room, seat 2 never says BYE);
 *   6. the dropped seat itself learns it was dropped.
 */
#include "protocol/rnet_protocol.h"
#include "recomp_net/recomp_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
static unsigned test_pid(void) { return (unsigned)_getpid(); }
#else
#include <unistd.h>
static unsigned test_pid(void) { return (unsigned)getpid(); }
#endif

#define MAGIC 0x524E4554u
#define DROP_MS 1500u

static int g_failures;
static rnet_u64 g_now_ms = 1000;

static void fail(const char *m)
{
    fprintf(stderr, "FAIL: %s\n", m);
    g_failures++;
}
#define EXPECT(c, m) do { if (!(c)) fail(m); } while (0)

static void sample_local(rnet_u32 tick, RNetInputSample *out, void *opaque)
{
    memset(out, 0, sizeof(*out));
    out->size = 4;
    out->bytes[0] = *(rnet_u8 *)opaque;
    out->bytes[1] = (rnet_u8)tick;
    out->valid = 1;
}
static void publish(rnet_u32 tick, const RNetInputSample *by_slot, int slots, void *opaque)
{
    (void)tick; (void)by_slot; (void)slots; (void)opaque;
}
static rnet_u64 now_ms(void *opaque) { (void)opaque; return g_now_ms; }

typedef struct Room
{
    RNetSession *s[4]; /* 0 host, 1..2 guests, 3 observer */
    rnet_u8 id[4];
    int alive[4];
} Room;

static void room_pump(Room *r, int steps)
{
    int i, k;
    for (i = 0; i < steps; ++i)
    {
        g_now_ms++;
        for (k = 0; k < 4; ++k)
        {
            if (!r->alive[k]) continue;
            rnet_session_pump(r->s[k]);
            if (rnet_session_try_admit(r->s[k], rnet_session_sim_tick(r->s[k])))
                rnet_session_advance(r->s[k]);
        }
    }
}

static int room_open(Room *r, unsigned port, rnet_u32 session)
{
    RNetConfig c[4];
    RNetHostVTable v;
    char bind[4][64], hub[64];
    int k, i;
    memset(r, 0, sizeof(*r));
    rnet_config_init_defaults(&c[0]);
    c[0].slot_count = 3;
    c[0].input_delay = 3;
    c[0].session_id = session;
    for (k = 1; k < 4; ++k) c[k] = c[0];
    c[0].local_slot = 0;
    c[1].local_slot = 1;
    c[2].local_slot = 2;
    c[3].local_slot = 3; /* observer sentinel == slot_count */
    c[3].wire_slot = 3;
    for (k = 0; k < 4; ++k) snprintf(bind[k], sizeof bind[k], "127.0.0.1:%u", port + (unsigned)k);
    snprintf(hub, sizeof hub, "%s", bind[0]);
    memset(&v, 0, sizeof v);
    v.sample_local = sample_local; v.publish = publish; v.now_ms = now_ms;
    for (k = 0; k < 4; ++k)
    {
        RNetHostVTable vk = v;
        r->id[k] = (rnet_u8)(10 + k);
        vk.ctx = &r->id[k];
        r->s[k] = rnet_session_create(&c[k], &vk);
        if (!r->s[k]) return 0;
        rnet_session_set_seat_loss_policy(r->s[k], DROP_MS);
        r->alive[k] = 1;
    }
    if (rnet_session_start_lan_hub(r->s[0], bind[0]) != 0) return 0;
    for (k = 1; k < 4; ++k)
        if (rnet_session_start_lan(r->s[k], bind[k], hub) != 0) return 0;
    for (i = 0; i < 20000; ++i)
    {
        room_pump(r, 1);
        if (rnet_session_is_running(r->s[0]) && rnet_session_is_running(r->s[1]) &&
            rnet_session_is_running(r->s[2]) && rnet_session_is_running(r->s[3]))
            break;
    }
    return rnet_session_is_running(r->s[0]) && rnet_session_is_running(r->s[2]);
}

static void room_close(Room *r)
{
    int k;
    for (k = 0; k < 4; ++k) rnet_session_destroy(r->s[k]);
}

static void wire_roundtrip(void)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    RNetDecodedPacket dec;
    RNetOccWire ev[2] = { { 3306u, 2, 0, 1 }, { 4000u, 2, 1, 2 } };
    int n;
    EXPECT(RNET_WIRE_VERSION == 6u, "wire version is 6");
    n = rnet_proto_encode_occ_event(buf, sizeof buf, MAGIC, 7u, 0, ev, 2);
    EXPECT(n > 0 && rnet_proto_decode(buf, (size_t)n, MAGIC, &dec) == 0, "occ_event round trips");
    EXPECT(dec.type == RNET_PKT_OCC_EVENT && dec.local_slot == 0 && dec.occ_count == 2 &&
               dec.occ[0].wire == 3306u && dec.occ[0].slot == 2 && !dec.occ[0].present && dec.occ[0].gen == 1 &&
               dec.occ[1].wire == 4000u && dec.occ[1].present && dec.occ[1].gen == 2,
           "occ_event fields");
    n = rnet_proto_encode_occ_ack(buf, sizeof buf, MAGIC, 7u, 2, 9);
    EXPECT(n > 0 && rnet_proto_decode(buf, (size_t)n, MAGIC, &dec) == 0 && dec.type == RNET_PKT_OCC_ACK &&
               dec.local_slot == 2 && dec.occ_ack_gen == 9,
           "occ_ack round trips");
    buf[12] ^= 0x55; /* corrupt: the checksum must refuse it */
    EXPECT(rnet_proto_decode(buf, (size_t)n, MAGIC, &dec) != 0, "corrupt occ_ack refused");
}

int main(void)
{
    Room r;
    RNetInputSample smp;
    rnet_u32 w, tip = 0;
    int slot = -1, present = -1;
    unsigned port = 43500u + (test_pid() % 500u) * 8u;
    int i;

    wire_roundtrip();

    /* ---- room 1: BYE, then the negative control ---- */
    if (!room_open(&r, port, 0x0cc00001u ^ test_pid())) { fail("room 1 did not start"); goto done; }
    room_pump(&r, 400);
    EXPECT(rnet_session_occ_gen(r.s[0]) == 0, "no event while everyone is here");
    EXPECT(rnet_session_remote_tip(r.s[0], 2, &tip) == 1 &&
               rnet_session_peek_remote_input(r.s[0], 2, tip, &smp) == 1 && smp.valid && !smp.absent,
           "a present seat's row is real");

    (void)rnet_session_send_bye(r.s[2]);
    r.alive[2] = 0; /* gone for good */
    room_pump(&r, 600); /* far below DROP_MS: this is the BYE path */
    EXPECT(rnet_session_occ_gen(r.s[0]) == 1, "authority dropped the BYE seat at once");
    EXPECT(rnet_session_absent_mask(r.s[0]) == (1u << 2), "authority: seat 2 absent");
    EXPECT(rnet_session_absent_mask(r.s[1]) == (1u << 2), "guest: seat 2 absent (table delivered)");
    EXPECT(rnet_session_absent_mask(r.s[3]) == (1u << 2), "observer: seat 2 absent");
    EXPECT(!rnet_session_peer_gone(r.s[0], 0) && rnet_session_peer_disconnected(r.s[1], 0) == 0,
           "a guest's BYE did not set the aggregate peer_gone");
    if (rnet_session_occ_event(r.s[0], 0, &w, &slot, &present))
    {
        rnet_u32 w1 = 0, w3 = 0;
        EXPECT(slot == 2 && present == 0, "event is 'seat 2 absent'");
        EXPECT(rnet_session_occ_event(r.s[1], 0, &w1, NULL, NULL) && w1 == w, "guest holds the same wire");
        EXPECT(rnet_session_occ_event(r.s[3], 0, &w3, NULL, NULL) && w3 == w, "observer holds the same wire");
        EXPECT(rnet_session_peek_remote_input(r.s[1], 2, w + 3, &smp) == 1 && smp.valid && smp.absent &&
                   smp.size == 0,
               "from the event wire on the row is the flagged zero row");
        EXPECT(w > 0 && rnet_session_peek_remote_input(r.s[1], 2, w - 1, &smp) == 1 && smp.valid &&
                   !smp.absent && smp.size == 4,
               "below the event wire the row is the real one the seat sent");
        EXPECT(rnet_session_occupied_at(r.s[1], 2, w - 1) && !rnet_session_occupied_at(r.s[1], 2, w),
               "occupied_at flips exactly at the wire");
    }
    else fail("no occupancy event on the authority");
    EXPECT(rnet_session_remote_tip(r.s[1], 2, &tip) == 0, "no tip to wait for on a dropped seat");
    EXPECT(rnet_session_peer_slot_disconnected(r.s[0], 2, 1) == 0, "a dropped seat is not 'disconnected' again");

    /* Negative control: seat 1 silent, and nobody else talks to the authority
     * except the observer, whose packets never count as a seat's. */
    r.alive[1] = 0;
    for (i = 0; i < 4; ++i) room_pump(&r, 1000);
    EXPECT(rnet_session_occ_gen(r.s[0]) == 1, "seat 1 NOT dropped when no other seat is heard (own link suspect)");
    room_close(&r);

    /* ---- room 2: silence while the others talk ---- */
    port += 4;
    if (!room_open(&r, port, 0x0cc00002u ^ test_pid())) { fail("room 2 did not start"); goto done; }
    room_pump(&r, 300);
    r.alive[2] = 0; /* crash: no BYE */
    room_pump(&r, DROP_MS / 2);
    EXPECT(rnet_session_occ_gen(r.s[0]) == 0, "not dropped before the timeout");
    room_pump(&r, DROP_MS);
    EXPECT(rnet_session_occ_gen(r.s[0]) == 1 && rnet_session_absent_mask(r.s[0]) == (1u << 2),
           "silent seat dropped after the timeout while seat 1 is heard");
    EXPECT(rnet_session_absent_mask(r.s[1]) == (1u << 2), "guest learned of it");
    EXPECT(!rnet_session_dropped_self(r.s[1]) && !rnet_session_dropped_self(r.s[0]), "survivors were not dropped");
    /* the dropped seat comes back to life (it was only stalled): it must learn */
    r.alive[2] = 1;
    room_pump(&r, 600);
    EXPECT(rnet_session_dropped_self(r.s[2]), "the dropped seat learns it was dropped");
    EXPECT(rnet_session_occ_gen(r.s[0]) == 1, "a returning ghost does not re-add itself");
    room_close(&r);

done:
    if (g_failures == 0)
    {
        printf("occ_schedule_test: ok\n");
        return 0;
    }
    fprintf(stderr, "occ_schedule_test: %d failure(s)\n", g_failures);
    return 1;
}
