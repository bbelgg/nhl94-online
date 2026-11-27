/*
 * nhl94_format.h - game format: 6v6 (the cartridge) or 4v4 (G, C, LW, LD per
 * team). See nhl94_format.c and docs/FOUR_ON_FOUR_PLAN.md.
 */
#ifndef NHL94_FORMAT_H
#define NHL94_FORMAT_H
#include <stddef.h>
#include <stdint.h>

#define NHL94_FORMAT_6V6 6
#define NHL94_FORMAT_4V4 4

/* The format this process offers (what it publishes when hosting):
 * NHL94_FORMAT env > --format > launcher > 6v6. */
int  nhl94_format_local(void);
/* The format the next game starts with: the host's (adopted) online, else
 * the local one. Read ONLY by the restoreteams hook, which latches it into
 * guest RAM (NHL94_FMT); every other hook reads the guest. */
int  nhl94_format_effective(void);
/* Launcher setting (nhl94_mode.c), lowest precedence. */
void nhl94_format_set_launcher(int fmt);
/* "--format 6v6|4v4". Returns argv entries consumed (0 = not ours). */
int  nhl94_format_handle_arg(const char *key, const char *val);
/* "6v6" / "4v4" -> 6 / 4, else 0. */
int  nhl94_format_parse(const char *v);
const char *nhl94_format_name(int fmt);
/* Netplay adoption of the host's format (from the image's seats value,
 * nhl94_seats.c); fmt = 0 ends the adoption. */
void nhl94_format_adopt(int fmt);
/* Is role (NHL94_POS_*) a seat role in this format? 4v4: G, LD, LW, C only. */
int  nhl94_format_role_ok(int fmt, int role);

/* One of the 4v4 hook sites (game.toml). *mine = 1 when pc is one; the
 * return value is the GameSpec.instruction_hook result. No-op (returns 0,
 * writes nothing) while guest FMT != 4, except the restoreteams latch. */
int  nhl94_format_instruction_hook(uint32_t pc, int *mine);

#endif /* NHL94_FORMAT_H */
