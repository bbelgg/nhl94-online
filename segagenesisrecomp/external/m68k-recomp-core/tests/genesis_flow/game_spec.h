#pragma once
#include <stdint.h>
typedef struct { int (*dispatch_override)(uint32_t addr); } GameSpec;
extern GameSpec g_game_spec;
