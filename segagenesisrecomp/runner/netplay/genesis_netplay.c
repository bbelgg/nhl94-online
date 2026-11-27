/* genesis_netplay.c -- N-seat netplay facade over recomp-net (see .h). */
#include "genesis_netplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "recomp_net/recomp_net.h"
#include "recomp_net/lobby_client.h"
#include "recomp_net/host_ice.h"
#include "genesis_netplay_rb.h"
#include "game_spec.h"

#if defined(_WIN32)
#include <windows.h>
static void np_sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void np_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

typedef struct GenesisNetplayState {
    RNetSession *session;
    GenesisNetplayConfig cfg;
    uint16_t staged_buttons;
    int      staged_valid;
    int      active;
    int      rollback;           /* 1 once the rollback host took admission */
    int      slot_count;
    int      local_slot;
    int      spectator;
    int      input_player;
    int      input_delay;
    int      input_prediction;
    int      needs_advance;
    int      latched_for_tick;
    uint32_t latched_sim_tick;
    uint16_t published[GENESIS_NETPLAY_MAX_SLOTS];   /* by session slot */
    uint32_t published_absent;   /* seats dropped from this tick (occupancy schedule) */
    uint8_t  slot_port[GENESIS_NETPLAY_MAX_SLOTS];
    uint32_t occupied_mask;
    uint8_t  pad_type[2];
    int      transport;          /* resolved GenesisNetTransport */
    uint32_t frames_finished;
    /* Validation only (GENESIS_NET_TEST_PAD, harness): a scripted local pad
     * "<period>[:<hold>[:<mask>]]" so a follower produces organic, guest-
     * visible input changes the peer has to predict. */
    uint32_t test_period, test_hold;
    uint16_t test_mask;
    int      sram_sync_sent, sram_sync_done;
    /* Drop policy (see genesis_netplay.h). */
    int      drop_policy;
    unsigned min_humans;
    /* GENESIS_NET_TEST_LEAVE (validation only). */
    int      test_leave_armed, test_leave_crash;
    uint32_t test_leave_tick;
    int      test_leave_seat;
    /* Join in progress (joiner side; see genesis_netplay.h). */
    int      late_join;          /* the room runs late-joinable matches */
    int      late_state;         /* 0 none, 1 catching up, 2 claiming, 3 seated, 4 refused */
    int      late_catching;      /* far behind live: the no-render loop runs */
    int      late_want;          /* session slot asked for, -1 = any */
    uint32_t late_caught_ms;     /* since when within reach of live */
    uint32_t late_log_ms;
    uint32_t late_begin_ms;      /* when the catch-up started (timeouts) */
    uint32_t late_added_agents;  /* host: late spectators handed to the hub */
} GenesisNetplayState;

static uint8_t *g_sram;
static uint32_t g_sram_size;
void genesis_netplay_set_sram(uint8_t *sram, uint32_t size) { g_sram = sram; g_sram_size = sram ? size : 0; }
int  genesis_netplay_sram_writes_allowed(void) { return !genesis_netplay_active() || genesis_netplay_is_host(); }

static GenesisNetplayState g_np;
static int g_return_to_lobby;
static GenesisNetplayRunTick g_tick_runner;

/* ---- env ------------------------------------------------------------------ */

static unsigned env_u(const char *name, unsigned fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long parsed;
    if (!value || !value[0]) return fallback;
    parsed = strtoul(value, &end, 0);
    return end && end != value ? (unsigned)parsed : fallback;
}

void genesis_netplay_config_defaults(GenesisNetplayConfig *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->slot_count = 2;
    cfg->input_delay = 2;
    cfg->rollback = 1;          /* rollback is the default mode */
    cfg->session_id = 1;
    strcpy(cfg->bind_hostport, "0.0.0.0:7777");
    for (int i = 0; i < GENESIS_NETPLAY_MAX_SLOTS; i++) {
        cfg->slot_port[i] = (uint8_t)i;
        cfg->lobby_seat[i] = -1;
    }
}

void genesis_netplay_apply_env(GenesisNetplayConfig *cfg)
{
    const char *v;
    if (!cfg) return;
    if ((v = getenv("GENESIS_NETPLAY"))) cfg->enabled = atoi(v) != 0;
    if ((v = getenv("GENESIS_NET_SLOT"))) cfg->local_slot = atoi(v);
    if ((v = getenv("GENESIS_NET_SLOTS"))) cfg->slot_count = atoi(v);
    if ((v = getenv("GENESIS_NET_SPECTATOR"))) cfg->spectator = atoi(v) != 0;
    if ((v = getenv("GENESIS_NET_SPECTATOR_WIRE_SLOT"))) cfg->spectator_wire_slot = atoi(v);
    if ((v = getenv("GENESIS_NET_SPECTATORS"))) cfg->expected_spectators = atoi(v);
    if ((v = getenv("GENESIS_NET_INPUT_PLAYER"))) cfg->input_player = atoi(v);
    /* GENESIS_NET_DELAY pins D (no auto delay). GENESIS_NET_DELAY_SEED starts
     * from a value the way a lobby launch does, auto delay on (harnesses). */
    if ((v = getenv("GENESIS_NET_DELAY_SEED")) && v[0]) cfg->input_delay = atoi(v);
    if ((v = getenv("GENESIS_NET_DELAY"))) cfg->input_delay = atoi(v);
    if ((v = getenv("GENESIS_NET_PREDICTION"))) cfg->input_prediction = atoi(v);
    if ((v = getenv("GENESIS_NET_MODE")) && v[0]) {
        if (!strcmp(v, "rollback") || !strcmp(v, "rb")) cfg->rollback = 1;
        else if (!strcmp(v, "delay")) cfg->rollback = 0;
    }
    cfg->session_id = env_u("GENESIS_NET_SESSION_ID", cfg->session_id);
    cfg->occupied_mask = env_u("GENESIS_NET_OCCUPIED", cfg->occupied_mask);
    if ((v = getenv("GENESIS_NET_LATE_JOIN"))) cfg->late_join = atoi(v) != 0;
    if ((v = getenv("GENESIS_NET_LATE_LAUNCH"))) cfg->late_launch = atoi(v) != 0;
    cfg->open_mask = env_u("GENESIS_NET_OPEN", cfg->open_mask);
    if ((v = getenv("GENESIS_NET_BIND")) && v[0])
        snprintf(cfg->bind_hostport, sizeof cfg->bind_hostport, "%s", v);
    if ((v = getenv("GENESIS_NET_PEER")) && v[0])
        snprintf(cfg->peer_hostport, sizeof cfg->peer_hostport, "%s", v);
    if ((v = getenv("GENESIS_NET_TRANSPORT")) && v[0]) {
        if (!strcmp(v, "ice")) cfg->transport = GENESIS_NET_TRANSPORT_ICE;
        else if (!strcmp(v, "lan")) cfg->transport = GENESIS_NET_TRANSPORT_LAN;
        else if (!strcmp(v, "hub")) cfg->transport = GENESIS_NET_TRANSPORT_HUB;
        else if (!strcmp(v, "relay")) cfg->transport = GENESIS_NET_TRANSPORT_RELAY;
        else if (!strcmp(v, "ice-hub")) cfg->transport = GENESIS_NET_TRANSPORT_ICE_HUB;
    }
    /* Relay-only ICE through the lobby's TURN (validation / restrictive
     * networks). The lobby selftest host also publishes it in the room caps
     * (Force TURN), so every peer's agents go relay-only. */
    if ((v = getenv("GENESIS_NET_FORCE_TURN")) && v[0]) cfg->force_turn = atoi(v) != 0;
    if ((v = getenv("GENESIS_NET_SLOT_PORTS")) && v[0]) {
        /* "a,b,c,d": session slot i plays logical player slot_port[i]. */
        int i = 0;
        for (const char *p = v; *p && i < GENESIS_NETPLAY_MAX_SLOTS; i++) {
            cfg->slot_port[i] = (uint8_t)atoi(p);
            p = strchr(p, ',');
            if (!p) break;
            p++;
        }
        cfg->slot_port_valid = 1;
    }
}

/* ---- pad blob ---------------------------------------------------------------- */

static void encode_pad(uint16_t buttons, RNetInputSample *out, rnet_u32 tick)
{
    memset(out, 0, sizeof(*out));
    out->tick = tick;
    out->size = GENESIS_NETPLAY_PAD_BYTES;
    out->bytes[0] = (rnet_u8)(buttons & 0xFFu);
    out->bytes[1] = (rnet_u8)((buttons >> 8) & 0x0Fu);
    /* [2..3] sync bytes: no Genesis game needs them yet; kept zero. */
    out->valid = 1;
}

