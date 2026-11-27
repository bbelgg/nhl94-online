/* Minimal stand-in for the consumer's generated-runtime ABI: only what the
 * flow test's generated code references. The semantics that matter to the
 * test (guest stack in a flat RAM array, push/pop of return slots, dispatch)
 * are real; video/audio/timing hooks are no-ops. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct M68KState {
    uint32_t D[8];
    uint32_t A[8];
    uint16_t SR;
    uint32_t PC;
    uint32_t USP;
} M68KState;

extern M68KState g_cpu;

#define SR_C (1u << 0)
#define SR_V (1u << 1)
#define SR_Z (1u << 2)
#define SR_N (1u << 3)
#define SR_X (1u << 4)

uint8_t  m68k_read8(uint32_t addr);
uint16_t m68k_read16(uint32_t addr);
uint32_t m68k_read32(uint32_t addr);
void m68k_write8(uint32_t addr, uint8_t v);
void m68k_write16(uint32_t addr, uint16_t v);
void m68k_write32(uint32_t addr, uint32_t v);

typedef void (*RecompFuncPtr)(void);
void recomp_tail_call(uint32_t addr);
void recomp_call_addr(uint32_t addr);
void recomp_call_func(RecompFuncPtr fn);
void crash_report_record_block(uint32_t block_addr);
void recomp_push_return(uint32_t ret_addr);
uint32_t recomp_resolve_ram_trampoline(uint32_t addr);
int recomp_dispatch_ram_stub(uint32_t addr);
void genesis_log_dispatch_miss(uint32_t addr);
void glue_check_vblank(void);

extern int *g_rte_pending_ptr;
#define g_rte_pending (*g_rte_pending_ptr)
extern uint64_t g_native_insn_count;
extern uint64_t g_cycle_accumulator;
extern uint64_t g_audio_cycle_counter;
extern uint64_t g_vblank_threshold;
extern uint64_t g_frame_count;
#define GEN_COSIM_TICK(cycles) ((void)(cycles))
