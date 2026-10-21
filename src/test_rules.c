/* The testers' rule -- no injuries, no cards -- and when it is in force.
 * See include/test_rules.h. */
#include "test_rules.h"

#ifdef SWOS_TEST_RULES

#ifdef SWOS_CHUNKY_BUILD
extern int ReplayTest_Armed(void);      /* replay_test.c: the replay corpus */
extern int ReplayTest_Running(void);
extern int Rollback_Enabled(void);      /* rollback_rig.c */
#endif
#ifdef SWOS_NETPLAY
extern int32_t NetplayMatch_Enabled(void);  /* netplay_match.c: a live match, or a test of one */
extern int32_t NetplayLobby_IsOpen(void);   /* netplay_lobby.c: the lobby that sets one up */
#endif

static int32_t s_setting;       /* [testing] no_injuries_cards */
static int32_t s_forced = -1;   /* a recording's own rule, while it is replayed */

void TestRules_SetSetting(int32_t on) { s_setting = on != 0; }
int32_t TestRules_Setting(void)       { return s_setting; }
void TestRules_Force(int32_t rule)    { s_forced = rule < 0 ? -1 : (rule != 0); }
static int32_t s_online;        /* since an online menu opened, until the main menu is back */
void TestRules_Online(int32_t online) { s_online = online != 0; }

int32_t TestRules_NoInjuriesCards(void)
{
    if (s_forced >= 0)
        return s_forced;
    if (!s_setting || s_online)
        return 0;
#ifdef SWOS_NETPLAY
    /* Online: the match, and the lobby before it -- the rule decides who may be picked,
     * and a host with it on set up a different match from one without (2026-10-11). */
    if (NetplayMatch_Enabled() || NetplayLobby_IsOpen())
        return 0;
#endif
#ifdef SWOS_CHUNKY_BUILD
    if (ReplayTest_Armed() || ReplayTest_Running() || Rollback_Enabled())
        return 0;
#endif
    return 1;
}

#else
/* ISO C forbids an empty translation unit. */
typedef int test_rules_compiled_out_t;
#endif
