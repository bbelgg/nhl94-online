/*
 * nhl94_spec.c — GameSpec for NHL Hockey '94 (Electronic Arts, 1993), USA/EU.
 *
 * ROM layout: flat 1 MB at $000000-$0FFFFF, identity-mapped. Battery SRAM is
 * declared in the cartridge header ("RA" $F8 $20, odd bytes $200001-$203FFF),
 * so the runner maps it from the header and sram_start/sram_end stay 0.
 *
 * Boot/dispatch (from the 68K vector table — see game.toml):
 *   Reset SSP    = $00FFFFF6
 *   EntryPoint   = $00000200  (SetupGame)
 *   VInt  (IRQ6) = $000076B2  (VBlankInt: jumps through the pointer at $FFB03A)
 *   HInt  (IRQ4) = $00015E6C  (ExpReturn: bare RTE)
 *
 * Minimal bring-up spec: the three mandatory entry/interrupt dispatchers plus
 * ROM identity. No fill_frame_record and no per-game TCP commands yet — the
 * WRAM layout is only partially named in the disassembly, and inventing
 * addresses would put unverified per-game literals in shared code
 * (PRINCIPLES #21). Dispatchers route through recomp_call_addr(), which uses
 * the generated dispatch table and falls back to the Tier-3 floor.
 */
#include "game_spec.h"
#include "genesis_runtime.h"
#include "nhl94_view.h"
#include "nhl94_seats.h"
#include "nhl94_format.h"
#include "nhl94_mode.h"
#include "nhl94_tags.h"
#include "nhl94_builds.h"
#include "nhl94_ram.h"

#include <stddef.h>
#include <stdint.h>

/* ---- Entry-point / interrupt dispatchers (canonical org-0 addresses) ---- */
static void nhl94_call_entry_point(void) { recomp_call_addr(0x000200u); }
static void nhl94_call_vblank(void)      { recomp_call_addr(0x0076B2u); }
static void nhl94_call_hblank(void)      { recomp_call_addr(0x015E6Cu); }

/* Save states are refused until this game has a resume point. The runner's
 * quickload replaces WRAM and the 68K registers and then restarts the game
 * fiber at resume_main_loop_pc; with none defined it leaves the fiber parked
 * wherever it was, deep inside NHL 94's nested screen subroutines, so the
 * restored guest stack no longer matches the native call chain and the next
 * return dispatches into garbage (owner report 2026-09-24: gamepad shoulder
 * quickload, interior-label misses to $900000-style addresses). Unlike the
 * Sonic titles there is no flat game-mode dispatcher to re-enter; a correct
 * resume needs a trampoline that re-walks the restored guest stack. Until
 * then both quicksave and quickload print this reason and do nothing. */
/* game.toml game_hook sites: the player-lock sites (nhl94_seats.c) act on
 * the simulation; the view sites (nhl94_view.c) only inside speculative view
 * ticks. */
static int nhl94_instruction_hook(uint32_t pc)
{
    int mine = 0;
    /* Per-player builds (nhl94_builds.c; no-ops unless the guest block is latched). The latch
     * runs BEFORE the format hook, which returns first for this pc. */
    if (pc == NHL94_ROM_RESTORETEAMS_R) nhl94_builds_latch();
    int r = nhl94_builds_instruction_hook(pc, &mine);
    if (mine) return r;
    /* 4v4 format sites (nhl94_format.c; no-ops unless guest FMT = 4). */
    r = nhl94_format_instruction_hook(pc, &mine);
    if (mine) return r;
    r = nhl94_seats_instruction_hook(pc, &mine);
    if (mine) return r;
    return nhl94_view_instruction_hook(pc);
}

static int nhl94_handle_arg(const char *key, const char *val)
{
    int r = nhl94_format_handle_arg(key, val);
    if (r) { nhl94_seats_reload(); return r; }   /* seat roles depend on the format */
    r = nhl94_builds_handle_arg(key, val);
    if (r) return r;
    return nhl94_seats_handle_arg(key, val);
}

/* The format (6v6 / 4v4) rides inside the seats value ("4v4", "4v4,<list>",
 * "*4<letters>"), so a build without 4v4 refuses a 4v4 host: nhl94_seats.c
 * parse_seats, docs/FOUR_ON_FOUR_PLAN.md section 5. */
static void nhl94_netplay_config_image(char *out, size_t cap)
{
    nhl94_seats_config_image(out, cap);
}

/* The host's image for the session (the guest adopts it), or this build's
 * own image to end the adoption. */
static int nhl94_netplay_config_adopt(const char *line)
{
    return nhl94_seats_adopt(line);
}

static const char *nhl94_state_unavailable(void) {
    return "NHL 94 has no save-state resume point yet (quicksave/quickload disabled)";
}

