#include "recomp_net/recomp_net.h"
#include "recomp_net/startup_profiles.h"

#include "ice/rnet_ice_internal.h"
#include "input/rnet_rings.h"
#include "platform/rnet_platform.h"
#include "protocol/rnet_protocol.h"
#include "transport/rnet_transport.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

const char *rnet_version_string(void)
{
    return "0.1.0";
}

rnet_u32 rnet_checksum(const void *data, size_t len)
{
    return rnet_proto_checksum((const rnet_u8 *)data, len);
}

typedef enum RNetSessionPhase
{
    RNET_PHASE_IDLE = 0,
    RNET_PHASE_LINKING,
    RNET_PHASE_READY,
    RNET_PHASE_RUNNING
} RNetSessionPhase;

/* Outbound STATE transfer. Chunks go out once and reach every receiver (the
 * relay / hub fans them out), but completion, the send window and
 * retransmission are all per receiver: the transfer is done only when every
 * seat in expect_mask has ACKed the whole blob, the window is anchored on the
 * SLOWEST receiver, and a receiver whose ACK stops advancing rewinds the
 * cursor to its own watermark. With one receiver this is exactly the former
 * single-peer sender. */
typedef struct RNetStateTx
{
    int active;
    int ready;
    rnet_u8 op;
    rnet_u8 slot;
    rnet_u32 xfer_id;
    rnet_u32 total;
    rnet_u32 crc;
    rnet_u8 *buf;
    rnet_u32 expect_mask;                  /* receivers the transfer waits on */
    rnet_u32 ack[RNET_MAX_SLOTS];          /* contiguous ACK per receiver */
    rnet_u64 ack_timer_ms[RNET_MAX_SLOTS]; /* last progress/timeout; 0 = none */
    rnet_u32 send_cursor;
    rnet_u64 last_tx_ms;
    rnet_u64 last_begin_ms;
    rnet_u64 last_progress_log_ms;
    rnet_u32 last_progress_acked;
    rnet_u64 start_ms;
    /* AIMD pacing for STATE_CHUNK (esp. ICE/TURN — juice drops on flood). */
    rnet_u32 cwnd;       /* in-flight byte budget */
    rnet_u32 chunks_cap; /* max chunks emitted per pump */
    rnet_u32 ack_timeout_ms;
} RNetStateTx;

/* Inbound STATE transfer from one source seat. */
typedef struct RNetStateRx
{
    int active;
    int ready;
    rnet_u8 op;
    rnet_u8 slot;
    rnet_u32 xfer_id;
    rnet_u32 total;
    rnet_u32 crc;
    rnet_u32 contiguity; /* bytes from 0 received */
    rnet_u8 *buf;
    rnet_u64 last_ack_ms;
    rnet_u64 start_ms;
    rnet_u8 bits[(RNET_STATE_MAX_CHUNKS + 7u) / 8u];
} RNetStateRx;

#define RNET_STATE_TAKEN_TX RNET_MAX_SLOTS

/* Growable per-tick input log (see RNetSession.row_log). Rows are 4 bytes; a
 * bit per tick says whether the row is held (the authority stores seats' rows
 * as they arrive, holes and all). */
#define RNET_LOG_ROW_BYTES 4u
#define RNET_LOG_MAX_TICKS (1u << 24)
typedef struct RNetRowLog
{
    rnet_u8 *rows;
    rnet_u8 *sizes; /* row size per tick (the neutral prefix has size 0) */
    rnet_u8 *have;
    rnet_u32 cap; /* ticks the buffers cover */
    rnet_u32 hi;  /* one past the highest tick stored */
} RNetRowLog;


typedef struct RNetIceHubBridge
{
    struct RNetSession *s;
    int slot;
} RNetIceHubBridge;

struct RNetSession
{
    /* Fixed startup agreement. Reserved RB_SYNC ops are consumed here, never
     * by the rollback driver's episode queue. Kept across rollback/resync. */
    struct {
        int active, refused;
        rnet_u32 qualification, occupied, offers, acks, acknowledged_by;
        rnet_u32 selected, transcript;
        RNetProfileCaps caps[RNET_MAX_SLOTS];
        rnet_u64 last_tx;
        int dirty;
    } profiles;
    RNetConfig cfg;
    RNetHostVTable host;
    RNetTransport transport;
    RNetIceAgent *ice;
    /* ICE hub (host-as-relay): one agent per guest seat in ice_hub_mask. */
    RNetIceAgent *ice_hub[RNET_HUB_MAX_PEERS];
    RNetIceHubBridge ice_hub_bridge[RNET_HUB_MAX_PEERS];
    rnet_u32 ice_hub_mask;
    RNetSessionPhase phase;
    RNetInputRing local_ring;
    RNetInputRing remote_rings[RNET_MAX_SLOTS];
    rnet_u8 peer_ready[RNET_MAX_SLOTS];
    rnet_u8 local_ready;
    rnet_u8 start_sent;
    rnet_u8 delay;
    /* Mid-session DELAY_SYNC: apply when sim_tick reaches effective_tick. */
    int delay_pending;
    rnet_u8 delay_pending_value;
    rnet_u32 delay_pending_effective;
    rnet_u64 delay_pending_last_tx_ms;
    rnet_u32 sim_tick;
    rnet_u32 highest_remote_ack;
    /* Multi-seat INPUT carries one contiguous acknowledgment per source.
     * UINT32_MAX means the receiver has not yet seen tick zero. */
    rnet_u32 remote_contiguous_ack[RNET_MAX_SLOTS];
    rnet_u32 peer_ack_tick[RNET_MAX_SLOTS];
    rnet_u8 peer_ack_seen[RNET_MAX_SLOTS];
    rnet_u64 last_hello_ms;
    rnet_u64 last_ready_ms;
    rnet_u64 last_input_ms;
    rnet_u32 last_input_tip;
    int last_input_tip_valid;
    int is_sim_authority; /* local_slot == 0 sends START */
    /* 1 when this session owns no seat: it simulates every slot from the
     * wire, contributes no input, and never becomes sim authority. */
    int is_observer;
    /* Sender id on the wire. Equal to cfg.local_slot for a seated peer; for
     * an observer it is a slot in the RELAY's namespace, which is what makes
     * the relay recognise it as a spectator and refuse to forward it. Never
     * used to index a per-seat array -- see the guards below. */
    rnet_u8 wire_slot;
    /* Resolved hashes and peer confirmations are prepared up
     * to D ticks ahead, so strict agreement normally completes before admit. */
    rnet_u32 published_tick[RNET_HISTORY_LENGTH];
    rnet_u32 published_hash[RNET_HISTORY_LENGTH];
    rnet_u8 published_valid[RNET_HISTORY_LENGTH];
    rnet_u32 peer_history_tick[RNET_HISTORY_LENGTH][RNET_MAX_SLOTS];
    rnet_u32 peer_history_hash[RNET_HISTORY_LENGTH][RNET_MAX_SLOTS];
    rnet_u8 peer_history_valid[RNET_HISTORY_LENGTH][RNET_MAX_SLOTS];
    rnet_u64 confirm_last_sent_ms[RNET_HISTORY_LENGTH];
    int input_desync;
    rnet_u32 desync_tick;
    rnet_u32 desync_local_hash;
    rnet_u32 desync_remote_hash;
    /* Peer liveness. Aggregate: any valid packet stamps last_peer_rx_ms and a
     * BYE from anyone sets peer_gone -- the two-seat view, kept as it was.
     * Per seat: the same two facts indexed by the packet's sender slot, so a
     * seat that went silent in a room of four is visible instead of hiding
     * behind the others' traffic. Indexed by wire slot; seats are < 8. */
    rnet_u64 last_peer_rx_ms;
    rnet_u64 session_start_ms;
    int peer_gone;
    rnet_u64 peer_rx_ms[RNET_MAX_SLOTS]; /* 0 = never heard from */
    rnet_u32 peer_gone_mask;             /* bit i = seat i sent BYE */
    /* Occupancy schedule (wire v6): the authority's append-only table of
     * "seat S present/absent from wire W" events, gen = index + 1. See
     * RNET_PKT_OCC_EVENT. seat_loss_timeout_ms != 0 turns the drop policy on:
     * a seat's BYE or silence then DROPS the seat (the game plays it as CPU)
     * instead of ending the match for everyone. */
    RNetOccWire occ[RNET_OCC_MAX];
    rnet_u8 occ_n;
    rnet_u16 occ_peer_ack[RNET_MAX_SLOTS]; /* authority: highest gen each seat acked */
    rnet_u64 occ_last_tx_ms;
    rnet_u32 seat_loss_timeout_ms;
    int dropped_self;                     /* the table says WE are absent */
    /* Authority: for a while after a drop, re-serve the dropped seat's last
     * rows (it can no longer retransmit them to a peer that lost one). */
    rnet_u64 reserve_until_ms[RNET_MAX_SLOTS];
    rnet_u64 reserve_last_ms[RNET_MAX_SLOTS];
    rnet_u32 reserve_hi[RNET_MAX_SLOTS];
    /* Chunked blob transfer. One outbound (tx) at a time; inbound (rx) is
     * per SOURCE seat so several guests can upload MEMCARD to the host at
     * once without one BEGIN clobbering another's receive. */
    RNetStateTx tx;
    RNetStateRx rx[RNET_MAX_SLOTS];
    /* Which transfer the last take_ready reported: -1 none, RNET_MAX_SLOTS =
     * tx, else rx[source]. state_finish finishes that one. */
    int state_taken;
    int state_stall_sim; /* probe + all transfers stall admit until finished */
    rnet_u32 state_next_xfer_id;
    /* Incoming transfer history is per sender, independent of our outgoing
     * receipts. Survives state_clear/hard_resync so a reordered BEGIN cannot
     * resurrect an old transfer or displace a newer one. */
    struct {
        rnet_u32 id, total, crc;
        rnet_u8 op, slot;
        int finished;
        rnet_u64 last_reack_ms; /* paces re-ACKs of a finished transfer */
    } state_received[RNET_MAX_SLOTS];
    /* Survives state_clear — warm-start the next ICE transfer in-session. */
    rnet_u32 state_sticky_cwnd;
    rnet_u32 state_sticky_chunks;
    /* Hash probe before transfer (host announce → guest reply). The prober
     * waits for a reply from every seat in expect_mask. */
    int state_probe_active;
    int state_probe_sender;
    rnet_u32 state_probe_expect_mask; /* host: seats that must answer */
    rnet_u32 state_probe_reply_mask;  /* host: seats that answered */
    rnet_u32 state_probe_match_mask;  /* host: seats that answered "match" */
    int state_probe_pending;     /* guest: awaiting app reply */
    int state_probe_match;
    rnet_u8 state_probe_op;
    rnet_u8 state_probe_slot;
    rnet_u32 state_probe_size;
    rnet_u32 state_probe_crc;
    rnet_u64 state_probe_last_tx_ms;
    /* Guest: answered "match" to a SAVE/SRAM/RB_KF hash probe and holds its
     * input until the host's STATE_PROBE_DONE (every seat matched) or a
     * STATE_BEGIN (someone missed). Re-sends the reply while parked. */
    int state_probe_parked;
    /* Host: the last probe generation released with STATE_PROBE_DONE, so a
     * parked guest whose DONE was lost is released again on its retry. */
    int state_probe_released_valid;
    rnet_u8 state_probe_released_op;
    rnet_u8 state_probe_released_slot;
    rnet_u32 state_probe_released_size;
    rnet_u32 state_probe_released_crc;
    /* When set, pump must not emit INPUT bundles. Used across LOAD apply/ready
     * so pre-resync tip rows cannot clobber the post-hard_resync epoch
     * (tick % RNET_HISTORY_LENGTH collisions). Cleared by prime_delay_inputs. */
    int input_send_suppress;
    /* Bumped on hard_resync. INPUT/CONFIRM carry this; other-epoch packets are
     * dropped so in-flight tips from a prior sim_tick=0 era cannot first-wins. */
    rnet_u16 input_epoch;
    /* Diagnostics (JSONL / HUD). */
    RNetAdmitStall last_stall;
    rnet_u32 consecutive_stalls;
    rnet_u32 admit_ok_count;
    rnet_u32 stall_streaks;
    rnet_u64 stall_started_ms;
    rnet_u32 last_admit_wait_ms;
    rnet_u32 max_admit_wait_ms;
    rnet_u32 packets_rx;
    rnet_u32 input_bundle_sends;
    /* §56 pipeline diagnostics: monotonic arrival stamp per remote wire row
     * (first-wins, mirrors remote_rings latching). Consumption slack =
     * now - arrival when the row is finally needed at admit. */
    rnet_u64 remote_arr_ms[RNET_MAX_SLOTS][RNET_HISTORY_LENGTH];
    rnet_u32 remote_arr_tick[RNET_MAX_SLOTS][RNET_HISTORY_LENGTH];
    /* ICE TURN auto-fallback timers (monotonic ms). */
    rnet_u64 ice_attempt_ms;
    rnet_u64 ice_completed_ms;
    /* Peer RB_FRAME_COMMIT queue (host drains via take_*). Sized for more than
     * one peer: every peer sends one per tick, and an INCREMENTAL host does not
     * drain while it replays, so four seats fill 64 entries in about twenty
     * ticks. Each entry keeps its sender -- a hash chain per peer needs it. */
#define RNET_RB_FC_QUEUE 256
    /* PSX-Link group scoping: when >= 0, rollback EPISODE/STATE packets
     * (SYNC, BASELINE, POST, STATE_*) are accepted only from this slot —
     * episode coordination is per-console-group in link sessions. FRAME_COMMIT
     * is NOT filtered (all seats emit the same machine-level pair fold), and
     * input-plane packets (INPUT, INPUT_CONFIRM, SEAL_ROWS, RESOLVED) are
     * session-wide. -1 = accept all (default). */
    int rb_peer_slot;
    rnet_u32 rb_fc_tick[RNET_RB_FC_QUEUE];
    rnet_u32 rb_fc_hash[RNET_RB_FC_QUEUE];
    rnet_u8 rb_fc_from[RNET_RB_FC_QUEUE];
    int rb_fc_q_head; /* next write */
    int rb_fc_q_tail; /* next read */
    int rb_fc_q_count;

    /* Peer RB episode control queues (small FIFOs). 32, not 8: with more than
     * two seats every peer's IDENT, COMMIT, QUIESCE and BEGIN land in the same
     * queue between two drains, and a hub relays them in bursts. A full queue
     * refuses (and logs) rather than overwriting. */
#define RNET_RB_CTRL_QUEUE 32
    struct {
        rnet_u32 epoch_id, mismatch_tick, load_tick, target_tick;
        rnet_u8 corrected_slot, initiator, flags;
        rnet_u8 from; /* sender's wire slot (packet header) */
    } rb_sync_q[RNET_RB_CTRL_QUEUE];
    int rb_sync_head, rb_sync_tail, rb_sync_count;

    struct {
        rnet_u32 epoch_id, mismatch_tick, target_tick, row_begin;
        rnet_u8 slot;
        rnet_u16 row_count;
        RNetRbWireFrame rows[RNET_RB_SEAL_ROWS_CHUNK_MAX];
        rnet_u8 from;
    } rb_seal_q[RNET_RB_CTRL_QUEUE];
    int rb_seal_head, rb_seal_tail, rb_seal_count;

    struct {
        rnet_u32 epoch_id, load_tick, digest_master, digest_a, digest_b, digest_c;
        rnet_u8 from;
    } rb_base_q[RNET_RB_CTRL_QUEUE];
    int rb_base_head, rb_base_tail, rb_base_count;

    struct {
        rnet_u32 epoch_id, target_tick, digest_master, input_digest;
        rnet_u8 match;
        rnet_u8 from;
    } rb_post_q[RNET_RB_CTRL_QUEUE];
    int rb_post_head, rb_post_tail, rb_post_count;
    /* Sender slot of the most recent rb_* take (-1 = none yet). A two-seat
     * host has one peer and never needs it; an N-seat episode has to know
     * WHICH peer a BASELINE / POST / COMMIT answers for, or the first reply
     * would stand in for every peer's. See rnet_session_rb_last_take_from. */
    int rb_last_from;
    /* Episode-control datagrams refused because their queue was full. These
     * used to vanish without a trace; a dropped BEGIN or POST then surfaced
     * seconds later as a watchdog abort that named the wrong cause. */
    rnet_u32 rb_ctrl_dropped;

    /* Mod-set handshake. Latest-only by design: see the header. The ACK is
     * latest-only PER SEAT: with more than two seats every guest answers the
     * host, and a single slot let the second answer overwrite the first --
     * the host then waited out its bound for a guest that had already
     * confirmed, and refused the match. */
    rnet_u8 peer_wire_version_mismatch; /* a HELLO of another wire version (0xFF = 0) */
    char modset_text[RNET_MODSET_TEXT_MAX];
    rnet_u8 modset_pending;
    char modset_ack_reason[RNET_MAX_SLOTS][RNET_MODSET_REASON_MAX];
    rnet_u8 modset_ack_status[RNET_MAX_SLOTS];
    rnet_u32 modset_ack_pending; /* bit i = seat i's answer is waiting */
    /* RESOLVED is a watermark, so the queue holds at most one entry per
     * sender (the highest): with more than two seats the receiver must know
     * WHICH peer has resolved how far, not just that somebody has. */
    struct {
        rnet_u32 through;
        rnet_u8 from;
    } rb_resolved_q[RNET_RB_CTRL_QUEUE];
    int rb_resolved_head, rb_resolved_tail, rb_resolved_count;

    /* Peer GBA Multi SEND barrier (0-delay; not pad INPUT). */
#define RNET_SIO_XFER_QUEUE 32
    struct {
        rnet_u32 seq;
        rnet_u16 send;
        rnet_u16 confirm_pad; /* lo=confirm IRQ, hi=vblank mod 256 */
        rnet_u8 unit_id;
    } sio_xfer_q[RNET_SIO_XFER_QUEUE];
    int sio_xfer_head, sio_xfer_tail, sio_xfer_count;

    /* ---- Spectator catch-up (wire version 3; see rnet_protocol.h) ----
     * Observer side. */
    rnet_u32 obs_seen_tip[RNET_MAX_SLOTS]; /* highest row tick seen from a seat,
                                            * stored or not */
    rnet_u8 obs_seen_valid[RNET_MAX_SLOTS];
    rnet_u32 obs_last_need[RNET_MAX_SLOTS];
    rnet_u64 obs_need_since_ms[RNET_MAX_SLOTS];
    rnet_u64 obs_last_catchup_ms;
    rnet_u64 obs_last_urgent_ms;
    rnet_u16 obs_commit0_mask; /* seats whose tick-0 FRAME_COMMIT arrived */
    rnet_u8 modset_rx_seen;
    int obs_refused;
    rnet_u8 obs_refuse_reason;
    rnet_u8 obs_refuse_from;
    rnet_u32 obs_refuse_tick;
    rnet_u32 obs_refuse_oldest;
    rnet_u32 obs_catchups_sent;
    /* Seat side. */
    rnet_u8 obs_ready_seen[32]; /* authority: bitset of spectator wire slots
                                 * that sent READY (roster gate) */
    rnet_u64 seats_ready_since_ms;
    int roster_wait_logged;
    struct {
        rnet_u8 used;
        rnet_u8 wire;
        rnet_u32 rows_need[RNET_MAX_SLOTS]; /* per source seat (join in progress) */
        rnet_u64 rows_ms[RNET_MAX_SLOTS];
        rnet_u64 refuse_ms;
        rnet_u64 commit0_ms;
        rnet_u64 modset_ms;
        rnet_u64 begin_ms;
        rnet_u32 state_ack;
        rnet_u64 state_ms;
        rnet_u32 budget_mp;  /* serve budget, milli-packets (token bucket) */
        rnet_u64 budget_ms;
    } obs_serve[16];
    rnet_u32 commit0_hash;
    rnet_u8 commit0_valid;
    char modset_sent_text[RNET_MODSET_TEXT_MAX];
    rnet_u8 modset_sent_valid;
    /* The last outbound transfer every seat completed, kept so a spectator
     * that missed it (late, or lost a chunk after the seats were done) can
     * still be served. */
    rnet_u8 *served_buf;
    rnet_u32 served_id;
    rnet_u32 served_total;
    rnet_u32 served_crc;
    rnet_u8 served_op;
    rnet_u8 served_slot;
    rnet_u32 obs_rows_served;
    rnet_u32 obs_chunks_served;

    /* Game-path RTT, measured from the acknowledgments INPUT already
     * carries: the wall time a tip was first sent, against the time a peer's
     * ack first covers it. Biased up by at most one peer tick (the ack rides
     * the peer's next bundle) -- an upper bound, which is the safe side for
     * provisioning D. Not the lobby's figure: that one is a WebSocket round
     * trip through the lobby server, i.e. two WAN legs per direction, and
     * on a LAN pair it reads 10x the real path. */
    rnet_u64 tip_sent_ms[RNET_HISTORY_LENGTH];
    rnet_u32 tip_sent_tick[RNET_HISTORY_LENGTH];
    /* Per remote seat (the hub topology: each guest has its own path to
     * the host, and a guest's path to another guest is two legs). D has to
     * cover the SLOWEST seat, so the aggregate below is the worst one; it
     * used to be one EMA fed by whichever peer acked a tip first, i.e. the
     * fastest seat in a room of 3+. */
    rnet_u32 link_rtt_x16[RNET_MAX_SLOTS];    /* EMA, ms * 16 */
    rnet_u32 link_jitter_x16[RNET_MAX_SLOTS]; /* EMA of |sample - rtt|, ms * 16 */
    rnet_u32 link_rtt_min[RNET_MAX_SLOTS];    /* ms, smallest sample seen */
    rnet_u32 link_samples[RNET_MAX_SLOTS];
    rnet_u32 link_last_ack[RNET_MAX_SLOTS];   /* last tip sampled from that seat */

    /* ---- Join in progress (cfg.late_join; docs/DROPIN.md section C) ----
     * Full input log: every row a seat produced, by wire tick, so a spectator
     * that arrives 10 000 ticks in can replay the match from tick 0 (the
     * 128-row rings cannot serve it). The authority logs every seat's rows as
     * they arrive, a seat its own. Genesis rows are 4 bytes. */
    RNetRowLog row_log[RNET_MAX_SLOTS];
    int row_log_off; /* a sample was not 4 bytes: logging is switched off */
    /* Authority: the ADD being agreed (one at a time). */
    int add_pending;
    rnet_u8 add_slot;
    rnet_u8 add_wire_id; /* the joiner's wire id (>= slot_count), 0xff = none */
    rnet_u16 add_gen;
    rnet_u32 add_wire;
    rnet_u64 add_since_ms;
    rnet_u64 add_grant_ms;
    rnet_u32 adds_done, adds_aborted;
    /* Joiner (observer): the claim in progress. */
    int claim_state; /* 0 idle, 1 asking, 2 granted (now a seat), 3 refused */
    rnet_u8 claim_want;
    rnet_u8 claim_reason;
    rnet_u64 claim_last_ms;
    rnet_u64 claim_retry_ms;
    rnet_u8 grant_slot;
    rnet_u32 grant_wire;
    rnet_u64 occ_req_ms; /* observer: last table request */
    rnet_u32 claimable_mask; /* authority: seats a joiner may be given ... */
    int claimable_set;       /* ... once set (else every open seat) */
    /* Authority: the host's own link is evidently fine until this + 1 s
     * (rnet_session_set_link_ok). */
    rnet_u64 link_ok_ms;
    /* Authority: claims are answered BUSY until this (after a join was
     * abandoned: one stuck joiner cannot make the room stall back to back). */
    rnet_u64 claim_cooldown_until_ms;
};

static rnet_u64 session_now(RNetSession *s)
{
    if (s->host.now_ms != NULL)
    {
        return s->host.now_ms(s->host.ctx);
    }
    return rnet_os_monotonic_ms();
}

const char *rnet_admit_stall_name(RNetAdmitStall reason)
{
    switch (reason) {
    case RNET_ADMIT_OK: return "ok";
    case RNET_ADMIT_NOT_RUNNING: return "not_running";
    case RNET_ADMIT_STATE_XFER: return "state_xfer";
    case RNET_ADMIT_SIM_MISMATCH: return "sim_mismatch";
    case RNET_ADMIT_DESYNC: return "desync";
    case RNET_ADMIT_WAIT_LOCAL_INPUT: return "wait_local_input";
    case RNET_ADMIT_WAIT_REMOTE_INPUT: return "wait_remote_input";
    case RNET_ADMIT_WAIT_CONFIRM: return "wait_confirm";
    default: return "unknown";
    }
}

static void note_admit_stall(RNetSession *s, RNetAdmitStall reason)
{
    rnet_u64 now;
    if (s == NULL)
        return;
    s->last_stall = reason;
    if (s->consecutive_stalls == 0)
        s->stall_streaks++;
    s->consecutive_stalls++;
    now = session_now(s);
    if (s->stall_started_ms == 0)
        s->stall_started_ms = now ? now : 1;
    s->last_admit_wait_ms = (rnet_u32)(now - s->stall_started_ms);
    if (s->last_admit_wait_ms > s->max_admit_wait_ms)
        s->max_admit_wait_ms = s->last_admit_wait_ms;
}

static void note_admit_ok(RNetSession *s)
{
    rnet_u64 now;
    if (s == NULL)
        return;
    now = session_now(s);
    if (s->stall_started_ms != 0) {
        s->last_admit_wait_ms = (rnet_u32)(now - s->stall_started_ms);
        if (s->last_admit_wait_ms > s->max_admit_wait_ms)
            s->max_admit_wait_ms = s->last_admit_wait_ms;
        s->stall_started_ms = 0;
    } else {
        s->last_admit_wait_ms = 0;
    }
    s->last_stall = RNET_ADMIT_OK;
    s->consecutive_stalls = 0;
    s->admit_ok_count++;
}

static void send_raw(RNetSession *s, const rnet_u8 *buf, int len);

/* Additive RB_SYNC startup ops, deliberately separate from episode state.
 * Offers are copied once. Retries do not observe or change host capabilities. */
#define PROFILE_OFFER RNET_RB_SYNC_OP_PROFILE_OFFER
#define PROFILE_ACK RNET_RB_SYNC_OP_PROFILE_ACK

static rnet_u32 profile_fold(rnet_u32 h, rnet_u32 word)
{
    int byte;
    for (byte = 0; byte < 4; ++byte) {
        h = (h ^ (word & 255u)) * 16777619u;
        word >>= 8;
    }
    return h;
}

static void profiles_choose(RNetSession *s)
{
    rnet_u32 common = ~(rnet_u32)0, h = 2166136261u;
    int i;
    if (s->profiles.offers != s->profiles.occupied || s->profiles.acks) return;
    h = profile_fold(h, s->profiles.qualification);
    h = profile_fold(h, s->cfg.slot_count);
    h = profile_fold(h, s->profiles.occupied);
    for (i = 0; i < s->cfg.slot_count; ++i) {
        const RNetProfileCaps *c = &s->profiles.caps[i];
        if (!(s->profiles.occupied & (1u << i))) continue;
        common &= c->profiles;
        h = profile_fold(h, (rnet_u32)i);
        h = profile_fold(h, c->cpu_features);
        h = profile_fold(h, c->backends);
        h = profile_fold(h, c->profiles);
        h = profile_fold(h, c->architecture);
    }
    if (!(common & 1u)) { s->profiles.refused = 1; return; }
    for (i = 31; i > 0 && !(common & (1u << i)); --i) {}
    s->profiles.selected = (rnet_u32)i;
    s->profiles.transcript = profile_fold(h, (rnet_u32)i);
    s->profiles.acks = 1u << s->cfg.local_slot;
    s->profiles.acknowledged_by = s->profiles.acks;
    s->profiles.dirty = 1;
}

static void profiles_receive(RNetSession *s, const RNetDecodedPacket *p)
{
    rnet_u32 bit;
    if (!s->profiles.active || s->profiles.refused ||
        p->local_slot >= s->cfg.slot_count || p->local_slot == s->cfg.local_slot)
        return;
    bit = 1u << p->local_slot;
    if (!(s->profiles.occupied & bit)) return;
    if (p->rb_epoch_id != s->profiles.qualification || p->rb_flags != 0u) {
        s->profiles.refused = 1;
        return;
    }
    if (p->rb_initiator == PROFILE_OFFER) {
        RNetProfileCaps caps;
        caps.cpu_features = p->rb_mismatch_tick;
        caps.backends = p->rb_load_tick;
        caps.profiles = p->rb_target_tick;
        caps.architecture = p->rb_corrected_slot;
        if (!(caps.profiles & 1u) || ((s->profiles.offers & bit) &&
            memcmp(&caps, &s->profiles.caps[p->local_slot], sizeof(caps)) != 0)) {
            s->profiles.refused = 1; /* changed offer: never renegotiate */
            return;
        }
        s->profiles.caps[p->local_slot] = caps;
        s->profiles.offers |= bit;
        profiles_choose(s);
    } else if (s->profiles.offers == s->profiles.occupied) {
        if (p->rb_mismatch_tick != s->profiles.selected ||
            p->rb_load_tick != s->profiles.transcript ||
            p->rb_corrected_slot != 0u ||
            !(p->rb_target_tick & bit) ||
            (p->rb_target_tick & ~s->profiles.occupied)) {
            s->profiles.refused = 1;
            return;
        }
        if (!(s->profiles.acks & bit)) s->profiles.dirty = 1;
        s->profiles.acks |= bit;
        if (p->rb_target_tick & (1u << s->cfg.local_slot))
            s->profiles.acknowledged_by |= bit;
    }
}

static void profiles_drive(RNetSession *s)
{
    rnet_u64 now;
    const RNetProfileCaps *c;
    if (!s->profiles.active || s->profiles.refused || s->phase != RNET_PHASE_RUNNING)
        return;
    if (!s->profiles.dirty && s->profiles.acknowledged_by == s->profiles.occupied)
        return;
    now = session_now(s);
    if (!s->profiles.dirty && s->profiles.last_tx && now - s->profiles.last_tx < 100u)
        return;
    c = &s->profiles.caps[s->cfg.local_slot];
    rnet_session_send_rb_sync(s, s->profiles.qualification, c->cpu_features,
        c->backends, c->profiles, (rnet_u8)c->architecture, PROFILE_OFFER, 0u);
    if (s->profiles.acks)
        rnet_session_send_rb_sync(s, s->profiles.qualification, s->profiles.selected,
            s->profiles.transcript, s->profiles.acks, 0u, PROFILE_ACK, 0u);
    s->profiles.last_tx = now ? now : 1u;
    s->profiles.dirty = 0;
}

int rnet_session_profiles_begin(RNetSession *s, rnet_u32 qualification,
                                const RNetProfileCaps *caps)
{
    int i;
    if (!s || !caps || !qualification || !(caps->profiles & 1u) ||
        caps->architecture > 255u || s->is_observer || s->profiles.active ||
        !rnet_config_slot_occupied(&s->cfg, s->cfg.local_slot) ||
        s->phase != RNET_PHASE_IDLE || s->sim_tick != 0u) return -1;
    for (i = 0; i < s->cfg.slot_count; ++i)
        if (rnet_config_slot_occupied(&s->cfg, (rnet_u8)i))
            s->profiles.occupied |= 1u << i;
    s->profiles.active = 1;
    s->profiles.qualification = qualification;
    s->profiles.caps[s->cfg.local_slot] = *caps;
    s->profiles.offers = 1u << s->cfg.local_slot;
    s->profiles.dirty = 1;
    profiles_choose(s);
    return 0;
}

int rnet_session_profiles_status(const RNetSession *s, rnet_u32 *profile,
                                 rnet_u32 *transcript)
{
    if (!s || !s->profiles.active || s->profiles.refused) return -1;
    if (s->profiles.acks != s->profiles.occupied) return 0;
    if (profile) *profile = s->profiles.selected;
    if (transcript) *transcript = s->profiles.transcript;
    return 1;
}

int rnet_session_profiles_peer(const RNetSession *s, int seat, RNetProfileCaps *out)
{
    if (!s || !out || seat < 0 || seat >= s->cfg.slot_count ||
        !(s->profiles.offers & (1u << seat))) return 0;
    *out = s->profiles.caps[seat];
    return 1;
}

/* An episode-control queue was full, so this datagram is refused. Said out
 * loud: the first one and every 64th after, so a storm cannot flood the log
 * but a single loss is never invisible. */
static void rb_ctrl_note_drop(RNetSession *s, const char *what)
{
    s->rb_ctrl_dropped++;
    if (s->rb_ctrl_dropped == 1u || (s->rb_ctrl_dropped % 64u) == 0u)
    {
        fprintf(stderr,
                "recomp_net: %s dropped -- episode-control queue full "
                "(%d entries, %u dropped this session); the host is not "
                "draining rb_* messages between pumps\n",
                what, RNET_RB_CTRL_QUEUE, (unsigned)s->rb_ctrl_dropped);
    }
}
/* Episode-control datagrams are retransmitted until answered, and a host
 * that is busy in an inline replay drains them only at present edges. With
 * three peers each resending POST / SEAL_ROWS every pump, a 32-entry FIFO
 * filled with copies of the same few messages and then refused the next new
 * one (1,344 drops on a 4-seat WAN race). A retransmit of a message already
 * waiting is therefore coalesced into it, and a full queue first sheds its
 * oldest entry when that entry belongs to an older episode than the incoming
 * one (the host would discard it on take anyway). */
static int rb_epoch_before(rnet_u32 a, rnet_u32 b)
{
    return (rnet_s32)(a - b) < 0;
}
#define RB_Q_AT(tail, i) (((tail) + (i)) % RNET_RB_CTRL_QUEUE)

static void send_input_bundle(RNetSession *s);
static void apply_pending_delay(RNetSession *s);
static void emit_delay_sync(RNetSession *s, rnet_u8 new_delay, rnet_u32 effective_tick);
static void seed_delay_prefix(RNetSession *s);
static void state_clear(RNetSession *s);
static void state_probe_clear(RNetSession *s);
static int state_any_active(const RNetSession *s);
static void state_drive_sender(RNetSession *s);
static void state_drive_probe(RNetSession *s);
static int emit_local_rows(RNetSession *s, rnet_u32 lo, rnet_u32 hi);
static void obs_on_ready(RNetSession *s, rnet_u8 wire);
static int obs_roster_ready(RNetSession *s, rnet_u64 now);
static void obs_on_catchup(RNetSession *s, const RNetDecodedPacket *pkt);
static void obs_on_state_ack(RNetSession *s, const RNetDecodedPacket *pkt);
static void obs_on_refuse(RNetSession *s, const RNetDecodedPacket *pkt);
static void obs_pump(RNetSession *s);
static void link_rtt_note_ack(RNetSession *s, rnet_u8 slot, rnet_u32 ack);
static int obs_wire_is_spectator(const RNetSession *s, rnet_u8 wire);
static void rlog_put(RNetSession *s, int slot, rnet_u32 tick, const rnet_u8 *bytes, rnet_u16 size);
static void contig_advance(RNetSession *s, int slot);
static void state_on_begin(RNetSession *s, const RNetDecodedPacket *pkt);
static void state_on_chunk(RNetSession *s, const RNetDecodedPacket *pkt);
static void state_on_ack(RNetSession *s, const RNetDecodedPacket *pkt);
static void state_on_probe(RNetSession *s, const RNetDecodedPacket *pkt);
static void state_on_probe_reply(RNetSession *s, const RNetDecodedPacket *pkt);
static void state_on_probe_done(RNetSession *s, const RNetDecodedPacket *pkt);
static void state_send_probe_done(RNetSession *s);

#if defined(RNET_ENABLE_ICE)
static void ice_emit_bridge(const RNetSignal *msg, void *user)
{
    RNetSession *s = (RNetSession *)user;
    if ((s != NULL) && (s->host.on_signal != NULL) && (msg != NULL))
    {
        s->host.on_signal(msg, s->host.ctx);
    }
}

