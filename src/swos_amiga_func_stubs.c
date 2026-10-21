/*
 * swos_amiga_func_stubs.c -- C stubs for Amiga assembly functions that
 * are still referenced by the ported C code but have no C definition.
 *
 * Each one is a no-op. They will be replaced with real SDL-aware
 * implementations as we wire up subsystems.
 *
 * (The interrupt handlers Int2/3/4/5 used to be entries in the m68k
 * vector table; on SDL the host pumps events directly so these never
 * fire.)
 */

#include "swos_types.h"
#include "swos_platform.h"
#include "display.h"   /* extern volatile struct Custom *custom */

#ifdef SWOS_CHUNKY_BUILD
/* SDL doesn't link the Amiga bootstrap; provide empty stubs so the
 * SWOS.c hardware-takeover path links cleanly. On Amiga these live
 * in resload.c (with real Exec/DOS calls) and we must NOT redefine. */
void SafeStart(void)        {}
void LoadDosLibrary(void)   {}
void DetachOSDisplay(void)  {}
#endif

/* The Int*Handler entries are direct m68k vector-table entries: the
 * CPU jumps here on a hardware interrupt and the function must return
 * via `rte`, not `rts`. The original SWOS2.s wrappers were pure asm
 * (`movem.l d0-d7/a0-a6,-(sp)` ... `movem.l (sp)+,...` / `rte`); GCC's
 * `__attribute__((interrupt))` is supposed to emit the same thing but
 * the alert we saw on first enable says it doesn't quite (F-line trap
 * F227 on boot — interrupt-attribute body returns via `rts`, popping
 * an `rte`-style frame, which lands the PC on garbage). Use naked
 * inline asm so the prologue/epilogue are exactly the m68k ABI. */
#if defined(__m68k__) || defined(__amigaos__) || defined(__amigaos4__)
extern void _Int2KeyboardHandler(void);  /* worker in interrupts.c */
extern void _Int3TimerHandler(void);     /* worker in interrupts.c */

/* Restore real ISR bodies. The earlier `move.w #imm,0xDFF09C` form was
 * being assembled to an opcode the CPU interpreted as line-F — load
 * the custom base into A0 first and use displacement addressing (which
 * is what the original SWOS2.s asm did via A6=CUSTOM). */

__attribute__((naked, used))
void Int2KeyboardHandler(void)
{
    __asm__ volatile (
        "movem.l %%d0-%%d7/%%a0-%%a6,-(%%sp)\n\t"
        "jsr     _Int2KeyboardHandler\n\t"
        "movem.l (%%sp)+,%%d0-%%d7/%%a0-%%a6\n\t"
        "rte\n\t"
        ::: "memory"
    );
}

__attribute__((naked, used))
void Int3TimerHandler(void)
{
    __asm__ volatile (
        "movem.l %%d0-%%d7/%%a0-%%a6,-(%%sp)\n\t"
        "jsr     _Int3TimerHandler\n\t"
        "movem.l (%%sp)+,%%d0-%%d7/%%a0-%%a6\n\t"
        "rte\n\t"
        ::: "memory"
    );
}

/* Level 3 is the VBlank and, while the background undraw runs, the blitter
 * (ProcessSpriteBlitQueueAsync, display.c). A finished blit starts the next entry of the
 * queue (12 bytes: A source, D dest, the modulo of both -- or, with bit 15 set, of D alone
 * with A contiguous: a tile blit, BLTSIZE) in a few instructions;
 * at the sentinel it switches the blitter interrupt off and clears g_blitQActive. The
 * VBlank's own work runs only for a VBlank, and acknowledges COPER/VERTB but never BLIT --
 * a blit that finished during it would otherwise be lost and the queue would stall. */