static uint16_t decode_pad(const RNetInputSample *in)
{
    if (!in || !in->valid || in->size < 2) return 0;
    return (uint16_t)((in->bytes[0] | ((uint16_t)in->bytes[1] << 8)) & GENESIS_NETPLAY_PAD_MASK);
}

static uint16_t test_pad(uint32_t tick)
{
    if (!g_np.test_period) return 0;
    return (tick % g_np.test_period) < g_np.test_hold ? g_np.test_mask : 0;
}

static void host_sample_local(rnet_u32 tick, RNetInputSample *out, void *ctx)
{
    GenesisNetplayState *st = (GenesisNetplayState *)ctx;
    uint16_t b = st->staged_valid ? st->staged_buttons : 0;
    if (st->test_period) b = test_pad(tick);
    encode_pad(b, out, tick);
}

static void apply_published(const uint16_t *rows, int slots, uint32_t absent)
{
    { static int dbg = -1; if (dbg < 0) dbg = getenv("GENESIS_NET_DEBUG_STAGE") != NULL;
      static uint16_t last[GENESIS_NETPLAY_MAX_SLOTS];
      for (int i = 0; dbg && rows && i < slots && i < GENESIS_NETPLAY_MAX_SLOTS; i++)
          if (rows[i] != last[i]) { fprintf(stderr, "[publish] sim=%u slot=%d rows=%03x\n",
                                            (unsigned)genesis_netplay_sim_tick(), i, rows[i]); last[i] = rows[i]; } }
    memset(g_np.published, 0, sizeof g_np.published);
    g_np.published_absent = absent;
    for (int i = 0; rows && i < slots && i < GENESIS_NETPLAY_MAX_SLOTS; i++)
        g_np.published[i] = rows[i] & GENESIS_NETPLAY_PAD_MASK;
}

/* Delay-sync publish (the rollback host publishes through np_rb_publish). */
static void host_publish(rnet_u32 tick, const RNetInputSample *by_slot, int slots, void *ctx)
{
    uint16_t rows[GENESIS_NETPLAY_MAX_SLOTS] = {0};
    (void)tick; (void)ctx;
    for (int i = 0; by_slot && i < slots && i < GENESIS_NETPLAY_MAX_SLOTS; i++)
        rows[i] = decode_pad(&by_slot[i]);
    apply_published(rows, slots, 0);
}

static void np_rb_publish(uint32_t tick, const uint16_t *rows, int slots, uint32_t absent)
{
    (void)tick;
    apply_published(rows, slots, absent);
}

/* ---- ICE signalling through the lobby WebSocket ------------------------------- */

#if defined(RNET_ENABLE_ICE)
static void host_on_signal(const RNetSignal *message, void *ctx)
{
    (void)ctx;
    if (message)
        (void)rnet_lobby_send_signal((int)message->type, (int)message->flag, message->text);
}
#endif

static void drain_lobby_signals(void)
{
#if defined(RNET_ENABLE_ICE)
    int type = 0, flag = 0;
    char text[2048];
    if (g_np.transport != GENESIS_NET_TRANSPORT_ICE) return;
    while (g_np.session && rnet_lobby_poll_signal(&type, &flag, text, sizeof(text))) {
        RNetSignal signal;
        memset(&signal, 0, sizeof(signal));
        if (type == (int)RNET_SIGNAL_LOCAL_SDP) type = (int)RNET_SIGNAL_REMOTE_SDP;
        else if (type == (int)RNET_SIGNAL_LOCAL_CANDIDATE) type = (int)RNET_SIGNAL_REMOTE_CANDIDATE;
        signal.type = (RNetSignalType)type;
        signal.flag = (rnet_u8)(flag & 0xff);
        snprintf(signal.text, sizeof signal.text, "%s", text);
        rnet_session_push_signal(g_np.session, &signal);
    }
#endif
}

/* ---- queries ------------------------------------------------------------------- */

int genesis_netplay_active(void) { return g_np.active && g_np.session; }
int genesis_netplay_rollback_active(void) { return genesis_netplay_active() && g_np.rollback; }
int genesis_netplay_is_running(void)
{
    return genesis_netplay_active() && rnet_session_is_running(g_np.session);
}
int genesis_netplay_is_spectator(void) { return genesis_netplay_active() && g_np.spectator; }
int genesis_netplay_local_slot(void) { return genesis_netplay_active() ? g_np.local_slot : -1; }
int genesis_netplay_local_player(void)
{
    int s = genesis_netplay_local_slot();
    return !g_np.spectator && s >= 0 && s < g_np.slot_count && s < GENESIS_NETPLAY_MAX_SLOTS
        ? g_np.slot_port[s] : -1;
}
int genesis_netplay_slot_count(void) { return genesis_netplay_active() ? g_np.slot_count : 2; }
int genesis_netplay_input_player(void) { return genesis_netplay_active() ? g_np.input_player : 0; }
int genesis_netplay_is_host(void) { return genesis_netplay_active() && g_np.local_slot == 0; }
uint32_t genesis_netplay_session_id(void) { return g_np.cfg.session_id; }
uint32_t genesis_netplay_sim_tick(void)
{
    if (!genesis_netplay_active()) return 0;
    return g_np.rollback ? genesis_netplay_rb_sim_tick() : rnet_session_sim_tick(g_np.session);
}

const char *genesis_netplay_transport_name(void)
{
    if (!genesis_netplay_active()) return "none";
    switch (g_np.transport) {
    case GENESIS_NET_TRANSPORT_ICE:   return "ice";
    case GENESIS_NET_TRANSPORT_HUB:   return "lan-hub";
    case GENESIS_NET_TRANSPORT_RELAY: return "relay";
    case GENESIS_NET_TRANSPORT_ICE_HUB: return "ice-hub";
    default:                          return "lan";
    }
}

void genesis_netplay_request_return_to_lobby(void) { g_return_to_lobby = 1; }
int  genesis_netplay_return_to_lobby_requested(void) { return g_return_to_lobby; }
void genesis_netplay_clear_return_to_lobby(void) { g_return_to_lobby = 0; }
/* A spectator a seat refused (recomp-net OBS_REFUSE): it cannot follow this
 * match any more, and says why instead of stalling. */
static char s_spectator_refusal[256];
static int spectator_refused(void)
{
    rnet_u8 reason = 0;
    rnet_u32 tick = 0, oldest = 0;
    if (s_spectator_refusal[0]) return 1;
    if (!g_np.spectator || !g_np.session ||
        !rnet_session_observer_refused(g_np.session, &reason, &tick, &oldest))
        return 0;
    if (reason == RNET_OBS_REFUSE_LATE_JOIN)
        snprintf(s_spectator_refusal, sizeof s_spectator_refusal,
                 "spectator_late_join: the match was already at tick %u (a spectator can join "
                 "until tick %u; later needs a state transfer this build does not have)",
                 (unsigned)tick, (unsigned)RNET_OBS_JOIN_MAX_TICK);
    else
        snprintf(s_spectator_refusal, sizeof s_spectator_refusal,
                 "spectator_rows_gone: needed row %u, the seats' history starts at %u (fell "
                 "too far behind; needs a state transfer this build does not have)",
                 (unsigned)tick, (unsigned)oldest);
    fprintf(stderr, "genesis_netplay: SPECTATOR CANNOT CONTINUE -- %s\n", s_spectator_refusal);
    return 1;
}

/* Why this peer left a drop-in match (dropped / host gone / no humans), shown
 * in the lobby like a refusal. Set by genesis_netplay_match_should_end. */
static char s_drop_msg[160];
/* Why a seat claim was refused ("no free seat ... you can keep watching"): the
 * peer keeps spectating, so it is NOT a reason the match ended and must not
 * stand in for the real one in genesis_netplay_refusal (it used to: a later
 * desync or quit then showed "no free seat"). */
static char s_claim_msg[160];
const char *genesis_netplay_claim_message(void) { return s_claim_msg; }
const char *genesis_netplay_refusal(void)
{
    if (s_spectator_refusal[0]) return s_spectator_refusal;
    if (s_drop_msg[0]) return s_drop_msg;
    return genesis_netplay_rollback_active() ? genesis_netplay_rb_refusal() : NULL;
}
int genesis_netplay_quiesced(void)
{
    return genesis_netplay_rollback_active() && genesis_netplay_rb_quiesced();
}

