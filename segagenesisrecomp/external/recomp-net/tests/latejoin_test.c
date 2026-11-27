/*
 * Join in progress (docs/DROPIN.md section C), session level.
 *
 * Real UDP over loopback, fake clock, rollback-style stepping (no delay-sync
 * admission: every session just produces its tip row for sim+D and advances
 * one tick per 16 ms; what is under test is the table, the log and the
 * claim, not admission). A 4-seat room: seats 0..2 are live, seat 3 is OPEN
 * at launch (cfg.open_mask), the authority runs the drop policy.
 *
 *   1. open seats: every peer seeds ABSENT-from-wire-0 for seat 3, so the
 *      room runs on without it (zero flagged rows, no tip to wait for);
 *   2. ADD + barrier: the authority schedules seat 3 present from a wire
 *      ahead of its tip; while a seat has not acked, the barrier holds; with
 *      nobody behind seat 3 it times out and the seat goes back to CPU;
 *   3. a spectator binding at tick ~900 replays from tick 0 out of the
 *      authority's and the seats' full logs: every row it consumes equals the
 *      row the authority holds, over the whole match;
 *   4. the spectator claims seat 3: SEAT_GRANT promotes its session, the hub
 *      identity moves, every present seat acks the ADD, the barrier clears;
 *      from wire A its rows are real on every peer, below A the flagged zero
 *      row (before and after a second claimant is refused).
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

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#define MAGIC 0x524E4554u
#define DROP_MS 1500u
#define D 3u

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
    rnet_u8 id = *(rnet_u8 *)opaque;
    memset(out, 0, sizeof(*out));
    out->size = 4;
    out->bytes[0] = id;
    out->bytes[1] = (rnet_u8)tick;
    out->bytes[2] = (rnet_u8)(tick >> 8);
    out->bytes[3] = (rnet_u8)(0x5au ^ id);
    out->valid = 1;
}
static void publish(rnet_u32 tick, const RNetInputSample *by_slot, int slots, void *opaque)
{
    (void)tick; (void)by_slot; (void)slots; (void)opaque;
}
static rnet_u64 now_ms(void *opaque) { (void)opaque; return g_now_ms; }

#define N 6 /* 0..2 seats, 3 unused, 4 and 5 = spectators (the second joins after the first left) */
typedef struct Room
{
    RNetSession *s[N];
    rnet_u8 id[N];
    int alive[N];
    unsigned port;
    rnet_u32 sim[N];
    /* The spectator's lockstep consumption, for the comparison. */
    rnet_u32 obs_sim_[N];
    rnet_u32 obs_rows_ok, obs_rows_bad;
} Room;

static RNetConfig base_cfg(rnet_u32 session)
{
    RNetConfig c;
    rnet_config_init_defaults(&c);
    c.slot_count = 4;
    c.input_delay = D;
    c.session_id = session;
    c.open_mask = 1u << 3;
    c.late_join = 1;
    return c;
}

static RNetSession *mk(Room *r, int k, rnet_u32 session, int observer)
{
    RNetConfig c = base_cfg(session);
    RNetHostVTable v;
    char bind[64], hub[64];
    c.local_slot = (rnet_u8)k;
    if (observer) { c.local_slot = 4; c.wire_slot = (rnet_u8)k; }
    if (k == 3) c.open_mask = 0; /* the seat that will be added is not seeded absent by itself: it learns the table */
    memset(&v, 0, sizeof v);
    v.sample_local = sample_local; v.publish = publish; v.now_ms = now_ms;
    r->id[k] = (rnet_u8)(10 + k);
    v.ctx = &r->id[k];
    r->s[k] = rnet_session_create(&c, &v);
    if (!r->s[k]) return NULL;
    rnet_session_set_seat_loss_policy(r->s[k], DROP_MS);
    snprintf(bind, sizeof bind, "127.0.0.1:%u", r->port + (unsigned)k);
    snprintf(hub, sizeof hub, "127.0.0.1:%u", r->port);
    r->alive[k] = 1;
    return (k == 0) ? (rnet_session_start_lan_hub(r->s[0], bind) == 0 ? r->s[k] : NULL)
                    : (rnet_session_start_lan(r->s[k], bind, hub) == 0 ? r->s[k] : NULL);
}

