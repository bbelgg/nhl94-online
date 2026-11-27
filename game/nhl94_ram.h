/*
 * nhl94_ram.h - NHL Hockey '94 (Genesis, retail) work-RAM map for the
 * per-player view work. 68000 addresses (work RAM is $FF0000-$FFFFFF;
 * the game addresses it with absolute-short ($FFxxxx) forms).
 *
 * Source of names: NHL94Genesis/src/ram94.asm (rs list from varstart
 * $FFFFB000; addresses computed from the rs order and checked against
 * RAM dumps of the running game). See docs/VIEW_RESEARCH.md for how each
 * item was verified.
 *
 * Tags in comments:
 *   [V]  VERIFIED: observed in a run of the stable build (RAM dump /
 *        screenshot), see VIEW_RESEARCH.md.
 *   [S]  FROM SOURCE: read from the disassembly, not observed.
 *
 * BYTE/BIT CONVENTION: all multi-byte values are big-endian. The asm tests
 * flag bits with btst/bset on the BYTE at the label, i.e. the HIGH byte of
 * a flag word. "gmdir equ 1" on gmode therefore is word mask 0x0200, not
 * 0x0002. The *_W masks below are masks on the 16-bit word.
 */
#ifndef NHL94_RAM_H
#define NHL94_RAM_H

/* ------------------------------------------------------------------ */
/* Sort objects (players, nets, puck, puck shadow)                     */
/* ------------------------------------------------------------------ */
#define NHL94_SORTCORDS          0xFFB04Au  /* SortCords [V] */
#define NHL94_SC_STRIDE          0x80u      /* SCstruct = 1<<scsize [V] */
#define NHL94_SC_COUNT           16u        /* Sortobjs [V] */
#define NHL94_SC_ADDR(n)         (NHL94_SORTCORDS + (uint32_t)(n) * NHL94_SC_STRIDE)

/* object indices (SCnum, also stored at +0x52) */
#define NHL94_SC_HOME_FIRST      0u   /* 0..5  home team (pfteam=0) [V] */
#define NHL94_SC_AWAY_FIRST      6u   /* 6..11 away team (pfteam=1) [V] */
#define NHL94_SC_NET_TOP         12u  /* net, Ypos +268 at setup [V] */
#define NHL94_SC_NET_BOTTOM      13u  /* net, Ypos -268 at setup [V] */
#define NHL94_SC_PUCK            14u  /* puckscnum [V] */
#define NHL94_SC_PUCK_SHADOW     15u  /* puck shadow [V] */
/* NOTE: object slot != on-ice position. Positions are reassigned to other
 * slots by line changes / penalties (observed). Always scan +position. [V] */

/* field offsets inside an object (ram94.asm "sort object structure") */
#define NHL94_OFF_XPOS       0x00 /* s16 x, rink pixels, 0 = centre; +2 = fraction (16.16) [V] */
#define NHL94_OFF_ATTRIBUTE  0x04 /* u16 VDP attr bits: 0x0800 hflip (right-handed skater),
                                     0x1000 vflip, bit0 = away colours [V] */
#define NHL94_OFF_FRAME      0x06 /* u16 sprite frame (bits 0-10 frame no., bits 11-15 flip/pal xor) [V] */
#define NHL94_OFF_OLDFRAME   0x08 /* u16 RENDER STATE: last frame whose tiles were DMA'd [V] */
#define NHL94_OFF_VROFFS     0x0A /* u8[8] RENDER STATE: per-piece tile offsets [S] */
#define NHL94_OFF_VRCHAR     0x12 /* u16 RENDER STATE: fixed VRAM tile base of this object [S] */
#define NHL94_OFF_YPOS       0x14 /* s16 y, rink pixels, +y = up the screen; +0x16 fraction [V] */
#define NHL94_OFF_ZPOS       0x18 /* s16 height above ice (screen y += 1.5*z); <0 = hidden [V/S] */
#define NHL94_OFF_OLDXPOS    0x1C /* s32 x before this frame's move [S] */
#define NHL94_OFF_OLDYPOS    0x20 /* s32 [S] */
#define NHL94_OFF_OLDZPOS    0x24 /* s32 [S] */
#define NHL94_OFF_XVEL       0x28 /* s16; pos(16.16) += vel*17*d7 per frame (NTSC, 22 on PAL) [V/S] */
#define NHL94_OFF_YVEL       0x2A /* s16 [V] */
#define NHL94_OFF_ZVEL       0x2C /* s16 [S] */
#define NHL94_OFF_POSITION   0x34 /* s16 0=G 1=LD 2=RD 3=LW 4=C 5=RW 6=extra attacker;
                                     negative ($FFxx) = off ice / penalty box [V] */