int genesis_netplay_draining(void)
{
    return genesis_netplay_rollback_active() && genesis_netplay_rb_draining();
}

void genesis_netplay_request_quiesce(void)
{
    if (genesis_netplay_rollback_active()) genesis_netplay_rb_request_quiesce();
}

void genesis_netplay_set_tick_runner(GenesisNetplayRunTick fn) { g_tick_runner = fn; }

void genesis_netplay_sim_input(GenesisSimInput *out)
{
    memset(out, 0, sizeof *out);
    out->pad_type[0] = g_np.pad_type[0];
    out->pad_type[1] = g_np.pad_type[1];
    for (int s = 0; s < g_np.slot_count && s < GENESIS_NETPLAY_MAX_SLOTS; s++) {
        int p = g_np.slot_port[s];
        if (p < 0 || p >= GENESIS_SIM_MAX_PLAYERS) continue;
        if (g_np.occupied_mask && !(g_np.occupied_mask & (1u << s))) continue;
        /* A seat the authority dropped is no human from the tick its schedule
         * says: zero pad, human_mask bit clear -> the game hands it to the CPU
         * (NHL94: enforce() releases seats not in human_mask). The mask and the
         * pads travel to the sim only through this sealed input, so a rollback
         * replays them exactly. */
        if (g_np.published_absent & (1u << s)) continue;
        out->pad[p] = g_np.published[s];
        out->human_mask |= 1u << p;
    }
}

uint16_t genesis_netplay_published_pad(int player)
{
    GenesisSimInput in;
    if (player < 0 || player >= GENESIS_SIM_MAX_PLAYERS) return 0;
    genesis_netplay_sim_input(&in);
    return in.pad[player];
}

/* ---- rollback host binding ------------------------------------------------------ */

static void np_rb_run_tick(void)
{
    GenesisSimInput in;
    genesis_netplay_sim_input(&in);
    if (g_tick_runner) g_tick_runner(&in);
}

static void np_rollback_try_start(void)
{
    GenesisNetplayRbBindings b;
    g_np.rollback = 0;
    if (!g_np.cfg.rollback) return;
    memset(&b, 0, sizeof b);
    b.session = &g_np.session;
    b.local_slot = &g_np.local_slot;
    b.slot_count = &g_np.slot_count;
    b.input_delay = &g_np.input_delay;
    b.input_prediction = &g_np.input_prediction;
    b.occupied_mask = g_np.occupied_mask;
    b.publish = np_rb_publish;
    b.run_tick = np_rb_run_tick;
    genesis_netplay_rb_bind(&b);
    if (genesis_netplay_rb_start()) {
        g_np.rollback = 1;
    } else {
        /* Doctrine: abort rather than silently degrade. A peer that fell back
         * to delay-sync while the others run rollback would never agree on an
         * admission protocol; refuse the match instead. */
        fprintf(stderr, "genesis_netplay: rollback host failed to start — refusing the match\n");
        genesis_netplay_rb_bind(NULL);
        g_return_to_lobby = 1;
    }
}

/* ---- start / stop ---------------------------------------------------------------- */

static int resolve_transport(const GenesisNetplayConfig *cfg, int seats)
{
    if (cfg->transport == GENESIS_NET_TRANSPORT_ICE) {
#if defined(RNET_ENABLE_ICE)
        if (seats != 2 || cfg->spectator) {
            fprintf(stderr, "genesis_netplay: ICE carries exactly two players; "
                            "%d seats%s need the relay\n", seats,
                    cfg->spectator ? " and a spectator" : "");
            return -1;
        }
        return GENESIS_NET_TRANSPORT_ICE;
#else
        fprintf(stderr, "genesis_netplay: ICE requested but this build has no ICE\n");
        return -1;
#endif
    }
    if (cfg->transport == GENESIS_NET_TRANSPORT_ICE_HUB) {
#if defined(RNET_ENABLE_ICE)
        return GENESIS_NET_TRANSPORT_ICE_HUB;
#else
        fprintf(stderr, "genesis_netplay: the host-ICE hub needs ICE; this build has none\n");
        return -1;
#endif
    }
    if (cfg->transport == GENESIS_NET_TRANSPORT_RELAY) return GENESIS_NET_TRANSPORT_RELAY;
    if (cfg->transport == GENESIS_NET_TRANSPORT_HUB) return GENESIS_NET_TRANSPORT_HUB;
    if (cfg->transport == GENESIS_NET_TRANSPORT_LAN) {
        if (seats > 2) return GENESIS_NET_TRANSPORT_HUB;
        return GENESIS_NET_TRANSPORT_LAN;
    }
    /* auto */
    return seats > 2 ? GENESIS_NET_TRANSPORT_HUB : GENESIS_NET_TRANSPORT_LAN;
}

#if defined(RNET_ENABLE_ICE)
static char s_ice_stun[128], s_ice_turn[128], s_ice_user[192], s_ice_pass[128];
static int start_ice(const GenesisNetplayConfig *cfg)
{
    RNetIceConfig ice;
    rnet_ice_config_init_defaults(&ice);
    ice.controlling = g_np.local_slot == 0 ? 1u : 0u;
    if (rnet_lobby_connected()) {
        (void)rnet_lobby_request_turn_credentials();
        for (int i = 0; i < 50; i++) {
            const RNetLobbyTurnCredentials *tc = rnet_lobby_turn_credentials();
            if (tc && tc->valid) break;
            rnet_lobby_pump();
            np_sleep_ms(10);
        }
        const RNetLobbyTurnCredentials *tc = rnet_lobby_turn_credentials();
        if (tc && tc->valid) {
            if (tc->stun_host[0]) {
                snprintf(s_ice_stun, sizeof s_ice_stun, "%s", tc->stun_host);
                ice.stun_host = s_ice_stun;
                ice.stun_port = (rnet_u16)(tc->stun_port > 0 ? tc->stun_port : 3478);
            }
            snprintf(s_ice_turn, sizeof s_ice_turn, "%s", tc->turn_host);
            snprintf(s_ice_user, sizeof s_ice_user, "%s", tc->username);
            snprintf(s_ice_pass, sizeof s_ice_pass, "%s", tc->password);
            ice.turn_host = s_ice_turn; ice.turn_user = s_ice_user; ice.turn_pass = s_ice_pass;
            ice.turn_port = (rnet_u16)(tc->turn_port > 0 ? tc->turn_port : 3478);
        }
    }
    if (cfg->force_turn) {
        if (!ice.turn_host) {
            fprintf(stderr, "genesis_netplay: FORCE_TURN without TURN credentials — refusing\n");
            return -1;
        }
        ice.force_relay = 1;
    }
    return rnet_session_start_ice(g_np.session, &ice);
}

/* One adopted agent's selected pair, for the log (host / srflx / prflx /
 * relay: "relay" = through the lobby's TURN). */
static void log_agent_path(const char *who, int slot, int agent_slot)
{
    char path[16] = "", local[96] = "", remote[96] = "";
    rnet_session_ice_selected(g_np.session, agent_slot, path, sizeof path, local, sizeof local,
                              remote, sizeof remote);
    fprintf(stderr, "genesis_netplay: %s %s slot %d path=%s local=%s remote=%s\n",
            g_np.transport == GENESIS_NET_TRANSPORT_ICE ? "ice" : "ice-hub", who, slot,
            path[0] ? path : "?", local, remote);
}

/* The host-ICE hub: the waiting room already connected one agent per guest
 * (and per spectator) to the host; the session adopts them -- nothing is
 * renegotiated, no port is bound. Host: each agent goes to its guest's
 * SESSION slot (HOST_FIRST: the launch's slot_port says which lobby seat
 * each session slot holds) or, for a spectator, to its wire slot. */
