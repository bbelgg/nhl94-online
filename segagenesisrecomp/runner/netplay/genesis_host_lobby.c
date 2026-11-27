/*
 * genesis_host_lobby.c -- Genesis adapter over recomp-ui's recomp_netplay_host
 * (see genesis_host_lobby.h). Shape copied from snesrecomp's
 * runner/src/netplay/snes_host_lobby.c.
 */
#include "genesis_host_lobby.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_netplay_host.h"
#include "recomp_net/lobby_client.h"
#include "recomp_net/host_relay.h"
#include "recomp_net/host_ice.h"
#include "genesis_netplay_identity.h"
#include "genesis_netplay_rb.h"
#include "../game_spec.h"

#if defined(_WIN32)
#include <windows.h>
static void hl_sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void hl_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif
#include "retcomm_rbengine/mono_ms.h"

/* ---- the session config in match caps -------------------------------------
 * The host publishes its GenesisSessionConfig as four engine keys; every peer
 * parses them into caps->ext and adopts them for the launch. 128 bytes of ext
 * (was 64): the struct is 108 (the game line is 96 bytes: per-seat builds). */
_Static_assert(sizeof(GenesisSessionConfig) + 1 <= RNET_LOBBY_CAPS_EXT_BYTES,
               "GenesisSessionConfig no longer fits RNetLobbyMatchCaps.ext");
typedef struct GenesisCapsExt {
    uint8_t valid;
    GenesisSessionConfig cfg;
} GenesisCapsExt;

static GenesisSessionConfig s_host_cfg;
static int s_host_cfg_valid;
static char s_host_cfg_text[512];
static void (*s_capture_local_config)(const RecompLauncherCSettings *, GenesisSessionConfig *);

static int caps_write(const RNetLobbyMatchCaps *caps, char *out, size_t cap, void *ctx)
{
    const GenesisCapsExt *e = (const GenesisCapsExt *)caps->ext.bytes;
    char esc[2 * sizeof e->cfg.game + 8];
    (void)ctx;
    if (!e->valid) return 0;
    rnet_lobby_json_escape(e->cfg.game, esc, sizeof esc);
    char vesc[2 * sizeof e->cfg.video + 8];
    rnet_lobby_json_escape(e->cfg.video, vesc, sizeof vesc);
    int n = snprintf(out, cap, ",\"gen_pads\":\"%u%u\",\"gen_ws\":%u,\"gen_cells\":%u,\"gen_game\":\"%s\",\"gen_video\":\"%s\"",
                     (unsigned)e->cfg.pad_type[0], (unsigned)e->cfg.pad_type[1],
                     (unsigned)e->cfg.ws_on, (unsigned)e->cfg.ws_cells, esc, vesc);
    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}

static void caps_parse(const char *json, RNetLobbyMatchCaps *caps, void *ctx)
{
    GenesisCapsExt *e = (GenesisCapsExt *)caps->ext.bytes;
    char pads[8] = "";
    (void)ctx;
    memset(e, 0, sizeof *e);
    if (!rnet_lobby_json_get_str(json, "gen_pads", pads, sizeof pads) || strlen(pads) != 2)
        return;
    e->cfg.pad_type[0] = pads[0] == '1';
    e->cfg.pad_type[1] = pads[1] == '1';
    e->cfg.ws_on = (uint8_t)(rnet_lobby_json_get_int(json, "gen_ws", 0) != 0);
    e->cfg.ws_cells = (uint8_t)rnet_lobby_json_get_int(json, "gen_cells", 0);
    /* A host that sent a longer game line than this build carries (an older
     * build with a smaller field, or a bug): do not adopt a cut copy. The
     * config stays "not valid", so the identity check refuses the match. */
    {
        char full[512] = "";
        rnet_lobby_json_get_str(json, "gen_game", full, sizeof full);
        if (strlen(full) >= sizeof e->cfg.game) {
            fprintf(stderr, "genesis_netplay: the host's game config line is %zu bytes (this build "
                            "carries %zu): not adopting a truncated copy\n", strlen(full),
                    sizeof e->cfg.game - 1);
            return;
        }
        memcpy(e->cfg.game, full, strlen(full) + 1);
    }
    rnet_lobby_json_get_str(json, "gen_video", e->cfg.video, sizeof e->cfg.video);
    e->valid = 1;
}

static const RNetLobbyCapsCodec k_codec = { caps_write, caps_parse, NULL };

static void fill_caps(void *ctx, const RecompLauncherCSettings *settings, RNetLobbyMatchCaps *caps)
{
    GenesisCapsExt *e = (GenesisCapsExt *)caps->ext.bytes;
    (void)ctx;
    memset(e, 0, sizeof *e);
    e->valid = 1;
    /* The host publishes its OWN configuration, never one it adopted. */
    if (s_capture_local_config) {
        s_capture_local_config(settings, &e->cfg);
        genesis_netplay_set_local_session_config(&e->cfg);
    } else {
        e->cfg = *genesis_netplay_local_session_config();
    }
}

