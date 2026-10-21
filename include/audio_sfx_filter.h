/*
 * audio_sfx_filter.h -- a gate every sound effect the game starts passes through (SDL builds).
 *
 * Netplay installs one: a rollback plays frames again, and the effects those frames start
 * were heard the first time (docs/NETPLAY.md, "Sound during a rollback"). Only the effects
 * are filtered. Volume changes are left alone -- a fade played again ends on the right value.
 */
#ifndef AUDIO_SFX_FILTER_H
#define AUDIO_SFX_FILTER_H

#ifdef SWOS_CHUNKY_BUILD

#include <stdint.h>

enum {
    AUDIO_SFX_PRESET = 1,       /* AudioApplyPresetToChannels: `id` is the preset, channel 0xFF */
    AUDIO_SFX_WAVE   = 2,       /* AudioChannelQueueNextWaveIfEnabled: `id` is the wave */
    AUDIO_SFX_VOLUME = 3,       /* AudioSetChannelVolume: `id` is the volume, channel 0xFF = all
                                 * (observer only -- volume changes are never filtered) */
    AUDIO_SFX_DOS    = 4        /* Audio_DosEvent (dos_sound.h): `channel` is the event, `id` its
                                 * argument. DOS_EV_TICK is filtered but never observed. */
};

/* Nonzero plays the effect. */
typedef int32_t (*AudioSfxFilter)(uint8_t kind, uint8_t channel, uint16_t id);

/* NULL, the default, plays everything. */
void Audio_SetSfxFilter(AudioSfxFilter filter);

/* Told of every effect the game starts and every channel volume it sets, BEFORE the filter
 * decides whether it is heard: the replay recorder (src/replay_v2.c) keeps what the match
 * did, whatever a rollback let through. NULL, the default, for none. */
typedef void (*AudioSfxObserver)(uint8_t kind, uint8_t channel, uint16_t id);
void Audio_SetSfxObserver(AudioSfxObserver observer);

#endif /* SWOS_CHUNKY_BUILD */
#endif /* AUDIO_SFX_FILTER_H */
