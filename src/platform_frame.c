/*
 * platform_frame.c -- what the chunky target shows, for whichever backend shows it.
 *
 * Which buffer holds the finished frame, the colours it is shown in, and the rate
 * the game is paced at do not depend on how the frame reaches the screen: the SDL
 * backend (platform_sdl.c) converts it to RGBA for a texture, WarpOS
 * (platform_wos.c) copies it onto an 8-bit RTG screen and loads the palette.
 * Kept here, once, so the two cannot show the same frame in different colours.
 * Moved out of platform_sdl.c unchanged on 2026-10-22.
 */

#ifdef SWOS_CHUNKY_BUILD

#include <stdio.h>
#include <stdlib.h>
#include "platform_sdl.h"
#include "display.h"
#include "dos_mode.h"        /* _gDosGraphics */
#include "dos_gameplay.h"    /* DosGameplay_TargetFps -- 70 (DOS) vs 50 (Amiga) */
#include "dos_graphics.h"    /* DosGfx_BuildPalette */

extern volatile uintptr_t _frontbufferBasePtr;
extern volatile uintptr_t _frontbufferBaseAltPtr;
extern uint8_t  _frontbufferMaskBuffer[];
extern uint16_t _useAltBaseFlag;
extern uint16_t _usePlaneOffsetFlag;

/* The copperlist holds the live palette in Amiga 0xRGB nibbles. */
extern uint8_t _copperListBuffer[];

/* Active host pacing rate (Hz). Normally the gameplay-mode target
 * (DosGameplay_TargetFps: 70 DOS / 50 Amiga). BUT while a LOADED replay is
 * playing (_playHighlightFlag), pace at the replay's OWN recorded framerate
 * (_replayPlaybackFps) so a DOS RPL always plays at 70 Hz and a native Amiga
 * .HIL at 50 Hz, independent of the current gameplay setting. The in-match
 * instant replay (PlayInstantReplayLoop) does NOT set _playHighlightFlag, so it
 * correctly stays on the gameplay rate — it was recorded at that rate. Every
 * per-frame pacing site reads this so the main frame clock, the worker
 * tick, and the ISR throttle all agree during replay playback. */
int Platform_ActivePacingFps(void)
{
    extern volatile uint16_t _playHighlightFlag;
    extern int _replayPlaybackFps;
    if (_playHighlightFlag != 0 && _replayPlaybackFps > 0)
        return _replayPlaybackFps;
    return DosGameplay_TargetFps();
}

/* Double-buffer semantics — read the *completed* frame, not the
 * one the game is currently drawing into.
 *
 * `SwapBuffersAndCameraViewState()` runs at each VBlank and flips
 * the two pointers: post-swap, `frontbufferBasePtr` is the next
 * draw target (main thread is about to fill it), and
 * `frontbufferBaseAltPtr` is the just-finished frame. Reading
 * `frontbufferBasePtr` means we're reading the buffer main is
 * writing into — any async timer render or even a slow main-path
 * render will catch it half-complete and tear sprites. Reading
 * `frontbufferBaseAltPtr` gives us a stable completed frame and
 * also eliminates the 1-frame lag that the pre-swap ↔ post-swap
 * pointer aliasing hid. `useAltBaseFlag` inverts this when the
 * game code wants to display the drawing target directly (used by
 * transient overlay paths).
 *
 * Rows are FRONTBUFFER_ROW_STRIDE_BYTES apart and always start at line 0:
 * the Amiga's copper Y offset (cameraYMod320) is not applied, because the
 * chunky build redraws the whole visible pitch to lines 0-199 every frame
 * (pitch.c:DrawChunkyPitch) -- applying it as well stitched two pitch views
 * together. */
const uint8_t *Platform_FrameToShow(void)
{
    if (_usePlaneOffsetFlag != 0)
        return _frontbufferMaskBuffer;
    if (_useAltBaseFlag != 0)
        return (const uint8_t *)(uintptr_t)_frontbufferBasePtr;
    return (const uint8_t *)(uintptr_t)_frontbufferBaseAltPtr;
}

