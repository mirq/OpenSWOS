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
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <libraries/asl.h>
#include <dos/var.h>
#include <proto/asl.h>
#include <powerpc/powerpc.h>
#include <powerpc/powerpc_protos.h>
#pragma pack(pop)

/* Library bases: the MOS2WOS startup opens exec and dos and nothing else, so
 * these are ours to define and open. PowerPCBase comes from startupwos.o. */
struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *CyberGfxBase;
struct Library       *AslBase;
extern struct Library *PowerPCBase;

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
static int                  s_flipped;          /* this frame went up with a flip */
static int                  s_locked;           /* the last frame was paced by its flip */
static struct MsgPort      *s_safe_port;
static int                  s_dbuf;             /* 1: the two buffers are in use */
static int                  s_back;             /* the buffer to draw into */
static int                  s_front = 0;        /* the buffer on screen */
static int                  s_safe_pending[2];  /* flipped away, not yet safe to write */
static uint8_t s_shadow[2][672 * 272];
static void dbuf_open(void);
/* The display's refresh is within 4% of `period`: its flips can pace the frames. */
static int display_paces(uint64_t period)
{
    return s_disp_period_us > period - period / 25 && s_disp_period_us < period + period / 25;
}
static void vsync_probe(void);
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
static uint64_t s_st_flip, s_st_max_flip;   /* time inside ChangeScreenBuffer */
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

/* ---- The clock ------------------------------------------------------ */

