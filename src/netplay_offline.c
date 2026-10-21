/*
 * netplay_offline.c -- what a build without online play answers.
 *
 * The chunky target without SDL (WarpOS, docs/WARPOS.md) plays offline only: it
 * builds none of the netplay, lobby, matchmaking, recorder or online-menu sources
 * (WOS_EXCLUDE_SRCS in Makefile.warpos). The shared game loop still asks whether a
 * match is live, and main() still parses the online options and runs their
 * self-tests, so these give the answers the real files give when nothing online
 * is running -- no match is live, nothing owns the clock, no start menu -- and
 * every online self-test and option says it is not in this build.
 *
 * The SDL build compiles the real files, so this one is empty there.
 */

#if defined(SWOS_CHUNKY_BUILD) && !defined(SWOS_SDL_BUILD)

#include <stdio.h>

#include "match_record.h"
#include "match_report.h"
#include "mm_client.h"
#include "netplay.h"
#include "netplay_live_test.h"
#include "netplay_match.h"
#include "online_menus.h"
#include "p2p_link.h"

static int32_t not_here(const char *what)
{
    fprintf(stderr, "%s: there is no online play in this build\n", what);
    return 1;
}

/* ---- netplay_match.c: no match is ever live ---- */
int32_t NetplayMatch_Enabled(void)    { return 0; }
int32_t NetplayMatch_OwnsClock(void)  { return 0; }
int32_t NetplayMatch_Start(void)      { return 0; }
int32_t NetplayMatch_IsLive(void)     { return 0; }
int32_t NetplayMatch_IsP2P(void)      { return 0; }
/* Only reached with NetplayMatch_Enabled(), which is never true here. */
int32_t NetplayMatch_Frame(void)      { return 0; }
int32_t NetplayMatch_Finish(void)     { return 0; }
void    NetplayMatch_SetBalance(int32_t on) { (void)on; }

int32_t NetplayMatch_LiveOutcome(const char **why)
{
    if (why)
        *why = "";
    return NETPLAY_LIVE_NONE;
}

/* A --netplay-* option: refused (-1, a bad value), so main() stops on it. main()
 * asks ArgError on every run, so it must stay NULL: nothing given together is wrong. */
int32_t NetplayMatch_ParseArg(const char *arg)  { (void)arg; return -1; }
const char *NetplayMatch_ArgError(void)         { return NULL; }

/* ---- the self-tests and test rigs ---- */
int32_t Netplay_SelfTest(void)        { return not_here("--netplay-selftest (GGPO)"); }
int32_t P2PLink_SelfTest(void)        { return not_here("--netplay-selftest (link)"); }
int32_t OnlineMenus_SelfTest(void)    { return not_here("--online-menus-selftest"); }
int32_t MmClient_SelfTest(const char *host, uint16_t port)
{
    (void)host; (void)port;
    return not_here("--mm-selftest");
}
int32_t MmClient_PairTest(const char *role, const char *host, uint16_t port)
{
    (void)role; (void)host; (void)port;
    return not_here("--mm-pair-test");
}
int32_t NetplayLiveTest_Arm(const char *role)       { (void)role; return not_here("--live-test"); }
int32_t NetplayLiveTest_StateDiff(const char *spec) { (void)spec; return not_here("--state-diff"); }
int32_t NetplayLiveTest_StreamDiff(const char *spec){ (void)spec; return not_here("--stream-diff"); }
int32_t NetplayLiveTest_Armed(void)   { return 0; }
void    NetplayLiveTest_Run(void)     { }

/* ---- settings main() hands on: nothing to configure ---- */
const char *MmClient_DefaultHost(void)               { return ""; }
uint16_t    MmClient_DefaultPort(void)               { return 0; }
void        MmClient_SetConfiguredServer(const char *spec) { (void)spec; }
void        MatchRecord_SetConfigured(const char *spec)    { (void)spec; }
void        MatchReport_SetEnabled(int32_t on)             { (void)on; }
void        MatchReport_Flush(uint32_t waitMs)             { (void)waitMs; }

/* ---- online_menus.c: the MULTIPLAYER flow ---- */
void    OnlineMenus_Init(void)                       { }
int32_t OnlineMenus_ShowStartMenu(void)              { return 0; }
void    OnlineMenus_SetStartMenu(const char *name)   { (void)name; }
void    OnlineMenus_SetAutoLogin(const char *name)   { (void)name; }
int32_t OnlineMenus_SetLoopbackLobby(const char *role) { (void)role; return -1; }

#endif