static void ice_hub_emit_bridge(const RNetSignal *msg, void *user)
{
    RNetIceHubBridge *b = (RNetIceHubBridge *)user;
    if ((b != NULL) && (b->s != NULL) && (b->s->host.on_signal != NULL) && (msg != NULL))
    {
        RNetSignal out = *msg;
        out.peer_slot = (rnet_u8)b->slot;
        b->s->host.on_signal(&out, b->s->host.ctx);
    }
}

static int ice_send_bridge(void *ice_ctx, const rnet_u8 *buf, size_t len)
{
    return rnet_ice_agent_send((RNetIceAgent *)ice_ctx, buf, len);
}

static int ice_recv_bridge(void *ice_ctx, rnet_u8 *buf, size_t cap, size_t *out_len)
{
    return rnet_ice_agent_recv((RNetIceAgent *)ice_ctx, buf, cap, out_len);
}
#endif /* RNET_ENABLE_ICE */

static int remote_tick_in_live_window(const RNetSession *s, rnet_u8 slot, rnet_u32 tick)
{
    rnet_u32 tip;
    rnet_u32 slop;
    rnet_u32 lo;
    rnet_u32 hi;
    rnet_u32 remote_hi;
    rnet_u32 ancient;
    if (s == NULL || s->phase != RNET_PHASE_RUNNING)
    {
        return 1;
    }
    tip = rnet_wire_tick_from_sim(s->sim_tick, s->delay);
    slop = (rnet_u32)s->cfg.bundle_redundancy + 8u;
    if (slop < 8u)
    {
        slop = 8u;
    }
    lo = (s->sim_tick > slop) ? (s->sim_tick - slop) : 0u;
    hi = tip + slop;
    /* A spectator is not paced by anyone: the seats run on without waiting
     * for it, so it routinely trails them by more than tip + slop. Rows past
     * that window used to be dropped -- and as a spectator sends no INPUT no
     * seat ever widened its retransmit to cover them: the spectator stalled
     * for good at the first tick it had dropped (seen: sim=15 with the seats
     * at 4900). Accept anything the ring can hold without overwriting a row
     * at or after lo; rows it still lacks are pulled with OBS_CATCHUP. */
    if (s->is_observer)
        hi = s->sim_tick + (rnet_u32)RNET_HISTORY_LENGTH - slop - 1u;
    if (tick >= lo && tick <= hi)
    {
        return 1;
    }
    /* Rematch / asymmetric boot: the faster peer invents up to P ahead, then
     * pcap_freeze. By then lo = sim-slop can sit above the stalled peer tip
     * (e.g. remote=5, sim=15, lo=7) so tip+1 is dropped forever and freeze
     * never clears. Accept gap-filling tips that extend the remote watermark
     * even when below lo; still reject ancient hard_resync residue. */
    if (slot >= s->cfg.slot_count)
    {
        return 0;
    }
    remote_hi = rnet_ring_highest_valid(&s->remote_rings[slot]);
    ancient = 64u;
    if (tick > remote_hi && tick <= hi &&
        (s->sim_tick <= ancient || tick + ancient >= s->sim_tick))
    {
        return 1;
    }
    return 0;
}

static void store_remote_frame(RNetSession *s, rnet_u8 slot, const RNetWireFrame *frame)
{
    RNetInputSample sample;
    RNetInputSample existing;
    if ((s == NULL) || (frame == NULL) || (slot >= s->cfg.slot_count) || (slot == s->cfg.local_slot))
    {
        return;
    }
    /* Drop previous-epoch residue after hard_resync (sim_tick→0). Those ticks
     * share ring slots with the new tip via tick%HISTORY and first-wins would
     * otherwise keep remotes_ready_for_sim failing until the peer stops. */
    if (!remote_tick_in_live_window(s, slot, frame->tick))
    {
        return;
    }
    /* A join-in-progress room logs rows of at most RNET_LOG_ROW_BYTES. A peer's
     * wider frame is not a row of this room (the room desyncs on it anyway); it
     * must not reach the log, whose rlog_put would otherwise switch the whole
     * log off for everybody (and with it late-join serving). */
    if (s->cfg.late_join && !s->row_log_off && frame->size > RNET_LOG_ROW_BYTES)
    {
        static int logged;
        if (!logged)
        {
            logged = 1;
            fprintf(stderr, "recomp-net: seat %u sent a %u-byte input row (this room's rows are <= %u): "
                            "ignored\n", (unsigned)slot, (unsigned)frame->size, (unsigned)RNET_LOG_ROW_BYTES);
        }
        return;
    }
    /* First-wins: later retransmits must not overwrite a latched wire row. */
    if (rnet_ring_get(&s->remote_rings[slot], frame->tick, &existing))
    {
        return;
    }
    memset(&sample, 0, sizeof(sample));
    sample.tick = frame->tick;
    sample.size = frame->size;
    if (frame->size > 0)
    {
        memcpy(sample.bytes, frame->bytes, frame->size);
    }
    sample.valid = 1;
    rnet_ring_store(&s->remote_rings[slot], &sample);
    if (s->is_sim_authority)
        rlog_put(s, slot, sample.tick, sample.bytes, sample.size);
    /* The contiguous watermark is what the authority's drop wire is built
     * from (occ_declare_drop). It used to be kept only for 3+ seats or an
     * observer, so a 2-seat room under the drop policy declared its guest
     * absent from wire 0 (the watermark sat at "nothing yet"): a spectator
     * replaying from the table then simulated the guest as CPU all match. A
     * 2-seat room WITHOUT the policy keeps the old acks (0xffffffff). */
    if (s->cfg.slot_count > 2 || s->is_observer || s->seat_loss_timeout_ms != 0u)
    {
        rnet_u32 next = s->remote_contiguous_ack[slot] + 1u;
        while (rnet_ring_get(&s->remote_rings[slot], next, &existing))
        {
            s->remote_contiguous_ack[slot] = next;
            if (next == 0xffffffffu)
                break;
            ++next;
        }
        contig_advance(s, slot);
    }
    {
        rnet_u32 idx = frame->tick % RNET_HISTORY_LENGTH;
        rnet_u64 now = session_now(s);
        s->remote_arr_tick[slot][idx] = frame->tick;
        s->remote_arr_ms[slot][idx] = now ? now : 1u;
    }
}

static rnet_u32 hash_resolved_inputs(rnet_u32 sim_tick, const RNetInputSample *by_slot, int slots)
{
    rnet_u8 buf[4 + RNET_MAX_SLOTS * (2 + RNET_INPUT_MAX)];
    size_t n = 0;
    int i;

    buf[n++] = (rnet_u8)(sim_tick & 0xFFu);
    buf[n++] = (rnet_u8)((sim_tick >> 8) & 0xFFu);
    buf[n++] = (rnet_u8)((sim_tick >> 16) & 0xFFu);
    buf[n++] = (rnet_u8)((sim_tick >> 24) & 0xFFu);
    for (i = 0; i < slots; ++i)
    {
        rnet_u16 sz = by_slot[i].size;
        if (sz > RNET_INPUT_MAX)
        {
            sz = RNET_INPUT_MAX;
        }
        buf[n++] = (rnet_u8)(sz & 0xFFu);
        buf[n++] = (rnet_u8)((sz >> 8) & 0xFFu);
        if (sz > 0)
        {
            memcpy(buf + n, by_slot[i].bytes, sz);
            n += sz;
        }
    }
    return rnet_proto_checksum(buf, n);
}

static void send_input_confirm_tick(RNetSession *s, rnet_u32 tick,
                                    rnet_u32 hash)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int len;
    rnet_u64 now;

    if (s == NULL) return;
    len = rnet_proto_encode_input_confirm(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->wire_slot,
                                          s->input_epoch, tick, hash);
    send_raw(s, buf, len);
    now = session_now(s);
    s->confirm_last_sent_ms[tick % RNET_HISTORY_LENGTH] = now;
}

/* ---- full input log (join in progress) ---------------------------------- */

static void rlog_free(RNetRowLog *l)
{
    free(l->rows);
    free(l->sizes);
    free(l->have);
    memset(l, 0, sizeof(*l));
}

static int rlog_reserve(RNetRowLog *l, rnet_u32 tick)
{
    rnet_u32 cap;
    rnet_u8 *rows, *have;
    if (tick < l->cap)
        return 1;
    if (tick >= RNET_LOG_MAX_TICKS)
        return 0;
    cap = l->cap ? l->cap : 4096u;
    while (cap <= tick)
        cap <<= 1;
    rows = (rnet_u8 *)realloc(l->rows, (size_t)cap * RNET_LOG_ROW_BYTES);
    if (rows == NULL)
        return 0;
    l->rows = rows;
    {
        rnet_u8 *sizes = (rnet_u8 *)realloc(l->sizes, (size_t)cap);
        if (sizes == NULL)
            return 0;
        l->sizes = sizes;
    }
    have = (rnet_u8 *)realloc(l->have, (size_t)(cap + 7u) / 8u);
    if (have == NULL)
        return 0;
    l->have = have;
    memset(l->have + (l->cap + 7u) / 8u, 0, (size_t)((cap + 7u) / 8u - (l->cap + 7u) / 8u));
    /* A byte shared by old and new ticks keeps its bits (memset starts past it). */
    l->cap = cap;
    return 1;
}

static void rlog_put(RNetSession *s, int slot, rnet_u32 tick, const rnet_u8 *bytes, rnet_u16 size)
{
    RNetRowLog *l;
    if (!s->cfg.late_join || s->row_log_off || slot < 0 || slot >= RNET_MAX_SLOTS)
        return;
    if (size > RNET_LOG_ROW_BYTES)
    {
        /* Rows wider than a 4-byte pad do not fit the log. Say so once;
         * serving falls back to the 128-row rings. Only the room's OWN rows
         * (the local seat's, or the authority's first sample) decide that: a
         * peer's wide frame is refused in store_remote_frame instead. */
        fprintf(stderr, "recomp-net: input log disabled (row size %u > %u)\n", (unsigned)size,
                (unsigned)RNET_LOG_ROW_BYTES);
        s->row_log_off = 1;
        return;
    }
    l = &s->row_log[slot];
    if (!rlog_reserve(l, tick))
        return;
    memset(l->rows + (size_t)tick * RNET_LOG_ROW_BYTES, 0, RNET_LOG_ROW_BYTES);
    if (size > 0)
        memcpy(l->rows + (size_t)tick * RNET_LOG_ROW_BYTES, bytes, size);
    l->sizes[tick] = (rnet_u8)size;
    l->have[tick >> 3] |= (rnet_u8)(1u << (tick & 7u));
    if (tick + 1u > l->hi)
        l->hi = tick + 1u;
}

static int rlog_get(const RNetRowLog *l, rnet_u32 tick, rnet_u8 *bytes, rnet_u16 *size)
{
    if (tick >= l->hi || !(l->have[tick >> 3] & (1u << (tick & 7u))))
        return 0;
    memcpy(bytes, l->rows + (size_t)tick * RNET_LOG_ROW_BYTES, RNET_LOG_ROW_BYTES);
    *size = l->sizes[tick];
    return 1;
}

static void local_ring_put(RNetSession *s, const RNetInputSample *sample)
{
    rnet_ring_store(&s->local_ring, sample);
    if (s->cfg.late_join && !s->is_observer && s->cfg.local_slot < s->cfg.slot_count)
        rlog_put(s, s->cfg.local_slot, sample->tick, sample->bytes, sample->size);
}

/* ---- occupancy schedule (wire v6) --------------------------------------- */

/* 1 when the table says `slot` is present at `wire` (the base occupied_mask is
 * a separate, static question). latest != 0 asks for the state after the whole
 * table, whatever wire the events are scheduled for. */
static int occ_present(const RNetSession *s, int slot, rnet_u32 wire, int latest)
{
    int present = 1;
    rnet_u8 i;
    for (i = 0; i < s->occ_n; ++i)
    {
        if (s->occ[i].slot == slot && (latest || s->occ[i].wire <= wire))
            present = s->occ[i].present;
    }
    return present;
}

/* A seat of the room (base occupied_mask) the table has taken away: its pad
 * is not a human's any more. Unoccupied seats of a sparse room are NOT
 * "absent" -- they never had an owner. LATEST = after the whole table, even
 * the events scheduled for a wire we have not reached (a dropped seat; also
 * the seat of a join that is still being agreed is NOT absent here). */
static int occ_absent_latest(const RNetSession *s, int slot)
{
    return s->occ_n != 0 && slot >= 0 && slot < (int)s->cfg.slot_count &&
           rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot) && !occ_present(s, slot, 0, 1);
}

static int occ_absent_at(const RNetSession *s, int slot, rnet_u32 wire)
{
    return s->occ_n != 0 && slot >= 0 && slot < (int)s->cfg.slot_count &&
           rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot) && !occ_present(s, slot, wire, 0);
}

/* The same question asked at the wire the sim is consuming now. This is what
 * "does anything wait on this seat" means once an ADD can be scheduled for a
 * wire in the future: the joiner is in the table at once (so every peer holds
 * the same schedule) but nothing waits for its rows before wire A. */
static int occ_absent_now(const RNetSession *s, int slot)
{
    /* A seat the table has dropped waits for nobody even before its drop wire
     * (that is how stage B lifts the stall); an ADD scheduled ahead adds the
     * time-aware half. */
    return occ_absent_latest(s, slot) || occ_absent_at(s, slot, s->sim_tick);
}

/* A seat that has a peer THERE at launch (a launch-time handshake -- READY,
 * the host's boot transfer -- waits for these only: an open seat has no
 * process behind it until somebody joins). */
static int seat_at_launch(const RNetSession *s, int slot)
{
    return rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot) &&
           !(s->cfg.open_mask & (1u << (unsigned)slot));
}

/* The wire the current occupant of `slot` produces rows from: the latest
 * PRESENT event's wire (0 when there is none). Rows below it belong to an
 * earlier occupant (or to nobody). */
static rnet_u32 occ_own_from(const RNetSession *s, int slot)
{
    rnet_u32 w = 0;
    rnet_u8 i;
    for (i = 0; i < s->occ_n; ++i)
        if (s->occ[i].slot == slot && s->occ[i].present)
            w = s->occ[i].wire;
    return w;
}

/* The first wire above `wire` the table makes `slot` present at, or
 * 0xffffffff when none is scheduled. */
static rnet_u32 occ_next_present_wire(const RNetSession *s, int slot, rnet_u32 wire)
{
    rnet_u32 best = 0xffffffffu;
    rnet_u8 i;
    for (i = 0; i < s->occ_n; ++i)
        if (s->occ[i].slot == slot && s->occ[i].present && s->occ[i].wire > wire &&
            s->occ[i].wire < best && occ_present(s, slot, s->occ[i].wire, 0))
            best = s->occ[i].wire;
    return best;
}

/* The contiguous watermark of a seat's rows, extended over the wires the
 * table makes absent (those rows are synthesized, nobody will ever send them).
 * Without this a seat that is absent below A never "has" its first rows, and
 * every acknowledgment, catch-up request and drop wire built from the
 * watermark would be stuck at "nothing yet". */
/* How far past the sim an ABSENT-until-further-notice seat's watermark may run.
 * An ADD scheduled later starts at least tip + RNET_ADD_LEAD_TICKS (112) ahead
 * of the authority and a peer is at most the prediction cap (<= 32) ahead of
 * it, so staying under sim + 64 can never pass a wire an ADD could still pick:
 * the watermark would otherwise claim rows the joiner has not produced. */
#define RNET_CONTIG_LOOKAHEAD 64u

static void contig_advance(RNetSession *s, int slot)
{
    rnet_u32 next, jump;
    RNetInputSample ex;
    int guard = 64;
    if (slot < 0 || slot >= (int)s->cfg.slot_count || slot >= RNET_MAX_SLOTS ||
        slot == (int)s->cfg.local_slot || s->occ_n == 0 ||
        !rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot))
        return;
    while (guard-- > 0)
    {
        next = s->remote_contiguous_ack[slot] + 1u; /* UINT32_MAX + 1 = tick 0 */
        if (rnet_ring_get(&s->remote_rings[slot], next, &ex))
        {
            s->remote_contiguous_ack[slot] = next;
            continue;
        }
        if (!occ_absent_at(s, slot, next))
            break;
        jump = occ_next_present_wire(s, slot, next);
        if (jump != 0xffffffffu && jump > next)
            s->remote_contiguous_ack[slot] = jump - 1u;
        else if (next < s->sim_tick + RNET_CONTIG_LOOKAHEAD)
            s->remote_contiguous_ack[slot] = next;
        else
            break;
    }
}

static void contig_advance_all(RNetSession *s)
{
    int slot;
    if (s->occ_n == 0)
        return;
    for (slot = 0; slot < (int)s->cfg.slot_count && slot < RNET_MAX_SLOTS; ++slot)
        contig_advance(s, slot);
}

static void occ_send(RNetSession *s)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int n;
    if (s->occ_n == 0)
        return;
    n = rnet_proto_encode_occ_event(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                    s->wire_slot, s->occ, s->occ_n);
    if (n > 0)
        send_raw(s, buf, n);
}

static void occ_log_event(const RNetSession *s, const RNetOccWire *e, const char *why)
{
    fprintf(stderr, "recomp-net: occupancy event gen=%u seat %u %s from wire %u%s%s (local seat %d)\n",
            (unsigned)e->gen, (unsigned)e->slot, e->present ? "PRESENT" : "ABSENT (CPU)",
            (unsigned)e->wire, why ? " -- " : "", why ? why : "", (int)s->cfg.local_slot);
}

static void occ_append(RNetSession *s, rnet_u32 wire, int slot, int present, const char *why)
{
    RNetOccWire *e = &s->occ[s->occ_n];
    e->wire = wire;
    e->slot = (rnet_u8)slot;
    e->present = present ? 1u : 0u;
    e->gen = (rnet_u16)(s->occ_n + 1u);
    s->occ_n++;
    occ_log_event(s, e, why);
    if (slot == (int)s->cfg.local_slot && !present)
        s->dropped_self = 1;
    /* A seat joining a room that had ONE peer: the single-peer filter of the
     * rollback control traffic (rb_peer_slot) would drop the joiner's packets. */
    if (present && s->rb_peer_slot >= 0)
        s->rb_peer_slot = -1;
    /* A seat given to somebody new starts clean: the BYE / silence of whoever
     * held it before must not count against the newcomer (the authority would
     * drop it again at once), and nobody has heard from it yet. */
    if (present && slot >= 0 && slot < RNET_MAX_SLOTS)
    {
        s->peer_gone_mask &= ~(1u << slot);
        s->peer_rx_ms[slot] = 0;
        s->peer_ack_seen[slot] = 0;
        s->peer_ack_tick[slot] = 0xffffffffu;
    }
    contig_advance_all(s);
}

/* The launch's open seats: scheduled ABSENT from wire 0 on every peer, the
 * same way, before anything is sent. They are the first events of the table,
 * so the authority's whole-table resend lines up with them gen for gen. */
static void occ_seed_open(RNetSession *s)
{
    int slot;
    if (s->cfg.open_mask == 0u || s->is_observer)
        return;
    for (slot = 0; slot < (int)s->cfg.slot_count && slot < RNET_MAX_SLOTS; ++slot)
        if ((s->cfg.open_mask & (1u << slot)) && rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot) &&
            s->occ_n < RNET_OCC_MAX)
            occ_append(s, 0u, slot, 0, "open seat at launch");
}

static void occ_on_event(RNetSession *s, const RNetDecodedPacket *pkt)
{
    rnet_u8 i;
    /* Only the authority writes the table, and the hub guarantees that a
     * packet naming seat 0 came from seat 0. */
    if (s->is_sim_authority || pkt->local_slot != 0)
        return;
    for (i = 0; i < pkt->occ_count; ++i)
    {
        const RNetOccWire *e = &pkt->occ[i];
        if (e->gen != (rnet_u16)(s->occ_n + 1u))
        {
            if (e->gen <= s->occ_n &&
                (s->occ[e->gen - 1u].wire != e->wire || s->occ[e->gen - 1u].slot != e->slot ||
                 s->occ[e->gen - 1u].present != e->present))
                fprintf(stderr,
                        "recomp-net: occupancy table DISAGREES with the authority at gen %u "
                        "(ours: seat %u wire %u present %u; theirs: seat %u wire %u present %u) "
                        "-- the peers' launch open-seat masks differ\n",
                        (unsigned)e->gen, (unsigned)s->occ[e->gen - 1u].slot,
                        (unsigned)s->occ[e->gen - 1u].wire, (unsigned)s->occ[e->gen - 1u].present,
                        (unsigned)e->slot, (unsigned)e->wire, (unsigned)e->present);
            continue; /* already held (or a gap: the next full resend fills it) */
        }
        if (e->slot >= s->cfg.slot_count || s->occ_n >= RNET_OCC_MAX)
            break;
        occ_append(s, e->wire, e->slot, e->present, "authority");
    }
    if (!s->is_observer && s->occ_n != 0)
    {
        rnet_u8 buf[64];
        int n = rnet_proto_encode_occ_ack(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                          s->wire_slot, (rnet_u16)s->occ_n);
        if (n > 0)
            send_raw(s, buf, n);
    }
}

/* Re-serve a dropped seat's last rows on its behalf. The seat itself would
 * answer a peer's acks with whatever that peer lost; once it is gone nobody
 * can, and a peer holding a hole below the drop wire would never get the real
 * row (it would keep a prediction the others replaced). The authority holds
 * every row it acked contiguously -- all of those below the drop wire. The
 * packet names the dropped seat, which is only legal from the authority's own
 * egress (the hub checks claimed seats on guest INGRESS). */
static void occ_serve_dropped_rows(RNetSession *s, int slot, rnet_u32 hi)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    RNetWireFrame frames[RNET_MAX_BUNDLE];
    rnet_u32 acks[RNET_MAX_SLOTS];
    rnet_u32 lo, t;
    int i;
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
        acks[i] = 0xffffffffu;
    lo = hi > 47u ? hi - 47u : 0u;
    t = lo;
    while (t <= hi)
    {
        int count = 0, len;
        while (t <= hi && count < RNET_MAX_BUNDLE)
        {
            RNetInputSample sample;
            if (rnet_ring_get(&s->remote_rings[slot], t, &sample))
            {
                frames[count].tick = sample.tick;
                frames[count].size = sample.size;
                memcpy(frames[count].bytes, sample.bytes, sample.size);
                count++;
            }
            ++t;
        }
        if (count == 0)
            continue;
        len = rnet_proto_encode_input_acks(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                           (rnet_u8)slot, s->input_epoch, 0xffffffffu, frames, count,
                                           acks, s->cfg.slot_count);
        if (len > 0)
            send_raw(s, buf, len);
    }
}

/* Table entries kept free for DROPs: a join (ADD, and its cancelling drop) is
 * refused once fewer than this many remain, so a departed seat can always be
 * recorded and an unconfirmed ADD can always be cancelled. */
#define RNET_OCC_RESERVE 8u
/* After a join was abandoned (not confirmed in time, or its joiner left while
 * it was agreed) claims are answered BUSY for this long, and after this many
 * abandoned joins in one match the room takes no more: a joiner that never
 * acks stalls the sealing for RNET_ADD_TIMEOUT_MS and costs 2 table entries
 * per try, and a claim is open to any spectator. */
#define RNET_ADD_COOLDOWN_MS 3000u
#define RNET_ADD_MAX_ABORTS 8u
/* A claimant's replay position (SEAT_CLAIM obs_tick) is its own word. It
 * cannot honestly be ahead of the authority's sealed tip by more than the
 * input delay and a little slack. */
#define RNET_CLAIM_TICK_SLACK 64u

/* Returns 1 when the drop is in the table. 0 = the table is full: the caller
 * must fall back to the legacy end of the match (peer_gone), or the departed
 * seat would be waited on for ever (nothing could ever record it). */
static int occ_declare_drop(RNetSession *s, int slot, const char *why)
{
    rnet_u32 wire, c = s->remote_contiguous_ack[slot];
    rnet_u8 i;
    if (s->occ_n >= RNET_OCC_MAX)
    {
        fprintf(stderr, "recomp-net: occupancy table full (%u events): cannot drop seat %d (%s) -- "
                        "the match ends as in a room without the drop policy\n",
                (unsigned)s->occ_n, slot, why ? why : "declared");
        s->peer_gone = 1;
        return 0;
    }
    /* From the first wire tick of this seat the authority does not hold: every
     * row below it is real and in the authority's ring, every row from it on
     * is replaced by the synthesized CPU row on every peer. */
    wire = (c == 0xffffffffu) ? 0u : c + 1u;
    for (i = 0; i < s->occ_n; ++i)
        if (s->occ[i].slot == slot && s->occ[i].wire > wire)
            wire = s->occ[i].wire; /* a seat's events stay monotone in wire */
    if (s->add_pending && s->add_slot == slot)
    {
        /* The join was still being agreed: it is over. */
        s->add_pending = 0;
        s->adds_aborted++;
        s->claim_cooldown_until_ms = session_now(s) + RNET_ADD_COOLDOWN_MS;
    }
    occ_append(s, wire, slot, 0, why);
    s->occ_last_tx_ms = 0;
    s->reserve_hi[slot] = wire ? wire - 1u : 0u;
    s->reserve_until_ms[slot] = session_now(s) + 3000u;
    s->reserve_last_ms[slot] = 0;
    (void)rnet_session_state_drop_peer(s, slot);
    return 1;
}

/* ---- join in progress: ADD, barrier, claims ------------------------------ */

/* Wire ticks between the authority's sealed tip and the wire a joiner's seat
 * becomes present at. It has to cover the barrier (the authority stops
 * sealing at A - D - P - 1, so nobody consumes a row at A before every peer
 * holds the event: P <= 32, D <= 20) plus the round trips of the grant. */
#define RNET_ADD_LEAD_TICKS 112u
/* A join that is not acknowledged by every present seat within this is
 * abandoned (the seat goes back to the CPU) so the match is never held. */
#define RNET_ADD_TIMEOUT_MS 6000u

static int occ_seat_free(const RNetSession *s, int slot)
{
    return slot >= 1 && slot < (int)s->cfg.slot_count && slot < RNET_MAX_SLOTS &&
           rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot) && occ_absent_latest(s, slot) &&
           (!s->claimable_set || (s->claimable_mask & (1u << slot))) &&
           !(s->add_pending && s->add_slot == slot);
}

static void occ_send_grant(RNetSession *s, rnet_u8 wire_id, rnet_u8 slot, rnet_u8 reason,
                           rnet_u32 wire, rnet_u16 gen)
{
    rnet_u8 buf[64];
    int n = rnet_proto_encode_seat_grant(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                         s->wire_slot, wire_id, slot, reason, wire, gen, s->delay);
    if (n > 0)
        send_raw(s, buf, n);
}

/* Authority: schedule `slot` PRESENT from a wire a safe distance ahead of the
 * sealed tip, and (for a spectator joiner, wire_id >= slot_count) re-bind its
 * hub identity to the seat. Returns 1 and the wire, or 0. */
int rnet_session_declare_seat_add(RNetSession *s, int slot, int wire_id, rnet_u32 *wire_out)
{
    rnet_u32 wire;
    rnet_u8 i;
    if (s == NULL || !s->is_sim_authority || s->phase != RNET_PHASE_RUNNING || s->add_pending ||
        s->occ_n + RNET_OCC_RESERVE > RNET_OCC_MAX || !occ_seat_free(s, slot))
        return 0;
    wire = rnet_wire_tick_from_sim(s->sim_tick, s->delay) + RNET_ADD_LEAD_TICKS;
    for (i = 0; i < s->occ_n; ++i)
        if (s->occ[i].slot == slot && s->occ[i].wire > wire)
            wire = s->occ[i].wire;
    if (wire_id >= (int)s->cfg.slot_count && wire_id < RNET_HUB_MAX_PEERS)
        rnet_session_hub_rebind(s, wire_id, slot);
    occ_append(s, wire, slot, 1, wire_id >= 0 ? "seat claimed by a joiner" : "declared");
    s->add_pending = 1;
    s->add_slot = (rnet_u8)slot;
    s->add_wire_id = (rnet_u8)(wire_id >= 0 && wire_id < 256 ? wire_id : 0xff);
    s->add_gen = (rnet_u16)s->occ_n;
    s->add_wire = wire;
    s->add_since_ms = session_now(s);
    s->add_grant_ms = 0;
    s->occ_last_tx_ms = 0;
    if (wire_out)
        *wire_out = wire;
    return 1;
}

/* 1 while the authority holds an ADD back (not every present seat acked it
 * yet), with the wire it is scheduled for: the driver stops sealing ticks
 * whose wire could reach it (see the barrier in the rollback driver). */
int rnet_session_occ_barrier(const RNetSession *s, rnet_u32 *wire)
{
    if (s == NULL || !s->add_pending)
        return 0;
    if (wire)
        *wire = s->add_wire;
    return 1;
}

static void occ_barrier_pump(RNetSession *s, rnet_u64 now)
{
    int slot, all = 1;
    if (!s->add_pending)
        return;
    for (slot = 1; slot < (int)s->cfg.slot_count && slot < RNET_MAX_SLOTS; ++slot)
    {
        if (!rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot) || !occ_present(s, slot, 0, 1))
            continue;
        if (s->occ_peer_ack[slot] < s->add_gen)
            all = 0;
    }
    if (all)
    {
        s->add_pending = 0;
        s->adds_done++;
        fprintf(stderr, "recomp-net: seat %u join agreed by every present seat (gen %u, wire %u, %u ms)\n",
                (unsigned)s->add_slot, (unsigned)s->add_gen, (unsigned)s->add_wire,
                (unsigned)(now - s->add_since_ms));
        return;
    }
    if (now - s->add_since_ms >= RNET_ADD_TIMEOUT_MS)
    {
        /* The joiner (or a peer) never confirmed: take the seat back at the
         * same wire (a later event at the same wire wins), so nobody is held. */
        fprintf(stderr, "recomp-net: seat %u join NOT confirmed within %u ms -- cancelled\n",
                (unsigned)s->add_slot, (unsigned)RNET_ADD_TIMEOUT_MS);
        s->add_pending = 0;
        s->adds_aborted++;
        s->claim_cooldown_until_ms = now + RNET_ADD_COOLDOWN_MS;
        if (s->occ_n < RNET_OCC_MAX)
        {
            rnet_u8 sl = s->add_slot;
            occ_append(s, s->add_wire, sl, 0, "join not confirmed");
            s->occ_last_tx_ms = 0;
        }
        return;
    }
    if (s->add_wire_id != 0xff && (s->add_grant_ms == 0 || now - s->add_grant_ms >= 100u))
    {
        occ_send_grant(s, s->add_wire_id, s->add_slot, 0, s->add_wire, s->add_gen);
        s->add_grant_ms = now ? now : 1u;
    }
}

/* Authority: a joiner's SEAT_CLAIM. */
static void occ_on_claim(RNetSession *s, const RNetDecodedPacket *pkt)
{
    rnet_u8 wire_id = pkt->local_slot;
    int slot = -1, i;
    if (!s->is_sim_authority || !obs_wire_is_spectator(s, wire_id))
        return;
    if (!s->cfg.late_join || s->phase != RNET_PHASE_RUNNING || s->seat_loss_timeout_ms == 0u)
    {
        occ_send_grant(s, wire_id, RNET_GRANT_NONE, RNET_GRANT_REASON_STATE, 0, 0);
        return;
    }
    if (s->add_pending && s->add_wire_id == wire_id)
        return; /* the pending grant is already being re-sent */
    if (s->add_pending)
    {
        occ_send_grant(s, wire_id, RNET_GRANT_NONE, RNET_GRANT_REASON_BUSY, 0, 0);
        return;
    }
    /* Too many abandoned joins this match: no more (see RNET_ADD_MAX_ABORTS);
     * right after one: not yet. An honest joiner re-asks on BUSY. */
    if (s->adds_aborted >= RNET_ADD_MAX_ABORTS)
    {
        occ_send_grant(s, wire_id, RNET_GRANT_NONE, RNET_GRANT_REASON_STATE, 0, 0);
        return;
    }
    if (s->claim_cooldown_until_ms != 0u && session_now(s) < s->claim_cooldown_until_ms)
    {
        occ_send_grant(s, wire_id, RNET_GRANT_NONE, RNET_GRANT_REASON_BUSY, 0, 0);
        return;
    }
    /* The replay position is the claimant's word: one ahead of anything the
     * seats have sealed is a lie (it would pass the live-edge test below). */
    if (pkt->obs_tick > s->sim_tick + RNET_CLAIM_TICK_SLACK)
    {
        occ_send_grant(s, wire_id, RNET_GRANT_NONE, RNET_GRANT_REASON_STATE, 0, 0);
        return;
    }
    /* It must be near the live edge: its replay at 1000+ ticks/s closes the
     * rest before A, but a joiner a minute behind would hold the barrier. */
    if (pkt->obs_tick + 160u < s->sim_tick)
    {
        occ_send_grant(s, wire_id, RNET_GRANT_NONE, RNET_GRANT_REASON_BUSY, 0, 0);
        return;
    }
    if (pkt->claim_want_slot != 0xffu && occ_seat_free(s, pkt->claim_want_slot))
        slot = pkt->claim_want_slot;
    else if (pkt->claim_want_slot == 0xffu)
        for (i = 1; i < (int)s->cfg.slot_count && slot < 0; ++i)
            if (occ_seat_free(s, i))
                slot = i;
    if (slot < 0)
    {
        occ_send_grant(s, wire_id, RNET_GRANT_NONE, RNET_GRANT_REASON_NO_SEAT, 0, 0);
        return;
    }
    if (rnet_session_declare_seat_add(s, slot, wire_id, NULL))
    {
        occ_send_grant(s, wire_id, (rnet_u8)slot, 0, s->add_wire, s->add_gen);
        s->add_grant_ms = session_now(s);
        occ_send(s);
        s->occ_last_tx_ms = session_now(s);
    }
    else
        occ_send_grant(s, wire_id, RNET_GRANT_NONE, RNET_GRANT_REASON_BUSY, 0, 0);
}

/* Observer -> seat: the grant for this wire id arrived. Everything the seat
 * needs at once: it stops being a spectator (own wire id, own ring, the
 * authority's delay), and keeps the table it already holds. */
static void session_promote(RNetSession *s, int slot, rnet_u8 delay)
{
    int i;
    s->cfg.local_slot = (rnet_u8)slot;
    s->cfg.wire_slot = (rnet_u8)slot;
    s->wire_slot = (rnet_u8)slot;
    s->is_observer = 0;
    s->is_sim_authority = 0;
    if (delay >= 2u && delay <= 20u)
    {
        s->delay = delay;
        s->cfg.input_delay = delay;
    }
    rnet_ring_clear(&s->local_ring);
    s->last_input_tip_valid = 0;
    s->last_input_ms = 0;
    memset(s->peer_ack_seen, 0, sizeof(s->peer_ack_seen));
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
        s->peer_ack_tick[i] = 0xffffffffu;
    s->claim_state = 2;
    s->grant_slot = (rnet_u8)slot;
    fprintf(stderr, "recomp-net: PROMOTED from spectator to seat %d (D=%u, sim %u)\n", slot,
            (unsigned)s->delay, (unsigned)s->sim_tick);
}

