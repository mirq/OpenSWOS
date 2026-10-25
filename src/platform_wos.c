/*
 * platform_wos.c -- the WarpOS build's screen, clock and event loop
 * (docs/WARPOS.md). The chunky target's platform API (platform_sdl.h) on an
 * 8-bit RTG screen: CyberGraphX's API, which Picasso96 speaks too.
 *
 * The game draws 8bpp chunky -- one byte, one palette index -- which IS the
 * pixel format of an 8-bit LUT8 screen, so a frame is shown by copying its rows
 * into the card's memory under LockBitMapTagList, and its colours by LoadRGB32
 * when they change. No conversion, no Chip RAM. The rules below are the RTG
 * manual's (Amiga-68K-BobrHopper docs/RTG_MANUAL.md), each one a bug somebody hit:
 *
 *   - BestCModeIDTagList never fails: it answers a chipset mode when nothing
 *     fits. Every mode is checked three ways (a CyberGraphX id, 8 bits deep,
 *     1 byte a pixel) and for room for the picture.
 *   - Between LockBitMapTagList and UnLockBitMap: nothing but the copy, no OS
 *     call, no log, no early return; base and pitch are read on every lock.
 *   - ActivateWindow AND ScreenToFront: one does not imply the other.
 *
 * Every OS call here is a switch to the 68k side and back, with a cache flush
 * each way (the ppcinline stubs' IF_CACHEFLUSHALL): a frame makes as few as it
 * can -- the lock and unlock, the window's messages, the two joystick ports, and
 * LoadRGB32 only when the palette changed. The clock and the wait between
 * frames are powerpc.library's own and never leave the PPC.
 *
 * Single-threaded, as the WASM build is: there is no timer thread, so the Amiga's
 * VBlank interrupt (_Int3TimerHandler) runs from Platform_VBlank, which every
 * wait in the chunky game goes through.
 *
 * The SDL build compiles platform_sdl.c, so this one is empty there.
 */

#if defined(SWOS_CHUNKY_BUILD) && !defined(SWOS_SDL_BUILD)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform_sdl.h"

#pragma pack(push, 2)
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#ifdef SWOS_WARPOS_BUILD
#include <cybergraphx/cybergraphics.h>
#include <proto/cybergraphics.h>
#else
#include "rtg_cgx68k.h"         /* the 68060: no CyberGraphX headers in its NDK */
#endif
#include <libraries/asl.h>
#include <dos/var.h>
#include <proto/asl.h>
#pragma pack(pop)
#include "rtg_clock.h"

/* Library bases: the MOS2WOS startup (and libnix's, on the 68060) opens exec and
 * dos and nothing else, so these are ours to define and open. */
struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *CyberGfxBase;
struct Library       *AslBase;

#ifndef SWOS_WARPOS_BUILD
/* The 68060: the stack libnix's startup swaps in before main() when the Shell's
 * is smaller (swapstack.o, which Makefile.rtg060 links with -u ___stkswap). The
 * WarpOS stub gives the PPC the same 1 MB (tools/elf2warpos --stack): the JSON
 * readers and the front end's recursion want far more than a Shell's 4 KB. */
unsigned long __stack = 1024UL * 1024UL;
#endif

extern int _screen_width, _screen_height;
extern void _Int3TimerHandler(void);

int  AudioSDL_Init(int mono);       /* audio_wos.c */
void AudioSDL_Shutdown(void);
void AudioSDL_Tick(void);
void InputWos_Init(void);
void InputWos_Shutdown(void);
void InputWos_Key(uint16_t code, int repeat);
void InputWos_PostFrame(void);
void InputWos_ClearKeyState(void);

/* The largest picture the game draws without widescreen: the Amiga view (the
 * DOS view, 320x200, fits inside it). The screen is at least this big. */
#define WOS_PICTURE_W 336
#define WOS_PICTURE_H 272

static struct Screen *s_screen;
static struct Window *s_window;
static UWORD         *s_blank_pointer;      /* Chip RAM: Intuition reads it with DMA */
static ULONG          s_mode_id;
static int            s_mode_w, s_mode_h;   /* the screen's size */
static int            s_use_lock;           /* 1: LockBitMapTagList + copy, 0: WriteLUTPixelArray */
static int            s_clear_borders = 1;  /* the picture moved or the screen is new */
static int            s_shown_w, s_shown_h; /* the picture size the borders were cleared for */
static PlatformFullscreenMode s_fullscreen_mode = PLATFORM_FULLSCREEN_EXCLUSIVE;
static int            s_win_scale = 1;

/* The palette as LoadRGB32 last loaded it, and its XRGB8 mirror for the
 * WriteLUTPixelArray fallback -- kept together, so a mid-session demotion to
 * the fallback shows the right colours at once. */
/* Double buffering (Intuition's screen buffers): the frame goes into the buffer
 * that is NOT on screen and ChangeScreenBuffer shows it at the next vertical
 * blank, so a scroll never tears where the copy crosses the beam. A buffer is
 * written again only after Intuition says it is off screen (its SafeMessage).
 * Without screen buffers (the driver gives none) the frame goes into the one
 * bitmap on screen, as before.
 *
 * Per buffer, the frame it holds, to send only the rows that differ: a buffer
 * holds the frame before last, so a still menu copies nothing and a scrolling
 * match all of it. */
static struct ScreenBuffer *s_sb[2];
static int                  s_no_safe_msgs;     /* no SafeMessage: none is waited for */
static uint64_t             s_disp_period_us;   /* the display's refresh, from timed flips */
static uint64_t             s_flip_cost_us;     /* the cheapest flip off the vertical blank */
static uint64_t             s_vblank_us;        /* a vertical blank, from the fitted flips */
static double               s_disp_period_frac; /* the refresh in us, with its fraction */
static int                  s_flipped;          /* this frame went up with a flip */
static int                  s_locked;           /* the last frame was paced by its flip */
static struct MsgPort      *s_safe_port;
static int                  s_dbuf;             /* 1: the two buffers are in use */
static int                  s_back;             /* the buffer to draw into */
static int                  s_front = 0;        /* the buffer on screen */
static int                  s_safe_pending[2];  /* flipped away, not yet safe to write */
static uint8_t s_shadow[2][672 * 272] __attribute__((aligned(4)));   /* rows_equal compares longwords */
static void dbuf_open(void);
static int getenv_no_vsync(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("SWOS_NO_VSYNC") != NULL;
    return v;
}
/* Where a bitmap's pixels are, from a lock (0 when it gives none). */
static ULONG bitmap_base(struct BitMap *bm)
{
    ULONG base = 0;
    struct TagItem t[2];
    APTR h;
    t[0].ti_Tag = LBMI_BASEADDRESS; t[0].ti_Data = (ULONG)&base;
    t[1].ti_Tag = TAG_DONE;         t[1].ti_Data = 0;
    h = LockBitMapTagList((APTR)bm, t);
    if (!h)
        return 0;
    UnLockBitMap(h);
    return base;
}
/* The display's refresh is within 4% of `period`: its flips can pace the frames. */
static int display_paces(uint64_t period)
{
    return s_disp_period_us > period - period / 25 && s_disp_period_us < period + period / 25;
}

/* The frame period. The display's own refresh when it is the game's rate: the game
 * made at 50.0 Hz on a 50.7 Hz display (measured, 2026-10-22) showed a frame twice
 * every 1.4 s -- a regular hitch. A refresh that is not the game's keeps the game's
 * rate (a 100 Hz mode shows each frame twice, evenly). */
static uint64_t frame_period_us(void)
{
    uint64_t period = 1000000u / (uint64_t)Platform_ActivePacingFps();
    if (!getenv_no_vsync() && display_paces(period))
        return s_disp_period_us;
    return period;
}
static void vsync_probe(void);
static void vblank_probe(void);
static void dbuf_close(void);
static void wait_safe(int b);
static uint64_t now_us(void);
static int     s_shadow_valid[2], s_shadow_w[2];

static uint8_t s_rgb[768];
static uint8_t s_rgb_loaded[768];
static int     s_palette_loaded;
static ULONG   s_ctable[256];

static void (*s_frame_hook)(void);

/* SWOS_PRESENT_STATS=1: where a frame's time goes, one line every 250 frames.
 * `work` is the game's own (simulation and drawing, all PPC), `events` the
 * window's messages and the joysticks, `show` the palette and the copy into the
 * card -- those two are the 68k side, and every call there is a switch and a
 * cache flush. `late` counts frames that missed their slot. */
static int      s_present_stats = -1;
static uint64_t s_st_work, s_st_events, s_st_audio, s_st_show, s_st_wait, s_st_last_end;
static int32_t  s_st_n, s_st_late, s_st_palette;
static uint32_t s_st_rows;
static uint32_t s_st_bytes, s_st_shifts;     /* scrolling on the card: bytes sent, shifts */
static uint64_t s_st_shift_us, s_st_cmp_us;  /* ... the BltBitMap call, the compare */
static uint64_t s_st_lock, s_st_copy, s_st_unlock;  /* inside the show: the lock's own cost */

static uint64_t now_us(void);
/* The match frame's phases (include/raster_bars.h's RASTER markers): the time from
 * one marker to the next goes to the first's phase. Only with SWOS_PRESENT_STATS. */
static const int k_phase_rgb[] = { 0x0F00, 0x0FF0, 0x0001, 0x0002, 0x0003, 0x0004, 0x00F0, 0x0F0F, 0x000F, 0x00FF, 0x0000 };
static const char *const k_phase_name[] = { "undraw", "camera", "scrolldraw", "hilcoords",
                                            "replayrec", "samples", "sim", "crowd", "sprites",
                                            "hud", "idle" };
#define NUM_PHASES 11
static uint64_t s_phase_us[NUM_PHASES], s_phase_t;
static int      s_phase_cur = -1;
static uint32_t s_phase_frames;

void Platform_Phase(int rgb12)
{
    uint64_t now;
    int i;
    if (s_present_stats <= 0)
        return;
    now = now_us();
    if (s_phase_cur >= 0)
        s_phase_us[s_phase_cur] += now - s_phase_t;
    for (i = 0; i < NUM_PHASES && k_phase_rgb[i] != rgb12; i++)
        ;
    s_phase_cur = i < NUM_PHASES - 1 ? i : -1;  /* idle: the frame's phases are over */
    if (i == 0)
        s_phase_frames++;
    s_phase_t = now;
}
static uint64_t s_st_flip, s_st_max_flip;   /* time inside ChangeScreenBuffer */
static uint32_t s_st_flips, s_st_refused, s_st_same, s_st_locked;
static uint64_t s_st_max_gap, s_st_max_events, s_st_prev_t0, s_st_oversleep;

/* ---- The program's drawer ------------------------------------------- */

/* The current directory, as AmigaDOS has it: CurrentDir hands back the lock it
 * replaces, which belongs to whoever started us and goes back at exit, never
 * UnLocked here. */
static BPTR s_old_dir;
static BPTR s_prog_dir;
static int  s_dir_entered;

static void leave_program_dir(void)
{
    if (s_dir_entered) {
        CurrentDir(s_old_dir);
        UnLock(s_prog_dir);
        s_dir_entered = 0;
    }
}

int Platform_EnterProgramDir(void)
{
    s_prog_dir = Lock((CONST_STRPTR)"PROGDIR:", ACCESS_READ);
    if (!s_prog_dir)
        return -1;
    s_old_dir = CurrentDir(s_prog_dir);
    s_dir_entered = 1;
    atexit(leave_program_dir);
    return 0;
}

/* ---- The log ---------------------------------------------------------- */

/* stderr as a stream whose every line goes, as it is written,
 *   - to SWOS.log, opened, appended and closed each time: AmigaDOS writes a file's
 *     size only on Close, so a log left open through a crash reads back empty;
 *   - to the amiga-bridge daemon, when it runs (its port AMIGABRIDGE): the
 *     development link to this machine, which shows the lines live and keeps them
 *     whatever becomes of the machine.
 * Installed first thing in main() with no file -- the bridge alone -- and given the
 * file once the game folder is known. Slower than a plain file, and worth it: a
 * write is a line, and a match writes few. */
#pragma pack(push, 2)
struct BridgeMsg {
    struct Message msg;
    UWORD          version;         /* 1 */
    UWORD          type;            /* 1 register, 3 log */
    ULONG          clientId;
    ULONG          cmdId;
    LONG           result;
    ULONG          dataLen;
    char           data[256];
    APTR           extData;
    ULONG          extDataLen;
};
#pragma pack(pop)

static char              s_log_path[1088];
static struct MsgPort   *s_ab_reply;
static struct BridgeMsg *s_ab_msg;
static ULONG             s_ab_client;

static int bridge_send(UWORD type, const char *data, ULONG len)
{
    struct MsgPort *daemon;
    struct Message *got;
    if (!s_ab_msg)
        return -1;
    if (len > sizeof s_ab_msg->data)
        len = sizeof s_ab_msg->data;
    memset(s_ab_msg, 0, sizeof *s_ab_msg);
    s_ab_msg->msg.mn_Node.ln_Type = NT_MESSAGE;
    s_ab_msg->msg.mn_Length = sizeof *s_ab_msg;
    s_ab_msg->msg.mn_ReplyPort = s_ab_reply;
    s_ab_msg->version = 1;
    s_ab_msg->type = type;
    s_ab_msg->clientId = s_ab_client;
    memcpy(s_ab_msg->data, data, len);
    s_ab_msg->dataLen = len;
    Forbid();
    daemon = FindPort((CONST_STRPTR)"AMIGABRIDGE");
    if (daemon)
        PutMsg(daemon, &s_ab_msg->msg);
    Permit();
    if (!daemon)
        return -1;
    for (;;) {
        WaitPort(s_ab_reply);
        got = GetMsg(s_ab_reply);
        if (got == &s_ab_msg->msg)
            break;
        if (got)
            ReplyMsg(got);          /* the daemon asking something: nothing to say */
    }
    return (int)s_ab_msg->result;
}

