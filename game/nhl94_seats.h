/*
 * nhl94_seats.h - per-skater human control ("player lock") for NHL Hockey
 * '94. Each seat (logical player of the simulation input) controls exactly
 * one on-ice role of one team; every other player stays under the game's AI.
 * See nhl94_seats.c and docs/PLAYER_LOCK.md.
 */
#ifndef NHL94_SEATS_H
#define NHL94_SEATS_H
#include <stddef.h>
#include <stdint.h>

/* Seats: 12 = 6v6, every on-ice player of both teams (goalies included).
 * Seats 1..4 (index 0..3) are the game's own pads 1..4 (cont1team..cont4team,
 * c1playernum..c4playernum, PadRaw1..4); seats 5..12 (index 4..11) are
 * virtual pads in unused guest RAM, swapped through pad 3's / pad 4's slots
 * (nhl94_seats.c, docs/PLAYER_LOCK.md). A seat beyond the engine's
 * GENESIS_SIM_MAX_PLAYERS gets no input and stays AI. */
#define NHL94_SEATS_MAX 12
#define NHL94_GAME_PADS 4

/* Parsed seat list (NHL94_SEATS or --seats). 0 = player lock off (the game
 * runs exactly as the ROM does). */
int  nhl94_seats_count(void);
/* Waiting-room label of seat `seat` under a host "game" image ("Home C"); 0 = none. */
int  nhl94_seats_label(const char *image, int seat, char *out, size_t cap);
/* The seat's team side (0 home / 1 away) and role under a host "game" image
 * (the waiting-room picker; nhl94_builds.c). 1 = it is a seat of that image. */
int  nhl94_seats_image_role(const char *image, int seat, int *team, int *role);
/* The lock's own gate: seats configured and not in highlights / a penalty
 * shot or shootout / the scoreboard rink. Reads guest RAM. */
int  nhl94_seats_lock_active(void);
/* Seat i: team 0 home / 1 away, role NHL94_POS_*. 0 when i is not a seat. */
int  nhl94_seats_get(int i, int *team, int *role);
/* The seat this process presents (its own view): the netplay local player
 * online, NHL94_LOCAL_SEAT (default 0) offline; -1 = none (spectator). */
int  nhl94_seats_local_seat(void);

/* The launcher's Control mode (nhl94_mode.c): a seat list, "" = Classic.
 * Lowest precedence: NHL94_SEATS env > --seats > launcher. */
void nhl94_seats_set_launcher(const char *list);
/* GameSpec.netplay_config_adopt: adopt a session's game image (the host's);
 * 0 = ok, non-zero = cannot run it. */
int  nhl94_seats_adopt(const char *image);

/* Re-read the local seat list (after a format change: in 4v4 only G / LD /
 * LW / C seats are valid, nhl94_format.h). */
void nhl94_seats_reload(void);
/* GameSpec.handle_arg: "--seats <list>". Returns argv entries consumed. */
int  nhl94_seats_handle_arg(const char *key, const char *val);
/* Session config line(s) for GameSpec.netplay_config_image. */
void nhl94_seats_config_image(char *out, size_t cap);

/* Presentation (name tags, nhl94_tags.c): for each on-ice object 0..11 the
 * seat (0-based) that controls it right now, -1 = AI. Reads guest RAM only.
 * Position lock: seat k's object as the lock resolved it this frame (its role
 * body, a substitute, or the shooter / goalie of a penalty shot: the
 * NHL94_SEATTGT array enforce writes), if it is pfjoycon. Classic (or the lock suspended for a
 * penalty shot / highlights): the objects the game's pads control
 * (c1playernum..c4playernum with cont1team..cont4team set, and pfjoycon);
 * pad i = seat i. */
void nhl94_seats_human_objects(int8_t obj_seat[12]);

/* The object (SCnum 0..11) seat `seat` controls this frame under Position lock
 * -- role body, sticky substitute, or penalty-shot shooter / goalie -- or -1
 * (no human, nobody on the ice, lock off). Presentation (camera, tags) and
 * the aim / call-for-pass code use this, never the role scan. */
int  nhl94_seats_control_object(int seat);
/* Presentation (call-for-pass icon, nhl94_tags.c): per on-ice object 0..11,
 * bit 0 = the human controlling it is CALLING (his seat holds B / X while a
 * teammate carries the puck), bit 1 = QUEUED (an AI carrier was asked for him,
 * the request is pending). Guest RAM only; all 0 outside Position lock. */
void nhl94_seats_call_flags(uint8_t obj_flags[12]);

/* TEST-ONLY sim knobs (NHL94_PASSASSIST, NHL94_SEATS_NO_AUTOLC,
 * NHL94_SEATS_KEEP_CAMERA_RULE, NHL94_SEATS_TEST_PS, NHL94_FORMAT_TEST_PENALTY)
 * change the simulation but are host environment, not part of the sealed
 * session: one peer with one left in its shell would desync a room. This is 1
 * when the knob may act (offline: tests, rb_probe), 0 while netplay is active
 * (logged once per `name`). Evaluate at use time, not when the env is read:
 * the session may come up after the first sim tick of a process. */
int  nhl94_sim_knob_allowed(const char *name);

/* One of the seat hook sites (game.toml). *mine = 1 when pc is a seat site;
 * the return value is the GameSpec.instruction_hook result. */
int  nhl94_seats_instruction_hook(uint32_t pc, int *mine);

#endif /* NHL94_SEATS_H */