#define NHL94_OFF_ASSNUM     0x36 /* u16 AI assignment index [S] */
#define NHL94_OFF_ASSLIST    0x38 /* u8[8] AI assignment queue (asstab numbers) [S] */
#define NHL94_OFF_TEMP1      0x40 /* u16 AI scratch [S] */
#define NHL94_OFF_SCNUM      0x52 /* u16 object index [V] */
#define NHL94_OFF_FACEDIR    0x54 /* u16 0..7: 0 up, 1 up-right, 2 right, 3 down-right,
                                     4 down, 5 down-left, 6 left, 7 up-left (clockwise, +y up) [V] */
#define NHL94_OFF_SPA        0x58 /* u16 animation (offset into SPAlist) [V] */
#define NHL94_OFF_SPANUM     0x5A /* u16 index into animation [S] */
#define NHL94_OFF_SPACNT     0x5C /* s16 frames to next anim frame [S] */
#define NHL94_OFF_NEWPOS     0x60 /* u8 requested position ($FF none); on the puck object
                                     the word at +0x60 is puckc [V] */
#define NHL94_OFF_NEWPNUM    0x61 /* u8 requested roster index ($FF none) [S] */
#define NHL94_OFF_PFLAGS     0x62 /* u8 pflags, see NHL94_PF_* [V] */
#define NHL94_OFF_PFLAGS2    0x63 /* u8 pflags2, see NHL94_PF2_* [V/S] */
#define NHL94_OFF_PFLAGS3    0x64 /* u8 pflags3 [S] */
#define NHL94_OFF_PNUM       0x66 /* u8 roster index of the player in this slot ($FF none) [V] */
#define NHL94_OFF_ROSTNUM    0x6F /* u8 jersey number, BCD [V] */
#define NHL94_OFF_HANDED     0x76 /* u8 bit0 set = left handed [S] */

/* pflags (+0x62) bits (byte bits) */
#define NHL94_PF_DOFF     0x01 /* pfdoff  deceleration off [S] */
#define NHL94_PF_NA       0x02 /* pfna    new assignment [S] */
#define NHL94_PF_NC       0x04 /* pfnc    no collision [S] */
#define NHL94_PF_JOYCON   0x08 /* pfjoycon human (pad) controlled [V] */
#define NHL94_PF_REV      0x10 /* pfrev   skating backwards [S] */
#define NHL94_PF_ALOCK    0x20 /* pfalock locked until animation done [S] */
#define NHL94_PF_TEAM     0x40 /* pfteam  0 home / 1 away [V] */
#define NHL94_PF_GOAL     0x80 /* pfgoal  goal shot at: 0 bottom (-y), 1 top (+y) [V] */
/* pflags2 (+0x63) */
#define NHL94_PF2_AIP     0x02 /* pf2aip animation in progress [S] */
#define NHL94_PF2_UNAV    0x04 /* pf2unav going to/from bench or box [V] */
#define NHL94_PF2_LCM     0x08 /* pf2lcm  line change menu open [S] */
#define NHL94_PF2_PEN     0x10 /* pf2pen  caused a penalty [S] */
#define NHL94_PF2_NPC     0x20 /* pf2npc  no player collision [S] */
#define NHL94_PF2_INJ     0x40 /* pf2inj  injured [S] */
/* pflags3 (+0x64) */
#define NHL94_PF3_GCTRL   0x04 /* pf3gctrl human goalie handed to AI (off-camera rule) [S] */

#define NHL94_POS_G   0
#define NHL94_POS_LD  1
#define NHL94_POS_RD  2
#define NHL94_POS_LW  3
#define NHL94_POS_C   4
#define NHL94_POS_RW  5
#define NHL94_POS_X   6   /* extra attacker (goalie pulled) [S] */

/* Puck (object 14) convenience addresses [V] */
#define NHL94_PUCKX       0xFFB74Au  /* puckx */
#define NHL94_PUCKY       0xFFB75Eu  /* pucky */
#define NHL94_PUCKZ       0xFFB762u  /* puckz */
#define NHL94_PUCKVX      0xFFB772u
#define NHL94_PUCKVY      0xFFB774u
#define NHL94_PUCKVZ      0xFFB776u
#define NHL94_PUCKC       0xFFB7AAu  /* s16 SCnum of puck carrier, negative = loose [V] */
/* passdir (nhl94.xml:1903): the pass direction 0..7 (0 +y, 1 +x+y, 2 +x, 3 +x-y, 4 -y, ...). setpassmode sets it to
 * facedir & 7; passmode sets d0 & 7 while a d-pad direction is held with B down -- after the lock's doinput remap, so it
 * is in rink (simulation) coordinates. Read by dopass. [V] */