static void occ_on_grant(RNetSession *s, const RNetDecodedPacket *pkt)
{
    if (!s->is_observer || pkt->obs_target_slot != s->wire_slot || pkt->local_slot != 0 ||
        s->claim_state != 1)
        return;
    if (pkt->grant_slot == RNET_GRANT_NONE)
    {
        s->claim_reason = pkt->obs_reason;
        if (pkt->obs_reason == RNET_GRANT_REASON_BUSY)
        {
            s->claim_retry_ms = session_now(s) + 500u; /* just ask again */
            return;
        }
        s->claim_state = 3;
        fprintf(stderr, "recomp-net: seat claim refused (reason %u)\n", (unsigned)pkt->obs_reason);
        return;
    }
    if (pkt->grant_slot >= s->cfg.slot_count)
        return;
    s->grant_wire = pkt->grant_wire;
    session_promote(s, pkt->grant_slot, pkt->new_delay);
}

/* Joiner: start asking for a seat (0xff = any free one). */
int rnet_session_request_seat(RNetSession *s, int want_slot)
{
    if (s == NULL || !s->is_observer || !s->cfg.late_join || s->claim_state == 1)
        return 0;
    s->claim_want = (rnet_u8)(want_slot >= 0 && want_slot < 255 ? want_slot : 0xff);
    s->claim_state = 1;
    s->claim_last_ms = 0;
    s->claim_retry_ms = 0;
    return 1;
}

/* 0 = no claim, 1 = asking, 2 = granted (this session is a seat now; *slot and
 * *wire say which and from where), 3 = refused (*slot = reason). */
int rnet_session_claim_status(const RNetSession *s, int *slot, rnet_u32 *wire)
{
    if (s == NULL)
        return 0;
    if (s->claim_state == 2)
    {
        if (slot) *slot = s->grant_slot;
        if (wire) *wire = s->grant_wire;
    }
    else if (s->claim_state == 3 && slot)
        *slot = s->claim_reason;
    return s->claim_state;
}

static void claim_pump(RNetSession *s, rnet_u64 now)
{
    rnet_u8 buf[64];
    int n;
    if (!s->is_observer || s->claim_state != 1 || s->phase != RNET_PHASE_RUNNING ||
        now < s->claim_retry_ms)
        return;
    if (s->claim_last_ms != 0 && now - s->claim_last_ms < 200u)
        return;
    s->claim_last_ms = now ? now : 1u;
    n = rnet_proto_encode_seat_claim(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                     s->wire_slot, s->claim_want, s->sim_tick);
    if (n > 0)
        send_raw(s, buf, n);
}

/* The authority's per-pump decisions: who is gone, and keeping the table
 * delivered. */
static void occ_pump(RNetSession *s)
{
    rnet_u64 now;
    int slot, j;
    int unacked = 0;
    if (s->is_observer)
    {
        /* A spectator needs the table to know which seats to wait for, and
         * has nobody to ack to: ask for it (and tell the authority how far it
         * holds it; a behind observer is answered at once). */
        now = session_now(s);
        if (s->phase == RNET_PHASE_RUNNING && (s->occ_req_ms == 0u || now - s->occ_req_ms >= 500u))
        {
            rnet_u8 buf[64];
            int n = rnet_proto_encode_occ_ack(buf, sizeof(buf), s->cfg.protocol_magic,
                                              s->cfg.session_id, s->wire_slot, (rnet_u16)s->occ_n);
            if (n > 0)
                send_raw(s, buf, n);
            s->occ_req_ms = now ? now : 1u;
        }
        contig_advance_all(s);
        claim_pump(s, now);
        return;
    }
    if (s->occ_n != 0 && s->phase == RNET_PHASE_RUNNING)
        contig_advance_all(s);
    if (s->seat_loss_timeout_ms == 0u || !s->is_sim_authority || s->phase != RNET_PHASE_RUNNING)
        return;
    now = session_now(s);
    for (slot = 1; slot < (int)s->cfg.slot_count && slot < RNET_MAX_SLOTS; ++slot)
    {
        rnet_u64 age;
        int others_alive = 0;
        if (!rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot) || !occ_present(s, slot, 0, 1))
            continue;
        if (s->peer_gone_mask & (1u << slot))
        {
            (void)occ_declare_drop(s, slot, "said BYE");
            continue;
        }
        if (s->peer_rx_ms[slot] == 0u)
            continue; /* never heard: the link budget, not the drop timer */
        age = rnet_session_peer_rx_age_ms(s, slot);
        if (age < s->seat_loss_timeout_ms)
            continue;
        /* Only drop for a peer that is silent while others are talking: if
         * everybody is quiet the authority's own link is what died, and
         * dropping every guest would just hand the match to the CPU. */
        for (j = 1; j < (int)s->cfg.slot_count && j < RNET_MAX_SLOTS; ++j)
        {
            if (j == slot || !rnet_config_slot_occupied(&s->cfg, (rnet_u8)j) || !occ_present(s, j, 0, 1))
                continue;
            if (rnet_session_peer_rx_age_ms(s, j) < s->seat_loss_timeout_ms / 2u)
                others_alive = 1;
        }
        /* ... unless the host's own link is evidently fine (its lobby
         * websocket answers): then a lone silent guest is the one that is
         * gone, and the host plays on against the CPU. */
        if (!others_alive && s->link_ok_ms != 0u && now - s->link_ok_ms < 1000u)
            others_alive = 1;
        if (others_alive)
            (void)occ_declare_drop(s, slot, "silent (connection lost)");
    }
    occ_barrier_pump(s, now);
    if (s->occ_n == 0)
        return;
    for (slot = 1; slot < (int)s->cfg.slot_count && slot < RNET_MAX_SLOTS; ++slot)
        if (rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot) && occ_present(s, slot, 0, 1) &&
            s->occ_peer_ack[slot] < s->occ_n)
            unacked = 1;
    if (s->occ_last_tx_ms == 0u || now - s->occ_last_tx_ms >= (unacked ? 100u : 1000u))
    {
        occ_send(s);
        s->occ_last_tx_ms = now ? now : 1u;
    }
    for (slot = 1; slot < (int)s->cfg.slot_count && slot < RNET_MAX_SLOTS; ++slot)
    {
        if (s->reserve_until_ms[slot] != 0u && now < s->reserve_until_ms[slot] &&
            (s->reserve_last_ms[slot] == 0u || now - s->reserve_last_ms[slot] >= 100u))
        {
            occ_serve_dropped_rows(s, slot, s->reserve_hi[slot]);
            s->reserve_last_ms[slot] = now ? now : 1u;
        }
    }
}

static void handle_decoded(RNetSession *s, const RNetDecodedPacket *pkt)
{
    int i;
    if ((s == NULL) || (pkt == NULL))
    {
        return;
    }
    if (rnet_proto_is_rb_control(pkt->type) && pkt->input_epoch != s->input_epoch)
        return;
    if (pkt->session_id != s->cfg.session_id)
    {
        return;
    }
    switch (pkt->type)
    {
    case RNET_PKT_HELLO:
        if (pkt->slot_count != s->cfg.slot_count)
        {
            break;
        }
        if (pkt->wire_version != RNET_WIRE_VERSION)
        {
            /* A different wire (e.g. the 7-byte seal row) would misparse
             * mid-match: never link with it. */
            if (s->peer_wire_version_mismatch == 0)
                fprintf(stderr,
                        "recomp-net: peer slot %u speaks wire version %u, this "
                        "build %u — not linking (update both to the same build)\n",
                        (unsigned)pkt->local_slot, (unsigned)pkt->wire_version,
                        (unsigned)RNET_WIRE_VERSION);
            s->peer_wire_version_mismatch = pkt->wire_version ? pkt->wire_version : 0xFFu;
            break;
        }
        /* Peer is alive; move toward READY once we have exchanged HELLO. */
        if (s->phase == RNET_PHASE_LINKING)
        {
            s->phase = RNET_PHASE_READY;
        }
        /* Guest adopts host delay before RUNNING (host slot 0 is authoritative). */
        if (s->cfg.local_slot != 0 && pkt->local_slot == 0)
        {
            rnet_u8 hello_delay = pkt->delay;
            if (hello_delay >= 2 && hello_delay <= 20 && hello_delay != s->delay)
            {
                if (s->phase != RNET_PHASE_RUNNING || s->sim_tick == 0)
                {
                    s->delay = hello_delay;
                    s->cfg.input_delay = hello_delay;
                    if (s->phase != RNET_PHASE_RUNNING)
                        seed_delay_prefix(s);
                }
                else if (s->sim_tick > 0)
                {
                    static int delay_mismatch_logged;
                    if (!delay_mismatch_logged)
                    {
                        fprintf(stderr,
                                "recomp-net: HELLO delay mismatch local=%u peer=%u "
                                "after RUNNING — not adopting\n",
                                (unsigned)s->delay, (unsigned)hello_delay);
                        delay_mismatch_logged = 1;
                    }
                }
            }
        }
        break;
    case RNET_PKT_READY:
        if (pkt->local_slot < s->cfg.slot_count)
        {
            s->peer_ready[pkt->local_slot] = 1;
            /* START goes out once (maybe_bootstrap). A seat only sends READY
             * until it has seen START, so a READY that reaches the authority
             * after it started is a seat whose START was lost -- and nothing
             * else would ever move that seat to RUNNING: the rest of the room
             * starts, waits on its input, and the match never begins. Answer
             * it with START again (tick 0, as the first one said; a seat that
             * is already running ignores it). Paced by the peer's READY
             * (100 ms). Seats here; a spectator's READY is answered by
             * obs_on_ready below. */
            if (s->is_sim_authority && s->start_sent && s->phase == RNET_PHASE_RUNNING &&
                rnet_config_slot_occupied(&s->cfg, pkt->local_slot))
            {
                rnet_u8 sbuf[RNET_MAX_PACKET];
                int slen = rnet_proto_encode_start(sbuf, sizeof(sbuf), s->cfg.protocol_magic,
                                                   s->cfg.session_id, 0);
                send_raw(s, sbuf, slen);
            }
        }
        else
        {
            /* A spectator (wire slot past the seats): the roster gate before
             * START, START again (or a loud refusal) after it. */
            obs_on_ready(s, pkt->local_slot);
        }
        break;
    case RNET_PKT_START:
        /* Host-only. The authority never takes START from a peer (in a
         * two-seat direct session nothing else filters it). */
        if (s->is_sim_authority)
        {
            break;
        }
        if (s->phase != RNET_PHASE_RUNNING)
        {
            s->sim_tick = pkt->start_tick;
            s->phase = RNET_PHASE_RUNNING;
            seed_delay_prefix(s);
            send_input_bundle(s);
        }
        break;
    case RNET_PKT_INPUT:
        /* Drop prior-epoch tips (rapid rematch hard_resync → sim=0). */
        if (pkt->input_epoch != s->input_epoch)
        {
            break;
        }
        for (i = 0; i < pkt->frame_count; ++i)
        {
            store_remote_frame(s, pkt->local_slot, &pkt->frames[i]);
            /* Spectator: the newest row each seat has produced, whether or
             * not it fit our window -- a row we hold nothing up to is a hole
             * to ask for (obs_pump). */
            if (s->is_observer && pkt->local_slot < s->cfg.slot_count &&
                (!s->obs_seen_valid[pkt->local_slot] ||
                 pkt->frames[i].tick > s->obs_seen_tip[pkt->local_slot]))
            {
                s->obs_seen_tip[pkt->local_slot] = pkt->frames[i].tick;
                s->obs_seen_valid[pkt->local_slot] = 1;
            }
        }
        if (s->cfg.slot_count > 2 && pkt->local_slot < s->cfg.slot_count &&
            pkt->ack_count == s->cfg.slot_count &&
            rnet_config_slot_occupied(&s->cfg, pkt->local_slot))
        {
            rnet_u32 ack = pkt->acks[s->cfg.local_slot];
            if (!s->peer_ack_seen[pkt->local_slot] ||
                (ack != 0xffffffffu &&
                 (s->peer_ack_tick[pkt->local_slot] == 0xffffffffu ||
                  ack > s->peer_ack_tick[pkt->local_slot])))
            {
                s->peer_ack_tick[pkt->local_slot] = ack;
                s->peer_ack_seen[pkt->local_slot] = 1;
                link_rtt_note_ack(s, pkt->local_slot, ack);
            }
        }
        else if (s->cfg.slot_count == 2 && pkt->ack_tick > s->highest_remote_ack)
        {
            s->highest_remote_ack = pkt->ack_tick;
            link_rtt_note_ack(s, pkt->local_slot, pkt->ack_tick);
        }
        break;
    case RNET_PKT_DELAY_SYNC:
        /* Host-only (the scheduler only changes D on the sim authority), and
         * only values rnet_session_request_delay_change can send (2..20): a
         * larger D outruns the 128-tick input rings. */
        if (s->is_sim_authority || pkt->new_delay < 2u || pkt->new_delay > 20u)
        {
            break;
        }
        /* Immediate when already past effective_tick (or not RUNNING yet).
         * Otherwise queue so both peers commit on the same sim tick. */
        if (pkt->effective_tick <= s->sim_tick || s->phase != RNET_PHASE_RUNNING)
        {
            s->delay = pkt->new_delay;
            s->cfg.input_delay = pkt->new_delay;
            s->delay_pending = 0;
        }
        else
        {
            s->delay_pending = 1;
            s->delay_pending_value = pkt->new_delay;
            s->delay_pending_effective = pkt->effective_tick;
        }
        break;
    case RNET_PKT_INPUT_CONFIRM:
        if (pkt->input_epoch != s->input_epoch)
        {
            break;
        }
        if (pkt->local_slot < s->cfg.slot_count)
        {
            rnet_u32 index = pkt->confirm_sim_tick % RNET_HISTORY_LENGTH;
            s->peer_history_tick[index][pkt->local_slot] = pkt->confirm_sim_tick;
            s->peer_history_hash[index][pkt->local_slot] = pkt->confirm_hash;
            s->peer_history_valid[index][pkt->local_slot] = 1;
            if (s->published_valid[index] &&
                s->published_tick[index] == pkt->confirm_sim_tick &&
                s->published_hash[index] != pkt->confirm_hash)
            {
                s->input_desync = 1;
                s->desync_tick = pkt->confirm_sim_tick;
                s->desync_local_hash = s->published_hash[index];
                s->desync_remote_hash = pkt->confirm_hash;
            }
        }
        break;
    case RNET_PKT_BYE:
        /* A spectator leaving ends nobody's match: not the seats' (it used
         * to -- any BYE set peer_gone, so one spectator closing its window,
         * or being refused, sent every seat back to the lobby), and not
         * another spectator's. Only a seat's BYE counts. */
        if (pkt->local_slot >= s->cfg.slot_count)
        {
            break;
        }
        if (pkt->local_slot != s->wire_slot)
        {
            /* Aggregate: any peer's BYE, as before -- except under the drop
             * policy, where a GUEST seat leaving is the authority's call (it
             * schedules the seat as CPU) and only the authority's own BYE
             * ends the match for the others. Per seat: who said it. */
            if (!(s->seat_loss_timeout_ms != 0u && pkt->local_slot != 0))
                s->peer_gone = 1;
            if (pkt->local_slot < RNET_MAX_SLOTS)
            {
                s->peer_gone_mask |= 1u << pkt->local_slot;
            }
        }
        break;
    case RNET_PKT_OCC_EVENT:
        occ_on_event(s, pkt);
        break;
    case RNET_PKT_OCC_ACK:
        /* A seat cannot hold more of the table than the authority wrote: a
         * larger gen would release the join barrier for an event it never held. */
        if (s->is_sim_authority && pkt->local_slot < RNET_MAX_SLOTS &&
            pkt->occ_ack_gen <= s->occ_n && pkt->occ_ack_gen > s->occ_peer_ack[pkt->local_slot])
            s->occ_peer_ack[pkt->local_slot] = pkt->occ_ack_gen;
        /* A spectator's request: it never acks (its gen is not a barrier), but
         * one that holds less than the table is answered now. */
        if (s->is_sim_authority && obs_wire_is_spectator(s, pkt->local_slot) &&
            pkt->occ_ack_gen < s->occ_n)
            s->occ_last_tx_ms = 0;
        break;
    case RNET_PKT_SEAT_CLAIM:
        occ_on_claim(s, pkt);
        break;
    case RNET_PKT_SEAT_GRANT:
        occ_on_grant(s, pkt);
        break;
    case RNET_PKT_STATE_BEGIN:
        if (s->rb_peer_slot >= 0 && (int)pkt->local_slot != s->rb_peer_slot)
        {
            break;
        }
        state_on_begin(s, pkt);
        break;
    case RNET_PKT_STATE_CHUNK:
        if (s->rb_peer_slot >= 0 && (int)pkt->local_slot != s->rb_peer_slot)
        {
            break;
        }
        state_on_chunk(s, pkt);
        break;
    case RNET_PKT_STATE_ACK:
        if (!s->is_observer && obs_wire_is_spectator(s, pkt->local_slot))
        {
            obs_on_state_ack(s, pkt);
            break;
        }
        if (s->rb_peer_slot >= 0 && (int)pkt->local_slot != s->rb_peer_slot)
        {
            break;
        }
        state_on_ack(s, pkt);
        break;
    case RNET_PKT_OBS_CATCHUP:
        obs_on_catchup(s, pkt);
        break;
    case RNET_PKT_OBS_REFUSE:
        obs_on_refuse(s, pkt);
        break;
    case RNET_PKT_STATE_PROBE:
        if (s->rb_peer_slot >= 0 && (int)pkt->local_slot != s->rb_peer_slot)
        {
            break;
        }
        state_on_probe(s, pkt);
        break;
    case RNET_PKT_STATE_PROBE_REPLY:
        if (s->rb_peer_slot >= 0 && (int)pkt->local_slot != s->rb_peer_slot)
        {
            break;
        }
        state_on_probe_reply(s, pkt);
        break;
    case RNET_PKT_STATE_PROBE_DONE:
        if (s->rb_peer_slot >= 0 && (int)pkt->local_slot != s->rb_peer_slot)
        {
            break;
        }
        state_on_probe_done(s, pkt);
        break;
    case RNET_PKT_SIO_MULTI_XFER:
        if (pkt->local_slot != s->wire_slot)
        {
            rnet_u16 pad = (rnet_u16)(pkt->sio_confirm |
                                      ((rnet_u16)pkt->sio_vblank << 8));
            if (s->sio_xfer_count < RNET_SIO_XFER_QUEUE)
            {
                s->sio_xfer_q[s->sio_xfer_head].seq = pkt->sio_xfer_seq;
                s->sio_xfer_q[s->sio_xfer_head].send = pkt->sio_send;
                s->sio_xfer_q[s->sio_xfer_head].unit_id = pkt->sio_unit_id;
                s->sio_xfer_q[s->sio_xfer_head].confirm_pad = pad;
                s->sio_xfer_head = (s->sio_xfer_head + 1) % RNET_SIO_XFER_QUEUE;
                s->sio_xfer_count++;
            }
            else
            {
                /* Drop oldest so a live Cable Club burst can still progress. */
                s->sio_xfer_tail = (s->sio_xfer_tail + 1) % RNET_SIO_XFER_QUEUE;
                s->sio_xfer_count--;
                s->sio_xfer_q[s->sio_xfer_head].seq = pkt->sio_xfer_seq;
                s->sio_xfer_q[s->sio_xfer_head].send = pkt->sio_send;
                s->sio_xfer_q[s->sio_xfer_head].unit_id = pkt->sio_unit_id;
                s->sio_xfer_q[s->sio_xfer_head].confirm_pad = pad;
                s->sio_xfer_head = (s->sio_xfer_head + 1) % RNET_SIO_XFER_QUEUE;
                s->sio_xfer_count++;
            }
        }
        break;
    case RNET_PKT_RB_FRAME_COMMIT:
        /* Not rb_peer_slot-filtered: PSX-Link seats all emit the same
         * machine-level pair fold, so every peer's commit is comparable. */
        if (pkt->local_slot != s->wire_slot)
        {
            if (s->is_observer && pkt->rb_through_tick == 0u && pkt->local_slot < 16u &&
                pkt->local_slot < s->cfg.slot_count)
                s->obs_commit0_mask |= (rnet_u16)(1u << pkt->local_slot);
            if (s->rb_fc_q_count < RNET_RB_FC_QUEUE)
            {
                s->rb_fc_tick[s->rb_fc_q_head] = pkt->rb_through_tick;
                s->rb_fc_hash[s->rb_fc_q_head] = pkt->rb_state_hash;
                s->rb_fc_from[s->rb_fc_q_head] = pkt->local_slot;
                s->rb_fc_q_head = (s->rb_fc_q_head + 1) % RNET_RB_FC_QUEUE;
                s->rb_fc_q_count++;
            }
            else
            {
                /* Drop oldest so the watermark can still track the tip. */
                s->rb_fc_q_tail = (s->rb_fc_q_tail + 1) % RNET_RB_FC_QUEUE;
                s->rb_fc_q_count--;
                s->rb_fc_tick[s->rb_fc_q_head] = pkt->rb_through_tick;
                s->rb_fc_hash[s->rb_fc_q_head] = pkt->rb_state_hash;
                s->rb_fc_from[s->rb_fc_q_head] = pkt->local_slot;
                s->rb_fc_q_head = (s->rb_fc_q_head + 1) % RNET_RB_FC_QUEUE;
                s->rb_fc_q_count++;
            }
        }
        break;
    case RNET_PKT_RB_SYNC:
        if (pkt->rb_initiator == PROFILE_OFFER || pkt->rb_initiator == PROFILE_ACK)
        {
            profiles_receive(s, pkt);
            break;
        }
        /* The single-peer pin (a host with ONE guest) keeps other seats'
         * rollback control out -- but not a spectator's IDENT: a late joiner
         * waits for every present seat's identity before tick 0 and asks for
         * it with its own, and the seats only answer what they hear. Dropping
         * it here left a joiner of a two-player room stuck until it gave up
         * (startup_identity_missing). Nothing else a spectator sends is let in. */
        if (s->rb_peer_slot >= 0 && (int)pkt->local_slot != s->rb_peer_slot &&
            !(pkt->local_slot >= s->cfg.slot_count && pkt->rb_initiator == RNET_RB_SYNC_OP_IDENT))
        {
            break;
        }
        if (pkt->local_slot != s->wire_slot)
        {
            int i, dup = 0;
            for (i = 0; i < s->rb_sync_count; ++i)
            {
                int k = RB_Q_AT(s->rb_sync_tail, i);
                if (s->rb_sync_q[k].from == pkt->local_slot &&
                    s->rb_sync_q[k].epoch_id == pkt->rb_epoch_id &&
                    s->rb_sync_q[k].mismatch_tick == pkt->rb_mismatch_tick &&
                    s->rb_sync_q[k].load_tick == pkt->rb_load_tick &&
                    s->rb_sync_q[k].target_tick == pkt->rb_target_tick &&
                    s->rb_sync_q[k].corrected_slot == pkt->rb_corrected_slot &&
                    s->rb_sync_q[k].initiator == pkt->rb_initiator &&
                    s->rb_sync_q[k].flags == pkt->rb_flags)
                {
                    dup = 1;
                    break;
                }
            }
            if (dup)
                break;
        }
        if (pkt->local_slot != s->wire_slot &&
            s->rb_sync_count < RNET_RB_CTRL_QUEUE)
        {
            s->rb_sync_q[s->rb_sync_head].epoch_id = pkt->rb_epoch_id;
            s->rb_sync_q[s->rb_sync_head].mismatch_tick = pkt->rb_mismatch_tick;
            s->rb_sync_q[s->rb_sync_head].load_tick = pkt->rb_load_tick;
            s->rb_sync_q[s->rb_sync_head].target_tick = pkt->rb_target_tick;
            s->rb_sync_q[s->rb_sync_head].corrected_slot = pkt->rb_corrected_slot;
            s->rb_sync_q[s->rb_sync_head].initiator = pkt->rb_initiator;
            s->rb_sync_q[s->rb_sync_head].flags = pkt->rb_flags;
            s->rb_sync_q[s->rb_sync_head].from = pkt->local_slot;
            s->rb_sync_head = (s->rb_sync_head + 1) % RNET_RB_CTRL_QUEUE;
            s->rb_sync_count++;
        }
        else if (pkt->local_slot != s->wire_slot)
        {
            rb_ctrl_note_drop(s, "RB_SYNC");
        }
        break;
    case RNET_PKT_RB_SEAL_ROWS:
        if (pkt->local_slot != s->wire_slot)
        {
            int i, dup = 0;
            for (i = 0; i < s->rb_seal_count; ++i)
            {
                int k = RB_Q_AT(s->rb_seal_tail, i);
                if (s->rb_seal_q[k].from == pkt->local_slot &&
                    s->rb_seal_q[k].epoch_id == pkt->rb_epoch_id &&
                    s->rb_seal_q[k].mismatch_tick == pkt->rb_mismatch_tick &&
                    s->rb_seal_q[k].target_tick == pkt->rb_target_tick &&
                    s->rb_seal_q[k].slot == pkt->rb_slot &&
                    s->rb_seal_q[k].row_begin == pkt->rb_row_begin &&
                    s->rb_seal_q[k].row_count ==
                        (pkt->rb_row_count > RNET_RB_SEAL_ROWS_CHUNK_MAX
                             ? RNET_RB_SEAL_ROWS_CHUNK_MAX : pkt->rb_row_count))
                {
                    dup = 1;
                    break;
                }
            }
            if (dup)
                break;
            while (s->rb_seal_count >= RNET_RB_CTRL_QUEUE &&
                   rb_epoch_before(s->rb_seal_q[s->rb_seal_tail].epoch_id, pkt->rb_epoch_id))
            {
                s->rb_seal_tail = (s->rb_seal_tail + 1) % RNET_RB_CTRL_QUEUE;
                s->rb_seal_count--;
            }
        }
        if (pkt->local_slot != s->wire_slot &&
            s->rb_seal_count < RNET_RB_CTRL_QUEUE)
        {
            s->rb_seal_q[s->rb_seal_head].epoch_id = pkt->rb_epoch_id;
            s->rb_seal_q[s->rb_seal_head].mismatch_tick = pkt->rb_mismatch_tick;
            s->rb_seal_q[s->rb_seal_head].target_tick = pkt->rb_target_tick;
            s->rb_seal_q[s->rb_seal_head].row_begin = pkt->rb_row_begin;
            s->rb_seal_q[s->rb_seal_head].slot = pkt->rb_slot;
            s->rb_seal_q[s->rb_seal_head].row_count = pkt->rb_row_count;
            if (pkt->rb_row_count > 0)
            {
                rnet_u16 n = pkt->rb_row_count;
                if (n > RNET_RB_SEAL_ROWS_CHUNK_MAX)
                    n = RNET_RB_SEAL_ROWS_CHUNK_MAX;
                memcpy(s->rb_seal_q[s->rb_seal_head].rows, pkt->rb_rows,
                       sizeof(RNetRbWireFrame) * n);
                s->rb_seal_q[s->rb_seal_head].row_count = n;
            }
            s->rb_seal_q[s->rb_seal_head].from = pkt->local_slot;
            s->rb_seal_head = (s->rb_seal_head + 1) % RNET_RB_CTRL_QUEUE;
            s->rb_seal_count++;
        }
        else if (pkt->local_slot != s->wire_slot)
        {
            rb_ctrl_note_drop(s, "RB_SEAL_ROWS");
        }
        break;
    case RNET_PKT_RB_BASELINE:
        if (s->rb_peer_slot >= 0 && (int)pkt->local_slot != s->rb_peer_slot)
        {
            break;
        }
        if (pkt->local_slot != s->wire_slot)
        {
            int i, dup = 0;
            for (i = 0; i < s->rb_base_count; ++i)
            {
                int k = RB_Q_AT(s->rb_base_tail, i);
                if (s->rb_base_q[k].from == pkt->local_slot &&
                    s->rb_base_q[k].epoch_id == pkt->rb_epoch_id &&
                    s->rb_base_q[k].load_tick == pkt->rb_load_tick)
                {
                    s->rb_base_q[k].digest_master = pkt->rb_digest_master;
                    s->rb_base_q[k].digest_a = pkt->rb_digest_a;
                    s->rb_base_q[k].digest_b = pkt->rb_digest_b;
                    s->rb_base_q[k].digest_c = pkt->rb_digest_c;
                    dup = 1;
                    break;
                }
            }
            if (dup)
                break;
            while (s->rb_base_count >= RNET_RB_CTRL_QUEUE &&
                   rb_epoch_before(s->rb_base_q[s->rb_base_tail].epoch_id, pkt->rb_epoch_id))
            {
                s->rb_base_tail = (s->rb_base_tail + 1) % RNET_RB_CTRL_QUEUE;
                s->rb_base_count--;
            }
        }
        if (pkt->local_slot != s->wire_slot &&
            s->rb_base_count < RNET_RB_CTRL_QUEUE)
        {
            s->rb_base_q[s->rb_base_head].epoch_id = pkt->rb_epoch_id;
            s->rb_base_q[s->rb_base_head].load_tick = pkt->rb_load_tick;
            s->rb_base_q[s->rb_base_head].digest_master = pkt->rb_digest_master;
            s->rb_base_q[s->rb_base_head].digest_a = pkt->rb_digest_a;
            s->rb_base_q[s->rb_base_head].digest_b = pkt->rb_digest_b;
            s->rb_base_q[s->rb_base_head].digest_c = pkt->rb_digest_c;
            s->rb_base_q[s->rb_base_head].from = pkt->local_slot;
            s->rb_base_head = (s->rb_base_head + 1) % RNET_RB_CTRL_QUEUE;
            s->rb_base_count++;
        }
        else if (pkt->local_slot != s->wire_slot)
        {
            rb_ctrl_note_drop(s, "RB_BASELINE");
        }
        break;
    case RNET_PKT_RB_POST:
        if (s->rb_peer_slot >= 0 && (int)pkt->local_slot != s->rb_peer_slot)
        {
            break;
        }
        if (pkt->local_slot != s->wire_slot)
        {
            int i, dup = 0;
            for (i = 0; i < s->rb_post_count; ++i)
            {
                int k = RB_Q_AT(s->rb_post_tail, i);
                if (s->rb_post_q[k].from == pkt->local_slot &&
                    s->rb_post_q[k].epoch_id == pkt->rb_epoch_id &&
                    s->rb_post_q[k].target_tick == pkt->rb_target_tick)
                {
                    s->rb_post_q[k].digest_master = pkt->rb_digest_master;
                    s->rb_post_q[k].input_digest = pkt->rb_input_digest;
                    s->rb_post_q[k].match = pkt->rb_match;
                    dup = 1;
                    break;
                }
            }
            if (dup)
                break;
            while (s->rb_post_count >= RNET_RB_CTRL_QUEUE &&
                   rb_epoch_before(s->rb_post_q[s->rb_post_tail].epoch_id, pkt->rb_epoch_id))
            {
                s->rb_post_tail = (s->rb_post_tail + 1) % RNET_RB_CTRL_QUEUE;
                s->rb_post_count--;
            }
        }
        if (pkt->local_slot != s->wire_slot &&
            s->rb_post_count < RNET_RB_CTRL_QUEUE)
        {
            s->rb_post_q[s->rb_post_head].epoch_id = pkt->rb_epoch_id;
            s->rb_post_q[s->rb_post_head].target_tick = pkt->rb_target_tick;
            s->rb_post_q[s->rb_post_head].digest_master = pkt->rb_digest_master;
            s->rb_post_q[s->rb_post_head].input_digest = pkt->rb_input_digest;
            s->rb_post_q[s->rb_post_head].match = pkt->rb_match;
            s->rb_post_q[s->rb_post_head].from = pkt->local_slot;
            s->rb_post_head = (s->rb_post_head + 1) % RNET_RB_CTRL_QUEUE;
            s->rb_post_count++;
        }
        else if (pkt->local_slot != s->wire_slot)
        {
            rb_ctrl_note_drop(s, "RB_POST");
        }
        break;
    case RNET_PKT_MODSET:
        if (pkt->local_slot != s->wire_slot)
        {
            memcpy(s->modset_text, pkt->modset_text, sizeof(s->modset_text));
            s->modset_text[sizeof(s->modset_text) - 1] = '\0';
            s->modset_pending = 1u;
            s->modset_rx_seen = 1u;
        }
        break;
    case RNET_PKT_MODSET_ACK:
        if (pkt->local_slot != s->wire_slot)
        {
            if (pkt->local_slot < RNET_MAX_SLOTS)
            {
                rnet_u8 from = pkt->local_slot;
                memcpy(s->modset_ack_reason[from], pkt->modset_reason,
                       sizeof(s->modset_ack_reason[from]));
                s->modset_ack_reason[from][sizeof(s->modset_ack_reason[from]) - 1] = '\0';
                s->modset_ack_status[from] = pkt->modset_status;
                s->modset_ack_pending |= 1u << from;
            }
        }
        break;
    case RNET_PKT_RB_RESOLVED:
        if (pkt->local_slot != s->wire_slot)
        {
            int i, merged = 0;
            for (i = 0; i < s->rb_resolved_count; ++i)
            {
                int k = RB_Q_AT(s->rb_resolved_tail, i);
                if (s->rb_resolved_q[k].from == pkt->local_slot)
                {
                    /* A watermark: a reordered older advert must not lower it. */
                    if (rb_epoch_before(s->rb_resolved_q[k].through, pkt->rb_resolved_through))
                        s->rb_resolved_q[k].through = pkt->rb_resolved_through;
                    merged = 1;
                    break;
                }
            }
            if (!merged && s->rb_resolved_count < RNET_RB_CTRL_QUEUE)
            {
                s->rb_resolved_q[s->rb_resolved_head].through = pkt->rb_resolved_through;
                s->rb_resolved_q[s->rb_resolved_head].from = pkt->local_slot;
                s->rb_resolved_head = (s->rb_resolved_head + 1) % RNET_RB_CTRL_QUEUE;
                s->rb_resolved_count++;
            }
            else if (!merged)
            {
                rb_ctrl_note_drop(s, "RB_RESOLVED");
            }
        }
        break;
    default:
        break;
    }
}

static void send_raw(RNetSession *s, const rnet_u8 *buf, int len)
{
    rnet_u8 tagged[RNET_MAX_PACKET];
    if (len >= 14 && rnet_proto_is_rb_control((rnet_u16)(buf[4] | (buf[5] << 8)))) {
        if ((size_t)len + 2 > sizeof(tagged)) return;
        memcpy(tagged, buf, (size_t)len);
        len = rnet_proto_tag_rb_epoch(tagged, (size_t)len, sizeof(tagged), s->input_epoch);
        if (len <= 0) return;
        buf = tagged;
    }

    if ((s == NULL) || (buf == NULL) || (len <= 0))
    {
        return;
    }
    (void)rnet_transport_send(&s->transport, buf, (size_t)len);
}

/* Sender seat of a decoded packet, or -1 when the packet carries none.
 * START has no slot field but only the sim authority (seat 0) sends it;
 * DELAY_SYNC has none and may come from any seat, so it is not attributed. */
static int packet_sender_slot(const RNetDecodedPacket *pkt)
{
    switch (pkt->type)
    {
    case RNET_PKT_START:
        return 0;
    case RNET_PKT_DELAY_SYNC:
        return -1;
    default:
        return (int)pkt->local_slot;
    }
}

