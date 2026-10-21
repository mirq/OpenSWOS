/* contest_log.h -- what the match decided each time two players met at the ball
 * (docs/CONTEST_LOG.md). SDL only; written while a recording is re-simulated
 * (`--srp-resim=<file> --contest-log=<csv>`), so it is the match as finally played, not
 * the frames a peer guessed and took back.
 *
 * Every call is a no-op unless the log is open, reads state and never writes it, and
 * draws no random number: a match played with the log on is the match without it. */
#ifndef CONTEST_LOG_H
#define CONTEST_LOG_H

#ifdef SWOS_CHUNKY_BUILD

#include <stdint.h>
#include "game_types.h"

extern int32_t g_contestLogOn;

int32_t ContestLog_Open(const char *path);      /* 0 = opened */
void    ContestLog_Close(void);
void    ContestLog_SetFrame(uint32_t frame);    /* the frame about to be played */
/* Whose game the next lines come from: 0 the match as finally played, 1 or 2 that
 * player's own game the first time it showed the frame (a re-simulation on the inputs it
 * had then). `quiet` plays without writing: the guessed frames leading up to it. */
void    ContestLog_SetView(int32_t view, int32_t quiet);

/* Why a player at the ball, with an opponent at it too, got no roll. */
enum {
    CONTEST_NO_SELF_LOCKED = 1,     /* he lost the last roll: locked out */
    CONTEST_NO_NOT_ON_IT,           /* close, but not within reach to play it */
    CONTEST_NO_BALL_HIGH,           /* the ball is in the air */
    CONTEST_NO_OPP_LOCKED,          /* the opponent lost the last roll: he plays it freely */
    CONTEST_NO_OPP_NOT_ON_BALL,     /* the opponent is not "on the ball" (fire held, shooting, too far) */
    CONTEST_NO_OPP_STILL,           /* the opponent holds no direction: he just takes it */
    CONTEST_NO_OPP_NOBODY           /* the opponent has no controlled player */
};

/* The roll (CalculateIfPlayerWinsBall): `team`'s `player` reached a ball `opp` is on.
 * usedMine is the rating the roll took for `team` -- read from `team`'s roster at the
 * OPPONENT's man's squad slot, as both originals do -- and usedTheirs the opponent's
 * man's. `locked` is the team that lost. */
void ContestLog_Roll(const TeamGeneralInfo *team, const Sprite *player, int32_t usedMine,
                     int32_t usedTheirs, int32_t odds, int32_t drawn, const TeamGeneralInfo *locked);
/* A contact that was not rolled for, and why. */
void ContestLog_NoRoll(const TeamGeneralInfo *team, const Sprite *player, int32_t reason);
/* A slide tackle reached the ball: the other team is locked out, with no roll. */
void ContestLog_Tackle(const TeamGeneralInfo *team, const Sprite *player);
/* A slide has come to rest: the player lies there for `ticks` updates of his team, read
 * from a table by HIS tackling skill (SetPlayerDowntimeAfterTackle: 30 for the worst
 * tackler down to 9 for the best; a computer player 3). `ticks` is in the roll's "drawn"
 * column. */
void ContestLog_Down(const TeamGeneralInfo *team, const Sprite *player, int32_t ticks);
/* The man on the ball turned for longer than his ball control allows and is locked out
 * himself: `steps` turning steps against the `allowed` his skill gives him. */
void ContestLog_Turn(const TeamGeneralInfo *team, const Sprite *player, int32_t steps, int32_t allowed);
/* A player took over a moving ball (the trap): call before and after the ball is placed. */
void ContestLog_TrapBefore(void);
void ContestLog_Trap(const TeamGeneralInfo *team, const Sprite *player);

#define CONTEST_LOG(call) do { if (g_contestLogOn) { call; } } while (0)

#else

#define CONTEST_LOG(call) do { } while (0)

#endif
#endif