static void apply_caps(void *ctx, const RNetLobbyMatchCaps *caps, RecompLauncherCNetplayLaunch *launch)
{
    const GenesisCapsExt *e = (const GenesisCapsExt *)caps->ext.bytes;
    (void)ctx; (void)launch;
    s_host_cfg_valid = e->valid;
    if (e->valid) {
        s_host_cfg = e->cfg;
        s_host_cfg.game[sizeof s_host_cfg.game - 1] = 0;
        s_host_cfg.video[sizeof s_host_cfg.video - 1] = 0;
        snprintf(s_host_cfg_text, sizeof s_host_cfg_text, "pad=%u,%u ws=%u cells=%u video=%s game=%s",
                 (unsigned)s_host_cfg.pad_type[0], (unsigned)s_host_cfg.pad_type[1],
                 (unsigned)s_host_cfg.ws_on, (unsigned)s_host_cfg.ws_cells,
                 s_host_cfg.video[0] ? s_host_cfg.video : "off", s_host_cfg.game);
    }
}

/* Waiting room: the game's role for a player seat under the host's published
 * configuration (GameSpec.netplay_seat_label reads the "game" image). */
static int seat_role_label(void *ctx, const RNetLobbyMatchCaps *caps, int slot, char *out, size_t cap)
{
    const GenesisCapsExt *e = (const GenesisCapsExt *)caps->ext.bytes;
    char game[sizeof e->cfg.game + 1];
    (void)ctx;
    if (!g_game_spec.netplay_seat_label || !e->valid) return 0;
    memcpy(game, e->cfg.game, sizeof e->cfg.game);
    game[sizeof e->cfg.game] = 0;
    return g_game_spec.netplay_seat_label(game, slot, out, cap);
}

/* Waiting room: the game's per-seat pick (GameSpec.lobby_pick_*) under the
 * host's published configuration. The pick is the local member's own; the
 * game forwards it to the host (rnet_lobby_set_member_data). */
static int pick_image(const RNetLobbyMatchCaps *caps, char *game, size_t cap)
{
    const GenesisCapsExt *e = (const GenesisCapsExt *)caps->ext.bytes;
    if (!e->valid || cap < sizeof e->cfg.game + 1) return 0;
    memcpy(game, e->cfg.game, sizeof e->cfg.game);
    game[sizeof e->cfg.game] = 0;
    return 1;
}

static int seat_pick_current(void *ctx, const RNetLobbyMatchCaps *caps, int slot, char *out, size_t cap)
{
    char game[sizeof((GenesisCapsExt *)0)->cfg.game + 1];
    (void)ctx;
    if (!g_game_spec.lobby_pick_current || !pick_image(caps, game, sizeof game)) return 0;
    return g_game_spec.lobby_pick_current(game, slot, out, cap);
}

static int seat_pick_query(void *ctx, const RNetLobbyMatchCaps *caps, int slot, const char *filter,
                           RecompPickRow *rows, int max)
{
    char game[sizeof((GenesisCapsExt *)0)->cfg.game + 1];
    (void)ctx;
    if (!g_game_spec.lobby_pick_query || !pick_image(caps, game, sizeof game)) return 0;
    /* RecompPickRow and GamePickRow are the same layout (recomp_launcher.h, game_spec.h). */
    _Static_assert(sizeof(RecompPickRow) == sizeof(GamePickRow), "pick row layouts differ");
    return g_game_spec.lobby_pick_query(game, slot, filter, (GamePickRow *)rows, max);
}

static int seat_pick_set(void *ctx, const RNetLobbyMatchCaps *caps, int slot, int id)
{
    char game[sizeof((GenesisCapsExt *)0)->cfg.game + 1];
    (void)ctx;
    if (!g_game_spec.lobby_pick_set || !pick_image(caps, game, sizeof game)) return -1;
    return g_game_spec.lobby_pick_set(game, slot, id);
}

const char *genesis_host_lobby_host_config(void) { return s_host_cfg_valid ? s_host_cfg_text : NULL; }

/* ---- host environment ----------------------------------------------------- */

static int last_fork(void *ctx, uint32_t *tick, const char **partition, uint32_t *mine, uint32_t *theirs)
{
    (void)ctx;
    return genesis_netplay_rb_last_fork(tick, partition, mine, theirs);
}

static char s_version[64];
static char s_sha[65];

