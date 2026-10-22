/*
 * audio_wos.c -- the WarpOS build's sound device: AHI (docs/WARPOS.md, "Sound").
 *
 * audio_sdl.c is the game's mixer on every chunky build -- the RJP1 music and
 * effects, the overlay sample -- and hands its mix, one game tick at a time, to
 * AudioOut_* (include/audio_out.h). Here that queue is a RING that AHI plays for
 * ever: one dynamic sample (AHIST_DYNAMICSAMPLE), looped on one channel, the way
 * the Quake ports on WarpOS do it (quake3amiga code/amiga/amiga_snd.c, after the
 * Quake 2 port by Jarmo Laakkonen and Hans-Joerg Frieden). Where AHI is playing
 * comes from an AHIET_CHANNELINFO effect hook, which AHI calls on every mix with
 * the play offset; we write ahead of it.
 *
 * The point of the design: a frame makes NO call to the 68k side. The PPC writes
 * the ring, writes those cache lines back (dcbf: the 68k does not snoop the PPC's
 * data cache), and reads the play position the hook left in memory, after
 * flushing that line too so it is not read from a stale cache.
 *
 * The rate: 22,050 Hz unless SWOS_AHI_RATE says otherwise, as the Quake ports
 * default to. AHI mixes on the 68k; the Amiga played the game's samples at well
 * under that.
 *
 * Also here: dos_sound.c's interface (the PC game's samples), which this build
 * does not play -- the Amiga's sounds are the game's own.
 *
 * The SDL build compiles none of this.
 */

#if defined(SWOS_CHUNKY_BUILD) && !defined(SWOS_SDL_BUILD)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "swos_types.h"
#include "audio_out.h"
#include "dos_sound.h"

#pragma pack(push, 2)
#include <exec/types.h>
#include <exec/memory.h>
#include <utility/hooks.h>
#include <devices/ahi.h>
#include <proto/exec.h>
#include <proto/ahi.h>
#pragma pack(pop)

/* The AHIET_CHANNELINFO hook, as 68k code: AHI calls it on its own side.
 *     hook->h_Data = info->ahieci_Offset[0]; return 0;
 * (amiga_snd.c's blob: movem.l a2,-(sp); movea.l a0,a2; move.l 12(a1),d0;
 *  move.l d0,16(a2); moveq #0,d0; movea.l (sp)+,a2; rts) */
static unsigned short s_hook_code[] = {
    0x48e7, 0x0020, 0x2448, 0x2029, 0x000c, 0x2540, 0x0010, 0x7000, 0x245f, 0x4e75
};

#pragma pack(push, 2)
struct ChannelInfo {
    struct AHIEffChannelInfo cinfo;
    ULONG                    x[1];          /* ahieci_Offset[] for the one channel */
};
#pragma pack(pop)

#define RING_FRAMES   16384                 /* 0.74 s at 22,050 Hz; a power of two */
#define FRAME_BYTES   4                     /* 16-bit stereo */
#define LEAD_FRAMES   512                   /* where writing restarts after an underrun */

struct Library *AHIBase;
static struct MsgPort      *s_port;
static struct AHIRequest   *s_req;
static int                  s_dev_open;
static struct AHIAudioCtrl *s_ctrl;
static int                  s_sound_loaded;
static int                  s_effect_on;
static struct ChannelInfo   s_info;
static struct Hook          s_hook;
static int16_t             *s_ring;         /* RING_FRAMES * 2 samples */
static uint32_t             s_write;        /* frames written, ever; the ring index is & (RING_FRAMES-1) */
static int                  s_freq;
static int                  s_open;

static void flush_range(const void *addr, uint32_t len)
{
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)31;
    uintptr_t end = ((uintptr_t)addr + len + 31) & ~(uintptr_t)31;
    for (; a < end; a += 32)
        __asm__ __volatile__ ("dcbf 0,%0" : : "r" (a) : "memory");
    __asm__ __volatile__ ("sync" ::: "memory");
}

