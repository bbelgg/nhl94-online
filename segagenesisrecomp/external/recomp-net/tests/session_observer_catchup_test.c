/*
 * Spectator catch-up (wire version 3): a spectator that was not there, or
 * lost something, can still follow the match -- or is told it cannot.
 *
 * Three real UDP peers over loopback, the host hubbing (the LAN hub / host
 * relay shape). Cases:
 *   1. roster gate: with expected_observers = 1 the host holds START until
 *      the spectator's READY arrives, then everyone runs;
 *   2. a spectator that binds after START (within the join window) is
 *      answered with START and pulls the seats' rows from tick 0, which no
 *      live INPUT bundle carries any more (they hold the last few rows);
 *   3. it also receives the host's boot transfer (SRAM) that every seat had
 *      already completed before it arrived;
 *   4. a spectator that binds past RNET_OBS_JOIN_MAX_TICK is refused with
 *      RNET_OBS_REFUSE_LATE_JOIN instead of sitting at tick 0;
 *   5. a spectator's BYE ends nobody's match.
 *   6. OBS_CATCHUP / OBS_REFUSE round-trip through the codec.
 */
#include "recomp_net/recomp_net.h"
#include "protocol/rnet_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
static unsigned test_pid(void) { return (unsigned)_getpid(); }
static void real_yield(void) { Sleep(1); }
#else
#include <unistd.h>
static unsigned test_pid(void) { return (unsigned)getpid(); }
static void real_yield(void) { usleep(200); }
#endif

typedef struct HostCtx
{
    rnet_u8 slot;
    int published;
} HostCtx;

static int g_failures;
static rnet_u64 g_now_ms = 1000;

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    g_failures++;
}

static void sample_local(rnet_u32 tick, RNetInputSample *out, void *opaque)
{
    HostCtx *ctx = (HostCtx *)opaque;
    memset(out, 0, sizeof(*out));
    out->size = 4;
    out->bytes[0] = ctx->slot;
    out->bytes[1] = (rnet_u8)tick;
    out->bytes[2] = (rnet_u8)(tick >> 8);
    out->bytes[3] = (rnet_u8)(0x5au ^ ctx->slot);
    out->valid = 1;
}

static void publish(rnet_u32 tick, const RNetInputSample *by_slot, int slots, void *opaque)
{
    (void)tick;
    (void)by_slot;
    (void)slots;
    ((HostCtx *)opaque)->published++;
}

static rnet_u64 now_ms(void *opaque)
{
    (void)opaque;
    return g_now_ms;
}

typedef struct Room
{
    RNetSession *a, *b, *c;
    HostCtx ha, hb, hc;
    RNetHostVTable va, vb, vc;
    RNetConfig ca, cb, cc;
    char bind_a[64], bind_b[64], bind_c[64];
} Room;

static void room_init(Room *r, unsigned base_port, rnet_u32 session, rnet_u8 expected)
{
    memset(r, 0, sizeof(*r));
    r->ha.slot = 0;
    r->hb.slot = 1;
    r->hc.slot = 9;
    r->va.sample_local = sample_local;
    r->va.publish = publish;
    r->va.now_ms = now_ms;
    r->va.ctx = &r->ha;
    r->vb = r->va;
    r->vb.ctx = &r->hb;
    r->vc = r->va;
    r->vc.ctx = &r->hc;
    rnet_config_init_defaults(&r->ca);
    r->ca.slot_count = 2;
    r->ca.input_delay = 3;
    r->ca.session_id = session;
    r->cb = r->ca;
    r->cc = r->ca;
    r->ca.local_slot = 0;
    r->ca.expected_observers = expected;
    r->cb.local_slot = 1;
    r->cc.local_slot = 2; /* observer sentinel */
    r->cc.wire_slot = 2;
    snprintf(r->bind_a, sizeof(r->bind_a), "127.0.0.1:%u", base_port);
    snprintf(r->bind_b, sizeof(r->bind_b), "127.0.0.1:%u", base_port + 1u);
    snprintf(r->bind_c, sizeof(r->bind_c), "127.0.0.1:%u", base_port + 2u);
    r->a = rnet_session_create(&r->ca, &r->va);
    r->b = rnet_session_create(&r->cb, &r->vb);
    r->c = rnet_session_create(&r->cc, &r->vc);
    if (!r->a || !r->b || !r->c || rnet_session_start_lan_hub(r->a, r->bind_a) != 0 ||
        rnet_session_start_lan(r->b, r->bind_b, r->bind_a) != 0)
        fail("room start");
}

static void room_start_spectator(Room *r)
{
    if (rnet_session_start_lan(r->c, r->bind_c, r->bind_a) != 0)
        fail("spectator start");
}

