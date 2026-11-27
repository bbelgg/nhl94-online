/* test_tagnames.c -- unit check of the seat -> lobby name mapping used by the
 * name tags (game/nhl94_tagnames.c), with a fake member table.
 *   cc -I../../game -o /tmp/test_tagnames test_tagnames.c ../../game/nhl94_tagnames.c && /tmp/test_tagnames
 * Prints what each seat's tag would show and exits non-zero on a mismatch. */
#include <stdio.h>
#include <string.h>
#include "nhl94_tagnames.h"

static int fails;
#define EXPECT(got, want) do { if (strcmp((got), (want))) { fails++; \
    printf("  FAIL: got \"%s\" want \"%s\"\n", (got), (want)); } } while (0)

static void M(NhlTagMember *m, int slot, int spec, const char *name)
{
    m->slot = slot; m->spectator = spec; snprintf(m->name, sizeof m->name, "%s", name);
}

int main(void)
{
    char out[32];
    /* a 4-person room; lobby seat == match seat (slot_port is the identity in
     * the SEAT policy and maps session slot -> the lobby seat it plays in HOST_FIRST) */
    NhlTagMember t[8];
    M(&t[0], 0, 0, "alice");                       /* host, lowercase */
    M(&t[1], 1, 0, "Bob the Builder 2000");        /* too long: truncated to 10 */
    M(&t[2], 3, 0, "Zoë_Ñ.7");                     /* non-ASCII -> '?' (one per char) */
    M(&t[3], 2, 1, "Watcher");                     /* spectator on slot 2: never a seat */
    M(&t[4], 5, 0, "   ");                         /* seated, blank name -> P6 */
    printf("seat -> tag name (local seat 1 'Bobby-Local', members: alice@0 Bob..@1 Zo..@3 spectator@2 blank@5)\n");
    const char *want[8] = { "ALICE", "BOB THE BU", "P3", "ZO?_?.7", "P5", "P6", "P7", "P8" };
    /* seat 1 is the local seat; its table row wins over local_name; seat 2's
     * only row is a spectator; seat 5's row is blank */
    for (int s = 0; s < 8; s++) {
        nhl94_tagname_for_seat(s, 1, "Bobby-Local", t, 5, out, sizeof out);
        printf("  seat %d (P%d) -> \"%s\"\n", s, s + 1, out);
        EXPECT(out, want[s]);
    }
    /* member table without the local row: the lobby's own display name */
    nhl94_tagname_for_seat(1, 1, "Bobby-Local", t + 0, 1, out, sizeof out);
    printf("  local fallback seat 1, table = {alice@0} -> \"%s\"\n", out);
    EXPECT(out, "BOBBY-LOCA");
    /* offline / LAN: no table, no local name */
    for (int s = 0; s < 12; s += 5) {
        nhl94_tagname_for_seat(s, 0, NULL, NULL, 0, out, sizeof out);
        printf("  offline seat %d -> \"%s\"\n", s, out);
    }
    nhl94_tagname_for_seat(11, 0, NULL, NULL, 0, out, sizeof out); EXPECT(out, "P12");
    nhl94_tagname_for_seat(0, 0, NULL, NULL, 0, out, sizeof out);  EXPECT(out, "P1");
    nhl94_tagname_sanitize("  padded  name here", out, sizeof out); EXPECT(out, "PADDED  NA");
    nhl94_tagname_sanitize("ab        cd", out, sizeof out);       EXPECT(out, "AB");    /* trailing space trimmed after cut */
    nhl94_tagname_sanitize("", out, sizeof out);                   EXPECT(out, "");
    nhl94_tagname_sanitize("日本語", out, sizeof out);               EXPECT(out, "???");
    /* a seat that plays as a real roster player (builds, nhl94_builds.c): his last name wins */
    nhl94_tagname_for_seat_build(0, 1, "Bobby-Local", t, 5, "Gretzky", out, sizeof out);   EXPECT(out, "GRETZKY");
    nhl94_tagname_for_seat_build(0, 1, NULL, t, 5, "Jocelyn Lemieux", out, sizeof out);    EXPECT(out, "JOCELYN LE");
    nhl94_tagname_for_seat_build(0, 1, NULL, t, 5, "", out, sizeof out);                   EXPECT(out, "ALICE");   /* no build */
    nhl94_tagname_for_seat_build(1, 1, "Bobby-Local", t, 5, NULL, out, sizeof out);        EXPECT(out, "BOB THE BU");
    nhl94_tagname_for_seat_build(7, 0, NULL, NULL, 0, "O'Neill", out, sizeof out);         EXPECT(out, "O?NEILL");
    printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails != 0;
}