static void bridge_open(void)
{
    static const char name[] = "OpenSWOS";
    if (s_ab_msg)
        return;
    s_ab_reply = CreateMsgPort();
    s_ab_msg = (struct BridgeMsg *)AllocVec(sizeof *s_ab_msg, MEMF_PUBLIC | MEMF_CLEAR);
    if (!s_ab_reply || !s_ab_msg || bridge_send(1, name, sizeof name) != 0) {
        if (s_ab_msg) FreeVec(s_ab_msg);
        if (s_ab_reply) DeleteMsgPort(s_ab_reply);
        s_ab_msg = NULL;
        s_ab_reply = NULL;
        return;
    }
    s_ab_client = s_ab_msg->clientId;
}

/* One write of stderr as log messages: "1|text", no newline, 250 bytes at most each. */
static void bridge_log(const char *buf, int n)
{
    char line[256];
    while (n > 0) {
        int k = n > 250 ? 250 : n, len = k;
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            len--;
        line[0] = '1';              /* AB_INFO */
        line[1] = '|';
        memcpy(line + 2, buf, (size_t)len);
        line[2 + len] = '\0';
        if (len > 0)
            bridge_send(3, line, (ULONG)(len + 3));
        buf += k;
        n -= k;
    }
}

/* Each log line used to be a round trip to the bridge task and an open/append/
 * close of SWOS.log -- several milliseconds -- and a tester build logs every menu
 * move ([menu-nav]): holding an arrow cost 60 ms a frame (measured, 2026-10-22).
 * So lines collect here and go to the file together: every 2 s from the frame
 * loop (Platform_LogTick, after the frame is shown), when the buffer fills, and at
 * exit. A crash loses at most the last two seconds. */
static char     s_log_buf[32768];
static int      s_log_len;
static uint64_t s_log_flushed_us;

#ifdef SWOS_WARPOS_BUILD
static void log_flush(void)
{
    FILE *f;
    if (s_log_len == 0)
        return;
    if (s_log_path[0] && (f = fopen(s_log_path, "a")) != NULL) {
        fwrite(s_log_buf, 1, (size_t)s_log_len, f);
        fclose(f);
    }
    s_log_len = 0;
}

static int sync_log_write(void *cookie, const char *buf, int n)
{
    (void)cookie;
    if (s_ab_msg)
        bridge_log(buf, n);
    if (!s_log_path[0])
        return n;
    if (s_log_len + n > (int)sizeof s_log_buf)
        log_flush();
    if (n > (int)sizeof s_log_buf) {        /* longer than the buffer: straight out */
        FILE *f = fopen(s_log_path, "a");
        if (f) {
            fwrite(buf, 1, (size_t)n, f);
            fclose(f);
        }
        return n;
    }
    memcpy(s_log_buf + s_log_len, buf, (size_t)n);
    s_log_len += n;
    return n;
}

/* From the frame loop, once a frame: the buffered lines out every 2 s. */
static uint64_t now_us(void);
static void Platform_LogTick(void)
{
    uint64_t now;
    if (s_log_len == 0)
        return;
    now = now_us();
    if (now - s_log_flushed_us >= 2000000u) {
        log_flush();
        s_log_flushed_us = now;
    }
}

/* path NULL: the bridge only (the first thing main() does). A path: that file too,
 * started afresh. */
int Platform_OpenSyncLog(const char *path)
{
    static FILE *s_log;
    static int s_atexit;
    /* The bridge copy is a development aid: SWOS_LOG_BRIDGE=1 (ENV:) asks for it. */
    if (getenv("SWOS_LOG_BRIDGE"))
        bridge_open();
    if (!s_atexit) {
        atexit(log_flush);
        s_atexit = 1;
    }
    if (path) {
        FILE *f;
        snprintf(s_log_path, sizeof s_log_path, "%s", path);
        f = fopen(s_log_path, "w");     /* a new log each run */
        if (f)
            fclose(f);
        else
            s_log_path[0] = '\0';
    }
    if (!s_log) {
        s_log = fwopen(NULL, sync_log_write);     /* newlib's BSD funopen */
        if (!s_log)
            return -1;
        setvbuf(s_log, NULL, _IOLBF, 0);  /* a line at a time */
        stderr = s_log;
    }
    return 0;
}

#else /* the 68060 */

/* libnix has no funopen, so stderr cannot be a cookie stream: it is SWOS.log itself,
 * reopened, with the 32 KB above as its buffer. What the PPC build gets from its
 * cookie it gets this way -- lines out together every 2 s (Platform_LogTick) and
 * when the buffer fills -- and the file is CLOSED at each of them and opened again
 * for append, so its size on disk is current when the machine crashes. No copy to
 * the bridge: nothing sees the lines on their way out. Until the game folder is
 * known stderr is the Shell's window. */
static uint64_t now_us(void);

static int log_reopen(const char *mode)
{
    if (!freopen(s_log_path, mode, stderr)) {
        s_log_path[0] = '\0';
        return -1;
    }
    setvbuf(stderr, s_log_buf, _IOFBF, sizeof s_log_buf);
    return 0;
}

static void log_flush(void)
{
    if (s_log_path[0])
        fflush(stderr);
}

static void Platform_LogTick(void)
{
    extern volatile uint16_t _playGame;
    uint64_t now;
    if (!s_log_path[0])
        return;
    /* Not during a match: closing and opening the file is file-system work on this
     * one processor, and it cost a frame or two every 2 s. stdio still writes when
     * its 32 KB fill; a crash reopens the log itself (rtg_crash_report). */
    if (_playGame)
        return;
    now = now_us();
    if (now - s_log_flushed_us >= 2000000u) {
        log_reopen("a");                /* fclose writes the size; nothing is lost */
        s_log_flushed_us = now;
    }
}

int Platform_OpenSyncLog(const char *path)
{
    if (!path)
        return 0;
    snprintf(s_log_path, sizeof s_log_path, "%s", path);
    return log_reopen("w");             /* a new log each run */
}

/* ---- A crash, in the log --------------------------------------------------
 * A CPU exception (a Line-F, an access fault, an illegal instruction) gives the
 * "Program failed" requester and nothing else: the address it happened at is
 * not on the screen and not in the log. So the task's trap handler (tc_TrapCode)
 * is ours: it notes the vector and the faulting PC, points the exception frame's
 * PC at rtg_crash_report and returns into it, in user mode, on the stack the
 * program had. The report goes to SWOS.log -- the PC, where main() is (the hunk
 * moved, the link's addresses did not: subtract to get an address for addr2line
 * against build/rtg060/m68k/SWOS.debug), and the stack, whose return addresses
 * are the call chain -- and then the game closes its screen and exits (or, with
 * SWOS_CRASH_REQUESTER=1, runs the fault again for the system's requester).
 *
 * On entry Exec has pushed the vector number (a long) on top of the CPU's frame,
 * whose PC is 2 bytes in for every 68060 frame format. */
static volatile ULONG s_trap_vec, s_trap_pc, s_trap_sp;
static APTR           s_old_trapcode;
void rtg_crash_report(void);
extern int main(int argc, char **argv);
void Platform_Shutdown(void);

__asm__(
    "   .text\n"
    "   .even\n"
    "_rtg_trap_handler:\n"
    "   move.l  (sp)+,_s_trap_vec\n"
    "   move.l  2(sp),_s_trap_pc\n"
    "   move.l  a0,-(sp)\n"
    "   move.l  usp,a0\n"
    "   move.l  a0,_s_trap_sp\n"
    "   move.l  (sp)+,a0\n"
    "   move.l  #_rtg_crash_report,2(sp)\n"
    "   rte\n");
extern void rtg_trap_handler(void);

void rtg_crash_report(void)
{
    ULONG *sp = (ULONG *)s_trap_sp;     /* the program's stack at the fault */
    ULONG pc = s_trap_pc, vec = s_trap_vec;
    const UWORD *code = (const UWORD *)(pc & ~1UL);
    int i;

    FindTask(NULL)->tc_TrapCode = s_old_trapcode;
    fprintf(stderr, "[crash] exception vector %lu at PC $%08lx; main() is at $%08lx "
                    "(link address: see `m68k-amigaos-nm SWOS.debug | grep _main$`)\n",
            (unsigned long)vec, (unsigned long)pc, (unsigned long)(uintptr_t)&main);
    fprintf(stderr, "[crash] code at the PC: %04x %04x %04x %04x %04x %04x %04x %04x\n",
            code[0], code[1], code[2], code[3], code[4], code[5], code[6], code[7]);
    fprintf(stderr, "[crash] the stack from $%08lx:\n", (unsigned long)(uintptr_t)sp);
    for (i = 0; i < 160; i += 8)
        fprintf(stderr, "[crash]  %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
                (unsigned long)sp[i], (unsigned long)sp[i + 1], (unsigned long)sp[i + 2],
                (unsigned long)sp[i + 3], (unsigned long)sp[i + 4], (unsigned long)sp[i + 5],
                (unsigned long)sp[i + 6], (unsigned long)sp[i + 7]);
    log_reopen("a");                    /* out, and the file's size written */
    /* Out, as cleanly as a crashed program can: the screen, AHI and the libraries
     * closed, so the machine is back at the Workbench rather than behind a
     * requester whose only way on is a reboot. SWOS_CRASH_REQUESTER=1 runs the
     * fault again instead, under the system's handler, for its requester. */
    if (getenv("SWOS_CRASH_REQUESTER"))
        ((void (*)(void))pc)();
    Platform_Shutdown();
    exit(20);
}

void Platform_InstallCrashTrap(void)
{
    struct Task *me = FindTask(NULL);
    s_old_trapcode = me->tc_TrapCode;
    me->tc_TrapCode = (APTR)rtg_trap_handler;
}

#endif

/* ---- The clock ------------------------------------------------------ */

static uint64_t now_us(void)
{
    return RtgClock_NowUs();
}

uint64_t Platform_NowMs(void)
{
    return now_us() / 1000u;
}

/* ---- The Amiga's VBlank interrupt ------------------------------------ */

static uint64_t s_last_isr_ms;
static uint64_t s_next_isr_us;
static uint32_t s_isr_count, s_present_count;

/* The Amiga's VBlank interrupt at the rate the game is paced at, by DEADLINE:
 * as many ticks as are due since the last call, at most two (a stall is not made
 * up in a burst). platform_sdl.c throttles instead -- no tick within 18 ms of the
 * last -- which its timer thread, calling every 20 ms, makes exact; called from the
 * frame loop, whose frames jitter around 20 ms, every short gap lost a tick and the
 * game's clock ran at 40 Hz (measured, 2026-10-22): the whole game at 80% speed.
 * One thread here, so no lock. */
void RunInt3HandlerThrottled(void)
{
    const uint64_t period = frame_period_us();
    uint64_t now = now_us();
    int ticks = 0;

    if (s_locked) {
        /* The frames follow the display: one tick each, so a refresh a hair off
         * 50 Hz never shows a frame twice or skips one against the timer. */
        ticks = 1;
        s_next_isr_us = now + period;
    } else if (s_next_isr_us == 0)
        s_next_isr_us = now;
    while (now >= s_next_isr_us && ticks < 2) {
        s_next_isr_us += period;
        ticks++;
    }
    if (now >= s_next_isr_us)
        s_next_isr_us = now + period;       /* far behind: the schedule starts again */
    if (ticks == 0)
        return;
    s_last_isr_ms = now / 1000u;
    while (ticks-- > 0) {
        _Int3TimerHandler();
        /* A heartbeat: alive, and whether frames are being shown or something
         * waits on the clock alone (a fade, a delay). */
        if (++s_isr_count % 500 == 0)
            fprintf(stderr, "[wos] alive: %u interrupt ticks, %u frames shown, clock %lu ms\n",
                    (unsigned)s_isr_count, (unsigned)s_present_count,
                    (unsigned long)(now / 1000u));
    }
}

/* A wait that never leaves the PPC on WarpOS (rtg_clock.c), in microseconds. */
void Platform_SleepUs(uint32_t us)
{
    RtgClock_WaitUs(us);
}

/* No timer thread to quiesce or resume. */
void Platform_QuiesceTimerThread(void) { }
void Platform_ResumeTimerThread(void)  { }

/* ---- Choosing a mode ------------------------------------------------- */

static int rtg_mode_ok(ULONG id);

/* An 8-bit RTG mode near w x h with room for the picture, or INVALID_ID. */
static ULONG rtg_mode(int w, int h)
{
    struct TagItem tags[4];
    ULONG id;

    tags[0].ti_Tag = CYBRBIDTG_NominalWidth;  tags[0].ti_Data = (ULONG)w;
    tags[1].ti_Tag = CYBRBIDTG_NominalHeight; tags[1].ti_Data = (ULONG)h;
    tags[2].ti_Tag = CYBRBIDTG_Depth;         tags[2].ti_Data = 8UL;
    tags[3].ti_Tag = TAG_END;                 tags[3].ti_Data = 0UL;
    id = BestCModeIDTagList(tags);
    return rtg_mode_ok(id) ? id : (ULONG)INVALID_ID;
}

/* An 8-bit RTG mode with room for the picture. BestCModeIDTagList answers a NATIVE
 * chipset mode when nothing matches, and a mode from SWOS_SCREENMODE or the
 * requester can be anything: every one is checked here. */