/* Where AHI is playing, in frames into the ring: the hook's last word, read past
 * any stale copy of it in the PPC's cache. */
static uint32_t play_frame(void)
{
    flush_range(&s_hook.h_Data, sizeof s_hook.h_Data);
    return (uint32_t)(uintptr_t)s_hook.h_Data & (RING_FRAMES - 1);
}

static void restart_writer(uint32_t p)
{
    s_write = (s_write & ~(uint32_t)(RING_FRAMES - 1)) + ((p + LEAD_FRAMES) & (RING_FRAMES - 1));
}

/* Frames written and not yet played. A writer that fell behind (the game stalled
 * longer than the ring) is put back just ahead of the player. */
static uint32_t ahead(void)
{
    uint32_t p = play_frame(), w = s_write & (RING_FRAMES - 1);
    uint32_t d = (w - p) & (RING_FRAMES - 1);
    if (d > RING_FRAMES / 2) {
        restart_writer(p);
        d = LEAD_FRAMES;
    }
    return d;
}

int AudioOut_Open(int want_freq, int *got_freq)
{
    const char *env = getenv("SWOS_AHI_RATE");
    struct AHISampleInfo sample;
    (void)want_freq;
    s_freq = env && atoi(env) >= 8000 && atoi(env) <= 48000 ? atoi(env) : 22050;

    if (!(s_port = CreateMsgPort()))
        goto fail;
    if (!(s_req = (struct AHIRequest *)CreateIORequest(s_port, sizeof(struct AHIRequest))))
        goto fail;
    s_req->ahir_Version = 4;
    if (OpenDevice((CONST_STRPTR)AHINAME, AHI_NO_UNIT, (struct IORequest *)s_req, 0) != 0) {
        fprintf(stderr, "[audio] no ahi.device v4: the game plays silent\n");
        goto fail;
    }
    s_dev_open = 1;
    AHIBase = (struct Library *)s_req->ahir_Std.io_Device;

    s_ctrl = AHI_AllocAudio(AHIA_AudioID, AHI_DEFAULT_ID,
                            AHIA_MixFreq, (ULONG)s_freq,
                            AHIA_Channels, 1,
                            AHIA_Sounds, 1,
                            TAG_END);
    if (!s_ctrl) {
        fprintf(stderr, "[audio] AHI_AllocAudio failed (the default mode in AHI prefs?)\n");
        goto fail;
    }
    s_ring = (int16_t *)AllocVec(RING_FRAMES * FRAME_BYTES, MEMF_PUBLIC | MEMF_CLEAR);
    if (!s_ring)
        goto fail;
    flush_range(s_ring, RING_FRAMES * FRAME_BYTES);

    sample.ahisi_Type    = AHIST_S16S;
    sample.ahisi_Address = s_ring;
    sample.ahisi_Length  = RING_FRAMES;
    if (AHI_LoadSound(0, AHIST_DYNAMICSAMPLE, &sample, s_ctrl) != 0)
        goto fail;
    s_sound_loaded = 1;
    if (AHI_ControlAudio(s_ctrl, AHIC_Play, TRUE, TAG_END) != 0)
        goto fail;

    s_hook.h_Entry = (HOOKFUNC)s_hook_code;
    s_hook.h_Data = 0;
    s_info.cinfo.ahie_Effect = AHIET_CHANNELINFO;
    s_info.cinfo.ahieci_Func = &s_hook;
    s_info.cinfo.ahieci_Channels = 1;
    flush_range(s_hook_code, sizeof s_hook_code);
    flush_range(&s_hook, sizeof s_hook);
    flush_range(&s_info, sizeof s_info);

    AHI_Play(s_ctrl,
             AHIP_BeginChannel, 0,
             AHIP_Freq,         (ULONG)s_freq,
             AHIP_Vol,          0x10000,
             AHIP_Pan,          0x8000,
             AHIP_Sound,        0,
             AHIP_EndChannel,   0,
             TAG_END);
    AHI_SetEffect(&s_info, s_ctrl);
    s_effect_on = 1;

    s_write = LEAD_FRAMES;
    s_open = 1;
    *got_freq = s_freq;
    fprintf(stderr, "[audio] AHI: 16-bit stereo at %d Hz, a ring of %d frames\n",
            s_freq, RING_FRAMES);
    return 0;

fail:
    AudioOut_Close();
    return -1;
}

