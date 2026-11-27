/* test_roster_table.c -- unit check of the builds' roster table, tokens, search, picker rows and
 * session-image segment (<build>/nhl94_roster/nhl94_roster_table.c, generated from the ROM, game/nhl94_builds.c, the +B part of
 * game/nhl94_seats.c), against the real ROM through a file-backed glue_peek.
 * Build with the game's own include flags (netplay off for the stubs):
 *   B=<build dir>; F=$(cd $B && ninja -t commands NHL94Recomp | grep -m1 nhl94_seats.c | tr ' ' '\n' \
 *       | grep '^-[ID]' | sed 's/RECOMP_NET=1/RECOMP_NET=0/' | tr '\n' ' ')
 *   eval cc -w $F -o /tmp/test_roster_table test_roster_table.c ../../game/nhl94_seats.c ../../game/nhl94_format.c \
 *       ../../game/nhl94_builds.c ../../game/nhl94_roster_table.c ../../game/nhl94_aim.c -lm && /tmp/test_roster_table [rom]
 * Exits non-zero on a mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nhl94_seats.h"
#include "nhl94_format.h"
#include "nhl94_builds.h"
#include "nhl94_roster_table.h"
#include "game_spec.h"
#include "genesis_runtime.h"

/* ---- stubs: a ROM-backed bus, RAM reads 0 ---- */
static uint8_t t_rom[0x100000];
M68KState g_cpu;
uint8_t  glue_peek8(uint32_t a) { return a < 0x100000 ? t_rom[a] : 0; }
uint16_t glue_peek16(uint32_t a) { return (uint16_t)(glue_peek8(a) << 8 | glue_peek8(a + 1)); }
uint32_t glue_peek32(uint32_t a) { return (uint32_t)glue_peek16(a) << 16 | glue_peek16(a + 2); }
void     glue_poke8(uint32_t a, uint8_t v) { (void)a; (void)v; }
void     glue_poke16(uint32_t a, uint16_t v) { (void)a; (void)v; }
void     glue_poke32(uint32_t a, uint32_t v) { (void)a; (void)v; }
uint32_t genesis_sim_pad(int i) { (void)i; return 0; }
uint32_t genesis_sim_human_mask(void) { return 0; }
uint32_t genesis_sim_tick_count(void) { return 0; }
int      genesis_multiview_speculating(void) { return 0; }
void     nhl94_mode_save(void) {}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
    FILE *f = fopen(argc > 1 ? argv[1] : "../../game/NHL94.bin", "rb");
    if (!f || fread(t_rom, 1, sizeof t_rom, f) != sizeof t_rom) { printf("cannot read the ROM\n"); return 2; }
    fclose(f);

    /* ---- the table ---- */
    CHECK(NHL_NTEAMS == 28 && g_nhl_nplayers == 684, "28 teams / 684 players (got %d / %u)", NHL_NTEAMS, g_nhl_nplayers);
    unsigned goalies = 0, total = 0;
    for (int t = 0; t < NHL_NTEAMS; t++) {
        const NhlRosterTeam *tm = &g_nhl_teams[t];
        CHECK(tm->first == total && tm->count > 0 && tm->count <= 26, "team %d first/count", t);
        total += tm->count;
        for (int s = 0; s < tm->count; s++) {
            const NhlRosterPlayer *p = &g_nhl_players[tm->first + s];
            CHECK(p->team == t && p->slot == s, "order team %d slot %d", t, s);
            goalies += p->flags & 1;
        }
        CHECK(g_nhl_players[tm->first + tm->line1[0] - 1].flags & 1, "%s line-1 G is a goalie", tm->abbr);
        for (int r = 1; r < 6; r++)
            CHECK(!(g_nhl_players[tm->first + tm->line1[r] - 1].flags & 1), "%s line-1 role %d is a skater", tm->abbr, r);
    }
    CHECK(total == g_nhl_nplayers, "sum of rosters");
    printf("table: %u players, %u goalies, crc %08X\n", total, goalies, g_nhl_table_crc);

    /* ---- tokens ---- */
    for (unsigned i = 0; i < g_nhl_nplayers; i++) {
        char tk[3];
        int t, s;
        const NhlRosterPlayer *p = &g_nhl_players[i];
        CHECK(nhl94_builds_token_encode(p->team, p->slot, tk) && !nhl94_builds_token_decode(tk, &t, &s) &&
              t == p->team && s == p->slot, "token round trip %u", i);
    }
    int t, s;
    CHECK(!nhl94_builds_token_decode("--", &t, &s) && t == -1, "-- is the default");
    CHECK(nhl94_builds_token_decode("ZZ", &t, &s) != 0, "ZZ invalid");
    CHECK(nhl94_builds_token_decode("V0", &t, &s) != 0, "team 31 invalid");
    char big[3];
    CHECK(!nhl94_builds_token_encode(0, g_nhl_teams[0].count, big), "slot past the roster refused");
    CHECK(nhl94_builds_token_decode("0Z", &t, &s) != 0, "slot 31 of team 0 (18 players) refused");

    /* ---- search ---- */
    int gretzky = -1, nmatch = 0;
    for (unsigned i = 0; i < g_nhl_nplayers; i++) {
        if (gretzky < 0 && !strcmp(g_nhl_players[i].name, "Wayne Gretzky")) gretzky = (int)i;   /* the Kings' (also on ASW) */
        nmatch += nhl94_builds_filter_match("gretz", (int)i);
    }
    CHECK(gretzky >= 0 && nmatch == 2, "'gretz' finds Gretzky on LA and on All-Stars West (%d)", nmatch);
    CHECK(nhl94_builds_filter_match("la gretzky", gretzky), "team abbreviation + name");
    CHECK(nhl94_builds_filter_match("Los Angeles Kings", gretzky) || nhl94_builds_filter_match("kings", gretzky), "city / nickname");
    CHECK(nhl94_builds_filter_match("#99", gretzky) && nhl94_builds_filter_match("99", gretzky), "jersey number");
    CHECK(!nhl94_builds_filter_match("gretzky mtl", gretzky), "AND of words");
    CHECK(nhl94_builds_filter_match("", gretzky) && nhl94_builds_filter_match("   ", gretzky), "empty filter matches");

    /* ---- the session segment ---- */
    setenv("NHL94_FORMAT", "6v6", 1);
    setenv("NHL94_SEATS", "home:C,away:G,home:LW", 1);
    setenv("NHL94_BUILDS", "home:C=LA:Gretzky,away:G=TOR:Potvin,S=ASE:Messier", 1);
    nhl94_seats_reload();
    char img[96], lab[96];
    img[0] = 0;
    nhl94_seats_config_image(img, sizeof img);
    printf("image: %s (%zu bytes)\n", img, strlen(img));
    CHECK(strstr(img, "nhl94.seats=p2:home:C,away:G,home:LW+B") != NULL, "image shape");
    CHECK(nhl94_seats_label(img, 0, lab, sizeof lab) && strstr(lab, "Home C: Gretzky (LA)"), "label 0 = %s", lab);
    CHECK(nhl94_seats_label(img, 1, lab, sizeof lab) && strstr(lab, "Away G: Potvin (TOR)"), "label 1 = %s", lab);
    CHECK(nhl94_seats_label(img, 2, lab, sizeof lab) && strstr(lab, "Home LW: Messier"), "label 2 = %s", lab);
    CHECK(nhl94_seats_adopt(img) == 0, "adopt own image");
    char bad[96];
    snprintf(bad, sizeof bad, "%s", img);
    char *p = strstr(bad, "+B");
    p[2] = p[2] == '0' ? '1' : '0';
    CHECK(nhl94_seats_adopt(bad) != 0, "another table id refused");
    snprintf(bad, sizeof bad, "%s", img);
    p = strstr(bad, "+B");
    p[7] = 'Z'; p[8] = 'Z';
    CHECK(nhl94_seats_adopt(bad) != 0, "a bad token refused");
    CHECK(nhl94_seats_adopt("nhl94.seats=p2:home:C,away:G") == 0, "no segment adopted (builds off for the session)");
    /* the 12-seat compact image with builds fits GenesisSessionConfig.game[96] */
    setenv("NHL94_SEATS", "home:C,away:C,home:LW,away:LW,home:RW,away:RW,home:LD,away:LD,home:RD,away:RD,home:G,away:G", 1);
    nhl94_seats_reload();
    char big96[96];
    big96[0] = 0;
    nhl94_seats_config_image(big96, sizeof big96);
    printf("12 seats: %s (%zu bytes)\n", big96, strlen(big96));
    CHECK(strlen(big96) < 96 && strstr(big96, "+B"), "12-seat image fits");
    /* 4v4: the format rides in the value, the segment after it */
    setenv("NHL94_FORMAT", "4v4", 1);
    setenv("NHL94_SEATS", "home:C,away:C,home:LW,away:LW,home:LD,away:LD,home:G,away:G", 1);
    setenv("NHL94_BUILDS", "home:G=ASE:Roy,S=LA:Gretzky", 1);
    nhl94_seats_reload();
    big96[0] = 0;
    nhl94_seats_config_image(big96, sizeof big96);
    printf("4v4 8 seats: %s (%zu bytes)\n", big96, strlen(big96));
    CHECK(!strncmp(big96, "nhl94.seats=p2:*4ekdjbhag+B", 27), "4v4 compact + segment");
    CHECK(nhl94_seats_label(big96, 6, lab, sizeof lab) && !strcmp(lab, "Home G: Roy (ASE)"), "4v4 label = %s", lab);
    CHECK(nhl94_seats_adopt(big96) == 0 && nhl94_format_effective() == NHL94_FORMAT_4V4 && nhl94_seats_count() == 8, "4v4 adopt");
    setenv("NHL94_FORMAT", "6v6", 1);
    nhl94_seats_adopt("");
    /* the 12-seat 6v6 list with builds off: the pre-builds image */
    setenv("NHL94_SEATS", "home:C,away:C,home:LW,away:LW,home:RW,away:RW,home:LD,away:LD,home:RD,away:RD,home:G,away:G", 1);
    setenv("NHL94_BUILDS", "off", 1);
    nhl94_seats_reload();
    big96[0] = 0;
    nhl94_seats_config_image(big96, sizeof big96);
    CHECK(!strcmp(big96, "nhl94.seats=p2:*ekdjflbhciag"), "builds off = old image (%s)", big96);

    /* ---- the picker ---- */
    setenv("NHL94_BUILDS", "", 1);
    setenv("NHL94_SEATS", "home:C,away:G", 1);
    nhl94_seats_reload();
    img[0] = 0;
    nhl94_seats_config_image(img, sizeof img);
    static GamePickRow rows[800];
    int ns = nhl94_builds_lobby_query(img, 0, "", rows, 800);
    int ng = nhl94_builds_lobby_query(img, 1, "", rows, 800);
    CHECK(ns == 684 - 65 && ng == 65, "skater seat lists %d, goalie seat %d", ns, ng);
    ns = nhl94_builds_lobby_query(img, 0, "", rows, 800);
    int groups = 0, seen = 0;
    char last[32] = "";
    for (int i = 0; i < ns; i++) {
        if (strcmp(rows[i].group, last)) { groups++; snprintf(last, sizeof last, "%s", rows[i].group); }
        if (rows[i].id == (g_nhl_players[gretzky].team << 5 | g_nhl_players[gretzky].slot)) seen = 1;
    }
    CHECK(groups == 28 && seen, "rows grouped by team: %d groups, Gretzky %s", groups, seen ? "listed" : "MISSING");
    int n = nhl94_builds_lobby_query(img, 0, "potvin", rows, 800);
    CHECK(n == 0, "Potvin is not a skater (%d)", n);
    n = nhl94_builds_lobby_query(img, 1, "potvin", rows, 800);
    CHECK(n == 2 && !strcmp(rows[0].label, "Felix Potvin"), "goalie seat finds Potvin (TOR, ASW) (%d)", n);
    n = nhl94_builds_lobby_query(img, 0, "wayne la", rows, 800);
    CHECK(n >= 1 && !strcmp(rows[0].label, "Wayne Gretzky"), "first match of 'wayne la' = %s", n ? rows[0].label : "-");
    CHECK(nhl94_builds_lobby_set(img, 0, rows[0].id) == 0, "set a skater pick");
    nhl94_builds_lobby_current(img, 0, lab, sizeof lab);
    CHECK(!strcmp(lab, "Wayne Gretzky (LA)"), "current = %s", lab);
    CHECK(nhl94_builds_lobby_set(img, 0, -1) == 0, "back to default");
    nhl94_builds_lobby_current(img, 0, lab, sizeof lab);
    CHECK(!strcmp(lab, "Team default"), "current after revert = %s", lab);
    int gid = (22 << 5) | 0;     /* TOR slot 0 */
    CHECK(nhl94_builds_lobby_set(img, 0, gid) != 0, "a goalie on a skater seat is refused (%s)", g_nhl_players[g_nhl_teams[22].first].name);
    CHECK(nhl94_builds_lobby_set(img, 1, (10 << 5) | 3) != 0, "a skater on the goalie seat is refused");
    nhl94_builds_lobby_set(img, 0, -1);
    nhl94_builds_lobby_set(img, 1, -1);

    printf("%s\n", fails ? "FAILED" : "all OK");
    return fails != 0;
}