static int rtg_mode_ok(ULONG id)
{
    if (id == (ULONG)INVALID_ID)                       return 0;
    if (!IsCyberModeID(id))                            return 0;
    if (GetCyberIDAttr(CYBRIDATTR_DEPTH, id) != 8UL)   return 0;
    if (GetCyberIDAttr(CYBRIDATTR_BPPIX, id) != 1UL)   return 0;
    if ((int)GetCyberIDAttr(CYBRIDATTR_WIDTH, id)  < WOS_PICTURE_W ||
        (int)GetCyberIDAttr(CYBRIDATTR_HEIGHT, id) < WOS_PICTURE_H)
        return 0;
    return 1;
}


/* The mode ladder: the size SWOS.ini asks for ([video] resolution), then the
 * picture's own size -- a 336x272 or 352x288 mode a user made in Picasso96Mode
 * shows it 1:1 and full screen -- then the common sizes it fits in, centred.
 * Every step down says why. */
/* The ASL screen-mode requester on the Workbench screen, 8-bit modes with room for
 * the picture. INVALID_ID when cancelled or unusable. */
static ULONG asl_pick_mode(void)
{
    struct ScreenModeRequester *req;
    ULONG id = (ULONG)INVALID_ID;

    AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 38);
    if (!AslBase) {
        fprintf(stderr, "[video] no asl.library v38: no screen-mode requester\n");
        return id;
    }
    req = (struct ScreenModeRequester *)AllocAslRequestTags(ASL_ScreenModeRequest,
              ASLSM_TitleText, (ULONG)"OpenSWOS: screen mode (8-bit RTG)",
              ASLSM_MinWidth,  (ULONG)WOS_PICTURE_W,
              ASLSM_MinHeight, (ULONG)WOS_PICTURE_H,
              ASLSM_MinDepth,  8UL,
              ASLSM_MaxDepth,  8UL,
              TAG_END);
    if (req) {
        if (AslRequestTags(req, TAG_END))
            id = req->sm_DisplayID;
        FreeAslRequest(req);
    }
    CloseLibrary(AslBase);
    AslBase = NULL;
    return id;
}

/* Remember a mode for the next start: ENVARC:SWOS_SCREENMODE (and ENV:). */
static void save_mode(ULONG id)
{
    char buf[16];
    snprintf(buf, sizeof buf, "0x%08lx", (unsigned long)id);
    if (SetVar((CONST_STRPTR)"SWOS_SCREENMODE", (CONST_STRPTR)buf, -1,
               GVF_GLOBAL_ONLY | GVF_SAVE_VAR))
        fprintf(stderr, "[video] mode %s saved in ENVARC:SWOS_SCREENMODE\n", buf);
}

static ULONG choose_mode(const PlatformInitOptions *opts)
{
    /* SWOS_ASL=1: ask, and remember the answer. */
    if (getenv("SWOS_ASL")) {
        ULONG id = asl_pick_mode();
        if (rtg_mode_ok(id)) {
            save_mode(id);
            return id;
        }
        if (id != (ULONG)INVALID_ID)
            fprintf(stderr, "[video] mode $%08lx from the requester is not an 8-bit RTG mode "
                            "with room for %dx%d\n", (unsigned long)id, WOS_PICTURE_W, WOS_PICTURE_H);
    }
    /* SWOS_SCREENMODE=0x...: that mode (the requester writes it). */
    {
        const char *v = getenv("SWOS_SCREENMODE");
        if (v && v[0]) {
            ULONG id = (ULONG)strtoul(v, NULL, 0);
            if (rtg_mode_ok(id))
                return id;
            fprintf(stderr, "[video] SWOS_SCREENMODE %s is not an 8-bit RTG mode with room "
                            "for %dx%d: the usual choice instead\n", v, WOS_PICTURE_W, WOS_PICTURE_H);
        }
    }
    static const int ladder[][2] = {
        { 336, 272 }, { 352, 288 }, { 400, 300 }, { 640, 480 }, { 640, 512 }, { 800, 600 },
    };
    ULONG id;
    int i;

    if (opts && opts->res_w > 0 && opts->res_h > 0) {
        id = rtg_mode(opts->res_w, opts->res_h);
        if (id != (ULONG)INVALID_ID)
            return id;
        fprintf(stderr, "[video] no 8-bit RTG mode at the %dx%d SWOS.ini asks for "
                        "(at least %dx%d)\n", opts->res_w, opts->res_h,
                WOS_PICTURE_W, WOS_PICTURE_H);
    }
    for (i = 0; i < (int)(sizeof ladder / sizeof ladder[0]); i++) {
        id = rtg_mode(ladder[i][0], ladder[i][1]);
        if (id != (ULONG)INVALID_ID)
            return id;
        fprintf(stderr, "[video] no 8-bit RTG mode for %dx%d\n", ladder[i][0], ladder[i][1]);
    }
    return (ULONG)INVALID_ID;
}

/* Take the lock once and look at what it gives, instead of trusting the mode. */
static void probe_lock(void)
{
    APTR  handle, base = NULL;
    ULONG bpr = 0, depth = 0, pixfmt = (ULONG)~0;
    struct TagItem t[5];

    t[0].ti_Tag = LBMI_BASEADDRESS; t[0].ti_Data = (ULONG)&base;
    t[1].ti_Tag = LBMI_BYTESPERROW; t[1].ti_Data = (ULONG)&bpr;
    t[2].ti_Tag = LBMI_DEPTH;       t[2].ti_Data = (ULONG)&depth;
    t[3].ti_Tag = LBMI_PIXFMT;      t[3].ti_Data = (ULONG)&pixfmt;
    t[4].ti_Tag = TAG_DONE;         t[4].ti_Data = 0UL;

    handle = LockBitMapTagList((APTR)s_screen->RastPort.BitMap, t);
    if (handle != NULL)
        UnLockBitMap(handle);   /* nothing at all in between */

    s_use_lock = (handle != NULL && base != NULL && depth == 8UL &&
                  pixfmt == PIXFMT_LUT8 && bpr >= (ULONG)s_mode_w);
    /* SWOS_NO_LOCK=1: WriteLUTPixelArray even when the lock would do, to compare
     * the two on a driver whose lock is slow. */
    if (getenv("SWOS_NO_LOCK"))
        s_use_lock = 0;
    fprintf(stderr, "[video] frames go %s\n",
            s_use_lock ? "straight into the card's memory (LockBitMapTagList)"
                       : "through WriteLUTPixelArray (the lock gave no 8-bit surface)");
}

/* ---- Lifecycle ------------------------------------------------------- */

/* SWOS_PRESENT_STATS=1: the machine's own memory speeds, once, so a frame's costs
 * can be read against them (fast RAM write, read, copy; 256 KB, past the caches). */
static void memory_bench(void)
{
    enum { N = 256 * 1024 };
    uint32_t *a = (uint32_t *)malloc(N), *b = (uint32_t *)malloc(N);
    volatile uint32_t sink = 0;
    uint64_t t0, t1, t2, t3;
    uint32_t i, sum = 0;
    if (!a || !b) {
        free(a);
        free(b);
        return;
    }
    memset(b, 1, N);
    t0 = now_us();
    for (i = 0; i < N / 4; i++)
        a[i] = i;
    t1 = now_us();
    for (i = 0; i < N / 4; i++)
        sum += b[i];
    sink = sum;
    t2 = now_us();
    memcpy(a, b, N);
    t3 = now_us();
    (void)sink;
    fprintf(stderr, "[present] fast RAM: write %.1f MB/s, read %.1f MB/s, copy %.1f MB/s\n",
            N / (double)(t1 - t0 + 1), N / (double)(t2 - t1 + 1), N / (double)(t3 - t2 + 1));
    {
        /* The CPU alone, registers only: 1M passes of add/xor/shift. */
        volatile uint32_t out;
        uint32_t x = 1, y = 3;
        uint64_t c0 = now_us(), c1;
        for (i = 0; i < 1000000u; i++) {
            x += y;
            y ^= x >> 3;
        }
        c1 = now_us();
        out = x + y;
        (void)out;
        fprintf(stderr, "[present] CPU: 1M loop passes in %.2f ms\n", (c1 - c0) / 1000.0);
    }
    free(a);
    free(b);
}

int Platform_Init(const PlatformInitOptions *opts)
{
    extern void Platform_RewireCustom(void);
    extern void Platform_PreArmIsrGlobals(void);

    if (opts) {
        s_fullscreen_mode = opts->fullscreen;
        s_win_scale = opts->win_scale;
    }

    fprintf(stderr, "[wos] Platform_Init: opening libraries\n");
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase       = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    /* v41: the first with LockBitMapTagList and WriteLUTPixelArray, and what
     * Picasso96's CyberGraphX layer reports. */
    CyberGfxBase  = OpenLibrary((CONST_STRPTR)CYBERGFXNAME, 41);
    if (!IntuitionBase || !GfxBase) {
        fprintf(stderr, "[video] intuition/graphics.library v39 missing (AmigaOS 3.0+ needed)\n");
        return -1;
    }
    if (!CyberGfxBase) {
        fprintf(stderr, "[video] no cybergraphics.library v41: this build needs an RTG "
                        "card (Picasso96 or CyberGraphX)\n");
        return -1;
    }

    fprintf(stderr, "[wos] choosing a mode\n");
    s_mode_id = choose_mode(opts);
    if (s_mode_id == (ULONG)INVALID_ID) {
        fprintf(stderr, "[video] no 8-bit RTG mode with room for %dx%d: add one in "
                        "Picasso96Mode (336x272 shows the game 1:1)\n",
                WOS_PICTURE_W, WOS_PICTURE_H);
        return -1;
    }
    s_mode_w = (int)GetCyberIDAttr(CYBRIDATTR_WIDTH, s_mode_id);
    s_mode_h = (int)GetCyberIDAttr(CYBRIDATTR_HEIGHT, s_mode_id);

    fprintf(stderr, "[wos] opening the screen, mode $%08lx %dx%d\n", (unsigned long)s_mode_id, s_mode_w, s_mode_h);
    /* NO SA_BitMap: the card owns the display memory. */
    s_screen = OpenScreenTags(NULL,
                              SA_Width,     (ULONG)s_mode_w,
                              SA_Height,    (ULONG)s_mode_h,
                              SA_Depth,     8UL,
                              SA_Type,      (ULONG)CUSTOMSCREEN,
                              SA_Quiet,     TRUE,
                              SA_ShowTitle, FALSE,
                              SA_Title,     (ULONG)"OpenSWOS",
                              SA_DisplayID, s_mode_id,
                              TAG_END);
    if (!s_screen) {
        fprintf(stderr, "[video] OpenScreen %dx%d (mode $%08lx) failed\n",
                s_mode_w, s_mode_h, (unsigned long)s_mode_id);
        return -1;
    }
    fprintf(stderr, "[video] RTG screen %dx%d, mode asked $%08lx, granted $%08lx; "
                    "the picture %dx%d centred\n",
            s_mode_w, s_mode_h, (unsigned long)s_mode_id,
            (unsigned long)GetVPModeID(&s_screen->ViewPort), _screen_width, _screen_height);

    fprintf(stderr, "[wos] opening the window\n");
    /* A borderless backdrop window, for the keyboard: Intuition has no other way
     * to read it. It never draws. */
    s_window = OpenWindowTags(NULL,
                              WA_CustomScreen, (ULONG)s_screen,
                              WA_Left, 0UL, WA_Top, 0UL,
                              WA_Width, (ULONG)s_mode_w, WA_Height, (ULONG)s_mode_h,
                              WA_Flags, (ULONG)(WFLG_BACKDROP | WFLG_BORDERLESS |
                                                WFLG_RMBTRAP | WFLG_ACTIVATE |
                                                WFLG_NOCAREREFRESH),
                              WA_IDCMP, (ULONG)IDCMP_RAWKEY,
                              TAG_END);
    if (!s_window) {
        fprintf(stderr, "[video] OpenWindow failed\n");
        return -1;
    }
    ActivateWindow(s_window);   /* WFLG_ACTIVATE alone is not reliable */
    ScreenToFront(s_screen);    /* and says nothing about which SCREEN shows */

    fprintf(stderr, "[wos] blanking the pointer\n");
    /* No pointer over the game: a sprite of zeros, in Chip RAM. */
    s_blank_pointer = (UWORD *)AllocVec(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR);
    if (s_blank_pointer)
        SetPointer(s_window, s_blank_pointer, 1, 16, 0, 0);

    fprintf(stderr, "[wos] clearing the screen, probing the lock\n");
    SetRast(&s_screen->RastPort, 0);    /* the border is colour 0, as on the Amiga */
    probe_lock();
    if (getenv("SWOS_VSYNC_PROBE"))
        vsync_probe();
    dbuf_open();
    if (!s_dbuf)
        vblank_probe();

    fprintf(stderr, "[wos] input, audio\n");
    Platform_RewireCustom();
    Platform_PreArmIsrGlobals();
    InputWos_Init();
    (void)AudioSDL_Init(opts ? opts->audio_mono : 1);
    s_last_isr_ms = Platform_NowMs();
    if (getenv("SWOS_PRESENT_STATS"))
        memory_bench();
    fprintf(stderr, "[wos] Platform_Init done\n");
    return 0;
}

void Platform_Shutdown(void)
{
    AudioSDL_Shutdown();
    InputWos_Shutdown();
    dbuf_close();
    if (s_window) {
        if (s_blank_pointer)
            ClearPointer(s_window);
        CloseWindow(s_window);
        s_window = NULL;
    }
    if (s_screen) {
        CloseScreen(s_screen);
        s_screen = NULL;
    }
    if (s_blank_pointer) {
        FreeVec(s_blank_pointer);
        s_blank_pointer = NULL;
    }
    if (CyberGfxBase)  { CloseLibrary(CyberGfxBase); CyberGfxBase = NULL; }
    if (GfxBase)       { CloseLibrary((struct Library *)GfxBase); GfxBase = NULL; }
    if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
}

