/*
 * audio_wos.c -- the WarpOS build's sound: SILENT, for now.
 *
 * A PLACEHOLDER so the WarpOS build links and runs (docs/WARPOS.md, "Sound"). It
 * answers the interface audio_sdl.c and dos_sound.c give the game -- the music
 * player, the effects, the PC game's samples -- as a machine with no sound
 * device would: every call accepted, nothing played, nothing busy.
 *
 * What replaces it: audio_sdl.c split into the mixer it mostly is (RJP1 music,
 * the effect voices, the overlay sample -- none of which needs SDL) and its
 * output (SDL's audio queue), with an AHI output here beside the SDL one.
 *
 * The SDL build compiles the real files, so this one is empty there.
 */

#if defined(SWOS_CHUNKY_BUILD) && !defined(SWOS_SDL_BUILD)

#include "swos_types.h"
#include "dos_sound.h"

/* ---- audio_sdl.c: the Amiga's music and effects ---- */
int  AudioSDL_Init(int mono)                 { (void)mono; return 0; }
void AudioSDL_Shutdown(void)                 { }
void AudioSDL_Tick(void)                     { }
void AudioSDL_PauseAndClear(void)            { }

int AudioSDL_StartSong(const uint8_t *song_data, int song_size,
                       const uint8_t *ins_data,  int ins_size,
                       int subsong)
{
    (void)song_data; (void)song_size; (void)ins_data; (void)ins_size; (void)subsong;
    return 0;
}
void AudioSDL_StopSong(void)                 { }
void AudioSDL_SetSongLoop(int loop)          { (void)loop; }
int  AudioSDL_StreamStart(const char *name, int loop) { (void)name; (void)loop; return 0; }
int  AudioSDL_PcMusicStart(void)             { return 0; }

int AudioSDL_LoadSfxBank(const uint8_t *song_data, int song_size,
                         const uint8_t *ins_data,  int ins_size)
{
    (void)song_data; (void)song_size; (void)ins_data; (void)ins_size;
    return 0;
}
void AudioSDL_TriggerSfxByNoteId(int channel, int note_id) { (void)channel; (void)note_id; }
void AudioSDL_TriggerSfxByWaveId(int channel, int wave_id) { (void)channel; (void)wave_id; }
void AudioSDL_TriggerSfxPreset(int preset_id)              { (void)preset_id; }
int  AudioSDL_SfxChannelBusy(int channel)                  { (void)channel; return 0; }
void AudioSDL_StopSfx(void)                  { }
void AudioSDL_SilenceSfx(void)               { }
void AudioSDL_SetSfxSecondPlayer(int on)     { (void)on; }
void AudioSDL_SetMasterVolume(int vol_0_64)  { (void)vol_0_64; }
void AudioSDL_SetChannelVolume(int channel, int vol_0_64) { (void)channel; (void)vol_0_64; }

int  AudioSDL_LoadOverlaySample(const char *path) { (void)path; return 0; }
void AudioSDL_PlayOverlaySample(void)        { }

void AudioSDL_PcmPlay(int voice, const uint8_t *data, uint32_t len, int rate, int volume, int loops)
{
    (void)voice; (void)data; (void)len; (void)rate; (void)volume; (void)loops;
}
void AudioSDL_PcmVolume(int voice, int volume) { (void)voice; (void)volume; }
void AudioSDL_PcmStop(int voice)             { (void)voice; }
int  AudioSDL_PcmPlaying(int voice)          { (void)voice; return 0; }
void AudioSDL_PcmMaster(int vol_0_64)        { (void)vol_0_64; }

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