void Platform_FramePalette(uint8_t *rgb)
{
    /* DOS graphics use the full 256-colour DOS palette, but ONLY during a
     * match. The intro, title and menu screens are still Amiga graphics and
     * need their normal Amiga (copper) palette — otherwise they render with the
     * wrong colours (olive backgrounds etc.). playGame is set at kickoff and
     * cleared when the match ends; playHighlightFlag does the same for HIL
     * highlight playback, which renders the in-match pitch/players but never
     * sets playGame. Either flag means the DOS pitch is on screen, so we must
     * use the DOS palette. (DOS menu backgrounds are a separate follow-up.) */
    extern volatile uint16_t _playGame, _playHighlightFlag;
    extern int32_t Pitch_DrawnThisMatch(void);
    extern void    Pitch_ResetDrawn(void);

    /* ... and only once the pitch is actually ON SCREEN, which is later than
     * "a match has started": _playGame is set on the first line of
     * StartMatchSequence, while the frontbuffer still holds the pre-match team
     * line-up that InitGameSaveTeams has yet to fade out. Without this the
     * line-up is displayed through the DOS PITCH palette for those frames --
     * the "distorted VS background for a frame or two" (reported 2026-09-22).
     * The highlights path avoids the same trap by setting its flag after the
     * fade (game_loop.c, ViewHighLightsMain); the live-match path cannot,
     * because _playGame means other things to the game logic, so the display
     * asks the pitch instead. */
    if (!_playGame && !_playHighlightFlag)
        Pitch_ResetDrawn();
    /* SWOS_VSDEBUG=1 counts the frames this gate saves. It is how the bug was
     * proved and is the cheapest regression check for it: on the rig path it
     * reports 1, and through the real menus one per frame of the VS fade. */
    {
        static int32_t dbg = -1, n;
        if (dbg < 0) dbg = getenv("SWOS_VSDEBUG") != NULL;
        if (dbg && _gDosGraphics && (_playGame || _playHighlightFlag)
            && !Pitch_DrawnThisMatch() && ++n <= 20)
            fprintf(stderr, "[vsdebug] blocked the DOS pitch palette before the pitch "
                            "was on screen (#%d)\n", n);
    }
    if (_gDosGraphics && (_playGame || _playHighlightFlag) && Pitch_DrawnThisMatch()) {
        /* The pitch-condition overlay keys off the low byte of
         * pitchNumberAndType (the high byte is which pitch graphic). */
        extern uint16_t _pitchNumberAndType;
        uint8_t dos[768];
        DosGfx_BuildPalette((int)(_pitchNumberAndType & 0xFF), dos);
        /* Apply the current fade level so fades work on the DOS palette like the
         * copper path does (g_fadeLevel: 0x80 bright .. 0 black; rgb*level/0x80).
         * DosGfx_BuildPalette rebuilds full-bright RGB each frame, so without
         * this scaling fade in/out had no visible effect on DOS pitches. */
        extern int g_fadeLevel;
        int fl = g_fadeLevel;
        if (fl < 0) fl = 0; else if (fl > 0x80) fl = 0x80;
        for (int i = 0; i < 768; ++i)
            rgb[i] = (uint8_t)((dos[i] * fl) >> 7);
        return;
    }
#ifdef AGA
    /* AGA: 256 colours across 8 banks of 32. WriteCopperPalette wrote each
     * bank's 32 value words starting at COP_PAL_Bx_C0_VAL_OFF with a 4-byte
     * stride (copper reg+val pairs), exactly like the OCS block below but for
     * 8 banks. Each value is a 12-bit 0x0RGB word; expand each nibble to 8-bit
     * (*17 maps 0xF→255). Fades are already baked into these copper values. */
    static const uint32_t aga_bank_val_off[8] = {
        COP_PAL_B0_C0_VAL_OFF, COP_PAL_B1_C0_VAL_OFF,
        COP_PAL_B2_C0_VAL_OFF, COP_PAL_B3_C0_VAL_OFF,
        COP_PAL_B4_C0_VAL_OFF, COP_PAL_B5_C0_VAL_OFF,
        COP_PAL_B6_C0_VAL_OFF, COP_PAL_B7_C0_VAL_OFF
    };
    for (int bank = 0; bank < 8; ++bank) {
        const uint8_t *base = _copperListBuffer + aga_bank_val_off[bank];
        for (int j = 0; j < 32; ++j) {
            uint16_t v = *(const uint16_t *)(base + j * 4);
            int idx = bank * 32 + j;
            rgb[idx * 3 + 0] = (uint8_t)(((v >> 8) & 0x0F) * 17);
            rgb[idx * 3 + 1] = (uint8_t)(((v >> 4) & 0x0F) * 17);
            rgb[idx * 3 + 2] = (uint8_t)(((v >> 0) & 0x0F) * 17);
        }
    }
#else
    /* OCS: two palette blocks of 16 colours immediately at known offsets.
     * Each MOVE pair is 4 bytes [reg_word][value_word], we want value_word.
     * Bank 0 starts at COP_PAL_B0_C0_VAL_OFF (0x72). Each entry is 4 bytes. */
    const uint16_t *bank0 = (const uint16_t *)(_copperListBuffer + 0x72);
    for (int i = 0; i < 16; ++i) {
        uint16_t v = bank0[i * 2];          /* skip the [reg] word */
        rgb[i * 3 + 0] = (uint8_t)(((v >> 8) & 0x0F) * 17);
        rgb[i * 3 + 1] = (uint8_t)(((v >> 4) & 0x0F) * 17);
        rgb[i * 3 + 2] = (uint8_t)(((v >> 0) & 0x0F) * 17);
    }
#endif
}

/* ---- Where the frame drew on top of the pitch ----------------------------
 * Render_MarkRect (platform_sdl.h). A bounded list a frame: a backend that reads
 * it clears it after each present; one that never does just leaves it full,
 * marked overflowed, at no cost. */
static int16_t s_marks[RENDER_MAX_MARKS][4];
static int     s_numMarks, s_marksOverflow;

void Render_MarkRect(int x, int y, int w, int h)
{
    Pitch_EnsureRect(x, y, w, h);       /* the grass under it, before it is drawn */
    if (s_numMarks >= RENDER_MAX_MARKS) {
        s_marksOverflow = 1;
        return;
    }
    s_marks[s_numMarks][0] = (int16_t)x;
    s_marks[s_numMarks][1] = (int16_t)y;
    s_marks[s_numMarks][2] = (int16_t)w;
    s_marks[s_numMarks][3] = (int16_t)h;
    s_numMarks++;
}

int Render_Marks(const int16_t (**marks)[4], int *overflow)
{
    *marks = (const int16_t (*)[4])s_marks;
    *overflow = s_marksOverflow;
    return s_numMarks;
}

void Render_ClearMarks(void)
{
    s_numMarks = 0;
    s_marksOverflow = 0;
}

#endif /* SWOS_CHUNKY_BUILD */
