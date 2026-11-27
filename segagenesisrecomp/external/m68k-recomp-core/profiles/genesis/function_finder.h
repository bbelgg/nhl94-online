/*
 * function_finder.h — 68K function boundary detection interface.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "rom_parser.h"
#include "game_config.h"

typedef struct {
    uint32_t addr;
    /* Bitset describing why this address is present.  Keeping provenance on
     * the entry (rather than only in log counters) lets the finder report how
     * much of an explicit TOML function list it can reproduce on its own.
     * Multiple bits are expected: an address may be both a configured root
     * and independently rediscovered by a direct call/table edge. */
    uint32_t evidence;
} FunctionEntry;

enum {
    FUNC_EVIDENCE_VECTOR = 1u << 0,
    FUNC_EVIDENCE_CONFIG = 1u << 1,
    FUNC_EVIDENCE_AUTO   = 1u << 2,
    FUNC_EVIDENCE_LATE   = 1u << 3,
};

typedef struct {
    FunctionEntry *entries;
    int            count;
    int            capacity;
} FunctionList;

void function_finder_run(const GenesisRom *rom, FunctionList *list,
                         const GameConfig *cfg, const char *diagnostics_dir);
void function_list_free(FunctionList *list);

/* True if the routine at `start` unconditionally pops its own return address
 * off the stack at entry (Obj_WaitOffscreen idiom). The code generator uses
 * this to emit `jsr`/`bsr` to such a routine as a non-returning tail transfer
 * instead of falling through to the instruction after the call. */
bool function_finder_pops_return_unconditionally(const GenesisRom *rom, uint32_t start);

/* TRAP #trap_num's handler, read from the ROM vector table ($80 + 4*n).
 * True only when the vector points at plausible ROM code (even, past the
 * $200-byte vector table + header, inside the ROM image). Discovery uses it
 * to register the handler as a function; the code generator uses it to call
 * that handler directly. Vectors into RAM or garbage return false. */
bool function_finder_trap_handler(const GenesisRom *rom, unsigned trap_num,
                                  uint32_t *handler_out);

/* True if the routine at `start` MAY rewrite its caller's return slot in
 * place (inline-argument idiom: `addq.l #8,(sp); rts`, `move.l a1,4(sp)`).
 * The code generator follows the rewritten address at run time and, given a
 * code oracle, resumes the caller's scan at the next instruction start. */
bool function_finder_may_rewrite_return_slot(const GenesisRom *rom, uint32_t start);

/* True if the routine at `start` always skips exactly `*bytes` of inline
 * arguments after its call (`addq.l #N,(sp); rts`). */
bool function_finder_fixed_return_skip(const GenesisRom *rom, uint32_t start, uint32_t *bytes);
