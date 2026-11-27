/* Bounded all-path proof for return-address consumers. */
#include "return_capture.h"

#include "m68k_decoder.h"

#define RETURN_CAPTURE_MAX_NODES 96

typedef enum {
    PROOF_VISITING,
    PROOF_TRUE,
    PROOF_FALSE,
} ProofState;

typedef struct {
    uint32_t pc;
    unsigned popped;
    ProofState state;
} ProofNode;

typedef struct {
    const GenesisRom *rom;
    ProofNode nodes[RETURN_CAPTURE_MAX_NODES];
    int node_count;
    bool allow_plain_exit;
    bool has_capturing_exit;
} ProofContext;

static bool ea_uses_sp(int ea)
{
    return ea >= (EA_An << 3) && ea <= ((EA_An_IDX << 3) | 7)
        && (ea & 7) == 7;
}

static unsigned return_pop_bytes(const M68KInstr *ins)
{
    if ((ins->size != M68K_SIZE_W && ins->size != M68K_SIZE_L)
            || ins->src_ea != ((EA_An_POST << 3) | 7)
            || ea_uses_sp(ins->dst_ea))
        return 0;
    unsigned width = ins->size == M68K_SIZE_L ? 4u : 2u;
    if (ins->mnemonic == MN_MOVE || ins->mnemonic == MN_MOVEA)
        return width;
    /* Loading A7 rebuilds the frame rather than merely consuming slots. */
    if (ins->mnemonic == MN_MOVEM && ((ins->words[0] >> 10) & 1) != 0
            && ins->words[1] != 0 && (ins->words[1] & 0x8000) == 0) {
        unsigned count = 0;
        for (unsigned bit = 0; bit < 16; bit++)
            count += (ins->words[1] >> bit) & 1u;
        return count * width;
    }
    return 0;
}

static bool invalid_stack_operation(const M68KInstr *ins)
{
    /* Apart from the first capture and final RTS, do not prove through SP
     * operands, frame changes or calls. This deliberately includes harmless
     * reads: a false result selects ordinary call handling, not missing code.
     * Some decoder operands are implicit, so check those separately. */
    if (ea_uses_sp(ins->src_ea) || ea_uses_sp(ins->dst_ea)
            || m68k_is_call(ins))
        return true;

    switch (ins->mnemonic) {
    case MN_PEA:
    case MN_LINK:
    case MN_UNLK:
    case MN_MOVEM:
    case MN_MOVE_USP:
    case MN_MOVEC:
        return true;
    case MN_LEA:
    case MN_ADDA:
    case MN_SUBA:
        return ins->reg == 7;
    case MN_EXG: {
        uint16_t opcode = ins->words[0];
        return ((opcode & 0xF1F8) == 0xC148
                    && (((opcode >> 9) & 7) == 7 || (opcode & 7) == 7))
            || ((opcode & 0xF1F8) == 0xC188 && (opcode & 7) == 7);
    }
    case MN_ADDX:
    case MN_SUBX:
    case MN_ABCD:
    case MN_SBCD:
        return ins->predec_mem_form
            && (ins->reg == 7 || (ins->src_ea & 7) == 7);
    case MN_CMPM:
        return ins->reg == 7 || (ins->src_ea & 7) == 7;
    default:
        return false;
    }
}

static bool prove_from(ProofContext *ctx, uint32_t pc, unsigned popped)
{
    for (int i = 0; i < ctx->node_count; i++) {
        if (ctx->nodes[i].pc == pc && ctx->nodes[i].popped == popped)
            return ctx->nodes[i].state == PROOF_TRUE;
    }

    /* Cycles (VISITING), unknown exits and budget exhaustion are unproven. */
    if (ctx->node_count >= RETURN_CAPTURE_MAX_NODES || (pc & 1) != 0
            || pc >= ctx->rom->rom_size || ctx->rom->rom_size - pc < 2)
        return false;

    int node_index = ctx->node_count++;
    ctx->nodes[node_index].pc = pc;
    ctx->nodes[node_index].popped = popped;
    ctx->nodes[node_index].state = PROOF_VISITING;

    M68KInstr ins;
    bool result = false;
    if (!m68k_decode(ctx->rom, pc, &ins) || ins.byte_length < 2
            || ins.byte_length > ctx->rom->rom_size - pc)
        goto done;

    uint32_t fallthrough = pc + ins.byte_length;
    unsigned pop_bytes = return_pop_bytes(&ins);
    if (pop_bytes && (ctx->allow_plain_exit
            || (!popped && pop_bytes == 4 && ins.size == M68K_SIZE_L))) {
        result = prove_from(ctx, fallthrough, popped + pop_bytes);
        goto done;
    }
    if (invalid_stack_operation(&ins))
        goto done;

    switch (ins.mnemonic) {
    case MN_RTS: {
        /* Local unwind may consume several complete slots, including slots
         * popped as words. Only a single longword permits an unconditional
         * call-site tail transfer; partial slots remain unproven. */
        bool whole_slots = popped >= 4 && (popped & 3u) == 0;
        ctx->has_capturing_exit |= whole_slots;
        result = ctx->allow_plain_exit ? (popped & 3u) == 0 : popped == 4;
        break;
    }
    case MN_Bcc:
    case MN_DBcc:
        if (ins.has_target)
            result = prove_from(ctx, ins.target_addr, popped)
                  && prove_from(ctx, fallthrough, popped);
        break;
    case MN_BRA:
    case MN_JMP:
        if (ins.has_target)
            result = prove_from(ctx, ins.target_addr, popped);
        break;
    case MN_OTHER:
    case MN_STOP:
    case MN_TRAP:
    case MN_TRAPV:
    case MN_CHK:
        break;
    default:
        if (!m68k_is_terminator(&ins))
            result = prove_from(ctx, fallthrough, popped);
        break;
    }

done:
    ctx->nodes[node_index].state = result ? PROOF_TRUE : PROOF_FALSE;
    return result;
}

