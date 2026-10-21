/* contest_log.c -- include/contest_log.h, docs/CONTEST_LOG.md. SDL only. */
#ifdef SWOS_CHUNKY_BUILD

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "contest_log.h"

extern Sprite _ballSprite;
extern TeamGeneralInfo _topTeamData, _bottomTeamData;
extern TeamGeneralInfo *_lastTeamPlayed;
extern PlayerInfo *GetPlayerPointerFromShirtNumber(Sprite *playerSprite, TeamGeneralInfo *teamData);

int32_t g_contestLogOn;

static FILE    *s_file;
static FILE    *s_open;         /* the file while it is muted (ContestLog_SetView) */
static uint32_t s_frame;
static int32_t  s_view;
static struct { int16_t x, y, z, speed; int32_t owner; } s_before;   /* the ball, before a trap */

int32_t ContestLog_Open(const char *path)
{
    ContestLog_Close();
    s_file = fopen(path, "w");
    if (!s_file)
        return 1;
    /* One line an event. who/opp are "P1"/"P2" (the netplay player numbers), then the
     * man: squad slot, name, tackling, ball control. */
    fprintf(s_file, "frame,event,who,slot,name,tackling,control,dist2,"
                    "opp,opp_slot,opp_name,opp_tackling,opp_control,opp_dist2,"
                    "used_who,used_from,used_opp,diff,odds,drawn,locked,"
                    "reason,ball_x,ball_y,ball_z,ball_speed,jump,from,view\n");
    s_open = s_file;
    s_view = 0;
    g_contestLogOn = 1;
    return 0;
}

void ContestLog_Close(void)
{
    if (s_open)
        fclose(s_open);
    s_file = s_open = NULL;
    g_contestLogOn = 0;
}

void ContestLog_SetView(int32_t view, int32_t quiet)
{
    s_view = view;
    s_file = quiet ? NULL : s_open;
}

void ContestLog_SetFrame(uint32_t frame)
{
    s_frame = frame;
}

static const char *who(const TeamGeneralInfo *team)
{
    if (!team)
        return "";
    return team->playerNumber == 1 ? "P1" : team->playerNumber == 2 ? "P2" : "CPU";
}

/* A name with nothing in it a CSV reader trips on. */
static void put_name(const PlayerInfo *info)
{
    int32_t i;
    for (i = 0; info && i < 15 && info->shortName[i]; i++) {
        char c = (char)info->shortName[i];
        fputc(c == ',' || c == '"' || c < ' ' ? ' ' : c, s_file);
    }
}

/* slot,name,tackling,control,dist2 for a team's man (empty fields when there is none). */
static void put_man(const TeamGeneralInfo *team, const Sprite *player)
{
    const PlayerInfo *info;
    if (!team || !player) {
        fprintf(s_file, ",,,,");
        return;
    }
    info = GetPlayerPointerFromShirtNumber((Sprite *)player, (TeamGeneralInfo *)team);
    fprintf(s_file, "%d,", (int)player->playerOrdinal);
    put_name(info);
    fprintf(s_file, ",%d,%d,%d", (int)info->tackling, (int)info->ballControl, (int)player->ballDistance);
}

static void put_ball(void)
{
    fprintf(s_file, "%d,%d,%d,%d", (int)_ballSprite.x.s.i, (int)_ballSprite.y.s.i,
            (int)_ballSprite.z.s.i, (int)_ballSprite.speed);
}

void ContestLog_Roll(const TeamGeneralInfo *team, const Sprite *player, int32_t usedMine,
                     int32_t usedTheirs, int32_t odds, int32_t drawn, const TeamGeneralInfo *locked)
{
    const TeamGeneralInfo *opp = team->opponentsTeam;
    const Sprite *theirs = opp->controlledPlayer;
    int32_t diff = usedMine - usedTheirs;

    if (!s_file)
        return;
    fprintf(s_file, "%u,roll,%s,", s_frame, who(team));
    put_man(team, player);
    fprintf(s_file, ",%s,", who(opp));
    put_man(opp, theirs);
    /* The rating taken for `team` is its roster's man in the opponent's man's squad slot. */
    fprintf(s_file, ",%d,", (int)usedMine);
    if (theirs)
        put_name(GetPlayerPointerFromShirtNumber((Sprite *)theirs, (TeamGeneralInfo *)team));
    fprintf(s_file, ",%d,%d,%d,%d,%s,,", (int)usedTheirs, (int)(diff < 0 ? -diff : diff),
            (int)odds, (int)drawn, who(locked));
    put_ball();
    fprintf(s_file, ",,,%d\n", (int)s_view);
}

