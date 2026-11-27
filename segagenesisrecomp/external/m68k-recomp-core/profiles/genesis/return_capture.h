/*
 * return_capture.h — conservative proof for 68000 return-address consumers.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rom_parser.h"

/*
 * True only when every reachable path from `start` consumes exactly one longword from
 * (A7)+ and reaches RTS without another stack operation or call. Both sides
 * of conditional branches are checked before and after the capture. Frame
 * reconstruction, unknown exits, cycles, malformed instructions and exhaustion
 * of the 96-node (PC, capture-state) budget conservatively return false.
 *
 * This covers both the straight-line Obj_WaitOffscreen idiom and helpers such
 * as Puyo Puyo's timer routine, whose conditional paths all consume the
 * caller's return address at different instructions. A false result means
 * unproven; the caller must retain ordinary/dynamic return handling.
 */
bool m68k_return_capture_is_unconditional(const GenesisRom *rom, uint32_t start);

/* Every path must be understood by the same bounded proof. Plain RTS exits
 * may coexist with exits consuming complete return slots through longword,
 * word or MOVEM pops. The path-local byte count selects the actual unwind;
 * this does not authorize unconditional call-site tail transfer. */
bool m68k_return_capture_has_capturing_exit(const GenesisRom *rom, uint32_t start);

/*
 * True when some path reachable from `start` (without descending into its
 * own callees) writes a longword INTO the caller's return slot in place —
 * `addq.l #N,(sp)`, `move.l a1,4(sp)`, `move.l (sp)+,(sp)` and friends —
 * so its eventual RTS resumes the caller somewhere other than the
 * instruction after the JSR. This is the inline-argument idiom (EA's
 * text printer stores its string right after the `jsr` and steps the
 * return address past it). A "may" answer: the generator adds a runtime
 * check at each call site and, with a code oracle, a static continuation.
 */
bool m68k_return_slot_rewrite_is_possible(const GenesisRom *rom, uint32_t start);

/*
 * True when EVERY path from `start` that reaches RTS adds the same constant N
 * to its caller's return address exactly once (`addq.l #N,(sp)` /
 * `addi.l #N,(sp)` at entry depth), i.e. it skips exactly N bytes of inline
 * arguments stored after every call to it. `*bytes` receives N. Reads of the
 * slot (`move.l (sp),<ea>`) and balanced calls are allowed; any other A7 or
 * slot operation, a path that returns without the skip, a second skip,
 * unknown exits and budget exhaustion return false. Codegen resumes the
 * caller at ret+N unconditionally on this answer, so it must be all-paths.
 */
bool m68k_return_slot_fixed_skip(const GenesisRom *rom, uint32_t start, uint32_t *bytes);
