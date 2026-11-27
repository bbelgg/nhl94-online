/* test_format_image.c -- unit check of the 4v4 format in the netplay session
 * image (game/nhl94_seats.c + game/nhl94_format.c): the image each format and
 * seat list produces, that it fits GenesisSessionConfig.game[48] (12 compact
 * seats included), that the guest's adopt + label round-trip it, and that
 * RD/RW seats in 4v4 are refused.
 * Build with the game's own include flags (netplay off for the stubs):
 *   B=<build dir>; F=$(cd $B && ninja -t commands NHL94Recomp | grep -m1 nhl94_seats.c | tr ' ' '\n' \
 *       | grep '^-[ID]' | sed 's/RECOMP_NET=1/RECOMP_NET=0/' | tr '\n' ' ')
 *   eval cc -w $F -o /tmp/test_format_image test_format_image.c ../../game/nhl94_seats.c \
 *       ../../game/nhl94_format.c ../../game/nhl94_builds.c <build>/nhl94_roster/nhl94_roster_table.c ../../game/nhl94_aim.c -lm && /tmp/test_format_image
 * The per-player builds segment ("+B...", tools/seats/test_roster_table.c) is switched off here
 * (NHL94_BUILDS=off): these are the seat-list images as they were before builds.
 * Exits non-zero on a mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nhl94_seats.h"
#include "nhl94_format.h"
#include "genesis_runtime.h"

/* ---- stubs for the engine symbols the two files reference ---- */
M68KState g_cpu;
uint8_t  glue_peek8(uint32_t a) { (void)a; return 0; }
uint16_t glue_peek16(uint32_t a) { (void)a; return 0; }
uint32_t glue_peek32(uint32_t a) { (void)a; return 0; }
void     glue_poke32(uint32_t a, uint32_t v) { (void)a; (void)v; }
void     nhl94_mode_save(void) {}
void     glue_poke8(uint32_t a, uint8_t v) { (void)a; (void)v; }
void     glue_poke16(uint32_t a, uint16_t v) { (void)a; (void)v; }
uint32_t genesis_sim_pad(int i) { (void)i; return 0; }
uint32_t genesis_sim_human_mask(void) { return 0; }
uint32_t genesis_sim_tick_count(void) { return 0; }
int      genesis_multiview_speculating(void) { return 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void image(const char *fmt, const char *seats, char *out)
{
    setenv("NHL94_FORMAT", fmt, 1);
    setenv("NHL94_SEATS", seats, 1);
    nhl94_seats_reload();
    out[0] = 0;
    nhl94_seats_config_image(out, 48);
}

int main(void)
{
    setenv("NHL94_BUILDS", "off", 1);
    char img[48], lab[32];
    static const char *s12 = "home:C,away:C,home:LW,away:LW,home:RW,away:RW,home:LD,away:LD,home:RD,away:RD,home:G,away:G";
    static const char *s8 = "home:C,away:C,home:LW,away:LW,home:LD,away:LD,home:G,away:G";
    struct { const char *fmt, *seats, *want; } c[] = {
        { "6v6", "", "" },
        { "6v6", "home:C,away:G", "nhl94.seats=p2:home:C,away:G" },
        { "4v4", "", "nhl94.seats=4v4" },
        { "4v4", "home:C,away:G", "nhl94.seats=p2:4v4,home:C,away:G" },
        { "4v4", s8, "nhl94.seats=p2:*4ekdjbhag" },
        { "6v6", s12, "nhl94.seats=p2:*ekdjflbhciag" },
        { "4v4", "home:C,home:RD", "nhl94.seats=4v4" },     /* RD refused in 4v4 -> Classic 4v4 */
    };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        image(c[i].fmt, c[i].seats, img);
        printf("%s %-40.40s -> \"%s\" (%zu bytes)\n", c[i].fmt, c[i].seats, img, strlen(img));
        CHECK(!strcmp(img, c[i].want), "want \"%s\"", c[i].want);
        CHECK(strlen(img) < 48, "image over 47 bytes");
    }
    /* Guest: local 6v6 Classic adopts the host's 4v4 8-seat image. */
    setenv("NHL94_FORMAT", "6v6", 1);
    setenv("NHL94_SEATS", "off", 1);
    nhl94_seats_reload();
    CHECK(nhl94_seats_adopt("nhl94.seats=p2:*4ekdjbhag") == 0, "adopt 4v4 compact");
    CHECK(nhl94_format_effective() == NHL94_FORMAT_4V4, "effective format after adopt = %d", nhl94_format_effective());
    CHECK(nhl94_seats_count() == 8, "8 seats adopted (%d)", nhl94_seats_count());
    CHECK(nhl94_seats_label("nhl94.seats=p2:*4ekdjbhag", 2, lab, sizeof lab) && !strcmp(lab, "Home LW"), "label seat 2 = %s", lab);
    CHECK(nhl94_seats_adopt("nhl94.seats=p2:4v4,home:C,away:RW") != 0, "4v4 + RW refused");
    CHECK(nhl94_seats_adopt("nhl94.seats=p2:*4ef") != 0, "compact 4v4 + RW refused");
    CHECK(nhl94_seats_adopt("nhl94.seats=4v4") == 0 && nhl94_format_effective() == NHL94_FORMAT_4V4 &&
          nhl94_seats_count() == 0, "Classic 4v4 adopted");
    /* Pass-logic marker (nhl94_seats.c SEATS_PASS_VER): a value with seats and no marker is an older build's
     * session (no aimed passing) and is refused; so is a newer marker; the "+B..." builds suffix is the
     * other owner's. Every old grammar form fails an OLD parser by itself (see the marker comment). */
    CHECK(nhl94_seats_adopt("nhl94.seats=home:C,away:G") != 0, "unmarked seats refused");
    CHECK(nhl94_seats_adopt("nhl94.seats=4v4,home:C,away:G") != 0, "unmarked 4v4 seats refused");
    CHECK(nhl94_seats_adopt("nhl94.seats=*ekd") != 0, "unmarked compact refused");
    CHECK(nhl94_seats_adopt("nhl94.seats=p3:home:C,away:G") != 0, "newer marker refused");
    CHECK(nhl94_seats_adopt("nhl94.seats=p1:home:C,away:G") != 0, "p1 (before the drop-in / penalty-shot sim changes) refused");
    CHECK(nhl94_seats_adopt("nhl94.seats=p0:home:C") != 0, "p0 is no marker");
    CHECK(nhl94_seats_adopt("nhl94.seats=p2:home:C,away:G") == 0 && nhl94_seats_count() == 2, "p2 list adopted");
    CHECK(nhl94_seats_label("nhl94.seats=p2:home:C,away:G", 1, lab, sizeof lab) && !strcmp(lab, "Away G"), "p2 label");
    CHECK(nhl94_seats_label("nhl94.seats=p2:home:C,away:G", 1, lab, sizeof lab) == 1, "a role is 1");
    CHECK(nhl94_seats_label("nhl94.seats=p2:home:C,away:G", 2, lab, sizeof lab) == 2 && !strcmp(lab, "No position"),
          "past the seat list = 2 \"No position\" (%s)", lab);
    CHECK(nhl94_seats_adopt("nhl94.seats=p2:4v4") == 0, "marker on a seatless 4v4 is harmless");
    CHECK(nhl94_seats_adopt("") == 0 && nhl94_format_effective() == NHL94_FORMAT_6V6, "no key = 6v6 (local again)");
    printf("%s\n", fails ? "FAILED" : "all OK");
    return fails != 0;
}