/* Does `ins` write a longword into the return slot in place?  The slot is
 * (A7) at the callee's entry depth, or d16(A7) once it has pushed its own
 * frame — accept any (A7)/(d16,A7) longword destination, which is a
 * conservative "may". Pushes (-(A7)) and MOVEM are ordinary frame traffic. */
static bool writes_return_slot(const M68KInstr *ins)
{
    if (ins->size != M68K_SIZE_L)
        return false;
    int ea;
    switch (ins->mnemonic) {
    case MN_MOVE:
        ea = ins->dst_ea;
        break;
    case MN_ADDQ: case MN_SUBQ:
    case MN_ADDI: case MN_SUBI:
        ea = ins->src_ea;          /* destination EA lives in src_ea */
        break;
    case MN_ADD: case MN_SUB:
        if (!(ins->words[0] & 0x0100))   /* Dn,<ea> (memory-destination) form only */
            return false;
        ea = ins->src_ea;
        break;
    default:
        return false;
    }
    int mode = (ea >> 3) & 7, reg = ea & 7;
    return reg == 7 && (mode == 2 /* (A7) */ || mode == 5 /* d16(A7) */);
}

bool m68k_return_slot_rewrite_is_possible(const GenesisRom *rom, uint32_t start)
{
    if (!rom)
        return false;
    uint32_t visited[RETURN_CAPTURE_MAX_NODES];
    uint32_t work[RETURN_CAPTURE_MAX_NODES];
    int nvis = 0, nwork = 0;
    work[nwork++] = start;
    while (nwork > 0) {
        uint32_t pc = work[--nwork];
        if (pc + 1 >= rom->rom_size)
            continue;
        bool seen = false;
        for (int i = 0; i < nvis; i++)
            if (visited[i] == pc) { seen = true; break; }
        if (seen)
            continue;
        if (nvis >= RETURN_CAPTURE_MAX_NODES)
            return false;              /* too large to reason about: assume plain */
        visited[nvis++] = pc;

        M68KInstr ins;
        if (!m68k_decode(rom, pc, &ins))
            continue;
        if (writes_return_slot(&ins))
            return true;

        uint32_t fallthrough = pc + ins.byte_length;
        switch (ins.mnemonic) {
        case MN_Bcc:
        case MN_DBcc:
            if (ins.has_target && nwork < RETURN_CAPTURE_MAX_NODES)
                work[nwork++] = ins.target_addr;
            if (nwork < RETURN_CAPTURE_MAX_NODES)
                work[nwork++] = fallthrough;
            break;
        case MN_BRA:
        case MN_JMP:
            if (ins.has_target && nwork < RETURN_CAPTURE_MAX_NODES)
                work[nwork++] = ins.target_addr;
            break;
        default:
            /* BSR/JSR: do not descend; the callee's own rewrites belong to
             * ITS call sites. Fall through past the call. */
            if (!m68k_is_terminator(&ins) && nwork < RETURN_CAPTURE_MAX_NODES)
                work[nwork++] = fallthrough;
            break;
        }
    }
    return false;
}

/* The skip shape: addq.l/addi.l #N,(A7) with A7 at entry depth. */
static bool fixed_skip_amount(const M68KInstr *ins, uint32_t *n)
{
    if (ins->size != M68K_SIZE_L || ins->src_ea != ((EA_An_IND << 3) | 7))
        return false;
    if (ins->mnemonic == MN_ADDQ)
        *n = ins->imm32;
    else if (ins->mnemonic == MN_ADDI && ins->word_count >= 3)
        *n = ((uint32_t)ins->words[1] << 16) | ins->words[2];
    else
        return false;
    return true;
}