static int start_ice_hub(const GenesisNetplayConfig *cfg, int seats)
{
    if (!cfg->spectator && g_np.local_slot == 0) {
        RNetLobbyIceSeat got[RNET_HOST_ICE_MAX_PEERS];
        RNetIceAdoptSeat adopt[RNET_HOST_ICE_MAX_PEERS];
        uint32_t have = 0;
        int n = rnet_lobby_ice_take_hub(got, RNET_HOST_ICE_MAX_PEERS), m = 0, bad = 0;
        if (n < 1) {
            fprintf(stderr, "genesis_netplay: ice-hub: no connected agents to adopt (%s)\n",
                    rnet_lobby_ice_launch_error());
            return -1;
        }
        for (int i = 0; i < n; i++) {
            int slot = -1;
            if (got[i].is_spectator) {
                slot = got[i].wire_slot;
                if (slot < seats || slot >= RNET_HUB_MAX_PEERS) slot = -1;
            } else {
                for (int k = 1; k < seats && k < GENESIS_NETPLAY_MAX_SLOTS; k++)
                    if (cfg->lobby_seat[k] == got[i].lobby_slot) { slot = k; break; }
            }
            if (slot < 1 || (have & (1u << slot))) {
                fprintf(stderr, "genesis_netplay: ice-hub: %s \"%s\" (lobby seat %d) has no session "
                                "slot here -- refusing\n", got[i].is_spectator ? "spectator" : "guest",
                        got[i].player_id, got[i].lobby_slot);
                bad = 1;
                break;
            }
            have |= 1u << slot;
            adopt[m].slot = slot;
            adopt[m].agent = got[i].agent;
            m++;
        }
        for (int k = 1; !bad && k < seats; k++) {
            if (g_np.cfg.occupied_mask && !(g_np.cfg.occupied_mask & (1u << k))) continue;
            if (cfg->open_mask & (1u << k)) continue;   /* open at launch: nobody to connect */
            if (!(have & (1u << k))) {
                fprintf(stderr, "genesis_netplay: ice-hub: seat %d has no connected agent -- "
                                "refusing\n", k);
                bad = 1;
            }
        }
        if (!bad && rnet_session_start_ice_hub_adopt(g_np.session, adopt, m) != 0) bad = 1;
        if (bad) {
            for (int i = 0; i < n; i++) rnet_host_ice_destroy_agent(got[i].agent);
            return -1;
        }
        for (int i = 0; i < m; i++)
            log_agent_path(adopt[i].slot >= seats ? "spectator" : "guest", adopt[i].slot, adopt[i].slot);
        fprintf(stderr, "genesis_netplay: ice-hub host carries %d agents (%d seats, %d spectators)\n",
                m, seats - 1, m - (seats - 1));
        return 0;
    }
    {
        RNetIceAgent *a = rnet_lobby_ice_take_guest_agent();
        if (!a) {
            fprintf(stderr, "genesis_netplay: ice-hub: no connected agent to the host (%s)\n",
                    rnet_lobby_ice_launch_error());
            return -1;
        }
        if (rnet_session_adopt_ice_agent(g_np.session, a) != 0) {
            rnet_host_ice_destroy_agent(a);
            return -1;
        }
        log_agent_path(cfg->spectator ? "spectator->host" : "guest->host", g_np.local_slot, -1);
        return 0;
    }
}
#endif

/* Validation knobs of a seated peer; a joiner arms them when it is granted
 * its seat (it has no pad and no seat before). */
static void arm_test_pad(void)
{
    const char *tp = getenv("GENESIS_NET_TEST_PAD");
    if (tp && tp[0] && !g_np.spectator) {
        unsigned a = 0, b = 0, m = 0;
        int n = sscanf(tp, "%u:%u:%x", &a, &b, &m);
        g_np.test_period = a;
        g_np.test_hold = n >= 2 ? b : (a > 1 ? a / 2 : 1);
        g_np.test_mask = (uint16_t)(n >= 3 ? m : 0x0008u /* RIGHT */);
        fprintf(stderr, "genesis_netplay: TEST PAD period=%u hold=%u mask=%03x "
                        "(validation only)\n", g_np.test_period, g_np.test_hold, g_np.test_mask);
    }
}

static void arm_test_leave(void)
{
    const char *tl = getenv("GENESIS_NET_TEST_LEAVE");
    if (tl && tl[0] && !g_np.spectator) {
        int seat = -1;
        const char *at = strchr(tl, '@');
        g_np.test_leave_tick = (uint32_t)strtoul(tl, NULL, 0);
        g_np.test_leave_crash = strstr(tl, ":crash") != NULL;
        if (at) seat = atoi(at + 1);
        g_np.test_leave_seat = seat;
        g_np.test_leave_armed = seat < 0 || seat == g_np.local_slot;
        if (g_np.test_leave_armed)
            fprintf(stderr, "genesis_netplay: TEST LEAVE at sim tick %u (%s) (validation only)\n",
                    (unsigned)g_np.test_leave_tick, g_np.test_leave_crash ? "crash" : "graceful");
    }
}