__attribute__((naked, used))
void Int3Handler(void)
{
    __asm__ volatile (
        "movem.l %%d0/%%a0-%%a1,-(%%sp)\n\t"
        "lea     0xDFF000,%%a0\n\t"
        "tst.w   g_blitQActive\n\t"
        "beq.s   2f\n\t"
        "btst    #6,0x1F(%%a0)\n\t"          /* INTREQR: BLIT */
        "beq.s   2f\n\t"
        "move.w  #0x0040,0x9C(%%a0)\n\t"     /* acknowledge BLIT */
        "movea.l g_blitQPtr,%%a1\n\t"
        "move.l  (%%a1)+,%%d0\n\t"
        "bmi.s   1f\n\t"                     /* sentinel: the queue is done */
        "move.l  %%d0,0x50(%%a0)\n\t"        /* BLTAPT */
        "move.l  (%%a1)+,0x54(%%a0)\n\t"     /* BLTDPT */
        "move.w  (%%a1)+,%%d0\n\t"
        "bclr    #15,%%d0\n\t"               /* flag: a tile blit, A contiguous */
        "beq.s   4f\n\t"
        "clr.w   0x64(%%a0)\n\t"             /* BLTAMOD 0 */
        "bra.s   5f\n\t"
        "4:\n\t"
        "move.w  %%d0,0x64(%%a0)\n\t"        /* BLTAMOD */
        "5:\n\t"
        "move.w  %%d0,0x66(%%a0)\n\t"        /* BLTDMOD */
        "move.w  (%%a1)+,0x58(%%a0)\n\t"     /* BLTSIZE: starts it */
        "move.l  %%a1,g_blitQPtr\n\t"
        "bra.s   2f\n\t"
        "1:\n\t"
        "move.w  #0x0040,0x9A(%%a0)\n\t"     /* INTENA: BLIT off */
        "clr.w   g_blitQActive\n\t"
        "2:\n\t"
        "move.w  0x1E(%%a0),%%d0\n\t"        /* INTREQR */
        "andi.w  #0x0030,%%d0\n\t"           /* a VBlank (or copper) one too? */
        "movem.l (%%sp)+,%%d0/%%a0-%%a1\n\t"
        "bne.s   3f\n\t"
        "rte\n\t"
        "3:\n\t"
        "movem.l %%d0-%%d7/%%a0-%%a6,-(%%sp)\n\t"
        "jsr     _Int3TimerHandler\n\t"
        "lea     0xDFF000,%%a0\n\t"
        "move.w  #0x0030,0x9C(%%a0)\n\t"
        "movem.l (%%sp)+,%%d0-%%d7/%%a0-%%a6\n\t"
        "rte\n\t"
        ::: "memory"
    );
}

/* Int4Handler and Int5Handler save A0 since they use it for the INTREQ
 * write. Without these pushes the interrupted code's A0 gets clobbered
 * to 0xDFF000 across the RTE — corrupted dos.library mid-call on Amiga
 * boot during the May 2026 bring-up. */
__attribute__((naked, used))
void Int4Handler(void)
{
    __asm__ volatile (
        "move.l  %%a0,-(%%sp)\n\t"
        "lea     0xDFF000,%%a0\n\t"
        "move.w  #0x0780,0x9C(%%a0)\n\t"   /* ack AUD0..AUD3 */
        "movea.l (%%sp)+,%%a0\n\t"
        "rte\n\t"
        ::: "memory"
    );
}

__attribute__((naked, used))
void Int5Handler(void)
{
    __asm__ volatile (
        "move.l  %%a0,-(%%sp)\n\t"
        "lea     0xDFF000,%%a0\n\t"
        "move.w  #0x1800,0x9C(%%a0)\n\t"   /* ack RBF + DSKSYN */
        "movea.l (%%sp)+,%%a0\n\t"
        "rte\n\t"
        ::: "memory"
    );
}

#else  /* SDL — never installed as a vector */
void Int2KeyboardHandler(void) {}
void Int3Handler(void)         {}
void Int3TimerHandler(void)    {}
void Int4Handler(void)         {}
void Int5Handler(void)         {}
#endif

/* The CallMenu* helpers are tiny m68k trampolines that swap argument
 * order before calling a menu callback. On SDL we just call the
 * function pointer directly with host calling convention. This is
 * what enables onInit / onSelect / beforeDraw callbacks -- without
 * these the main menu never runs SWOSMainMenuInit so things like
 * the START and SAVE buttons never get hidden. */
void CallMenuCallback(void *funcPtr)
{
    if (funcPtr) ((void (*)(void))funcPtr)();
}

void CallMenuFuncWithXY(void *funcPtr, uint16_t x, uint16_t y)
{
    if (funcPtr) ((void (*)(uint16_t, uint16_t))funcPtr)(x, y);
}

void CallMenuOnSelect(void *funcPtr, void *selectedEntry)
{
    if (funcPtr) ((void (*)(void *))funcPtr)(selectedEntry);
}