static void room_destroy(Room *r)
{
    rnet_session_destroy(r->a);
    rnet_session_destroy(r->b);
    rnet_session_destroy(r->c);
}

/* One ms of fake time: pump everyone; seats admit up to `seat_ticks`. */
static void room_step(Room *r, int with_c, rnet_u32 seat_ticks)
{
    g_now_ms++;
    rnet_session_pump(r->a);
    rnet_session_pump(r->b);
    if (with_c)
        rnet_session_pump(r->c);
    if (rnet_session_sim_tick(r->a) < seat_ticks && rnet_session_try_admit(r->a, rnet_session_sim_tick(r->a)))
        rnet_session_advance(r->a);
    if (rnet_session_sim_tick(r->b) < seat_ticks && rnet_session_try_admit(r->b, rnet_session_sim_tick(r->b)))
        rnet_session_advance(r->b);
}

static void test_roster_gate(unsigned port, rnet_u32 sid)
{
    Room r;
    int i;
    room_init(&r, port, sid, 1);
    for (i = 0; i < 2000; ++i)
        room_step(&r, 0, 0);
    if (rnet_session_is_running(r.a) || rnet_session_is_running(r.b))
        fail("roster: START went out before the rostered spectator was ready");
    room_start_spectator(&r);
    for (i = 0; i < 3000 && !(rnet_session_is_running(r.a) && rnet_session_is_running(r.b) &&
                              rnet_session_is_running(r.c));
         ++i)
    {
        room_step(&r, 1, 0);
        /* Fake time races real UDP delivery: let loopback packets land, or the
         * 3000 steps can finish before the spectator's READY is even read. */
        real_yield();
    }
    if (!rnet_session_is_running(r.a) || !rnet_session_is_running(r.b) || !rnet_session_is_running(r.c))
        fail("roster: the room did not start once the spectator was ready");
    room_destroy(&r);
}

static void test_join_after_start(unsigned port, rnet_u32 sid)
{
    Room r;
    int i, rows = 0;
    static const rnet_u8 sram[5000] = { 1, 2, 3, 4, 5 };
    rnet_u8 op = 0, slot = 0;
    const void *data = NULL;
    size_t size = 0;
    int got_sram = 0;

    room_init(&r, port, sid, 0);
    for (i = 0; i < 3000 && !(rnet_session_is_running(r.a) && rnet_session_is_running(r.b)); ++i)
        room_step(&r, 0, 0);
    if (!rnet_session_is_running(r.a) || !rnet_session_is_running(r.b))
        fail("join: seats did not start");
    /* The host's boot transfer, completed by the only seat before the
     * spectator exists. */
    if (rnet_session_state_begin(r.a, RNET_STATE_OP_SRAM, 0, sram, sizeof(sram)) != 0)
        fail("join: state_begin");
    for (i = 0; i < 3000; ++i)
    {
        room_step(&r, 0, 0);
        if (rnet_session_state_take_ready(r.b, &op, &slot, &data, &size))
        {
            rnet_session_state_finish(r.b, 0);
            break;
        }
    }
    for (i = 0; i < 3000; ++i)
    {
        room_step(&r, 0, 0);
        if (rnet_session_state_take_ready(r.a, &op, &slot, &data, &size))
        {
            rnet_session_state_finish(r.a, 0);
            break;
        }
    }
    /* Seats play 20 ticks: well past what the live INPUT window still carries. */
    for (i = 0; i < 5000 && (rnet_session_sim_tick(r.a) < 20u || rnet_session_sim_tick(r.b) < 20u); ++i)
        room_step(&r, 0, 20u);
    if (rnet_session_sim_tick(r.a) < 20u)
        fail("join: seats did not advance");
    room_start_spectator(&r);
    for (i = 0; i < 5000; ++i)
    {
        room_step(&r, 1, 20u);
        if (!got_sram && rnet_session_state_take_ready(r.c, &op, &slot, &data, &size))
        {
            got_sram = (op == RNET_STATE_OP_SRAM && size == sizeof(sram) && !memcmp(data, sram, size));
            if (!got_sram)
                fail("join: the served transfer differs");
            rnet_session_state_finish(r.c, 0);
        }
        if (rnet_session_is_running(r.c) && got_sram)
        {
            RNetInputSample s0, s1;
            rows = 0;
            /* Wire 0..D-1 is the neutral prefix; wire D is the first real
             * sample (sim 0), long out of every live bundle. */
            if (rnet_session_peek_remote_input(r.c, 0, 0, &s0) && rnet_session_peek_remote_input(r.c, 1, 0, &s1) &&
                rnet_session_peek_remote_input(r.c, 0, r.ca.input_delay, &s0) &&
                rnet_session_peek_remote_input(r.c, 1, r.ca.input_delay, &s1) &&
                s0.bytes[0] == 0 && s1.bytes[0] == 1 && s0.bytes[1] == 0 && s1.bytes[1] == 0)
                rows = 1;
            if (rows)
                break;
        }
    }
    if (!rnet_session_is_running(r.c))
        fail("join: a spectator that bound after START was never started");
    if (!got_sram)
        fail("join: the spectator never received the host's completed transfer");
    if (!rows)
        fail("join: the spectator never recovered the seats' tick-0 rows");
    if (rnet_session_observer_refused(r.c, NULL, NULL, NULL))
        fail("join: a spectator inside the join window was refused");
    /* 5: its BYE ends nobody's match. */
    rnet_session_send_bye(r.c);
    for (i = 0; i < 200; ++i)
        room_step(&r, 0, 20u);
    if (rnet_session_peer_disconnected(r.a, 0) || rnet_session_peer_disconnected(r.b, 0))
        fail("bye: a spectator leaving ended the seats' match");
    room_destroy(&r);
}

