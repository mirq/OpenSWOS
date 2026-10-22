| stub.s -- the 68k first hunk of a WarpOS executable made by elf2warpos.py.
|
| AmigaOS loads the file (LoadSeg), runs this, and this starts the PowerPC part:
|
|   1. Workbench start: wait for and take the WBStartup message.
|   2. Open powerpc.library.
|   3. Walk the segment list into a table of hunk addresses (on the stack).
|   3b. Make sure the PowerPC can run them: on a Sonnet the PPC executes only from
|      its own memory (MEMF_PPC); a hunk LoadSeg put anywhere else is copied there
|      and the table points at the copy. (Sonnet's powerpc.library patches LoadSeg
|      to do this, but only for a file it has marked as PPC in its protection bits;
|      on the first load of an unmarked file the code was left in 68k memory and
|      the PPC took an instruction-fetch exception.)
|   4. Apply the relocation table, which is the LAST hunk (format below).
|   5. Make sure of the stack: WarpOS gives the PPC side a stack the size of this
|      task's, which is whatever launched it (4 KB from an icon, 16 KB from some
|      shells). Smaller than v_stack (elf2warpos.py --stack writes it), it swaps
|      to one of that size (StackSwap) for the run.
|   6. RunPPC the code at the start of hunk 2 (the ELF's .text: _start), with
|      the arguments startupwos.o expects in its PPCArgs.
|   7. Close the library, reply the WBStartup message, return the PPC's result.
|
| The behaviour and the PPCArgs layout are the ones Elf2Exe2's stub has, which
| startupwos.o (MOS2WOS) is written against; this is our own implementation of
| them, so the converter can run on Linux and does not depend on Elf2Exe2.
|
| Relocation table, all longwords, big-endian:
|     { patch_hunk; { count; symbol_hunk; entry * count } ...; 0 } ...; 0
| an entry, with `off` the byte offset in patch_hunk and B = symbol_hunk's address:
|     0ooooooo...            ADDR32:     (off).l += B
|     1100oooo...  value     ADDR16_HA:  (off).w = ha(value + B)   (value: the next longword)
|     1101oooo...            ADDR16_LO:  (off).w += B & 0xffff
|     1110oooo...            REL24:      the branch at off: field = field + B - (address of the branch)
| (Elf2Exe2 also has a packed HA/LO pair, 10dddddddd hhh...; we never emit it,
| but apply it, so a table either converter wrote works here.)
|
| Assemble: m68k-amiga-elf-as -m68020 --register-prefix-optional stub.s -o stub.o
|           m68k-amiga-elf-objcopy -O binary -j .text stub.o stub.bin
| then paste stub.bin's hex into STUB in elf2warpos.py (elf2warpos.py --stub-hex).

MEMF_PUBLIC     = 0x00001
MEMF_PPC        = 0x02000                | SonnetAmiga: memory the PowerPC executes from
MEMF_CLEAR      = 0x10000

        .text
start:
        movem.l d1-a6,-(sp)
        lea     vars(pc),a5
        move.l  4.w,a6
        move.l  a6,v_sysbase-vars(a5)
        move.l  a0,v_args-vars(a5)
        move.l  d0,v_arglen-vars(a5)
        clr.l   v_wbmsg-vars(a5)

        | Workbench start: take the WBStartup message before anything else.
        sub.l   a1,a1
        jsr     -294(a6)                | FindTask(NULL)
        move.l  d0,a3
        tst.l   172(a3)                 | pr_CLI
        bne.s   .cli
        lea     92(a3),a0               | pr_MsgPort
        jsr     -384(a6)                | WaitPort
        lea     92(a3),a0
        jsr     -372(a6)                | GetMsg
        move.l  d0,v_wbmsg-vars(a5)
.cli:
        lea     ppcname(pc),a1
        moveq   #14,d0
        jsr     -552(a6)                | OpenLibrary("powerpc.library", 14)
        move.l  d0,v_ppcbase-vars(a5)
        bne.s   .haveppc
        moveq   #20,d7
        bra     .replywb

.haveppc:
        | Count the hunks: our own segment, then every next one.
        lea     start-4(pc),a0
        moveq   #1,d0
.count:
        move.l  (a0),d1
        beq.s   .counted
        lsl.l   #2,d1
        move.l  d1,a0
        addq.l  #1,d0
        bra.s   .count
.counted:
        move.l  d0,v_nhunks-vars(a5)
        lsl.l   #2,d0
        sub.l   d0,sp
        move.l  sp,a4                   | a4: the hunk address table
        move.l  v_nhunks-vars(a5),d0
        subq.l  #1,d0
        move.l  sp,a1
        lea     start(pc),a0
.fill:
        move.l  a0,(a1)+
        move.l  -4(a0),d1               | BPTR to the next segment
        lsl.l   #2,d1
        addq.l  #4,d1
        move.l  d1,a0
        dbf     d0,.fill

        | Hunks 1 .. n-2 (not the stub, not the table) into PPC memory. The copies'
        | addresses are kept beside the table, to be freed after the run.
        move.l  v_nhunks-vars(a5),d0
        lsl.l   #2,d0
        move.l  #MEMF_PUBLIC+MEMF_CLEAR,d1
        move.l  v_sysbase-vars(a5),a6
        jsr     -684(a6)                | AllocVec: the copies' addresses
        move.l  d0,v_copies-vars(a5)
        beq     .nomem
        moveq   #1,d6                   | d6: the hunk
.place:
        move.l  v_nhunks-vars(a5),d0
        subq.l  #1,d0
        cmp.l   d0,d6
        bcc.s   .placed
        move.l  (a4,d6.l*4),a2          | the hunk's data
        move.l  -8(a2),d5               | its segment's length, with the 8-byte header
        subq.l  #8,d5
        ble.s   .nextplace              | an empty hunk: nothing to run from
        move.l  a2,a1
        jsr     -534(a6)                | TypeOfMem
        andi.l  #MEMF_PPC,d0
        bne.s   .nextplace              | already where the PPC can run it
        move.l  d5,d0
        move.l  #MEMF_PUBLIC+MEMF_PPC+MEMF_CLEAR,d1
        jsr     -684(a6)                | AllocVec
        tst.l   d0
        beq     .nomem
        move.l  v_copies-vars(a5),a0
        move.l  d0,(a0,d6.l*4)
        move.l  d0,(a4,d6.l*4)          | relocate and run the copy
        move.l  a2,a0
        move.l  d0,a1
        move.l  d5,d0
        jsr     -624(a6)                | CopyMem(original, copy, length)
.nextplace:
        addq.l  #1,d6
        bra.s   .place
.placed:

        | The relocation table: the last hunk.
        move.l  v_nhunks-vars(a5),d0
        subq.l  #1,d0
        move.l  (a4,d0.l*4),a0
.patch:
        move.l  (a0)+,d0                | patch hunk, 0 = done
        beq     .run
        move.l  (a4,d0.l*4),a2
.group:
        move.l  (a0)+,d7                | count, 0 = next patch hunk
        beq.s   .patch
        move.l  (a0)+,d0
        move.l  (a4,d0.l*4),d3          | d3: the symbol hunk's address
.entry:
        move.l  (a0)+,d2
        bpl     .addr32
        btst    #30,d2
        beq     .pair
        btst    #29,d2
        bne     .rel24
        btst    #28,d2
        beq     .ha
        | ADDR16_LO
        andi.l  #0x0fffffff,d2
        move.w  (a2,d2.l),d4
        add.w   d3,d4
        move.w  d4,(a2,d2.l)
        bra     .next
.ha:
        andi.l  #0x0fffffff,d2
        move.l  (a0)+,d4
        add.l   d3,d4
        tst.w   d4
        bpl.s   .hapos
        addi.l  #0x10000,d4
.hapos:
        swap    d4
        move.w  d4,(a2,d2.l)
        bra     .next
.rel24:
        andi.l  #0x0fffffff,d2
        lea     (a2,d2.l),a3
        move.l  (a3),d4
        andi.l  #0x03fffffc,d4
        add.l   d3,d4
        sub.l   a3,d4
        andi.l  #0x03fffffc,d4
        andi.l  #0xfc000003,(a3)
        or.l    d4,(a3)
        bra     .next
.pair:
        move.l  d2,d4
        andi.l  #0x003fffff,d4
        lsl.l   #2,d4                   | d4: the HA instruction
        move.w  2(a2,d4.l),d5
        swap    d5
        move.l  d2,d6
        swap    d6
        lsr.l   #4,d6
        andi.l  #0x3fc,d6
        add.l   d4,d6                   | d6: the LO instruction
        move.w  2(a2,d6.l),d5
        add.l   d3,d5
        tst.w   d5
        bpl.s   .pairpos
        addi.l  #0x10000,d5
.pairpos:
        move.w  d5,2(a2,d6.l)
        swap    d5
        move.w  d5,2(a2,d4.l)
        bra     .next
.addr32:
        add.l   d3,(a2,d2.l)
.next:
        subq.l  #1,d7
        bne     .entry
        bra     .group

.run:
        | The stack: tc_SPUpper - tc_SPLower of this task, against v_stack.
        clr.l   v_newstk-vars(a5)
        move.l  v_sysbase-vars(a5),a6
        sub.l   a1,a1
        jsr     -294(a6)                | FindTask(NULL)
        move.l  d0,a3
        move.l  62(a3),d0               | tc_SPUpper
        sub.l   58(a3),d0               | tc_SPLower
        cmp.l   v_stack-vars(a5),d0
        bcc.s   .stackok
        move.l  v_stack-vars(a5),d0
        moveq   #0,d1                   | MEMF_ANY
        jsr     -684(a6)                | AllocVec
        move.l  d0,v_newstk-vars(a5)
        beq.s   .stackok                | no memory: run on the stack there is
        lea     v_sss-vars(a5),a0       | struct StackSwapStruct
        move.l  d0,(a0)                 | stk_Lower
        add.l   v_stack-vars(a5),d0
        move.l  d0,4(a0)                | stk_Upper
        move.l  d0,8(a0)                | stk_Pointer
        jsr     -732(a6)                | StackSwap
.stackok:
        | RunPPC: struct PPCArgs is 144 bytes. PP_Code = hunk 2 (.text, _start at +0).
        lea     -144(sp),sp
        move.l  8(a4),(sp)              | PP_Code
        clr.l   4(sp)                   | PP_Offset
        clr.l   8(sp)                   | PP_Flags
        clr.l   12(sp)                  | PP_Stack
        clr.l   16(sp)                  | PP_StackSize
        move.l  v_args-vars(a5),20(sp)  | PP_Regs[0]: the argument string
        move.l  v_ppcbase-vars(a5),24(sp) | PP_Regs[1]: PowerPCBase
        move.l  v_wbmsg-vars(a5),52(sp) | PP_Regs[8]: the WBStartup message
        move.l  v_arglen-vars(a5),56(sp)  | PP_Regs[9]: the argument length
        move.l  sp,a0
        move.l  v_ppcbase-vars(a5),a6
        jsr     -30(a6)                 | RunPPC
        move.l  20(sp),d7               | PP_Regs[0]: the result
        lea     144(sp),sp
        move.l  v_newstk-vars(a5),d0
        beq.s   .ownstack
        move.l  v_sysbase-vars(a5),a6
        lea     v_sss-vars(a5),a0
        jsr     -732(a6)                | StackSwap back
        move.l  v_newstk-vars(a5),a1
        jsr     -690(a6)                | FreeVec
.ownstack:
        bsr     .freecopies
        move.l  v_nhunks-vars(a5),d0
        lea     (sp,d0.l*4),sp          | drop the hunk table
        move.l  v_ppcbase-vars(a5),a1
        move.l  v_sysbase-vars(a5),a6
        jsr     -414(a6)                | CloseLibrary

.replywb:
        move.l  v_sysbase-vars(a5),a6
        move.l  v_wbmsg-vars(a5),d2
        beq.s   .done
        jsr     -132(a6)                | Forbid: the reply must be the last thing
        move.l  d2,a1
        jsr     -378(a6)                | ReplyMsg
.done:
        move.l  d7,d0
        movem.l (sp)+,d1-a6
        rts

        | No memory for the copies: give back what there is, close up, fail.
.nomem:
        bsr     .freecopies
        move.l  v_nhunks-vars(a5),d0
        lea     (sp,d0.l*4),sp          | drop the hunk table
        move.l  v_ppcbase-vars(a5),a1
        move.l  v_sysbase-vars(a5),a6
        jsr     -414(a6)                | CloseLibrary
        moveq   #20,d7
        bra.s   .replywb

        | FreeVec every copy, then the list of them. Keeps d7.
.freecopies:
        move.l  v_copies-vars(a5),d0
        beq.s   .fcdone
        move.l  v_sysbase-vars(a5),a6
        move.l  d0,a2
        move.l  v_nhunks-vars(a5),d6
.fcloop:
        subq.l  #1,d6
        bmi.s   .fclist
        move.l  (a2,d6.l*4),d0
        beq.s   .fcloop
        move.l  d0,a1
        jsr     -690(a6)                | FreeVec
        bra.s   .fcloop
.fclist:
        move.l  a2,a1
        jsr     -690(a6)                | FreeVec
        clr.l   v_copies-vars(a5)
.fcdone:
        rts

        .balign 4
vars:
v_sysbase:  .long 0
v_args:     .long 0
v_arglen:   .long 0
v_nhunks:   .long 0
v_wbmsg:    .long 0
v_ppcbase:  .long 0
v_newstk:   .long 0
v_copies:   .long 0
v_sss:      .long 0,0,0             | struct StackSwapStruct
v_stack:    .long 0x53544b21        | "STK!": elf2warpos.py writes the stack size here
ppcname:    .asciz "powerpc.library"
        .balign 4