#define NHL94_PASSDIR     0xFFBEDCu

/* Draw order: OOlist = 16 bytes of (SCnum*2), ascending Ypos; first entry
 * gets highest VDP sprite priority [V]. Ylist/OOlistpos are its helpers. */
#define NHL94_YLIST       0xFFB84Au  /* s16[16] [S] */
#define NHL94_OOLISTPOS   0xFFB86Au  /* u16[16] [S] */
#define NHL94_OOLIST      0xFFB88Au  /* u8[16] [V] */

/* ------------------------------------------------------------------ */
/* Camera / scroll                                                     */
/* ------------------------------------------------------------------ */
#define NHL94_VPOS        0xFFBD18u  /* s16 camera centre y (rink px), clamp -200..256 [V] */
#define NHL94_OLDROW      0xFFBD1Au  /* s16 Vpos>>3 last streamed; $7D0 forces full redraw [V/S] */
#define NHL94_HPOS        0xFFBD1Cu  /* s16 camera centre x, clamp -60..60 [V] */
#define NHL94_HSCROLL     0xFFBD1Eu  /* s16 = -64 - Hpos (written to VRAM hscroll) [V] */
#define NHL94_VSCROLL     0xFFBD20u  /* s16 = -Vpos (written to VSRAM) [V] */
#define NHL94_CAMERAPOS   0xFFBE08u  /* replay camera struct (11 words + camtrack at +0x16) [S] */
#define NHL94_XC1         0xFFBF8Eu  /* s16 camera target x / scroll-lock x [V] */
#define NHL94_YC1         0xFFBF90u  /* s16 camera target y / scroll-lock y [V] */
#define NHL94_YLEADER     0xFFBF92u  /* s16 look-ahead toward carrier's attack goal, +-50 [V] */

/* ------------------------------------------------------------------ */
/* Game mode / flags (flag words: masks are on the 16-bit word)        */
/* ------------------------------------------------------------------ */
#define NHL94_GMODE       0xFFC2EAu
#define NHL94_GMODE_CLOCK_W   0x0100 /* gmclock clock stopped [V] */
#define NHL94_GMODE_DIR_W     0x0200 /* gmdir 0 = home attacks up (+y), 1 = home attacks down [V] */
#define NHL94_GMODE_HL_W      0x1000 /* gmhl highlight mode: intermission highlights of OTHER games
                                        are running in SortCords - not the real game [V] */
#define NHL94_SFLAGS      0xFFC2ECu
#define NHL94_SFLAGS_PAUSE_W  0x0100 /* sfpz [S] */
#define NHL94_SFLAGS_SLOCK_W  0x4000 /* sfslock camera locked to xc1/yc1 [S] */
#define NHL94_SFLAGS_HOR_W    0x8000 /* sfhor horizontal (scoreboard) rink mode [V] */
#define NHL94_SFLAGS2     0xFFC2EEu
#define NHL94_SFLAGS2_FACEOFF_W 0x0100 /* sf2faceoff [S] */
#define NHL94_SFLAGS2_REPLAY_W  0x0800 /* sf2replay [V] */
#define NHL94_SFLAGS3     0xFFC2F0u
#define NHL94_SFLAGS4     0xFFC2F4u
#define NHL94_SFLAGS4_REVANGLE_W 0x1000 /* bit 4: reverse-angle (180 deg) rendering [V] */
#define NHL94_GMODE2      0xFFC2FAu  /* bit0 (0x0100) shootout [S] */

#define NHL94_DISFLAGS    0xFFBF78u  /* 0x0100 dfok: sprite list ready for vblank DMA [S] */
#define NHL94_VBINT       0xFFB03Au  /* u32 vblank handler; 0x00015D9A in game [V] */
#define NHL94_VCOUNT      0xFFB03Eu  /* u16 vblank counter [V] */
#define NHL94_OLDVCOUNT   0xFFB040u  /* u16 Vcount at last game frame (d7 = Vcount-OldVcount) [V] */
#define NHL94_RECBPR      0xFFB036u  /* u32 replay record pointer (replay buffer $FF0000..) [V] */
#define NHL94_SEED        0xFFD066u  /* u32 RNG seed (randomd0) [V] */

#define NHL94_GSP         0xFFC466u  /* u16 period 0..3 (3 = OT), 4 = game over [V] */
#define NHL94_GAMECLOCK   0xFFC468u  /* u16 seconds left (+2 = jiffy fraction) [V] */

/* ------------------------------------------------------------------ */
/* Controllers                                                         */
/* ------------------------------------------------------------------ */
#define NHL94_PADCONTROLBITS   0xFFBE78u /* nibbles: [3:0] puck carrier, [7:4] pad1 player,
                                            [11:8] pad2 player, [15:12] replay; $F none [V] */