static void pump_recv(RNetSession *s)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int n;
    RNetDecodedPacket pkt;
    /* Per pump. The datagrams a pump has to take scale with the room: every
     * seat's INPUT bundle, FRAME_COMMIT and rollback control reach every peer
     * (through a hub, all of them). 64 was sized for 2-4 seats; at 12 a
     * backlog built up behind it and read as 100-300 ms of "link RTT". */
    int guard = state_any_active(s) ? 512 : 32 * (int)s->cfg.slot_count;
    if (guard < 64)
        guard = 64;

    while (guard-- > 0)
    {
        n = rnet_transport_recv(&s->transport, buf, sizeof(buf));
        if (n <= 0)
        {
            break;
        }
        if (rnet_proto_decode(buf, (size_t)n, s->cfg.protocol_magic, &pkt) == 0 &&
            pkt.session_id == s->cfg.session_id)
        {
            int from = packet_sender_slot(&pkt);
            rnet_u64 rx_now;
            /* A seat the table dropped is a ghost: its INPUT rows are still
             * stored (rows below its drop wire are real, and the authority
             * re-serves them in its name), but nothing else it says counts and
             * it does not prove the seat alive. */
            int ghost = from >= 0 && from != (int)s->wire_slot && occ_absent_latest(s, from);
            if (ghost && pkt.type != RNET_PKT_INPUT)
                continue;
            rnet_transport_accept_pending_peer(&s->transport);
            /* Liveness is the seats' business: a seat whose only peer died
             * must not be kept "connected" by spectators' traffic (catch-up
             * requests, their FRAME_COMMITs), and a spectator is alive while
             * the SEATS are -- not while other spectators are. */
            rx_now = session_now(s);
            if (from < (int)s->cfg.slot_count && !ghost)
                s->last_peer_rx_ms = rx_now;
            if (from >= 0 && from < RNET_MAX_SLOTS && from != (int)s->wire_slot && !ghost)
            {
                s->peer_rx_ms[from] = rx_now ? rx_now : 1u;
            }
            s->packets_rx++;
            handle_decoded(s, &pkt);
        }
    }
}

static int state_rx_any_active(const RNetSession *s)
{
    int i;
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
    {
        if (s->rx[i].active)
        {
            return 1;
        }
    }
    return 0;
}

/* 1 while any transfer (outbound or any inbound) is open, ready or not --
 * what the single-transfer code called state_active. */
static int state_any_active(const RNetSession *s)
{
    return s != NULL && (s->tx.active || state_rx_any_active(s));
}

/* 1 while some open transfer has not completed yet. */
static int state_any_in_progress(const RNetSession *s)
{
    int i;
    if (s->tx.active && !s->tx.ready)
    {
        return 1;
    }
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
    {
        if (s->rx[i].active && !s->rx[i].ready)
        {
            return 1;
        }
    }
    return 0;
}

static int state_popcount(rnet_u32 v)
{
    int n = 0;
    while (v != 0u)
    {
        v &= v - 1u;
        n++;
    }
    return n;
}

/* Seats a transfer or probe of `op` from this session must reach.
 *   MEMCARD (guest -> host): the host alone.
 *   anything else (host -> guests): every occupied seat but our own, or
 *   only rb_peer_slot when PSX-Link group scoping is on -- STATE packets
 *   from any other seat are filtered there, so waiting on one would hang.
 * Observers are never counted: they are not seats. */
static rnet_u32 state_default_receivers(const RNetSession *s, rnet_u8 op)
{
    rnet_u32 mask = 0;
    rnet_u8 slot;
    if (op == RNET_STATE_OP_MEMCARD)
    {
        return 1u;
    }
    if (s->rb_peer_slot >= 0)
    {
        if (s->rb_peer_slot < RNET_MAX_SLOTS && s->rb_peer_slot != (int)s->cfg.local_slot &&
            seat_at_launch(s, s->rb_peer_slot))
        {
            return 1u << s->rb_peer_slot;
        }
        return 0;
    }
    for (slot = 0; slot < s->cfg.slot_count && slot < RNET_MAX_SLOTS; ++slot)
    {
        if (slot != s->cfg.local_slot && seat_at_launch(s, slot))
        {
            mask |= 1u << slot;
        }
    }
    return mask;
}

static int state_probe_all_replied(const RNetSession *s)
{
    return (s->state_probe_reply_mask & s->state_probe_expect_mask) == s->state_probe_expect_mask;
}

static void state_probe_clear(RNetSession *s)
{
    if (s == NULL)
    {
        return;
    }
    s->state_probe_active = 0;
    s->state_probe_sender = 0;
    s->state_probe_expect_mask = 0;
    s->state_probe_reply_mask = 0;
    s->state_probe_match_mask = 0;
    s->state_probe_pending = 0;
    s->state_probe_match = 0;
    s->state_probe_op = 0;
    s->state_probe_slot = 0;
    s->state_probe_size = 0;
    s->state_probe_crc = 0;
    s->state_probe_last_tx_ms = 0;
    s->state_probe_parked = 0;
    if (!state_any_active(s))
    {
        s->state_stall_sim = 0;
    }
}

/* ICE AIMD for multi‑MB MotK .pst over TURN. Cold-start used to be 8 KiB / 4
 * chunks (far too shy); AIMD still backs off when juice drops. LAN keeps a
 * wide fixed budget. Sticky cwnd warm-starts the next in-session transfer. */
#define RNET_STATE_ICE_CWND_MIN        (8u * 1024u)
#define RNET_STATE_ICE_CWND_START      (32u * 1024u)
#define RNET_STATE_ICE_CWND_MAX        (256u * 1024u)
#define RNET_STATE_ICE_CHUNKS_MIN      4u
#define RNET_STATE_ICE_CHUNKS_START    16u
#define RNET_STATE_ICE_CHUNKS_MAX      64u
#define RNET_STATE_ICE_AI_CWND         (8u * 1024u)
#define RNET_STATE_ICE_AI_CHUNKS       2u
#define RNET_STATE_ICE_ACK_TO_MIN_MS   40u
#define RNET_STATE_ICE_ACK_TO_START_MS 80u
#define RNET_STATE_ICE_ACK_TO_MAX_MS   180u
#define RNET_STATE_LAN_CWND            (256u * 1024u)
#define RNET_STATE_LAN_CHUNKS          128u
#define RNET_STATE_LAN_ACK_TO_MS       40u
/* Re-ACK pacing for a transfer this receiver already finished (the sender
 * missed the final ACK and is still retransmitting chunks). */
#define RNET_STATE_REACK_MS            20u

static int state_transport_is_ice(const RNetSession *s)
{
    return s != NULL && (s->transport.mode == RNET_TRANSPORT_ICE || s->transport.mode == RNET_TRANSPORT_ICE_HUB);
}

static void state_pacing_reset(RNetSession *s)
{
    if (s == NULL)
        return;
    if (state_transport_is_ice(s))
    {
        rnet_u32 cwnd = RNET_STATE_ICE_CWND_START;
        rnet_u32 chunks = RNET_STATE_ICE_CHUNKS_START;
        if (s->state_sticky_cwnd >= RNET_STATE_ICE_CWND_START)
        {
            cwnd = s->state_sticky_cwnd;
            if (cwnd > RNET_STATE_ICE_CWND_MAX)
                cwnd = RNET_STATE_ICE_CWND_MAX;
        }
        if (s->state_sticky_chunks >= RNET_STATE_ICE_CHUNKS_START)
        {
            chunks = s->state_sticky_chunks;
            if (chunks > RNET_STATE_ICE_CHUNKS_MAX)
                chunks = RNET_STATE_ICE_CHUNKS_MAX;
        }
        s->tx.cwnd = cwnd;
        s->tx.chunks_cap = chunks;
        s->tx.ack_timeout_ms = RNET_STATE_ICE_ACK_TO_START_MS;
    }
    else
    {
        s->tx.cwnd = RNET_STATE_LAN_CWND;
        s->tx.chunks_cap = RNET_STATE_LAN_CHUNKS;
        s->tx.ack_timeout_ms = RNET_STATE_LAN_ACK_TO_MS;
    }
}

static void state_pacing_on_ack_progress(RNetSession *s)
{
    if (s == NULL || !state_transport_is_ice(s))
        return;
    /* Additive increase: +8 KiB and +2 chunks/pump per advancing ACK. */
    if (s->tx.cwnd < RNET_STATE_ICE_CWND_MAX)
    {
        s->tx.cwnd += RNET_STATE_ICE_AI_CWND;
        if (s->tx.cwnd > RNET_STATE_ICE_CWND_MAX)
            s->tx.cwnd = RNET_STATE_ICE_CWND_MAX;
    }
    if (s->tx.chunks_cap < RNET_STATE_ICE_CHUNKS_MAX)
    {
        s->tx.chunks_cap += RNET_STATE_ICE_AI_CHUNKS;
        if (s->tx.chunks_cap > RNET_STATE_ICE_CHUNKS_MAX)
            s->tx.chunks_cap = RNET_STATE_ICE_CHUNKS_MAX;
    }
    if (s->tx.ack_timeout_ms > RNET_STATE_ICE_ACK_TO_MIN_MS)
    {
        s->tx.ack_timeout_ms -= 5u;
        if (s->tx.ack_timeout_ms < RNET_STATE_ICE_ACK_TO_MIN_MS)
            s->tx.ack_timeout_ms = RNET_STATE_ICE_ACK_TO_MIN_MS;
    }
}

static void state_pacing_on_timeout(RNetSession *s)
{
    if (s == NULL || !state_transport_is_ice(s))
        return;
    /* Multiplicative decrease — juice/TURN drop when we outrun the relay. */
    s->tx.cwnd /= 2u;
    if (s->tx.cwnd < RNET_STATE_ICE_CWND_MIN)
        s->tx.cwnd = RNET_STATE_ICE_CWND_MIN;
    s->tx.chunks_cap /= 2u;
    if (s->tx.chunks_cap < RNET_STATE_ICE_CHUNKS_MIN)
        s->tx.chunks_cap = RNET_STATE_ICE_CHUNKS_MIN;
    s->tx.ack_timeout_ms += 20u;
    if (s->tx.ack_timeout_ms > RNET_STATE_ICE_ACK_TO_MAX_MS)
        s->tx.ack_timeout_ms = RNET_STATE_ICE_ACK_TO_MAX_MS;
    /* Decay sticky so the next transfer does not restart at a failed peak. */
    if (s->state_sticky_cwnd > s->tx.cwnd)
        s->state_sticky_cwnd = s->tx.cwnd;
    if (s->state_sticky_chunks > s->tx.chunks_cap)
        s->state_sticky_chunks = s->tx.chunks_cap;
}

static void state_pacing_remember_success(RNetSession *s)
{
    if (s == NULL || !state_transport_is_ice(s))
        return;
    if (s->tx.cwnd > s->state_sticky_cwnd)
        s->state_sticky_cwnd = s->tx.cwnd;
    if (s->tx.chunks_cap > s->state_sticky_chunks)
        s->state_sticky_chunks = s->tx.chunks_cap;
}

/* After one part of the transfer state was cleared: admit stays stalled while
 * any other transfer is open, else it follows the probe (as the
 * single-transfer state_clear did). */
static void state_recompute_stall(RNetSession *s)
{
    s->state_stall_sim = state_any_active(s) ? 1 : (s->state_probe_active ? 1 : 0);
}

static void state_tx_clear(RNetSession *s)
{
    /* A transfer every seat completed is kept (not freed) for spectators:
     * they receive the broadcast passively, and one that came late or lost a
     * chunk after the seats were done has no other way to get it. MEMCARD is
     * a guest upload to the host alone and is never served. */
    if (s->tx.active && s->tx.ready && s->tx.buf != NULL &&
        s->tx.op != RNET_STATE_OP_MEMCARD && !s->is_observer)
    {
        free(s->served_buf);
        s->served_buf = s->tx.buf;
        s->served_id = s->tx.xfer_id;
        s->served_total = s->tx.total;
        s->served_crc = s->tx.crc;
        s->served_op = s->tx.op;
        s->served_slot = s->tx.slot;
        s->tx.buf = NULL;
    }
    free(s->tx.buf);
    memset(&s->tx, 0, sizeof(s->tx));
    if (s->state_taken == RNET_STATE_TAKEN_TX)
    {
        s->state_taken = -1;
    }
}

static void state_rx_clear(RNetSession *s, int src)
{
    RNetStateRx *rx = &s->rx[src];
    free(rx->buf);
    rx->buf = NULL;
    rx->active = 0;
    rx->ready = 0;
    rx->op = 0;
    rx->slot = 0;
    rx->xfer_id = 0;
    rx->total = 0;
    rx->crc = 0;
    rx->contiguity = 0;
    rx->last_ack_ms = 0;
    rx->start_ms = 0;
    memset(rx->bits, 0, sizeof(rx->bits));
    if (s->state_taken == src)
    {
        s->state_taken = -1;
    }
}

/* Clear every transfer, outbound and inbound. */
static void state_clear(RNetSession *s)
{
    int i;
    if (s == NULL)
    {
        return;
    }
    state_tx_clear(s);
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
    {
        state_rx_clear(s, i);
    }
    s->state_taken = -1;
    state_recompute_stall(s);
}

static void state_rx_set_chunk(RNetStateRx *rx, rnet_u32 chunk_index)
{
    if (chunk_index >= RNET_STATE_MAX_CHUNKS)
    {
        return;
    }
    rx->bits[chunk_index >> 3] |= (rnet_u8)(1u << (chunk_index & 7u));
}

static int state_rx_has_chunk(const RNetStateRx *rx, rnet_u32 chunk_index)
{
    if (chunk_index >= RNET_STATE_MAX_CHUNKS)
    {
        return 0;
    }
    return (rx->bits[chunk_index >> 3] >> (chunk_index & 7u)) & 1;
}

static void state_rx_advance_contiguity(RNetStateRx *rx)
{
    rnet_u32 chunks = (rx->total + RNET_STATE_CHUNK_MAX - 1u) / RNET_STATE_CHUNK_MAX;
    rnet_u32 i = rx->contiguity / RNET_STATE_CHUNK_MAX;
    while (i < chunks && state_rx_has_chunk(rx, i))
    {
        rnet_u32 end = (i + 1u) * RNET_STATE_CHUNK_MAX;
        if (end > rx->total)
        {
            end = rx->total;
        }
        rx->contiguity = end;
        i++;
    }
}

static void state_send_ack_raw(RNetSession *s, rnet_u32 xfer_id, rnet_u32 ack_bytes)
{
    rnet_u8 buf[64];
    int n = rnet_proto_encode_state_ack(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->wire_slot,
                                        xfer_id, ack_bytes);
    if (n > 0)
    {
        send_raw(s, buf, n);
    }
}

static void state_rx_send_ack(RNetSession *s, RNetStateRx *rx)
{
    state_send_ack_raw(s, rx->xfer_id, rx->contiguity);
    rx->last_ack_ms = session_now(s);
}

static void state_rx_mark_ready_if_complete(RNetSession *s, RNetStateRx *rx)
{
    rnet_u32 crc;
    if (!rx->active || rx->ready || rx->buf == NULL)
    {
        return;
    }
    if (rx->contiguity < rx->total)
    {
        return;
    }
    crc = rnet_proto_checksum(rx->buf, rx->total);
    if (crc != rx->crc)
    {
        /* Restart receive — ask host to resend from 0 by ACKing 0 after clear. */
        rx->contiguity = 0;
        state_rx_send_ack(s, rx);
        return;
    }
    rx->ready = 1;
    state_rx_send_ack(s, rx);
}

/* Lowest contiguous ACK over the receivers still expected (total when none
 * are left -- every one of them was dropped by the host). */
static rnet_u32 state_tx_min_ack(const RNetSession *s)
{
    rnet_u32 min = s->tx.total;
    int r;
    for (r = 0; r < RNET_MAX_SLOTS; ++r)
    {
        if ((s->tx.expect_mask & (1u << r)) && s->tx.ack[r] < min)
        {
            min = s->tx.ack[r];
        }
    }
    return min;
}

/* Receivers in expect_mask that have ACKed the whole blob. */
static rnet_u32 state_tx_done_mask(const RNetSession *s)
{
    rnet_u32 done = 0;
    int r;
    for (r = 0; r < RNET_MAX_SLOTS; ++r)
    {
        if ((s->tx.expect_mask & (1u << r)) && s->tx.ack[r] >= s->tx.total)
        {
            done |= 1u << r;
        }
    }
    return done;
}

static void state_tx_mark_ready_if_complete(RNetSession *s)
{
    if (!s->tx.active || s->tx.ready || s->tx.buf == NULL)
    {
        return;
    }
    if (state_tx_min_ack(s) >= s->tx.total)
    {
        s->tx.ready = 1;
        state_pacing_remember_success(s);
    }
}

static void state_drive_sender(RNetSession *s)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int n;
    int r;
    rnet_u64 now;
    rnet_u32 window_end;
    rnet_u32 min_ack;
    rnet_u32 sent_this_pump = 0;
    const int is_ice = state_transport_is_ice(s);
    const rnet_u64 kBeginRetransmitMs = is_ice ? 80ULL : 40ULL;
    rnet_u32 cwnd;
    rnet_u32 chunks_cap;
    rnet_u64 ack_timeout_ms;
    int backoff = 0;
    int need_begin = 0;

    if (!s->tx.active || s->tx.ready || s->tx.buf == NULL)
    {
        return;
    }
    if (s->tx.cwnd == 0 || s->tx.chunks_cap == 0)
        state_pacing_reset(s);
    cwnd = s->tx.cwnd;
    chunks_cap = s->tx.chunks_cap;
    ack_timeout_ms = (rnet_u64)s->tx.ack_timeout_ms;
    if (ack_timeout_ms == 0)
        ack_timeout_ms = is_ice ? (rnet_u64)RNET_STATE_ICE_ACK_TO_START_MS
                                : (rnet_u64)RNET_STATE_LAN_ACK_TO_MS;

    now = session_now(s);
    /* Retransmit BEGIN until EVERY expected receiver has ACKed past 0 (saw
     * BEGIN). Stopping at the first one left a receiver that lost BEGIN with
     * nothing to open its receive, so it dropped every chunk. */
    for (r = 0; r < RNET_MAX_SLOTS; ++r)
    {
        if ((s->tx.expect_mask & (1u << r)) && s->tx.ack[r] == 0)
        {
            need_begin = 1;
            break;
        }
    }
    if (need_begin &&
        (s->tx.last_begin_ms == 0 || now - s->tx.last_begin_ms >= kBeginRetransmitMs))
    {
        n = rnet_proto_encode_state_begin(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->wire_slot,
                                          s->tx.op, s->tx.slot, s->tx.xfer_id, s->tx.total, s->tx.crc);
        if (n > 0)
        {
            send_raw(s, buf, n);
        }
        s->tx.last_begin_ms = now;
    }

    min_ack = state_tx_min_ack(s);
    if (min_ack >= s->tx.total)
    {
        state_tx_mark_ready_if_complete(s);
        return;
    }

    /* Per-receiver ACK timeout: a receiver whose ACK has not advanced for
     * ack_timeout_ms rewinds the send cursor to ITS watermark. The timer runs
     * from that receiver's last progress ACK or, before any, from the
     * transfer's start -- a first burst whose chunks overtook BEGIN
     * (reordering) or lost chunk 0 otherwise left the ACK at 0 with nothing
     * ever resent (reproduced with the link simulator at 35 ms +/- 15 ms).
     * Per receiver, not one timer: with several receivers a fast one's
     * progress kept a single timer fresh, and a slow one that lost chunks was
     * never served. Back off (AIMD) when the slowest receiver times out or a
     * rewind actually happened -- a fast receiver that is merely waiting for
     * the window to reach its gap is not loss. */
    for (r = 0; r < RNET_MAX_SLOTS; ++r)
    {
        rnet_u64 base;
        if (!(s->tx.expect_mask & (1u << r)) || s->tx.ack[r] >= s->tx.total)
        {
            continue;
        }
        base = (s->tx.ack_timer_ms[r] != 0) ? s->tx.ack_timer_ms[r] : s->tx.start_ms;
        if (now - base < ack_timeout_ms)
        {
            continue;
        }
        if (s->tx.send_cursor > s->tx.ack[r] || s->tx.ack[r] == min_ack)
        {
            backoff = 1;
        }
        if (s->tx.send_cursor > s->tx.ack[r])
        {
            s->tx.send_cursor = s->tx.ack[r];
        }
        s->tx.ack_timer_ms[r] = now; /* avoid spinning every pump */
    }
    if (backoff)
    {
        state_pacing_on_timeout(s);
        cwnd = s->tx.cwnd;
        chunks_cap = s->tx.chunks_cap;
        ack_timeout_ms = (rnet_u64)s->tx.ack_timeout_ms;
    }

    if (s->tx.send_cursor < min_ack)
    {
        s->tx.send_cursor = min_ack;
    }
    window_end = min_ack + cwnd;
    if (window_end > s->tx.total)
    {
        window_end = s->tx.total;
    }

    while (s->tx.send_cursor < window_end && sent_this_pump < chunks_cap)
    {
        rnet_u32 off = s->tx.send_cursor;
        rnet_u32 left = s->tx.total - off;
        rnet_u16 chunk = (left > RNET_STATE_CHUNK_MAX) ? (rnet_u16)RNET_STATE_CHUNK_MAX : (rnet_u16)left;
        n = rnet_proto_encode_state_chunk(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->wire_slot,
                                          s->tx.xfer_id, off, s->tx.buf + off, chunk);
        if (n <= 0)
        {
            break;
        }
        send_raw(s, buf, n);
        s->tx.send_cursor += chunk;
        sent_this_pump++;
    }
    s->tx.last_tx_ms = now;

    /* Progress log every ~500ms (or on first ACK) — useful on slow TURN paths.
     * "acked" is the slowest receiver's watermark. */
    if (min_ack != s->tx.last_progress_acked || s->tx.last_progress_log_ms == 0 ||
        now - s->tx.last_progress_log_ms >= 500ULL)
    {
        unsigned kib_s = 0;
        char rcv[48];
        int expect_n = state_popcount(s->tx.expect_mask);
        rcv[0] = '\0';
        if (s->tx.start_ms != 0 && now > s->tx.start_ms)
        {
            rnet_u64 elapsed = now - s->tx.start_ms;
            if (elapsed > 0)
                kib_s = (unsigned)((min_ack * 1000ULL) / elapsed / 1024ULL);
        }
        if (expect_n > 1)
        {
            snprintf(rcv, sizeof(rcv), " rcv_done=%d/%d mask=0x%02x", state_popcount(state_tx_done_mask(s)),
                     expect_n, (unsigned)s->tx.expect_mask);
        }
        fprintf(stderr,
                "rnet_state: xfer_id=%u op=%u %u/%u acked (%u KiB/s) cwnd=%u chunks=%u to=%ums%s%s\n",
                (unsigned)s->tx.xfer_id, (unsigned)s->tx.op, (unsigned)min_ack,
                (unsigned)s->tx.total, kib_s, (unsigned)s->tx.cwnd, (unsigned)s->tx.chunks_cap,
                (unsigned)s->tx.ack_timeout_ms, is_ice ? " (ice)" : "", rcv);
        s->tx.last_progress_log_ms = now;
        s->tx.last_progress_acked = min_ack;
    }

    state_tx_mark_ready_if_complete(s);
}

static void state_on_begin(RNetSession *s, const RNetDecodedPacket *pkt)
{
    rnet_u8 src = pkt->local_slot;
    RNetStateRx *rx;
    if (src == s->wire_slot)
    {
        return; /* ignore echo */
    }
    if (pkt->state_op == RNET_STATE_OP_MEMCARD)
    {
        /* Peer→host upload: only the host receives it, and only from a
         * guest. Other guests see the same broadcast and must not open a
         * receive for it, or they would sit on a transfer nobody drives for
         * them (the sender only tracks the host's ACKs). */
        if (s->cfg.local_slot != 0 || src == 0)
        {
            return;
        }
    }
    else if (s->cfg.local_slot == 0)
    {
        return; /* host never receives BEGIN */
    }
    if (pkt->state_total_size == 0 || pkt->state_total_size > RNET_STATE_MAX)
    {
        return;
    }
    if (src >= RNET_MAX_SLOTS || !pkt->state_xfer_id)
        return;
    rx = &s->rx[src];
    if (s->state_received[src].id)
    {
        /* Low 30 bits are the serial number; MEMCARD sets bit 30 as its
         * historical direction namespace. Compare serials modulo that space.
         * A MEMCARD id carries the sender's seat in bits 24..29 and its serial
         * in the low 24, so its serials compare in a 24-bit window (a 30-bit
         * compare would call every BEGIN after the serial wraps stale). */
        const int seat_ids = (pkt->state_xfer_id & 0x40000000u) && (s->state_received[src].id & 0x40000000u);
        const rnet_u32 space = seat_ids ? 0x00ffffffu : 0x3fffffffu;
        const rnet_u32 half = (space >> 1) + 1u;
        rnet_u32 distance = (pkt->state_xfer_id - s->state_received[src].id) & space;
        if (distance == 0 && pkt->state_xfer_id != s->state_received[src].id)
            return; /* same serial, different seat bits: not this sender's */
        if (distance == 0)
        {
            if (pkt->state_xfer_id != s->state_received[src].id ||
                pkt->state_total_size != s->state_received[src].total ||
                pkt->state_payload_crc != s->state_received[src].crc ||
                pkt->state_op != s->state_received[src].op ||
                pkt->state_slot != s->state_received[src].slot)
                return;
            if (s->state_received[src].finished)
                state_send_ack_raw(s, pkt->state_xfer_id, pkt->state_total_size);
            else if (rx->active && rx->xfer_id == pkt->state_xfer_id && rx->buf != NULL)
                state_rx_send_ack(s, rx);
            return;
        }
        if (distance >= half) return; /* stale BEGIN, even during a newer transfer */
    }
    /* Hash-miss path: a transfer from the prober replaces the probe it
     * answered. As a prober we drop our probe only when `src` is the one
     * seat it waits on (the two-seat rule, unchanged); a host whose barrier
     * waits on several seats keeps it while guests upload. */
    if (s->state_probe_active &&
        (!s->state_probe_sender || s->state_probe_expect_mask == (1u << src)))
    {
        state_probe_clear(s);
    }
    /* Supersede: a new transfer from the ONE receiver of our outbound
     * transfer replaces it, exactly as the single-transfer session did -- the
     * peer only starts its own after consuming ours (a guest's MEMCARD
     * receipt after the host's proposal, the host's BOOT after a guest's
     * upload), and hosts rely on the outbound transfer being gone rather
     * than completing (gbarecomp multiplayer_startup.cpp / _checkpoint.cpp).
     * An outbound transfer that still has other receivers keeps running for
     * them; `src` completes it through its ACK / finished re-ACK. */
    if (s->tx.active && s->tx.expect_mask == (1u << src))
    {
        state_tx_clear(s);
    }
    /* A new transfer from `src` replaces only src's receive: another source's
     * receive is untouched. */
    state_rx_clear(s, src);
    rx->buf = (rnet_u8 *)malloc(pkt->state_total_size);
    if (rx->buf == NULL)
    {
        state_recompute_stall(s);
        return;
    }
    memset(rx->buf, 0, pkt->state_total_size);
    rx->active = 1;
    rx->ready = 0;
    s->state_stall_sim = 1;
    rx->op = pkt->state_op;
    rx->slot = pkt->state_slot;
    rx->xfer_id = pkt->state_xfer_id;
    s->state_received[src].id = pkt->state_xfer_id;
    s->state_received[src].total = pkt->state_total_size;
    s->state_received[src].crc = pkt->state_payload_crc;
    s->state_received[src].op = pkt->state_op;
    s->state_received[src].slot = pkt->state_slot;
    s->state_received[src].finished = 0;
    s->state_received[src].last_reack_ms = 0;
    rx->total = pkt->state_total_size;
    rx->crc = pkt->state_payload_crc;
    rx->contiguity = 0;
    rx->start_ms = session_now(s);
    state_rx_send_ack(s, rx);
}

static void state_on_chunk(RNetSession *s, const RNetDecodedPacket *pkt)
{
    rnet_u32 end;
    rnet_u8 src = pkt->local_slot;
    RNetStateRx *rx;
    if (src >= RNET_MAX_SLOTS || src == s->wire_slot)
    {
        return;
    }
    rx = &s->rx[src];
    if (!rx->active || rx->buf == NULL || pkt->state_xfer_id != rx->xfer_id)
    {
        /* A chunk of a transfer we already finished: the sender missed our
         * final ACK and is retransmitting. Answer with the full watermark
         * (paced), or it would retransmit forever. */
        if (s->state_received[src].id != 0 && s->state_received[src].finished &&
            s->state_received[src].id == pkt->state_xfer_id)
        {
            rnet_u64 now = session_now(s);
            if (s->state_received[src].last_reack_ms == 0 ||
                now - s->state_received[src].last_reack_ms >= RNET_STATE_REACK_MS)
            {
                state_send_ack_raw(s, pkt->state_xfer_id, s->state_received[src].total);
                s->state_received[src].last_reack_ms = now ? now : 1u;
            }
        }
        return;
    }
    if (pkt->state_offset > rx->total || pkt->state_chunk_size == 0)
    {
        return;
    }
    end = pkt->state_offset + (rnet_u32)pkt->state_chunk_size;
    if (end > rx->total)
    {
        return;
    }
    memcpy(rx->buf + pkt->state_offset, pkt->state_chunk, pkt->state_chunk_size);
    {
        rnet_u32 chunk_index = pkt->state_offset / RNET_STATE_CHUNK_MAX;
        state_rx_set_chunk(rx, chunk_index);
        state_rx_advance_contiguity(rx);
    }
    {
        rnet_u64 now = session_now(s);
        /* ICE: ACK every chunk so the sender's cwnd can grow without waiting
         * on a 4ms coalesce that fights TURN RTT. LAN keeps light coalescing. */
        if (state_transport_is_ice(s) || now - rx->last_ack_ms >= 4ULL ||
            rx->contiguity >= rx->total)
        {
            state_rx_send_ack(s, rx);
        }
    }
    state_rx_mark_ready_if_complete(s, rx);
}

static void state_on_ack(RNetSession *s, const RNetDecodedPacket *pkt)
{
    rnet_u8 from = pkt->local_slot;
    rnet_u32 min_before;
    rnet_u32 ack;
    if (!s->tx.active)
    {
        return;
    }
    if (pkt->state_xfer_id != s->tx.xfer_id)
    {
        return;
    }
    /* Only an expected receiver's ACK counts. The single-peer sender took the
     * max over ANY ACK, so with several receivers the fastest one completed
     * the transfer for all of them (and an observer's ACK could too). */
    if (from >= RNET_MAX_SLOTS || !(s->tx.expect_mask & (1u << from)))
    {
        return;
    }
    ack = pkt->state_ack_bytes;
    if (ack > s->tx.total)
    {
        ack = s->tx.total;
    }
    if (ack > s->tx.ack[from])
    {
        rnet_u64 now = session_now(s);
        min_before = state_tx_min_ack(s);
        s->tx.ack[from] = ack;
        s->tx.ack_timer_ms[from] = now ? now : 1u;
        s->tx.last_tx_ms = 0; /* send next chunk immediately */
        /* Grow the window only when the SLOWEST receiver advances: the window
         * is anchored on it, and N receivers each ACKing must not grow cwnd N
         * times as fast. With one receiver every advance is the slowest's. */
        if (state_tx_min_ack(s) > min_before)
        {
            state_pacing_on_ack_progress(s);
        }
    }
    state_tx_mark_ready_if_complete(s);
}

/* ------------------------------------------------------------------------ */
/* Spectator catch-up (wire version 3)                                       */
/*                                                                           */
/* A spectator follows confirmed rows only, so it can neither predict past a */
/* lost datagram nor be helped by the seats' own recovery: seats retransmit */
/* from each other's INPUT acknowledgments, and a spectator sends no INPUT. */
/* Before this it depended on three things it had no way to ask for again:  */
/* START (sent once, re-sent to seats only), the tip-redundancy window of   */
/* each INPUT bundle (3 rows), and the broadcast of the host's boot-time   */
/* transfer, mod set and tick-0 commit. OBS_CATCHUP asks for all of them;   */
/* each seat answers for what it owns.                                      */
/* ------------------------------------------------------------------------ */

#define RNET_OBS_CATCHUP_PERIOD_MS 250u /* status while nothing is missing */
#define RNET_OBS_URGENT_PERIOD_MS 30u   /* while a row is missing */
#define RNET_OBS_HOLE_GRACE_MS 20u      /* reordering under jitter, not loss */
#define RNET_OBS_SILENT_MS 150u         /* no row at all from a seat */
#define RNET_OBS_PREFETCH 32u           /* ask this far ahead of the sim */
#define RNET_OBS_PREFETCH_LOG 88u       /* ... with a full log behind the seats (ring is 128) */
#define RNET_OBS_SERVE_ROWS 63u         /* rows one answer carries (3 packets) */
#define RNET_OBS_SERVE_CHUNKS 8u        /* transfer chunks one answer carries */
/* Per-spectator serving budget. The pacing above only throttles a REPEATED
 * request, so a peer varying `need` or its ack got a full answer (3 row
 * packets, 8 chunks) per ~90-byte request, fanned out by a hub. Honest
 * catch-up peaks near 500 packets/s (chunks every 25 ms plus rows every
 * 20 ms); the bucket allows 800/s with a burst of 64. */
#define RNET_OBS_SERVE_PPS 800u
#define RNET_OBS_SERVE_BURST 64u

static int obs_wire_is_spectator(const RNetSession *s, rnet_u8 wire)
{
    return wire >= s->cfg.slot_count;
}

/* Per-spectator pacing for the answers a seat sends. 16 entries, LRU by
 * nothing cleverer than "first free, else the slot the wire id hashes to":
 * a hub holds at most 8 peers, a relay's gallery is pacing-only state. */
static int obs_serve_index(RNetSession *s, rnet_u8 wire)
{
    int i;
    int n = (int)(sizeof(s->obs_serve) / sizeof(s->obs_serve[0]));
    for (i = 0; i < n; ++i)
        if (s->obs_serve[i].used && s->obs_serve[i].wire == wire)
            return i;
    for (i = 0; i < n; ++i)
        if (!s->obs_serve[i].used)
            break;
    if (i == n)
        i = wire % n;
    memset(&s->obs_serve[i], 0, sizeof(s->obs_serve[i]));
    s->obs_serve[i].used = 1;
    s->obs_serve[i].wire = wire;
    {
        int k;
        for (k = 0; k < RNET_MAX_SLOTS; ++k)
            s->obs_serve[i].rows_need[k] = RNET_OBS_NEED_NONE;
    }
    s->obs_serve[i].budget_mp = RNET_OBS_SERVE_BURST * 1000u;
    return i;
}

/* Take up to `want` packets from spectator e's budget; returns how many. */
static unsigned obs_serve_take(RNetSession *s, int e, rnet_u64 now, unsigned want)
{
    rnet_u64 elapsed;
    rnet_u64 mp;
    unsigned got;
    elapsed = (s->obs_serve[e].budget_ms != 0 && now > s->obs_serve[e].budget_ms)
                  ? now - s->obs_serve[e].budget_ms
                  : 0u;
    if (s->obs_serve[e].budget_ms == 0 || now > s->obs_serve[e].budget_ms)
        s->obs_serve[e].budget_ms = now ? now : 1u;
    mp = (rnet_u64)s->obs_serve[e].budget_mp + elapsed * RNET_OBS_SERVE_PPS;
    if (mp > (rnet_u64)RNET_OBS_SERVE_BURST * 1000u)
        mp = (rnet_u64)RNET_OBS_SERVE_BURST * 1000u;
    got = (unsigned)(mp / 1000u);
    if (got > want)
        got = want;
    s->obs_serve[e].budget_mp = (rnet_u32)(mp - (rnet_u64)got * 1000u);
    return got;
}

static void obs_send_refuse(RNetSession *s, rnet_u8 target, rnet_u8 reason, rnet_u32 tick,
                            rnet_u32 oldest)
{
    rnet_u8 buf[64];
    int n = rnet_proto_encode_obs_refuse(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                         s->wire_slot, target, reason, tick, oldest);
    if (n > 0)
        send_raw(s, buf, n);
}

static int obs_popcount_ready(const RNetSession *s)
{
    int i, n = 0;
    for (i = 0; i < 32; ++i)
    {
        rnet_u8 b = s->obs_ready_seen[i];
        while (b)
        {
            b &= (rnet_u8)(b - 1u);
            n++;
        }
    }
    return n;
}