static void test_join_too_late(unsigned port, rnet_u32 sid)
{
    Room r;
    int i;
    rnet_u8 reason = 0;
    rnet_u32 tick = 0;
    room_init(&r, port, sid, 0);
    for (i = 0; i < 20000 && rnet_session_sim_tick(r.a) < RNET_OBS_JOIN_MAX_TICK + 12u; ++i)
        room_step(&r, 0, RNET_OBS_JOIN_MAX_TICK + 12u);
    if (rnet_session_sim_tick(r.a) < RNET_OBS_JOIN_MAX_TICK + 12u)
        fail("late: seats did not advance past the join window");
    room_start_spectator(&r);
    for (i = 0; i < 3000 && !rnet_session_observer_refused(r.c, NULL, NULL, NULL); ++i)
        room_step(&r, 1, RNET_OBS_JOIN_MAX_TICK + 12u);
    if (!rnet_session_observer_refused(r.c, &reason, &tick, NULL))
        fail("late: a spectator past the join window was not refused");
    else if (reason != RNET_OBS_REFUSE_LATE_JOIN || tick <= RNET_OBS_JOIN_MAX_TICK)
        fail("late: wrong refusal");
    if (rnet_session_is_running(r.c))
        fail("late: a refused spectator was started");
    room_destroy(&r);
}

static void test_codec(void)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    RNetDecodedPacket p;
    rnet_u32 need[3] = { 7u, RNET_OBS_NEED_NONE, 0u };
    int n = rnet_proto_encode_obs_catchup(buf, sizeof(buf), 0x524E4554u, 42u, 5u, 3u,
                                          RNET_OBS_FLAG_RUNNING, 99u, 0x2u, need, 3u);
    if (n <= 0 || rnet_proto_decode(buf, (size_t)n, 0x524E4554u, &p) != 0 ||
        p.type != RNET_PKT_OBS_CATCHUP || p.local_slot != 5u || p.input_epoch != 3u ||
        p.obs_flags != RNET_OBS_FLAG_RUNNING || p.obs_host_xfer_done != 99u ||
        p.obs_commit0_mask != 0x2u || p.ack_count != 3u || p.acks[0] != 7u ||
        p.acks[1] != RNET_OBS_NEED_NONE || p.acks[2] != 0u)
        fail("codec: OBS_CATCHUP");
    n = rnet_proto_encode_obs_refuse(buf, sizeof(buf), 0x524E4554u, 42u, 0u, 5u,
                                     RNET_OBS_REFUSE_ROWS_GONE, 300u, 200u);
    if (n <= 0 || rnet_proto_decode(buf, (size_t)n, 0x524E4554u, &p) != 0 ||
        p.type != RNET_PKT_OBS_REFUSE || p.obs_target_slot != 5u ||
        p.obs_reason != RNET_OBS_REFUSE_ROWS_GONE || p.obs_tick != 300u || p.obs_oldest != 200u)
        fail("codec: OBS_REFUSE");
}

int main(void)
{
    const unsigned base = 43200u + (test_pid() % 700u) * 9u;
    const rnet_u32 sid = 0x4f424331u ^ test_pid();
    test_codec();
    test_roster_gate(base, sid);
    test_join_after_start(base + 3u, sid + 1u);
    test_join_too_late(base + 6u, sid + 2u);
    if (g_failures == 0)
    {
        printf("session_observer_catchup_test: ok\n");
        return 0;
    }
    fprintf(stderr, "session_observer_catchup_test: %d failure(s)\n", g_failures);
    return 1;
}
