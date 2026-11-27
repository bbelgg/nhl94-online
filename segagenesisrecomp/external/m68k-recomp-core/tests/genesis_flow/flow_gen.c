/* Emits C for the synthetic program in flow_rom.h with one GameConfig variant:
 *   plain    - no declarations, no code oracle
 *   declared - the length-word callee is an [[inline_args]] callee
 *   oracle   - a code oracle says the inline-word block is data
 *   upstream - dynamic_return_slots off: verifies (no run) that the emitted
 *              C is the upstream static-count call/return form
 * usage: flow_gen <variant> <out_dir> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "code_generator.h"
#include "flow_rom.h"

uint16_t rom_read16(const GenesisRom *rom, uint32_t addr)
{
    if (addr >= rom->rom_size || rom->rom_size - addr < 2) return 0;
    return (uint16_t)(((uint16_t)rom->rom_data[addr] << 8) | rom->rom_data[addr + 1]);
}
uint32_t rom_read32(const GenesisRom *rom, uint32_t addr)
{
    return ((uint32_t)rom_read16(rom, addr) << 16) | rom_read16(rom, addr + 2);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: flow_gen <variant> <out_dir>\n"); return 2; }
    static uint8_t bytes[FLOW_ROM_SIZE];
    if (flow_build_rom(bytes)) { fprintf(stderr, "ROM layout mismatch\n"); return 2; }
    GenesisRom rom = { bytes, FLOW_ROM_SIZE };

    static GameConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    bool upstream = !strcmp(argv[1], "upstream");
    cfg.dynamic_return_slots = !upstream;
    if (upstream) {
        /* switch off: nothing else configured */
    } else if (!strcmp(argv[1], "declared")) {
        cfg.inline_word_callees[cfg.inline_word_callee_count++] = R_INLINEW_CALLEE;
    } else if (!strcmp(argv[1], "oracle")) {
        /* every instruction start the real program has; the block's data
         * words are deliberately absent. */
        const uint32_t known[] = { R_INLINEW_CALLER, R_INLINEW_CONT, R_INLINEW_CONT + 2 };
        for (size_t i = 0; i < sizeof known / sizeof *known; i++)
            cfg.code_addrs[cfg.code_addr_count++] = known[i];
        for (size_t i = 0; i < sizeof flow_entries / sizeof *flow_entries; i++)
            cfg.code_addrs[cfg.code_addr_count++] = flow_entries[i];
        /* keep the list sorted */
        for (int i = 1; i < cfg.code_addr_count; i++)
            for (int j = i; j > 0 && cfg.code_addrs[j-1] > cfg.code_addrs[j]; j--) {
                uint32_t t = cfg.code_addrs[j]; cfg.code_addrs[j] = cfg.code_addrs[j-1];
                cfg.code_addrs[j-1] = t;
            }
    } else if (strcmp(argv[1], "plain")) {
        fprintf(stderr, "unknown variant %s\n", argv[1]);
        return 2;
    }

    /* plain: with neither a declaration nor an oracle the generator cannot
     * know the rewritten return address, so the continuation must exist as
     * a function entry for the run-time follow to dispatch to. */
    uint32_t addrs[32];
    size_t n = 0;
    for (size_t i = 0; i < sizeof flow_entries / sizeof *flow_entries; i++) {
        if (flow_entries[i] > R_INLINEW_CONT && !strcmp(argv[1], "plain") &&
            (n == 0 || addrs[n - 1] != R_INLINEW_CONT))
            addrs[n++] = R_INLINEW_CONT;
        addrs[n++] = flow_entries[i];
    }
    FunctionList list = {0};
    list.entries = calloc(n, sizeof *list.entries);
    list.capacity = (int)n;
    for (size_t i = 0; i < n; i++) {
        list.entries[i].addr = addrs[i];
        list.entries[i].evidence = FUNC_EVIDENCE_CONFIG;
    }
    list.count = (int)n;

    AnnotationTable at = {0};
    char dispatch[512];
    snprintf(dispatch, sizeof dispatch, "%s/flow_dispatch.c", argv[2]);
    bool ok = codegen_emit(&rom, &list, argv[2], "flow", dispatch, &at, &cfg, false);
    free(list.entries);
    if (ok && upstream) {
        /* Concatenate the emitted C and check for the upstream markers. */
        static char text[1 << 22];
        size_t len = 0;
        for (int i = -1; i < 32; i++) {
            char path[512];
            if (i < 0) snprintf(path, sizeof path, "%s/flow_dispatch.c", argv[2]);
            else snprintf(path, sizeof path, "%s/flow_part%02d.c", argv[2], i);
            FILE *fp = fopen(path, "rb");
            if (!fp) continue;
            len += fread(text + len, 1, sizeof text - 1 - len, fp);
            fclose(fp);
        }
        text[len] = 0;
        const char *must[] = { "/* RTE/skip propagation (pre-pop) */",
                               "/* normal JSR pop */",
                               "g_rte_pending = _tail_k;",
                               "g_rte_pending = _sp_popped / 4;" };
        const char *never[] = { "crash_report_record_block", "our return address: resume",
                                "guest-pushed continuation", "computed jump" };
        for (size_t i = 0; i < sizeof must / sizeof *must; i++)
            if (!strstr(text, must[i])) { fprintf(stderr, "FAIL upstream: missing '%s'\n", must[i]); ok = false; }
        for (size_t i = 0; i < sizeof never / sizeof *never; i++)
            if (strstr(text, never[i])) { fprintf(stderr, "FAIL upstream: found '%s'\n", never[i]); ok = false; }
        if (ok) printf("upstream form verified\n");
    }
    return ok ? 0 : 1;
}
