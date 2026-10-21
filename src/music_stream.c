/*
 * music_stream.c -- a music track decoded as it plays. See include/music_stream.h.
 *
 * The file is read whole (a few megabytes) and decoded by dr_mp3 (third_party/dr_mp3,
 * public domain) a tick's worth at a time on the audio thread, then brought to the
 * device's rate by linear interpolation when the two differ.
 */
#ifdef SWOS_CHUNKY_BUILD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../third_party/dr_mp3/dr_mp3.h"

#include "music_stream.h"
#include "swos_io.h"

static drmp3    s_mp3;
static uint8_t *s_file;
static int32_t  s_playing, s_loop;
static int16_t  s_prev[2], s_next[2];       /* the two source frames an output frame lies between */
static uint32_t s_phase;                    /* 16.16 between them */
static int32_t  s_primed;

static const char *path_of(const char *name, char *buf, uint32_t size)
{
    char rel[96];
    snprintf(rel, sizeof rel, "sound_dos/%s.mp3", name);
    return swos_data_path(rel, buf, size);
}

int32_t MusicStream_Exists(const char *name)
{
    char anchored[1024];
    FILE *f = fopen(path_of(name, anchored, (uint32_t)sizeof anchored), "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

void MusicStream_Stop(void)
{
    if (s_file) {
        drmp3_uninit(&s_mp3);
        free(s_file);
        s_file = NULL;
    }
    s_playing = 0;
}

int32_t MusicStream_Start(const char *name, int32_t loop)
{
    char anchored[1024];
    FILE *f;
    int32_t size;

    MusicStream_Stop();
    f = fopen(path_of(name, anchored, (uint32_t)sizeof anchored), "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    size = (int32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0 && size < 64 * 1024 * 1024) {
        s_file = (uint8_t *)malloc((size_t)size);
        if (s_file && fread(s_file, 1, (size_t)size, f) != (size_t)size) {
            free(s_file);
            s_file = NULL;
        }
    }
    fclose(f);
    if (!s_file)
        return 0;
    if (!drmp3_init_memory(&s_mp3, s_file, (size_t)size, NULL) || s_mp3.channels < 1 || s_mp3.channels > 2) {
        fprintf(stderr, "[music] sound_dos/%s.mp3 is not an MP3 this can play\n", name);
        free(s_file);
        s_file = NULL;
        return 0;
    }
    s_loop = loop;
    s_phase = 0;
    s_primed = 0;
    s_prev[0] = s_prev[1] = s_next[0] = s_next[1] = 0;
    s_playing = 1;
    return 1;
}

int32_t MusicStream_Playing(void)
{
    return s_playing;
}

/* The next frame of the file, as stereo. 0 at the end of a track that does not repeat. */
static int32_t next_frame(int16_t *lr)
{
    drmp3_int16 in[2];
    if (drmp3_read_pcm_frames_s16(&s_mp3, 1, in) != 1) {
        if (!s_loop || !drmp3_seek_to_pcm_frame(&s_mp3, 0) || drmp3_read_pcm_frames_s16(&s_mp3, 1, in) != 1)
            return 0;
    }
    lr[0] = in[0];
    lr[1] = s_mp3.channels == 2 ? in[1] : in[0];
    return 1;
}

void MusicStream_Render(int16_t *out, int32_t frames, int32_t rate, int32_t gain)
{
    uint32_t step;
    int32_t i;

    if (!s_playing || rate <= 0 || !s_mp3.sampleRate)
        return;
    step = (uint32_t)(((uint64_t)s_mp3.sampleRate << 16) / (uint32_t)rate);
    if (!s_primed) {
        s_primed = 1;
        if (!next_frame(s_prev) || !next_frame(s_next)) {
            s_playing = 0;
            return;
        }
    }
    for (i = 0; i < frames; i++) {
        int32_t c;
        while (s_phase >= 0x10000) {
            s_phase -= 0x10000;
            s_prev[0] = s_next[0];
            s_prev[1] = s_next[1];
            if (!next_frame(s_next)) {
                s_playing = 0;
                return;
            }
        }
        for (c = 0; c < 2; c++) {
            int32_t v = s_prev[c] + (int32_t)(((int64_t)(s_next[c] - s_prev[c]) * s_phase) >> 16);
            v = out[i * 2 + c] + v * gain / 256;
            out[i * 2 + c] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
        s_phase += step;
    }
}

#endif /* SWOS_CHUNKY_BUILD */