/* ---- Showing a frame ------------------------------------------------- */

static void load_palette(void)
{
    static ULONG table[1 + 256 * 3 + 1];
    int i;

    Platform_FramePalette(s_rgb);
    if (s_palette_loaded && memcmp(s_rgb, s_rgb_loaded, sizeof s_rgb) == 0)
        return;                 /* unchanged: no 68k call this frame */
    table[0] = (256UL << 16) | 0UL;
    for (i = 0; i < 256; i++) {
        ULONG r = s_rgb[i * 3 + 0], g = s_rgb[i * 3 + 1], b = s_rgb[i * 3 + 2];
        table[1 + i * 3 + 0] = r * 0x01010101UL;
        table[1 + i * 3 + 1] = g * 0x01010101UL;
        table[1 + i * 3 + 2] = b * 0x01010101UL;
        s_ctable[i] = (r << 16) | (g << 8) | b;
    }
    table[1 + 256 * 3] = 0UL;
    LoadRGB32(&s_screen->ViewPort, table);
    s_st_palette++;
    memcpy(s_rgb_loaded, s_rgb, sizeof s_rgb);
    s_palette_loaded = 1;
}

/* Wait until buffer b is off screen: Intuition replies its SafeMessage then --
 * polled, never blocked on: the Picasso96 driver on the Sonnet sent none at all,
 * and waiting for it froze the game on its second frame (2026-10-22). A flip shows
 * at the next vertical blank, so 25 ms without the message means the buffer is off
 * screen anyway; a driver that sends none is not waited for again. */
static void wait_safe(int b)
{
    uint64_t start;
    if (!s_safe_pending[b])
        return;
    if (s_no_safe_msgs) {
        s_safe_pending[b] = 0;
        return;
    }
    start = now_us();
    while (s_safe_pending[b]) {
        struct Message *m;
        while ((m = GetMsg(s_safe_port)) != NULL) {
            int i;
            for (i = 0; i < 2; i++)
                if (m == &s_sb[i]->sb_DBufInfo->dbi_SafeMessage)
                    s_safe_pending[i] = 0;
        }
        if (!s_safe_pending[b])
            break;
        if (now_us() - start > 25000u) {
            s_safe_pending[0] = s_safe_pending[1] = 0;
            s_no_safe_msgs = 1;
            fprintf(stderr, "[video] the driver sends no SafeMessage: flipping without "
                            "waiting for it\n");
            break;
        }
        RtgClock_WaitUs(1000);
    }
}

/* SWOS_VSYNC_PROBE=1: how each way of meeting the display's vertical blank behaves
 * on this board -- the intervals between successive returns of WaitTOF, WaitBOVP
 * and back-to-back ChangeScreenBuffer flips. A wait that tracks the display
 * shows ~1e6/refresh us apart with little spread. */
static void probe_report(const char *what, uint64_t *t, int n)
{
    uint64_t mn = ~(uint64_t)0, mx = 0, sum = 0;
    int i;
    for (i = 1; i < n; i++) {
        uint64_t d = t[i] - t[i - 1];
        if (d < mn) mn = d;
        if (d > mx) mx = d;
        sum += d;
    }
    fprintf(stderr, "[vsync] %s: interval avg %lu us, min %lu, max %lu (%d calls)\n", what,
            (unsigned long)(sum / (uint64_t)(n - 1)), (unsigned long)mn, (unsigned long)mx, n);
}

static void vsync_probe(void)
{
    enum { N = 60 };
    static uint64_t t[N];
    struct ScreenBuffer *a, *b;
    int i;

    for (i = 0; i < N; i++) {
        WaitTOF();
        t[i] = now_us();
    }
    probe_report("WaitTOF", t, N);
    for (i = 0; i < N; i++) {
        WaitBOVP(&s_screen->ViewPort);
        t[i] = now_us();
    }
    probe_report("WaitBOVP", t, N);

    a = AllocScreenBuffer(s_screen, NULL, SB_SCREEN_BITMAP);
    b = AllocScreenBuffer(s_screen, NULL, SB_COPY_BITMAP);
    if (a && b) {
        int busy = 0;
        a->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = NULL;
        a->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = NULL;
        b->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = NULL;
        b->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = NULL;
        for (i = 0; i < N; i++) {
            if (!ChangeScreenBuffer(s_screen, (i & 1) ? a : b))
                busy++;
            t[i] = now_us();
        }
        probe_report("ChangeScreenBuffer", t, N);
        if (busy)
            fprintf(stderr, "[vsync] ChangeScreenBuffer refused %d of %d flips\n", busy, N);
        /* the screen's own bitmap back on screen before they go */
        while (!ChangeScreenBuffer(s_screen, a))
            RtgClock_WaitUs(2000);
        RtgClock_WaitUs(50000);
    } else {
        fprintf(stderr, "[vsync] no screen buffers\n");
    }
    if (b) FreeScreenBuffer(s_screen, b);
    if (a) FreeScreenBuffer(s_screen, a);
}

static void dbuf_open(void)
{
    int i;
    /* SWOS_NO_DBUF=1: one buffer, drawn while it is on screen, paced by the timer. */
    if (getenv("SWOS_NO_DBUF"))
        return;
    s_sb[0] = AllocScreenBuffer(s_screen, NULL, SB_SCREEN_BITMAP);
    s_sb[1] = AllocScreenBuffer(s_screen, NULL, SB_COPY_BITMAP);
    s_safe_port = CreateMsgPort();
    if (!s_sb[0] || !s_sb[1] || !s_safe_port) {
        if (s_sb[1]) FreeScreenBuffer(s_screen, s_sb[1]);
        if (s_sb[0]) FreeScreenBuffer(s_screen, s_sb[0]);
        if (s_safe_port) DeleteMsgPort(s_safe_port);
        s_sb[0] = s_sb[1] = NULL;
        s_safe_port = NULL;
        fprintf(stderr, "[video] no screen buffers from the driver: one buffer, "
                        "drawn while it is on screen\n");
        return;
    }
    /* No SafeMessage asked for, and none waited for: Picasso96 replies it a refresh
     * or more after the flip, and waiting for it every frame halved the frame rate
     * (2026-10-22). A flip shows at the next vertical blank -- at most a refresh,
     * 10 ms at 100 Hz, 16.7 at 60 -- and a buffer is drawn into again only a frame
     * (20 ms) after it was flipped away from, by when it is off screen. */
    for (i = 0; i < 2; i++) {
        s_sb[i]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = NULL;
        s_sb[i]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = NULL;
    }
    s_no_safe_msgs = 1;

    /* ChangeScreenBuffer returns at the board's vertical blank -- the only wait on
     * the RTG display here: WaitTOF and WaitBOVP follow the native chipset
     * (SWOS_VSYNC_PROBE, 2026-10-22). Time a run of flips between the two (blank)
     * buffers for the refresh. When it is the game's 50 Hz the flips pace the
     * frames: no tear, no beat against the timer. Otherwise (60, 75, 100 Hz) the
     * timer paces and one buffer is drawn while on screen -- the flip and the timer
     * waiting one after the other halved the frame rate (2026-10-22). */
    {
        /* 100 flips, two seconds of a black screen. Each return is a vertical blank
         * plus however long the driver took to come back, which varies by a
         * millisecond or more: 14 flips left the period ~60 us out, and the frames
         * drifted across the refresh -- a tear passing every few seconds
         * (2026-10-22). A line fitted through all 100, each numbered by the
         * refreshes since the first, puts the period within a few us and the blank
         * at the line, not at one late return. */
        enum { N = 100 };
        static uint64_t t[N];
        static int64_t  kk[N];
        uint64_t d[N - 1], x, med;
        int j, k;
        for (i = 0; i < N; i++) {
            ChangeScreenBuffer(s_screen, s_sb[(i + 1) & 1]);
            t[i] = now_us();
        }
        while (!ChangeScreenBuffer(s_screen, s_sb[0]))      /* its own bitmap again */
            RtgClock_WaitUs(2000);
        RtgClock_WaitUs(30000);
        /* the median interval: a missed blank or a late return does not move it */
        for (j = 0; j < N - 1; j++)
            d[j] = t[j + 1] - t[j];
        for (j = 1; j < N - 1; j++)
            for (k = j; k > 0 && d[k] < d[k - 1]; k--)
                x = d[k], d[k] = d[k - 1], d[k - 1] = x;
        med = d[(N - 1) / 2];
        /* each flip's refresh number, then the least-squares line t = a + b*k */
        kk[0] = 0;
        for (j = 1; j < N; j++) {
            int64_t r = (int64_t)((t[j] - t[j - 1] + med / 2) / med);
            kk[j] = kk[j - 1] + (r < 1 ? 1 : r);
        }
        {
            double sk = 0, st = 0, skk = 0, skt = 0, b, a, n = (double)N;
            for (j = 0; j < N; j++) {
                double kj = (double)kk[j], tj = (double)(t[j] - t[0]);
                sk += kj; st += tj; skk += kj * kj; skt += kj * tj;
            }
            b = (n * skt - sk * st) / (n * skk - sk * sk);
            a = (st - b * sk) / n;
            /* The line runs through the earliest returns' side too: a return is
             * never early, so shift it down to the soonest one -- that is nearest
             * the blank itself. */
            {
                double lo = 0;
                for (j = 0; j < N; j++) {
                    double r = (double)(t[j] - t[0]) - (a + b * (double)kk[j]);
                    if (j == 0 || r < lo)
                        lo = r;
                }
                a += lo;
            }
            s_disp_period_us = (uint64_t)(b + 0.5);
            s_disp_period_frac = b;
            s_vblank_us = t[0] + (uint64_t)(a + b * (double)kk[N - 1]);
        }
    }
    /* What one flip costs on its own, at different points of the refresh: a flip
     * that only waits for the vertical blank costs anything from 0 to a refresh,
     * one the driver does in software (copying the bitmap) always costs the copy.
     * Measured here (Sonnet, 640x480, 2026-10-22): every flip 21-36 ms, even one
     * every five seconds in a menu -- double buffering then costs a frame. */
    {
        uint64_t mn = ~(uint64_t)0, t0;
        int j;
        for (j = 0; j < 8; j++) {
            RtgClock_WaitUs(3000 + 2300 * (ULONG)j);
            t0 = now_us();
            ChangeScreenBuffer(s_screen, s_sb[(j + 1) & 1]);
            t0 = now_us() - t0;
            if (t0 < mn)
                mn = t0;
        }
        while (!ChangeScreenBuffer(s_screen, s_sb[0]))
            RtgClock_WaitUs(2000);
        RtgClock_WaitUs(30000);
        s_flip_cost_us = mn;
        fprintf(stderr, "[video] screen buffers at $%08lx and $%08lx (the screen's $%08lx); "
                        "the cheapest of 8 flips %lu us\n",
                (unsigned long)bitmap_base(s_sb[0]->sb_BitMap),
                (unsigned long)bitmap_base(s_sb[1]->sb_BitMap),
                (unsigned long)bitmap_base(s_screen->RastPort.BitMap), (unsigned long)mn);
    }
    if (!getenv("SWOS_DBUF") && (s_flip_cost_us > 4000 || !display_paces(1000000u / 50u))) {
        FreeScreenBuffer(s_screen, s_sb[1]);
        FreeScreenBuffer(s_screen, s_sb[0]);
        DeleteMsgPort(s_safe_port);
        s_sb[0] = s_sb[1] = NULL;
        s_safe_port = NULL;
        if (s_flip_cost_us > 4000)
            fprintf(stderr, "[video] a flip costs %lu us even off the vertical blank (the "
                            "driver copies?): one buffer, paced by the timer\n",
                    (unsigned long)s_flip_cost_us);
        else
            fprintf(stderr, "[video] display refresh %lu us (%.1f Hz), not the game's 50: one "
                            "buffer, paced by the timer (a 50 or 100 Hz mode looks smoothest)\n",
                    (unsigned long)s_disp_period_us, 1e6 / (double)s_disp_period_us);
        return;
    }
    s_dbuf = 1;
    s_front = 0;
    s_back = 1;
    fprintf(stderr, "[video] double buffered, display refresh %lu us (%.1f Hz): frames paced "
                    "by the flip\n", (unsigned long)s_disp_period_us,
            1e6 / (double)s_disp_period_us);
}

static void dbuf_close(void)
{
    if (!s_dbuf)
        return;
    /* The screen's own bitmap back on screen before anything is freed. */
    if (s_front != 0) {
        wait_safe(0);
        while (!ChangeScreenBuffer(s_screen, s_sb[0]))
            RtgClock_WaitUs(2000);
        s_safe_pending[1] = 1;
        s_front = 0;
    }
    wait_safe(0);
    wait_safe(1);
    FreeScreenBuffer(s_screen, s_sb[1]);
    FreeScreenBuffer(s_screen, s_sb[0]);
    DeleteMsgPort(s_safe_port);
    s_sb[0] = s_sb[1] = NULL;
    s_safe_port = NULL;
    s_dbuf = 0;
}

/* 1 when two rows are the same. The change detection compares every row of every
 * frame; libnix's memcmp is an inline byte loop. Rows are longword multiples
 * (336, 352, 384 ...) and both buffers longword-aligned, so longwords it is, with
 * a byte tail for any other width. */