int genesis_host_lobby_init(const GenesisHostLobbyIdentity *id)
{
    RecompNetplayHostHooks h;
    if (!id || !id->game_name || !id->game_name[0]) return -1;
    s_capture_local_config = id->capture_local_config;
    snprintf(s_version, sizeof s_version, "%s", genesis_identity_game_version(id->release_version));
    snprintf(s_sha, sizeof s_sha, "%s", id->rom_sha256_hex ? id->rom_sha256_hex : "");
    memset(&h, 0, sizeof h);
    h.game_name = id->game_name;
    h.game_version = s_version;
    h.content_fingerprint = s_sha[0] ? s_sha : NULL;
    h.lan_registry_path = id->lan_registry_path;
    h.platform = "genesis";
    h.legacy_env_prefix = "GENESIS_NET_";
    h.max_players = id->max_players < 2 ? 2 : id->max_players > GENESIS_SIM_MAX_PLAYERS ? GENESIS_SIM_MAX_PLAYERS : id->max_players;
    {
        /* The room ceiling is the game's (GameSpec.logical_players). A
         * transport test runs more seats than a title declares (the headless
         * room's _PLAYERS, or GENESIS_NET_MAX_PLAYERS by hand): the extra
         * seats' pads still travel and reach genesis_sim_pad; a game that
         * reads only 4 simply ignores them. Capped at the engine's 12. */
        const char *ov = getenv("GENESIS_NET_MAX_PLAYERS");
        int want = ov && ov[0] ? atoi(ov) : 0;
        if (!want && getenv("GENESIS_LOBBY_SELFTEST"))
            want = atoi(getenv("GENESIS_LOBBY_SELFTEST_PLAYERS") ? getenv("GENESIS_LOBBY_SELFTEST_PLAYERS") : "0");
        if (want > h.max_players) {
            h.max_players = want > GENESIS_SIM_MAX_PLAYERS ? GENESIS_SIM_MAX_PLAYERS : want;
            fprintf(stderr, "genesis_netplay: room ceiling raised to %d seats (test override; the "
                            "game declares %d)\n", h.max_players, id->max_players);
        }
    }
#if defined(RECOMP_NETPLAY_HOST_HAS_NETPLAY_MAX_PLAYERS)
    /* The online ceiling, stated: the Host panel's Max Players combo offers
     * 2..this (recomp-ui caps it at RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS, 12),
     * independent of the local controller seats the console profile has. */
    h.netplay_max_players = h.max_players;
#endif
    h.slot_policy = RECOMP_NETPLAY_SLOTS_HOST_FIRST;
    h.input_player = 0;
    h.caps_codec = &k_codec;
    h.fill_match_caps = fill_caps;
    h.apply_match_caps = apply_caps;
    h.seat_role_label = seat_role_label;
    h.seat_pick_current = seat_pick_current;
    h.seat_pick_query = seat_pick_query;
    h.seat_pick_set = seat_pick_set;
    h.last_fork = last_fork;
    h.auto_ready_guests = 1;
    h.rematch_set_ready = 1;
    /* A room of 3+ seats asks every guest to press Ready itself (EASHL-style):
     * the server clears everyone's ready on a join / kick / seat change and
     * that now sticks, the host's Play waits for all of them. A 2-seat room
     * arms itself as before. GENESIS_NET_MANUAL_READY=0 turns it off (read by
     * recomp-ui from legacy_env_prefix (set above) + "MANUAL_READY"). */
    h.manual_ready = 1;
    fprintf(stderr, "genesis_netplay: lobby identity game=\"%s\" version=%s content=%s seats=%d\n",
            id->game_name, s_version, s_sha[0] ? s_sha : "(unknown)", h.max_players);
    if (recomp_netplay_host_init(&h) != 0) return -1;
    /* The lobby server relays nothing (recomp-net-server WS_LOBBY.md "Host
     * relay"), and the deployed one (a39d1d4, ws_lobby.rs sanitize_match_caps)
     * puts EVERY online room on the host relay over ICE: the waiting room
     * connects one ICE agent per guest to the host, and the match runs on
     * them (the ice-hub transport). Ask for it ourselves, so the caps the
     * room sees say what the server will do anyway. */
    rnet_lobby_set_relay_via_ice(1);
    return 0;
}

/* ---- relay policy per room --------------------------------------------------
 * Every online room asks for the host relay over ICE (the server imposes it
 * anyway): the host carries the match on the waiting room's ICE agents --
 * the ice-hub transport for 3+ seats or a gallery, plain two-player ICE for
 * a 2-seat room without spectators (see config_from_launch). LAN / Direct IP
 * rooms are host-carried already. */
static RecompLauncherCNetplayCallbacks s_callbacks;
static int (*s_base_create)(void *, const char *, char *, const char *,
                            const RecompLauncherCSettings *, int, int);

static int create_with_relay_policy(void *ctx, const char *lobby_name, char *host_endpoint,
                                    const char *password, const RecompLauncherCSettings *settings,
                                    int lan_only, int max_slots)
{
    const int host_relay = !lan_only;
    /* The game's session image must fit the room's config line whole: a cut
     * one would be adopted by every peer (consistent, so nothing would notice)
     * and lose the later seats' picks. Refuse to host, loudly. */
    if (s_capture_local_config) {
        GenesisSessionConfig probe;
        memset(&probe, 0, sizeof probe);
        s_capture_local_config(settings, &probe);
        if (probe.game_overflow) {
            fprintf(stderr, "genesis_netplay: REFUSING to host: the session config image does not fit "
                            "the room's %zu-byte game line (see the line above). Fewer seats, or "
                            "shorter builds, then try again.\n", sizeof probe.game - 1);
            return -1;
        }
    }
    rnet_lobby_set_relay_host_pref(host_relay);
    /* Join in progress: ask for a room that can be joined while its match
     * runs (EASHL drop-in). A server that does not know the option ignores it
     * and the room behaves exactly as before. GENESIS_NET_LATE_JOIN=0 opts
     * out. 3+ seat rollback rooms only (the drop policy's shape). */
    {
        const char *lj = getenv("GENESIS_NET_LATE_JOIN");
        rnet_lobby_set_late_join_pref(!lan_only && max_slots >= 3 && !(lj && lj[0] == '0'));
    }
    fprintf(stderr, "genesis_netplay: %s room, %d seats: %s\n", lan_only ? "LAN" : "online",
            max_slots, lan_only ? "host-carried" : "host relay over ICE (waiting-room agents)");
    return s_base_create(ctx, lobby_name, host_endpoint, password, settings, lan_only, max_slots);
}