#define NHL94_PADCONTROLBITS34 0xFFBE86u /* same for pads 3/4 [S] */
#define NHL94_C1PLAYERNUM 0xFFC320u  /* s16 SCnum controlled by pad 1, negative none [V] */
#define NHL94_C2PLAYERNUM 0xFFC322u  /* [V] */
#define NHL94_C3PLAYERNUM 0xFFC324u  /* [S] */
#define NHL94_C4PLAYERNUM 0xFFC326u  /* [S] */
#define NHL94_CONT1TEAM   0xFFC328u  /* u16 0 none, 1 home, 2 away [V] */
#define NHL94_CONT2TEAM   0xFFC32Au  /* [V] */
#define NHL94_CONT3TEAM   0xFFC32Cu  /* [S] */
#define NHL94_CONT4TEAM   0xFFC32Eu  /* [S] */
#define NHL94_HOMETEAM    0xFFC330u  /* team number [V] */
#define NHL94_VISTEAM     0xFFC332u  /* [V] */

/* Pad input (ram94.asm rs list; see docs/PLAYER_LOCK.md) [S] */
#define NHL94_PADRAW1     0xFFBEF6u  /* u8 x4 PadRaw1..4: last pad byte per pad, ACTIVE LOW
                                        (bit 7 S, 6 A, 5 C, 4 B, 3 R, 2 L, 1 D, 0 U), written by
                                        ReadJoyData in vblank, read by ReadJoy1..4 [S] */
#define NHL94_LJ1         0xFFBEEEu  /* u16 x4 lj1..lj4: last frame's buttons (edge detection) [S] */
#define NHL94_BHOLDTIMER  0xFFBF06u  /* u8 x2 B-hold timers pad 1 / pad 2 (bholdtimer34 +2) [S] */
#define NHL94_JOYPUCKCARRIER 0xFFC318u /* s16 pad index of the doinput in progress [S] */
#define NHL94_BA_PS_FLAGS 0xFFC2F2u  /* bit 2 (word 0x0400): penalty shot / shootout [S] */
#define NHL94_BA_PS_ACTIVE_W 0x0400
#define NHL94_FOURWAYPLAY 0xFFD046u  /* u16 1 = EA 4 Way Play adaptor found [S] */
/* pflags3 bit 3 (word-free byte mask 0x08): restorepl keeps pfna clear when set [S] */
#define NHL94_PF3_KEEPNA  0x08

/* Options (front end) */
#define NHL94_OPTPLAYMODE 0xFFD048u
#define NHL94_OPTNOP      0xFFD04Au  /* Players: 0 = "Demo" (all CPU), 1 One-Home, ... [V] */
#define NHL94_OPTPERLEN   0xFFD050u  /* 0 5min, 1 10min, 2 20min, 3 30s [S] */
#define NHL94_OPTGOALIE   0xFFD052u
#define NHL94_OPTPEN      0xFFD056u
#define NHL94_OPTLINE     0xFFD058u  /* 0 = line changes on [V] */
#define NHL94_GOALIEMODE1 0xFFD05Au
#define NHL94_GOALIEMODE2 0xFFD05Cu
#define NHL94_DEMOFLAG    0xFFD064u

/* ------------------------------------------------------------------ */
/* Team structs                                                        */
/* ------------------------------------------------------------------ */
#define NHL94_TMSTRUCT    0xFFC6CEu  /* home team struct [V] */
#define NHL94_AWTMSTRUCT  0xFFCA32u  /* away team struct (tmsize 0x364 later) [V] */
#define NHL94_TM_SCORE    0x0C       /* u16 [V] */
#define NHL94_TM_LINE     0x16       /* u16 current line [S] */
#define NHL94_TM_SORT     0x22       /* u16 low word of first SortCords object [V] */
#define NHL94_TM_AP       0x24       /* u16 active players on ice 4..6 [S] */
#define NHL94_TM_GOALIE   0x26       /* s16 goalie index, negative = pulled [S] */
#define NHL94_TM_PDST     0x66       /* s16[26] -2 bench, -1 ice, >=0 penalty box [S] */
#define NHL94_PBNUM       0xFFC3EAu  /* u8 players in box: high nibble home, low away [V] */

/* ------------------------------------------------------------------ */
/* Render-only buffers                                                 */
/* ------------------------------------------------------------------ */
#define NHL94_DMALIST     0xFFB8B4u  /* {u32 src, u16 words, u16 vram} entries [S] */
#define NHL94_DMALISTEND  0xFFBD14u  /* u32 [S] */
#define NHL94_SATT        0xFFC018u  /* VDP sprite table copy, 8 bytes/sprite [V] */
#define NHL94_SATTSIZE    0xFFC2E8u  /* u16 words [V] */

