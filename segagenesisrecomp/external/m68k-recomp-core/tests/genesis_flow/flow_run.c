/* Executes the code generated from flow_rom.h against a tiny flat-memory
 * runtime and asserts PC flow (via side-effect registers), A7 and registers.
 * FLOW_VARIANT: 0 plain, 1 declared ([[inline_args]]), 2 code oracle. */
#include <stdio.h>
#include <string.h>

#include "flow_rom.h"
#include "game_spec.h"
#include "genesis_runtime.h"

#ifndef FLOW_VARIANT
#define FLOW_VARIANT 0
#endif

M68KState g_cpu;
static uint8_t s_rom[FLOW_ROM_SIZE];
static uint8_t s_ram[0x10000];
static int s_rte_pending;
int *g_rte_pending_ptr = &s_rte_pending;
uint64_t g_native_insn_count, g_cycle_accumulator, g_audio_cycle_counter;
uint64_t g_vblank_threshold = ~0ull, g_frame_count;
GameSpec g_game_spec;

static unsigned s_dispatch_misses;
static uint32_t s_last_miss;

static uint8_t *mem(uint32_t a, uint32_t n)
{
    a &= 0xFFFFFFu;
    if (a >= 0xFF0000u && a - 0xFF0000u + n <= sizeof s_ram) return &s_ram[a - 0xFF0000u];
    if (a + n <= FLOW_ROM_SIZE) return &s_rom[a];
    return NULL;
}
uint8_t m68k_read8(uint32_t a) { uint8_t *p = mem(a, 1); return p ? p[0] : 0; }
uint16_t m68k_read16(uint32_t a) { uint8_t *p = mem(a, 2); return p ? (uint16_t)(p[0] << 8 | p[1]) : 0; }
uint32_t m68k_read32(uint32_t a) { return (uint32_t)m68k_read16(a) << 16 | m68k_read16(a + 2); }
void m68k_write8(uint32_t a, uint8_t v) { uint8_t *p = mem(a, 1); if (p) p[0] = v; }
void m68k_write16(uint32_t a, uint16_t v) { uint8_t *p = mem(a, 2); if (p) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; } }
void m68k_write32(uint32_t a, uint32_t v) { m68k_write16(a, (uint16_t)(v >> 16)); m68k_write16(a + 2, (uint16_t)v); }

void crash_report_record_block(uint32_t a) { (void)a; }
void recomp_push_return(uint32_t ret) { g_cpu.A[7] -= 4; m68k_write32(g_cpu.A[7], ret); }
uint32_t recomp_resolve_ram_trampoline(uint32_t a) { return a; }
int recomp_dispatch_ram_stub(uint32_t a) { (void)a; return 0; }
void genesis_log_dispatch_miss(uint32_t a) { s_dispatch_misses++; s_last_miss = a; }
void glue_check_vblank(void) {}

static unsigned failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s: ", s_name); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#define STACK_TOP 0xFFFE00u
static const char *s_name;

static void begin(const char *name)
{
    s_name = name;
    memset(&g_cpu, 0, sizeof g_cpu);
    for (int i = 0; i < 8; i++) { g_cpu.D[i] = 0x1000u + (uint32_t)i; g_cpu.A[i] = 0x2000u + (uint32_t)i; }
    g_cpu.A[7] = STACK_TOP;
    s_rte_pending = 0;
    s_dispatch_misses = 0;
    memset(s_ram, 0, sizeof s_ram);
}
static void run(uint32_t entry) { recomp_call_addr(entry); }
static void common(void)
{
    CHECK(g_cpu.A[7] == STACK_TOP, "A7=$%X, expected $%X (entry's own slot is never popped)", g_cpu.A[7], STACK_TOP);
    CHECK(s_dispatch_misses == 0, "%u dispatch miss(es), last $%06X", s_dispatch_misses, s_last_miss);
    CHECK(s_rte_pending == 0, "g_rte_pending=%d leaked", s_rte_pending);
}
#define INIT(i) (0x1000u + (i))

