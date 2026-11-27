/*
 * Review fixes for the occupancy / join-in-progress session code
 * (docs/DROPIN.md "Review fixes"):
 *
 *   1. rv2#1  a 2-seat room under the drop policy: the authority drops the
 *      leaving guest from the wire the guest really reached, not from wire 0
 *      (the contiguous watermark used to be kept only for rooms of 3+ seats or
 *      observers, so the drop wire came out as 0 and a spectator replaying the
 *      match from the table simulated the guest as CPU for the whole match);
 *   2. rv2#6 / rv3#4  a lone guest that goes silent is dropped when the host
 *      says its own link is evidently fine (rnet_session_set_link_ok), and not
 *      without that evidence (the old own-link-suspect rule).
 *
 * Real UDP over loopback, fake clock, as occ_schedule_test.
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

/* seats = 2 or 3 (seat 0 hubs); one observer after the seats */
typedef struct Room
{
    RNetSession *s[4];
    rnet_u8 id[4];
    int alive[4];
    int seats;
    int link_ok; /* the host's own-link evidence, fed every ms */
} Room;

static void room_pump(Room *r, int steps)
{
    int i, k;
    for (i = 0; i < steps; ++i)
    {
        g_now_ms++;
        if (r->link_ok && r->s[0])
            rnet_session_set_link_ok(r->s[0], 1);
        for (k = 0; k <= r->seats; ++k)
        {
            if (!r->alive[k]) continue;
            rnet_session_pump(r->s[k]);
            if (rnet_session_try_admit(r->s[k], rnet_session_sim_tick(r->s[k])))
                rnet_session_advance(r->s[k]);
        }
    }
}

static int room_open(Room *r, int seats, unsigned port, rnet_u32 session)
{
    RNetConfig c[4];
    RNetHostVTable v;
    char bind[4][64], hub[64];
    int k, i, n = seats + 1;
    memset(r, 0, sizeof(*r));
    r->seats = seats;
    rnet_config_init_defaults(&c[0]);
    c[0].slot_count = (rnet_u8)seats;
    c[0].input_delay = 3;
    c[0].session_id = session;
    for (k = 1; k < n; ++k) c[k] = c[0];
    for (k = 0; k < seats; ++k) c[k].local_slot = (rnet_u8)k;
    c[seats].local_slot = (rnet_u8)seats; /* observer sentinel */
    c[seats].wire_slot = (rnet_u8)seats;
    for (k = 0; k < n; ++k) snprintf(bind[k], sizeof bind[k], "127.0.0.1:%u", port + (unsigned)k);
    snprintf(hub, sizeof hub, "%s", bind[0]);
    memset(&v, 0, sizeof v);
    v.sample_local = sample_local; v.publish = publish; v.now_ms = now_ms;
    for (k = 0; k < n; ++k)
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
    for (k = 1; k < n; ++k)
        if (rnet_session_start_lan(r->s[k], bind[k], hub) != 0) return 0;
    for (i = 0; i < 20000; ++i)
    {
        int all = 1;
        room_pump(r, 1);
        for (k = 0; k < n; ++k) all = all && rnet_session_is_running(r->s[k]);
        if (all) break;
    }
    return rnet_session_is_running(r->s[0]) && rnet_session_is_running(r->s[1]);
}

static void room_close(Room *r)
{
    int k;
    for (k = 0; k <= r->seats; ++k) if (r->s[k]) rnet_session_destroy(r->s[k]);
}

int main(void)
{
    Room r;
    RNetInputSample smp;
    rnet_u32 w = 0, sim0;
    int slot = -1, present = -1;
    unsigned port = 45500u + (test_pid() % 300u) * 8u;

    /* ---- 1. two seats + a spectator: the guest's BYE drops it from where it really was ---- */
    if (!room_open(&r, 2, port, 0x0bb00001u ^ test_pid())) { fail("2-seat room did not start"); goto done; }
    room_pump(&r, 900);
    sim0 = rnet_session_sim_tick(r.s[0]);
    EXPECT(sim0 > 40, "the 2-seat room runs");
    (void)rnet_session_send_bye(r.s[1]);
    r.alive[1] = 0;
    room_pump(&r, 600);
    EXPECT(rnet_session_occ_gen(r.s[0]) == 1, "authority recorded the guest's drop");
    if (rnet_session_occ_event(r.s[0], 0, &w, &slot, &present))
    {
        EXPECT(slot == 1 && present == 0, "event is 'seat 1 absent'");
        EXPECT(w > 20u, "the drop wire is where the guest's rows ended, not wire 0");
        EXPECT(w <= sim0 + 600u, "...and not beyond anything the guest could have sent");
        EXPECT(rnet_session_peek_remote_input(r.s[0], 1, w - 1, &smp) == 1 && smp.valid && !smp.absent,
               "the authority holds the guest's real row just below the drop wire");
        EXPECT(rnet_session_occ_event(r.s[2], 0, &w, NULL, NULL), "the spectator holds the table");
    }
    else fail("no occupancy event on the authority");
    room_close(&r);

    /* ---- 2. link evidence: a lone silent guest of a 3-seat room whose second seat is absent ---- */
    port += 8;
    if (!room_open(&r, 3, port, 0x0bb00002u ^ test_pid())) { fail("3-seat room did not start"); goto done; }
    room_pump(&r, 400);
    (void)rnet_session_send_bye(r.s[2]); /* seat 2 leaves: seat 1 is the only guest left */
    r.alive[2] = 0;
    room_pump(&r, 600);
    EXPECT(rnet_session_occ_gen(r.s[0]) == 1, "seat 2 dropped");
    r.alive[1] = 0; /* the last guest crashes */
    for (w = 0; w < 4; ++w) room_pump(&r, 1000);
    EXPECT(rnet_session_occ_gen(r.s[0]) == 1,
           "no evidence the host's link is fine: the lone silent guest is NOT dropped (as before)");
    r.link_ok = 1; /* the runner: the host's lobby websocket is alive */
    room_pump(&r, 600);
    EXPECT(rnet_session_occ_gen(r.s[0]) == 2 && rnet_session_absent_mask(r.s[0]) == ((1u << 1) | (1u << 2)),
           "with the host's link evidently fine the lone silent guest is dropped too");
    room_close(&r);

done:
    if (g_failures == 0) { printf("occ_harden_test: ok\n"); return 0; }
    fprintf(stderr, "occ_harden_test: %d failure(s)\n", g_failures);
    return 1;
}
