/*
 * music_stream.h -- a music track played from a compressed file: the PC CD release's
 * title song (SDL builds; docs/DOS_SOUND.md, "The CD track").
 */
#ifndef MUSIC_STREAM_H
#define MUSIC_STREAM_H

#ifdef SWOS_CHUNKY_BUILD

#include <stdint.h>

/* Called by the audio layer (src/audio_sdl.c) under its lock.
 * Start sound_dos/<name>.mp3 from the beginning; nonzero when it is playing. */
int32_t MusicStream_Start(const char *name, int32_t loop);
void    MusicStream_Stop(void);
int32_t MusicStream_Playing(void);
/* Add `frames` stereo frames at `rate` Hz to `out`, at gain/256. */
void    MusicStream_Render(int16_t *out, int32_t frames, int32_t rate, int32_t gain);

/* Whether sound_dos/<name>.mp3 is there (no lock needed). */
int32_t MusicStream_Exists(const char *name);

#endif /* SWOS_CHUNKY_BUILD */
#endif /* MUSIC_STREAM_H */