int main(void)
{
    if (flow_build_rom(s_rom)) { fprintf(stderr, "ROM layout mismatch\n"); return 2; }

    begin("normal call/return");
    run(R_NORMAL_TOP);
    CHECK(g_cpu.D[0] == 1 && g_cpu.D[1] == 2 && g_cpu.D[2] == 7,
          "d0/d1/d2 = %X/%X/%X, expected 1/2/7 (caller resumes after the call)", g_cpu.D[0], g_cpu.D[1], g_cpu.D[2]);
    CHECK(g_cpu.D[3] == INIT(3), "d3 clobbered");
    common();

    begin("callee discards its caller's slot (addq.l #4,sp; rts)");
    run(R_DISCARD_TOP);
    CHECK(g_cpu.D[1] == INIT(1), "d1=%X: code after the discarded call ran", g_cpu.D[1]);
    CHECK(g_cpu.D[3] == 5, "d3=%X: the surviving outer frame did not resume", g_cpu.D[3]);
    CHECK(g_cpu.D[6] == INIT(6) + 1, "d6=%X: outer frame resumed %d times, expected once", g_cpu.D[6], (int)(g_cpu.D[6] - INIT(6)));
    common();

    begin("dispatcher exit pops slot + saved registers (addq.w #4,sp; movem.l (sp)+; rts)");
    run(R_DISP_TOP);
    CHECK(g_cpu.D[4] == INIT(4), "d4=%X: code after the handler call ran", g_cpu.D[4]);
    CHECK(g_cpu.D[0] == INIT(0) && g_cpu.D[1] == INIT(1) && g_cpu.D[2] == INIT(2),
          "d0-d2 not restored from the dispatcher's frame: %X %X %X", g_cpu.D[0], g_cpu.D[1], g_cpu.D[2]);
    CHECK(g_cpu.A[0] == 0x2000u && g_cpu.A[1] == 0x2001u && g_cpu.A[2] == 0x2002u,
          "a0-a2 not restored: %X %X %X", g_cpu.A[0], g_cpu.A[1], g_cpu.A[2]);
    CHECK(g_cpu.D[3] == 5, "d3=%X: top frame did not resume at its own return", g_cpu.D[3]);
    CHECK(g_cpu.D[6] == INIT(6) + 1, "d6=%X: top frame resumed %d times, expected once (an A7-delta unwind skips it)",
          g_cpu.D[6], (int)(g_cpu.D[6] - INIT(6)));
    common();

    begin("inline args, fixed skip (addq.l #8,(sp); rts), no declaration");
    run(R_INLINE8_CALLER);
    CHECK(g_cpu.D[0] == INIT(0), "d0=%X: the 8 inline data bytes were executed as code", g_cpu.D[0]);
    CHECK(g_cpu.D[1] == 3, "d1=%X: caller did not resume after the inline block", g_cpu.D[1]);
    common();

    begin(FLOW_VARIANT == 0 ? "inline args, length word, no declaration/oracle (continuation is an entry)"
          : FLOW_VARIANT == 1 ? "inline args, length word, [[inline_args]] declared"
          : "inline args, length word, code oracle marks the block as data");
    run(R_INLINEW_CALLER);
    CHECK(g_cpu.D[0] == INIT(0), "d0=%X: inline data executed as code", g_cpu.D[0]);
    CHECK(g_cpu.D[1] == 3, "d1=%X: caller did not resume after the inline block (variant %d)", g_cpu.D[1], FLOW_VARIANT);
    common();

    begin("indirect jsr (a0), callee consumes two slots");
    run(R_IND_TOP);
    CHECK(g_cpu.D[1] == INIT(1), "d1=%X: code after the indirect call ran", g_cpu.D[1]);
    CHECK(g_cpu.D[3] == 5, "d3=%X: outer frame did not resume", g_cpu.D[3]);
    CHECK(g_cpu.D[6] == INIT(6) + 1, "d6=%X: outer frame resumed %d times, expected once", g_cpu.D[6], (int)(g_cpu.D[6] - INIT(6)));
    CHECK(g_cpu.A[0] == R_IND_CALLEE, "a0=%X, expected the callee address", g_cpu.A[0]);
    common();

    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