int genesis_netplay_start(const GenesisNetplayConfig *cfg)
{
    RNetConfig rcfg;
    RNetHostVTable host;
    int seats, transport, rc = 0;
    uint32_t eff_occupied;

    if (!cfg || !cfg->enabled) return -1;
    genesis_netplay_shutdown();
    g_return_to_lobby = 0;
    s_spectator_refusal[0] = 0;
    s_drop_msg[0] = 0;
    s_claim_msg[0] = 0;
    g_np.cfg = *cfg;

    seats = cfg->slot_count > 0 ? cfg->slot_count : 2;
    if (seats > GENESIS_NETPLAY_MAX_SLOTS) {
        fprintf(stderr, "genesis_netplay: %d seats requested; Genesis carries at most %d "
                        "logical players — refusing\n", seats, GENESIS_NETPLAY_MAX_SLOTS);
        return -1;
    }
    transport = resolve_transport(cfg, seats);
    if (transport < 0) return -4;
    if (cfg->spectator && transport != GENESIS_NET_TRANSPORT_RELAY &&
        transport != GENESIS_NET_TRANSPORT_HUB && transport != GENESIS_NET_TRANSPORT_ICE_HUB) {
        fprintf(stderr, "genesis_netplay: a spectator needs a relay (the lobby's input "
                        "relay, a LAN hub or the host-ICE hub) — refusing rather than showing "
                        "a black screen\n");
        return -5;
    }

    rnet_config_init_defaults(&rcfg);
    rcfg.slot_count = (rnet_u8)(seats < 2 ? 2 : seats);
    if (cfg->spectator) {
        if (cfg->spectator_wire_slot < seats || cfg->spectator_wire_slot > 0xff) {
            fprintf(stderr, "genesis_netplay: spectator without a usable relay slot (%d, "
                            "seats=%d) — refusing\n", cfg->spectator_wire_slot, seats);
            return -1;
        }
        rcfg.local_slot = (rnet_u8)rcfg.slot_count;   /* observer sentinel */
        rcfg.wire_slot = (rnet_u8)cfg->spectator_wire_slot;
    } else {
        int slot = cfg->local_slot < 0 ? 0 : cfg->local_slot;
        if (slot >= rcfg.slot_count) slot = rcfg.slot_count - 1;
        rcfg.local_slot = (rnet_u8)slot;
    }
    rcfg.input_delay = (rnet_u8)(cfg->input_delay < 0 ? 0 : cfg->input_delay > 20 ? 20 : cfg->input_delay);
    {
        /* Where D comes from, said once. GENESIS_NET_DELAY pins it (no auto
         * delay, no floor); otherwise the launch's seed is only a start: the
         * host's auto delay fits D to the measured game-path RTT within the
         * first ~5-10 s (recomp-net rnet_sched.c np_link_delay). A guest
         * always plays the host's D (HELLO), whatever it was given. */
        const char *ov = getenv("GENESIS_NET_DELAY");
        int pinned = ov && ov[0];
        if (pinned && !getenv("RBE_RB_AUTO_DELAY"))
#if defined(_WIN32)
            _putenv_s("RBE_RB_AUTO_DELAY", "0");
#else
            setenv("RBE_RB_AUTO_DELAY", "0", 0);
#endif
        fprintf(stderr, "genesis_netplay: input delay D=%u (%s)%s\n", (unsigned)rcfg.input_delay,
                pinned ? "GENESIS_NET_DELAY override; auto delay off"
                       : "launch seed; auto delay fits it to the measured link",
                (!cfg->spectator && cfg->local_slot != 0) ? " -- a guest adopts the host's D" : "");
    }
    rcfg.session_id = cfg->session_id ? cfg->session_id : 1u;
    eff_occupied = cfg->occupied_mask;
    /* Join in progress needs the rollback host (the occupancy schedule and the
     * barrier live in its driver) and the drop policy's 3+ seats. */
    if (cfg->late_join && cfg->rollback && seats >= 3 && getenv("GENESIS_NET_LATE_JOIN_OFF") == NULL) {
        rcfg.late_join = 1;
        rcfg.open_mask = cfg->open_mask;
    } else if (cfg->late_join || cfg->open_mask) {
        /* Without it an open seat would be a seat the match waits on for ever:
         * take it out of the room instead (the old sparse-room rule). */
        uint32_t all = seats >= 32 ? 0xffffffffu : ((1u << seats) - 1u);
        if (!eff_occupied) eff_occupied = all;
        eff_occupied &= ~cfg->open_mask;
        fprintf(stderr, "genesis_netplay: join in progress OFF for this match (rollback=%d seats=%d); "
                        "open seats %x are not in the room\n", cfg->rollback, seats,
                (unsigned)cfg->open_mask);
    }
    rcfg.occupied_mask = eff_occupied;
    /* The start barrier waits for rostered spectators only where their READY
     * can reach the host: a hub (LAN, the host's own port, or the host-ICE
     * hub). The lobby's server relay drops a spectator's packets, and
     * two-seat ICE has none. */
    if (!cfg->spectator && rcfg.local_slot == 0 &&
        (transport == GENESIS_NET_TRANSPORT_HUB || transport == GENESIS_NET_TRANSPORT_ICE_HUB) &&
        cfg->expected_spectators > 0)
        rcfg.expected_observers = (rnet_u8)(cfg->expected_spectators > 255 ? 255
                                                                          : cfg->expected_spectators);

    memset(&host, 0, sizeof host);
    host.sample_local = host_sample_local;
    host.publish = host_publish;
    host.ctx = &g_np;
#if defined(RNET_ENABLE_ICE)
    if (transport == GENESIS_NET_TRANSPORT_ICE) host.on_signal = host_on_signal;
#endif
    g_np.session = rnet_session_create(&rcfg, &host);
    if (!g_np.session) return -2;
    g_np.local_slot = (int)rcfg.local_slot;
    g_np.transport = transport;

    switch (transport) {
    case GENESIS_NET_TRANSPORT_ICE:
#if defined(RNET_ENABLE_ICE)
        rc = start_ice(cfg);
#else
        rc = -1;
#endif
        break;
    case GENESIS_NET_TRANSPORT_ICE_HUB:
#if defined(RNET_ENABLE_ICE)
        rc = start_ice_hub(cfg, (int)rcfg.slot_count);
#else
        rc = -1;
#endif
        break;
    case GENESIS_NET_TRANSPORT_HUB:
        rc = (!cfg->spectator && rcfg.local_slot == 0)
                 ? rnet_session_start_lan_hub(g_np.session, cfg->bind_hostport)
                 : rnet_session_start_lan(g_np.session, cfg->bind_hostport, cfg->peer_hostport);
        break;
    default:   /* LAN and the lobby relay are the same UDP transport */
        rc = rnet_session_start_lan(g_np.session, cfg->bind_hostport, cfg->peer_hostport);
        break;
    }
    if (rc != 0) {
        fprintf(stderr, "genesis_netplay: transport %s failed to start\n",
                genesis_netplay_transport_name());
        rnet_session_destroy(g_np.session);
        memset(&g_np, 0, sizeof g_np);
        return -3;
    }

    g_np.active = 1;
    g_np.slot_count = (int)rcfg.slot_count;
    g_np.spectator = cfg->spectator ? 1 : 0;
    g_np.input_player = cfg->input_player >= 0 && cfg->input_player < GENESIS_SIM_MAX_PLAYERS
                            ? cfg->input_player : 0;
    g_np.input_delay = (int)rcfg.input_delay;
    g_np.input_prediction = cfg->input_prediction;
    if (g_np.input_prediction && g_np.input_prediction < 2) g_np.input_prediction = 2;
    if (g_np.input_prediction > 32) g_np.input_prediction = 32;
    g_np.occupied_mask = eff_occupied;
    g_np.late_join = rcfg.late_join;
    g_np.late_want = -1;
    g_np.late_state = (rcfg.late_join && cfg->spectator && cfg->late_launch) ? 1 : 0;
    { const char *ls = getenv("GENESIS_NET_LATE_SEAT"); if (ls && ls[0]) g_np.late_want = atoi(ls); }
    for (int i = 0; i < GENESIS_NETPLAY_MAX_SLOTS; i++)
        g_np.slot_port[i] = cfg->slot_port_valid || cfg->slot_port[i] ? cfg->slot_port[i] : (uint8_t)i;
    arm_test_pad();

    np_rollback_try_start();
    /* Join in progress: a joiner may only be given a seat that has a ROLE under
     * this match's game image (NHL94: the seat list the host sealed -- a seat
     * past its end has none and would be a human with nothing to control). */
    if (g_np.late_join && g_np.local_slot == 0 && !g_np.spectator && g_game_spec.netplay_seat_label) {
        uint32_t ok = 0;
        char lab[64];
        for (int k = 1; k < g_np.slot_count && k < GENESIS_NETPLAY_MAX_SLOTS; k++)
            if (g_game_spec.netplay_seat_label(genesis_netplay_session_config()->game, g_np.slot_port[k],
                                               lab, sizeof lab) == 1)   /* 2 = label, no role */
                ok |= 1u << k;
        rnet_session_set_claimable_mask(g_np.session, ok);
        fprintf(stderr, "genesis_netplay: join in progress: claimable seats %x (seats with a role)\n",
                (unsigned)ok);
    }
    /* Drop policy: only the rollback host supports it (a delay-sync session
     * would stall on the vanished seat), and a 2-seat room keeps the old rule
     * -- one guest leaving IS the match ending, nothing to hand to a CPU
     * that the host would not rather play alone. */
    if (g_np.rollback && g_np.slot_count > 2) {
        unsigned ms = env_u("GENESIS_NET_DROP_MS", 2000);
        g_np.min_humans = env_u("GENESIS_NET_MIN_HUMANS", 1);
        if (ms) {
            rnet_session_set_seat_loss_policy(g_np.session, ms);
            g_np.drop_policy = 1;
            fprintf(stderr, "genesis_netplay: drop-in policy on: a guest seat lost for %u ms (or "
                            "BYE) becomes CPU; the match ends only if the host leaves, this peer "
                            "is dropped, or fewer than %u humans remain\n", ms, g_np.min_humans);
        }
    }
    arm_test_leave();
    /* The waiting room's agents now belong to the session: no new ones
     * beside the live match (cleared again on shutdown / soft return). */
    if (transport == GENESIS_NET_TRANSPORT_ICE || transport == GENESIS_NET_TRANSPORT_ICE_HUB) {
        rnet_lobby_set_match_running(1);
        /* The launch is consumed: the room view is "seated" again, which is
         * what lets a join-in-progress host keep answering agents during the
         * match (the launcher clears it too; a headless room never did). */
        rnet_lobby_clear_launch_pending();
    }

    {
        char ports[3 * GENESIS_NETPLAY_MAX_SLOTS + 1];
        size_t pn = 0;
        ports[0] = 0;
        for (int i = 0; i < g_np.slot_count && i < GENESIS_NETPLAY_MAX_SLOTS; i++)
            pn += (size_t)snprintf(ports + pn, sizeof ports - pn, "%s%u", i ? "," : "",
                                   (unsigned)g_np.slot_port[i]);
        fprintf(stderr, "genesis_netplay: started transport=%s slot=%d slots=%d input=%d "
                        "session=%u delay=%u prediction=%d mode=%s occupied=%x ports=%s "
                        "bind=%s peer=%s force_turn=%d%s\n",
                genesis_netplay_transport_name(), g_np.local_slot, g_np.slot_count,
                g_np.input_player, (unsigned)rcfg.session_id, (unsigned)rcfg.input_delay,
                g_np.input_prediction, g_np.rollback ? "rollback" : "delay",
                (unsigned)g_np.occupied_mask, ports, cfg->bind_hostport, cfg->peer_hostport,
                cfg->force_turn ? 1 : 0, g_np.spectator ? " SPECTATOR" : "");
    }
    return 0;
}