static int rows_equal(const uint8_t *a, const uint8_t *b, int n)
{
    const uint32_t *la = (const uint32_t *)a, *lb = (const uint32_t *)b;
    int q;
    if ((((uintptr_t)a | (uintptr_t)b) & 3) != 0)
        return memcmp(a, b, (size_t)n) == 0;
    for (q = n >> 2; q; q--)
        if (*la++ != *lb++)
            return 0;
    a = (const uint8_t *)la;
    b = (const uint8_t *)lb;
    for (q = n & 3; q; q--)
        if (*a++ != *b++)
            return 0;
    return 1;
}

/* The picture's rows into the screen at (dx, dy), under the lock. Returns 0 when
 * the lock gave nothing usable: the caller demotes to WriteLUTPixelArray. */
static int blit_locked(struct BitMap *bm, const uint8_t *fb, int stride, int dx, int dy, int w, int h)
{
    APTR  handle, base = NULL;
    ULONG bpr = 0, depth = 0, pixfmt = (ULONG)~0;
    struct TagItem t[5];
    int   usable, row;

    t[0].ti_Tag = LBMI_BASEADDRESS; t[0].ti_Data = (ULONG)&base;
    t[1].ti_Tag = LBMI_BYTESPERROW; t[1].ti_Data = (ULONG)&bpr;
    t[2].ti_Tag = LBMI_DEPTH;       t[2].ti_Data = (ULONG)&depth;
    t[3].ti_Tag = LBMI_PIXFMT;      t[3].ti_Data = (ULONG)&pixfmt;
    t[4].ti_Tag = TAG_DONE;         t[4].ti_Data = 0UL;

    uint64_t tl0 = 0, tl1 = 0, tl2 = 0;
    if (s_present_stats > 0)
        tl0 = now_us();
    handle = LockBitMapTagList((APTR)bm, t);
    if (handle == NULL)
        return 0;
    /* ---- LOCK HELD: the copy only, no OS calls, no early return. (The E-clock
     * reads for SWOS_PRESENT_STATS are timer.device's, not the graphics system's.) ---- */
    if (s_present_stats > 0)
        tl1 = now_us();
    usable = (base != NULL && depth == 8UL && pixfmt == PIXFMT_LUT8 &&
              bpr >= (ULONG)(dx + w));
    if (usable) {
        const uint8_t *src = fb;
        uint8_t *dst = (uint8_t *)base + (ULONG)dy * bpr + dx;
        for (row = 0; row < h; row++) {
            memcpy(dst, src, (size_t)w);
            src += stride;
            dst += bpr;         /* the CARD's stride, not ours */
        }
    }
    if (s_present_stats > 0)
        tl2 = now_us();
    UnLockBitMap(handle);
    /* ---- LOCK RELEASED ---- */
    if (s_present_stats > 0) {
        uint64_t tl3 = now_us();
        s_st_lock += tl1 - tl0;
        s_st_copy += tl2 - tl1;
        s_st_unlock += tl3 - tl2;
    }
    return usable;
}

/* ---- Scrolling on the card -------------------------------------------------
 * What crosses to the card is the cost on a 68060: its writes go through the
 * Zorro III -> Prometheus aperture at ~6 MB/s (the Radeon9200 driver's measured
 * ceiling, p96-driver docs/04-performance.md), so a scrolling match frame
 * (60-90 KB) took 11-17 ms of the 20. The PPC writes the card over PCI at ~30 MB/s
 * and never needed this.
 *
 * So the card moves what it already shows: when the camera moved by (ddx, ddy),
 * one BltBitMap shifts the picture on the card (the driver's BlitRect, on the GPU),
 * and only what then still differs crosses the bus -- the strip the scroll
 * uncovered, the players, the ball, the HUD -- in 16-pixel segments, merged into
 * runs, under one lock.
 *
 * The shadow of what is on screen is kept in WORLD coordinates, wrapping (a torus
 * W x H): screen (x, y) shown at camera (cx, cy) is shadow[(y+cy) mod H][(x+cx)
 * mod W]. A scroll then moves nothing in fast RAM (which is slow too: ~28 MB/s), and
 * the shift on the card keeps "screen == shadow" true everywhere but the uncovered
 * strip, which is always sent. Every segment is compared, so the picture is right
 * whatever moved: something that does not follow the camera (the HUD, a menu) is
 * simply a difference. SWOS_NO_SCROLLBLIT=1 keeps the row-by-row path below; on
 * WarpOS it is off unless SWOS_SCROLLBLIT=1. */
int16_t Render_CameraX(void);      /* where the frame was drawn from (game_types.h) */
int16_t Render_CameraY(void);

#define SEG 16
struct Run { int16_t y, x0, x1; };
static struct Run s_runs[272 * (672 / SEG + 1)];
static int  s_tor_valid, s_tor_w, s_tor_h, s_tor_cx, s_tor_cy;
static int  s_scrollblit = -1;
static int  s_shift_probe;
static uint64_t s_shift_probe_us;

static int mod_pos(int a, int m)
{
    a %= m;
    return a < 0 ? a + m : a;
}

/* Marks in dirty[] every SEG-pixel segment of fb's [x0, x0+n) that differs from
 * the shadow bytes s[0, n). A tight longword loop, 16 bytes a pass -- this compares
 * every pixel of every frame, and a call per segment cost more than the memory
 * (~20 ms a frame on a 68060 against ~5 ms of reading) -- which, on a difference,
 * marks that segment and resumes at the next. The shadow side is any alignment
 * (the torus wraps at any column).
 *
 * A longword at any address: the 68060 (and the PPC) read one misaligned in
 * hardware, and through this type gcc emits a plain move.l/cmp.l -- __builtin_memcpy
 * went through a stack temporary, ten instructions a longword. */
typedef uint32_t __attribute__((aligned(1), __may_alias__)) u32_any;

static void diff_span(const uint8_t *f, const uint8_t *s, int x0, int n, uint8_t *dirty)
{
    int i = 0;
    while (i + 16 <= n) {
        const u32_any *a = (const u32_any *)(f + i), *b = (const u32_any *)(s + i);
        if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]) {
            i += 16;
            continue;
        }
        break;                              /* a difference in these 16: find it below */
    }
    while (i + 4 <= n) {
        if (*(const u32_any *)(f + i) != *(const u32_any *)(s + i)) {
            int seg;
            while (f[i] == s[i])            /* the byte that differs: a longword can */
                i++;                        /* straddle two segments */
            seg = (x0 + i) / SEG;
            dirty[seg] = 1;
            i = (seg + 1) * SEG - x0;       /* the next segment: this one is dirty already */
            while (i + 16 <= n) {           /* and back to 16 at a time while it matches */
                const u32_any *a = (const u32_any *)(f + i), *b = (const u32_any *)(s + i);
                if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3])
                    break;
                i += 16;
            }
            continue;
        }
        i += 4;
    }
    for (; i < n; i++)
        if (f[i] != s[i])
            dirty[(x0 + i) / SEG] = 1;
}

/* n bytes, longwords first, inline: the shadow takes ~900 small pieces a frame, and a
 * memcpy call each (a byte loop under 16) was ~2 ms of it. */
static inline void put_bytes(uint8_t *d, const uint8_t *f, int n)
{
    for (; n >= 4; n -= 4, d += 4, f += 4)
        *(u32_any *)d = *(const u32_any *)f;
    while (n-- > 0)
        *d++ = *f++;
}

static void seg_store(const uint8_t *f, uint8_t *srow, int c, int n, int W)
{
    if (c + n <= W) {
        put_bytes(srow + c, f, n);
    } else {
        put_bytes(srow + c, f, W - c);
        put_bytes(srow, f + (W - c), n - (W - c));
    }
}

/* Comparing every pixel of every frame was still ~10 ms on a 68060 (two whole
 * frames read at ~40 MB/s, plus the loop). In a match nearly all of the frame is the
 * pitch, which the shift on the card has already put right; what can differ is
 * where something was drawn on top of it -- this frame (Render_MarkRect: every
 * sprite, glyph, filled rectangle, crowd tile, full-frame copy) and the frame before
 * (what it drew is still on the card, shifted) -- and the strip the shift uncovered.
 * So in a match those are SENT, in 4-pixel units,
 * without comparing and without keeping the shadow: on this machine's memory a
 * 16-byte cache line missed costs ~0.4 us, and comparing 10,000 pixels in small
 * rectangles took ~5 ms, sorting them into intervals ~2 ms and keeping the shadow
 * ~2 ms more, where sending them takes ~2.
 *
 * Because something could still draw without marking, the card itself is the check:
 * VERIFY_ROWS rows are read back after every upload and held against the frame, the
 * whole picture every ~2.7 s. A difference sends everything again and compares
 * whole frames for FULL_FRAMES frames. Outside a match every frame is compared. */
#define MARK_PAD     0                           /* the marks are the blits' own rectangles */
#define FULL_FRAMES  50
#define UNITS_WORDS  6                           /* 4-pixel units, 32 a word: 672 wide */
static uint32_t s_bits[272][UNITS_WORDS];       /* per row: the 4-pixel units to send */
static uint8_t  s_row_any[272];
static int      s_shadow_ok;                    /* the shadow is what the card shows */
static int      s_vrow;                         /* the next rows read back from the card */
#define VERIFY_ROWS  2                           /* read over Zorro: ~0.1 ms a row */
static uint32_t s_st_verify_fail;
static uint64_t s_st_p_ensure, s_st_p_bits, s_st_p_runs;   /* the preparing, in parts */
static int16_t  s_prev_marks[RENDER_MAX_MARKS][4];
static int      s_prev_n, s_prev_ok, s_prev_cx, s_prev_cy;
static int      s_full_frames;
static uint32_t s_st_tracked, s_st_untracked;
static uint32_t s_st_marks, s_st_area;          /* what the tracked frames sent */

/* Rows of 4-pixel units, most significant bit first (unit u is bit 31 - u % 32 of
 * word u / 32), so a run is found with count-leading-zeros (bfffo on the 68060)
 * rather than a bit at a time: on this machine the bit loop was ~3 ms a frame.
 * Whole longwords because over the bus a byte costs what a longword does, and
 * every pixel of fb is right to send (w and the picture's left edge on the card
 * are longword multiples). */
static void unit_masks(int x0, int x1, int w, uint32_t *m, int nwords)
{
    int a, b, wd;
    if (x0 < 0) x0 = 0;
    if (x1 > w) x1 = w;
    a = x0 >> 2;
    b = x0 < x1 ? (x1 + 3) >> 2 : a;            /* exclusive */
    for (wd = 0; wd < nwords; wd++) {
        int lo = a > wd * 32 ? a : wd * 32, hi = b < wd * 32 + 32 ? b : wd * 32 + 32;
        uint32_t v = 0;
        if (lo < hi) {
            int start = lo - wd * 32, end = hi - wd * 32;
            v = 0xFFFFFFFFu >> start;
            if (end < 32)
                v &= ~(0xFFFFFFFFu >> end);
        }
        m[wd] = v;
    }
}

/* Rows [y0, y1) get the units [x0, x1) -- only the words those touch (one or two
 * for a sprite), walked by pointer: ~60 rectangles of ~20 rows a frame. */
static void bits_add_span(int x0, int x1, int y0, int y1, int w, int h)
{
    uint32_t m[UNITS_WORDS];
    uint32_t *row;
    uint8_t *any;
    int wa, wb, wd, y;
    if (x0 < 0) x0 = 0;
    if (x1 > w) x1 = w;
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;
    if (x0 >= x1 || y0 >= y1)
        return;
    unit_masks(x0, x1, w, m, UNITS_WORDS);
    wa = (x0 >> 2) >> 5;
    wb = (((x1 + 3) >> 2) - 1) >> 5;
    row = s_bits[y0];
    any = &s_row_any[y0];
    for (y = y0; y < y1; y++, row += UNITS_WORDS, any++) {
        *any = 1;
        for (wd = wa; wd <= wb; wd++)
            row[wd] |= m[wd];
    }
}

static void bits_add_rect(int x0, int y0, int x1, int y1, int w, int h)
{
    bits_add_span(x0 - MARK_PAD, x1 + MARK_PAD, y0 - MARK_PAD, y1 + MARK_PAD, w, h);
}

/* TEMP diagnostics: HELP (or the first read-back mismatch) writes RAM:SWOSSNAPn --
 * "SNP1", w, h (BE16), the palette (768), the game's frame (w*h), then what the card
 * shows (w*h) -- for a glitch to be seen off the machine. */
static int s_snap_req, s_snap_n;