/* One fake millisecond: every live session pumps; every 16th ms a seat
 * produces its tip row and advances. The spectator consumes in lockstep:
 * a tick only once every seat's row for it is available (as the driver's
 * observer does), and compares it with the authority's view. */
static void step(Room *r, int ms)
{
    int i, k;
    for (i = 0; i < ms; ++i)
    {
        g_now_ms++;
        for (k = 0; k < N; ++k)
        {
            RNetSession *s = r->s[k];
            if (!s || !r->alive[k]) continue;
            rnet_session_pump(s);
            if (!rnet_session_is_running(s)) continue;
            if (rnet_session_is_observer(s))
            {
                /* lockstep consumption, as fast as the rows arrive */
                for (;;)
                {
                    int slot, ok = 1;
                    RNetInputSample row[4];
                    for (slot = 0; slot < 4 && ok; ++slot)
                        ok = rnet_session_peek_remote_input(s, slot, r->obs_sim_[k], &row[slot]) && row[slot].valid;
                    if (!ok) break;
                    /* The truth is a function of the wire tick: seats 0..2 sample
                     * (id, sim, sim>>8, ..) at sim = wire - D (the neutral prefix
                     * before it has size 0); seat 3 is absent in this room. */
                    for (slot = 0; slot < 4; ++slot)
                    {
                        rnet_u32 w = r->obs_sim_[k];
                        int same;
                        if (slot == 3)
                            same = row[slot].absent && row[slot].size == 0;
                        else if (w < D)
                            same = !row[slot].absent && row[slot].size == 0;
                        else
                            same = !row[slot].absent && row[slot].size == 4 &&
                                   row[slot].bytes[0] == (rnet_u8)(10 + slot) &&
                                   row[slot].bytes[1] == (rnet_u8)(w - D) &&
                                   row[slot].bytes[2] == (rnet_u8)((w - D) >> 8) &&
                                   row[slot].bytes[3] == (rnet_u8)(0x5au ^ (10 + slot));
                        if (same) r->obs_rows_ok++; else r->obs_rows_bad++;
                    }
                    r->obs_sim_[k]++;
                    rnet_session_set_sim_tick(s, r->obs_sim_[k]);
                }
                continue;
            }
            if ((g_now_ms % 16u) == 0u)
            {
                rnet_u32 t = rnet_session_sim_tick(s);
                (void)rnet_session_prepare_local_tip(s, t);
                rnet_session_set_sim_tick(s, t + 1u);
            }
        }
    }
}

static void room_close(Room *r)
{
    int k;
    for (k = 0; k < N; ++k) if (r->s[k]) rnet_session_destroy(r->s[k]);
    memset(r, 0, sizeof *r);
}

static int room_open(Room *r, unsigned port, rnet_u32 session, int with_seat3)
{
    int k, i;
    memset(r, 0, sizeof *r);
    r->port = port;
    for (k = 0; k < 3; ++k) if (!mk(r, k, session, 0)) return 0;
    if (with_seat3 && !mk(r, 3, session, 0)) return 0;
    for (i = 0; i < 20000; ++i)
    {
        step(r, 1);
        if (rnet_session_is_running(r->s[0]) && rnet_session_is_running(r->s[1]) &&
            rnet_session_is_running(r->s[2]) && (!with_seat3 || rnet_session_is_running(r->s[3])))
            return 1;
    }
    return 0;
}

#ifndef _WIN32
/* A claim sent by a raw UDP socket that is no session (a hostile spectator). */
static int forge_open(void) { return (int)socket(AF_INET, SOCK_DGRAM, 0); }
static void forge_claim(int fd, unsigned hub_port, rnet_u32 session, rnet_u8 wire_id, rnet_u32 obs_tick)
{
    rnet_u8 buf[128];
    struct sockaddr_in a;
    int n = rnet_proto_encode_seat_claim(buf, sizeof buf, MAGIC, session, wire_id, 0xffu, obs_tick);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)hub_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (n > 0) (void)sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&a, sizeof a);
}
#endif