const RecompLauncherCNetplayCallbacks *genesis_host_lobby_callbacks(void)
{
    if (!s_base_create) {
        s_callbacks = *recomp_netplay_host_callbacks();
        s_base_create = s_callbacks.create;
        s_callbacks.create = create_with_relay_policy;
    }
    return &s_callbacks;
}

/* The room's match caps say the host carries the match over ICE agents. */
static int host_relay_via_ice(void)
{
    const RNetLobbyMatchCaps *caps = rnet_lobby_match_caps();
    return caps && caps->valid && caps->relay_host && caps->relay_via_ice;
}

int genesis_host_lobby_config_from_launch(const RecompLauncherCNetplayLaunch *l, GenesisNetplayConfig *cfg)
{
    if (!l || !cfg || !l->enabled) return -1;
    cfg->enabled = 1;
    cfg->spectator = l->is_spectator ? 1 : 0;
    cfg->spectator_wire_slot = l->spectator_wire_slot;
    cfg->local_slot = l->local_slot;
    cfg->slot_count = l->player_count > 0 ? l->player_count : (l->max_slots > 0 ? l->max_slots : 2);
    cfg->input_player = l->input_player >= 0 ? l->input_player : 0;
    /* GENESIS_NET_DELAY is the operator's override and wins over the room's
     * seed (genesis_netplay_start pins it: no auto delay). */
    {
        const char *ov = getenv("GENESIS_NET_DELAY");
        if (!(ov && ov[0]))
            cfg->input_delay = l->input_delay > 0 ? l->input_delay : 2;
    }
    cfg->input_prediction = l->input_prediction;
    /* A LAN room settles no mode (docs/HOST_NETPLAY.md); rollback stays the
     * Genesis default there, and GENESIS_NET_MODE still overrides. */
    cfg->rollback = recomp_netplay_host_in_lan() ? cfg->rollback : (l->rollback ? 1 : 0);
    cfg->session_id = l->session_id;
    /* The room's Force TURN, or this peer's own knob (GENESIS_NET_FORCE_TURN,
     * applied before the launch). */
    cfg->force_turn = l->force_turn || cfg->force_turn;
    cfg->occupied_mask = l->occupied_mask;
    /* Join in progress: every room seat has a session slot, the open ones are
     * the CPU's until somebody joins (docs/DROPIN.md section C). */
    cfg->late_join = l->late_join ? 1 : 0;
    cfg->late_launch = l->late_launch ? 1 : 0;
    cfg->open_mask = l->open_slot_mask;
    snprintf(cfg->bind_hostport, sizeof cfg->bind_hostport, "%s", l->bind_hostport);
    snprintf(cfg->peer_hostport, sizeof cfg->peer_hostport, "%s", l->peer_hostport);
    /* The transport the LAUNCH stated: the host's own port (host binds and
     * hubs, guests dial it), a server UDP relay (servers that still run one),
     * or two-player ICE. */
    if (recomp_netplay_host_in_lan())
        cfg->transport = GENESIS_NET_TRANSPORT_LAN;
#if defined(RECOMP_LAUNCHER_HAS_HOST_RELAY)
    else if ((l->transport_ice_hub || l->transport_host) && host_relay_via_ice()) {
        /* recomp-net-server a39d1d4 puts every online room on the host relay
         * OVER ICE (ws_lobby.rs sanitize_match_caps): the waiting room
         * connected one agent per guest (and per spectator) to the host and
         * the launch hands them over. 3+ seats or any spectator: the host
         * runs the hub over those agents (ice-hub). Two seats and no gallery:
         * plain two-player ICE, the path a real two-Mac match has run on
         * (fresh agents over the lobby signal, its own TURN fallback and
         * Force TURN); the waiting-room pair is released.
         * GENESIS_NET_ICE_HUB_2P=1 puts a 2-seat room on the hub too. */
        const char *h2 = getenv("GENESIS_NET_ICE_HUB_2P");
        const int hub = cfg->slot_count > 2 || cfg->spectator || rnet_lobby_spectator_count() > 0 ||
                        (h2 && h2[0] == '1');
        cfg->transport = hub ? GENESIS_NET_TRANSPORT_ICE_HUB : GENESIS_NET_TRANSPORT_ICE;
        if (!hub) rnet_lobby_ice_discard();
        fprintf(stderr, "genesis_netplay: host relay over ICE, %d seats%s -> %s\n", cfg->slot_count,
                cfg->spectator ? " (this peer spectates)" : "", hub ? "ice-hub" : "ice (2 seats)");
    }
    else if (l->transport_host)
        cfg->transport = GENESIS_NET_TRANSPORT_HUB;
#endif
    else if (l->force_input_relay)
        cfg->transport = GENESIS_NET_TRANSPORT_RELAY;
    else
        cfg->transport = GENESIS_NET_TRANSPORT_ICE;
    for (int i = 0; i < GENESIS_NETPLAY_MAX_SLOTS; i++) cfg->lobby_seat[i] = -1;
    if (l->slot_port_valid) {
        cfg->slot_port_valid = 1;
        for (int i = 0; i < GENESIS_NETPLAY_MAX_SLOTS && i < RECOMP_LAUNCHER_NETPLAY_MAX_MEMBERS + 1; i++) {
            int p = l->slot_port[i];
            cfg->slot_port[i] = (uint8_t)(p >= 0 && p < GENESIS_NETPLAY_MAX_SLOTS ? p : i);
            cfg->lobby_seat[i] = (int8_t)(p >= 0 && p < 127 ? p : -1);
        }
    } else {
        /* SEAT policy / older launches: session slot == lobby seat. */
        for (int i = 0; i < GENESIS_NETPLAY_MAX_SLOTS; i++) cfg->lobby_seat[i] = (int8_t)i;
    }
    /* The host's start barrier waits for the room's spectators (bounded), so
     * one that launches a moment after the seats still sees tick 0. Only on
     * the host's own port (a hub): the server relay does not forward a
     * spectator's READY. */
    if (!cfg->spectator && cfg->local_slot == 0 &&
        (cfg->transport == GENESIS_NET_TRANSPORT_HUB || cfg->transport == GENESIS_NET_TRANSPORT_ICE_HUB))
        cfg->expected_spectators = rnet_lobby_spectator_count();
    /* Adopt the host's session configuration (ephemeral: never persisted). */
    genesis_netplay_adopt_session_config(s_host_cfg_valid ? &s_host_cfg : NULL);
    return 0;
}

