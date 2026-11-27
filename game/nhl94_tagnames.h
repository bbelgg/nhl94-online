/*
 * nhl94_tagnames.h - which name a human-controlled player's tag shows
 * (pure logic, no engine dependencies, unit-tested by
 * tools/seats/test_tagnames.c). See nhl94_tags.c and docs/PLAYER_LOCK.md,
 * "Name tags".
 */
#ifndef NHL94_TAGNAMES_H
#define NHL94_TAGNAMES_H
#include <stddef.h>

#define NHL94_TAGNAME_MAX 10   /* displayed characters */

/* One row of the lobby's live member table (RNetLobbyMember, reduced to what
 * the mapping needs). */
typedef struct {
    int  slot;        /* lobby seat == the match's logical player (seat) index */
    int  spectator;   /* gallery row: never a seat */
    char name[64];
} NhlTagMember;

/* Display form of a lobby name: upper case, only what the 5x7 font draws
 * (A-Z 0-9 space - : . _ ; anything else, and each UTF-8 sequence, becomes a
 * single '?'), outer spaces trimmed, at most NHL94_TAGNAME_MAX characters.
 * Returns the length; out is "" for an empty / blank name. cap >= 1. */
int nhl94_tagname_sanitize(const char *in, char *out, size_t cap);

/* The name for seat `seat` (0-based): the non-spectator member whose slot is
 * `seat` (first one wins); else, for the local seat, `local_name` (the lobby's
 * own display name; may be NULL); else "P<seat+1>" (offline, LAN, or a seat
 * the table does not know). `members` may be NULL with n = 0. */
void nhl94_tagname_for_seat(int seat, int local_seat, const char *local_name,
                            const NhlTagMember *members, int n, char *out, size_t cap);

/* The same, for a seat whose player plays as a real roster player (a build,
 * nhl94_builds.c): that player's last name (`build_name`, sanitized the same
 * way) wins over the lobby name. NULL / "" / blank = as nhl94_tagname_for_seat. */
void nhl94_tagname_for_seat_build(int seat, int local_seat, const char *local_name,
                                  const NhlTagMember *members, int n, const char *build_name,
                                  char *out, size_t cap);

#endif
