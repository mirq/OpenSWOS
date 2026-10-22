/*
 * auto_match.c -- SWOS_AUTO_MATCH: computer against computer with nobody at the
 * controls, for testing a port's speed and stability on its own machine.
 *
 *   SWOS_AUTO_MATCH=<n>   play n friendlies (0 or empty: keep playing), then quit
 *
 * Skips the front end, as the replay rig and the arcade mode do: two national
 * teams from TEAM.080 (a different pair each match), both TEAM_COMPUTER, the
 * shortest match clock, no substitutes. The PLAY MATCH prompts a computer-only
 * friendly would show are skipped too (AutoMatch_SetupPlayers, from SetupPlayers).
 * Not for the arcade, netplay or the rigs: a front-end start, like any friendly.
 */
#ifdef SWOS_CHUNKY_BUILD

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_types.h"
#include "image_cmd.h"      /* ProcessImageCommandStream */
#include "match_sim.h"      /* InitTeams */
#include "menus.h"          /* InitUserTactics */

extern TeamFileEx _selectedTeams[];
extern uint16_t   _numSelectedTeams;
extern uint8_t    _teamFileBuffer[];
extern int16_t    LoadTeamFile(uint16_t extNumber);
extern void       InitAndPlayGame(TeamFileEx *team1, TeamFileEx *team2);
extern void       StopMusic(void);
extern uint16_t   _menuBGImageCmdStream[];
extern uint8_t   *_menuImageStreamBaseEnd;
extern volatile uint16_t _quitToDos;

extern uint16_t _gameType, _pitchTypeOrSeason, _friendlyPitchType, _maxSubstitutes;
extern uint16_t _minSubstitutes, _gameLength, _trainingGame, _isCareer;
extern uint16_t _twoPlayers, _exitGameFlag;
extern uint16_t _pl1Coach, _pl2Coach;
extern uint16_t _topTeamPlayerNo, _topTeamCoachNo, _bottomTeamPlayerNo, _bottomTeamCoachNo;
extern uint16_t _pl1Tactics, _pl2Tactics;
extern uint16_t _team1SelectedPlayerIndex, _team2SelectedPlayerIndex;

enum { kNationalTeamsFile = 0x50, kTeamComputer = 1 };

static int      s_armed = -1;      /* -1: SWOS_AUTO_MATCH not read yet */
static int32_t  s_matches;         /* how many to play, 0 = no end */
static uint32_t s_seed = 0x5eed1234u;

int AutoMatch_Armed(void)
{
    if (s_armed < 0) {
        const char *v = getenv("SWOS_AUTO_MATCH");
        s_armed = v != NULL;
        s_matches = v ? (int32_t)atoi(v) : 0;
    }
    return s_armed;
}

static uint32_t next_rand(void)
{
    s_seed = s_seed * 1103515245u + 12345u;
    return s_seed >> 8;
}

/* The ord-th team of the national-teams file into _selectedTeams[slot], computer
 * controlled. 0 when it is there. */
static int load_team(int slot, uint16_t pick)
{
    uint16_t count;
    TeamFileEx *rec;

    if (LoadTeamFile(kNationalTeamsFile) != 0) {
        fprintf(stderr, "[auto] TEAM.%03X does not load\n", kNationalTeamsFile);
        return -1;
    }
    /* _teamFileBuffer: [u16 host-native count][count * TeamFileEx] (arcade.c) */
    count = *(uint16_t *)_teamFileBuffer;
    if (count == 0)
        return -1;
    rec = (TeamFileEx *)(_teamFileBuffer + 2);
    memcpy(&_selectedTeams[slot], &rec[pick % count], sizeof(TeamFileEx));
    _selectedTeams[slot].team.teamControls = kTeamComputer;
    fprintf(stderr, "[auto] team %d: %.*s\n", slot + 1,
            (int)sizeof _selectedTeams[slot].team.teamName,
            (const char *)_selectedTeams[slot].team.teamName);
    return 0;
}

/* SetupPlayers' two PLAY MATCH prompts, answered: nobody plays, nobody coaches. */
void AutoMatch_SetupPlayers(TeamFileEx *team1, TeamFileEx *team2)
{
    _isCareer = 0;
    _exitGameFlag = 0;
    _team1SelectedPlayerIndex = 0xFFFF;
    _team2SelectedPlayerIndex = 0xFFFF;
    _twoPlayers = 0;
    _pl1Coach = 0;
    _pl2Coach = 0;
    _topTeamPlayerNo = _topTeamCoachNo = 0;
    _bottomTeamPlayerNo = _bottomTeamCoachNo = 0;
    _pl1Tactics = (uint16_t)(int16_t)(int8_t)team1->team.tactics;
    _pl2Tactics = (uint16_t)(int16_t)(int8_t)team2->team.tactics;
}

void AutoMatch_Run(void)
{
    int32_t played = 0;

    fprintf(stderr, "[auto] computer against computer, %s\n",
            s_matches > 0 ? "a set number of matches" : "until quit");
    /* What the front end would have done first (replay_test.c's applySetup). */
    InitUserTactics();

    while (!_quitToDos && (s_matches <= 0 || played < s_matches)) {
        uint16_t a = (uint16_t)(next_rand() % 64u), b = (uint16_t)(next_rand() % 63u);
        if (b >= a)
            b++;                        /* never the same team twice */
        _numSelectedTeams = 0;
        if (load_team(0, a) != 0 || load_team(1, b) != 0)
            break;
        _numSelectedTeams = 2;

        _gameType = 0;                  /* a friendly */
        _trainingGame = 0;
        _isCareer = 0;
        _pitchTypeOrSeason = 0;
        _friendlyPitchType = (uint16_t)(next_rand() % 7u);
        _maxSubstitutes = 0;
        _minSubstitutes = 0;
        _gameLength = 0;                /* the shortest match */

        /* The stadium line-up's little players come from menus.raw, which the
         * skipped front end never extracted (arcade.c, replay_test.c). */
        ProcessImageCommandStream(_menuBGImageCmdStream, _menuImageStreamBaseEnd);
        InitTeams();
        StopMusic();
        fprintf(stderr, "[auto] match %d: kick-off\n", (int)played + 1);
        InitAndPlayGame(&_selectedTeams[0], &_selectedTeams[1]);
        played++;
        fprintf(stderr, "[auto] match %d over\n", (int)played);
    }
    fprintf(stderr, "[auto] %d played, quitting\n", (int)played);
}

#endif /* SWOS_CHUNKY_BUILD */
