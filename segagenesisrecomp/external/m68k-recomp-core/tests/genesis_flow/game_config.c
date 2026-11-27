#include "game_config.h"

bool game_config_is_blacklisted(const GameConfig *c, uint32_t a) { (void)c; (void)a; return false; }
bool game_config_has_code_oracle(const GameConfig *c) { return c && c->code_addr_count > 0; }
bool game_config_is_known_code(const GameConfig *c, uint32_t a)
{
    if (!game_config_has_code_oracle(c)) return true;
    for (int i = 0; i < c->code_addr_count; i++)
        if (c->code_addrs[i] == a) return true;
    return false;
}
uint32_t game_config_next_known_code(const GameConfig *c, uint32_t a)
{
    for (int i = 0; c && i < c->code_addr_count; i++)
        if (c->code_addrs[i] >= a) return c->code_addrs[i];
    return 0;
}
int game_config_inline_arg_kind(const GameConfig *c, uint32_t callee)
{
    for (int i = 0; c && i < c->inline_word_callee_count; i++)
        if (c->inline_word_callees[i] == callee) return 1;
    return 0;
}
bool game_config_has_runtime_oracle(const GameConfig *c) { (void)c; return false; }
bool game_config_runtime_table_promotions(const GameConfig *c) { (void)c; return false; }
bool game_config_runtime_observed(const GameConfig *c, uint32_t a) { (void)c; (void)a; return false; }