static void snapshot(void)
{
    const uint8_t *fb = Platform_FrameToShow();
    int w = _screen_width, h = _screen_height, dx, dy, y;
    uint8_t *buf;
    size_t n;
    APTR handle, base = NULL;
    ULONG bpr = 0;
    struct TagItem t[3];
    char path[32];
    FILE *f;

    s_snap_req = 0;
    if (!fb || !s_screen)
        return;
    if (w > s_mode_w) w = s_mode_w;
    if (h > s_mode_h) h = s_mode_h;
    dx = (s_mode_w - w) / 2;
    dy = (s_mode_h - h) / 2;
    n = 8 + 768 + 2 * (size_t)w * h;
    buf = (uint8_t *)malloc(n);
    if (!buf)
        return;
    memcpy(buf, "SNP1", 4);
    buf[4] = (uint8_t)(w >> 8); buf[5] = (uint8_t)w;
    buf[6] = (uint8_t)(h >> 8); buf[7] = (uint8_t)h;
    memcpy(buf + 8, s_rgb_loaded, 768);
    for (y = 0; y < h; y++)
        memcpy(buf + 776 + (size_t)y * w, fb + (size_t)y * _screen_width, (size_t)w);
    memset(buf + 776 + (size_t)w * h, 0, (size_t)w * h);
    t[0].ti_Tag = LBMI_BASEADDRESS; t[0].ti_Data = (ULONG)&base;
    t[1].ti_Tag = LBMI_BYTESPERROW; t[1].ti_Data = (ULONG)&bpr;
    t[2].ti_Tag = TAG_DONE;         t[2].ti_Data = 0UL;
    handle = LockBitMapTagList((APTR)s_screen->RastPort.BitMap, t);
    if (handle) {
        if (base)
            for (y = 0; y < h; y++)
                memcpy(buf + 776 + (size_t)w * h + (size_t)y * w,
                       (uint8_t *)base + (ULONG)(dy + y) * bpr + dx, (size_t)w);
        UnLockBitMap(handle);
    }
    snprintf(path, sizeof path, "RAM:SWOSSNAP%d", s_snap_n++ % 8);
    f = fopen(path, "wb");
    if (f) {
        fwrite(buf, 1, n, f);
        fclose(f);
    }
    free(buf);
    fprintf(stderr, "[snap] %s: %dx%d, camera %d,%d\n", path, w, h,
            Render_CameraX(), Render_CameraY());
}

static void pace(void);
static int s_paced;                             /* pace() already ran this present */
static uint64_t s_next_frame_us;               /* the next frame's slot (pace) */

/* ---- The card's vertical blank ---------------------------------------------
 * The frame's slot follows the display from a vertical blank fitted once, at the
 * start, on the CPU's timer. The timer's crystal and the card's pixel clock part
 * by a few millionths: ~1 us a frame, ~4 ms a half -- enough to walk the upload
 * from ahead of the beam into the middle of the picture, where the players tore
 * between two frames (from the second half on, 2026-10-22). Where the board gives
 * Picasso96 a vertical-blank interrupt, WaitBOVP() on our screen returns at the
 * card's own blank, and the upload waits for that instead of the timer.
 *
 * Without the interrupt Picasso96 makes its vertical blanks from the system timer:
 * the right rate, any phase. So it is probed, against the blank the flips were
 * fitted to (dbuf_open): the board's blank lands where that one does, every time;
 * the timer's lands anywhere and stays there. */
static int      s_vbl_ok;
static uint32_t s_vbl_waits;                    /* stats: frames that waited on it */

static void vblank_probe(void)
{
    enum { N = 30 };
    uint64_t t[N];
    double per, off, worst = 0;
    int i;
    if (!s_disp_period_us || !s_vblank_us || getenv("SWOS_NO_VBLANK_WAIT")) {
        fprintf(stderr, "[video] the vertical blank: not used\n");
        return;
    }
    WaitBOVP(&s_screen->ViewPort);
    for (i = 0; i < N; i++) {
        WaitBOVP(&s_screen->ViewPort);
        t[i] = now_us();
    }
    per = (double)(t[N - 1] - t[0]) / (double)(N - 1);
    /* each return against the fitted blank, in [-period/2, period/2) */
    for (i = 0; i < N; i++) {
        double d = (double)((int64_t)(t[i] - s_vblank_us));
        d -= s_disp_period_frac * (double)(int64_t)(d / s_disp_period_frac);
        if (d >= s_disp_period_frac / 2) d -= s_disp_period_frac;
        if (d < -s_disp_period_frac / 2) d += s_disp_period_frac;
        if (d < 0) d = -d;
        if (d > worst) worst = d;
    }
    off = per - s_disp_period_frac;
    if (off < 0) off = -off;
    s_vbl_ok = off < s_disp_period_frac * 0.002 && worst < 1500.0;
    fprintf(stderr, "[video] the vertical blank: WaitBOVP every %.1f us (the display %.1f), "
                    "at most %.0f us from the fitted blank: %s\n", per, s_disp_period_frac,
            worst, s_vbl_ok ? "the board's -- the upload waits for it"
                            : "not the board's (no interrupt?) -- the timer paces");
}

/* In place of pace() before the upload: the card's blank, then the upload ahead
 * of the beam. The slot is re-phased from it every frame. */
static int vblank_pace(void)
{
    uint64_t now;
    if (!s_vbl_ok)
        return 0;
    WaitBOVP(&s_screen->ViewPort);
    now = now_us();
    if (s_next_frame_us && now > s_next_frame_us + s_disp_period_us + s_disp_period_us / 2)
        s_st_late++;
    s_next_frame_us = now;
    s_vbl_waits++;
    return 1;
}

/* 1 when the frame went up this way; 0: use the row path (the lock gave no 8-bit
 * surface). */
static int present_scrolled(const uint8_t *fb, int w, int h, int dx, int dy)
{
    struct BitMap *bm = s_screen->RastPort.BitMap;
    uint8_t *sh = s_shadow[0];
    int cx = Render_CameraX(), cy = Render_CameraY();
    int ddx = cx - s_tor_cx, ddy = cy - s_tor_cy;
    int full = !s_tor_valid || w != s_tor_w || h != s_tor_h ||
               ddx >= w || -ddx >= w || ddy >= h || -ddy >= h;
    int nruns = 0, y;
    int nseg = (w + SEG - 1) / SEG;
    uint8_t dirty[672 / SEG + 1];
    /* Columns and rows the shift uncovers: always sent. */
    int ex0 = 0, ex1 = 0, ey0 = 0, ey1 = 0;

    uint64_t tc1 = s_present_stats > 0 ? now_us() : 0;
    const int16_t (*marks)[4];
    int over, nm = Render_Marks(&marks, &over), i;
    int ncr = (h + SEG - 1) / SEG;
    int c0 = mod_pos(cx, w);
    int tracked;

    if (!full) {
        if (ddx > 0) { ex0 = w - ddx; ex1 = w; } else if (ddx < 0) { ex0 = 0; ex1 = -ddx; }
        if (ddy > 0) { ey0 = h - ddy; ey1 = h; } else if (ddy < 0) { ey0 = 0; ey1 = -ddy; }
    }
    {
        extern volatile uint16_t _playGame;
        tracked = !full && _playGame && !over && s_prev_ok && s_full_frames == 0 &&
                  h <= (int)(sizeof s_row_any) && w <= UNITS_WORDS * 32 * 4;
    }
    uint64_t tq0 = s_present_stats > 0 ? now_us() : 0;
    uint64_t tq1 = tq0, tq2 = tq0;
    if (tracked) {
        /* What to send: this frame's marks, last frame's where the shift put them
         * (the pitch goes back there), and what the shift uncovers. Sent blind: on
         * this machine comparing them cost more than sending (above). The shadow is
         * not kept meanwhile; what checks the picture is the card itself, read back
         * after the upload (VERIFY_ROWS below). */
        int nwords = (w / 4 + 31) / 32;
        for (y = 0; y < h; y++)
            if (s_row_any[y]) {
                int wd;
                for (wd = 0; wd < nwords; wd++)
                    s_bits[y][wd] = 0;
                s_row_any[y] = 0;
            }
        for (i = 0; i < nm; i++)
            bits_add_rect(marks[i][0], marks[i][1], marks[i][0] + marks[i][2],
                          marks[i][1] + marks[i][3], w, h);
        for (i = 0; i < s_prev_n; i++) {
            int px = s_prev_marks[i][0] - (cx - s_prev_cx), py = s_prev_marks[i][1] - (cy - s_prev_cy);
            bits_add_rect(px, py, px + s_prev_marks[i][2], py + s_prev_marks[i][3], w, h);
        }
        if (ey1 > ey0)
            bits_add_span(0, w, ey0, ey1, w, h);
        if (ex1 > ex0)
            bits_add_span(ex0, ex1, 0, h, w, h);
        if (s_present_stats > 0)
            tq2 = now_us();
        for (y = 0; y < h; y++) {
            int wd, open = -1;
            if (!s_row_any[y])
                continue;
            for (wd = 0; wd < nwords; wd++) {
                uint32_t v = s_bits[y][wd];
                int pos = 0, base = wd * 32;
                while (pos < 32) {
                    if (open < 0) {                     /* the next run's start */
                        uint32_t t = v << pos;
                        if (!t)
                            break;
                        pos += __builtin_clz(t);
                        open = base + pos;
                    }
                    {                                   /* ... and its end */
                        /* the shift brings zeros in from the right, so a run that
                         * reaches the word's end counts to it: open into the next */
                        int n = __builtin_clz(~(v << pos));
                        if (pos + n >= 32)
                            break;
                        pos += n;
                        s_runs[nruns].y = (int16_t)y;
                        s_runs[nruns].x0 = (int16_t)(open * 4);
                        s_runs[nruns].x1 = (int16_t)((base + pos) * 4 < w ? (base + pos) * 4 : w);
                        s_st_area += (uint32_t)(s_runs[nruns].x1 - s_runs[nruns].x0);
                        nruns++;
                        open = -1;
                    }
                }
            }
            if (open >= 0) {
                s_runs[nruns].y = (int16_t)y; s_runs[nruns].x0 = (int16_t)(open * 4);
                s_runs[nruns].x1 = (int16_t)w;
                s_st_area += (uint32_t)(w - open * 4);
                nruns++;
            }
        }
        /* The pitch where it is sent or read back and nobody drew (pitch.c:
         * drawn only where it is looked at). */
        for (i = 0; i < nruns; i++)
            Pitch_FillSpan(fb, s_runs[i].y, s_runs[i].x0, s_runs[i].x1);
        for (i = 0; i < VERIFY_ROWS; i++)
            Pitch_FillSpan(fb, (s_vrow + i) % h, 0, w);
        s_shadow_ok = 0;
        s_st_tracked++;
        s_st_marks += (uint32_t)(nm + s_prev_n);
        if (s_present_stats > 0) {
            uint64_t tq3 = now_us();
            s_st_p_ensure += tq1 - tq0;
            s_st_p_bits += tq2 - tq1;
            s_st_p_runs += tq3 - tq2;
        }
    } else {
        /* Everything compared: outside a match, after a jump, while an unmarked
         * writer is suspected. */
        Pitch_EnsureAll(fb);
        for (y = 0; y < h; y++) {
            const uint8_t *frow = fb + (size_t)y * _screen_width;
            uint8_t *srow = sh + (size_t)mod_pos(y + cy, h) * w;
            int x, open = -1;
            if (full || (y >= ey0 && y < ey1)) {
                seg_store(frow, srow, c0, w, w);
                s_runs[nruns].y = (int16_t)y; s_runs[nruns].x0 = 0; s_runs[nruns].x1 = (int16_t)w;
                nruns++;
                continue;
            }
            memset(dirty, 0, (size_t)nseg);
            /* Two spans where the torus wraps: fb [0, w-c0) is shadow [c0, w), the
             * rest shadow [0, c0). */
            diff_span(frow, srow + c0, 0, w - c0, dirty);
            if (c0)
                diff_span(frow + (w - c0), srow, w - c0, c0, dirty);
            for (x = ex0 / SEG; x < nseg && x * SEG < ex1; x++)
                dirty[x] = 1;                   /* uncovered by the shift */
            for (x = 0; x < nseg; x++) {
                int x0 = x * SEG;
                if (dirty[x]) {
                    int n2 = w - x0 < SEG ? w - x0 : SEG;
                    int c = c0 + x0;
                    if (c >= w)
                        c -= w;
                    seg_store(frow + x0, srow, c, n2, w);
                    if (open < 0)
                        open = x0;
                } else if (open >= 0) {
                    s_runs[nruns].y = (int16_t)y; s_runs[nruns].x0 = (int16_t)open; s_runs[nruns].x1 = (int16_t)x0;
                    nruns++;
                    open = -1;
                }
            }
            if (open >= 0) {
                s_runs[nruns].y = (int16_t)y; s_runs[nruns].x0 = (int16_t)open; s_runs[nruns].x1 = (int16_t)w;
                nruns++;
            }
        }
        s_shadow_ok = 1;
    }
    /* This frame's marks are the next one's "drawn last frame". */
    s_prev_n = over ? 0 : nm;
    s_prev_ok = !over;
    if (s_prev_n)
        memcpy(s_prev_marks, marks, (size_t)s_prev_n * sizeof s_prev_marks[0]);
    s_prev_cx = cx;
    s_prev_cy = cy;
    Render_ClearMarks();
    s_tor_valid = 1;
    s_tor_w = w;
    s_tor_h = h;
    s_tor_cx = cx;
    s_tor_cy = cy;

    /* Everything above was the CPU's alone; the card changes from here, and the
     * shift and the upload go back to back. Shifted first and compared after, the
     * card showed every player and the ball moved WITH the pitch for the ~8 ms of
     * the compare: they shook against the grass (2026-10-22). */
    /* ... and they go at the frame's slot, just after the vertical blank, ahead of
     * the beam (the picture's top row is ~4 ms into the refresh at 640x480). Sent
     * when the game finished instead, they landed wherever the beam was: once the
     * frame took ~7 ms rather than ~17, in the middle of the picture, and the
     * players, the ball and the clock tore between two frames (2026-10-22). */
    if (!vblank_pace())
        pace();
    s_paced = 1;
    uint64_t tc0 = s_present_stats > 0 ? now_us() : 0;
    if (!full && (ddx || ddy)) {
        int sx = ddx > 0 ? ddx : 0, sy = ddy > 0 ? ddy : 0;
        int tx = ddx < 0 ? -ddx : 0, ty = ddy < 0 ? -ddy : 0;
        int adx = ddx < 0 ? -ddx : ddx, ady = ddy < 0 ? -ddy : ddy;
        uint64_t b0 = now_us();
        BltBitMap(bm, (WORD)(dx + sx), (WORD)(dy + sy), bm, (WORD)(dx + tx), (WORD)(dy + ty),
                  (WORD)(w - adx), (WORD)(h - ady), 0xC0, 0xFF, NULL);
        s_st_shifts++;
        /* Any RTG board takes this call, but only one whose BlitRect is hardware
         * does it without the CPU: a software one reads the card's memory back over
         * the bus, slower than sending the frame. The first 50 shifts, waited for,
         * decide: over 3 ms each on average, and the row path takes over. */
        if (s_shift_probe < 50) {
            WaitBlit();
            s_shift_probe_us += now_us() - b0;
            if (++s_shift_probe == 50) {
                uint64_t avg = s_shift_probe_us / 50;
                if (avg > 3000) {
                    fprintf(stderr, "[video] a shift on the card takes %lu us: not the "
                                    "board's blitter -- the row path from now on\n",
                            (unsigned long)avg);
                    s_scrollblit = 0;
                } else
                    fprintf(stderr, "[video] the card shifts the picture in %lu us: "
                                    "scrolling on the card\n", (unsigned long)avg);
            }
        }
    }
    if (s_present_stats > 0) {
        uint64_t tc2 = now_us();
        s_st_cmp_us += tc0 - tc1;
        s_st_shift_us += tc2 - tc0;
    }

    if (nruns == 0) {
        s_st_same++;
        return 1;
    }
    {
        APTR  handle, base = NULL;
        ULONG bpr = 0, depth = 0, pixfmt = (ULONG)~0;
        struct TagItem t[5];
        int   usable, i;
        int   verify_bad = 0;
        uint64_t tl0 = 0, tl1 = 0, tl2 = 0;
        t[0].ti_Tag = LBMI_BASEADDRESS; t[0].ti_Data = (ULONG)&base;
        t[1].ti_Tag = LBMI_BYTESPERROW; t[1].ti_Data = (ULONG)&bpr;
        t[2].ti_Tag = LBMI_DEPTH;       t[2].ti_Data = (ULONG)&depth;
        t[3].ti_Tag = LBMI_PIXFMT;      t[3].ti_Data = (ULONG)&pixfmt;
        t[4].ti_Tag = TAG_DONE;         t[4].ti_Data = 0UL;
        if (s_present_stats > 0)
            tl0 = now_us();
        handle = LockBitMapTagList((APTR)bm, t);    /* waits for the shift to finish */
        if (handle == NULL) {
            s_tor_valid = 0;
            return 0;
        }
        /* ---- LOCK HELD: the copy only, no OS calls, no early return ---- */
        if (s_present_stats > 0)
            tl1 = now_us();
        usable = (base != NULL && depth == 8UL && pixfmt == PIXFMT_LUT8 &&
                  bpr >= (ULONG)(dx + w));
        if (usable) {
            uint8_t *card = (uint8_t *)base + (ULONG)dy * bpr + dx;
            for (i = 0; i < nruns; i++) {
                const struct Run *r = &s_runs[i];
                uint8_t *d = card + (ULONG)r->y * bpr + r->x0;
                const uint8_t *f = fb + (size_t)r->y * _screen_width + r->x0;
                int n = r->x1 - r->x0;
                if ((((uintptr_t)d | (uintptr_t)f | (uintptr_t)n) & 3) == 0) {
                    uint32_t *dl = (uint32_t *)d;   /* longwords, the usual case */
                    const uint32_t *fl = (const uint32_t *)f;
                    for (n >>= 2; n >= 4; n -= 4, dl += 4, fl += 4) {
                        dl[0] = fl[0]; dl[1] = fl[1]; dl[2] = fl[2]; dl[3] = fl[3];
                    }
                    while (n-- > 0)
                        *dl++ = *fl++;
                } else
                    memcpy(d, f, (size_t)n);
            }
            /* In a match nothing compares the frame any more: read VERIFY_ROWS rows
             * back from the card and hold them against it -- the whole picture every
             * h / VERIFY_ROWS frames (~1.4 s). A difference is a writer that did not
             * mark (or the card not showing what was sent): everything is sent again
             * and compared for FULL_FRAMES frames. */
            if (tracked) {
                int r;
                for (r = 0; r < VERIFY_ROWS; r++) {
                    int vy = (s_vrow + r) % h;
                    const u32_any *c = (const u32_any *)(card + (ULONG)vy * bpr);
                    const u32_any *f = (const u32_any *)(fb + (size_t)vy * _screen_width);
                    int k;
                    for (k = 0; k < w / 4; k++)
                        if (c[k] != f[k]) {
                            verify_bad = vy + 1;
                            break;
                        }
                }
                s_vrow = (s_vrow + VERIFY_ROWS) % h;
            }
        }
        if (s_present_stats > 0)
            tl2 = now_us();
        UnLockBitMap(handle);
        /* ---- LOCK RELEASED ---- */
        if (s_present_stats > 0) {
            uint64_t tl3 = now_us();
            s_st_lock += tl1 - tl0;
            s_st_copy += tl2 - tl1;
            s_st_unlock += tl3 - tl2;
            for (i = 0; i < nruns; i++)
                s_st_bytes += (uint32_t)(s_runs[i].x1 - s_runs[i].x0);
        }
        if (!usable) {
            s_tor_valid = 0;
            return 0;
        }
        if (verify_bad) {
            if (s_snap_n == 0)
                s_snap_req = 1;                 /* TEMP: the first one, as it is */
            if (s_st_verify_fail++ < 5)
                fprintf(stderr, "[video] the card's row %d is not the frame: a change nobody "
                                "marked -- sending everything, comparing for %d frames\n",
                        verify_bad - 1, FULL_FRAMES);
            s_full_frames = FULL_FRAMES;
            s_tor_valid = 0;
        }
    }
    return 1;
}