/* Rink geometry (rink pixels) */
#define NHL94_NET_Y           268  /* nets at (0,+-268) [V] */
#define NHL94_GOALIE_HOME_Y   247  /* goalie crease, observed +-245..249 [V] */
#define NHL94_BLUELINE_Y      0x58 /* 88  Blueline [S] */
#define NHL94_GOALLINE_Y      264  /* Blueline+goalline (88+176) [S] */
#define NHL94_BOARDS_Y        0x128/* 296 end boards (SetShotMode) [S] */
#define NHL94_SIDELINE_X      0x88 /* 136 Sideline [S] */
#define NHL94_HPOS_MIN  (-60)
#define NHL94_HPOS_MAX    60
#define NHL94_VPOS_MIN  (-200)
#define NHL94_VPOS_MAX   256

/* ROM tables / routines (retail ROM addresses) */
#define NHL94_ROM_REVFRAMETBL 0x0F600Eu /* u16[] frame -> 180-degree frame (RestoreReplayFrame) [V] */
#define NHL94_ROM_GAMELOOP    0x007938u
#define NHL94_ROM_DOGAMEFRAME 0x00794Eu
#define NHL94_ROM_UPDATEPLAYERS 0x00A9D6u
#define NHL94_ROM_CHECKWINDOW 0x00AFCAu /* camera update */
#define NHL94_ROM_UPDATEREPLAY 0x00A8CAu
#define NHL94_ROM_SETVIDEO    0x015EC0u
#define NHL94_ROM_UPDATESCROLL 0x015F34u
#define NHL94_ROM_SETSORTCORDS 0x0165FCu
#define NHL94_ROM_ADDFRAME2   0x0167AAu
#define NHL94_ROM_FIND3D      0x016920u
#define NHL94_ROM_VBLANK      0x015D9Au /* VBlank (ai_GameVBlankRoutine) */
/* Player lock hook sites (game.toml, nhl94_seats.c). Found by byte pattern in
 * the retail ROM and matched to annotations_from_disasm.csv names. */
#define NHL94_ROM_READJOYDATA 0x01A370u /* sound94.asm ReadJoyData (vblank pad read) */
#define NHL94_ROM_DOINPUT     0x00B0E8u /* logic94.asm doinput (d0-d3 = pad, a3 = player) */
#define NHL94_ROM_SETC1PLAYER 0x00C0BCu /* logic94.asm setc1player (d0 = new pad-1 player) */
#define NHL94_ROM_SETC2PLAYER 0x00C0DAu /* logic94.asm setc2player */
#define NHL94_ROM_RESTOREPL   0x00C0F8u /* logic94.asm restorepl */
#define NHL94_ROM_ASSGOALIECTRL 0x00D3C6u /* logic94.asm assgoaliectrl (human goalie) */
#define NHL94_ROM_GOALIECTRL_CONT2 0x00D462u /* assgoaliectrl .cont2: after the camera rule */
/* Call for pass (nhl94_seats.c) */
#define NHL94_ROM_DOPASS          0x00BC64u /* logic94.asm dopass (a3 = passer) */
#define NHL94_ROM_DOPASS_PICKED   0x00BD5Eu /* dopass: tst.l d4 after the receiver loop (a0 = receiver) */
#define NHL94_ROM_CHK4PASS        0x00E444u /* logic94.asm chk4pass (AI carrier looks for a pass) */
#define NHL94_ROM_CHK4PASS_GATE   0x00E456u /* chk4pass: cmp.w #$C,d0 (random "consider a pass" roll) */
#define NHL94_ROM_CHK4PASS_PICK   0x00E464u /* chk4pass: cmpi.w #6,SCnum(a3) after the random teammate roll */
#define NHL94_ROM_CHK4PASS_BLUE   0x00E4F8u /* chk4pass: eor.w d0,d1 (pass across the blue line = offside risk) */
#define NHL94_ROM_CHK4PASS_BACK   0x00E502u /* chk4pass .oko: cmp.w #$58,d0 (no backward pass outside the zone) */

/* Player-lock request block in UNUSED work RAM: the bottom of the stack area
 * PALflag+2 ($FFDEF4) .. Stack ($FFFFFE). Begin's RAM clear stops at PALflag
 * and the deepest stack seen in play is $FFFEC9 (measured), so this block is
 * never touched by the game. In guest RAM so every snapshot / digest covers it. */
