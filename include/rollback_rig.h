/*
 * rollback_rig.h -- the rollback state test rig (SDL builds). docs/ROLLBACK.md.
 *
 * Runs inside the replay rig's headless match (src/replay_test.c), using the
 * recording for its setup and its inputs only.
 */
#ifndef ROLLBACK_RIG_H
#define ROLLBACK_RIG_H

#ifdef SWOS_CHUNKY_BUILD

#include "swos_types.h"

typedef enum {
    ROLLBACK_OFF = 0,
    ROLLBACK_TRACE,     /* per-frame state hashes only */
    ROLLBACK_SCAN,      /* report every symbol the match writes */
    ROLLBACK_CHECK,     /* roll back the sim_state registry, re-simulate, compare */
    ROLLBACK_ORACLE,    /* the same, restoring ALL of the executable's writable data */
    ROLLBACK_DISCOVER,  /* per symbol: restore everything but it, and see if the game changes */
} RollbackMode;

/* A --rollback* command-line argument: 1 = consumed, -1 = bad value. */
int      Rollback_ParseArg(const char *arg);
int      Rollback_Enabled(void);
/* Whether the rig changes the match it runs in -- rewinds it or loads state into it --
 * rather than only watching it (trace, dump, save-state). */
int      Rollback_Intervenes(void);
/* Whether it does anything at frame boundaries it only watches: a trace, a dump, a save. */
int      Rollback_WatchesFrames(void);

/* ReplayTest_Run, just before kick-off. */
void     Rollback_MatchStart(void);

/* Top of ReplayTest_FrameStart, before frame `frame` runs. May rewind the replay
 * (ReplayTest_SetDriverState). */
void     Rollback_FrameBoundary(uint32_t frame);

/* End of the run: write the reports; the process exit code (0 pass, 2 fail). */
int      Rollback_Finish(void);

#endif /* SWOS_CHUNKY_BUILD */
#endif /* ROLLBACK_RIG_H */
