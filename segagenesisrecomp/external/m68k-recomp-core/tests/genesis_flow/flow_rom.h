/* Synthetic 68000 program shared by the generator and the runner.
 * Every routine sits at a fixed address; flow_build_rom() asserts that. */
#pragma once
#include <stdint.h>
#include <string.h>

#define FLOW_ROM_SIZE 0x1000u

/* normal call / return */
#define R_NORMAL_TOP   0x400u
#define R_NORMAL_CALLEE 0x440u
/* callee discards its caller's slot (addq.l #4,sp; rts) */
#define R_DISCARD_TOP  0x480u
#define R_DISCARD_MID  0x4C0u
#define R_DISCARD_CALLEE 0x500u
/* dispatcher exit: addq.w #4,sp; movem.l (sp)+,d0-d3/a0-a2; rts */
#define R_DISP_TOP     0x540u
#define R_DISP_DISP    0x580u
#define R_DISP_HANDLER 0x5C0u
/* inline arguments, fixed skip (addq.l #8,(sp); rts) */
#define R_INLINE8_CALLER 0x600u
#define R_INLINE8_CALLEE 0x640u
/* inline arguments, length word (rewrites the slot to ret + word) */
#define R_INLINEW_CALLER 0x680u
#define R_INLINEW_CALLEE 0x6C0u
/* indirect jsr (a0) whose callee consumes two slots */
#define R_IND_TOP      0x700u
#define R_IND_MID      0x740u
#define R_IND_CALLEE   0x780u
/* the instruction after the inline-word block */
#define R_INLINEW_CONT 0x68Eu

typedef struct { uint8_t *rom; uint32_t pc; } FlowAsm;

static inline void fa_org(FlowAsm *a, uint32_t addr) { a->pc = addr; }
static inline void fa_w(FlowAsm *a, uint16_t w)
{
    a->rom[a->pc] = (uint8_t)(w >> 8);
    a->rom[a->pc + 1] = (uint8_t)w;
    a->pc += 2;
}
static inline void fa_l(FlowAsm *a, uint32_t l) { fa_w(a, (uint16_t)(l >> 16)); fa_w(a, (uint16_t)l); }
static inline void fa_jsr(FlowAsm *a, uint32_t t) { fa_w(a, 0x4EB9); fa_l(a, t); }
static inline void fa_rts(FlowAsm *a) { fa_w(a, 0x4E75); }
static inline void fa_moveq(FlowAsm *a, int d, int v) { fa_w(a, (uint16_t)(0x7000 | (d << 9) | (v & 0xFF))); }

/* Returns 0 on success. */
static inline int flow_build_rom(uint8_t *rom)
{
    FlowAsm a = { rom, 0 };
    memset(rom, 0, FLOW_ROM_SIZE);

    fa_org(&a, R_NORMAL_TOP);
    fa_moveq(&a, 0, 1); fa_jsr(&a, R_NORMAL_CALLEE); fa_moveq(&a, 1, 2); fa_rts(&a);
    fa_org(&a, R_NORMAL_CALLEE);
    fa_moveq(&a, 2, 7); fa_rts(&a);

    /* Top: jsr Mid; moveq #5,d3; addq.l #1,d6; rts */
    fa_org(&a, R_DISCARD_TOP);
    fa_jsr(&a, R_DISCARD_MID); fa_moveq(&a, 3, 5); fa_w(&a, 0x5286); fa_rts(&a);
    fa_org(&a, R_DISCARD_MID);
    fa_jsr(&a, R_DISCARD_CALLEE); fa_moveq(&a, 1, 9); fa_rts(&a);
    fa_org(&a, R_DISCARD_CALLEE);
    fa_w(&a, 0x588F); fa_rts(&a);                      /* addq.l #4,sp */

    fa_org(&a, R_DISP_TOP);
    fa_jsr(&a, R_DISP_DISP); fa_moveq(&a, 3, 5); fa_w(&a, 0x5286); fa_rts(&a);
    fa_org(&a, R_DISP_DISP);
    fa_w(&a, 0x48E7); fa_w(&a, 0xF0E0);                /* movem.l d0-d3/a0-a2,-(sp) */
    fa_moveq(&a, 1, 99);
    fa_jsr(&a, R_DISP_HANDLER);
    fa_moveq(&a, 4, 77);                               /* never reached */
    fa_w(&a, 0x4CDF); fa_w(&a, 0x070F);                /* movem.l (sp)+,d0-d3/a0-a2 */
    fa_rts(&a);
    fa_org(&a, R_DISP_HANDLER);
    fa_w(&a, 0x584F);                                  /* addq.w #4,sp */
    fa_w(&a, 0x4CDF); fa_w(&a, 0x070F);                /* movem.l (sp)+,d0-d3/a0-a2 */
    fa_rts(&a);

    fa_org(&a, R_INLINE8_CALLER);
    fa_jsr(&a, R_INLINE8_CALLEE);
    fa_w(&a, 0x7063); fa_w(&a, 0x7063); fa_w(&a, 0x7063); fa_w(&a, 0x7063); /* 8 bytes of data */
    fa_moveq(&a, 1, 3); fa_rts(&a);
    fa_org(&a, R_INLINE8_CALLEE);
    fa_w(&a, 0x5097); fa_rts(&a);                      /* addq.l #8,(sp) */

    fa_org(&a, R_INLINEW_CALLER);
    fa_jsr(&a, R_INLINEW_CALLEE);
    fa_w(&a, 0x0008); fa_w(&a, 0x7063); fa_w(&a, 0x7063); fa_w(&a, 0x7063); /* length word (8) + data */
    if (a.pc != R_INLINEW_CONT) return 1;
    fa_moveq(&a, 1, 3); fa_rts(&a);
    fa_org(&a, R_INLINEW_CALLEE);
    fa_w(&a, 0x2257);                                  /* movea.l (sp),a1 */
    fa_w(&a, 0xD2D1);                                  /* adda.w (a1),a1 */
    fa_w(&a, 0x2E89);                                  /* move.l a1,(sp) */
    fa_rts(&a);

    fa_org(&a, R_IND_TOP);
    fa_jsr(&a, R_IND_MID); fa_moveq(&a, 3, 5); fa_w(&a, 0x5286); fa_rts(&a);
    fa_org(&a, R_IND_MID);
    fa_w(&a, 0x41F9); fa_l(&a, R_IND_CALLEE);          /* lea Callee,a0 */
    fa_w(&a, 0x4E90);                                  /* jsr (a0) */
    fa_moveq(&a, 1, 9); fa_rts(&a);                    /* skipped */
    fa_org(&a, R_IND_CALLEE);
    fa_w(&a, 0x588F); fa_rts(&a);                      /* addq.l #4,sp; rts */
    return 0;
}

static const uint32_t flow_entries[] = {
    R_NORMAL_TOP, R_NORMAL_CALLEE, R_DISCARD_TOP, R_DISCARD_MID, R_DISCARD_CALLEE,
    R_DISP_TOP, R_DISP_DISP, R_DISP_HANDLER, R_INLINE8_CALLER, R_INLINE8_CALLEE,
    R_INLINEW_CALLER, R_INLINEW_CALLEE, R_IND_TOP, R_IND_MID, R_IND_CALLEE,
};