void Platform_RenderFrame(void)
{
    const uint8_t *fb;
    int w = _screen_width, h = _screen_height;
    int dx, dy;

    if (!s_screen)
        return;
    load_palette();
    fb = Platform_FrameToShow();
    if (!fb)
        return;
    if (w > s_mode_w) w = s_mode_w;
    if (h > s_mode_h) h = s_mode_h;
    dx = (s_mode_w - w) / 2;
    dy = (s_mode_h - h) / 2;

    /* The picture changed size (the DOS view in a match): what it no longer
     * covers goes back to the border colour -- in both buffers. */
    if (s_clear_borders || w != s_shown_w || h != s_shown_h) {
        if (s_dbuf) {
            int b;
            for (b = 0; b < 2; b++) {
                struct RastPort rp;
                wait_safe(b);
                InitRastPort(&rp);
                rp.BitMap = s_sb[b]->sb_BitMap;
                SetRast(&rp, 0);
            }
        } else {
            SetRast(&s_screen->RastPort, 0);
        }
        s_shown_w = w;
        s_shown_h = h;
        s_clear_borders = 0;
        s_shadow_valid[0] = s_shadow_valid[1] = 0;  /* cleared: every row again */
        s_tor_valid = 0;
    }

    /* On by default where the bus is the bottleneck (the 68060); WarpOS writes the
     * card over PCI and keeps the row path unless SWOS_SCROLLBLIT=1. */
    if (s_scrollblit < 0) {
#ifdef SWOS_WARPOS_BUILD
        s_scrollblit = getenv("SWOS_SCROLLBLIT") != NULL;
#else
        s_scrollblit = getenv("SWOS_NO_SCROLLBLIT") == NULL;
#endif
    }
    if (!s_dbuf && s_use_lock && s_scrollblit) {
        if (present_scrolled(fb, w, h, dx, dy)) {
            extern volatile uint16_t _playGame;
            /* The next frame's pitch only where it is looked at, when this one
             * went up tracked and the next one should too. */
            static int s_lazy_pitch = -1;  /* SWOS_NO_LAZY_PITCH=1: the whole pitch every frame */
            if (s_lazy_pitch < 0)
                s_lazy_pitch = getenv("SWOS_NO_LAZY_PITCH") == NULL;
            Pitch_AllowLazy(s_lazy_pitch && s_scrollblit && s_tor_valid && s_full_frames == 0 &&
                            _playGame);
            s_front = 0;
            s_shadow_valid[0] = 0;      /* the row path's shadow is not kept meanwhile */
            return;
        }
        s_scrollblit = 0;
        fprintf(stderr, "[video] the lock gave no 8-bit surface: the row path from now on\n");
    }
    Pitch_AllowLazy(0);
    Pitch_EnsureAll(fb);                /* the row path reads all of it */

    {
        int b = s_dbuf ? s_back : 0;
        int r0 = 0, r1 = h, r;
        struct BitMap *bm;

        /* What is on screen already shows this frame: nothing to do. */
        if (s_shadow_valid[s_front] && s_shadow_w[s_front] == w) {
            const uint8_t *sh = s_shadow[s_front];
            for (r = 0; r < h; r++)
                if (!rows_equal(fb + (size_t)r * _screen_width, sh + (size_t)r * w, w))
                    break;
            if (r == h) {
                s_st_same++;
                return;
            }
        }
        if (s_dbuf)
            wait_safe(b);
        /* The rows that differ from what this buffer holds. */
        if (s_shadow_valid[b] && w == s_shadow_w[b]) {
            const uint8_t *sh = s_shadow[b];
            while (r0 < h && rows_equal(fb + (size_t)r0 * _screen_width, sh + (size_t)r0 * w, w))
                r0++;
            while (r1 > r0 && rows_equal(fb + (size_t)(r1 - 1) * _screen_width, sh + (size_t)(r1 - 1) * w, w))
                r1--;
        }
        for (r = r0; r < r1; r++)
            memcpy(s_shadow[b] + (size_t)r * w, fb + (size_t)r * _screen_width, (size_t)w);
        s_shadow_valid[b] = 1;
        s_shadow_w[b] = w;
        s_st_rows += (uint32_t)(r1 - r0);
        bm = s_dbuf ? s_sb[b]->sb_BitMap : s_screen->RastPort.BitMap;

        if (r1 > r0) {
            if (s_use_lock && !blit_locked(bm, fb + (size_t)r0 * _screen_width, _screen_width,
                                           dx, dy + r0, w, r1 - r0)) {
                s_use_lock = 0;
                fprintf(stderr, "[video] the lock stopped giving an 8-bit surface: "
                                "WriteLUTPixelArray from now on\n");
            }
            if (!s_use_lock) {
                struct RastPort rp, *dst = &s_screen->RastPort;
                if (s_dbuf) {
                    InitRastPort(&rp);
                    rp.BitMap = bm;
                    dst = &rp;
                }
                WriteLUTPixelArray((APTR)fb, 0, (UWORD)r0, (UWORD)_screen_width,
                                   dst, (APTR)s_ctable,
                                   (UWORD)dx, (UWORD)(dy + r0), (UWORD)w, (UWORD)(r1 - r0),
                                   (UBYTE)CTABFMT_XRGB8);
            }
        }

        if (s_dbuf) {
            /* Show it at the next vertical blank. Busy (a flip still pending): the
             * frame stays in this buffer and goes up with the next one. */
            uint64_t f0 = now_us(), fd;
            ULONG ok = ChangeScreenBuffer(s_screen, s_sb[b]);
            fd = now_us() - f0;
            s_st_flip += fd;
            if (fd > s_st_max_flip)
                s_st_max_flip = fd;
            if (!ok)
                s_st_refused++;
            if (ok) {
                s_st_flips++;
                s_flipped = 1;
                s_safe_pending[s_front] = 1;    /* the old one, once it is off screen */
                s_front = b;
                s_back = 1 - b;
            }
        } else {
            s_front = 0;
        }
    }
}

/* ---- The event loop and the pace ------------------------------------- */

/* One 68k call when nothing happened: SetSignal reads the window port's signal and
 * CTRL-C together, and the port is drained only when its signal says so. */
static void pump_events(void)
{
    struct IntuiMessage *msg;
    ULONG port_sig = s_window ? (1UL << s_window->UserPort->mp_SigBit) : 0;
    ULONG sigs = SetSignal(0L, port_sig | SIGBREAKF_CTRL_C);   /* read and clear both */

    if (sigs & port_sig) {
        while ((msg = (struct IntuiMessage *)GetMsg(s_window->UserPort)) != NULL) {
            ULONG cls = msg->Class;
            UWORD code = msg->Code, qual = msg->Qualifier;
            ReplyMsg((struct Message *)msg);
            if (cls == IDCMP_RAWKEY && code == 0x5F)        /* HELP: snapshot() */
                s_snap_req = 1;
            else if (cls == IDCMP_RAWKEY)
                InputWos_Key(code, (qual & IEQUALIFIER_REPEAT) != 0);
        }
    }
    /* CTRL-C from the Shell that started it: quit, as closing the window does. */
    if (sigs & SIGBREAKF_CTRL_C)
        Platform_RequestQuit();
    InputWos_PostFrame();
}

/* WaitTime wakes on a coarse tick: measured on the Sonnet it came back 9 ms late on
 * average (2026-10-22), so a 6 ms wait cost 15 and a quarter of the frames missed
 * their slot. It is used only for what can stand to be up to WAIT_SPIN_US late;
 * the rest is a spin on the PPC timebase, which takes nothing from the 68k. */
/* WaitTime(3 ms) came back 16 ms later in a menu (overslept 13 ms a frame,
 * 2026-10-22): within a frame only a spin is on time.
 *
 * The 68060 has no second processor to spin on: a spin there IS the 68k, every
 * other task at the game's priority or below waits through it (AHI and input run
 * above it). Its wait is timer.device's MICROHZ unit (rtg_clock.c), which overslept
 * ~0.7 ms a frame on average and more at times: with a match frame at ~17.5 of its
 * 20 ms, 2 ms of spin left a quarter to a half of the frames late. 5 ms. */