/* Authority: a spectator's READY. Before START it counts toward the roster;
 * after, it is a spectator that never saw START -- it bound at the hub after
 * START went out, or START was lost -- and nothing else would ever start it.
 * Answer START again while a replay from tick 0 can still be served, refuse
 * loudly after. Paced by the spectator's own READY (100 ms). */
static void obs_on_ready(RNetSession *s, rnet_u8 wire)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int len;
    if (!s->is_sim_authority || !obs_wire_is_spectator(s, wire))
        return;
    s->obs_ready_seen[wire >> 3] |= (rnet_u8)(1u << (wire & 7u));
    if (!s->start_sent || s->phase != RNET_PHASE_RUNNING)
        return;
    if (s->sim_tick <= RNET_OBS_JOIN_MAX_TICK || s->cfg.late_join)
    {
        len = rnet_proto_encode_start(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, 0);
        send_raw(s, buf, len);
        return;
    }
    {
        int e = obs_serve_index(s, wire);
        rnet_u64 now = session_now(s);
        if (s->obs_serve[e].refuse_ms == 0 || now - s->obs_serve[e].refuse_ms >= 200u)
        {
            s->obs_serve[e].refuse_ms = now ? now : 1u;
            fprintf(stderr,
                    "recomp-net: spectator (wire slot %u) asked to start at tick %u -- past the "
                    "join window (%u); refused (late join needs a state transfer)\n",
                    (unsigned)wire, (unsigned)s->sim_tick, (unsigned)RNET_OBS_JOIN_MAX_TICK);
            obs_send_refuse(s, wire, RNET_OBS_REFUSE_LATE_JOIN, s->sim_tick, 0u);
        }
    }
}

/* Authority: may START go out yet, as far as rostered spectators go? */
static int obs_roster_ready(RNetSession *s, rnet_u64 now)
{
    int have;
    if (s->cfg.expected_observers == 0)
        return 1;
    have = obs_popcount_ready(s);
    if (have >= (int)s->cfg.expected_observers)
    {
        if (s->roster_wait_logged)
            fprintf(stderr, "recomp-net: all %u rostered spectator(s) ready -- starting\n",
                    (unsigned)s->cfg.expected_observers);
        return 1;
    }
    if (s->seats_ready_since_ms == 0)
        s->seats_ready_since_ms = now ? now : 1u;
    if (!s->roster_wait_logged)
    {
        s->roster_wait_logged = 1;
        fprintf(stderr,
                "recomp-net: every seat is ready; holding START for %d more rostered "
                "spectator(s) (up to %u ms)\n",
                (int)s->cfg.expected_observers - have, (unsigned)RNET_OBS_ROSTER_WAIT_MS);
    }
    if (now - s->seats_ready_since_ms >= RNET_OBS_ROSTER_WAIT_MS)
    {
        fprintf(stderr,
                "recomp-net: %d of %u rostered spectator(s) never sent READY within %u ms -- "
                "starting without them (they can still join until tick %u)\n",
                (int)s->cfg.expected_observers - have, (unsigned)s->cfg.expected_observers,
                (unsigned)RNET_OBS_ROSTER_WAIT_MS, (unsigned)RNET_OBS_JOIN_MAX_TICK);
        return 1;
    }
    return 0;
}

/* INPUT bundles of `source`'s rows [lo, hi] out of the full log, named as that
 * seat. A seat serving its own rows carries its real acknowledgments; the
 * authority speaking for another seat (a dropped one, or one whose earlier
 * occupant left) carries none -- the packet is data for a spectator, not an
 * ack for anybody. Returns the number of packets sent. */
static int emit_log_rows(RNetSession *s, int source, rnet_u32 lo, rnet_u32 hi)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    RNetWireFrame frames[RNET_MAX_BUNDLE];
    rnet_u32 ack_vector[RNET_MAX_SLOTS];
    rnet_u32 ack, t;
    rnet_u8 slot;
    int sent = 0;
    int own = source == (int)s->cfg.local_slot;
    for (slot = 0; slot < RNET_MAX_SLOTS; ++slot)
        ack_vector[slot] = own && slot < s->cfg.slot_count ? s->remote_contiguous_ack[slot] : 0xffffffffu;
    ack = own ? rnet_ring_highest_valid(&s->remote_rings[(s->cfg.local_slot + 1) % s->cfg.slot_count])
              : 0xffffffffu;
    t = lo;
    while (t <= hi)
    {
        int count = 0, len;
        while (t <= hi && count < RNET_MAX_BUNDLE)
        {
            rnet_u8 bytes[RNET_LOG_ROW_BYTES];
            rnet_u16 size = 0;
            if (rlog_get(&s->row_log[source], t, bytes, &size))
            {
                frames[count].tick = t;
                frames[count].size = size;
                memcpy(frames[count].bytes, bytes, RNET_LOG_ROW_BYTES);
                count++;
            }
            if (t == 0xffffffffu)
                break;
            t++;
        }
        if (count == 0)
            continue;
        len = rnet_proto_encode_input_acks(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                           (rnet_u8)source, s->input_epoch, ack, frames, count,
                                           ack_vector, s->cfg.slot_count);
        if (len < 0)
            break;
        send_raw(s, buf, len);
        s->input_bundle_sends++;
        sent++;
    }
    return sent;
}

/* 1 when this seat answers a spectator's need for `source`'s rows at `need`:
 * its own always; as the authority also the rows nobody else can serve (the
 * seat dropped, or an earlier occupant's rows below the current one's wire). */
static int obs_serves_source(const RNetSession *s, int source, rnet_u32 need)
{
    if (source == (int)s->cfg.local_slot)
        return 1;
    if (!s->cfg.late_join || !s->is_sim_authority || source < 0 || source >= (int)s->cfg.slot_count)
        return 0;
    return occ_absent_latest(s, source) || need < occ_own_from(s, source);
}

/* Seat: serve the spectator the rows of seat `source` from `need` on. */
static void obs_serve_rows(RNetSession *s, int e, rnet_u8 wire, int source, rnet_u32 need, rnet_u64 now)
{
    RNetInputSample sample;
    rnet_u32 tip, hi;
    int use_log = s->cfg.late_join && !s->row_log_off;
    if (s->input_send_suppress || s->state_stall_sim)
        return; /* a load is being applied: the ring is about to be replaced */
    if (use_log)
    {
        rnet_u8 bytes[RNET_LOG_ROW_BYTES];
        rnet_u16 size;
        const RNetRowLog *l = &s->row_log[source];
        if (l->hi == 0u || need >= l->hi)
            return; /* not produced yet: the live INPUT will carry it */
        tip = l->hi - 1u;
        if (!rlog_get(l, need, bytes, &size))
        {
            /* A hole (never held, e.g. an absent range): nothing to say. Said
             * once a second so a spectator stuck on it is not a silent stall. */
            if (s->obs_serve[e].refuse_ms == 0 || now - s->obs_serve[e].refuse_ms >= 1000u)
            {
                s->obs_serve[e].refuse_ms = now ? now : 1u;
                fprintf(stderr, "recomp-net: spectator (wire slot %u) needs seat %d row %u; the log has "
                                "no such row (it holds up to %u)\n",
                        (unsigned)wire, source, (unsigned)need, (unsigned)l->hi);
            }
            return;
        }
    }
    else
    {
        if (source != (int)s->cfg.local_slot)
            return;
        tip = rnet_ring_highest_valid(&s->local_ring);
        if (!rnet_ring_get(&s->local_ring, tip, &sample) || need > tip)
            return; /* not produced yet: the live INPUT will carry it */
        if (!rnet_ring_get(&s->local_ring, need, &sample))
        {
            /* Aged out of the history (or a previous epoch). Nothing on the wire
             * can bring it back: say so instead of leaving the spectator stalled. */
            if (s->obs_serve[e].refuse_ms == 0 || now - s->obs_serve[e].refuse_ms >= 200u)
            {
                rnet_u32 oldest = tip >= RNET_HISTORY_LENGTH - 1u ? tip - (RNET_HISTORY_LENGTH - 1u) : 0u;
                s->obs_serve[e].refuse_ms = now ? now : 1u;
                fprintf(stderr,
                        "recomp-net: spectator (wire slot %u) needs seat %u row %u; history now "
                        "starts at %u -- refused\n",
                        (unsigned)wire, (unsigned)s->cfg.local_slot, (unsigned)need, (unsigned)oldest);
                obs_send_refuse(s, wire, RNET_OBS_REFUSE_ROWS_GONE, need, oldest);
            }
            return;
        }
    }
    if (s->obs_serve[e].rows_need[source] == need && now - s->obs_serve[e].rows_ms[source] < 20u)
        return;
    hi = (tip - need > RNET_OBS_SERVE_ROWS) ? need + RNET_OBS_SERVE_ROWS : tip;
    /* One INPUT packet per bundle of rows; charge the answer up front. */
    if (obs_serve_take(s, e, now, 3u) < 3u)
        return;
    s->obs_serve[e].rows_need[source] = need;
    s->obs_serve[e].rows_ms[source] = now ? now : 1u;
    s->obs_rows_served += hi - need + 1u;
    if (use_log)
        (void)emit_log_rows(s, source, need, hi);
    else
        (void)emit_local_rows(s, need, hi);
}

static void obs_send_begin(RNetSession *s, rnet_u8 op, rnet_u8 slot, rnet_u32 id, rnet_u32 total,
                           rnet_u32 crc)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int n = rnet_proto_encode_state_begin(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                          s->wire_slot, op, slot, id, total, crc);
    if (n > 0)
        send_raw(s, buf, n);
}

/* Seat: a spectator's OBS_CATCHUP. */
static void obs_on_catchup(RNetSession *s, const RNetDecodedPacket *pkt)
{
    rnet_u8 wire = pkt->local_slot;
    rnet_u64 now;
    int e;
    if (s->is_observer || !obs_wire_is_spectator(s, wire) || s->phase != RNET_PHASE_RUNNING ||
        pkt->input_epoch != s->input_epoch)
        return;
    now = session_now(s);
    e = obs_serve_index(s, wire);
    if (pkt->ack_count == s->cfg.slot_count)
    {
        int src;
        for (src = 0; src < (int)pkt->ack_count && src < RNET_MAX_SLOTS; ++src)
        {
            if (pkt->acks[src] != RNET_OBS_NEED_NONE && obs_serves_source(s, src, pkt->acks[src]))
                obs_serve_rows(s, e, wire, src, pkt->acks[src], now);
        }
    }
    /* Our tick-0 FRAME_COMMIT: the spectator's boot-digest gate waits on it
     * (and times out after seconds without it). */
    if (s->commit0_valid && s->cfg.local_slot < 16u &&
        !(pkt->obs_commit0_mask & (1u << s->cfg.local_slot)) &&
        (s->obs_serve[e].commit0_ms == 0 || now - s->obs_serve[e].commit0_ms >= 100u))
    {
        rnet_u8 buf[RNET_MAX_PACKET];
        int n = rnet_proto_encode_rb_frame_commit(buf, sizeof(buf), s->cfg.protocol_magic,
                                                  s->cfg.session_id, s->wire_slot, 0u, s->commit0_hash);
        s->obs_serve[e].commit0_ms = now ? now : 1u;
        if (n > 0)
            send_raw(s, buf, n);
    }
    if (!s->is_sim_authority)
        return;
    if (s->modset_sent_valid && !(pkt->obs_flags & RNET_OBS_FLAG_HAVE_MODSET) &&
        (s->obs_serve[e].modset_ms == 0 || now - s->obs_serve[e].modset_ms >= 100u))
    {
        rnet_u8 buf[RNET_MAX_PACKET];
        int n = rnet_proto_encode_modset(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                         s->wire_slot, s->modset_sent_text);
        s->obs_serve[e].modset_ms = now ? now : 1u;
        if (n > 0)
            rnet_transport_send(&s->transport, buf, (size_t)n);
    }
    /* The current (seats still receiving) or last completed transfer, if the
     * spectator does not have it: BEGIN opens its receive, its ACKs then pull
     * the chunks (obs_on_state_ack). */
    if (s->obs_serve[e].begin_ms == 0 || now - s->obs_serve[e].begin_ms >= 100u)
    {
        if (s->tx.active && s->tx.buf != NULL && s->tx.op != RNET_STATE_OP_MEMCARD)
        {
            if (pkt->obs_host_xfer_done != s->tx.xfer_id)
            {
                s->obs_serve[e].begin_ms = now ? now : 1u;
                obs_send_begin(s, s->tx.op, s->tx.slot, s->tx.xfer_id, s->tx.total, s->tx.crc);
            }
        }
        else if (s->served_buf != NULL && pkt->obs_host_xfer_done != s->served_id)
        {
            s->obs_serve[e].begin_ms = now ? now : 1u;
            obs_send_begin(s, s->served_op, s->served_slot, s->served_id, s->served_total,
                           s->served_crc);
        }
    }
}

/* Seat: a spectator's STATE_ACK. The sender's transfer logic counts seats
 * only (a spectator must never hold up, or complete, a transfer for them),
 * so it is served here, from the open transfer or the retained one. */
static void obs_on_state_ack(RNetSession *s, const RNetDecodedPacket *pkt)
{
    const rnet_u8 *blob = NULL;
    rnet_u32 total = 0, off;
    rnet_u64 now;
    unsigned sent = 0, limit;
    int e;
    if (s->tx.active && s->tx.buf != NULL && s->tx.xfer_id == pkt->state_xfer_id &&
        s->tx.op != RNET_STATE_OP_MEMCARD)
    {
        blob = s->tx.buf;
        total = s->tx.total;
    }
    else if (s->served_buf != NULL && s->served_id == pkt->state_xfer_id)
    {
        blob = s->served_buf;
        total = s->served_total;
    }
    if (blob == NULL || pkt->state_ack_bytes >= total)
        return;
    now = session_now(s);
    e = obs_serve_index(s, pkt->local_slot);
    if (s->obs_serve[e].state_ack == pkt->state_ack_bytes && s->obs_serve[e].state_ms != 0 &&
        now - s->obs_serve[e].state_ms < 25u)
        return;
    s->obs_serve[e].state_ack = pkt->state_ack_bytes;
    s->obs_serve[e].state_ms = now ? now : 1u;
    /* Chunks are aligned to RNET_STATE_CHUNK_MAX; the ACK is the receiver's
     * contiguous watermark, i.e. a chunk boundary (or total). */
    off = (pkt->state_ack_bytes / RNET_STATE_CHUNK_MAX) * RNET_STATE_CHUNK_MAX;
    {
        unsigned want = (total - off + RNET_STATE_CHUNK_MAX - 1u) / RNET_STATE_CHUNK_MAX;
        unsigned allow;
        if (want > RNET_OBS_SERVE_CHUNKS)
            want = RNET_OBS_SERVE_CHUNKS;
        allow = obs_serve_take(s, e, now, want);
        if (allow == 0)
            return;
        limit = allow;
    }
    while (off < total && sent < limit)
    {
        rnet_u8 buf[RNET_MAX_PACKET];
        rnet_u32 left = total - off;
        rnet_u16 chunk = (left > RNET_STATE_CHUNK_MAX) ? (rnet_u16)RNET_STATE_CHUNK_MAX : (rnet_u16)left;
        int n = rnet_proto_encode_state_chunk(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                              s->wire_slot, pkt->state_xfer_id, off, blob + off, chunk);
        if (n <= 0)
            break;
        send_raw(s, buf, n);
        off += chunk;
        sent++;
    }
    s->obs_chunks_served += sent;
}

/* Spectator: a seat refused us. Latched; the host reads it and leaves. */
static void obs_on_refuse(RNetSession *s, const RNetDecodedPacket *pkt)
{
    /* Only a seat may refuse a spectator; another spectator must not be
     * able to end someone else's match. */
    if (!s->is_observer || pkt->obs_target_slot != s->wire_slot || s->obs_refused ||
        pkt->local_slot >= s->cfg.slot_count)
        return;
    s->obs_refused = 1;
    s->obs_refuse_reason = pkt->obs_reason;
    s->obs_refuse_from = pkt->local_slot;
    s->obs_refuse_tick = pkt->obs_tick;
    s->obs_refuse_oldest = pkt->obs_oldest;
    if (pkt->obs_reason == RNET_OBS_REFUSE_LATE_JOIN)
        fprintf(stderr,
                "recomp-net: SPECTATOR REFUSED -- the match is already at tick %u; a spectator "
                "replays it from tick 0 out of the seats' input history and can join only until "
                "tick %u. Joining later needs a state transfer (a snapshot of a confirmed tick "
                "plus the rows after it), which this build does not have.\n",
                (unsigned)pkt->obs_tick, (unsigned)RNET_OBS_JOIN_MAX_TICK);
    else
        fprintf(stderr,
                "recomp-net: SPECTATOR REFUSED -- seat %u no longer holds row %u (its history "
                "starts at %u): this spectator fell more than %u ticks behind the seats and "
                "cannot continue without a state transfer.\n",
                (unsigned)pkt->local_slot, (unsigned)pkt->obs_tick, (unsigned)pkt->obs_oldest,
                (unsigned)RNET_HISTORY_LENGTH);
}

/* Spectator, every pump while RUNNING: ask for what is missing. */
static void obs_pump(RNetSession *s)
{
    rnet_u32 need[RNET_MAX_SLOTS];
    rnet_u32 horizon;
    rnet_u64 now;
    rnet_u8 slot, flags;
    rnet_u32 host_xfer;
    int urgent = 0;
    int i;
    if (!s->is_observer || s->phase != RNET_PHASE_RUNNING || s->obs_refused)
        return;
    now = session_now(s);
    /* An inbound transfer with a hole: the sender's own retransmit follows
     * the seats' ACKs and stops once they are done. Re-ACK our watermark so
     * the sender serves the rest (obs_on_state_ack). */
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
    {
        RNetStateRx *rx = &s->rx[i];
        if (rx->active && !rx->ready && rx->buf != NULL && now - rx->last_ack_ms >= 50u)
            state_rx_send_ack(s, rx);
    }
    /* A replay from tick 0 of a long match is paced by round trips: with the
     * full log behind the seats, ask as far ahead as the ring accepts. */
    horizon = s->sim_tick + (rnet_u32)s->delay +
              (s->cfg.late_join ? RNET_OBS_PREFETCH_LOG : RNET_OBS_PREFETCH);
    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        rnet_u32 n;
        rnet_u64 age;
        need[slot] = RNET_OBS_NEED_NONE;
        if (!rnet_config_slot_occupied(&s->cfg, slot))
            continue;
        contig_advance(s, slot);
        n = s->remote_contiguous_ack[slot] + 1u; /* UINT32_MAX + 1 = tick 0 */
        if (occ_absent_at(s, slot, n))
            continue; /* synthesized from here: nothing to ask for */
        if (n != s->obs_last_need[slot] || s->obs_need_since_ms[slot] == 0)
        {
            s->obs_last_need[slot] = n;
            s->obs_need_since_ms[slot] = now ? now : 1u;
        }
        if (n > horizon)
            continue;
        age = now - s->obs_need_since_ms[slot];
        if ((s->obs_seen_valid[slot] && s->obs_seen_tip[slot] >= n && age >= RNET_OBS_HOLE_GRACE_MS) ||
            age >= RNET_OBS_SILENT_MS)
        {
            need[slot] = n;
            urgent = 1;
        }
    }
    if (urgent ? (now - s->obs_last_urgent_ms < RNET_OBS_URGENT_PERIOD_MS)
               : (s->obs_last_catchup_ms != 0 &&
                  now - s->obs_last_catchup_ms < RNET_OBS_CATCHUP_PERIOD_MS))
        return;
    flags = RNET_OBS_FLAG_RUNNING;
    if (s->modset_rx_seen)
        flags |= RNET_OBS_FLAG_HAVE_MODSET;
    host_xfer = 0;
    if (s->rx[0].active)
        host_xfer = s->rx[0].xfer_id;
    else if (s->state_received[0].finished)
        host_xfer = s->state_received[0].id;
    {
        rnet_u8 buf[RNET_MAX_PACKET];
        int n = rnet_proto_encode_obs_catchup(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                              s->wire_slot, s->input_epoch, flags, host_xfer,
                                              s->obs_commit0_mask, need, s->cfg.slot_count);
        if (n > 0)
        {
            send_raw(s, buf, n);
            s->obs_catchups_sent++;
        }
    }
    s->obs_last_catchup_ms = now ? now : 1u;
    if (urgent)
        s->obs_last_urgent_ms = now ? now : 1u;
}

/* The PROBE replied mask is 16 bits on the wire (version 4): one bit per seat. */
typedef char rnet_probe_replied_mask_fits_u16[(RNET_MAX_SLOTS <= 16) ? 1 : -1];

static void state_drive_probe(RNetSession *s)
{
    rnet_u8 buf[64];
    int n;
    rnet_u64 now;

    if (s->state_probe_active && !s->state_probe_sender && s->state_probe_parked)
    {
        /* A lost STATE_PROBE_DONE must not park this seat for good: repeat
         * the matching reply; the host answers it with DONE again. */
        now = session_now(s);
        if (s->state_probe_last_tx_ms == 0 || now - s->state_probe_last_tx_ms >= 50ULL)
        {
            n = rnet_proto_encode_state_probe_reply(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                                    s->wire_slot, s->state_probe_op, s->state_probe_slot, 1u,
                                                    s->state_probe_size, s->state_probe_crc);
            if (n > 0)
            {
                send_raw(s, buf, n);
                s->state_probe_last_tx_ms = now;
            }
        }
        return;
    }
    if (!s->state_probe_active || !s->state_probe_sender || state_probe_all_replied(s))
    {
        return;
    }
    now = session_now(s);
    /* Ready/hash probes: snappy retransmit — 40ms left a visible post-load hitch. */
    if (s->state_probe_last_tx_ms != 0 && now - s->state_probe_last_tx_ms < 8ULL)
    {
        return;
    }
    /* The replied mask tells seats that already answered to ignore this
     * retransmit (0 while nobody has -- the only value a two-seat host ever
     * sends, since it stops at the first reply). */
    n = rnet_proto_encode_state_probe_ex(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                         s->wire_slot, s->state_probe_op, s->state_probe_slot,
                                         s->state_probe_size, s->state_probe_crc,
                                         (rnet_u16)(s->state_probe_reply_mask & 0xffffu));
    if (n > 0)
    {
        send_raw(s, buf, n);
        s->state_probe_last_tx_ms = now;
    }
}

static void state_on_probe(RNetSession *s, const RNetDecodedPacket *pkt)
{
    if (pkt->local_slot == s->wire_slot)
    {
        return;
    }
    if (s->cfg.local_slot == 0)
    {
        return; /* host never receives PROBE */
    }
    /* The prober already holds our reply to this probe: it is retransmitting
     * for a slower seat. Re-raising it would hand a LOAD/BOOT ready probe we
     * already answered (and cleared) back to the app as a fresh one. */
    if (s->wire_slot < 16u && (pkt->state_probe_replied & (1u << s->wire_slot)))
    {
        return;
    }
    if (state_any_active(s))
    {
        return; /* transfer in flight takes precedence */
    }
    /* Retransmit of a probe we already answered — resend REPLY, do not re-arm. */
    if (s->state_probe_active && !s->state_probe_sender && !s->state_probe_pending &&
        s->state_probe_op == pkt->state_op && s->state_probe_slot == pkt->state_slot &&
        s->state_probe_size == pkt->state_total_size && s->state_probe_crc == pkt->state_payload_crc)
    {
        rnet_u8 buf[64];
        int n = rnet_proto_encode_state_probe_reply(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                                    s->wire_slot, s->state_probe_op, s->state_probe_slot,
                                                    s->state_probe_match ? 1u : 0u, s->state_probe_size,
                                                    s->state_probe_crc);
        if (n > 0)
        {
            send_raw(s, buf, n);
        }
        return;
    }
    /* Fresh probe — surface to the app. size==0 (coord save) must not stall
     * admit or deferred savestate_poll never runs (deadlock). */
    s->state_probe_active = 1;
    s->state_probe_sender = 0;
    s->state_probe_pending = 1;
    s->state_probe_expect_mask = 0;
    s->state_probe_reply_mask = 0;
    s->state_probe_match_mask = 0;
    s->state_probe_match = 0;
    s->state_probe_op = pkt->state_op;
    s->state_probe_slot = pkt->state_slot;
    s->state_probe_size = pkt->state_total_size;
    s->state_probe_crc = pkt->state_payload_crc;
    s->state_stall_sim = (pkt->state_total_size != 0) ? 1 : 0;
}

static void state_on_probe_reply(RNetSession *s, const RNetDecodedPacket *pkt)
{
    rnet_u8 from = pkt->local_slot;
    if (!s->state_probe_active || !s->state_probe_sender)
    {
        if (s->state_probe_released_valid && pkt->state_probe_match && from != s->wire_slot &&
            pkt->state_op == s->state_probe_released_op && pkt->state_slot == s->state_probe_released_slot &&
            pkt->state_total_size == s->state_probe_released_size &&
            pkt->state_payload_crc == s->state_probe_released_crc)
        {
            rnet_u8 buf[64];
            int n = rnet_proto_encode_state_probe_done(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                                       s->wire_slot, s->state_probe_released_op,
                                                       s->state_probe_released_slot, s->state_probe_released_size,
                                                       s->state_probe_released_crc);
            if (n > 0)
            {
                send_raw(s, buf, n);
            }
        }
        return;
    }
    if (from == s->wire_slot)
    {
        return;
    }
    /* Bind to the active probe generation. SAVE coord (size=0,crc=target)
     * and LOAD ready (size=0,crc=ready) share op/slot with the following
     * hash probe — a late ACK must not satisfy the next probe. */
    if (pkt->state_op != s->state_probe_op || pkt->state_slot != s->state_probe_slot ||
        pkt->state_total_size != s->state_probe_size || pkt->state_payload_crc != s->state_probe_crc)
    {
        return;
    }
    /* Per seat: every expected seat must answer; one seat's reply never
     * stands in for another's. Latest answer per seat wins. */
    if (from >= RNET_MAX_SLOTS || !(s->state_probe_expect_mask & (1u << from)))
    {
        return;
    }
    s->state_probe_reply_mask |= 1u << from;
    if (pkt->state_probe_match)
    {
        s->state_probe_match_mask |= 1u << from;
    }
    else
    {
        s->state_probe_match_mask &= ~(1u << from);
    }
}

/* Guest: the host heard "match" from every expected seat for this probe
 * generation; resume. A LOAD/BOOT match never parks, so DONE only releases
 * SAVE/SRAM/RB_KF. */
static void state_on_probe_done(RNetSession *s, const RNetDecodedPacket *pkt)
{
    if (s->cfg.local_slot == 0 || pkt->local_slot == s->wire_slot || !s->state_probe_active ||
        s->state_probe_sender || !s->state_probe_parked || pkt->state_op != s->state_probe_op ||
        pkt->state_slot != s->state_probe_slot || pkt->state_total_size != s->state_probe_size ||
        pkt->state_payload_crc != s->state_probe_crc)
    {
        return;
    }
    state_probe_clear(s);
}

/* Host: release every parked guest once all expected seats matched. */
static void state_send_probe_done(RNetSession *s)
{
    rnet_u8 buf[64];
    int n;
    if (!s->state_probe_active || !s->state_probe_sender || s->state_probe_size == 0 ||
        !state_probe_all_replied(s) ||
        (s->state_probe_match_mask & s->state_probe_expect_mask) != s->state_probe_expect_mask)
    {
        return;
    }
    n = rnet_proto_encode_state_probe_done(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                           s->wire_slot, s->state_probe_op, s->state_probe_slot,
                                           s->state_probe_size, s->state_probe_crc);
    if (n > 0)
    {
        send_raw(s, buf, n);
    }
    s->state_probe_released_valid = 1;
    s->state_probe_released_op = s->state_probe_op;
    s->state_probe_released_slot = s->state_probe_slot;
    s->state_probe_released_size = s->state_probe_size;
    s->state_probe_released_crc = s->state_probe_crc;
}

static void maybe_bootstrap(RNetSession *s)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int len;
    rnet_u64 now = session_now(s);
    rnet_u8 all_ready = 1;
    rnet_u8 slot;

    if (s->phase == RNET_PHASE_LINKING)
    {
        if (now - s->last_hello_ms >= 100ULL)
        {
            len = rnet_proto_encode_hello(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->wire_slot,
                                          s->cfg.slot_count, s->delay);
            send_raw(s, buf, len);
            s->last_hello_ms = now;
        }
    }

    if (s->phase == RNET_PHASE_READY || s->phase == RNET_PHASE_LINKING)
    {
        if (!s->local_ready)
        {
            s->local_ready = 1;
        }
        if (now - s->last_ready_ms >= 100ULL)
        {
            len = rnet_proto_encode_ready(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->wire_slot);
            send_raw(s, buf, len);
            s->last_ready_ms = now;
        }
        /* Mark our own seat ready. An observer has no seat to mark -- and
         * local_slot is the sentinel, which would index one past the array
         * when the room is full. It still waits for every seat below. */
        if (!s->is_observer)
        {
            s->peer_ready[s->cfg.local_slot] = 1;
        }
        for (slot = 0; slot < s->cfg.slot_count; ++slot)
        {
            if (!seat_at_launch(s, slot))
            {
                s->peer_ready[slot] = 1; /* empty or open seat — no READY expected */
                continue;
            }
            if (!s->peer_ready[slot])
            {
                all_ready = 0;
                break;
            }
        }
        if (all_ready && s->is_sim_authority && !s->start_sent && obs_roster_ready(s, now))
        {
            len = rnet_proto_encode_start(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, 0);
            send_raw(s, buf, len);
            s->start_sent = 1;
            s->sim_tick = 0;
            s->phase = RNET_PHASE_RUNNING;
            seed_delay_prefix(s);
            send_input_bundle(s);
        }
    }
}

/* INPUT bundles carrying this seat's own rows [lo, hi] from the local ring,
 * with the usual acknowledgments. Returns the number of packets sent. Shared
 * by the tip/retransmit path and by the spectator catch-up service, so a
 * served row is byte-for-byte the INPUT a seat would have sent anyway. */
static int emit_local_rows(RNetSession *s, rnet_u32 lo, rnet_u32 hi)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    RNetWireFrame frames[RNET_MAX_BUNDLE];
    rnet_u32 ack;
    rnet_u32 ack_vector[RNET_MAX_SLOTS];
    rnet_u32 t;
    rnet_u8 slot;
    int sent = 0;

    /* Which peer's progress to acknowledge. Seated peers point at the next
     * seat round; an observer has no "next" seat from a seat of its own, and
     * the sentinel would pick a ring by accident of arithmetic. Seat 0 is the
     * sim authority and the one peer guaranteed present, so an observer acks
     * against that. */
    ack = rnet_ring_highest_valid(
        &s->remote_rings[s->is_observer
                             ? 0
                             : ((s->cfg.local_slot + 1) % s->cfg.slot_count)]);
    for (slot = 0; slot < s->cfg.slot_count; ++slot)
        ack_vector[slot] = s->remote_contiguous_ack[slot];
    t = lo;
    while (t <= hi)
    {
        int count = 0;
        int len;
        while (t <= hi && count < RNET_MAX_BUNDLE)
        {
            RNetInputSample sample;
            if (rnet_ring_get(&s->local_ring, t, &sample))
            {
                frames[count].tick = sample.tick;
                frames[count].size = sample.size;
                memcpy(frames[count].bytes, sample.bytes, sample.size);
                count++;
            }
            if (t == 0xffffffffu)
            {
                break;
            }
            t++;
        }
        if (count == 0)
        {
            continue;
        }
        if (s->cfg.slot_count > 2)
            len = rnet_proto_encode_input_acks(buf, sizeof(buf), s->cfg.protocol_magic,
                                               s->cfg.session_id, s->wire_slot, s->input_epoch,
                                               ack, frames, count, ack_vector, s->cfg.slot_count);
        else
            len = rnet_proto_encode_input(buf, sizeof(buf), s->cfg.protocol_magic,
                                          s->cfg.session_id, s->wire_slot, s->input_epoch, ack,
                                          frames, count);
        if (len < 0)
        {
            break;
        }
        send_raw(s, buf, len);
        s->input_bundle_sends++;
        sent++;
    }
    return sent;
}

static void send_input_bundle(RNetSession *s)
{
    int red = (int)s->cfg.bundle_redundancy;
    rnet_u32 tip;
    rnet_u32 lo;
    rnet_u8 slot;
    int sent_any = 0;
    rnet_u64 now = session_now(s);

    /* The one choke point for "a spectator sends no input".
     *
     * Guarded here rather than at each of the seven call sites -- several are
     * stall paths that retransmit, and a spectator's local_ring is empty
     * anyway, so those would send empty bundles forever. One guard cannot be
     * forgotten when an eighth call site is added. */
    if (s->is_observer)
    {
        return;
    }

    if (s->phase != RNET_PHASE_RUNNING)
    {
        return;
    }
    /* Stall or explicit suppress: do not emit pre-resync tips during load. */
    if (s->state_stall_sim || s->input_send_suppress)
    {
        return;
    }
    tip = rnet_wire_tick_from_sim(s->sim_tick, s->delay);
    /* A new future tip is latency-sensitive and sends immediately. Repeated
     * pumps for the same tip are reliability retransmits, not new data. */
    if (s->last_input_tip_valid && s->last_input_tip == tip &&
        now - s->last_input_ms < 8ULL)
    {
        return;
    }
    if (red < 1)
    {
        red = 1;
    }
    /* Startup must carry the complete neutral delay prefix. Otherwise delays
     * larger than one INPUT packet can never admit tick zero. Do not clamp
     * this window to RNET_MAX_BUNDLE — emit multiple packets instead. */
    if (red < (int)s->delay + 1)
    {
        red = (int)s->delay + 1;
    }
    lo = (tip + 1U > (rnet_u32)red) ? (tip + 1U - (rnet_u32)red) : 0U;
    /* Peer ACK of our tips: resend from ack+1 when they fell behind the
     * tip-redundancy window (WAN/TURN loss). Cap so one pump cannot emit
     * the whole 128-slot history. */
    if (s->cfg.slot_count > 2)
    {
        rnet_u32 ack_lo = tip + 1u;
        rnet_u32 max_back = (rnet_u32)RNET_HISTORY_LENGTH / 2u;
        if (max_back < 32u) max_back = 32u;
        if (max_back > 64u) max_back = 64u;
        for (slot = 0; slot < s->cfg.slot_count; ++slot)
        {
            rnet_u32 peer_lo;
            if (slot == s->cfg.local_slot || !rnet_config_slot_occupied(&s->cfg, slot) ||
                occ_absent_latest(s, slot) ||
                (occ_absent_now(s, slot) && !s->peer_ack_seen[slot]))
                continue;
            peer_lo = (!s->peer_ack_seen[slot] || s->peer_ack_tick[slot] == 0xffffffffu)
                          ? 0u : s->peer_ack_tick[slot] + 1u;
            if (peer_lo < ack_lo) ack_lo = peer_lo;
        }
        if (tip + 1u > max_back && ack_lo + max_back <= tip)
            ack_lo = tip + 1u - max_back;
        if (ack_lo < lo) lo = ack_lo;
    }
    else if (s->highest_remote_ack < tip)
    {
        rnet_u32 ack_lo = s->highest_remote_ack + 1U;
        rnet_u32 max_back = (rnet_u32)RNET_HISTORY_LENGTH / 2U;
        if (max_back < 32U)
            max_back = 32U;
        if (max_back > 64U)
            max_back = 64U;
        if (tip + 1U > max_back && ack_lo + max_back <= tip)
            ack_lo = tip + 1U - max_back;
        if (ack_lo < lo)
            lo = ack_lo;
    }
    sent_any = emit_local_rows(s, lo, tip);
    if (!sent_any)
    {
        return;
    }
    if (!s->last_input_tip_valid || s->last_input_tip != tip)
    {
        /* First send of this tip: the RTT clock starts here. */
        s->tip_sent_tick[tip % RNET_HISTORY_LENGTH] = tip;
        s->tip_sent_ms[tip % RNET_HISTORY_LENGTH] = now ? now : 1u;
    }
    s->last_input_ms = now;
    s->last_input_tip = tip;
    s->last_input_tip_valid = 1;
}

