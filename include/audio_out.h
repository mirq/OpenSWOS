/*
 * audio_out.h -- where audio_sdl.c's mix goes: the sound device, as a queue.
 *
 * audio_sdl.c is the game's mixer (the RJP1 music and effects, the PC game's
 * samples and music, the overlay sample) and needs from the device only this: a
 * queue of signed 16-bit stereo frames at a rate the device chose, how much of it
 * is still to play, and a way to drop it. The SDL build implements it with SDL's
 * audio queue (in audio_sdl.c itself), WarpOS with AHI (audio_wos.c).
 */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include "swos_types.h"

/* Open the device for 16-bit stereo near `want_freq`; 0 and the rate it gave in
 * *got_freq, or -1. Starts paused only if the device needs to: AudioOut_Pause(0)
 * follows. */
int      AudioOut_Open(int want_freq, int *got_freq);
void     AudioOut_Close(void);
/* 1: stop playing at once (the window closed); 0: play. */
void     AudioOut_Pause(int on);
/* Drop everything queued that has not played yet. */
void     AudioOut_Clear(void);
/* Bytes queued and not yet played (4 a stereo frame). */
uint32_t AudioOut_Queued(void);
/* Append `bytes` of interleaved S16 stereo. The device copies it. */
void     AudioOut_Queue(const int16_t *pcm, uint32_t bytes);

#endif