#ifdef SWOS_WARPOS_BUILD
#define WAIT_SPIN_US 21000
#else
#define WAIT_SPIN_US 5000
#endif

static int      s_catchup_frames = 1;


/* Wait for the next frame's slot at the rate the game is paced at. A frame that
 * ran late runs the next one at once; more than the catch-up's worth behind, the
 * backlog is dropped rather than run back to back (platform_sdl.c says why). */
static void pace(void)
{
    uint64_t period = frame_period_us();
    uint64_t now = now_us();

    /* Paced by the display: the flip already waited for the vertical blank. The
     * timer takes over when a frame does not flip (nothing on screen changed). */
    s_locked = s_flipped && display_paces(period);
    s_flipped = 0;
    if (s_locked) {
        s_st_locked++;
        if (s_next_frame_us && now > s_next_frame_us + period + period / 2)
            s_st_late++;
        s_next_frame_us = now;
        return;
    }

    if (s_next_frame_us == 0) {
        s_next_frame_us = now;
        /* Frames that follow the display start just after a vertical blank: the
         * copy (~3 ms) then runs ahead of the beam, which reaches the picture's top
         * row 4 ms into the refresh at 640x480. SWOS_VSYNC_PHASE_US moves it. */
        if (s_vblank_us && period == s_disp_period_us && now > s_vblank_us) {
            const char *ph = getenv("SWOS_VSYNC_PHASE_US");
            uint64_t phase = ph ? (uint64_t)strtoul(ph, NULL, 0) % period : 0;
            s_next_frame_us = s_vblank_us + phase +
                              ((now - s_vblank_us) / period) * period;
            fprintf(stderr, "[video] frames follow the display: %.2f us apart, phase %lu us "
                            "after the vertical blank\n", s_disp_period_frac,
                    (unsigned long)phase);
        }
    }
    s_next_frame_us += period;
    /* the refresh's fraction of a microsecond, carried: 0.5 us a frame is a full
     * refresh of drift in 13 minutes */
    if (period == s_disp_period_us && s_disp_period_frac > 0) {
        static double s_carry;
        s_carry += s_disp_period_frac - (double)period;
        if (s_carry >= 1.0)       { s_next_frame_us += 1; s_carry -= 1.0; }
        else if (s_carry <= -1.0) { s_next_frame_us -= 1; s_carry += 1.0; }
    }
    if (s_next_frame_us > now) {
        uint64_t wait = s_next_frame_us - now;
        if (wait > period * 2)
            wait = period, s_next_frame_us = now + period;     /* the clock went back */
        /* WaitTime comes back late by up to a scheduler slice: sleep to within
         * WAIT_SPIN_US of the slot, then spin on the timebase, which on WarpOS
         * costs the 68k nothing. Measured before this: 70 of 250 frames late. */
        if (wait > WAIT_SPIN_US) {
            uint64_t asked = wait - WAIT_SPIN_US, t = now_us(), slept;
            RtgClock_WaitUs((uint32_t)asked);
            slept = now_us() - t;
            if (slept > asked)
                s_st_oversleep += slept - asked;
        }
        while (now_us() < s_next_frame_us)
            ;
        s_st_wait += wait;
    } else {
        s_st_late++;
        if (now - s_next_frame_us > period * (uint64_t)s_catchup_frames)
            s_next_frame_us += ((now - s_next_frame_us) / period) * period;  /* in phase */
    }
}

void Platform_SetCatchUpFrames(int frames)
{
    s_catchup_frames = frames < 1 ? 1 : frames;
}

void Platform_DelayNextFrame(uint32_t us)
{
    s_next_frame_us += us;
}

static void present_stats(uint64_t t0, uint64_t ta, uint64_t t1, uint64_t t2)
{
    if (s_st_last_end)
        s_st_work += t0 - s_st_last_end;
    if (s_st_prev_t0 && t0 - s_st_prev_t0 > s_st_max_gap)
        s_st_max_gap = t0 - s_st_prev_t0;
    s_st_prev_t0 = t0;
    if (ta - t0 > s_st_max_events)
        s_st_max_events = ta - t0;
    s_st_events += ta - t0;
    s_st_audio += t1 - ta;
    s_st_show += t2 - t1;
    if (++s_st_n >= 250) {
        double n = (double)s_st_n;
        fprintf(stderr, "[present] per frame: work %.2f ms, events %.2f ms, audio %.2f ms, "
                        "show %.2f ms (%.0f rows), wait %.2f ms\n",
                s_st_work / n / 1000.0, s_st_events / n / 1000.0, s_st_audio / n / 1000.0,
                s_st_show / n / 1000.0, s_st_rows / n, s_st_wait / n / 1000.0);
        if (s_phase_frames) {
            double m = (double)s_phase_frames * 1000.0;
            int i;
            fprintf(stderr, "[present] phases per match frame (%u frames):", (unsigned)s_phase_frames);
            for (i = 0; i < NUM_PHASES - 1; i++)
                fprintf(stderr, " %s %.2f ms", k_phase_name[i], s_phase_us[i] / m);
            fprintf(stderr, "\n");
            memset(s_phase_us, 0, sizeof s_phase_us);
            s_phase_frames = 0;
        }
        {
            /* Which of the game's two buffers is shown: the finished one (the one
             * drawn last, _frontbufferBaseAltPtr after the swap) or the next. */
            extern volatile uintptr_t _frontbufferBasePtr, _frontbufferBaseAltPtr;
            extern uint16_t _useAltBaseFlag, _usePlaneOffsetFlag;
            const uint8_t *shown = Platform_FrameToShow();
            fprintf(stderr, "[present] shown buffer: %s (useAlt %u, planeOffset %u)\n",
                    shown == (const uint8_t *)_frontbufferBaseAltPtr ? "the finished one" :
                    shown == (const uint8_t *)_frontbufferBasePtr ? "the one being drawn" : "another",
                    (unsigned)_useAltBaseFlag, (unsigned)_usePlaneOffsetFlag);
        }
        if (s_st_bytes || s_st_shifts) {
            fprintf(stderr, "[present] scrolled on the card %u times; %.1f KB a frame to the card; "
                            "shift call %.2f ms, preparing %.2f ms a frame; %u frames sent "
                            "from the marks\n",
                    (unsigned)s_st_shifts, s_st_bytes / n / 1024.0,
                    s_st_shift_us / n / 1000.0, s_st_cmp_us / n / 1000.0,
                    (unsigned)s_st_tracked);
            if (s_st_tracked)
                fprintf(stderr, "[present] tracked: %.1f marks, %.0f pixels sent from them a frame; "
                                "%u read-back mismatches; bits %.2f ms, runs %.2f ms\n",
                        s_st_marks / (double)s_st_tracked, s_st_area / (double)s_st_tracked,
                        (unsigned)s_st_verify_fail,
                        s_st_p_bits / (double)s_st_tracked / 1000.0,
                        s_st_p_runs / (double)s_st_tracked / 1000.0);
        }
        if (s_use_lock)
            fprintf(stderr, "[present] show = lock %.2f ms + copy %.2f ms + unlock %.2f ms a frame "
                            "(the copy %.1f MB/s)\n",
                    s_st_lock / n / 1000.0, s_st_copy / n / 1000.0, s_st_unlock / n / 1000.0,
                    s_st_copy ? (double)(s_st_bytes ? s_st_bytes : s_st_rows * (uint32_t)_screen_width) /
                                (double)s_st_copy : 0.0);
        fprintf(stderr, "[present] %d late of %d at %d Hz, %d palette loads; slowest frame "
                        "%.1f ms, slowest events %.1f ms; WaitTime overslept %.2f ms a frame; "
                        "flip %.2f ms (slowest %.1f); %u flips, %u refused, %u unchanged, "
                        "%u display-paced\n",
                s_st_late, s_st_n, Platform_ActivePacingFps(), s_st_palette,
                s_st_max_gap / 1000.0, s_st_max_events / 1000.0, s_st_oversleep / n / 1000.0,
                s_st_flip / n / 1000.0, s_st_max_flip / 1000.0,
                (unsigned)s_st_flips, (unsigned)s_st_refused, (unsigned)s_st_same,
                (unsigned)s_st_locked);
        if (s_vbl_ok)
            fprintf(stderr, "[present] %u frames sent at the card's vertical blank\n",
                    (unsigned)s_vbl_waits);
        s_vbl_waits = 0;
        s_st_flips = s_st_refused = s_st_same = s_st_locked = 0;
        s_st_flip = s_st_max_flip = 0;
        s_st_oversleep = 0;
        s_st_max_gap = s_st_max_events = 0;
        s_st_work = s_st_events = s_st_audio = s_st_show = s_st_wait = 0;
        s_st_n = s_st_late = s_st_palette = 0;
        s_st_rows = 0;
        s_st_lock = s_st_copy = s_st_unlock = 0;
        s_st_bytes = s_st_shifts = 0;
        s_st_shift_us = s_st_cmp_us = 0;
        s_st_tracked = 0;
        s_st_marks = s_st_area = 0;
        s_st_verify_fail = 0;
        s_st_p_ensure = s_st_p_bits = s_st_p_runs = 0;
    }
}

int Platform_Present(void)
{
    static int32_t s_frames;
    s_present_count++;
    if (s_frames < 3 || s_frames == 50 || s_frames == 500)
        fprintf(stderr, "[wos] frame %d\n", (int)s_frames);
    s_frames++;
    uint64_t t0 = 0, ta = 0, t1 = 0, t2 = 0;
    if (s_present_stats < 0)
        s_present_stats = getenv("SWOS_PRESENT_STATS") != NULL;
    if (s_present_stats)
        t0 = now_us();
    Platform_Phase(0x0000);     /* the match frame's last phase ends here */
    pump_events();
    if (s_present_stats)
        ta = now_us();
    /* The sound: one thread, so this is its only pump, once a frame -- as on WASM,
     * real ticks until the queue is at its target (each returns at once when it is),
     * never a pad of silence. */
    {
        int i;
        for (i = 0; i < 16; i++)
            AudioSDL_Tick();
    }
    if (s_present_stats)
        t1 = now_us();
    Platform_RenderFrame();
    if (s_snap_req)
        snapshot();
    if (s_present_stats) {
        t2 = now_us();
        present_stats(t0, ta, t1, t2);
    }
    {
        /* TEMP: every frame that came more than 200 ms after the one before, and
         * what the game was doing -- the testers' "it stops for seconds". */
        extern volatile uint16_t _playGame, _playHighlightFlag, _fadeState;
        extern uint16_t _replayState;
        static uint64_t s_hitch_prev;
        uint64_t th = now_us();
        if (s_hitch_prev && th - s_hitch_prev > 200000)
            fprintf(stderr, "[hitch] %lu ms before frame %d: playGame %u, replayState %u, "
                            "highlight %u, fade %u\n",
                    (unsigned long)((th - s_hitch_prev) / 1000), (int)s_frames,
                    (unsigned)_playGame, (unsigned)_replayState,
                    (unsigned)_playHighlightFlag, (unsigned)_fadeState);
        s_hitch_prev = th;
    }
    Platform_LogTick();
    if (!s_paced)
        pace();
    s_paced = 0;
    if (s_present_stats)
        s_st_last_end = now_us();
    return 0;
}

void Platform_SetFrameHook(void (*hook)(void))
{
    s_frame_hook = hook;
}

int Platform_VBlank(void)
{
    RunInt3HandlerThrottled();
    if (s_frame_hook)
        s_frame_hook();
    return Platform_Present();
}

/* ---- The rest of the API --------------------------------------------- */

void Platform_ViewChanged(void)
{
    s_clear_borders = 1;
}

/* What SWOS.ini said, so Settings_AppendMissing writes back what it read: the
 * screen here is always a screen of its own, at no scale. */
int Platform_GetWindowScale(void)                     { return s_win_scale; }
PlatformFullscreenMode Platform_GetFullscreenMode(void) { return s_fullscreen_mode; }

uint8_t Platform_NearestColour(uint8_t r, uint8_t g, uint8_t b)
{
    int32_t i, best = 1, bestD = 0x7FFFFFFF;
    for (i = 1; i < 256; i++) {
        int32_t dr = s_rgb[i * 3 + 0] - r, dg = s_rgb[i * 3 + 1] - g, db = s_rgb[i * 3 + 2] - b;
        int32_t d = dr * dr + dg * dg + db * db;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return (uint8_t)best;
}

/* The HUD layer exists to escape the sub-pixel scroll shift, which this build
 * does not do: the HUD stays in the frame, as on the Amiga. */
uint8_t *Platform_HudLayerBuffer(void)            { return NULL; }
void     Platform_HudLayerReset(void)             { }
void     Platform_HudLayerMark(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }

/* The arcade cabinet is SDL-only (its coin keys, its BMP sheets). */
void Platform_ArcadeInput(int *coin1, int *coin2, int *service, int *start1, int *start2)
{
    if (coin1)   *coin1 = 0;
    if (coin2)   *coin2 = 0;
    if (service) *service = 0;
    if (start1)  *start1 = 0;
    if (start2)  *start2 = 0;
}
int Platform_ArcadeDebugWin(void) { return 0; }
int Platform_LoadIndexedBMP(const char *path, uint8_t *out8, int out_cap,
                            uint32_t *outPal, int *outW, int *outH)
{
    (void)out8; (void)out_cap; (void)outPal; (void)outW; (void)outH;
    fprintf(stderr, "[arcade] %s: the arcade cabinet is not in this build\n", path);
    return 1;
}

#endif