#define NHL94_SEATREQ        0xFFDF00u
#define NHL94_SEATREQ_TARGET (NHL94_SEATREQ + 0x0) /* u16 receiver SC + 1, 0 = no request */
#define NHL94_SEATREQ_CARRIER (NHL94_SEATREQ + 0x2) /* u16 AI carrier SC the request is for */
#define NHL94_SEATREQ_SEAT   (NHL94_SEATREQ + 0x4) /* u16 requesting seat */
#define NHL94_SEATREQ_LEFT   (NHL94_SEATREQ + 0x6) /* s16 game frames left in the window */
#define NHL94_SEATREQ_PREV   (NHL94_SEATREQ + 0x8) /* u16[12] previous call buttons per seat (edges),
                                                       $FFDF08..$FFDF1F */

/* Virtual pads for seats 5..12 (seat index 4..11), same unused block. The
 * game has four pad structures; a seat k >= 4 keeps its own copy of what
 * the game swaps for pads 3/4 (updateplayers .13 / pad 4 block) and is
 * swapped into pad 3's slots (home seat) or pad 4's (away seat) for the
 * pad block of ITS object only -- the game's own swap then carries it into
 * pad 1 / pad 2 around doinput. 16 bytes per seat, $FFDF20..$FFDF9F. */
#define NHL94_VPAD           0xFFDF20u
#define NHL94_VPAD_FIRST     4                     /* seat index of the first virtual pad */
#define NHL94_VPAD_ADDR(k)   (NHL94_VPAD + 0x10u * (uint32_t)((k) - NHL94_VPAD_FIRST))
#define NHL94_VPAD_TEAM      0x0  /* u16 0 none, 1 home, 2 away (as contNteam) */
#define NHL94_VPAD_PLAYER    0x2  /* s16 SCnum of the seat's player, -1 none (as cNplayernum) */
#define NHL94_VPAD_BHOLD     0x4  /* u16 B-hold timer word (as bholdtimer34) */
#define NHL94_VPAD_AHOLD     0x6  /* u16 A-hold timer word (as aholdtimer34) */
#define NHL94_VPAD_PADWORD   0x8  /* u16 pad word (as padword3/4) */
#define NHL94_VPAD_PCB       0xA  /* u16 PadControlBits word (as PadControlBits34) */
#define NHL94_VPAD_LJ        0xC  /* u16 last buttons, ReadJoy edge detection (as lj3/4) */
/* Swap state while a virtual pad sits in pad 3/4's slots ($FFDFA0..$FFDFB3):
 * set at updateplayers .t1, undone at .t1cont, both in the same object's
 * pass. The real pad 3/4 values are parked here meanwhile. */
#define NHL94_VSWAP          0xFFDFA0u
#define NHL94_VSWAP_SEAT     (NHL94_VSWAP + 0x00) /* u16 seat index + 1, 0 = no swap */
#define NHL94_VSWAP_SLOT     (NHL94_VSWAP + 0x02) /* u16 0 = pad 3 slots, 1 = pad 4 slots */
#define NHL94_VSWAP_TEAM     (NHL94_VSWAP + 0x04) /* u16 saved cont3/4team */
#define NHL94_VSWAP_PLAYER   (NHL94_VSWAP + 0x06) /* u16 saved c3/4playernum */
#define NHL94_VSWAP_BHOLD    (NHL94_VSWAP + 0x08) /* u16 saved bholdtimer34 */
#define NHL94_VSWAP_AHOLD    (NHL94_VSWAP + 0x0A) /* u16 saved aholdtimer34 */
#define NHL94_VSWAP_PADWORD  (NHL94_VSWAP + 0x0C) /* u16 saved padword3/4 */
#define NHL94_VSWAP_PCB      (NHL94_VSWAP + 0x0E) /* u16 saved PadControlBits34 */
#define NHL94_VSWAP_LJ       (NHL94_VSWAP + 0x10) /* u16 saved lj3/4 */
#define NHL94_VSWAP_RAW      (NHL94_VSWAP + 0x12) /* u8 saved PadRaw3/4 (a V-blank inside the swap
                                                     updates this copy instead) */

/* Pad 3/4 structures the virtual pads swap with (ram94.asm; addresses read
 * from the retail ROM's pad-3/4 swap code in updateplayers $AD42..$AE7C) */
#define NHL94_LJ3            0xFFBEF2u  /* lj4 = +2 */
#define NHL94_BHOLDTIMER34   0xFFBF08u
#define NHL94_PADWORD3       0xFFBF0Au  /* padword4 = +2 */
#define NHL94_AHOLDTIMER34   0xFFD420u
#define NHL94_ROM_UPD_T1     0x00AD42u  /* updateplayers .t1: cmp.w #$B,d6 (pads 3/4 next; d6 = SCnum) */
#define NHL94_ROM_UPD_T1CONT 0x00AE7Cu  /* updateplayers .t1cont: pads done */

/* Drop-in / drop-out block (docs/DROPIN.md): $FFE300..$FFE33F is this feature's
 * own guest RAM (never written by the game: the stack bottom area). */