/* Seat `slot`'s ack first covered our tip `ack`: one RTT sample for that
 * seat. Each seat samples a tip at most once (link_last_ack), so every seat
 * of a room measures its own path. */
static void link_rtt_note_ack(RNetSession *s, rnet_u8 slot, rnet_u32 ack)
{
    rnet_u32 idx = ack % RNET_HISTORY_LENGTH;
    rnet_u64 now, sent;
    rnet_u32 sample, dev;
    if (slot >= RNET_MAX_SLOTS || ack == 0xffffffffu || s->tip_sent_tick[idx] != ack ||
        s->tip_sent_ms[idx] == 0u)
        return;
    if (s->link_samples[slot] != 0u && ack <= s->link_last_ack[slot])
        return; /* one sample per tip and seat */
    s->link_last_ack[slot] = ack;
    sent = s->tip_sent_ms[idx];
    now = session_now(s);
    if (now < sent || now - sent > 2000u)
        return;
    sample = (rnet_u32)(now - sent);
    if (s->link_samples[slot] == 0u)
    {
        s->link_rtt_x16[slot] = sample * 16u;
        s->link_jitter_x16[slot] = 0u;
        s->link_rtt_min[slot] = sample;
    }
    else
    {
        rnet_u32 rtt = s->link_rtt_x16[slot] / 16u;
        dev = sample > rtt ? sample - rtt : rtt - sample;
        s->link_rtt_x16[slot] = (s->link_rtt_x16[slot] * 15u + sample * 16u) / 16u;
        /* Jitter: fast up, slow down -- a spike must widen D at once, a calm
         * stretch should only narrow it gradually. */
        if (dev * 16u > s->link_jitter_x16[slot])
            s->link_jitter_x16[slot] = (s->link_jitter_x16[slot] + dev * 16u) / 2u;
        else
            s->link_jitter_x16[slot] = (s->link_jitter_x16[slot] * 31u + dev * 16u) / 32u;
        if (sample < s->link_rtt_min[slot])
            s->link_rtt_min[slot] = sample;
    }
    s->link_samples[slot]++;
}

static int remotes_ready_for_play_wire(RNetSession *s, rnet_u32 play_wire)
{
    rnet_u8 slot;
    RNetInputSample tmp;

    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        if (slot == s->cfg.local_slot)
        {
            continue;
        }
        if (!rnet_config_slot_occupied(&s->cfg, slot))
        {
            continue; /* empty lobby seat — local neutral, not on the wire */
        }
        if (!rnet_ring_get(&s->remote_rings[slot], play_wire, &tmp))
        {
            return 0;
        }
    }
    return 1;
}

/* Gameplay ticks 0..D-1 have no prior human sample. Seed them with a
 * deterministic neutral row; fresh input sampled at sim T is stored at T+D. */
static void seed_delay_prefix(RNetSession *s)
{
    rnet_u32 t;
    if (s == NULL) return;
    for (t = 0; t < (rnet_u32)s->delay; ++t)
    {
        RNetInputSample sample;
        if (rnet_ring_get(&s->local_ring, t, &sample)) continue;
        memset(&sample, 0, sizeof(sample));
        sample.tick = t;
        sample.valid = 1;
        local_ring_put(s, &sample);
    }
}

static int collect_wire_inputs(RNetSession *s, rnet_u32 wire,
                               RNetInputSample *resolved)
{
    rnet_u8 slot;
    if (s == NULL || resolved == NULL) return 0;
    memset(resolved, 0, sizeof(RNetInputSample) * RNET_MAX_SLOTS);
    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        RNetInputSample sample;
        int found;
        if (!rnet_config_slot_occupied(&s->cfg, slot))
        {
            memset(&sample, 0, sizeof(sample));
            sample.tick = wire;
            sample.valid = 1;
            resolved[slot] = sample;
            continue;
        }
        found = slot == s->cfg.local_slot
            ? rnet_ring_get(&s->local_ring, wire, &sample)
            : rnet_ring_get(&s->remote_rings[slot], wire, &sample);
        if (!found) return 0;
        resolved[slot] = sample;
        resolved[slot].tick = wire;
    }
    return 1;
}

/* Resolve and advertise a wire row as soon as both peers' inputs exist. The
 * row may be D frames in the future; that lead time absorbs confirmation RTT. */
static int prepare_wire_confirm(RNetSession *s, rnet_u32 wire, int force_send)
{
    RNetInputSample resolved[RNET_MAX_SLOTS];
    rnet_u32 index = wire % RNET_HISTORY_LENGTH;
    rnet_u32 hash;
    rnet_u64 now;
    rnet_u8 slot;

    if (!collect_wire_inputs(s, wire, resolved)) return 0;
    hash = hash_resolved_inputs(wire, resolved, (int)s->cfg.slot_count);
    s->published_tick[index] = wire;
    s->published_hash[index] = hash;
    s->published_valid[index] = 1;

    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        if (slot == s->cfg.local_slot) continue;
        if (!rnet_config_slot_occupied(&s->cfg, slot)) continue;
        if (s->peer_history_valid[index][slot] &&
            s->peer_history_tick[index][slot] == wire &&
            s->peer_history_hash[index][slot] != hash)
        {
            s->input_desync = 1;
            s->desync_tick = wire;
            s->desync_local_hash = hash;
            s->desync_remote_hash = s->peer_history_hash[index][slot];
            return 0;
        }
    }

    now = session_now(s);
    if (force_send || s->confirm_last_sent_ms[index] == 0 ||
        now - s->confirm_last_sent_ms[index] >= 4ULL)
        send_input_confirm_tick(s, wire, hash);
    return 1;
}

static int wire_confirmations_agree(RNetSession *s, rnet_u32 wire)
{
    rnet_u32 index = wire % RNET_HISTORY_LENGTH;
    rnet_u8 slot;
    if (!s->published_valid[index] || s->published_tick[index] != wire)
        return 0;
    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        if (slot == s->cfg.local_slot) continue;
        if (!rnet_config_slot_occupied(&s->cfg, slot)) continue;
        if (!s->peer_history_valid[index][slot] ||
            s->peer_history_tick[index][slot] != wire)
            return 0;
        if (s->peer_history_hash[index][slot] != s->published_hash[index])
            return 0;
    }
    return 1;
}

/* Retransmit confirms for [play - trail, tip], not only [play, tip].
 * Otherwise a lost INPUT_CONFIRM for an already-admitted tick leaves the
 * slower peer in wait_confirm forever while the faster peer runs ahead
 * (spam rematch over ICE: guest@19 wait_confirm, host@26 wait_remote).
 * Prefer the cached published hash so trailing ticks still retransmit after
 * rings have advanced past the original inputs. */
static void refresh_confirm_window(RNetSession *s, rnet_u32 play_wire, rnet_u32 sample_wire)
{
    rnet_u32 trail;
    rnet_u32 lo;
    rnet_u32 w;
    rnet_u64 now;

    if (s == NULL)
        return;
    trail = (rnet_u32)s->delay + (rnet_u32)s->cfg.bundle_redundancy + 8u;
    if (trail < 16u)
        trail = 16u;
    if (trail > (rnet_u32)RNET_HISTORY_LENGTH / 2u)
        trail = (rnet_u32)RNET_HISTORY_LENGTH / 2u;
    lo = (play_wire > trail) ? (play_wire - trail) : 0u;
    if (sample_wire < lo)
        sample_wire = lo;
    now = session_now(s);
    for (w = lo; w <= sample_wire; ++w)
    {
        rnet_u32 index = w % RNET_HISTORY_LENGTH;
        if (s->published_valid[index] && s->published_tick[index] == w)
        {
            if (s->confirm_last_sent_ms[index] == 0 ||
                now - s->confirm_last_sent_ms[index] >= 4ULL)
                send_input_confirm_tick(s, w, s->published_hash[index]);
        }
        else
        {
            (void)prepare_wire_confirm(s, w, 0);
        }
        if (w == 0xffffffffu)
            break;
    }
}

RNetSession *rnet_session_create(const RNetConfig *cfg, const RNetHostVTable *host)
{
    RNetSession *s;
    rnet_u8 i;
    if ((cfg == NULL) || (host == NULL) || (host->sample_local == NULL) || (host->publish == NULL))
    {
        return NULL;
    }
    if (cfg->slot_count < 2 || cfg->slot_count > RNET_MAX_SLOTS)
    {
        return NULL;
    }
    /* `slot_count` exactly is the observer sentinel; above it is a seat that
     * does not exist and stays an error, so a typo cannot quietly become a
     * session that waits on nobody. */
    if (cfg->local_slot > cfg->slot_count)
    {
        return NULL;
    }
    /* An observer must carry a wire id outside the seat range. Sending as a
     * player slot would have the relay forward its packets and the peers
     * store them as that seat's input -- the exact thing a spectator must not
     * be able to do. Refuse rather than silently send as a player. */
    if (cfg->local_slot == cfg->slot_count &&
        rnet_config_wire_slot(cfg) < cfg->slot_count)
    {
        return NULL;
    }
    s = (RNetSession *)calloc(1, sizeof(*s));
    if (s == NULL)
    {
        return NULL;
    }
    s->cfg = *cfg;
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
        s->remote_contiguous_ack[i] = 0xffffffffu;
    s->host = *host;
    s->delay = cfg->input_delay;
    s->phase = RNET_PHASE_IDLE;
    s->is_observer = rnet_config_is_observer(cfg) ? 1 : 0;
    s->wire_slot = rnet_config_wire_slot(cfg);
    /* An observer is never authority: its local_slot is the sentinel, never
     * 0, so this already answers no -- stated rather than relied upon. */
    s->is_sim_authority = (!s->is_observer && cfg->local_slot == 0) ? 1 : 0;
    s->rb_peer_slot = -1;
    s->rb_last_from = -1;
    s->state_taken = -1;
    s->session_start_ms = rnet_os_monotonic_ms();
    s->last_peer_rx_ms = 0;
    s->peer_gone = 0;
    rnet_transport_init(&s->transport);
    rnet_ring_clear(&s->local_ring);
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
    {
        rnet_ring_clear(&s->remote_rings[i]);
    }
    occ_seed_open(s);
    return s;
}

void rnet_session_destroy(RNetSession *s)
{
    if (s == NULL)
    {
        return;
    }
    state_clear(s);
    free(s->served_buf);
    s->served_buf = NULL;
    {
        int k;
        for (k = 0; k < RNET_MAX_SLOTS; ++k)
            rlog_free(&s->row_log[k]);
    }
    rnet_transport_shutdown(&s->transport);
    rnet_ice_agent_destroy(s->ice);
    s->ice = NULL;
    {
        int i;
        for (i = 0; i < RNET_HUB_MAX_PEERS; ++i)
        {
            rnet_ice_agent_destroy(s->ice_hub[i]);
            s->ice_hub[i] = NULL;
        }
    }
    free(s);
}

int rnet_session_start_lan(RNetSession *s, const char *bind_hostport, const char *peer_hostport)
{
    if (s == NULL)
    {
        return -1;
    }
    if (rnet_transport_start_lan(&s->transport, bind_hostport, peer_hostport) != 0)
    {
        return -1;
    }
    s->phase = RNET_PHASE_LINKING;
    s->last_hello_ms = 0;
    return 0;
}

int rnet_session_start_lan_hub(RNetSession *s, const char *bind_hostport)
{
    if (s == NULL)
    {
        return -1;
    }
    /* Transport fan-out hub (lobby owner). Sim local_slot is independent —
     * seats may be reordered; guests still dial this peer's endpoint. */
    if (rnet_transport_start_lan_hub(&s->transport, bind_hostport) != 0)
    {
        return -1;
    }
    s->phase = RNET_PHASE_LINKING;
    s->last_hello_ms = 0;
    return 0;
}

int rnet_session_start_ice(RNetSession *s, const RNetIceConfig *ice)
{
#if !defined(RNET_ENABLE_ICE)
    (void)s;
    (void)ice;
    return -1;
#else
    RNetIceConfig local;
    if ((s == NULL) || (ice == NULL))
    {
        return -1;
    }
    local = *ice;
    if (s->ice != NULL)
    {
        rnet_ice_agent_destroy(s->ice);
        s->ice = NULL;
    }
    s->ice = rnet_ice_agent_create(&local, ice_emit_bridge, s);
    if (s->ice == NULL)
    {
        return -1;
    }
    rnet_transport_shutdown(&s->transport);
    rnet_transport_init(&s->transport);
    s->transport.mode = RNET_TRANSPORT_ICE;
    s->transport.ice_send = ice_send_bridge;
    s->transport.ice_recv = ice_recv_bridge;
    s->transport.ice_ctx = s->ice;
    s->ice_attempt_ms = session_now(s);
    s->ice_completed_ms = 0;
    if (rnet_ice_agent_start_gathering(s->ice) != 0)
    {
        return -1;
    }
    /* Stay IDLE until ICE completes; pump will promote to LINKING. */
    s->phase = RNET_PHASE_IDLE;
    return 0;
#endif
}

int rnet_session_start_ice_hub(RNetSession *s, const RNetIceConfig *ice, rnet_u32 guest_slot_mask)
{
#if !defined(RNET_ENABLE_ICE)
    (void)s;
    (void)ice;
    (void)guest_slot_mask;
    return -1;
#else
    RNetIceConfig local;
    void *ctx[RNET_HUB_MAX_PEERS];
    int i;
    int any = 0;

    if ((s == NULL) || (ice == NULL))
    {
        return -1;
    }
    /* Guest seats are 1..RNET_HUB_MAX_PEERS-1; bit 0 and out-of-range bits invalid. */
    if ((guest_slot_mask & 1u) != 0u ||
        guest_slot_mask == 0u)
    {
        return -1;
    }
    local = *ice;
    local.bind_port = 0;   /* never a fixed port: N agents on one port collide */
    local.controlling = 0; /* host answers; guests offer */

    if (s->ice != NULL)
    {
        rnet_ice_agent_destroy(s->ice);
        s->ice = NULL;
    }
    for (i = 0; i < RNET_HUB_MAX_PEERS; ++i)
    {
        rnet_ice_agent_destroy(s->ice_hub[i]);
        s->ice_hub[i] = NULL;
        ctx[i] = NULL;
    }
    s->ice_hub_mask = 0;
    rnet_transport_shutdown(&s->transport);
    rnet_transport_init(&s->transport);

    for (i = 1; i < RNET_HUB_MAX_PEERS; ++i)
    {
        if ((guest_slot_mask & (1u << i)) == 0u)
        {
            continue;
        }
        s->ice_hub_bridge[i].s = s;
        s->ice_hub_bridge[i].slot = i;
        s->ice_hub[i] = rnet_ice_agent_create(&local, ice_hub_emit_bridge, &s->ice_hub_bridge[i]);
        if (s->ice_hub[i] == NULL)
        {
            goto fail;
        }
        ctx[i] = s->ice_hub[i];
        any = 1;
    }
    if (!any || rnet_transport_start_ice_hub(&s->transport, ice_send_bridge, ice_recv_bridge, ctx) != 0)
    {
        goto fail;
    }
    s->ice_hub_mask = guest_slot_mask;
    s->ice_attempt_ms = session_now(s);
    s->ice_completed_ms = 0;
    for (i = 1; i < RNET_HUB_MAX_PEERS; ++i)
    {
        /* Answerer: records gather_pending; gathers on the guest's offer. */
        if (s->ice_hub[i] != NULL && rnet_ice_agent_start_gathering(s->ice_hub[i]) != 0)
        {
            goto fail;
        }
    }
    s->phase = RNET_PHASE_IDLE;
    return 0;
fail:
    for (i = 0; i < RNET_HUB_MAX_PEERS; ++i)
    {
        rnet_ice_agent_destroy(s->ice_hub[i]);
        s->ice_hub[i] = NULL;
    }
    s->ice_hub_mask = 0;
    rnet_transport_shutdown(&s->transport);
    rnet_transport_init(&s->transport);
    return -1;
#endif
}

/* Tear down whatever transport the session had, so an adopted agent starts
 * from a clean slate. */
static void session_ice_reset_transport(RNetSession *s)
{
    int i;
    if (s->ice != NULL)
    {
        rnet_ice_agent_destroy(s->ice);
        s->ice = NULL;
    }
    for (i = 0; i < RNET_HUB_MAX_PEERS; ++i)
    {
        rnet_ice_agent_destroy(s->ice_hub[i]);
        s->ice_hub[i] = NULL;
    }
    s->ice_hub_mask = 0;
    rnet_transport_shutdown(&s->transport);
    rnet_transport_init(&s->transport);
}

/* Hub identity of a joiner: it arrived as a spectator on the hub index (the
 * wire id the lobby gave it) and, once it holds seat `to`, speaks as `to`.
 * The hub checks every datagram's named seat against the agent / address it
 * came in on, so the binding moves with the grant (host side only). */
int rnet_session_hub_rebind(RNetSession *s, int from, int to)
{
    if (s == NULL || from < 1 || to < 1 || from == to || from >= RNET_HUB_MAX_PEERS ||
        to >= RNET_HUB_MAX_PEERS)
        return 0;
#if defined(RNET_ENABLE_ICE)
    if (s->ice_hub[from] != NULL && s->ice_hub[to] != NULL)
    {
        /* A previous occupant of the seat left its agent behind. */
        s->transport.hub_ice_ctx[to] = NULL;
        rnet_ice_agent_destroy(s->ice_hub[to]);
        s->ice_hub[to] = NULL;
        s->ice_hub_mask &= ~(1u << to);
    }
    if (s->ice_hub[from] != NULL && s->ice_hub[to] == NULL)
    {
        s->ice_hub[to] = s->ice_hub[from];
        s->ice_hub[from] = NULL;
        s->ice_hub_bridge[to].s = s;
        s->ice_hub_bridge[to].slot = to;
        rnet_ice_agent_set_emit(s->ice_hub[to], ice_hub_emit_bridge, &s->ice_hub_bridge[to]);
        s->ice_hub_mask = (s->ice_hub_mask & ~(1u << from)) | (1u << to);
    }
#endif
    return rnet_transport_hub_rebind(&s->transport, from, to);
}

/* Host: a peer that connected while the match runs (a late spectator) gets
 * its own hub index. The agent must be COMPLETED; the session owns it after. */
int rnet_session_hub_add_agent(RNetSession *s, int wire_slot, RNetIceAgent *agent)
{
#if !defined(RNET_ENABLE_ICE)
    (void)s; (void)wire_slot; (void)agent;
    return -1;
#else
    if (s == NULL || agent == NULL || wire_slot < 1 || wire_slot >= RNET_HUB_MAX_PEERS ||
        s->transport.mode != RNET_TRANSPORT_ICE_HUB ||
        rnet_ice_agent_state(agent) != RNET_ICE_STATE_COMPLETED)
        return -1;
    if (s->ice_hub[wire_slot] != NULL)
    {
        /* The same gallery index again: its earlier occupant left (or was
         * granted a seat and moved to another index). */
        s->transport.hub_ice_ctx[wire_slot] = NULL;
        rnet_ice_agent_destroy(s->ice_hub[wire_slot]);
        s->ice_hub[wire_slot] = NULL;
        s->ice_hub_mask &= ~(1u << wire_slot);
        s->transport.hub_slot_known[wire_slot] = 0;
    }
    s->ice_hub[wire_slot] = agent;
    s->ice_hub_bridge[wire_slot].s = s;
    s->ice_hub_bridge[wire_slot].slot = wire_slot;
    rnet_ice_agent_set_emit(agent, ice_hub_emit_bridge, &s->ice_hub_bridge[wire_slot]);
    rnet_ice_agent_freeze(agent);
    s->ice_hub_mask |= 1u << wire_slot;
    return rnet_transport_hub_add_ctx(&s->transport, wire_slot, agent);
#endif
}

int rnet_session_start_ice_hub_adopt(RNetSession *s, const RNetIceAdoptSeat *seats, int n)
{
#if !defined(RNET_ENABLE_ICE)
    (void)s;
    (void)seats;
    (void)n;
    return -1;
#else
    void *ctx[RNET_HUB_MAX_PEERS];
    rnet_u32 mask = 0;
    int i;

    if ((s == NULL) || (seats == NULL) || n < 1 || n >= RNET_HUB_MAX_PEERS)
    {
        return -1;
    }
    /* Validate everything before taking anything: on -1 the caller still owns
     * every agent. The mask is built only from agents that are COMPLETED -- a
     * seat that is not connected fails the start instead of shrinking the room. */
    for (i = 0; i < n; ++i)
    {
        const int slot = seats[i].slot;
        if (seats[i].agent == NULL || slot < 1 || slot >= RNET_HUB_MAX_PEERS ||
            (mask & (1u << slot)) != 0u ||
            rnet_ice_agent_state(seats[i].agent) != RNET_ICE_STATE_COMPLETED)
        {
            return -1;
        }
        mask |= (1u << slot);
    }
    memset(ctx, 0, sizeof(ctx));
    session_ice_reset_transport(s);
    for (i = 0; i < n; ++i)
    {
        const int slot = seats[i].slot;
        s->ice_hub[slot] = seats[i].agent;
        s->ice_hub_bridge[slot].s = s;
        s->ice_hub_bridge[slot].slot = slot;
        rnet_ice_agent_set_emit(seats[i].agent, ice_hub_emit_bridge, &s->ice_hub_bridge[slot]);
        rnet_ice_agent_freeze(seats[i].agent);
        ctx[slot] = seats[i].agent;
    }
    if (rnet_transport_start_ice_hub(&s->transport, ice_send_bridge, ice_recv_bridge, ctx) != 0)
    {
        /* Not reachable for a non-empty ctx; still, hand ownership back. */
        for (i = 0; i < n; ++i)
            s->ice_hub[seats[i].slot] = NULL;
        return -1;
    }
    s->ice_hub_mask = mask;
    /* Dead-path timers measure from adoption, not from the waiting room. */
    s->ice_attempt_ms = session_now(s);
    s->ice_completed_ms = 0;
    s->last_peer_rx_ms = 0ULL;
    s->phase = RNET_PHASE_IDLE;
    return 0;
#endif
}

int rnet_session_adopt_ice_agent(RNetSession *s, RNetIceAgent *agent)
{
#if !defined(RNET_ENABLE_ICE)
    (void)s;
    (void)agent;
    return -1;
#else
    if ((s == NULL) || (agent == NULL) ||
        rnet_ice_agent_state(agent) != RNET_ICE_STATE_COMPLETED)
    {
        return -1;
    }
    session_ice_reset_transport(s);
    s->ice = agent;
    rnet_ice_agent_set_emit(agent, ice_emit_bridge, s);
    rnet_ice_agent_freeze(agent);
    s->transport.mode = RNET_TRANSPORT_ICE;
    s->transport.ice_send = ice_send_bridge;
    s->transport.ice_recv = ice_recv_bridge;
    s->transport.ice_ctx = agent;
    s->ice_attempt_ms = session_now(s);
    s->ice_completed_ms = 0;
    s->last_peer_rx_ms = 0ULL;
    s->phase = RNET_PHASE_IDLE;
    return 0;
#endif
}

#if defined(RNET_ENABLE_ICE)
/* After STUN/host ICE fails or stalls, one automatic gather with force_relay
 * when TURN credentials are present. Opt out: RNET_ICE_NO_RELAY_FALLBACK=1.
 *
 * Timers (env overrides):
 *   RNET_ICE_RELAY_FALLBACK_MS — general stall (default 5000, was 12000)
 *   RNET_ICE_RELAY_PRIVATE_MS  — early fallback when every remote so far is
 *                                RFC1918 (default 2500); host/STUN cannot
 *                                reach CGNAT LAN candidates
 *   RNET_ICE_RELAY_DEAD_MS     — completed non-relay but no session RX
 */
static void session_maybe_ice_relay_fallback(RNetSession *s)
{
    RNetIceState st;
    rnet_u64 now;
    rnet_u64 elapsed;
    rnet_u64 stuck_ms = 5000ULL;
    rnet_u64 private_ms = 2500ULL;
    rnet_u64 dead_ms = 6000ULL;
    const char *env;
    char path[32];
    int only_private;

    if (s == NULL || s->ice == NULL || s->phase == RNET_PHASE_RUNNING)
        return;
    if (rnet_ice_agent_relay_fallback_done(s->ice) || rnet_ice_agent_is_force_relay(s->ice))
        return;
    if (!rnet_ice_agent_has_turn(s->ice))
        return;
    env = getenv("RNET_ICE_NO_RELAY_FALLBACK");
    if (env != NULL && env[0] != '\0' && env[0] != '0')
        return;
    env = getenv("RNET_ICE_RELAY_FALLBACK_MS");
    if (env != NULL && env[0] != '\0')
    {
        long v = strtol(env, NULL, 10);
        if (v >= 2000L && v <= 60000L)
            stuck_ms = (rnet_u64)v;
    }
    env = getenv("RNET_ICE_RELAY_PRIVATE_MS");
    if (env != NULL && env[0] != '\0')
    {
        long v = strtol(env, NULL, 10);
        if (v >= 1000L && v <= 30000L)
            private_ms = (rnet_u64)v;
    }
    env = getenv("RNET_ICE_RELAY_DEAD_MS");
    if (env != NULL && env[0] != '\0')
    {
        long v = strtol(env, NULL, 10);
        if (v >= 2000L && v <= 30000L)
            dead_ms = (rnet_u64)v;
    }
    if (private_ms > stuck_ms)
        private_ms = stuck_ms;

    now = session_now(s);
    elapsed = (s->ice_attempt_ms != 0ULL && now >= s->ice_attempt_ms)
                  ? (now - s->ice_attempt_ms)
                  : 0ULL;
    st = rnet_ice_agent_state(s->ice);
    only_private = rnet_ice_agent_remote_only_private(s->ice);

    if (st == RNET_ICE_STATE_FAILED)
        goto do_fallback;
    if (st != RNET_ICE_STATE_CONNECTED && st != RNET_ICE_STATE_COMPLETED)
    {
        /* CGNAT / internet: only private host remotes will never connect via
         * host/STUN — cut over to force_relay without waiting the full stall. */
        if (only_private && elapsed >= private_ms)
            goto do_fallback;
        if (elapsed >= stuck_ms)
            goto do_fallback;
        return;
    }
    /* Completed on a non-relay path but no session packets yet — flaky CGNAT. */
    if (s->last_peer_rx_ms == 0ULL && s->ice_completed_ms != 0ULL &&
        now - s->ice_completed_ms >= dead_ms)
    {
        path[0] = '\0';
        rnet_ice_agent_selected_info(s->ice, path, sizeof(path), NULL, 0, NULL, 0);
        if (path[0] != '\0' && strcmp(path, "relay") != 0)
            goto do_fallback;
    }
    return;

do_fallback:
    if (only_private)
    {
        fprintf(stderr,
                "rnet_ice: TURN fallback with RFC1918-only remotes so far — "
                "restarting force_relay so both sides can gather typ relay "
                "(early=%llums stall=%llums)\n",
                (unsigned long long)private_ms, (unsigned long long)stuck_ms);
    }
    else
    {
        fprintf(stderr,
                "rnet_ice: TURN fallback after host/STUN stall (%llums) — "
                "restarting force_relay\n",
                (unsigned long long)stuck_ms);
    }
    if (rnet_ice_agent_restart_force_relay(s->ice) != 0)
        return;
    s->transport.ice_ctx = s->ice;
    s->ice_attempt_ms = now;
    s->ice_completed_ms = 0;
    s->phase = RNET_PHASE_IDLE;
}
#endif

void rnet_session_pump(RNetSession *s)
{
    if (s == NULL)
    {
        return;
    }
    if (s->ice_hub_mask != 0u)
    {
        int i;
        int all_completed = 1;
        for (i = 1; i < RNET_HUB_MAX_PEERS; ++i)
        {
            if (s->ice_hub[i] == NULL)
            {
                continue;
            }
            rnet_ice_agent_poll(s->ice_hub[i]);
            if (rnet_ice_agent_state(s->ice_hub[i]) == RNET_ICE_STATE_COMPLETED)
            {
                /* Negotiation is over: no later signal may rebuild this seat. */
                rnet_ice_agent_freeze(s->ice_hub[i]);
            }
            else
            {
                all_completed = 0;
            }
        }
        if (s->phase == RNET_PHASE_IDLE && all_completed)
        {
            s->phase = RNET_PHASE_LINKING;
            if (s->ice_completed_ms == 0ULL)
                s->ice_completed_ms = session_now(s);
        }
    }
    if (s->ice != NULL)
    {
#if defined(RNET_ENABLE_ICE)
        session_maybe_ice_relay_fallback(s);
#endif
        rnet_ice_agent_poll(s->ice);
        if (rnet_ice_agent_state(s->ice) == RNET_ICE_STATE_COMPLETED &&
            (!rnet_ice_agent_has_turn(s->ice) || s->last_peer_rx_ms != 0ULL))
        {
            /* Negotiation is over: a repeated REMOTE_SDP/CANDIDATE must not
             * rebuild the live agent. With TURN configured and no session
             * traffic yet, the dead-path fallback may still restart ICE (and
             * the peer's restart offer must be honoured), so freeze later. */
            rnet_ice_agent_freeze(s->ice);
        }
        if (s->phase == RNET_PHASE_IDLE && rnet_ice_agent_state(s->ice) == RNET_ICE_STATE_COMPLETED)
        {
            s->phase = RNET_PHASE_LINKING;
            if (s->ice_completed_ms == 0ULL)
                s->ice_completed_ms = session_now(s);
        }
    }
    pump_recv(s);
    occ_pump(s);
    maybe_bootstrap(s);
    profiles_drive(s);
    obs_pump(s);
    if (s->state_probe_active)
    {
        state_drive_probe(s);
    }
    if (state_any_active(s))
    {
        /* Burst recv↔send so ACK progress can refill the window inside one
         * host pump (otherwise TURN transfers are gated to one cwnd/frame). */
        int burst;
        for (burst = 0; burst < 4; burst++)
        {
            state_drive_sender(s);
            if (!state_any_in_progress(s))
                break;
            pump_recv(s);
        }
    }
    /* LOAD apply/ready suppresses INPUT; emit HELLO so peers keep stamping
     * last_peer_rx_ms (hash-match apply of a multi‑MB .pst is otherwise silent). */
    if (s->phase == RNET_PHASE_RUNNING && (s->input_send_suppress || s->state_stall_sim))
    {
        rnet_u64 now = session_now(s);
        if (now - s->last_hello_ms >= 250ULL)
        {
            rnet_u8 buf[RNET_MAX_PACKET];
            int len = rnet_proto_encode_hello(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                              s->wire_slot, s->cfg.slot_count, s->delay);
            send_raw(s, buf, len);
            s->last_hello_ms = now;
        }
    }
    send_input_bundle(s);
    /* Retransmit pending DELAY_SYNC until both peers apply (UDP). */
    if (s->delay_pending && s->phase == RNET_PHASE_RUNNING)
    {
        rnet_u64 now = session_now(s);
        if (s->delay_pending_last_tx_ms == 0ULL ||
            now - s->delay_pending_last_tx_ms >= 50ULL)
        {
            emit_delay_sync(s, s->delay_pending_value, s->delay_pending_effective);
        }
    }
}

int rnet_session_wait_recv(RNetSession *s, int timeout_ms)
{
    if (s == NULL)
    {
        return 0;
    }
    if (timeout_ms < 0)
    {
        timeout_ms = 0;
    }
    /* LAN UDP: block in poll so the peer can run without us busy-spinning. */
    if (s->transport.mode == RNET_TRANSPORT_LAN_UDP && rnet_os_socket_valid(s->transport.sock))
    {
        int r = rnet_os_poll_recv(s->transport.sock, timeout_ms);
        return (r > 0) ? 1 : 0;
    }
    /* ICE / no sock: coarse sleep only — still better than a busy spin. */
    if (timeout_ms > 0)
    {
        rnet_os_sleep_micros((unsigned)timeout_ms * 1000U);
    }
    return 0;
}

