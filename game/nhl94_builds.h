/*
 * nhl94_builds.h - per-player "builds": each seat plays with the attributes
 * (and name) of a REAL NHL 94 roster player of the player's choice, or of
 * the team's 1st-line player for the seat's role by default. See
 * nhl94_builds.c and docs/BUILDS.md.
 */
#ifndef NHL94_BUILDS_H
#define NHL94_BUILDS_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct GamePickRow;

/* Builds are on for the seats in play (default). NHL94_BUILDS=off / --builds
 * off keeps the cartridge's ratings (the pre-builds hashes). */
int  nhl94_builds_enabled(void);

/* ---- session image: the "+B" part of the seats value (nhl94_seats.c) ----
 * "+B" <T4> <tok per seat>, T4 = low 16 bits of the roster table CRC (4 hex
 * digits), tok = 2 chars of 0-9A-V (team * 32 + roster slot, base 32) or "--"
 * (the team's 1st-line player). Seats in the seat list's order. */
/* This process's own choice for seats team[i]/role[i], i < n. "" when builds
 * are off or n == 0. */
void nhl94_builds_suffix(const int *team, const int *role, int n, char *out, size_t cap);
/* Parse a "+B..." segment (NULL / "" = no builds). 0 = ok, tokens in tok[]
 * (-1 = default), *n = how many; non-zero = refuse (bad CRC, token). */
int  nhl94_builds_parse(const char *seg, int *tok, int max, int *n);
/* Adopt the session's segment (the host's image; NULL = none -> builds off).
 * Non-zero = cannot run it. Used only while a netplay session is active. */
int  nhl94_builds_adopt(const char *seg);
/* Waiting-room: the seat's build under `seg`: label "Gretzky (LA)" or "" for
 * the default. 1 when the seat has a non-default build. */
int  nhl94_builds_seat_text(const char *seg, int seat, char *out, size_t cap);

/* GameSpec.handle_arg: "--builds <spec>". Returns argv entries consumed. */
int  nhl94_builds_handle_arg(const char *key, const char *val);

/* ---- the simulation side (guest RAM $FFE000, game.toml sites) ---- */
/* restoreteams .r: latch the seats' builds for the team in a2 (idempotent). */
void nhl94_builds_latch(void);
/* $015BA6 (setplayer attribute record), $018BC8 (GetPlayerNamePointer),
 * $00A9D6 (trace only). */
int  nhl94_builds_instruction_hook(uint32_t pc, int *mine);

/* Presentation (name tags): the last name of seat's latched build, upper-case
 * source form ("Gretzky"); "" when the seat has no build (Classic, builds off,
 * before the latch). Reads guest RAM. */
void nhl94_builds_seat_name(int seat, char *out, size_t cap);

/* Tell the lobby (so the host) this machine's picks; idempotent. */
void nhl94_builds_announce(void);

/* ---- persistence (nhl94-control.ini keys build_skater / build_goalie) ---- */
int  nhl94_builds_ini_key(const char *key, const char *val);      /* 1 = handled */
void nhl94_builds_ini_write(FILE *f);

/* ---- waiting room (GameSpec.lobby_pick_*) ---- */
int  nhl94_builds_lobby_current(const char *image, int seat, char *out, size_t cap);
int  nhl94_builds_lobby_query(const char *image, int seat, const char *filter,
                              struct GamePickRow *rows, int max);
int  nhl94_builds_lobby_set(const char *image, int seat, int id);

/* Pure helpers (tools/builds/test_roster_table.c) */
int  nhl94_builds_token_encode(int team, int slot, char out[3]);
int  nhl94_builds_token_decode(const char *two, int *team, int *slot);   /* "--" -> 0 with -1,-1 */
int  nhl94_builds_filter_match(const char *filter, int player_index);

#endif
