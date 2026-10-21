/*
 * dos_music.h -- the PC game's menu music: menu.xmi played on an emulated AdLib chip
 * with the game's own instruments (SDL builds; docs/DOS_SOUND.md, "The music").
 */
#ifndef DOS_MUSIC_H
#define DOS_MUSIC_H

#ifdef SWOS_CHUNKY_BUILD

#include <stdint.h>

/* [audio] music / --music: 0 the Amiga's menu music, 1 the PC's. */
void    DosMusic_SetStyle(int32_t pc);
int32_t DosMusic_Style(void);
/* [audio] menu_music / --menu-music, with the PC's music: 0 menu.xmi on the emulated
 * chip, 1 a recording of it, sound_dos/menu.mp3. */
void    DosMusic_SetMenuRecording(int32_t on);
int32_t DosMusic_MenuRecording(void);

/* Read sound_dos/menu.xmi and sound_dos/sample.ad (once). Nonzero when the song can be played. */
int32_t DosMusic_Load(void);

/* The three below are called by the audio layer (src/audio_sdl.c) under its lock:
 * the song is rendered on the audio thread. */
void    DosMusic_Start(void);           /* from the beginning; it repeats */
void    DosMusic_Stop(void);
int32_t DosMusic_Playing(void);
/* Add `frames` stereo frames at `rate` Hz to `out`, at gain/256. */
void    DosMusic_Render(int16_t *out, int32_t frames, int32_t rate, int32_t gain);

/* --dos-music-selftest: plays the song through once without sound and reports what it
 * found. Returns the number of failures. */
int32_t DosMusic_SelfTest(void);

#endif /* SWOS_CHUNKY_BUILD */
#endif /* DOS_MUSIC_H */