#define NHL94_DROP_BLOCK     0xFFE300u
#define NHL94_DROP_PAUSE_CTR (NHL94_DROP_BLOCK + 0x00) /* u16 frames the game has sat in the pause menu
                                                           whose pad belongs to a seat that left */
#define NHL94_DROP_JOIN_PEND (NHL94_DROP_BLOCK + 0x02) /* u16 bit k = seat k became a human while play was
                                                           live and waits for the next whistle to take its player */
/* Penalty-shot / shootout attempt latch (nhl94_seats.c ps_targets): the seats
 * chosen for the attempt are kept until it ends, so a seat leaving cannot
 * re-deal the shooter. $FFE304..$FFE30F is reserved for it. */
#define NHL94_PS_LATCH_SHOOTER (NHL94_DROP_BLOCK + 0x04) /* u8 shooter seat + 1, 0 none */
#define NHL94_PS_LATCH_GOALIE  (NHL94_DROP_BLOCK + 0x05) /* u8 defending goalie's seat + 1, 0 none */
#define NHL94_PS_LATCH_KEY     (NHL94_DROP_BLOCK + 0x06) /* u16 attempt identity (team, shooter object, shootout
                                                             round) with bit 0 set; 0 = nothing latched */
#define NHL94_PAUSEPAD       0xFFC316u  /* u16 pausepad: 0 = pad 1 or 2 (sfpj), 3 / 4 = pad 3 / 4 [S] */
#define NHL94_SFLAGS_PAUSEJ_W 0x0200    /* sfpj (sflags bit 1): the pausing controller is pad 2 [S] */
#define NHL94_DROP_PAUSE_FRAMES 300     /* orphaned pause menu: resume after this many game frames */

/* ------------------------------------------------------------------ */
/* 4v4 format (nhl94_format.c, docs/FOUR_ON_FOUR_PLAN.md)              */
/* ------------------------------------------------------------------ */
#define NHL94_TM_FLAGS       0x30       /* u16 tmflags (byte bit 0 tmflcc) [S] */
#define NHL94_TM_BOXLIST     0x9A       /* u8[] penalty box list: roster offsets (player*2),
                                           negative ends (tmpdst + 2*maxros) [S] */
#define NHL94_MAXROS         26         /* roster slots per team (tmpdst entries) [S] */
#define NHL94_TMSIZE         0x364      /* AWTMSTRUCT - TMSTRUCT [V] */
#define NHL94_PENBUF         0xFFC3A4u  /* u16[MaxPen+1] pending calls (ClearPenaltyBuffer's
                                           movea.w at ROM $0128A6) [S/R] */
#define NHL94_PENBUF_CLEAR_WORDS 32     /* ClearPenaltyBuffer clears MaxPen words [S/R] */
/* tmpdst word bits tested with btst on the HIGH byte: #4/#5/#6 -> word bits 12/13/14 */
#define NHL94_PDST_BIT4      0x1000u    /* not counted by InProgress_ap (ends a coincidental) [S] */
#define NHL94_PDST_JUSTCALLED 0x2000u   /* bit 5: cleared by InProgress_ap [S] */
#define NHL94_PDST_COINC     0x4000u    /* bit 6: coincidental [S] */

/* Hook sites (ROM bytes checked [R]; see the plan's section 4 table) */
#define NHL94_ROM_RESTORETEAMS_R  0x007806u /* restoreteams .r: moveq #50,d0 after move.w #6,tmap(a2) */
#define NHL94_ROM_SETPLLIST_PRIO  0x015842u /* SetPlList: tst.w tmgoalie(a2) after movea.l #priolist,a0 */
#define NHL94_ROM_RELEASEPL_PRIO  0x012A06u /* releasepl: same pair */
#define NHL94_ROM_GETLINEE_PRIO   0x012ECAu /* getlinee: move.w tmap(a2),d4 after movea.l #priolist,a0 */
#define NHL94_ROM_FACEOFF_APL     0x00FA7Au /* puckfaceoff2 .t0: adda.w d4,a1 after movea.l #.apl,a1 */
#define NHL94_ROM_INPROGRESS_AP   0x0123DEu /* InProgress_ap entry: moveq #6,d1 (REPLACED in 4v4) */
#define NHL94_ROM_PENGOAL_CMP6    0x01284Eu /* PenGoalStuff: cmpi.w #6,tmap(a1) */
#define NHL94_ROM_LOWESTPEN_CMP6  0x012A50u /* GetLowestPen .endoflist: cmpi.w #6,tmap(a3) */
#define NHL94_ROM_FACEOFF_NORM    0x00F9D8u /* puckfaceoff2: movea.w #tmstruct,a2 after bsr ResetBench */
#define NHL94_ROM_SETPERSONEL_P2  0x0157E4u /* setpersonel ($015788) second pass: moveq #5,d4 / movea.w #PlList,a4 */
#define NHL94_ROM_PENSHOTCHK      0x01211Au /* PenShotChk entry: movem.l d1-d3/a0,-(sp) (REPLACED in 4v4) */
#define NHL94_PLLIST         0xFFCF14u  /* u8[6] players wanted on ice (1-based roster, 0 none); +6 plpos[6] [S/R] */

