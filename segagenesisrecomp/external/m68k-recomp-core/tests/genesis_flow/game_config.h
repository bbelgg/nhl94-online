/* Minimal consumer-owned GameConfig for the synthetic codegen-flow test.
 * Only the fields/queries the genesis profile reads are provided; every
 * discovery feature is off unless a test sets it here. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { JT_FMT_ABS_L = 0, JT_FMT_PCREL_W = 1, JT_FMT_BRA_W = 2, JT_FMT_BRA_S = 3 } JumpTableFormat;
typedef struct { uint32_t start_addr, end_addr, stride_bytes; JumpTableFormat format; } JumpTableEntry;
typedef enum {
    WS_SITE_MASK10 = 0, WS_SITE_ADDREG, WS_SITE_SUBREG, WS_SITE_CULL_LEFT, WS_SITE_ADDIMM,
    WS_SITE_SUBIMM, WS_SITE_CALL_WIDEN, WS_SITE_CULL_WINDOW_LEFT, WS_SITE_ADDMEM,
    WS_SITE_GAME_HOOK,
} WsSiteKind;
typedef struct { uint32_t addr; WsSiteKind kind; uint8_t reg, shift, scale; uint16_t base;
                 uint32_t target, gate; } WsSite;

typedef struct {
    char            output_prefix[64];
    JumpTableEntry *jump_tables;
    int             jump_table_count;
    uint32_t       *extra_funcs;
    int             extra_func_count;
    uint32_t       *late_extra_funcs;
    int             late_extra_func_count;
    uint32_t       *function_pointer_helpers;
    int             function_pointer_helper_count;
    uint32_t       *extra_seeds;
    int             extra_seed_count;
    uint32_t        vblank_yield_addr;
    bool            allow_68020_branch;
    bool            jump_table_autodiscovery;
    bool            dynamic_return_slots;
    bool            function_aliases;
    WsSite         *ws_sites;
    int             ws_site_count;

    /* Test-owned: [[inline_args]] callees (length = "word") and an optional
     * code oracle (sorted list of known instruction starts). */
    uint32_t        inline_word_callees[8];
    int             inline_word_callee_count;
    uint32_t        code_addrs[64];
    int             code_addr_count;
} GameConfig;

bool     game_config_is_blacklisted(const GameConfig *cfg, uint32_t addr);
bool     game_config_is_known_code(const GameConfig *cfg, uint32_t addr);
bool     game_config_has_code_oracle(const GameConfig *cfg);
int      game_config_inline_arg_kind(const GameConfig *cfg, uint32_t callee);
uint32_t game_config_next_known_code(const GameConfig *cfg, uint32_t addr);
bool     game_config_has_runtime_oracle(const GameConfig *cfg);
bool     game_config_runtime_table_promotions(const GameConfig *cfg);
bool     game_config_runtime_observed(const GameConfig *cfg, uint32_t addr);