void genesis_netplay_print_summary(void)
{
    if (!g_np.active) return;
    if (g_np.rollback) genesis_netplay_rb_print_summary();
    {
        rnet_u32 catchups = 0, rows = 0, chunks = 0;
        rnet_session_observer_counters(g_np.session, &catchups, &rows, &chunks);
        fprintf(stderr, "genesis_netplay: summary frames=%u sim=%u transport=%s "
                        "spectator_catchups=%u rows_served=%u chunks_served=%u%s%s\n",
                (unsigned)g_np.frames_finished, (unsigned)genesis_netplay_sim_tick(),
                genesis_netplay_transport_name(), (unsigned)catchups, (unsigned)rows,
                (unsigned)chunks, s_spectator_refusal[0] ? " refused=" : "",
                s_spectator_refusal[0] ? s_spectator_refusal : "");
    }
    {
        /* The input delay this peer ended on and the game-path RTT to every
         * other seat (the host's controller fits D to the worst of these). */
        char per[512];
        size_t pn = 0;
        rnet_u32 wr = 0, wj = 0, wn = 0;
        per[0] = 0;
        for (int i = 0; i < g_np.slot_count && i < GENESIS_NETPLAY_MAX_SLOTS; i++) {
            rnet_u32 r = 0, j = 0, k = 0;
            if (!rnet_session_link_rtt_slot(g_np.session, i, &r, &j, &k)) continue;
            pn += (size_t)snprintf(per + pn, sizeof per - pn, " s%d=%u/%u/n%u", i, (unsigned)r,
                                   (unsigned)j, (unsigned)k);
            if (pn >= sizeof per) break;
        }
        (void)rnet_session_link_rtt(g_np.session, &wr, &wj, &wn);
#if defined(RNET_ENABLE_ICE)
        if (g_np.transport == GENESIS_NET_TRANSPORT_ICE) log_agent_path("(plain, 2 seats)", g_np.local_slot, -1);
#endif
        RNetSessionStats sst;
        rnet_session_get_stats(g_np.session, &sst);
        fprintf(stderr, "genesis_netplay: link D=%u worst_rtt=%u jitter=%u ice_recv_drops=%u "
                        "per_seat(rtt/jitter/samples):%s\n",
                (unsigned)rnet_session_committed_delay(g_np.session), (unsigned)wr, (unsigned)wj,
                (unsigned)sst.ice_recv_drops, per[0] ? per : " none");
    }
}

void genesis_netplay_shutdown(void)
{
    if (g_np.rollback) {
        genesis_netplay_rb_shutdown();
        genesis_netplay_rb_bind(NULL);
    }
    if (g_np.session) {
        (void)rnet_session_send_bye(g_np.session);
        rnet_session_destroy(g_np.session);
        rnet_lobby_set_match_running(0);
    }
    {
        uint8_t keep[2] = { g_np.pad_type[0], g_np.pad_type[1] };
        memset(&g_np, 0, sizeof(g_np));
        g_np.pad_type[0] = keep[0]; g_np.pad_type[1] = keep[1];
    }
}

/* ---- per tick ------------------------------------------------------------------- */

int genesis_netplay_needs_local_sample(void)
{
    uint32_t tick;
    if (!genesis_netplay_active() || g_np.spectator) return 0;
    if (g_np.rollback) return 1;   /* re-read on every admit attempt */
    if (!rnet_session_is_running(g_np.session)) return 1;
    tick = rnet_session_sim_tick(g_np.session);
    return !g_np.latched_for_tick || g_np.latched_sim_tick != tick;
}

void genesis_netplay_stage_local(uint16_t buttons)
{
    uint32_t tick;
    if (!genesis_netplay_active()) return;
    buttons &= GENESIS_NETPLAY_PAD_MASK;
    { static uint16_t last; if (getenv("GENESIS_NET_DEBUG_STAGE") && buttons != last) {
        fprintf(stderr, "[stage] sim=%u buttons=%03x\n", (unsigned)genesis_netplay_sim_tick(), buttons); last = buttons; } }
    if (g_np.rollback) {
        g_np.staged_buttons = buttons;
        g_np.staged_valid = 1;
        return;
    }
    tick = rnet_session_sim_tick(g_np.session);
    if (g_np.latched_for_tick && g_np.latched_sim_tick == tick) return;
    g_np.staged_buttons = buttons;
    g_np.staged_valid = 1;
    g_np.latched_for_tick = 1;
    g_np.latched_sim_tick = tick;
}

/* The initial SRAM barrier: the host sends, every guest applies; nobody
 * admits tick 0 before it completes, so the boot digest covers the host's
 * save on every peer. */
static int sram_barrier(void)
{
    rnet_u8 op = 0, slot = 0;
    const void *data = NULL;
    size_t size = 0;
    if (g_np.sram_sync_done) return 1;
    if (!g_sram || !g_sram_size) { g_np.sram_sync_done = 1; return 1; }
    if (!rnet_session_is_running(g_np.session)) return 0;
    if (g_np.local_slot == 0 && !g_np.spectator && !g_np.sram_sync_sent) {
        if (rnet_session_state_begin(g_np.session, RNET_STATE_OP_SRAM, 0, g_sram, g_sram_size) != 0) {
            fprintf(stderr, "genesis_netplay: SRAM sync could not start — refusing the match\n");
            g_return_to_lobby = 1;
            return 0;
        }
        g_np.sram_sync_sent = 1;
        fprintf(stderr, "genesis_netplay: syncing host SRAM (%u bytes)\n", (unsigned)g_sram_size);
    }
    if (rnet_session_state_take_ready(g_np.session, &op, &slot, &data, &size)) {
        if (op == RNET_STATE_OP_SRAM && g_np.local_slot != 0 && data && size) {
            size_t n = size < g_sram_size ? size : g_sram_size;
            memcpy(g_sram, data, n);
            if (n < g_sram_size) memset(g_sram + n, 0, g_sram_size - n);
            fprintf(stderr, "genesis_netplay: applied host SRAM (%zu bytes)\n", size);
        }
        rnet_session_state_finish(g_np.session, 0);
        if (op == RNET_STATE_OP_SRAM) {
            g_np.sram_sync_done = 1;
            if (g_np.local_slot == 0) fprintf(stderr, "genesis_netplay: host SRAM delivered\n");
        }
    }
    return g_np.sram_sync_done;
}

/* ---- join in progress ------------------------------------------------------------ */

#define LATE_CATCH_FAR 90u    /* behind live by more than this: the no-render loop */
#define LATE_CATCH_NEAR 30u   /* ... and back to presenting below this */
#define LATE_CLAIM_LAG 48u    /* within this of live (sustained): ask for a seat */
#define LATE_CLAIM_HOLD_MS 300u
/* The no-render catch-up shows nothing (genesis_netplay_catchup_progress is
 * all there is), so it is bounded: no live position from the host at all, or a
 * replay that does not get near live, ends the join with a message. A
 * 20-minute match replays in about a minute (~0.8 ms a tick). */
#define LATE_NOLIVE_MS 30000u
#define LATE_CATCH_MAX_MS 300000u

static uint32_t np_now_ms(void)
{
#if defined(_WIN32)
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}

int genesis_netplay_catching_up(void)
{
    return genesis_netplay_active() && g_np.spectator && g_np.late_state == 1 && g_np.late_catching;
}
int genesis_netplay_late_state(void) { return genesis_netplay_active() ? g_np.late_state : 0; }

int genesis_netplay_catchup_progress(void)
{
    uint32_t live, sim;
    if (!genesis_netplay_active() || !g_np.spectator) return 0;
    live = rnet_session_observer_live_tick(g_np.session);
    sim = genesis_netplay_sim_tick();
    return live ? (int)((uint64_t)(sim > live ? live : sim) * 100u / live) : 0;
}

/* The seat was granted: this peer is a seat now. The session already speaks as
 * the seat (rnet_session promoted itself on the grant); the facade, the
 * rollback driver and the validation knobs follow. */
static void late_promote(int slot, uint32_t wire)
{
    /* The grant names a seat of THIS match's table: anything else is a broken
     * peer (the session promoted itself on it already, so there is no going
     * back to watching -- leave). */
    if (slot <= 0 || slot >= g_np.slot_count || slot >= GENESIS_NETPLAY_MAX_SLOTS) {
        g_np.late_state = 4;
        snprintf(s_claim_msg, sizeof s_claim_msg, "The host granted an invalid seat (%d).", slot);
        snprintf(s_drop_msg, sizeof s_drop_msg, "%s", s_claim_msg);
        fprintf(stderr, "genesis_netplay: late join: %s -- leaving the match\n", s_claim_msg);
        g_return_to_lobby = 1;
        return;
    }
    g_np.local_slot = slot;
    g_np.spectator = 0;
    g_np.cfg.spectator = 0;
    g_np.staged_valid = 0;
    g_np.late_state = 3;
    g_np.late_catching = 0;
    if (g_np.rollback && !genesis_netplay_rb_promote()) {
        /* The session is a seat now but the driver is still an observer: it
         * would never send this seat's input and the room would play a seat
         * nobody reads (or stall on it). Do not announce a join that did not
         * happen. */
        g_np.late_state = 4;
        snprintf(s_claim_msg, sizeof s_claim_msg, "Joining the match failed (the rollback driver "
                 "could not take the seat).");
        snprintf(s_drop_msg, sizeof s_drop_msg, "%s", s_claim_msg);
        fprintf(stderr, "genesis_netplay: late join: %s -- leaving the match\n", s_claim_msg);
        g_return_to_lobby = 1;
        return;
    }
    fprintf(stderr, "genesis_netplay: JOINED the match as seat %d (logical player %d), present from "
                    "wire %u; the player takes over at the next whistle\n", slot,
            slot >= 0 && slot < GENESIS_NETPLAY_MAX_SLOTS ? g_np.slot_port[slot] : -1, (unsigned)wire);
    arm_test_pad();
    arm_test_leave();
}