int rnet_session_try_admit(RNetSession *s, rnet_u32 sim_tick)
{
    RNetInputSample resolved[RNET_MAX_SLOTS];
    RNetInputSample local_play;
    RNetInputSample local_future;
    rnet_u32 play_wire;
    rnet_u32 sample_wire;
    rnet_u32 hash;
    rnet_u8 slot;

    if ((s == NULL) || (s->phase != RNET_PHASE_RUNNING))
    {
        if (s)
            note_admit_stall(s, RNET_ADMIT_NOT_RUNNING);
        return 0;
    }
    if (s->profiles.active && rnet_session_profiles_status(s, NULL, NULL) != 1)
    {
        note_admit_stall(s, RNET_ADMIT_NOT_RUNNING);
        return 0;
    }
    if (s->state_stall_sim && (state_any_active(s) || s->state_probe_active))
    {
        /* Stall while probe or chunked transfer is in flight. */
        note_admit_stall(s, RNET_ADMIT_STATE_XFER);
        return 0;
    }
    if (sim_tick != s->sim_tick)
    {
        /* Host must advance in lockstep with session clock. */
        note_admit_stall(s, RNET_ADMIT_SIM_MISMATCH);
        return 0;
    }
    if (s->input_desync)
    {
        note_admit_stall(s, RNET_ADMIT_DESYNC);
        return 0;
    }

    /* Simulate wire T while sampling local input for wire T+D. Once the
     * neutral prefix is consumed, peer input normally arrived D frames ago. */
    play_wire = sim_tick;
    sample_wire = rnet_wire_tick_from_sim(sim_tick, s->delay);
    memset(&local_future, 0, sizeof(local_future));
    memset(&local_play, 0, sizeof(local_play));
    if (!s->is_observer)
    {
        if (!rnet_ring_get(&s->local_ring, sample_wire, &local_future))
        {
            memset(&local_future, 0, sizeof(local_future));
            s->host.sample_local(sim_tick, &local_future, s->host.ctx);
            local_future.tick = sample_wire;
            local_future.valid = 1;
            if (local_future.size > RNET_INPUT_MAX)
            {
                local_future.size = RNET_INPUT_MAX;
            }
            local_ring_put(s, &local_future);
        }

        send_input_bundle(s);
    }
    /* An observer samples nothing and sends nothing.
     *
     * Not sampling is the point: its controllers must not reach the match,
     * and the surest way is for the pad never to enter the pipeline at all.
     * Not sending follows -- it has no row anyone needs, the relay would drop
     * it anyway, and on a direct path a peer would reject it as an
     * out-of-range seat. Three layers agreeing, none of them relied upon
     * alone. It still sends HELLO / READY / confirms, which is what binds it
     * at the relay so the match reaches it. */
    refresh_confirm_window(s, play_wire, sample_wire);

    if (!s->is_observer && !rnet_ring_get(&s->local_ring, play_wire, &local_play))
    {
        if (play_wire == sample_wire)
            local_play = local_future;
        else
        {
            send_input_bundle(s);
            note_admit_stall(s, RNET_ADMIT_WAIT_LOCAL_INPUT);
            return 0;
        }
    }

    if (!remotes_ready_for_play_wire(s, play_wire))
    {
        send_input_bundle(s);
        note_admit_stall(s, RNET_ADMIT_WAIT_REMOTE_INPUT);
        return 0;
    }

    memset(resolved, 0, sizeof(resolved));
    for (slot = 0; slot < s->cfg.slot_count; ++slot)
    {
        if (slot == s->cfg.local_slot)
        {
            resolved[slot] = local_play;
            resolved[slot].tick = sim_tick;
        }
        else if (!rnet_config_slot_occupied(&s->cfg, slot))
        {
            /* Sparse lobby seat: deterministic neutral (matches delay-prefix
             * seed zeros). All peers synthesize the same bytes locally. */
            memset(&resolved[slot], 0, sizeof(resolved[slot]));
            resolved[slot].tick = sim_tick;
            resolved[slot].valid = 1;
        }
        else
        {
            RNetInputSample remote;
            if (!rnet_ring_get(&s->remote_rings[slot], play_wire, &remote))
            {
                note_admit_stall(s, RNET_ADMIT_WAIT_REMOTE_INPUT);
                return 0;
            }
            resolved[slot] = remote;
            resolved[slot].tick = sim_tick;
        }
    }

    hash = hash_resolved_inputs(sim_tick, resolved, (int)s->cfg.slot_count);

    if (!prepare_wire_confirm(s, play_wire, 0) || s->input_desync) {
        note_admit_stall(s, s->input_desync ? RNET_ADMIT_DESYNC
                                            : RNET_ADMIT_WAIT_CONFIRM);
        return 0;
    }
    if (s->published_hash[play_wire % RNET_HISTORY_LENGTH] != hash)
    {
        s->input_desync = 1;
        s->desync_tick = play_wire;
        s->desync_local_hash = hash;
        s->desync_remote_hash =
            s->published_hash[play_wire % RNET_HISTORY_LENGTH];
        note_admit_stall(s, RNET_ADMIT_DESYNC);
        return 0;
    }
    if (!wire_confirmations_agree(s, play_wire))
    {
        (void)prepare_wire_confirm(s, play_wire, 0);
        send_input_bundle(s);
        note_admit_stall(s, RNET_ADMIT_WAIT_CONFIRM);
        return 0;
    }
    s->host.publish(sim_tick, resolved, (int)s->cfg.slot_count, s->host.ctx);
    note_admit_ok(s);
    return 1;

#if 0 /* Legacy strict confirmation barrier; superseded by delay pipeline. */

    if (!s->confirm_active || s->confirm_sim_tick != sim_tick)
    {
        /* Peer INPUT_CONFIRM may arrive before we activate. Preserve those
         * same-tick sightings — wiping them races the slower peer into a
         * permanent stall once the faster peer admits and stops retransmit. */
        rnet_u8 saved_seen[RNET_MAX_SLOTS];
        rnet_u32 saved_hash[RNET_MAX_SLOTS];
        memcpy(saved_seen, s->confirm_seen, sizeof(saved_seen));
        memcpy(saved_hash, s->peer_confirm_hash, sizeof(saved_hash));

        s->confirm_active = 1;
        s->confirm_sim_tick = sim_tick;
        s->confirm_hash = hash;
        memcpy(s->confirm_resolved, resolved, sizeof(resolved));
        memset(s->confirm_seen, 0, sizeof(s->confirm_seen));
        memset(s->peer_confirm_hash, 0, sizeof(s->peer_confirm_hash));
        for (slot = 0; slot < s->cfg.slot_count; ++slot)
        {
            if (slot == s->cfg.local_slot)
            {
                continue;
            }
            if (saved_seen[slot])
            {
                s->confirm_seen[slot] = 1;
                s->peer_confirm_hash[slot] = saved_hash[slot];
                if (saved_hash[slot] != hash)
                {
                    s->input_desync = 1;
                    s->desync_tick = sim_tick;
                    s->desync_local_hash = hash;
                    s->desync_remote_hash = saved_hash[slot];
                    return 0;
                }
            }
        }
        s->confirm_seen[s->cfg.local_slot] = 1;
        s->peer_confirm_hash[s->cfg.local_slot] = hash;
        send_input_confirm(s);
        send_input_bundle(s);
        /* Peer CONFIRM may already be in saved_seen (arrived before we
         * activated). Admit immediately when everyone already agrees. */
        if (confirms_agree(s))
        {
            s->host.publish(sim_tick, s->confirm_resolved, (int)s->cfg.slot_count, s->host.ctx);
            s->confirm_active = 0;
            return 1;
        }
        return 0;
    }

    if (hash != s->confirm_hash)
    {
        s->input_desync = 1;
        s->desync_tick = sim_tick;
        s->desync_local_hash = hash;
        s->desync_remote_hash = s->confirm_hash;
        return 0;
    }

    now = session_now(s);
    if (now - s->last_confirm_ms >= 4ULL)
    {
        send_input_confirm(s);
    }

    if (!confirms_agree(s))
    {
        send_input_bundle(s);
        return 0;
    }

    s->host.publish(sim_tick, s->confirm_resolved, (int)s->cfg.slot_count, s->host.ctx);
    s->confirm_active = 0;
    return 1;
#endif
}

static void apply_pending_delay(RNetSession *s)
{
    if (s == NULL || !s->delay_pending)
    {
        return;
    }
    if (s->sim_tick < s->delay_pending_effective)
    {
        return;
    }
    s->delay = s->delay_pending_value;
    s->cfg.input_delay = s->delay_pending_value;
    s->delay_pending = 0;
}

static void emit_delay_sync(RNetSession *s, rnet_u8 new_delay, rnet_u32 effective_tick)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int len;

    if (s == NULL)
    {
        return;
    }
    len = rnet_proto_encode_delay_sync(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                       new_delay, effective_tick);
    if (len > 0)
    {
        send_raw(s, buf, len);
        s->delay_pending_last_tx_ms = session_now(s);
    }
}

void rnet_session_advance(RNetSession *s)
{
    if (s == NULL)
    {
        return;
    }
    s->sim_tick++;
    apply_pending_delay(s);
}

int rnet_session_input_desync(const RNetSession *s, rnet_u32 *tick, rnet_u32 *local_hash, rnet_u32 *remote_hash)
{
    if ((s == NULL) || !s->input_desync)
    {
        return 0;
    }
    if (tick != NULL)
    {
        *tick = s->desync_tick;
    }
    if (local_hash != NULL)
    {
        *local_hash = s->desync_local_hash;
    }
    if (remote_hash != NULL)
    {
        *remote_hash = s->desync_remote_hash;
    }
    return 1;
}

int rnet_session_send_bye(RNetSession *s)
{
    rnet_u8 buf[64];
    int n;

    if ((s == NULL) || (s->transport.mode == RNET_TRANSPORT_NONE))
    {
        return -1;
    }
    n = rnet_proto_encode_bye(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->wire_slot);
    if (n <= 0)
    {
        return -1;
    }
    /* Best-effort: send a few times so a single loss doesn't leave peer hanging. */
    (void)rnet_transport_send(&s->transport, buf, (size_t)n);
    (void)rnet_transport_send(&s->transport, buf, (size_t)n);
    (void)rnet_transport_send(&s->transport, buf, (size_t)n);
    return 0;
}

int rnet_session_peer_disconnected(const RNetSession *s, rnet_u64 timeout_ms)
{
    rnet_u64 now;
    rnet_u64 last;

    if (s == NULL)
    {
        return 0;
    }
    if (s->peer_gone)
    {
        return 1;
    }
    if (timeout_ms == 0)
    {
        return 0;
    }
    now = rnet_os_monotonic_ms();
    if (s->last_peer_rx_ms == 0)
    {
        /* No peer traffic yet — only after we expected packets (linking/running).
         * Rematch session_reboot is often >15s on the slower peer; the old
         * timeout_ms*10 (~15s at 1500) false-disconnected the fast peer and
         * BYE'd both back to lobby. Keep a long link budget; callers that
         * pass timeout_ms==0 skip this path entirely (BYE-only). */
        rnet_u64 link_budget_ms;
        if (s->phase != RNET_PHASE_RUNNING && s->phase != RNET_PHASE_LINKING)
        {
            return 0;
        }
        link_budget_ms = timeout_ms * 60u;
        if (link_budget_ms < 90000u)
            link_budget_ms = 90000u;
        if (s->session_start_ms != 0 && (now - s->session_start_ms) > link_budget_ms)
        {
            return 1;
        }
        return 0;
    }
    last = s->last_peer_rx_ms;
    return (now > last && (now - last) >= timeout_ms) ? 1 : 0;
}

void rnet_session_touch_peer_liveness(RNetSession *s)
{
    rnet_u64 now;
    rnet_u8 slot;
    if (s == NULL)
    {
        return;
    }
    now = session_now(s);
    /* Per seat: every occupied remote seat that has not said BYE. Done even
     * when the aggregate is already gone -- one seat leaving a room of four
     * must not freeze the others' silence budgets. */
    for (slot = 0; slot < s->cfg.slot_count && slot < RNET_MAX_SLOTS; ++slot)
    {
        if (slot == s->cfg.local_slot || !rnet_config_slot_occupied(&s->cfg, slot) ||
            (s->peer_gone_mask & (1u << slot)))
        {
            continue;
        }
        s->peer_rx_ms[slot] = now ? now : 1u;
    }
    if (s->peer_gone)
    {
        return;
    }
    s->last_peer_rx_ms = now;
}

/* Remote seat index valid for the per-seat liveness accessors. */
static int liveness_seat_ok(const RNetSession *s, int slot)
{
    return s != NULL && slot >= 0 && slot < (int)s->cfg.slot_count && slot < RNET_MAX_SLOTS &&
           slot != (int)s->cfg.local_slot;
}

int rnet_session_peer_gone(const RNetSession *s, int slot)
{
    if (!liveness_seat_ok(s, slot))
    {
        return 0;
    }
    return (s->peer_gone_mask & (1u << slot)) ? 1 : 0;
}

rnet_u32 rnet_session_peer_gone_mask(const RNetSession *s)
{
    return (s != NULL) ? s->peer_gone_mask : 0u;
}

rnet_u64 rnet_session_peer_rx_age_ms(const RNetSession *s, int slot)
{
    rnet_u64 now;
    if (!liveness_seat_ok(s, slot) || s->peer_rx_ms[slot] == 0)
    {
        return RNET_PEER_RX_NEVER;
    }
    now = session_now((RNetSession *)s);
    return (now > s->peer_rx_ms[slot]) ? (now - s->peer_rx_ms[slot]) : 0u;
}

int rnet_session_peer_slot_disconnected(const RNetSession *s, int slot, rnet_u64 timeout_ms)
{
    rnet_u64 age;
    if (!liveness_seat_ok(s, slot) || !rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot) ||
        occ_absent_latest(s, slot))
    {
        return 0;
    }
    if (s->peer_gone_mask & (1u << slot))
    {
        return 1;
    }
    if (timeout_ms == 0)
    {
        return 0;
    }
    if (s->peer_rx_ms[slot] == 0)
    {
        /* Never heard from this seat: the same long link budget as the
         * aggregate (rematch boots can exceed 15 s on one peer). */
        rnet_u64 link_budget_ms;
        rnet_u64 os_now;
        if (s->phase != RNET_PHASE_RUNNING && s->phase != RNET_PHASE_LINKING)
        {
            return 0;
        }
        link_budget_ms = timeout_ms * 60u;
        if (link_budget_ms < 90000u)
            link_budget_ms = 90000u;
        os_now = rnet_os_monotonic_ms();
        return (s->session_start_ms != 0 && os_now - s->session_start_ms > link_budget_ms) ? 1 : 0;
    }
    age = rnet_session_peer_rx_age_ms(s, slot);
    return (age >= timeout_ms) ? 1 : 0;
}

rnet_u32 rnet_session_disconnected_peers(const RNetSession *s, rnet_u64 timeout_ms)
{
    rnet_u32 mask = 0;
    int slot;
    if (s == NULL)
    {
        return 0;
    }
    for (slot = 0; slot < (int)s->cfg.slot_count && slot < RNET_MAX_SLOTS; ++slot)
    {
        if (rnet_session_peer_slot_disconnected(s, slot, timeout_ms))
        {
            mask |= 1u << slot;
        }
    }
    return mask;
}

void rnet_session_set_seat_loss_policy(RNetSession *s, rnet_u32 drop_timeout_ms)
{
    if (s != NULL)
        s->seat_loss_timeout_ms = drop_timeout_ms;
}

void rnet_session_set_link_ok(RNetSession *s, int ok)
{
    if (s != NULL)
    {
        rnet_u64 now = session_now(s);
        s->link_ok_ms = ok ? (now ? now : 1u) : 0u;
    }
}

int rnet_session_occupied_at(const RNetSession *s, int slot, rnet_u32 wire)
{
    if (s == NULL || slot < 0 || slot >= (int)s->cfg.slot_count)
        return 0;
    return rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot) && occ_present(s, slot, wire, 0);
}

rnet_u32 rnet_session_absent_mask(const RNetSession *s)
{
    rnet_u32 mask = 0;
    int slot;
    if (s == NULL)
        return 0;
    for (slot = 0; slot < (int)s->cfg.slot_count && slot < RNET_MAX_SLOTS; ++slot)
        if (occ_absent_latest(s, slot))
            mask |= 1u << slot;
    return mask;
}

rnet_u32 rnet_session_absent_mask_at(const RNetSession *s, rnet_u32 wire)
{
    rnet_u32 mask = 0;
    int slot;
    if (s == NULL)
        return 0;
    for (slot = 0; slot < (int)s->cfg.slot_count && slot < RNET_MAX_SLOTS; ++slot)
        if (occ_absent_latest(s, slot) || occ_absent_at(s, slot, wire))
            mask |= 1u << slot;
    return mask;
}

void rnet_session_set_claimable_mask(RNetSession *s, rnet_u32 mask)
{
    if (s != NULL)
    {
        s->claimable_mask = mask;
        s->claimable_set = 1;
    }
}

rnet_u32 rnet_session_observer_live_tick(const RNetSession *s)
{
    rnet_u32 hi = 0;
    int slot;
    if (s == NULL || !s->is_observer)
        return 0;
    for (slot = 0; slot < (int)s->cfg.slot_count && slot < RNET_MAX_SLOTS; ++slot)
        if (s->obs_seen_valid[slot] && s->obs_seen_tip[slot] > hi)
            hi = s->obs_seen_tip[slot];
    return hi;
}

void rnet_session_late_join_counters(const RNetSession *s, rnet_u32 *added, rnet_u32 *aborted)
{
    if (added) *added = s ? s->adds_done : 0u;
    if (aborted) *aborted = s ? s->adds_aborted : 0u;
}

rnet_u32 rnet_session_occ_gen(const RNetSession *s)
{
    return s != NULL ? s->occ_n : 0u;
}

int rnet_session_occ_event(const RNetSession *s, int index, rnet_u32 *wire, int *slot, int *present)
{
    if (s == NULL || index < 0 || index >= (int)s->occ_n)
        return 0;
    if (wire) *wire = s->occ[index].wire;
    if (slot) *slot = s->occ[index].slot;
    if (present) *present = s->occ[index].present;
    return 1;
}

int rnet_session_dropped_self(const RNetSession *s)
{
    return s != NULL && s->dropped_self;
}

int rnet_session_declare_seat_drop(RNetSession *s, int slot, const char *reason)
{
    if (s == NULL || !s->is_sim_authority || slot < 1 || slot >= (int)s->cfg.slot_count ||
        slot >= RNET_MAX_SLOTS || !rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot) ||
        !occ_present(s, slot, 0, 1) || s->occ_n >= RNET_OCC_MAX)
        return 0;
    occ_declare_drop(s, slot, reason ? reason : "declared");
    return 1;
}

void rnet_session_push_signal(RNetSession *s, const RNetSignal *msg)
{
    if ((s == NULL) || (msg == NULL) || (s->ice == NULL))
    {
        return;
    }
    rnet_ice_agent_push_signal(s->ice, msg);
}

int rnet_session_push_signal_from(RNetSession *s, int slot, const RNetSignal *sig)
{
    if ((s == NULL) || (sig == NULL) || slot < 1 || slot >= RNET_HUB_MAX_PEERS ||
        s->ice_hub[slot] == NULL)
    {
        return -1;
    }
    rnet_ice_agent_push_signal(s->ice_hub[slot], sig);
    return 0;
}

rnet_u8 rnet_session_committed_delay(const RNetSession *s)
{
    return (s != NULL) ? s->delay : 0;
}

int rnet_session_request_delay_change(RNetSession *s, rnet_u8 new_delay)
{
    rnet_u32 effective;
    rnet_u8 clamped;

    if (s == NULL || s->phase != RNET_PHASE_RUNNING)
    {
        return 0;
    }
    clamped = new_delay;
    if (clamped < 2u)
    {
        clamped = 2u;
    }
    if (clamped > 20u)
    {
        clamped = 20u;
    }
    if (clamped == s->delay && !s->delay_pending)
    {
        return 0;
    }
    /* Margin: 2*D + 8 ticks so the packet can land before both peers admit. */
    effective = s->sim_tick + ((rnet_u32)s->delay * 2u) + 8u;
    if (s->delay_pending && s->delay_pending_effective > s->sim_tick &&
        s->delay_pending_value == clamped)
    {
        /* Already scheduled — refresh the wire copy. */
        emit_delay_sync(s, clamped, s->delay_pending_effective);
        return 1;
    }
    s->delay_pending = 1;
    s->delay_pending_value = clamped;
    s->delay_pending_effective = effective;
    emit_delay_sync(s, clamped, effective);
    return 1;
}

int rnet_session_local_slot(const RNetSession *s)
{
    return (s != NULL) ? (int)s->cfg.local_slot : -1;
}

int rnet_session_is_observer(const RNetSession *s)
{
    return (s != NULL) ? s->is_observer : 0;
}

rnet_u32 rnet_session_sim_tick(const RNetSession *s)
{
    return (s != NULL) ? s->sim_tick : 0;
}

int rnet_session_is_running(const RNetSession *s)
{
    return (s != NULL && s->phase == RNET_PHASE_RUNNING) ? 1 : 0;
}

RNetIceState rnet_session_ice_state(const RNetSession *s)
{
    if (s == NULL)
    {
        return RNET_ICE_STATE_IDLE;
    }
    if (s->ice_hub_mask != 0u)
    {
        /* Minimum over seats (enum order IDLE < ... < COMPLETED); FAILED is
         * the highest value but dominates, so report it if any seat failed. */
        int i;
        RNetIceState min = RNET_ICE_STATE_COMPLETED;
        for (i = 1; i < RNET_HUB_MAX_PEERS; ++i)
        {
            RNetIceState st;
            if (s->ice_hub[i] == NULL)
                continue;
            st = rnet_ice_agent_state(s->ice_hub[i]);
            if (st == RNET_ICE_STATE_FAILED)
                return RNET_ICE_STATE_FAILED;
            if (st < min)
                min = st;
        }
        return min;
    }
    if (s->ice == NULL)
    {
        return RNET_ICE_STATE_IDLE;
    }
    return rnet_ice_agent_state(s->ice);
}

void rnet_session_get_stats(const RNetSession *s, RNetSessionStats *out)
{
    rnet_u8 slot;
    rnet_u32 highest_remote = 0;
    rnet_u64 now;
    int have_remote = 0;

    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (s == NULL)
        return;

    out->sim_tick = s->sim_tick;
    out->delay = s->delay;
    out->local_slot = s->cfg.local_slot;
    out->slot_count = s->cfg.slot_count;
    out->is_running = (s->phase == RNET_PHASE_RUNNING) ? 1 : 0;
    out->peer_gone = s->peer_gone;
    out->input_desync = s->input_desync;
    out->desync_tick = s->desync_tick;
    out->desync_local_hash = s->desync_local_hash;
    out->desync_remote_hash = s->desync_remote_hash;
    out->ice_state = rnet_session_ice_state(s);
    out->last_stall = s->last_stall;
    out->consecutive_stalls = s->consecutive_stalls;
    out->admit_ok_count = s->admit_ok_count;
    out->stall_streaks = s->stall_streaks;
    out->last_admit_wait_ms = s->last_admit_wait_ms;
    out->max_admit_wait_ms = s->max_admit_wait_ms;
    out->state_busy = (state_any_active(s) || s->state_probe_active) ? 1 : 0;
    /* The single-transfer fields describe the outbound transfer when there is
     * one, else the lowest-seat inbound one (the only one with two seats). */
    if (s->tx.active)
    {
        out->state_op = s->tx.op;
        out->state_sender = 1;
        out->state_bytes_total = s->tx.total;
        out->state_bytes_acked = state_tx_min_ack(s);
    }
    else
    {
        int i;
        const RNetStateRx *rx = NULL;
        for (i = 0; i < RNET_MAX_SLOTS && rx == NULL; ++i)
        {
            if (s->rx[i].active)
                rx = &s->rx[i];
        }
        out->state_op = (rx != NULL) ? rx->op : (s->state_probe_active ? s->state_probe_op : 0);
        out->state_sender = 0;
        out->state_bytes_total = (rx != NULL) ? rx->total : 0;
        out->state_bytes_acked = (rx != NULL) ? rx->contiguity : 0;
    }
    out->state_expect_mask = s->tx.active ? s->tx.expect_mask : 0u;
    out->state_done_mask = s->tx.active ? state_tx_done_mask(s) : 0u;
    {
        int i;
        for (i = 0; i < RNET_MAX_SLOTS; ++i)
        {
            if (s->rx[i].active)
                out->state_rx_mask |= 1u << i;
        }
    }
    out->state_probe_expect_mask = (s->state_probe_active && s->state_probe_sender) ? s->state_probe_expect_mask : 0u;
    out->state_probe_reply_mask = (s->state_probe_active && s->state_probe_sender) ? s->state_probe_reply_mask : 0u;
    out->peer_gone_mask = s->peer_gone_mask;
    {
        int i;
        for (i = 0; i < RNET_MAX_SLOTS; ++i)
            out->peer_rx_age_ms[i] = rnet_session_peer_rx_age_ms(s, i);
    }
    out->packets_rx = s->packets_rx;
    out->input_bundle_sends = s->input_bundle_sends;

    now = session_now((RNetSession *)s);
    if (s->last_peer_rx_ms != 0)
        out->last_peer_rx_age_ms = now - s->last_peer_rx_ms;
    /* Refresh live stall wait while still blocked. */
    if (s->stall_started_ms != 0 && s->last_stall != RNET_ADMIT_OK) {
        out->last_admit_wait_ms = (rnet_u32)(now - s->stall_started_ms);
        if (out->last_admit_wait_ms > out->max_admit_wait_ms)
            out->max_admit_wait_ms = out->last_admit_wait_ms;
    }

    for (slot = 0; slot < s->cfg.slot_count; ++slot) {
        rnet_u32 tip;
        if (slot == s->cfg.local_slot)
            continue;
        if (!rnet_config_slot_occupied(&s->cfg, slot) || occ_absent_now(s, slot))
            continue;
        tip = rnet_ring_highest_valid(&s->remote_rings[slot]);
        if (!have_remote || tip > highest_remote)
            highest_remote = tip;
        have_remote = 1;
    }
    out->highest_remote_wire = highest_remote;
    out->remote_lead = have_remote ? (int)highest_remote - (int)s->sim_tick : 0;

#if defined(RNET_ENABLE_ICE)
    if (s->ice_hub_mask != 0u) {
        int i;
        rnet_u32 drops = 0;
        for (i = 1; i < RNET_HUB_MAX_PEERS; ++i)
            drops += rnet_ice_agent_recv_drops(s->ice_hub[i]);
        out->ice_recv_drops = drops;
        if (out->ice_state == RNET_ICE_STATE_FAILED)
            snprintf(out->ice_path, sizeof(out->ice_path), "failed");
        else if (out->ice_state == RNET_ICE_STATE_COMPLETED)
            snprintf(out->ice_path, sizeof(out->ice_path), "hub");
        else
            snprintf(out->ice_path, sizeof(out->ice_path), "pending");
    } else if (s->ice != NULL) {
        out->ice_recv_drops = rnet_ice_agent_recv_drops(s->ice);
        rnet_ice_agent_selected_info(s->ice, out->ice_path, sizeof(out->ice_path),
                                     out->ice_local, sizeof(out->ice_local),
                                     out->ice_remote, sizeof(out->ice_remote));
        if (out->ice_state == RNET_ICE_STATE_FAILED)
            snprintf(out->ice_path, sizeof(out->ice_path), "failed");
        else if (out->ice_state != RNET_ICE_STATE_CONNECTED &&
                 out->ice_state != RNET_ICE_STATE_COMPLETED &&
                 out->ice_path[0] == '\0')
            snprintf(out->ice_path, sizeof(out->ice_path), "pending");
    } else
#endif
    {
        snprintf(out->ice_path, sizeof(out->ice_path), "lan");
    }
}

int rnet_session_state_probe(RNetSession *s, rnet_u8 op, rnet_u8 slot, rnet_u32 total_size, rnet_u32 payload_crc)
{
    rnet_u32 expect;
    if ((s == NULL) || s->cfg.local_slot != 0 || s->phase != RNET_PHASE_RUNNING)
    {
        return -1;
    }
    if (state_any_active(s) || (s->state_probe_active && s->state_probe_sender && !state_probe_all_replied(s)))
    {
        return -1;
    }
    if (op != RNET_STATE_OP_SAVE && op != RNET_STATE_OP_LOAD && op != RNET_STATE_OP_SRAM &&
        op != RNET_STATE_OP_RB_KF && op != RNET_STATE_OP_BOOT && op != RNET_STATE_OP_MENU)
    {
        return -1;
    }
    if (total_size > RNET_STATE_MAX)
    {
        return -1;
    }
    expect = state_default_receivers(s, op);
    if (expect == 0u)
    {
        return -1; /* nobody to answer */
    }

    state_probe_clear(s);
    s->state_probe_released_valid = 0;
    s->state_probe_active = 1;
    s->state_probe_sender = 1;
    s->state_probe_expect_mask = expect;
    s->state_probe_reply_mask = 0;
    s->state_probe_match_mask = 0;
    s->state_probe_pending = 0;
    s->state_probe_match = 0;
    s->state_probe_op = op;
    s->state_probe_slot = slot;
    s->state_probe_size = total_size;
    s->state_probe_crc = payload_crc;
    s->state_probe_last_tx_ms = 0;
    /* size==0 probes must not stall INPUT: SAVE coord and LOAD ready both need
     * the slower peer to keep admitting for savestate_poll. App-layer LOAD_READY
     * freezes sim until mutual ready + hard_resync; stalling send_input_bundle
     * here starves the late applier's tip runway (spam-load hang).
     * Hash probe (size!=0): stall until agree or transfer. */
    s->state_stall_sim = (total_size != 0) ? 1 : 0;
    state_drive_probe(s);
    return 0;
}

int rnet_session_state_probe_take_reply(RNetSession *s, int *match_out)
{
    if ((s == NULL) || !s->state_probe_active || !s->state_probe_sender || !state_probe_all_replied(s))
    {
        return 0;
    }
    if (match_out)
    {
        *match_out = ((s->state_probe_match_mask & s->state_probe_expect_mask) == s->state_probe_expect_mask) ? 1 : 0;
    }
    return 1;
}

int rnet_session_state_probe_take_reply_from(RNetSession *s, int slot, int *match_out)
{
    if ((s == NULL) || !s->state_probe_active || !s->state_probe_sender || slot < 0 || slot >= RNET_MAX_SLOTS ||
        !(s->state_probe_expect_mask & (1u << slot)) || !(s->state_probe_reply_mask & (1u << slot)))
    {
        return 0;
    }
    if (match_out)
    {
        *match_out = (s->state_probe_match_mask & (1u << slot)) ? 1 : 0;
    }
    return 1;
}

int rnet_session_state_probe_replies(const RNetSession *s, rnet_u32 *expect_mask, rnet_u32 *replied_mask,
                                     rnet_u32 *match_mask)
{
    if ((s == NULL) || !s->state_probe_active || !s->state_probe_sender)
    {
        return 0;
    }
    if (expect_mask)
    {
        *expect_mask = s->state_probe_expect_mask;
    }
    if (replied_mask)
    {
        *replied_mask = s->state_probe_reply_mask & s->state_probe_expect_mask;
    }
    if (match_mask)
    {
        *match_mask = s->state_probe_match_mask & s->state_probe_reply_mask & s->state_probe_expect_mask;
    }
    return 1;
}

int rnet_session_state_probe_pending(const RNetSession *s, rnet_u8 *op_out, rnet_u8 *slot_out, rnet_u32 *size_out,
                                     rnet_u32 *crc_out)
{
    if ((s == NULL) || !s->state_probe_active || !s->state_probe_pending)
    {
        return 0;
    }
    if (op_out)
    {
        *op_out = s->state_probe_op;
    }
    if (slot_out)
    {
        *slot_out = s->state_probe_slot;
    }
    if (size_out)
    {
        *size_out = s->state_probe_size;
    }
    if (crc_out)
    {
        *crc_out = s->state_probe_crc;
    }
    return 1;
}

int rnet_session_state_probe_reply(RNetSession *s, int match)
{
    rnet_u8 buf[64];
    int n;

    if ((s == NULL) || !s->state_probe_active || s->state_probe_sender || !s->state_probe_pending)
    {
        return -1;
    }
    n = rnet_proto_encode_state_probe_reply(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                            s->wire_slot, s->state_probe_op, s->state_probe_slot,
                                            match ? 1u : 0u, s->state_probe_size, s->state_probe_crc);
    if (n <= 0)
    {
        return -1;
    }
    send_raw(s, buf, n);
    s->state_probe_pending = 0;
    s->state_probe_match = match ? 1 : 0;
    if (s->state_probe_size == 0)
    {
        s->state_stall_sim = 0;
        if (s->state_probe_op == RNET_STATE_OP_LOAD || s->state_probe_op == RNET_STATE_OP_BOOT)
        {
            /* Post-load / post-boot ready ACK — done; do not keep probe for retransmit. */
            state_probe_clear(s);
            return 0;
        }
        /* SAVE coord ACK: leave probe active for retransmit replies. */
        return 0;
    }
    if (match)
    {
        if (s->state_probe_op == RNET_STATE_OP_LOAD || s->state_probe_op == RNET_STATE_OP_BOOT)
        {
            /* LOAD/BOOT: the app keeps pumping until its staged restore is
             * applied (its own ready probe follows), so release now. */
            state_probe_clear(s);
        }
        else
        {
            /* SAVE/SRAM/RB_KF: another seat may still miss, and its transfer
             * must start from the same tick. Hold input until the host has
             * heard every seat and sends DONE (or BEGIN on a miss). */
            s->state_probe_parked = 1;
            s->state_probe_last_tx_ms = session_now(s);
        }
    }
    /* Hash miss: keep stall until STATE_BEGIN (or a new probe). */
    return 0;
}

void rnet_session_state_probe_finish(RNetSession *s)
{
    if (s != NULL)
    {
        state_send_probe_done(s);
    }
    state_probe_clear(s);
}

int rnet_session_state_begin(RNetSession *s, rnet_u8 op, rnet_u8 slot, const void *data, size_t size)
{
    rnet_u8 buf[64];
    int n;
    rnet_u32 expect;

    if ((s == NULL) || (data == NULL) || (size == 0) || (size > RNET_STATE_MAX))
    {
        return -1;
    }
    if (s->phase != RNET_PHASE_RUNNING || state_any_active(s) || s->is_observer)
    {
        return -1;
    }
    if (op == RNET_STATE_OP_MEMCARD)
    {
        /* Guest-only upload: the host is the receiver of this op, never its
         * sender (it would be broadcasting to peers that drop it). */
        if (s->cfg.local_slot == 0)
        {
            return -1;
        }
    }
    else if (s->cfg.local_slot != 0)
    {
        return -1;
    }
    else if (op != RNET_STATE_OP_SAVE && op != RNET_STATE_OP_LOAD && op != RNET_STATE_OP_SRAM &&
             op != RNET_STATE_OP_RB_KF && op != RNET_STATE_OP_BOOT && op != RNET_STATE_OP_MENU)
    {
        return -1;
    }
    expect = state_default_receivers(s, op);
    if (expect == 0u)
    {
        return -1; /* nobody to send to */
    }

    /* Drop any open probe — transfer is the authority path after a hash miss. */
    state_probe_clear(s);

    state_tx_clear(s);
    s->tx.buf = (rnet_u8 *)malloc(size);
    if (s->tx.buf == NULL)
    {
        state_recompute_stall(s);
        return -1;
    }
    memcpy(s->tx.buf, data, size);
    s->tx.active = 1;
    s->tx.ready = 0;
    s->state_stall_sim = 1;
    s->tx.op = op;
    s->tx.slot = slot;
    s->tx.expect_mask = expect;
    s->state_next_xfer_id++;
    if (s->state_next_xfer_id == 0)
    {
        s->state_next_xfer_id = 1;
    }
    s->tx.xfer_id = s->state_next_xfer_id;
    if (op == RNET_STATE_OP_MEMCARD)
    {
        /* The receiver compares against the ids of transfers IT finished — its
         * own, small counter. Keep guest uploads out of that space so a fresh
         * upload is never mistaken for a completed host transfer and re-ACKed
         * instead of received.
         *
         * Bits 24..29 carry (seat - 1): the host's STATE_ACK names only the
         * transfer id, and it is broadcast to every guest, so two guests
         * uploading at once with the same serial would each take the host's
         * ACK of the OTHER's upload as its own. Seat 1 gets 0 there, so a
         * two-seat guest's id is exactly what it always was. The serial keeps
         * the low 24 bits; the receiver's stale-BEGIN window compares ids from
         * one sender, whose seat bits never change. */
        s->tx.xfer_id = 0x40000000u | ((((rnet_u32)s->cfg.local_slot - 1u) & 0x3fu) << 24) |
                        (s->state_next_xfer_id & 0x00ffffffu);
    }
    s->tx.total = (rnet_u32)size;
    s->tx.crc = rnet_proto_checksum(s->tx.buf, s->tx.total);
    s->tx.send_cursor = 0;
    s->tx.start_ms = session_now(s);
    state_pacing_reset(s);

    n = rnet_proto_encode_state_begin(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id, s->wire_slot,
                                      s->tx.op, s->tx.slot, s->tx.xfer_id, s->tx.total, s->tx.crc);
    if (n > 0)
    {
        send_raw(s, buf, n);
        s->tx.last_begin_ms = session_now(s);
    }
    state_drive_sender(s);
    return 0;
}

int rnet_session_state_busy(const RNetSession *s)
{
    if (s == NULL)
    {
        return 0;
    }
    if (s->state_probe_active && s->state_probe_sender && !state_probe_all_replied(s))
    {
        return 1;
    }
    if (s->state_probe_active && (s->state_probe_pending || s->state_probe_parked))
    {
        return 1;
    }
    return state_any_in_progress(s);
}

/* The transfer take_ready reports: the outbound one when it is complete,
 * else the ready inbound one from the lowest source seat. -1 = none ready. */
static int state_pick_ready(const RNetSession *s)
{
    int i;
    if (s->tx.active && s->tx.ready && s->tx.buf != NULL)
    {
        return RNET_STATE_TAKEN_TX;
    }
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
    {
        if (s->rx[i].active && s->rx[i].ready && s->rx[i].buf != NULL)
        {
            return i;
        }
    }
    return -1;
}

int rnet_session_state_take_ready_from(RNetSession *s, int *from_out, rnet_u8 *op_out, rnet_u8 *slot_out,
                                       const void **data_out, size_t *size_out)
{
    int pick;
    rnet_u8 op, slot;
    const rnet_u8 *data;
    rnet_u32 total;
    int from;
    if (s == NULL)
    {
        return 0;
    }
    pick = state_pick_ready(s);
    if (pick < 0)
    {
        return 0;
    }
    if (pick == RNET_STATE_TAKEN_TX)
    {
        op = s->tx.op;
        slot = s->tx.slot;
        data = s->tx.buf;
        total = s->tx.total;
        from = (int)s->cfg.local_slot;
    }
    else
    {
        op = s->rx[pick].op;
        slot = s->rx[pick].slot;
        data = s->rx[pick].buf;
        total = s->rx[pick].total;
        from = pick;
    }
    s->state_taken = pick;
    if (from_out)
    {
        *from_out = from;
    }
    if (op_out)
    {
        *op_out = op;
    }
    if (slot_out)
    {
        *slot_out = slot;
    }
    if (data_out)
    {
        *data_out = data;
    }
    if (size_out)
    {
        *size_out = total;
    }
    return 1;
}