const GameSpec g_game_spec = {
    .display_name           = "NHL Hockey '94",
    /* Up to 12 seats (6v6) reach the game (nhl94_seats.c: the game's own
     * four controller structures plus virtual pads for seats 5..12); the
     * engine clamps this to what it carries (GENESIS_SIM_MAX_PLAYERS,
     * INPUT_MAX_PLAYERS, the lobby's room size). */
    .logical_players        = NHL94_SEATS_MAX,
    /* The seat list changes the simulation: every peer must match it. */
    .netplay_config_image   = nhl94_netplay_config_image,
    .netplay_config_adopt   = nhl94_netplay_config_adopt,
    /* Launcher: Mods > Control mode (Classic / Position lock + seat roles),
     * persisted in nhl94-control.ini (nhl94_mode.c). */
    .load_settings          = nhl94_mode_load,
    .mods                   = nhl94_mode_mods,
    .short_name             = "NHL94",

    /* USA/EU cartridge dump: 1 MB, CRC32 9438F5DD, serial GM T-50656 -00. */
    .expected_rom_crc32     = 0x9438F5DDu,
    .expected_rom_size      = 0x100000u,

    /* Native by default (2026-09-23): discovery is disassembly-driven and
     * human games plus the attract loop run with an empty floor manifest,
     * so a dispatch miss is now a loud regression, as for the Sonic titles.
     * Set GENESIS_FLOOR=1 in the environment to re-enable the Tier-3 miss
     * fallback while diagnosing one (it feeds floor_coverage.toml). */
    .tier3_floor_default    = 0,

    /* NHL 94's V-blank routines drive the Z80 sound driver through a
     * request/acknowledge handshake in Z80 RAM ($A00096/97) and busy-wait
     * for the reply. Delivered atomically the scheduler cannot step the Z80
     * during the handler, so the wait never ends (instruction watchdog).
     * Interleaved delivery runs the handler on the game fiber and lets its
     * bus accesses yield to the scheduler, as on hardware. */
    .vint_interleave_default = 1,

    /* Header-declared SRAM; sram_start/end intentionally 0 (see file header). */

    .call_entry_point       = nhl94_call_entry_point,
    .call_vblank            = nhl94_call_vblank,
    .call_hblank            = nhl94_call_hblank,
    .resume_main_loop_pc    = 0u,
    .save_resume_pc         = NULL,
    .state_unavailable_reason = nhl94_state_unavailable,
    .dispatch_main_loop_pc  = 0u,
    .call_periodic          = NULL,

    .on_post_reset          = NULL,
    .on_frame_pre           = NULL,
    .on_frame_post          = NULL,
    .on_hblank              = NULL,

    /* --seats <list>: player lock seats (same syntax as NHL94_SEATS). */
    .handle_arg             = nhl94_handle_arg,
    .arg_usage              = "--seats home:C,away:G  per-skater human control (docs/PLAYER_LOCK.md)\n"
                              "  --format 6v6|4v4       4v4 = G, C, LW, LD per team (docs/FOUR_ON_FOUR_PLAN.md)\n"
                              "  --builds off|home:C=LA:Gretzky,away:G=TOR:Potvin,S=..  real-player builds (docs/BUILDS.md)",
    .dispatch_override      = NULL,
    /* game_hook sites (game.toml): per-player camera views (nhl94_view.c,
     * speculative view ticks only) and the player lock (nhl94_seats.c). */
    .instruction_hook       = nhl94_instruction_hook,

    .fill_frame_record      = NULL,
    .frame_record_version   = 0u,

    .commands               = NULL,
    .command_count          = 0,

    /* Speculative per-player views (runner/multiview.h). The game builds a
     * frame's display list in one tick and the next vblank uploads it, so
     * a view needs two ticks to show its own camera (nhl94_view.c). */
    .view_prepare           = nhl94_view_prepare,
    .view_user              = NULL,
    .view_ticks             = 2,
    .view_ready             = nhl94_view_ready,
    .view_default_count     = nhl94_view_default_count,

    /* Name tags of the humans on the ice, drawn onto the presented frames
     * (nhl94_tags.c; presentation only). */
    .present_overlay        = nhl94_view_present_overlay,
    .netplay_seat_label     = nhl94_seats_label,
    /* Waiting-room build picker (nhl94_builds.c). */
    .lobby_pick_current     = nhl94_builds_lobby_current,
    .lobby_pick_query       = nhl94_builds_lobby_query,
    .lobby_pick_set         = nhl94_builds_lobby_set,
    .pacer_catchup_default  = 1,   /* rollback netplay: hitches are caught up */
    .hv_counter_line_phase  = 1,   /* fatigue rolls on $C00008's H byte */
    /* Zoomed position-lock view: extra columns / rows (nhl94_view.c). */
    .view_geometry          = nhl94_view_geometry,
};