static void late_pump(void)
{
    if (!g_np.late_join || !g_np.session) return;
    if (g_np.spectator && g_np.late_state >= 1 && g_np.late_state <= 2) {
        uint32_t live = rnet_session_observer_live_tick(g_np.session);
        uint32_t sim = genesis_netplay_sim_tick();
        uint32_t lag = live > sim ? live - sim : 0u;
        uint32_t now = np_now_ms();
        if (g_np.late_state == 1) {
            if (!g_np.late_begin_ms) g_np.late_begin_ms = now ? now : 1u;
            if ((live == 0 && now - g_np.late_begin_ms > LATE_NOLIVE_MS) ||
                (g_np.late_catching && now - g_np.late_begin_ms > LATE_CATCH_MAX_MS)) {
                g_np.late_state = 4;
                g_np.late_catching = 0;
                snprintf(s_drop_msg, sizeof s_drop_msg, live == 0
                         ? "Could not join: the host sent no match data in %u s."
                         : "Could not join: catching up with the match took longer than %u s.",
                         (unsigned)((live == 0 ? LATE_NOLIVE_MS : LATE_CATCH_MAX_MS) / 1000u));
                fprintf(stderr, "genesis_netplay: late join abandoned: %s (replayed %u of %u ticks)\n",
                        s_drop_msg, (unsigned)sim, (unsigned)live);
                g_return_to_lobby = 1;
                return;
            }
            if (live == 0) { g_np.late_catching = 1; }
            else if (lag > LATE_CATCH_FAR) g_np.late_catching = 1;
            else if (lag <= LATE_CATCH_NEAR) g_np.late_catching = 0;
            if (now - g_np.late_log_ms >= 2000u) {
                g_np.late_log_ms = now;
                fprintf(stderr, "genesis_netplay: late join: replayed %u of %u ticks (lag %u)%s\n",
                        (unsigned)sim, (unsigned)live, (unsigned)lag,
                        g_np.late_catching ? " [catching up, no render]" : "");
            }
            if (live != 0 && lag <= LATE_CLAIM_LAG) {
                if (!g_np.late_caught_ms) g_np.late_caught_ms = now ? now : 1u;
                if (now - g_np.late_caught_ms >= LATE_CLAIM_HOLD_MS &&
                    rnet_session_request_seat(g_np.session, g_np.late_want)) {
                    g_np.late_state = 2;
                    fprintf(stderr, "genesis_netplay: late join: caught up at tick %u (live %u) -- "
                                    "asking for a seat (%s)\n", (unsigned)sim, (unsigned)live,
                            g_np.late_want >= 0 ? "the one named" : "any free one");
                }
            } else {
                g_np.late_caught_ms = 0;
            }
        }
        if (g_np.late_state == 2) {
            int slot = -1;
            uint32_t wire = 0;
            int st = rnet_session_claim_status(g_np.session, &slot, &wire);
            if (st == 2) late_promote(slot, wire);
            else if (st == 3) {
                g_np.late_state = 4;
                snprintf(s_claim_msg, sizeof s_claim_msg, "The match has no free seat for you right now "
                         "(reason %d). You can keep watching.", slot);
                fprintf(stderr, "genesis_netplay: late join: seat claim refused (reason %d)\n", slot);
            }
        }
    }
#if defined(RNET_ENABLE_ICE)
    /* Host: spectators that connected after launch get a hub index. */
    if (g_np.local_slot == 0 && !g_np.spectator && g_np.transport == GENESIS_NET_TRANSPORT_ICE_HUB) {
        static uint32_t next_ms;
        uint32_t now = np_now_ms();
        static uint32_t log_ms;
        if (now - log_ms >= 5000u) {
            RNetHostIceStatus hs;
            log_ms = now;
            if (rnet_lobby_host_ice_status(&hs) && hs.peer_count > 0)
                fprintf(stderr, "genesis_netplay: late agents: %d waiting spectator(s), %d connected\n",
                        hs.peer_count, hs.completed);
        }
        if (now - next_ms >= 100u) {
            RNetLobbyIceSeat late[4];
            int n;
            next_ms = now;
            n = rnet_lobby_ice_take_late(late, 4);
            for (int i = 0; i < n; i++) {
                if (rnet_session_hub_add_agent(g_np.session, late[i].wire_slot, late[i].agent) == 0) {
                    g_np.late_added_agents++;
                    fprintf(stderr, "genesis_netplay: late spectator \"%s\" joined the hub at wire slot %d\n",
                            late[i].player_id, late[i].wire_slot);
                } else {
                    fprintf(stderr, "genesis_netplay: late spectator \"%s\": hub refused its agent\n",
                            late[i].player_id);
                    rnet_host_ice_destroy_agent(late[i].agent);
                }
            }
        }
    }
#endif
}

int genesis_netplay_poll_admit(void)
{
    uint32_t tick;
    if (!genesis_netplay_active()) return 1;
    if (g_np.transport == GENESIS_NET_TRANSPORT_ICE || rnet_lobby_connected())
        rnet_lobby_pump();
    drain_lobby_signals();
    rnet_session_pump(g_np.session);
    late_pump();
    if (spectator_refused()) {
        g_return_to_lobby = 1;
        return 0;
    }
    if (!sram_barrier()) return 0;
    if (g_np.rollback) {
        /* Idempotent between an admission and its finish_frame: the host
         * loop polls again around present to overlap wire work, and the
         * driver must see exactly one poll per admitted tick. */
        if (g_np.needs_advance) return 1;
        int admitted = genesis_netplay_rb_poll_admit();
        if (genesis_netplay_rb_refusal()) g_return_to_lobby = 1;
        if (admitted) g_np.needs_advance = 1;
        return admitted;
    }
    if (!rnet_session_is_running(g_np.session)) return 0;
    if (g_np.needs_advance) return 1;
    tick = rnet_session_sim_tick(g_np.session);
    if (!rnet_session_try_admit(g_np.session, tick)) return 0;
    g_np.needs_advance = 1;
    return 1;
}

void genesis_netplay_wait_recv(int timeout_ms)
{
    if (!genesis_netplay_active()) return;
    (void)rnet_session_wait_recv(g_np.session, timeout_ms);
}

void genesis_netplay_finish_frame(void)
{
    if (!genesis_netplay_active()) return;
    if (g_np.test_leave_armed && genesis_netplay_sim_tick() >= g_np.test_leave_tick) {
        g_np.test_leave_armed = 0;
        if (g_np.test_leave_crash) {
            fprintf(stderr, "genesis_netplay: TEST LEAVE -- crashing now (no BYE)\n");
            fflush(stderr);
#if defined(_WIN32)
            ExitProcess(137);
#else
            raise(SIGKILL);
#endif
        }
        fprintf(stderr, "genesis_netplay: TEST LEAVE -- leaving the match\n");
        g_return_to_lobby = 1;
    }
    if (g_np.rollback) {
        if (!g_np.needs_advance) return;
        genesis_netplay_rb_finish_frame();
        g_np.needs_advance = 0;
        g_np.frames_finished++;
        return;
    }
    g_np.frames_finished++;
    if (!g_np.needs_advance) return;
    rnet_session_advance(g_np.session);
    g_np.needs_advance = 0;
    g_np.latched_for_tick = 0;
}

int genesis_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash)
{
    return genesis_netplay_active() && !g_np.rollback &&
           rnet_session_input_desync(g_np.session, tick, local_hash, remote_hash);
}

int genesis_netplay_peer_disconnected(uint32_t timeout_ms)
{
    if (!genesis_netplay_active()) return 0;
    return rnet_session_peer_disconnected(g_np.session, timeout_ms ? timeout_ms : 1500);
}