int rnet_session_state_take_ready(RNetSession *s, rnet_u8 *op_out, rnet_u8 *slot_out, const void **data_out,
                                  size_t *size_out)
{
    return rnet_session_state_take_ready_from(s, NULL, op_out, slot_out, data_out, size_out);
}

int rnet_session_state_drop_peer(RNetSession *s, int slot)
{
    int changed = 0;
    if (s == NULL || slot < 0 || slot >= RNET_MAX_SLOTS)
    {
        return 0;
    }
    if (s->tx.active && (s->tx.expect_mask & (1u << slot)))
    {
        s->tx.expect_mask &= ~(1u << slot);
        changed = 1;
        state_tx_mark_ready_if_complete(s);
    }
    if (s->state_probe_active && s->state_probe_sender && (s->state_probe_expect_mask & (1u << slot)))
    {
        s->state_probe_expect_mask &= ~(1u << slot);
        changed = 1;
    }
    return changed;
}

int rnet_session_state_progress(const RNetSession *s, int slot, rnet_u32 *acked_out, rnet_u32 *total_out)
{
    if (s == NULL || !s->tx.active || slot < 0 || slot >= RNET_MAX_SLOTS || !(s->tx.expect_mask & (1u << slot)))
    {
        return 0;
    }
    if (acked_out)
    {
        *acked_out = s->tx.ack[slot];
    }
    if (total_out)
    {
        *total_out = s->tx.total;
    }
    return 1;
}

rnet_u32 rnet_session_state_pending_receivers(const RNetSession *s)
{
    if (s == NULL || !s->tx.active)
    {
        return 0u;
    }
    return s->tx.expect_mask & ~state_tx_done_mask(s);
}

rnet_u32 rnet_session_state_inbound_mask(const RNetSession *s)
{
    rnet_u32 mask = 0;
    int i;
    if (s == NULL)
    {
        return 0u;
    }
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
    {
        if (s->rx[i].active)
        {
            mask |= 1u << i;
        }
    }
    return mask;
}

void rnet_session_hard_resync(RNetSession *s)
{
    rnet_u8 i;
    if (s == NULL)
    {
        return;
    }
    rnet_ring_clear(&s->local_ring);
    /* Clear remotes too: leftover tip rows from a prior post-load epoch are
     * first-wins and can let one peer admit on stale wire=D inputs. Both peers
     * re-prime after mutual ready and wait for a fresh tip exchange. */
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
    {
        rnet_ring_clear(&s->remote_rings[i]);
    }
    memset(s->published_valid, 0, sizeof(s->published_valid));
    memset(s->peer_history_valid, 0, sizeof(s->peer_history_valid));
    memset(s->confirm_last_sent_ms, 0, sizeof(s->confirm_last_sent_ms));
    s->input_desync = 0;
    s->desync_tick = 0;
    s->desync_local_hash = 0;
    s->desync_remote_hash = 0;
    s->highest_remote_ack = 0;
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
        s->remote_contiguous_ack[i] = 0xffffffffu;
    memset(s->peer_ack_seen, 0, sizeof(s->peer_ack_seen));
    /* A new era restarts wire time: the old schedule's wire ticks mean nothing. */
    s->occ_n = 0;
    s->dropped_self = 0;
    memset(s->occ_peer_ack, 0, sizeof(s->occ_peer_ack));
    memset(s->reserve_until_ms, 0, sizeof(s->reserve_until_ms));
    s->add_pending = 0;
    {
        int k;
        for (k = 0; k < RNET_MAX_SLOTS; ++k)
            rlog_free(&s->row_log[k]);
        s->row_log_off = 0;
    }
    s->last_input_tip_valid = 0;
    memset(s->tip_sent_ms, 0, sizeof(s->tip_sent_ms)); /* RTT clock: new tick space */
    memset(s->link_last_ack, 0, sizeof(s->link_last_ack));
    /* Peers may have applied a load on different sim ticks; restart together. */
    s->sim_tick = 0;
    occ_seed_open(s); /* the launch's open seats are open again in the new era */
    /* Invalidate in-flight INPUT/CONFIRM from the previous era (same low ticks
     * would otherwise first-wins into this window — spam rematch + stick mash). */
    s->input_epoch = (rnet_u16)(s->input_epoch + 1u);
    s->rb_fc_q_head = s->rb_fc_q_tail = s->rb_fc_q_count = 0;
    s->rb_sync_head = s->rb_sync_tail = s->rb_sync_count = 0;
    s->rb_seal_head = s->rb_seal_tail = s->rb_seal_count = 0;
    s->rb_base_head = s->rb_base_tail = s->rb_base_count = 0;
    s->rb_post_head = s->rb_post_tail = s->rb_post_count = 0;
    s->rb_resolved_head = s->rb_resolved_tail = s->rb_resolved_count = 0;
    s->rb_last_from = -1;

    /* Keep suppress until prime_delay_inputs — avoids emitting an empty tip. */
    s->input_send_suppress = 1;
    s->delay_pending = 0;
}

void rnet_session_set_input_send_suppress(RNetSession *s, int suppress)
{
    if (s == NULL)
    {
        return;
    }
    s->input_send_suppress = suppress ? 1 : 0;
}

void rnet_session_prime_delay_inputs(RNetSession *s, const rnet_u8 *bytes, rnet_u16 size)
{
    rnet_u32 tip;
    rnet_u32 t;
    if (s == NULL || bytes == NULL || size == 0 || size > RNET_INPUT_MAX)
    {
        return;
    }
    if (s->phase != RNET_PHASE_RUNNING)
    {
        return;
    }
    tip = rnet_wire_tick_from_sim(s->sim_tick, s->delay);
    for (t = s->sim_tick; t < tip; ++t)
    {
        RNetInputSample sample;
        memset(&sample, 0, sizeof(sample));
        sample.tick = t;
        sample.size = size;
        memcpy(sample.bytes, bytes, size);
        sample.valid = 1;
        local_ring_put(s, &sample);
    }
    s->last_input_ms = 0;
    s->last_input_tip_valid = 0;
    s->input_send_suppress = 0;
    /* Prime must emit even if a LOAD ready probe still has state_stall_sim
     * (host commits sync before probe_finish). */
    {
        int saved_stall = s->state_stall_sim;
        s->state_stall_sim = 0;
        send_input_bundle(s);
        s->state_stall_sim = saved_stall;
    }
}

/* Finish (ready) or abort (not ready) one inbound transfer. */
static void state_finish_rx(RNetSession *s, int src)
{
    if (s->rx[src].active && s->rx[src].ready)
    {
        s->state_received[src].finished = 1;
    }
    state_rx_clear(s, src);
}

void rnet_session_state_finish_from(RNetSession *s, int from_slot, int hard_resync)
{
    int i;
    if (s == NULL)
    {
        return;
    }
    if (from_slot == RNET_STATE_FROM_ALL)
    {
        for (i = 0; i < RNET_MAX_SLOTS; ++i)
        {
            state_finish_rx(s, i);
        }
        state_tx_clear(s);
    }
    else if (!s->is_observer && from_slot == (int)s->cfg.local_slot)
    {
        state_tx_clear(s);
    }
    else if (from_slot >= 0 && from_slot < RNET_MAX_SLOTS)
    {
        state_finish_rx(s, from_slot);
    }
    if (hard_resync)
    {
        rnet_session_hard_resync(s);
    }
    state_recompute_stall(s);
}

void rnet_session_state_finish(RNetSession *s, int hard_resync)
{
    int target;
    if (s == NULL)
    {
        return;
    }
    /* The transfer the last take_ready reported, if it is still open; else
     * whichever take_ready would report now; else (nothing ready) abort
     * everything -- with one transfer open that is exactly the old call. */
    target = s->state_taken;
    if (target == RNET_STATE_TAKEN_TX ? !s->tx.active : (target < 0 || !s->rx[target].active))
    {
        target = state_pick_ready(s);
    }
    if (target < 0)
    {
        rnet_session_state_finish_from(s, RNET_STATE_FROM_ALL, hard_resync);
        return;
    }
    if (target == RNET_STATE_TAKEN_TX)
    {
        state_tx_clear(s);
    }
    else
    {
        state_finish_rx(s, target);
    }
    s->state_taken = -1;
    if (hard_resync)
    {
        rnet_session_hard_resync(s);
    }
    state_recompute_stall(s);
}

int rnet_session_send_rb_frame_commit(RNetSession *s, rnet_u32 through_tick,
                                      rnet_u32 state_hash)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int n;
    if (s == NULL || s->phase != RNET_PHASE_RUNNING)
    {
        return -1;
    }
    n = rnet_proto_encode_rb_frame_commit(buf, sizeof(buf), s->cfg.protocol_magic,
                                          s->cfg.session_id, s->wire_slot,
                                          through_tick, state_hash);
    if (n <= 0)
    {
        return -1;
    }
    if (through_tick == 0u && !s->is_observer)
    {
        /* Kept for spectators: their boot-digest gate needs every seat's
         * tick 0, which is sent once. */
        s->commit0_hash = state_hash;
        s->commit0_valid = 1u;
    }
    send_raw(s, buf, n);
    return 0;
}

int rnet_session_remote_tip(const RNetSession *s, int slot, rnet_u32 *tip)
{
    if (s == NULL || slot < 0 || slot >= (int)s->cfg.slot_count ||
        slot == (int)s->cfg.local_slot || tip == NULL)
    {
        return 0;
    }
    if (!rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot))
    {
        return 0;
    }
    /* A dropped seat has no tip to wait for: unbounded, so neither the
     * prediction cap nor the slowest-tip pacing holds anyone on it. */
    if (occ_absent_now(s, slot))
    {
        return 0;
    }
    *tip = rnet_ring_highest_valid(&s->remote_rings[slot]);
    /* A seat present from wire A has nothing below it to wait for: every row
     * under A is synthesized, so its tip is at least A - 1 (without this a
     * joiner whose first row is A would read as "tip 0" and pace the room). */
    if (s->occ_n != 0)
    {
        rnet_u32 own = occ_own_from(s, slot);
        if (own != 0u && *tip + 1u < own)
            *tip = own - 1u;
    }
    return 1;
}

void rnet_session_set_rb_peer_slot(RNetSession *s, int slot)
{
    if (s == NULL)
    {
        return;
    }
    s->rb_peer_slot = slot;
}

int rnet_session_take_rb_frame_commit(RNetSession *s, rnet_u32 *through_tick,
                                      rnet_u32 *state_hash)
{
    if (s == NULL || s->rb_fc_q_count <= 0)
    {
        return 0;
    }
    if (through_tick)
    {
        *through_tick = s->rb_fc_tick[s->rb_fc_q_tail];
    }
    if (state_hash)
    {
        *state_hash = s->rb_fc_hash[s->rb_fc_q_tail];
    }
    s->rb_last_from = s->rb_fc_from[s->rb_fc_q_tail];
    s->rb_fc_q_tail = (s->rb_fc_q_tail + 1) % RNET_RB_FC_QUEUE;
    s->rb_fc_q_count--;
    return 1;
}

int rnet_session_prepare_local_tip(RNetSession *s, rnet_u32 sim_tick)
{
    rnet_u32 sample_wire;
    RNetInputSample local_future;

    if (s == NULL || s->phase != RNET_PHASE_RUNNING)
    {
        return 0;
    }
    if (sim_tick != s->sim_tick)
    {
        return 0;
    }
    sample_wire = rnet_wire_tick_from_sim(sim_tick, s->delay);
    /* Fill EVERY missing wire in [sim .. sim+D], not just the tip. In steady
     * state only sample_wire is missing (one sample/admit, unchanged). After
     * a mid-session DELAY_SYNC increase the production tip jumps from
     * sim-1+D_old to sim+D_new, leaving [sim+D_old .. sim+D_new-1] unproduced
     * — without the back-fill the peer must invent across the gap. A
     * consumer at wire = sim (real-delay rollback, lockstep play) also
     * relies on the whole prefix existing. Missing rows repeat the current
     * sample (hold-current), matching prime_delay_inputs semantics. */
    {
        rnet_u32 w;
        int have_sample = 0;
        for (w = sim_tick; w <= sample_wire; ++w)
        {
            RNetInputSample existing;
            if (rnet_ring_get(&s->local_ring, w, &existing))
            {
                continue;
            }
            if (!have_sample)
            {
                memset(&local_future, 0, sizeof(local_future));
                if (s->host.sample_local)
                {
                    s->host.sample_local(sim_tick, &local_future, s->host.ctx);
                }
                local_future.valid = 1;
                if (local_future.size > RNET_INPUT_MAX)
                {
                    local_future.size = RNET_INPUT_MAX;
                }
                have_sample = 1;
            }
            local_future.tick = w;
            local_ring_put(s, &local_future);
        }
    }
    send_input_bundle(s);
    return 1;
}

rnet_u32 rnet_session_remote_arrival_age_ms(const RNetSession *s, int slot,
                                            rnet_u32 wire_tick)
{
    rnet_u32 idx;
    rnet_u64 now;
    if (s == NULL || slot < 0 || slot >= (int)s->cfg.slot_count ||
        slot == (int)s->cfg.local_slot)
    {
        return 0xffffffffu;
    }
    idx = wire_tick % RNET_HISTORY_LENGTH;
    if (s->remote_arr_ms[slot][idx] == 0ull ||
        s->remote_arr_tick[slot][idx] != wire_tick)
    {
        return 0xffffffffu;
    }
    now = session_now((RNetSession *)s);
    if (now <= s->remote_arr_ms[slot][idx])
    {
        return 0u;
    }
    return (rnet_u32)(now - s->remote_arr_ms[slot][idx]);
}

int rnet_session_peek_input(const RNetSession *s, int slot, rnet_u32 wire_tick,
                            RNetInputSample *out)
{
    if (s == NULL || out == NULL || slot < 0 || slot >= (int)s->cfg.slot_count)
    {
        return 0;
    }
    if (slot == (int)s->cfg.local_slot)
    {
        return rnet_ring_get(&s->local_ring, wire_tick, out);
    }
    return rnet_ring_get(&s->remote_rings[slot], wire_tick, out);
}

int rnet_session_peek_remote_input(const RNetSession *s, int slot, rnet_u32 wire_tick,
                                   RNetInputSample *out)
{
    if (s == NULL || slot == (int)s->cfg.local_slot)
    {
        return 0;
    }
    if (slot < 0 || slot >= (int)s->cfg.slot_count)
    {
        return 0;
    }
    if (!rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot) || occ_absent_at(s, slot, wire_tick))
    {
        if (out)
        {
            memset(out, 0, sizeof(*out));
            out->tick = wire_tick;
            out->valid = 1;
            /* The room's own empty seats are legacy neutral; a DROPPED seat is
             * flagged so the game can hand it to the CPU. */
            out->absent = rnet_config_slot_occupied(&s->cfg, (rnet_u8)slot) ? 1u : 0u;
        }
        return 1;
    }
    return rnet_session_peek_input(s, slot, wire_tick, out);
}

void rnet_session_set_sim_tick(RNetSession *s, rnet_u32 sim_tick)
{
    if (s == NULL)
    {
        return;
    }
    s->sim_tick = sim_tick;
    apply_pending_delay(s);
}

void rnet_session_clear_remote_inputs(RNetSession *s)
{
    rnet_u8 i;
    if (s == NULL)
    {
        return;
    }
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
    {
        if (i == s->cfg.local_slot)
            continue;
        rnet_ring_clear(&s->remote_rings[i]);
    }
    s->highest_remote_ack = 0;
    for (i = 0; i < RNET_MAX_SLOTS; ++i)
        s->remote_contiguous_ack[i] = 0xffffffffu;
    memset(s->peer_ack_seen, 0, sizeof(s->peer_ack_seen));
}

int rnet_session_send_rb_sync(RNetSession *s, rnet_u32 epoch_id, rnet_u32 mismatch_tick,
                              rnet_u32 load_tick, rnet_u32 target_tick,
                              rnet_u8 corrected_slot, rnet_u8 op, rnet_u8 flags)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int n;
    if (s == NULL || s->phase != RNET_PHASE_RUNNING)
        return -1;
    n = rnet_proto_encode_rb_sync(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                  s->wire_slot, epoch_id, mismatch_tick, load_tick,
                                  target_tick, corrected_slot, op, flags);
    if (n <= 0)
        return -1;
    send_raw(s, buf, n);
    return 0;
}

int rnet_session_take_rb_sync(RNetSession *s, rnet_u32 *epoch_id, rnet_u32 *mismatch_tick,
                              rnet_u32 *load_tick, rnet_u32 *target_tick,
                              rnet_u8 *corrected_slot, rnet_u8 *op, rnet_u8 *flags)
{
    if (s == NULL || s->rb_sync_count <= 0)
        return 0;
    if (epoch_id)
        *epoch_id = s->rb_sync_q[s->rb_sync_tail].epoch_id;
    if (mismatch_tick)
        *mismatch_tick = s->rb_sync_q[s->rb_sync_tail].mismatch_tick;
    if (load_tick)
        *load_tick = s->rb_sync_q[s->rb_sync_tail].load_tick;
    if (target_tick)
        *target_tick = s->rb_sync_q[s->rb_sync_tail].target_tick;
    if (corrected_slot)
        *corrected_slot = s->rb_sync_q[s->rb_sync_tail].corrected_slot;
    if (op)
        *op = s->rb_sync_q[s->rb_sync_tail].initiator;
    if (flags)
        *flags = s->rb_sync_q[s->rb_sync_tail].flags;
    s->rb_last_from = s->rb_sync_q[s->rb_sync_tail].from;
    s->rb_sync_tail = (s->rb_sync_tail + 1) % RNET_RB_CTRL_QUEUE;
    s->rb_sync_count--;
    return 1;
}

int rnet_session_send_rb_seal_rows(RNetSession *s, rnet_u32 epoch_id, rnet_u32 mismatch_tick,
                                   rnet_u32 target_tick, rnet_u8 slot, rnet_u32 row_begin,
                                   const RNetRbFrame *rows, rnet_u16 row_count)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    RNetRbWireFrame wire[RNET_RB_SEAL_ROWS_CHUNK_MAX];
    rnet_u16 i, n;
    int enc;
    if (s == NULL || s->phase != RNET_PHASE_RUNNING || rows == NULL)
        return -1;
    n = row_count;
    if (n > RNET_RB_SEAL_ROWS_CHUNK_MAX)
        n = RNET_RB_SEAL_ROWS_CHUNK_MAX;
    for (i = 0; i < n; ++i)
    {
        wire[i].buttons = rows[i].buttons;
        wire[i].stick_x = (rnet_s8)rows[i].stick_x;
        wire[i].stick_y = (rnet_s8)rows[i].stick_y;
        wire[i].source = rows[i].analog;
        wire[i].is_predicted = rows[i].is_predicted;
        wire[i].is_valid = rows[i].is_valid;
        wire[i].rx = rows[i].rx;
        wire[i].ry = rows[i].ry;
    }
    enc = rnet_proto_encode_rb_seal_rows(buf, sizeof(buf), s->cfg.protocol_magic,
                                         s->cfg.session_id, s->wire_slot, epoch_id,
                                         mismatch_tick, target_tick, slot, row_begin, wire, n);
    if (enc <= 0)
        return -1;
    send_raw(s, buf, enc);
    return 0;
}

int rnet_session_take_rb_seal_rows(RNetSession *s, rnet_u32 *epoch_id, rnet_u32 *mismatch_tick,
                                   rnet_u32 *target_tick, rnet_u8 *slot, rnet_u32 *row_begin,
                                   RNetRbFrame *rows, rnet_u16 *row_count)
{
    rnet_u16 i, n;
    if (s == NULL || s->rb_seal_count <= 0)
        return 0;
    if (epoch_id)
        *epoch_id = s->rb_seal_q[s->rb_seal_tail].epoch_id;
    if (mismatch_tick)
        *mismatch_tick = s->rb_seal_q[s->rb_seal_tail].mismatch_tick;
    if (target_tick)
        *target_tick = s->rb_seal_q[s->rb_seal_tail].target_tick;
    if (slot)
        *slot = s->rb_seal_q[s->rb_seal_tail].slot;
    if (row_begin)
        *row_begin = s->rb_seal_q[s->rb_seal_tail].row_begin;
    n = s->rb_seal_q[s->rb_seal_tail].row_count;
    if (rows && n > 0)
    {
        for (i = 0; i < n; ++i)
        {
            rows[i].tick = s->rb_seal_q[s->rb_seal_tail].row_begin + i;
            rows[i].buttons = s->rb_seal_q[s->rb_seal_tail].rows[i].buttons;
            rows[i].stick_x = (int8_t)s->rb_seal_q[s->rb_seal_tail].rows[i].stick_x;
            rows[i].stick_y = (int8_t)s->rb_seal_q[s->rb_seal_tail].rows[i].stick_y;
            rows[i].analog = s->rb_seal_q[s->rb_seal_tail].rows[i].source;
            rows[i].is_predicted = s->rb_seal_q[s->rb_seal_tail].rows[i].is_predicted;
            rows[i].is_valid = s->rb_seal_q[s->rb_seal_tail].rows[i].is_valid;
            rows[i].rx = s->rb_seal_q[s->rb_seal_tail].rows[i].rx;
            rows[i].ry = s->rb_seal_q[s->rb_seal_tail].rows[i].ry;
        }
    }
    if (row_count)
        *row_count = n;
    s->rb_last_from = s->rb_seal_q[s->rb_seal_tail].from;
    s->rb_seal_tail = (s->rb_seal_tail + 1) % RNET_RB_CTRL_QUEUE;
    s->rb_seal_count--;
    return 1;
}

int rnet_session_send_rb_baseline(RNetSession *s, rnet_u32 epoch_id, rnet_u32 load_tick,
                                  rnet_u32 digest_master, rnet_u32 digest_a, rnet_u32 digest_b,
                                  rnet_u32 digest_c)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int n;
    if (s == NULL || s->phase != RNET_PHASE_RUNNING)
        return -1;
    n = rnet_proto_encode_rb_baseline(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                      s->wire_slot, epoch_id, load_tick, digest_master,
                                      digest_a, digest_b, digest_c);
    if (n <= 0)
        return -1;
    send_raw(s, buf, n);
    return 0;
}

int rnet_session_take_rb_baseline(RNetSession *s, rnet_u32 *epoch_id, rnet_u32 *load_tick,
                                  rnet_u32 *digest_master, rnet_u32 *digest_a, rnet_u32 *digest_b,
                                  rnet_u32 *digest_c)
{
    if (s == NULL || s->rb_base_count <= 0)
        return 0;
    if (epoch_id)
        *epoch_id = s->rb_base_q[s->rb_base_tail].epoch_id;
    if (load_tick)
        *load_tick = s->rb_base_q[s->rb_base_tail].load_tick;
    if (digest_master)
        *digest_master = s->rb_base_q[s->rb_base_tail].digest_master;
    if (digest_a)
        *digest_a = s->rb_base_q[s->rb_base_tail].digest_a;
    if (digest_b)
        *digest_b = s->rb_base_q[s->rb_base_tail].digest_b;
    if (digest_c)
        *digest_c = s->rb_base_q[s->rb_base_tail].digest_c;
    s->rb_last_from = s->rb_base_q[s->rb_base_tail].from;
    s->rb_base_tail = (s->rb_base_tail + 1) % RNET_RB_CTRL_QUEUE;
    s->rb_base_count--;
    return 1;
}

int rnet_session_send_rb_post(RNetSession *s, rnet_u32 epoch_id, rnet_u32 target_tick,
                              rnet_u32 digest_master, rnet_u32 input_digest, rnet_u8 match)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int n;
    if (s == NULL || s->phase != RNET_PHASE_RUNNING)
        return -1;
    n = rnet_proto_encode_rb_post(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                  s->wire_slot, epoch_id, target_tick, digest_master,
                                  input_digest, match);
    if (n <= 0)
        return -1;
    send_raw(s, buf, n);
    return 0;
}

int rnet_session_take_rb_post(RNetSession *s, rnet_u32 *epoch_id, rnet_u32 *target_tick,
                              rnet_u32 *digest_master, rnet_u32 *input_digest, rnet_u8 *match)
{
    if (s == NULL || s->rb_post_count <= 0)
        return 0;
    if (epoch_id)
        *epoch_id = s->rb_post_q[s->rb_post_tail].epoch_id;
    if (target_tick)
        *target_tick = s->rb_post_q[s->rb_post_tail].target_tick;
    if (digest_master)
        *digest_master = s->rb_post_q[s->rb_post_tail].digest_master;
    if (input_digest)
        *input_digest = s->rb_post_q[s->rb_post_tail].input_digest;
    if (match)
        *match = s->rb_post_q[s->rb_post_tail].match;
    s->rb_last_from = s->rb_post_q[s->rb_post_tail].from;
    s->rb_post_tail = (s->rb_post_tail + 1) % RNET_RB_CTRL_QUEUE;
    s->rb_post_count--;
    return 1;
}

int rnet_session_rb_last_take_from(const RNetSession *s)
{
    return (s != NULL) ? s->rb_last_from : -1;
}

rnet_u32 rnet_session_rb_ctrl_dropped(const RNetSession *s)
{
    return (s != NULL) ? s->rb_ctrl_dropped : 0u;
}

int rnet_session_send_sio_multi_xfer(RNetSession *s, rnet_u8 unit_id, rnet_u32 seq,
                                     rnet_u16 send, rnet_u16 confirm_pad)
{
    rnet_u8 buf[64];
    int n;
    if (s == NULL || s->transport.mode == RNET_TRANSPORT_NONE)
        return -1;
    /* Allow during READY/RUNNING so Cable Club can start as soon as UDP is up. */
    if (s->phase != RNET_PHASE_RUNNING && s->phase != RNET_PHASE_READY)
        return -1;
    n = rnet_proto_encode_sio_multi_xfer(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                         s->wire_slot, unit_id, seq, send, confirm_pad);
    if (n <= 0)
        return -1;
    /* Small redundancy: Multi barrier cannot hide behind INPUT FEC. */
    send_raw(s, buf, n);
    send_raw(s, buf, n);
    return 0;
}

int rnet_session_poll_sio_multi_xfer(RNetSession *s, rnet_u8 *unit_id, rnet_u32 *seq,
                                     rnet_u16 *send, rnet_u16 *confirm_pad)
{
    if (s == NULL || s->sio_xfer_count <= 0)
        return 0;
    if (unit_id)
        *unit_id = s->sio_xfer_q[s->sio_xfer_tail].unit_id;
    if (seq)
        *seq = s->sio_xfer_q[s->sio_xfer_tail].seq;
    if (send)
        *send = s->sio_xfer_q[s->sio_xfer_tail].send;
    if (confirm_pad)
        *confirm_pad = s->sio_xfer_q[s->sio_xfer_tail].confirm_pad;
    s->sio_xfer_tail = (s->sio_xfer_tail + 1) % RNET_SIO_XFER_QUEUE;
    s->sio_xfer_count--;
    return 1;
}

int rnet_session_peer_wire_version_mismatch(const RNetSession *s)
{
    if (s == NULL || s->peer_wire_version_mismatch == 0u)
        return 0;
    return s->peer_wire_version_mismatch == 0xFFu ? -1 : (int)s->peer_wire_version_mismatch;
}

int rnet_session_send_modset(RNetSession *s, const char *text)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int n;
    if ((s == NULL) || (text == NULL) || (s->phase != RNET_PHASE_RUNNING))
    {
        return -1;
    }
    n = rnet_proto_encode_modset(buf, sizeof(buf), s->cfg.protocol_magic,
                                 s->cfg.session_id, (rnet_u8)s->wire_slot,
                                 text);
    if (n <= 0)
    {
        return -1;
    }
    /* Kept for spectators: the host re-sends it only until the SEATS have
     * answered, so a spectator that lost it or came later would wait out the
     * mod-set timeout and refuse the match. */
    snprintf(s->modset_sent_text, sizeof(s->modset_sent_text), "%s", text);
    s->modset_sent_valid = 1u;
    rnet_transport_send(&s->transport, buf, (size_t)n);
    return 0;
}

int rnet_session_take_modset(RNetSession *s, char *out, rnet_u32 cap)
{
    if ((s == NULL) || (out == NULL) || (cap == 0u) || (s->modset_pending == 0u))
    {
        return 0;
    }
    s->modset_pending = 0u;
    if ((rnet_u32)strlen(s->modset_text) >= cap)
    {
        return 0; /* caller's buffer cannot hold it; never hand back a prefix */
    }
    memcpy(out, s->modset_text, strlen(s->modset_text) + 1u);
    return 1;
}

int rnet_session_send_modset_ack(RNetSession *s, rnet_u8 status,
                                 const char *reason)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int n;
    if ((s == NULL) || (s->phase != RNET_PHASE_RUNNING))
    {
        return -1;
    }
    n = rnet_proto_encode_modset_ack(buf, sizeof(buf), s->cfg.protocol_magic,
                                     s->cfg.session_id,
                                     (rnet_u8)s->wire_slot, status,
                                     reason);
    if (n <= 0)
    {
        return -1;
    }
    rnet_transport_send(&s->transport, buf, (size_t)n);
    return 0;
}

int rnet_session_take_modset_ack(RNetSession *s, rnet_u8 *status, char *reason,
                                 rnet_u32 cap)
{
    int from;
    if ((s == NULL) || (s->modset_ack_pending == 0u))
    {
        return 0;
    }
    /* Lowest seat first; each seat's answer is taken once. The sender is
     * rnet_session_rb_last_take_from(), as for the rb_* takes. */
    for (from = 0; from < RNET_MAX_SLOTS; ++from)
    {
        if (s->modset_ack_pending & (1u << from))
        {
            break;
        }
    }
    s->modset_ack_pending &= ~(1u << from);
    s->rb_last_from = from;
    if (status != NULL)
    {
        *status = s->modset_ack_status[from];
    }
    if ((reason != NULL) && (cap > 0u))
    {
        rnet_u32 n = (rnet_u32)strlen(s->modset_ack_reason[from]);
        if (n >= cap)
        {
            n = cap - 1u;
        }
        memcpy(reason, s->modset_ack_reason[from], n);
        reason[n] = '\0';
    }
    return 1;
}

int rnet_session_send_rb_resolved(RNetSession *s, rnet_u32 resolved_through)
{
    rnet_u8 buf[RNET_MAX_PACKET];
    int n;
    if (s == NULL || s->phase != RNET_PHASE_RUNNING)
        return -1;
    n = rnet_proto_encode_rb_resolved(buf, sizeof(buf), s->cfg.protocol_magic, s->cfg.session_id,
                                      s->wire_slot, resolved_through);
    if (n <= 0)
        return -1;
    send_raw(s, buf, n);
    return 0;
}

int rnet_session_take_rb_resolved(RNetSession *s, rnet_u32 *resolved_through)
{
    if (s == NULL || s->rb_resolved_count <= 0)
        return 0;
    if (resolved_through)
        *resolved_through = s->rb_resolved_q[s->rb_resolved_tail].through;
    s->rb_last_from = s->rb_resolved_q[s->rb_resolved_tail].from;
    s->rb_resolved_tail = (s->rb_resolved_tail + 1) % RNET_RB_CTRL_QUEUE;
    s->rb_resolved_count--;
    return 1;
}

int rnet_session_observer_refused(const RNetSession *s, rnet_u8 *reason, rnet_u32 *tick,
                                  rnet_u32 *oldest)
{
    if (s == NULL || !s->obs_refused)
        return 0;
    if (reason)
        *reason = s->obs_refuse_reason;
    if (tick)
        *tick = s->obs_refuse_tick;
    if (oldest)
        *oldest = s->obs_refuse_oldest;
    return 1;
}

void rnet_session_observer_counters(const RNetSession *s, rnet_u32 *catchups_sent,
                                    rnet_u32 *rows_served, rnet_u32 *chunks_served)
{
    if (catchups_sent)
        *catchups_sent = s ? s->obs_catchups_sent : 0u;
    if (rows_served)
        *rows_served = s ? s->obs_rows_served : 0u;
    if (chunks_served)
        *chunks_served = s ? s->obs_chunks_served : 0u;
}

int rnet_session_link_rtt_slot(const RNetSession *s, int slot, rnet_u32 *rtt_ms,
                               rnet_u32 *jitter_ms, rnet_u32 *samples)
{
    if (s == NULL || slot < 0 || slot >= RNET_MAX_SLOTS || s->link_samples[slot] == 0u)
    {
        if (samples)
            *samples = 0u;
        return 0;
    }
    if (rtt_ms)
        *rtt_ms = (s->link_rtt_x16[slot] + 8u) / 16u;
    if (jitter_ms)
        *jitter_ms = (s->link_jitter_x16[slot] + 8u) / 16u;
    if (samples)
        *samples = s->link_samples[slot];
    return 1;
}

/* The worst seat: the one whose half RTT plus jitter (what D must cover) is
 * largest. Two seats: the one peer, as before.
 *
 * A HUB host (LAN hub, host-ICE hub) relays every guest's rows to the other
 * guests, so the slowest path in the room is guest -> host -> guest: the two
 * worst legs back to back. The host's controller decides D for everyone, so
 * here it gets that path: RTT and jitter of the two worst seats added (their
 * samples: the smaller count). A guest's own guest-to-guest samples already
 * include both legs. */
int rnet_session_link_rtt(const RNetSession *s, rnet_u32 *rtt_ms, rnet_u32 *jitter_ms,
                          rnet_u32 *samples)
{
    int i, worst = -1, second = -1;
    rnet_u32 worst_need = 0u, second_need = 0u;
    if (s != NULL)
    {
        for (i = 0; i < RNET_MAX_SLOTS; ++i)
        {
            rnet_u32 need;
            if (s->link_samples[i] == 0u)
                continue;
            need = s->link_rtt_x16[i] / 2u + s->link_jitter_x16[i];
            if (worst < 0 || need > worst_need)
            {
                second = worst;
                second_need = worst_need;
                worst = i;
                worst_need = need;
            }
            else if (second < 0 || need > second_need)
            {
                second = i;
                second_need = need;
            }
        }
    }
    if (worst < 0)
    {
        if (samples)
            *samples = 0u;
        return 0;
    }
    if (second >= 0 && s->transport.hub_mode && s->is_sim_authority)
    {
        rnet_u32 r1 = 0, j1 = 0, n1 = 0, r2 = 0, j2 = 0, n2 = 0;
        (void)rnet_session_link_rtt_slot(s, worst, &r1, &j1, &n1);
        (void)rnet_session_link_rtt_slot(s, second, &r2, &j2, &n2);
        if (rtt_ms)
            *rtt_ms = r1 + r2;
        if (jitter_ms)
            *jitter_ms = j1 + j2;
        if (samples)
            *samples = n1 < n2 ? n1 : n2;
        return 1;
    }
    return rnet_session_link_rtt_slot(s, worst, rtt_ms, jitter_ms, samples);
}

int rnet_session_ice_selected(const RNetSession *s, int slot, char *path, size_t path_len,
                              char *local, size_t local_len, char *remote, size_t remote_len)
{
    const RNetIceAgent *a = NULL;
    if (path && path_len) path[0] = '\0';
    if (local && local_len) local[0] = '\0';
    if (remote && remote_len) remote[0] = '\0';
    if (s == NULL)
        return 0;
    if (slot < 0)
        a = s->ice;
    else if (slot < RNET_HUB_MAX_PEERS)
        a = s->ice_hub[slot];
    if (a == NULL)
        return 0;
    rnet_ice_agent_selected_info(a, path, path_len, local, local_len, remote, remote_len);
    return 1;
}