void genesis_host_lobby_begin_soft_return(RecompLauncherCGameInfo *gi)
{
    recomp_netplay_host_begin_soft_return(gi, 1);
}

void genesis_host_lobby_prepare_rematch(void) { recomp_netplay_host_prepare_rematch(); }
void genesis_host_lobby_set_runtime_error(const char *code) { recomp_netplay_host_set_runtime_error(code); }

/* ---- headless room ------------------------------------------------------------ */

int genesis_host_lobby_selftest_role(void)
{
    const char *r = getenv("GENESIS_LOBBY_SELFTEST");
    if (!r) return 0;
    if (!strcmp(r, "host")) return 1;
    if (!strcmp(r, "guest")) return 2;
    /* Join in progress: a guest that joins a room whose match already runs
     * (it waits for the row to say "in progress"), replays the match, claims
     * a free seat. */
    if (!strcmp(r, "latejoin")) return 2;
    return 0;
}

static const char *env_or(const char *k, const char *d) { const char *v = getenv(k); return v && v[0] ? v : d; }

void genesis_host_lobby_selftest_report_room(int round)
{
    const RecompLauncherCNetplayCallbacks *cb = genesis_host_lobby_callbacks();
    int role = genesis_host_lobby_selftest_role();
    if (!role || !cb) return;
    for (int i = 0; i < 20; i++) { cb->pump(NULL); hl_sleep_ms(10); }
    const char *e = cb->last_error ? cb->last_error(NULL) : NULL;
    fprintf(stderr, "[lobby-selftest] %s after match %d back in the room: in_lobby=%d is_host=%d "
                    "members=%d last_error=\"%s\"\n", role == 1 ? "host" : "guest", round - 1,
            cb->in_lobby(NULL), cb->is_host(NULL), cb->member_count(NULL), e ? e : "");
}

/* A host-relay room is startable once the host has advertised its port. The
 * guests' proof is the server's to judge at Start (it does not echo path
 * reports back promptly), so a refused Start is retried, as a host would. */
static int host_relay_ready(void)
{
    const RNetLobbyMatchCaps *caps = rnet_lobby_match_caps();
    RNetHostRelayStatus st;
    if (!caps || !caps->valid || !caps->relay_host) return 1;
    /* NHL94 local patch: recomp-net-server a39d1d4 puts EVERY online room on
     * the host relay over ICE (ws_lobby.rs sanitize_match_caps). That kind
     * advertises no port, so waiting for one never ends; the ICE agents are
     * the server's to judge at Start, like the guests' proof above. */
    if (caps->relay_via_ice) {
        /* Start only once every guest's (and spectator's) waiting-room agent
         * is COMPLETED: a launch with one that is not is refused by the
         * lobby client (ice_not_connected). */
        RNetHostIceStatus hs;
        static uint32_t next_log;
        int want = rnet_lobby_member_count() - 1;
        if (!rnet_lobby_host_ice_status(&hs)) return 0;
        if (rbe_mono_ms() >= next_log) {
            next_log = rbe_mono_ms() + 2000u;
            fprintf(stderr, "[lobby-selftest] host ice agents completed=%d/%d (peers %d)\n",
                    hs.completed, want, hs.peer_count);
        }
        return hs.completed >= want && want > 0;
    }
    return rnet_lobby_host_relay_status(&st) && st.advertised[0];
}