int genesis_netplay_drop_policy_active(void) { return genesis_netplay_active() && g_np.drop_policy; }

int genesis_netplay_humans_present(void)
{
    int n = 0;
    uint32_t absent;
    if (!genesis_netplay_active()) return 0;
    absent = rnet_session_absent_mask(g_np.session);
    for (int i = 0; i < g_np.slot_count && i < GENESIS_NETPLAY_MAX_SLOTS; i++) {
        if (g_np.occupied_mask && !(g_np.occupied_mask & (1u << i))) continue;
        if (!(absent & (1u << i))) n++;
    }
    return n;
}

int genesis_netplay_match_should_end(const char **why)
{
    if (why) *why = NULL;
    if (!genesis_netplay_active()) return 0;
    if (!g_np.drop_policy)
        return genesis_netplay_peer_disconnected(3000);
    /* A peer that sat frozen for seconds (window drag, SIGSTOP) wakes up with
     * stale silence timers and a queue holding the host's OCC_EVENT: read the
     * wire first, or it would blame "the host left" for what is its own drop. */
    rnet_session_pump(g_np.session);
    /* We were dropped: the others already play our seat as CPU, and we would
     * keep simulating a seat nobody reads. Leave, and say why. */
    if (rnet_session_dropped_self(g_np.session)) {
        snprintf(s_drop_msg, sizeof s_drop_msg, "You were removed from the match: your connection "
                 "to the host timed out. Your player is now played by the CPU.");
        if (why) *why = s_drop_msg;
        return 1;
    }
    if (g_np.local_slot != 0) {
        /* The host carries the match: it leaving (BYE, or silence) ends it. */
        if (rnet_session_peer_gone(g_np.session, 0) ||
            rnet_session_peer_slot_disconnected(g_np.session, 0, 3000)) {
            snprintf(s_drop_msg, sizeof s_drop_msg, "The host left the match.");
            if (why) *why = s_drop_msg;
            return 1;
        }
    } else {
        /* The host: every human still seated silent at once is OUR link dying
         * (the pump drops a seat only while others are heard), the legacy
         * "peer gone" end. */
        int present = 0, silent = 0;
        uint32_t absent = rnet_session_absent_mask(g_np.session);
        /* The host's own link evidence for the session's silent-seat rule: the
         * lobby websocket is pumped every frame (poll_admit), so "connected"
         * is fresh. A lone remote human whose process crashed is then dropped
         * to the CPU like any other seat, and the match goes on (below) instead
         * of ending "Lost the connection to every player". No lobby (LAN) or a
         * dead websocket = no evidence = the old rule. */
        const int link_ok = rnet_lobby_connected();
        rnet_session_set_link_ok(g_np.session, link_ok);
        /* peer_gone on the host under the policy: the occupancy table is full
         * and a departed seat could not be recorded (recomp-net): the legacy
         * end of the match. */
        if (rnet_session_peer_disconnected(g_np.session, 0)) {
            snprintf(s_drop_msg, sizeof s_drop_msg, "A player left and the match cannot record it.");
            if (why) *why = s_drop_msg;
            return 1;
        }
        for (int i = 1; i < g_np.slot_count && i < GENESIS_NETPLAY_MAX_SLOTS; i++) {
            if ((g_np.occupied_mask && !(g_np.occupied_mask & (1u << i))) || (absent & (1u << i)))
                continue;
            present++;
            if (rnet_session_peer_slot_disconnected(g_np.session, i, 3000) &&
                !rnet_session_peer_gone(g_np.session, i)) silent++;
        }
        if (present && silent == present && !link_ok) {
            snprintf(s_drop_msg, sizeof s_drop_msg, "Lost the connection to every player.");
            if (why) *why = s_drop_msg;
            return 1;
        }
    }
    if ((unsigned)genesis_netplay_humans_present() < g_np.min_humans) {
        snprintf(s_drop_msg, sizeof s_drop_msg, "Not enough human players left in the match.");
        if (why) *why = s_drop_msg;
        return 1;
    }
    return 0;
}

/* ---- config seal + identity -------------------------------------------------------- */

static char s_config_image[1024];
static GenesisSessionConfig s_local_cfg, s_session_cfg;
static int s_session_cfg_adopted;
static char s_engine_knobs[256];
void genesis_netplay_set_engine_knobs(const char *line)
{
    snprintf(s_engine_knobs, sizeof s_engine_knobs, "%s", line ? line : "");
}

void genesis_netplay_set_local_session_config(const GenesisSessionConfig *c)
{
    if (c) s_local_cfg = *c;
    if (!s_session_cfg_adopted) s_session_cfg = s_local_cfg;
}

void genesis_netplay_adopt_session_config(const GenesisSessionConfig *host)
{
    s_session_cfg_adopted = host != NULL;
    s_session_cfg = host ? *host : s_local_cfg;
    s_session_cfg.game[sizeof s_session_cfg.game - 1] = 0;
    s_session_cfg.video[sizeof s_session_cfg.video - 1] = 0;
}

const GenesisSessionConfig *genesis_netplay_session_config(void) { return &s_session_cfg; }
const GenesisSessionConfig *genesis_netplay_local_session_config(void) { return &s_local_cfg; }

/* The seat plan the simulation routes pads by: which session slot plays which
 * logical player (slot_port), which seats are human (occupied_mask) and which
 * the CPU's at launch (open_mask). genesis_netplay_sim_input uses all three on
 * EVERY tick, so a joiner replaying from tick 0 needs the values the host
 * launched with -- not what its own late launch derived from the room as it is
 * now (a guest left, a seat moved). Folded into the sealed image, so a peer
 * whose plan differs fails the content identity and refuses the match instead
 * of replaying a different one (recomp-net judges the host's IDENT). */
static char s_seat_plan[160];
void genesis_netplay_set_seat_plan(const GenesisNetplayConfig *cfg)
{
    int n;
    s_seat_plan[0] = 0;
    if (!cfg || !cfg->enabled) return;
    n = snprintf(s_seat_plan, sizeof s_seat_plan, "slots=%d ports=",
                 cfg->slot_count > 0 ? cfg->slot_count : 2);
    for (int i = 0; i < cfg->slot_count && i < GENESIS_NETPLAY_MAX_SLOTS && n > 0 &&
                    (size_t)n < sizeof s_seat_plan; i++)
        n += snprintf(s_seat_plan + n, sizeof s_seat_plan - (size_t)n, "%s%u", i ? "," : "",
                      (unsigned)(cfg->slot_port_valid || cfg->slot_port[i] ? cfg->slot_port[i]
                                                                          : (uint8_t)i));
    if (n > 0 && (size_t)n < sizeof s_seat_plan)
        snprintf(s_seat_plan + n, sizeof s_seat_plan - (size_t)n, " occ=%x open=%x",
                 (unsigned)cfg->occupied_mask, (unsigned)cfg->open_mask);
}

void genesis_netplay_config_seal(void)
{
    const GenesisSessionConfig *c = &s_session_cfg;
    g_np.pad_type[0] = c->pad_type[0] ? 1 : 0;
    g_np.pad_type[1] = c->pad_type[1] ? 1 : 0;
    snprintf(s_config_image, sizeof s_config_image,
             "genesis-config/3\npad=%u,%u\nws=%u cells=%u\nvideo=%s\ngame=%s\nknobs=%s\nseats=%s\n",
             (unsigned)g_np.pad_type[0], (unsigned)g_np.pad_type[1],
             (unsigned)(c->ws_on ? 1 : 0), (unsigned)(c->ws_on ? c->ws_cells : 0),
             c->video[0] ? c->video : "off", c->game, s_engine_knobs, s_seat_plan);
    genesis_netplay_rb_set_config_image(s_config_image);
    fprintf(stderr, "genesis_netplay: session config (%s):\n%s",
            s_session_cfg_adopted ? "adopted from the host" : "local", s_config_image);
}

const char *genesis_netplay_config_image(void) { return s_config_image; }

void genesis_netplay_set_identity(uint32_t build_fp, const uint8_t rom_sha256[32])
{
    /* content = FNV-1a over the ROM digest and the sealed config image: two
     * peers with the same ROM but a different session configuration are
     * running different games and must be refused (NETPLAY.md section 4). */
    uint32_t h = 2166136261u;
    for (int i = 0; rom_sha256 && i < 32; i++) { h ^= rom_sha256[i]; h *= 16777619u; }
    for (const char *p = s_config_image; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    genesis_netplay_rb_set_identity(build_fp, h ? h : 1u);
}