static uint64_t now_us(void)
{
    struct timeval tv;
    GetSysTimePPC(&tv);
    return (uint64_t)tv.tv_secs * 1000000u + tv.tv_micro;
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
    const uint64_t period = 1000000u / (uint64_t)Platform_ActivePacingFps();
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

/* A wait that never leaves the PPC: powerpc.library's WaitTime, in microseconds. */
void Platform_SleepUs(uint32_t us)
{
    WaitTime(0, (ULONG)us);
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
    fprintf(stderr, "[video] frames go %s\n",
            s_use_lock ? "straight into the card's memory (LockBitMapTagList)"
                       : "through WriteLUTPixelArray (the lock gave no 8-bit surface)");
}

/* ---- Lifecycle ------------------------------------------------------- */

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

    fprintf(stderr, "[wos] input, audio\n");
    Platform_RewireCustom();
    Platform_PreArmIsrGlobals();
    InputWos_Init();
    (void)AudioSDL_Init(opts ? opts->audio_mono : 1);
    s_last_isr_ms = Platform_NowMs();
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
        WaitTime(0, 1000);
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
            WaitTime(0, 2000);
        WaitTime(0, 50000);
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
        enum { N = 14 };
        uint64_t t[N], d[N - 1], x;
        int j, k;
        for (i = 0; i < N; i++) {
            ChangeScreenBuffer(s_screen, s_sb[(i + 1) & 1]);
            t[i] = now_us();
        }
        while (!ChangeScreenBuffer(s_screen, s_sb[0]))      /* its own bitmap again */
            WaitTime(0, 2000);
        WaitTime(0, 30000);
        /* the median interval: a missed blank or a late return does not move it */
        for (j = 0; j < N - 1; j++)
            d[j] = t[j + 1] - t[j];
        for (j = 1; j < N - 1; j++)
            for (k = j; k > 0 && d[k] < d[k - 1]; k--)
                x = d[k], d[k] = d[k - 1], d[k - 1] = x;
        s_disp_period_us = d[(N - 1) / 2];
    }
    if (!getenv("SWOS_DBUF") && !display_paces(1000000u / 50u)) {
        FreeScreenBuffer(s_screen, s_sb[1]);
        FreeScreenBuffer(s_screen, s_sb[0]);
        DeleteMsgPort(s_safe_port);
        s_sb[0] = s_sb[1] = NULL;
        s_safe_port = NULL;
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
            WaitTime(0, 2000);
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

    handle = LockBitMapTagList((APTR)bm, t);
    if (handle == NULL)
        return 0;
    /* ---- LOCK HELD: the copy only, no OS calls, no early return ---- */
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
    UnLockBitMap(handle);
    /* ---- LOCK RELEASED ---- */
    return usable;
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
    }

    {
        int b = s_dbuf ? s_back : 0;
        int r0 = 0, r1 = h, r;
        struct BitMap *bm;

        /* What is on screen already shows this frame: nothing to do. */
        if (s_shadow_valid[s_front] && s_shadow_w[s_front] == w) {
            const uint8_t *sh = s_shadow[s_front];
            for (r = 0; r < h; r++)
                if (memcmp(fb + (size_t)r * _screen_width, sh + (size_t)r * w, (size_t)w) != 0)
                    break;
            if (r == h)
                return;
        }
        if (s_dbuf)
            wait_safe(b);
        /* The rows that differ from what this buffer holds. */
        if (s_shadow_valid[b] && w == s_shadow_w[b]) {
            const uint8_t *sh = s_shadow[b];
            while (r0 < h && memcmp(fb + (size_t)r0 * _screen_width, sh + (size_t)r0 * w, (size_t)w) == 0)
                r0++;
            while (r1 > r0 && memcmp(fb + (size_t)(r1 - 1) * _screen_width, sh + (size_t)(r1 - 1) * w, (size_t)w) == 0)
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
            if (ok) {
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
            if (cls == IDCMP_RAWKEY)
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
#define WAIT_SPIN_US 11000

static uint64_t s_next_frame_us;
static int      s_catchup_frames = 1;


/* Wait for the next frame's slot at the rate the game is paced at. A frame that
 * ran late runs the next one at once; more than the catch-up's worth behind, the
 * backlog is dropped rather than run back to back (platform_sdl.c says why). */
static void pace(void)
{
    uint64_t period = 1000000u / (uint64_t)Platform_ActivePacingFps();
    uint64_t now = now_us();

    /* Paced by the display: the flip already waited for the vertical blank. The
     * timer takes over when a frame does not flip (nothing on screen changed). */
    s_locked = s_flipped && display_paces(period);
    s_flipped = 0;
    if (s_locked) {
        if (s_next_frame_us && now > s_next_frame_us + period + period / 2)
            s_st_late++;
        s_next_frame_us = now;
        return;
    }

    if (s_next_frame_us == 0)
        s_next_frame_us = now;
    s_next_frame_us += period;
    if (s_next_frame_us > now) {
        uint64_t wait = s_next_frame_us - now;
        if (wait > period * 2)
            wait = period, s_next_frame_us = now + period;     /* the clock went back */
        /* WaitTime comes back late by up to a scheduler slice: sleep to within
         * WAIT_SPIN_US of the slot, then spin on the timebase, which costs the 68k
         * nothing. Measured before this: 70 of 250 frames late. */
        if (wait > WAIT_SPIN_US) {
            uint64_t asked = wait - WAIT_SPIN_US, t = now_us(), slept;
            WaitTime(0, (ULONG)asked);
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
            s_next_frame_us = now;
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
        fprintf(stderr, "[present] %d late of %d at %d Hz, %d palette loads; slowest frame "
                        "%.1f ms, slowest events %.1f ms; WaitTime overslept %.2f ms a frame; "
                        "flip %.2f ms (slowest %.1f)\n",
                s_st_late, s_st_n, Platform_ActivePacingFps(), s_st_palette,
                s_st_max_gap / 1000.0, s_st_max_events / 1000.0, s_st_oversleep / n / 1000.0,
                s_st_flip / n / 1000.0, s_st_max_flip / 1000.0);
        s_st_flip = s_st_max_flip = 0;
        s_st_oversleep = 0;
        s_st_max_gap = s_st_max_events = 0;
        s_st_work = s_st_events = s_st_audio = s_st_show = s_st_wait = 0;
        s_st_n = s_st_late = s_st_palette = 0;
        s_st_rows = 0;
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
    if (s_present_stats) {
        t2 = now_us();
        present_stats(t0, ta, t1, t2);
    }
    Platform_LogTick();
    pace();
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