void AudioOut_Close(void)
{
    if (s_ctrl) {
        if (s_effect_on) {
            s_info.cinfo.ahie_Effect = AHIET_CHANNELINFO | AHIET_CANCEL;
            AHI_SetEffect(&s_info, s_ctrl);
            s_effect_on = 0;
        }
        AHI_ControlAudio(s_ctrl, AHIC_Play, FALSE, TAG_END);
        if (s_sound_loaded) {
            AHI_UnloadSound(0, s_ctrl);
            s_sound_loaded = 0;
        }
        AHI_FreeAudio(s_ctrl);
        s_ctrl = NULL;
    }
    if (s_ring) {
        FreeVec(s_ring);
        s_ring = NULL;
    }
    if (s_dev_open) {
        CloseDevice((struct IORequest *)s_req);
        s_dev_open = 0;
    }
    if (s_req) {
        DeleteIORequest((struct IORequest *)s_req);
        s_req = NULL;
    }
    if (s_port) {
        DeleteMsgPort(s_port);
        s_port = NULL;
    }
    AHIBase = NULL;
    s_open = 0;
}

/* Drop what is queued: silence over the whole ring, the writer back to the player. */
void AudioOut_Clear(void)
{
    if (!s_open)
        return;
    memset(s_ring, 0, RING_FRAMES * FRAME_BYTES);
    flush_range(s_ring, RING_FRAMES * FRAME_BYTES);
    restart_writer(play_frame());
}

/* Quiet at once (the game is closing): the ring goes silent from where AHI is. */
void AudioOut_Pause(int on)
{
    if (on)
        AudioOut_Clear();
}

uint32_t AudioOut_Queued(void)
{
    return s_open ? ahead() * FRAME_BYTES : 0;
}

void AudioOut_Queue(const int16_t *pcm, uint32_t bytes)
{
    uint32_t frames = bytes / FRAME_BYTES, w, first;
    if (!s_open || frames == 0)
        return;
    if (frames > RING_FRAMES / 2)
        frames = RING_FRAMES / 2;
    w = s_write & (RING_FRAMES - 1);
    first = RING_FRAMES - w;
    if (first > frames)
        first = frames;
    memcpy(s_ring + w * 2, pcm, first * FRAME_BYTES);
    flush_range(s_ring + w * 2, first * FRAME_BYTES);
    if (frames > first) {
        memcpy(s_ring, pcm + first * 2, (frames - first) * FRAME_BYTES);
        flush_range(s_ring, (frames - first) * FRAME_BYTES);
    }
    s_write += frames;
}

/* ---- dos_sound.c: the PC game's samples, never live here ---- */
void    DosSound_Event(uint8_t event, uint16_t arg) { (void)event; (void)arg; }
int32_t DosSound_Live(void)                  { return 0; }
void    DosSound_Stop(void)                  { }
void    DosSound_ChantStart(int32_t slot)    { (void)slot; }
void    DosSound_ChantVolume(int32_t volume) { (void)volume; }
void    DosSound_SetStyle(int32_t pc)        { (void)pc; }
int32_t DosSound_Style(void)                 { return 0; }
void    DosSound_SetCommentaryDir(const char *dir) { (void)dir; }
void    DosSound_SetCommentary(int32_t on)   { (void)on; }
int32_t DosSound_SelfTest(void)              { return 1; }

#endif
