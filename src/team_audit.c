/*
 * team_audit.c -- every team in the game, through the player-price code, as one hash.
 *
 * WHY THIS EXISTS. The replay corpus is 15 recordings, so it plays ~15 fixtures out of the
 * hundreds of squads in bin/data (72 TEAM.* files). Everything the cross-platform
 * comparison has proved therefore rests on those teams' player data. The one gameplay bug
 * that comparison DID find -- the goalkeeper price reading 35 bytes in front of
 * skillsPerQuadrant (b7fc214) -- was reached only by a player whose position byte decoded
 * to 0, i.e. it was the DATA that chose whether the bug fired. A bug that needs a squad the
 * corpus never loads is invisible to every test we have.
 *
 * So: load every team file, walk every team, and run every player through GetPlayerPrice --
 * the same call the match makes (AdjustPlayerSkills -> ApplyTeamTactics) -- folding the
 * results into one hash. Two machines printing the same hash have agreed about every
 * player in the game; two machines printing different ones have a data-dependent
 * divergence, and the per-file lines say which file to look in.
 *
 * It is also the cheapest way to point a sanitizer at all of that data: an out-of-bounds
 * read that only a particular squad performs shows up here in seconds, where the corpus
 * would never have loaded that squad at all.
 */
#ifdef SWOS_CHUNKY_BUILD

#include <stdio.h>
#include <string.h>

#include "swos_types.h"
#include "game_types.h"

extern uint8_t  _teamFileBuffer[];
extern int16_t  LoadTeamFile(uint16_t extNumber);
extern int16_t  GetPlayerPrice(TeamFile *teamFile, PlayerFile *playerFile, uint16_t playerNumber);
extern uint16_t _currentTeamNumber, _selTeamNumber;

static int32_t s_armed;

int32_t TeamAudit_Armed(void) { return s_armed; }
void    TeamAudit_Arm(void)   { s_armed = 1; }

/* FNV-1a, the same mixer the state hash uses: order matters, so a price landing on the
 * wrong player still changes it. */
static void mix(uint64_t *h, uint64_t v)
{
    int32_t i;
    for (i = 0; i < 8; i++) {
        *h ^= (v >> (8 * i)) & 0xFFu;
        *h *= 1099511628211ull;
    }
}

void TeamAudit_Run(void)
{
    uint64_t all = 1469598103934665603ull;
    int32_t file, teams = 0, players = 0, files = 0;

    /* GetPlayer short-circuits to the career/current team for these, which would make the
     * answer depend on menu state rather than on the file. */
    _currentTeamNumber = 0xFFFF;
    _selTeamNumber = 0xFFFF;

    printf("[team-audit] every team in bin/data through GetPlayerPrice\n");
    for (file = 0; file <= 99; file++) {
        uint64_t fh = 1469598103934665603ull;
        uint16_t count, i;
        TeamFileEx *rec;
        int32_t fileTeams = 0;

        /* ONLY files that exist. LoadTeamFile on a missing one calls ShowErrorMenu /
         * ShowFsErrorByCode -- a modal error that waits for a key, which in a headless
         * batch run is a hang, not an error. The numbering has gaps (there is no
         * TEAM.009), so this is not hypothetical: the audit stopped dead at the first one
         * and sat there until it was killed. */
        {
            char probe[64];
            FILE *pf;
            snprintf(probe, sizeof probe, "data/TEAM.%03d", file);
            if ((pf = fopen(probe, "rb")) == NULL)
                continue;
            fclose(pf);
        }
        if (LoadTeamFile((uint16_t)file) != 0)
            continue;
        count = *(uint16_t *)_teamFileBuffer;        /* host-native count, then the records */
        if (count == 0 || count > 1000)
            continue;
        rec = (TeamFileEx *)(_teamFileBuffer + 2);
        files++;
        for (i = 0; i < count; i++) {
            uint16_t p;
            /* 1..10, the range the match itself uses: ApplyTeamTactics loops i < 11 and
             * takes the GOALKEEPER's price straight from the file, so GetPlayerPrice only
             * ever sees the ten outfield players -- which is also exactly how many
             * positions a 370-byte tactics record holds (9 + 10 * 35). Looping to 16 here
             * walked off the end of _teamTactics442 and ASan said so, which is the audit
             * being wrong, not the game. */
            for (p = 1; p <= 10; p++) {
                /* GetPlayer's formula (teamBase + index * sizeof(PlayerFile)) applied to
                 * the record we already hold. Going through GetPlayer instead means
                 * GetTeamPointer, which RELOADS the team file for a team whose country is
                 * not the one currently loaded -- thousands of file loads off /mnt/c, and
                 * the audit crawled. */
                uint8_t *base = (uint8_t *)&rec[i] + (uint16_t)(p * sizeof(PlayerFile));
                int16_t price;
                if (!base)
                    continue;
                price = GetPlayerPrice(&rec[i].team, (PlayerFile *)base, p);
                mix(&fh, (uint64_t)(uint16_t)price);
                players++;
            }
            teams++;
            fileTeams++;
        }
        /* No timings here: this output is compared between machines byte for byte. */
        printf("[team-audit] TEAM.%03d  %3d teams  %016llx\n", file, fileTeams,
               (unsigned long long)fh);
        mix(&all, fh);
    }
    printf("[team-audit] %d files, %d teams, %d players: %016llx\n", files, teams, players,
           (unsigned long long)all);
    fflush(stdout);
}

#endif /* SWOS_CHUNKY_BUILD */
