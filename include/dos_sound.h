/*
 * dos_sound.h -- the PC game's sounds: its sampled effects, crowd and commentary
 * (SDL builds; docs/DOS_SOUND.md).
 *
 * The match code says WHAT happened, through DOS_SOUND(event, arg), at the places
 * the PC game calls its sound routines. An event is not a sound: it goes through the
 * same gate as the Amiga's effects (audio_sfx_filter.h), so a rollback does not
 * repeat it and a recording keeps it, and only then reaches src/dos_sound.c, which
 * owns everything that follows from it -- which line of commentary, the delays, the
 * "not the same line twice" memory. None of that is match state: the match never
 * reads it back, it draws from a random generator of its own, and two players may
 * have different sound styles in one online match.
 *
 * The events are sent whatever the style, so that a recording made with the Amiga's
 * sound plays with the PC's and the other way round.
 */
#ifndef DOS_SOUND_H
#define DOS_SOUND_H

/* Never renumber: the numbers are in recordings (docs/REPLAY_FORMAT_V2.md, AUDI kind 4). */
enum {
    DOS_EV_MATCH_START   = 1,   /* arg: the top team's primary shirt colour | 0x100 when training */
    DOS_EV_MATCH_END     = 2,   /* the PC game's StopAudio */
    DOS_EV_TICK          = 3,   /* PlayEnqueuedSamples: once a frame. arg bit 0: a penalty shoot-out */

    DOS_EV_KICK          = 10,
    DOS_EV_BOUNCE        = 11,
    DOS_EV_WHISTLE       = 12,
    DOS_EV_FOUL_WHISTLE  = 13,
    DOS_EV_END_WHISTLE   = 14,
    DOS_EV_GOAL_CROWD    = 15,  /* PlayHomeGoalSample / PlayAwayGoalSample: one sample */
    DOS_EV_MISS_CROWD    = 16,  /* PlayMissGoalSample */

    DOS_EV_GOAL          = 20,  /* arg bit 0: a penalty shoot-out (penaltiesState == -1) */
    DOS_EV_OWN_GOAL      = 21,
    DOS_EV_NEAR_MISS     = 22,  /* arg bit 0 as DOS_EV_GOAL */
    DOS_EV_KEEPER_SAVED  = 23,  /* arg bit 0 as DOS_EV_GOAL */
    DOS_EV_KEEPER_CLAIMED = 24,
    DOS_EV_WOODWORK      = 25,  /* arg: 1 when the PC game's flag picks the post line over the bar's */
    DOS_EV_PENALTY       = 26,
    DOS_EV_FOUL          = 27,
    DOS_EV_DANGEROUS_PLAY = 28,
    DOS_EV_GOOD_TACKLE   = 29,
    DOS_EV_PASS          = 30,  /* a pass to a team mate: every fifth is a "good pass" */
    DOS_EV_CORNER        = 31,
    DOS_EV_THROW_IN      = 32,
    DOS_EV_YELLOW_CARD   = 33,
    DOS_EV_RED_CARD      = 34,
    DOS_EV_SUBSTITUTION  = 35,
    DOS_EV_TACTICS       = 36,
    DOS_EV_SCORE         = 37,  /* after a goal. arg: team 1's goals << 8 | team 2's */
    DOS_EV_GAME_OVER     = 38,  /* arg as DOS_EV_SCORE */
    /* Not the PC game's: SWOS 2020's two extra commentary categories, heard only from a
     * commentary folder (docs/DOS_SOUND.md, "Your own commentary"). arg: 1 when the
     * player's team is a human's. */
    DOS_EV_HEADER        = 39,
    DOS_EV_INJURY        = 40
};

#ifdef SWOS_CHUNKY_BUILD

#include <stdint.h>

/* The gate (src/audio.c): observer, rollback filter, then DosSound_Event. */
void Audio_DosEvent(uint8_t event, uint16_t arg);
#define DOS_SOUND(event, arg)   Audio_DosEvent((uint8_t)(event), (uint16_t)(arg))

/* [audio] style / --sound-style: 0 the Amiga's sound, 1 the PC's.
 * [audio] commentary: the PC game's commentary -- 0 off, 1 on (over the Amiga's sound
 * too), 2 auto: with the PC's sound only. */
void    DosSound_SetStyle(int32_t pc);
int32_t DosSound_Style(void);
void    DosSound_SetCommentary(int32_t on);
/* [audio] commentary_dir: the folder of commentary categories, relative to the game's
 * data or absolute. NULL or "" is sound_dos/commentary. Before DosSound_SetStyle. */
void    DosSound_SetCommentaryDir(const char *dir);

/* The PC's effects, crowd and chants have this match: the Amiga's effects are not
 * started. (Commentary alone, over the Amiga's sound, does not count.) */
int32_t DosSound_Live(void);

/* An event that passed the gate. */
void DosSound_Event(uint8_t event, uint16_t arg);

/* What the Amiga's crowd chant code does, which the PC chants follow: a chant
 * starting (its place in _crowdSamples) and the chant channel's volume, 0..64. */
void DosSound_ChantStart(int32_t slot);
void DosSound_ChantVolume(int32_t volume);

/* Everything off: the match is over, or the sound is being reset under it. */
void DosSound_Stop(void);

/* --dos-sound-selftest: every file of the tables is there and every table entry
 * names one. Returns the number of failures. */
int32_t DosSound_SelfTest(void);

#else
#define DOS_SOUND(event, arg)   ((void)0)
#endif

#endif /* DOS_SOUND_H */
