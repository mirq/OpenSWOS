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
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <powerpc/powerpc.h>
#include <powerpc/powerpc_protos.h>
#pragma pack(pop)

/* Library bases: the MOS2WOS startup opens exec and dos and nothing else, so
 * these are ours to define and open. PowerPCBase comes from startupwos.o. */
struct IntuitionBase *IntuitionBase;
struct GfxBase       *GfxBase;
struct Library       *CyberGfxBase;
extern struct Library *PowerPCBase;

extern int _screen_width, _screen_height;
extern void _Int3TimerHandler(void);

int  AudioSDL_Init(int mono);       /* audio_wos.c */
void AudioSDL_Shutdown(void);
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
static uint8_t s_rgb[768];
static uint8_t s_rgb_loaded[768];
static int     s_palette_loaded;
static ULONG   s_ctable[256];

static void (*s_frame_hook)(void);

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

/* As platform_sdl.c's, without the lock: there is one thread. Throttled to just
 * under a frame so a wait loop that calls it twice in a frame ticks once. */
void RunInt3HandlerThrottled(void)
{
    const uint64_t thr = (uint64_t)(1000 / Platform_ActivePacingFps() - 2);
    uint64_t now = Platform_NowMs();
    if (now - s_last_isr_ms < thr)
        return;
    s_last_isr_ms = now;
    _Int3TimerHandler();
}

/* No timer thread to quiesce or resume. */
void Platform_QuiesceTimerThread(void) { }
void Platform_ResumeTimerThread(void)  { }

/* ---- Choosing a mode ------------------------------------------------- */

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

    /* TRAP: when nothing matches this is a NATIVE chipset mode id. */
    if (id == (ULONG)INVALID_ID)                       return (ULONG)INVALID_ID;
    if (!IsCyberModeID(id))                            return (ULONG)INVALID_ID;
    if (GetCyberIDAttr(CYBRIDATTR_DEPTH, id) != 8UL)   return (ULONG)INVALID_ID;
    if (GetCyberIDAttr(CYBRIDATTR_BPPIX, id) != 1UL)   return (ULONG)INVALID_ID;
    if ((int)GetCyberIDAttr(CYBRIDATTR_WIDTH, id)  < WOS_PICTURE_W ||
        (int)GetCyberIDAttr(CYBRIDATTR_HEIGHT, id) < WOS_PICTURE_H)
        return (ULONG)INVALID_ID;
    return id;
}

/* The mode ladder: the size SWOS.ini asks for ([video] resolution), then the
 * picture's own size -- a 336x272 or 352x288 mode a user made in Picasso96Mode
 * shows it 1:1 and full screen -- then the common sizes it fits in, centred.
 * Every step down says why. */
static ULONG choose_mode(const PlatformInitOptions *opts)
{
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

    s_mode_id = choose_mode(opts);
    if (s_mode_id == (ULONG)INVALID_ID) {
        fprintf(stderr, "[video] no 8-bit RTG mode with room for %dx%d: add one in "
                        "Picasso96Mode (336x272 shows the game 1:1)\n",
                WOS_PICTURE_W, WOS_PICTURE_H);
        return -1;
    }
    s_mode_w = (int)GetCyberIDAttr(CYBRIDATTR_WIDTH, s_mode_id);
    s_mode_h = (int)GetCyberIDAttr(CYBRIDATTR_HEIGHT, s_mode_id);

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

    /* No pointer over the game: a sprite of zeros, in Chip RAM. */
    s_blank_pointer = (UWORD *)AllocVec(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR);
    if (s_blank_pointer)
        SetPointer(s_window, s_blank_pointer, 1, 16, 0, 0);

    SetRast(&s_screen->RastPort, 0);    /* the border is colour 0, as on the Amiga */
    probe_lock();

    Platform_RewireCustom();
    Platform_PreArmIsrGlobals();
    InputWos_Init();
    (void)AudioSDL_Init(opts ? opts->audio_mono : 1);
    s_last_isr_ms = Platform_NowMs();
    return 0;
}

void Platform_Shutdown(void)
{
    AudioSDL_Shutdown();
    InputWos_Shutdown();
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
    memcpy(s_rgb_loaded, s_rgb, sizeof s_rgb);
    s_palette_loaded = 1;
}

/* The picture's rows into the screen at (dx, dy), under the lock. Returns 0 when
 * the lock gave nothing usable: the caller demotes to WriteLUTPixelArray. */
static int blit_locked(const uint8_t *fb, int stride, int dx, int dy, int w, int h)
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

    handle = LockBitMapTagList((APTR)s_screen->RastPort.BitMap, t);
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
     * covers goes back to the border colour. */
    if (s_clear_borders || w != s_shown_w || h != s_shown_h) {
        SetRast(&s_screen->RastPort, 0);
        s_shown_w = w;
        s_shown_h = h;
        s_clear_borders = 0;
    }

    if (s_use_lock && !blit_locked(fb, _screen_width, dx, dy, w, h)) {
        s_use_lock = 0;
        fprintf(stderr, "[video] the lock stopped giving an 8-bit surface: "
                        "WriteLUTPixelArray from now on\n");
    }
    if (!s_use_lock)
        WriteLUTPixelArray((APTR)fb, 0, 0, (UWORD)_screen_width,
                           &s_screen->RastPort, (APTR)s_ctable,
                           (UWORD)dx, (UWORD)dy, (UWORD)w, (UWORD)h,
                           (UBYTE)CTABFMT_XRGB8);
}

/* ---- The event loop and the pace ------------------------------------- */

static void pump_events(void)
{
    struct IntuiMessage *msg;

    if (s_window) {
        while ((msg = (struct IntuiMessage *)GetMsg(s_window->UserPort)) != NULL) {
            ULONG cls = msg->Class;
            UWORD code = msg->Code, qual = msg->Qualifier;
            ReplyMsg((struct Message *)msg);
            if (cls == IDCMP_RAWKEY)
                InputWos_Key(code, (qual & IEQUALIFIER_REPEAT) != 0);
        }
    }
    /* CTRL-C from the Shell that started it: quit, as closing the window does. */
    if (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        Platform_RequestQuit();
    InputWos_PostFrame();
}

static uint64_t s_next_frame_us;
static int      s_catchup_frames = 1;

/* Wait for the next frame's slot at the rate the game is paced at. A frame that
 * ran late runs the next one at once; more than the catch-up's worth behind, the
 * backlog is dropped rather than run back to back (platform_sdl.c says why). */
static void pace(void)
{
    uint64_t period = 1000000u / (uint64_t)Platform_ActivePacingFps();
    uint64_t now = now_us();

    if (s_next_frame_us == 0)
        s_next_frame_us = now;
    s_next_frame_us += period;
    if (s_next_frame_us > now) {
        uint64_t wait = s_next_frame_us - now;
        if (wait > period * 2)
            wait = period, s_next_frame_us = now + period;     /* the clock went back */
        WaitTime(0, (ULONG)wait);
    } else if (now - s_next_frame_us > period * (uint64_t)s_catchup_frames) {
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

int Platform_Present(void)
{
    pump_events();
    Platform_RenderFrame();
    pace();
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