/* Guest-RAM block after the virtual-pad swap state (same unused stack-bottom
 * area as NHL94_SEATREQ: never written by the game). FMT is latched ONLY by
 * the restoreteams hook (game start); every other 4v4 hook reads FMT here,
 * never the host setting, so rollback and every peer see the same. */
#define NHL94_FMT            0xFFDFC0u  /* u16 0 = 6v6 (cartridge), 4 = 4v4 */
#define NHL94_FMT_4V4        4u
#define NHL94_PRIO4          0xFFDFC2u  /* u8[8] 0,1,4,3,2,5,6,$FF: G, LD, C, LW, RD, RW, X */
#define NHL94_APL4           0xFFDFCAu  /* u8[24] faceoff .apl rows for tmap 6 / 5 / 4 */
#define NHL94_FMT_TESTPEN    0xFFDFE2u  /* u16 TEST ONLY (NHL94_FORMAT_TEST_PENALTY): bit k = injected call k done */
#define NHL94_DELAYEDPEN     0xFFC300u  /* u8 (first byte of a long): bit 1 home / bit 2 away delayed call,
                                           bit 0 PP goal (PenGoalStuff bset at ROM $012884) [S/R] */
#define NHL94_PEN_TRIPPING   0x20       /* Penaltylist offset of Tripping (a 2-minute minor) [S] */

/* Penalty shot set-up (PenShotChk / getBAplayerInfo, penalty94.asm) [S/R] */
#define NHL94_BA_PS_PENDING_B 0x08      /* BA_PS_flags high byte bit 3: penalty shot pending */
#define NHL94_BA_TEAM        0xFFD404u  /* u16 0 home / 1 away: the shooting team */
#define NHL94_BA_SKTR_SCNUM  0xFFD406u  /* u16 shooter's SCnum */
#define NHL94_BA_GOALIE_SCNUM 0xFFD408u /* u16 defending goalie's SCnum */
#define NHL94_BA_SKATER_OFF  0xFFD40Au  /* u16 shooter's pnum */
#define NHL94_BA_GOALIE_OFF  0xFFD40Cu  /* u16 goalie's pnum */
#define NHL94_BA_CHECKER_OFF 0xFFD40Eu  /* u16 offender's pnum */
#define NHL94_PSPENALTY      0xFFD410u  /* u16 the penalty-shot penalty number */

/* "Human always has a player" block (nhl94_seats.c; docs/PLAYER_LOCK.md "Line
 * changes, substitutes, pulled goalie, penalty shots"). Two gaps of the same
 * never-written stack-bottom area: $FFDFB4-$FFDFBF and $FFDFE4-$FFDFFF.
 * Everything is guest RAM (snapshots / digests / rollback cover it) and is
 * only written while seats are configured, so Classic stays hash-identical. */
#define NHL94_SEATTGT(k)     (0xFFDFB4u + (uint32_t)(k)) /* u8[12] SC+1 of the object seat k controls this frame,
                                                            0 none ($FFDFB4..BF): written by enforce, read by
                                                            presentation (tags, camera) */
#define NHL94_SEATSUB(k)     (0xFFDFE4u + (uint32_t)(k)) /* u8[12] sticky substitute SC+1, 0 none ($FFDFE4..EF) */
#define NHL94_SEATVAC(k)     (0xFFDFF0u + (uint32_t)(k)) /* u8[12] frames the role has been vacant, saturating
                                                            ($FFDFF0..FB) */
#define NHL94_PSROLE         0xFFDFFCu  /* u16 role+1 of the fouled skater latched while a penalty shot is
                                           pending, 0 none ($FFDFFE..FF spare) */
#define NHL94_SFLAGS7        0xFFC2FCu  /* byte; bit 4 = line changes forced automatic (GameSetUp $0F8474) [S/R] */
#define NHL94_SFLAGS7_AUTOLC 0x10
#define NHL94_SHOOTOUTROUND  0xFFD578u  /* u16 shootout round [S] */
#define NHL94_ROM_SETPERSONEL 0x015788u /* collide94.asm setpersonel entry (observation site; (A7) = caller) */

#endif /* NHL94_RAM_H */