static const char *const k_reason[] = {
    "", "self-locked", "not-on-it", "ball-high", "opp-locked", "opp-not-on-ball", "opp-still",
    "opp-nobody"
};

void ContestLog_NoRoll(const TeamGeneralInfo *team, const Sprite *player, int32_t reason)
{
    const TeamGeneralInfo *opp = team->opponentsTeam;
    const Sprite *theirs = opp->controlledPlayer;

    /* A CONTACT is two men at the ball: the opponent's within the engine's own "close"
     * (squared distance 72, about 8 pixels). One man alone at the ball is just play. */
    if (!s_file || !theirs || theirs->ballDistance > 72)
        return;
    fprintf(s_file, "%u,no-roll,%s,", s_frame, who(team));
    put_man(team, player);
    fprintf(s_file, ",%s,", who(opp));
    put_man(opp, theirs);
    fprintf(s_file, ",,,,,,,,%s,", reason >= 1 && reason <= 7 ? k_reason[reason] : "?");
    put_ball();
    fprintf(s_file, ",,,%d\n", (int)s_view);
}

void ContestLog_Tackle(const TeamGeneralInfo *team, const Sprite *player)
{
    const TeamGeneralInfo *opp = team->opponentsTeam;
    if (!s_file)
        return;
    fprintf(s_file, "%u,tackle,%s,", s_frame, who(team));
    put_man(team, player);
    fprintf(s_file, ",%s,", who(opp));
    put_man(opp, opp->controlledPlayer);
    fprintf(s_file, ",,,,,,,%s,%s,", who(opp), opp->playerHasBall ? "opp-on-ball" : "loose-ball");
    put_ball();
    fprintf(s_file, ",,,%d\n", (int)s_view);
}

void ContestLog_Down(const TeamGeneralInfo *team, const Sprite *player, int32_t ticks)
{
    if (!s_file)
        return;
    fprintf(s_file, "%u,down,%s,", s_frame, who(team));
    put_man(team, player);
    fprintf(s_file, ",,,,,,,,,,,,%d,,%s,", (int)ticks,
            (uint16_t)player->tacklingTimer == 0xFFFF ? "computer" : "by-tackling-skill");
    put_ball();
    fprintf(s_file, ",,,%d\n", (int)s_view);
}

void ContestLog_Turn(const TeamGeneralInfo *team, const Sprite *player, int32_t steps, int32_t allowed)
{
    const TeamGeneralInfo *opp = team->opponentsTeam;
    if (!s_file)
        return;
    fprintf(s_file, "%u,turn,%s,", s_frame, who(team));
    put_man(team, player);
    fprintf(s_file, ",%s,", who(opp));
    put_man(opp, opp->controlledPlayer);
    /* odds = the turning steps his ball control allows, drawn = the steps he took. */
    fprintf(s_file, ",,,,,%d,%d,%s,,", (int)allowed, (int)steps, who(team));
    put_ball();
    fprintf(s_file, ",,,%d\n", (int)s_view);
}

void ContestLog_TrapBefore(void)
{
    s_before.x = _ballSprite.x.s.i;
    s_before.y = _ballSprite.y.s.i;
    s_before.z = _ballSprite.z.s.i;
    s_before.speed = _ballSprite.speed;
    s_before.owner = _lastTeamPlayed == &_topTeamData ? 1 : _lastTeamPlayed == &_bottomTeamData ? 2 : 0;
}

void ContestLog_Trap(const TeamGeneralInfo *team, const Sprite *player)
{
    int32_t dx = _ballSprite.x.s.i - s_before.x, dy = _ballSprite.y.s.i - s_before.y;
    const TeamGeneralInfo *from = s_before.owner == 1 ? &_topTeamData :
                                  s_before.owner == 2 ? &_bottomTeamData : NULL;
    if (!s_file)
        return;
    fprintf(s_file, "%u,trap,%s,", s_frame, who(team));
    put_man(team, player);
    /* The ball as it was, how far it was moved (squared), and whose it had been. */
    fprintf(s_file, ",,,,,,,,,,,,,,,%d,%d,%d,%d,%d,%s,%d\n", (int)s_before.x, (int)s_before.y,
            (int)s_before.z, (int)s_before.speed, (int)(dx * dx + dy * dy), who(from), (int)s_view);
}

#endif /* SWOS_CHUNKY_BUILD */