int main(void)
{
    Room r;
    RNetInputSample smp;
    rnet_u32 tip = 0, w = 0, A = 0, added = 0, aborted = 0;
    int slot = -1, present = -1, i, st;
    unsigned port = 44500u + (test_pid() % 400u) * 8u;

    /* ---- room 1: open seat + ADD that nobody takes (timeout) ---- */
    if (!room_open(&r, port, 0x0ee00001u ^ test_pid(), 0)) { fail("room 1 did not start"); goto done; }
    step(&r, 800);
    EXPECT(rnet_session_occ_gen(r.s[0]) == 1 && rnet_session_occ_gen(r.s[1]) == 1 &&
               rnet_session_occ_gen(r.s[2]) == 1,
           "every peer seeded the open seat (gen 1) with nothing on the wire");
    EXPECT(rnet_session_occ_event(r.s[1], 0, &w, &slot, &present) && w == 0 && slot == 3 && !present,
           "event 1 = seat 3 ABSENT from wire 0");
    EXPECT(rnet_session_absent_mask(r.s[2]) == (1u << 3), "absent mask has the open seat");
    EXPECT(rnet_session_peek_remote_input(r.s[1], 3, 5, &smp) == 1 && smp.valid && smp.absent && smp.size == 0,
           "the open seat's row is the flagged zero row");
    EXPECT(rnet_session_remote_tip(r.s[1], 3, &tip) == 0, "no tip to wait for on an open seat");
    EXPECT(rnet_session_sim_tick(r.s[1]) > 40, "the room runs without the open seat");
    EXPECT(rnet_session_declare_seat_add(r.s[1], 3, -1, &w) == 0, "only the authority declares an ADD");
    EXPECT(rnet_session_declare_seat_add(r.s[0], 3, -1, &A) == 1 && A > rnet_session_sim_tick(r.s[0]) + 64u,
           "authority declares the ADD a safe distance ahead of its tip");
    EXPECT(rnet_session_declare_seat_add(r.s[0], 3, -1, &w) == 0, "one ADD at a time");
    EXPECT(rnet_session_occ_barrier(r.s[0], &w) == 1 && w == A, "the barrier holds");
    EXPECT(rnet_session_remote_tip(r.s[1], 3, &tip) == 0 || 1, "(tip query is safe)");
    step(&r, 200);
    EXPECT(rnet_session_occ_gen(r.s[1]) == 2 && rnet_session_occ_gen(r.s[2]) == 2, "guests hold the ADD");
    EXPECT(rnet_session_remote_tip(r.s[1], 3, &tip) == 0, "before A nothing waits for the pending seat");
    EXPECT(rnet_session_absent_mask(r.s[1]) == 0u, "latest table: seat 3 is present (scheduled)");
    EXPECT(rnet_session_absent_mask_at(r.s[1], 5) == (1u << 3), "...but absent at a wire below A");
    EXPECT(rnet_session_occ_barrier(r.s[0], &w) == 1, "still held: seat 3 never acks");
    step(&r, 6500);
    EXPECT(rnet_session_occ_barrier(r.s[0], &w) == 0, "barrier released by the timeout");
    rnet_session_late_join_counters(r.s[0], &added, &aborted);
    EXPECT(added == 0 && aborted == 1, "the unconfirmed join was cancelled");
    EXPECT(rnet_session_absent_mask(r.s[0]) == (1u << 3) && rnet_session_absent_mask(r.s[2]) == (1u << 3),
           "seat 3 is CPU again for everybody");
    EXPECT(rnet_session_peek_remote_input(r.s[2], 3, A + 5, &smp) == 1 && smp.absent, "its rows stay flagged");
    room_close(&r);

    /* ---- room 2: late spectator, catch-up from the logs, claim, promotion ---- */
    port += 8;
    if (!room_open(&r, port, 0x0ee00002u ^ test_pid(), 0)) { fail("room 2 did not start"); goto done; }
    step(&r, 15000); /* ~900 ticks */
    EXPECT(rnet_session_sim_tick(r.s[0]) > 600, "the match is past tick 600");
    if (!mk(&r, 4, 0x0ee00002u ^ test_pid(), 1)) { fail("spectator did not start"); goto done; }
    for (i = 0; i < 40 && r.obs_sim_[4] < 600; ++i) step(&r, 250);
    EXPECT(r.obs_sim_[4] >= 600, "the spectator replayed past tick 600 from the logs");
    EXPECT(r.obs_rows_ok > 2400 && r.obs_rows_bad == 0, "every replayed row equals the authority's");
    printf("latejoin_test: spectator replayed %u ticks (rows ok %u, bad %u), authority at %u\n",
           (unsigned)r.obs_sim_[4], (unsigned)r.obs_rows_ok, (unsigned)r.obs_rows_bad,
           (unsigned)rnet_session_sim_tick(r.s[0]));
    /* catch up to the live edge, then claim */
    for (i = 0; i < 40 && r.obs_sim_[4] + 30u < rnet_session_sim_tick(r.s[0]); ++i) step(&r, 100);
    EXPECT(r.obs_sim_[4] + 60u >= rnet_session_sim_tick(r.s[0]), "the spectator is within reach of the live edge");
    EXPECT(rnet_session_request_seat(r.s[4], 3) == 1, "claim sent");
    for (i = 0; i < 60 && rnet_session_is_observer(r.s[4]); ++i) step(&r, 50);
    EXPECT(!rnet_session_is_observer(r.s[4]), "the grant promoted the spectator to a seat");
    st = rnet_session_claim_status(r.s[4], &slot, &A);
    EXPECT(st == 2 && slot == 3, "claim status: granted seat 3");
    if (rnet_session_is_observer(r.s[4])) goto done;
    /* from here it is a seat: it produces rows like the others */
    r.id[4] = 99;
    for (i = 0; i < 30 && rnet_session_occ_barrier(r.s[0], &w); ++i) step(&r, 100);
    EXPECT(rnet_session_occ_barrier(r.s[0], &w) == 0, "barrier cleared with the promoted joiner's ack");
    rnet_session_late_join_counters(r.s[0], &added, &aborted);
    EXPECT(added == 1 && aborted == 0, "join agreed (authority)");
    printf("latejoin_test: seat %d from wire %u\n", slot, (unsigned)A);
    /* The joiner's tick counter: it continues from where its replay stopped. */
    rnet_session_set_sim_tick(r.s[4], r.obs_sim_[4]);
    r.sim[4] = r.obs_sim_[4];
    for (i = 0; i < 400 && !(rnet_session_remote_tip(r.s[1], 3, &tip) && tip >= A + 8u); ++i) step(&r, 20);
    EXPECT(rnet_session_remote_tip(r.s[1], 3, &tip) && tip >= A + 8u, "the joiner's rows reached wire A + 8");
    EXPECT(rnet_session_peek_remote_input(r.s[1], 3, A - 1, &smp) == 1 && smp.absent, "below A: flagged zero row");
    EXPECT(rnet_session_peek_remote_input(r.s[1], 3, A + 6, &smp) == 1 && smp.valid && !smp.absent &&
               smp.bytes[0] == 99 && smp.bytes[1] == (rnet_u8)(A + 6 - D),
           "from A the joiner's own rows reach the seats under its seat id (hub identity re-bound)");
    EXPECT(rnet_session_occ_gen(r.s[2]) == 2 && rnet_session_occ_gen(r.s[4]) == 2, "everyone holds the same 2 events");

    /* The joiner leaves (BYE): its seat is dropped; a SECOND spectator then
     * claims the same seat. The hub still holds the first occupant's pinned
     * address / agent for that seat -- the grant must replace it, or the
     * second joiner's packets (naming the seat) would be dropped as spoofed. */
    (void)rnet_session_send_bye(r.s[4]);
    r.alive[4] = 0;
    for (i = 0; i < 30; ++i) step(&r, 50);
    EXPECT(rnet_session_occ_gen(r.s[0]) == 3 && rnet_session_absent_mask(r.s[0]) == (1u << 3),
           "the leaving joiner's seat is dropped (gen 3)");
    EXPECT(mk(&r, 5, 0x0ee00002u ^ test_pid(), 1) != NULL, "second spectator started");
    {
        rnet_u32 A2 = 0;
        int slot2 = -1;
        for (i = 0; i < 60 && r.obs_sim_[5] + 30u < rnet_session_sim_tick(r.s[0]); ++i) step(&r, 250);
        EXPECT(rnet_session_request_seat(r.s[5], 3) == 1, "second claim sent");
        for (i = 0; i < 80 && rnet_session_is_observer(r.s[5]); ++i) step(&r, 50);
        EXPECT(!rnet_session_is_observer(r.s[5]), "the second spectator was promoted");
        EXPECT(rnet_session_claim_status(r.s[5], &slot2, &A2) == 2 && slot2 == 3, "second grant: seat 3 again");
        r.id[5] = 98;
        rnet_session_set_sim_tick(r.s[5], r.obs_sim_[5]);
        for (i = 0; i < 400 && !(rnet_session_remote_tip(r.s[1], 3, &tip) && tip >= A2 + 8u); ++i) step(&r, 20);
        EXPECT(rnet_session_remote_tip(r.s[1], 3, &tip) && tip >= A2 + 8u,
               "the second joiner's rows reach the seats (stale hub identity replaced)");
        EXPECT(rnet_session_peek_remote_input(r.s[1], 3, A2 + 6, &smp) == 1 && smp.valid && !smp.absent &&
                   smp.bytes[0] == 98, "its rows are real from its wire");
        EXPECT(rnet_session_occ_gen(r.s[2]) == 4, "everyone holds 4 events (seed, add, drop, add)");
    }
    room_close(&r);

#ifndef _WIN32
    /* ---- room 3: claim abuse ---- */
    port += 8;
    {
        rnet_u32 sess = 0x0ee00003u ^ test_pid();
        int fd, pending_seen = 0, refused = 0, attempt;
        unsigned hubp;
        if (!room_open(&r, port, sess, 0)) { fail("room 3 did not start"); goto done; }
        hubp = port;
        step(&r, 800);
        EXPECT(rnet_session_occ_gen(r.s[0]) == 1, "room 3: only the seeded open seat");
        fd = forge_open();
        forge_claim(fd, hubp, sess, 6, 0x7fffffffu);
        step(&r, 150);
        EXPECT(rnet_session_occ_gen(r.s[0]) == 1 && rnet_session_occ_barrier(r.s[0], &w) == 0,
               "a claim whose replay position is far ahead of the authority's tip is refused");
        for (attempt = 0; attempt < 40; ++attempt)
        {
            forge_claim(fd, hubp, sess, 6, rnet_session_sim_tick(r.s[0]));
            step(&r, 100);
            if (rnet_session_occ_barrier(r.s[0], &w))
            {
                pending_seen++;
                close(fd); /* the hub moved this address to the seat: the next claim is a new peer */
                fd = forge_open();
                step(&r, 6500); /* never confirmed: cancelled */
                EXPECT(rnet_session_occ_barrier(r.s[0], &w) == 0, "the unconfirmed join was cancelled");
                forge_claim(fd, hubp, sess, 6, rnet_session_sim_tick(r.s[0]));
                step(&r, 100);
                if (pending_seen == 1)
                    EXPECT(rnet_session_occ_barrier(r.s[0], &w) == 0,
                           "right after a cancelled join the next claim is answered BUSY");
            }
            else { refused++; step(&r, 500); }
        }
        close(fd);
        rnet_session_late_join_counters(r.s[0], &added, &aborted);
        printf("latejoin_test: abusive claims: %d joins taken (aborted %u), %d refused, table %u events\n",
               pending_seen, (unsigned)aborted, refused, (unsigned)rnet_session_occ_gen(r.s[0]));
        EXPECT(pending_seen == 8 && aborted == 8, "at most 8 abandoned joins are taken");
        EXPECT(rnet_session_occ_gen(r.s[0]) == 1u + 2u * 8u, "2 table entries per abandoned join");
        EXPECT(rnet_session_declare_seat_drop(r.s[0], 1, "test") == 1 &&
                   rnet_session_absent_mask(r.s[0]) == ((1u << 1) | (1u << 3)),
               "a seat can still be dropped afterwards (the table keeps room for drops)");
        room_close(&r);
    }
#endif

done:
    if (g_failures == 0) { printf("latejoin_test: ok\n"); return 0; }
    fprintf(stderr, "latejoin_test: %d failure(s)\n", g_failures);
    return 1;
}