/* A plain read of the return slot that changes neither A7 nor the slot:
 * `move.l (sp),<ea>` / `move.l d16(sp),<ea>` (destination not SP-based). */
static bool reads_slot_only(const M68KInstr *ins)
{
    if (ins->mnemonic != MN_MOVE && ins->mnemonic != MN_MOVEA)
        return false;
    int mode = (ins->src_ea >> 3) & 7;
    return (ins->src_ea & 7) == 7
        && (mode == EA_An_IND || mode == EA_An_DISP)
        && !ea_uses_sp(ins->dst_ea);
}

/* Every path from `start` that reaches RTS must apply the same skip exactly
 * once, and nothing else may move A7 or touch the slot. Exploring
 * (pc, applied) states covers loops: re-reaching a state adds no new path,
 * and a second skip on any path fails. Calls are taken as balanced, as in
 * the rewrite analysis. Anything unproven (unknown exits, budget) is false,
 * which keeps the caller on ordinary/dynamic return handling. */
bool m68k_return_slot_fixed_skip(const GenesisRom *rom, uint32_t start, uint32_t *bytes)
{
    if (!rom)
        return false;
    uint32_t visited[RETURN_CAPTURE_MAX_NODES];
    uint32_t work[RETURN_CAPTURE_MAX_NODES];
    int nvis = 0, nwork = 0;
    uint32_t skip = 0;
    bool found = false;
    /* A state is pc << 1 | applied; ROM addresses stay below 2^31. */
    if (start >= 0x80000000u)
        return false;
    work[nwork++] = start << 1;
    while (nwork > 0) {
        uint32_t st = work[--nwork];
        uint32_t pc = st >> 1;
        bool applied = (st & 1u) != 0;
        bool seen = false;
        for (int i = 0; i < nvis; i++)
            if (visited[i] == st) { seen = true; break; }
        if (seen)
            continue;
        if (nvis >= RETURN_CAPTURE_MAX_NODES || (pc & 1u) != 0
                || pc >= rom->rom_size || rom->rom_size - pc < 2)
            return false;
        visited[nvis++] = st;

        M68KInstr ins;
        if (!m68k_decode(rom, pc, &ins) || ins.byte_length < 2
                || ins.byte_length > rom->rom_size - pc)
            return false;
        uint32_t fallthrough = pc + ins.byte_length;
        uint32_t n;
        uint32_t next[2];
        int nnext = 0;

        if (fixed_skip_amount(&ins, &n)) {
            if (applied || n == 0 || (n & 1u) != 0 || (found && n != skip))
                return false;
            skip = n;
            found = true;
            next[nnext++] = fallthrough;
            applied = true;
        } else if (m68k_is_call(&ins)) {
            if (ea_uses_sp(ins.src_ea))
                return false;
            next[nnext++] = fallthrough;
        } else if (ins.mnemonic == MN_RTS) {
            if (!applied)
                return false;          /* this path returns to ret, not ret+N */
            continue;
        } else if (reads_slot_only(&ins)) {
            next[nnext++] = fallthrough;
        } else if (invalid_stack_operation(&ins)) {
            return false;              /* A7 moved or the slot was touched */
        } else {
            switch (ins.mnemonic) {
            case MN_Bcc:
            case MN_DBcc:
                if (!ins.has_target)
                    return false;
                next[nnext++] = ins.target_addr;
                next[nnext++] = fallthrough;
                break;
            case MN_BRA:
            case MN_JMP:
                if (!ins.has_target)
                    return false;
                next[nnext++] = ins.target_addr;
                break;
            case MN_OTHER:
            case MN_STOP:
            case MN_TRAP:
            case MN_TRAPV:
            case MN_CHK:
                return false;
            default:
                if (m68k_is_terminator(&ins))
                    return false;      /* RTE/RTR/ILLEGAL: unknown exit */
                next[nnext++] = fallthrough;
                break;
            }
        }
        for (int i = 0; i < nnext; i++) {
            if (next[i] >= 0x80000000u || nwork >= RETURN_CAPTURE_MAX_NODES)
                return false;
            work[nwork++] = (next[i] << 1) | (applied ? 1u : 0u);
        }
    }
    if (!found)
        return false;
    *bytes = skip;
    return true;
}

bool m68k_return_capture_is_unconditional(const GenesisRom *rom, uint32_t start)
{
    if (!rom)
        return false;
    ProofContext ctx = {0};
    ctx.rom = rom;
    return prove_from(&ctx, start, 0);
}

bool m68k_return_capture_has_capturing_exit(const GenesisRom *rom, uint32_t start)
{
    if (!rom)
        return false;
    ProofContext ctx = {0};
    ctx.rom = rom;
    ctx.allow_plain_exit = true;
    return prove_from(&ctx, start, 0) && ctx.has_capturing_exit;
}