int genesis_host_lobby_selftest_room(int round, RecompLauncherCNetplayLaunch *out)
{
    const RecompLauncherCNetplayCallbacks *cb = genesis_host_lobby_callbacks();
    int role = genesis_host_lobby_selftest_role();
    int is_host = role == 1;
    const char *rname = is_host ? "host" : "guest";
    int players = atoi(env_or("GENESIS_LOBBY_SELFTEST_PLAYERS", "2"));
    int spectators = atoi(env_or("GENESIS_LOBBY_SELFTEST_SPECTATORS", "0"));
    /* Seats the room has (default = players): more seats than players leaves
     * open seats; GENESIS_LOBBY_SELFTEST_SEAT=k makes a guest take seat k
     * (seat_move_self) before readying, e.g. a 2-player 12-seat room with
     * the guest at seat 11 -- humans with CPU seats between them. */
    int room_seats = atoi(env_or("GENESIS_LOBBY_SELFTEST_ROOM_SEATS", "0"));
    int want_seat = atoi(env_or("GENESIS_LOBBY_SELFTEST_SEAT", "-1"));
    int seat_done = 0, labels_logged = 0;
    /* GENESIS_LOBBY_SELFTEST_HOST_SEAT=k (host): move the host to seat k as soon
     * as the room exists, before anybody joins, so it is NOT in lobby seat 0
     * (the first guest then takes seat 0). The session plan keeps the host as
     * slot 0 whatever its seat; this is the drill for everything that must
     * key on the lobby seat instead (builds, labels). */
    int host_seat = atoi(env_or("GENESIS_LOBBY_SELFTEST_HOST_SEAT", "-1"));
    /* Ready / swap drills (the waiting room's explicit Ready, docs/DROPIN.md D):
     *   GENESIS_LOBBY_SELFTEST_READY=1 (default) ready, again after every clear;
     *     0 = this peer never readies (the host's Play must stay blocked, by
     *     name); N>=2 = readies N seconds after it joined (blocked, then goes);
     *   GENESIS_LOBBY_SELFTEST_SWAP=<seat>: this peer asks for a trade with the
     *     player in <seat> once the room is full; every peer answers a request
     *     with GENESIS_LOBBY_SELFTEST_ACCEPT_SWAP (default 1 = accept). */
    const int late_role = getenv("GENESIS_LOBBY_SELFTEST") && !strcmp(getenv("GENESIS_LOBBY_SELFTEST"), "latejoin");
    int ready_mode = late_role ? 0 : atoi(env_or("GENESIS_LOBBY_SELFTEST_READY", "1"));
    int swap_to = atoi(env_or("GENESIS_LOBBY_SELFTEST_SWAP", "-1"));
    int accept_swap = atoi(env_or("GENESIS_LOBBY_SELFTEST_ACCEPT_SWAP", "1"));
    int ready_n = 0, swap_asked = 0, swap_logged = 0;
    uint32_t joined_at = 0, next_ready = 0, next_block_log = 0, gate_at = 0;
    /* GENESIS_LOBBY_SELFTEST_HOLD_MS (host): wait this long after the room is
     * full before pressing Play, so a swap drill has time to finish. */
    uint32_t hold_ms = (uint32_t)atoi(env_or("GENESIS_LOBBY_SELFTEST_HOLD_MS", "0"));
    int lan = atoi(env_or("GENESIS_LOBBY_SELFTEST_LAN", "0"));
    int lan_port = atoi(env_or("GENESIS_LOBBY_SELFTEST_LAN_PORT", "17790"));
    const char *lobby = env_or("GENESIS_LOBBY_SELFTEST_LOBBY", "genesis-selftest");
    const char *name = env_or("GENESIS_LOBBY_SELFTEST_NAME", is_host ? "HostTest" : "GuestTest");
    uint32_t deadline = rbe_mono_ms() + 90000u;
    int joined = 0, ready_sent = 0, started = 0, force_turn_done = 0;
    uint32_t next_list = 0;
    uint32_t next_relay_log = 0, next_start = 0, start_sent_at = 0;

    if (!role || !cb || !out) return -1;
    if (players < 2) players = 2;
    if (players > GENESIS_SIM_MAX_PLAYERS) players = GENESIS_SIM_MAX_PLAYERS;
    if (room_seats < players) room_seats = players;
    if (room_seats > GENESIS_SIM_MAX_PLAYERS) room_seats = GENESIS_SIM_MAX_PLAYERS;
    memset(out, 0, sizeof *out);

    if (round == 1) {
        cb->set_player_name(NULL, name);
        if (!lan) {
            if (cb->connect(NULL) != 0) { fprintf(stderr, "[lobby-selftest] %s connect failed\n", rname); return -2; }
            while (!rnet_lobby_player_id()[0]) {
                cb->pump(NULL);
                if (!cb->connected(NULL) || rbe_mono_ms() > deadline) {
                    fprintf(stderr, "[lobby-selftest] %s no welcome\n", rname);
                    return -3;
                }
                hl_sleep_ms(10);
            }
        }
        if (is_host) {
            char ep[64];
            if (spectators > 0 && cb->allow_spectators_set) cb->allow_spectators_set(NULL, 1);
            snprintf(ep, sizeof ep, lan ? "127.0.0.1:%d" : "0.0.0.0:%d", lan ? lan_port : 7777);
            int rc = cb->create(NULL, lobby, ep, "", NULL, lan ? 1 : 0, lan ? 2 : room_seats);
            fprintf(stderr, "[lobby-selftest] host round=1 %s create rc=%d seats=%d\n",
                    lan ? "lan" : "online", rc, lan ? 2 : room_seats);
            if (rc != 0) return -5;
            joined = 1;
        } else if (lan) {
            char gb[64], id[80];
            snprintf(gb, sizeof gb, "0.0.0.0:%d", lan_port + 1);
            snprintf(id, sizeof id, "lan:127.0.0.1:%d", lan_port);
            int rc = -1;
            while (rbe_mono_ms() < deadline && (rc = cb->join(NULL, id, "", gb)) != 0) hl_sleep_ms(250);
            fprintf(stderr, "[lobby-selftest] guest round=1 lan join rc=%d\n", rc);
            if (rc != 0) return -7;
            joined = 1;
        }
    } else {
        joined = 1;
    }
    while (rbe_mono_ms() < deadline) {
        cb->pump(NULL);
        if (is_host && !lan && !force_turn_done && cb->in_lobby(NULL) && cb->is_host(NULL)) {
            /* GENESIS_NET_FORCE_TURN=1: the room's caps say Force TURN, so
             * every peer's agents go relay-only through the lobby's TURN.
             * Once the room exists (recomp-ui takes it from its host only). */
            const char *ft = getenv("GENESIS_NET_FORCE_TURN");
            force_turn_done = 1;
            if (ft && atoi(ft) && cb->force_turn_set) {
                int frc = cb->force_turn_set(NULL, 1);
                fprintf(stderr, "[lobby-selftest] host: Force TURN on (room caps) rc=%d\n", frc);
            }
        }
        if (!is_host && !joined && rbe_mono_ms() >= next_list) {
            cb->request_list(NULL);
            next_list = rbe_mono_ms() + 500u;
        }
        if (!is_host && !joined) {
            RecompLauncherCNetplayLobby row;
            for (int i = 0; i < cb->list_count(NULL); i++) {
                if (cb->list_get(NULL, i, &row) && !strcmp(row.name, lobby) &&
                    (!late_role || row.in_progress)) {
                    /* Several guests on one machine need distinct game ports. */
                    char gb[64];
                    snprintf(gb, sizeof gb, "%s", env_or("GENESIS_LOBBY_SELFTEST_BIND", ""));
                    if (cb->join(NULL, row.lobby_id, "", gb) == 0) {
                        joined = 1;
                        fprintf(stderr, "[lobby-selftest] guest round=%d join sent\n", round);
                    }
                    break;
                }
            }
        }
        int need = lan ? 2 : players + spectators;
        if (is_host && !lan && host_seat > 0 && joined && cb->in_lobby(NULL) && cb->seat_move_self) {
            int mrc = cb->seat_move_self(NULL, host_seat);
            fprintf(stderr, "[lobby-selftest] host round=%d seat_move_self(%d) rc=%d\n", round, host_seat, mrc);
            host_seat = -1;                       /* once */
        }
        if (joined && cb->in_lobby(NULL) && !is_host && want_seat >= 0 && !seat_done &&
            cb->seat_move_self) {
            int mrc = cb->seat_move_self(NULL, want_seat);
            seat_done = mrc == 0;
            fprintf(stderr, "[lobby-selftest] guest round=%d seat_move_self(%d) rc=%d\n", round, want_seat, mrc);
            if (!seat_done) want_seat = -1;   /* refused: say so once, carry on */
        }
        if (joined && cb->in_lobby(NULL) && !labels_logged && cb->seat_role_label &&
            cb->member_count(NULL) >= need) {
            char lab[32];
            labels_logged = 1;
            for (int s = 0; s < room_seats; s++)
                fprintf(stderr, "[lobby-selftest] %s seat %d label=\"%s\"\n", rname, s,
                        cb->seat_role_label(NULL, s, lab, sizeof lab) ? lab : "");
        }
        if (!lan && rbe_mono_ms() >= next_relay_log) {
            RNetHostRelayStatus st;
            next_relay_log = rbe_mono_ms() + 2000u;
            if (rnet_lobby_host_relay_status(&st))
                fprintf(stderr, "[lobby-selftest] %s relay role=%d endpoint=%s advertised=%s answered=%u "
                                "probed=%s report=%s\n", rname, st.role, st.port.endpoint, st.advertised,
                        (unsigned)st.port.probes_answered, st.probed, st.last_report);
        }
        if (joined && !joined_at) joined_at = rbe_mono_ms();
        /* A seat change answered for / asked by this peer (the room is full). */
        if (joined && cb->in_lobby(NULL) && cb->seat_swap_incoming) {
            char who[64] = {0};
            int from = -1;
            if (cb->seat_swap_incoming(NULL, who, sizeof who, &from)) {
                char mine[32] = "", theirs[32] = "";
                if (cb->seat_role_label) {
                    (void)cb->seat_role_label(NULL, from, theirs, sizeof theirs);
                    for (int k = 0; k < cb->member_count(NULL); k++) {
                        RecompLauncherCNetplayMember mm;
                        if (cb->member_get(NULL, k, &mm) && mm.is_local)
                            (void)cb->seat_role_label(NULL, mm.slot, mine, sizeof mine);
                    }
                }
                fprintf(stderr, "[lobby-selftest] %s round=%d swap request from \"%s\" (%s) for my %s -> %s\n",
                        rname, round, who, theirs, mine, accept_swap ? "accept" : "decline");
                (void)cb->seat_swap_respond(NULL, accept_swap ? 1 : 0);
            }
        }
        if (joined && cb->in_lobby(NULL) && !swap_asked && swap_to >= 0 && cb->seat_swap_request &&
            cb->member_count(NULL) >= need && rbe_mono_ms() - joined_at > 3000u) {
            int src = cb->seat_swap_request(NULL, swap_to);
            swap_asked = 1;
            fprintf(stderr, "[lobby-selftest] %s round=%d seat_swap_request(%d) rc=%d\n", rname, round, swap_to, src);
        }
        if (swap_asked && !swap_logged && cb->seat_swap_outgoing) {
            int st = cb->seat_swap_outgoing(NULL);
            if (st == 2 || st == -1) {
                swap_logged = 1;
                fprintf(stderr, "[lobby-selftest] %s round=%d swap %s\n", rname, round,
                        st == 2 ? "ACCEPTED" : "DECLINED");
                if (cb->seat_swap_clear) cb->seat_swap_clear(NULL);
            }
        }
        /* Ready: the host always; a guest per ready_mode. The server clears
         * everyone's ready on a join, kick or seat change, and an explicit-Ready
         * room (3+ seats) does not re-arm by itself, so say it again whenever
         * it was cleared (paced: the echo takes a round trip). */
        if (joined && cb->in_lobby(NULL) && (!is_host || cb->member_count(NULL) >= need)) {
            int want = is_host || ready_mode == 1 ||
                       (ready_mode >= 2 && rbe_mono_ms() - joined_at > (uint32_t)ready_mode * 1000u);
            if (want && !cb->local_ready(NULL) && rbe_mono_ms() >= next_ready) {
                ready_sent = cb->set_ready(NULL, 1) == 0;
                next_ready = rbe_mono_ms() + 400u;
                if (ready_sent && ++ready_n <= 6)
                    fprintf(stderr, "[lobby-selftest] %s round=%d ready (#%d)\n", rname, round, ready_n);
            }
        }
        if (is_host && !started && cb->member_count(NULL) >= need && !cb->all_ready(NULL) &&
            rbe_mono_ms() >= next_block_log) {
            char who[96] = "";
            next_block_log = rbe_mono_ms() + 3000u;
            if (rnet_lobby_match_blocked_by_unready(who, sizeof who) > 0)
                fprintf(stderr, "[lobby-selftest] host round=%d Play blocked: %s is not ready\n", round, who);
        }
        if (is_host && !gate_at && cb->member_count(NULL) >= need) gate_at = rbe_mono_ms() + hold_ms;
        if (is_host && !started && cb->member_count(NULL) >= need && cb->all_ready(NULL) &&
            rbe_mono_ms() >= gate_at && (lan || host_relay_ready()) && rbe_mono_ms() >= next_start) {
            started = cb->request_start(NULL, NULL) == 0;
            start_sent_at = rbe_mono_ms();
            fprintf(stderr, "[lobby-selftest] host round=%d start=%d members=%d\n", round, started,
                    cb->member_count(NULL));
        }
        if (is_host && started && !cb->launch_pending(NULL) && rbe_mono_ms() - start_sent_at > 3000u) {
            const char *e = cb->last_error ? cb->last_error(NULL) : NULL;
            fprintf(stderr, "[lobby-selftest] host round=%d start refused (%s); retrying\n", round,
                    e && e[0] ? e : "no launch");
            started = 0;
            next_start = rbe_mono_ms() + 2000u;
        }
        if (cb->launch_pending(NULL)) {
            int ok = cb->fill_launch(NULL, out);
            fprintf(stderr, "[lobby-selftest] %s round=%d fill_launch=%d slot=%d players=%d "
                            "session=%u bind=%s peer=%s occupied=%x spectator=%d wire=%d\n", rname, round, ok,
                    out->local_slot, out->player_count, (unsigned)out->session_id,
                    out->bind_hostport, out->peer_hostport, (unsigned)out->occupied_mask,
                    out->is_spectator, out->spectator_wire_slot);
            return ok ? 0 : -11;
        }
        hl_sleep_ms(10);
    }
    {
        const RNetLobbyJoinInfo *ji = rnet_lobby_join_info();
        fprintf(stderr, "[lobby-selftest] %s round=%d TIMED OUT waiting for a launch "
                        "(in_lobby=%d members=%d all_ready=%d session=%u join_error=\"%s\" room_error=\"%s\")\n",
                rname, round, cb->in_lobby(NULL), cb->member_count(NULL), cb->all_ready(NULL),
                ji ? (unsigned)ji->session_id : 0u, ji ? ji->last_error : "",
                cb->last_error && cb->last_error(NULL) ? cb->last_error(NULL) : "");
    }
    return -10;
}
