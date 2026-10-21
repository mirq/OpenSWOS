/*
 * replay_test.h — recorded-game replay regression rig (SDL build only).
 *
 * Replays a `.swr` (produced by scripts/rgd_convert.py --emit from a
 * swos-port `.RGD` recording) frame by frame: inject the recorded input,
 * run one game-loop iteration, compare every recorded NAMED FIELD against
 * the matching global, and stop at the first divergence.
 *
 * See docs/TESTS.md. The rig deliberately never parses a DOS struct — the
 * converter turns the recording into named fields so the two trees' layouts
 * can keep diverging.
 */
#ifndef REPLAY_TEST_H
#define REPLAY_TEST_H

#ifdef SWOS_CHUNKY_BUILD

#include "swos_types.h"
#include "game_types.h"
#include "match_step.h"

/* Run swos-port's CalculateDeltaXAndY reference vectors against ours and
 * report. Standalone (`--replay-selftest`) — no recording needed. Returns 0
 * when all vectors match, 2 otherwise. */
int  ReplayTest_MathSelfTest(void);

/* Parse `path` and arm the rig. 0 = ok, non-zero = the run should abort.
 * `level` is one of "scalars", "teams", "info", "sprites" (or NULL to take
 * the level the file was emitted at); fields above the requested level are
 * loaded but not compared. `skipPrefixes` is an optional comma-separated
 * list of field-name prefixes to mute for this run. */
int  ReplayTest_Load(const char *path, const char *level, int maxErrors,
                     const char *skipPrefixes);

/* Non-zero once ReplayTest_Load has succeeded — the front-end is skipped. */
int  ReplayTest_Armed(void);

/* Non-zero while the match is being replayed (frame hooks live). */
int  ReplayTest_Running(void);

/* Drive the whole replay: install the recorded setup, start the match, and
 * exit the process with 0 (all frames matched) or 2 (divergence). */
void ReplayTest_Run(void);

/* Menu-free player assignment, from the recording's header. Called by
 * SetupPlayers in place of the two ShowPlayMatchMenu prompts. */
void ReplayTest_SetupPlayers(TeamFileEx *team1, TeamFileEx *team2);

/* Game-loop hooks (game_loop.c): take this frame's record and inject its
 * input, then compare the resulting state and advance the clock. */
void ReplayTest_FrameStart(void);
void ReplayTest_FrameEnd(void);

/* ---- for a netplay match (src/netplay_match.c) ----
 * It applies the input inside its own frame step, which GGPO repeats, so
 * ReplayTest_FrameStart leaves the input to it and ReplayTest_FrameEnd comes
 * in parts: the comparison, made on the frame as first played, then the move
 * to the next record. The clock is the step's (MatchStep_AdvanceClock). */
void     ReplayTest_FrameCheck(void);
void     ReplayTest_FrameDone(void);
uint32_t ReplayTest_FrameIndex(void);   /* the record being replayed */

/* Frame `frame`'s recorded input, as the players' inputs; none past the recording's end. */
void     ReplayTest_RecordedInput(uint32_t frame, MatchPlayerInput in[MATCH_PLAYERS]);

/* ---- for a netplay match between two peers ----
 * A peer plays frames ahead of its confirmed input and GGPO rolls them back, so it drives
 * the recording by frame number rather than through the per-frame hooks. */
void     ReplayTest_PrepareFirstFrame(void);   /* the recording's kick-off state, once */
uint32_t ReplayTest_FrameCount(void);
void     ReplayTest_Finish(int32_t code);       /* report, and exit the process */

/* The recordings pin the RNG to one value — random.c returns it for both
 * Rand and Rand2 while this is >= 0. */
extern int _replayFixedRandom;

/* While the rig owns the frame clock, the Int3 handler must not advance
 * _currentGameTick: on Amiga it ticks once per VBlank in lockstep with the
 * loop, but on SDL two threads drive it from a wall clock. The rig
 * advances it exactly once per game-loop iteration instead. */
int  ReplayTest_OwnsGameTick(void);

/* ---- for the rollback rig (src/rollback_rig.c, docs/ROLLBACK.md) ----
 * The replay's own position -- what a rewind has to put back alongside the
 * game state. Its input latches are game state (g_matchInputLatch). */
typedef struct {
    uint32_t frame;
} ReplayDriverState;

void     ReplayTest_GetDriverState(ReplayDriverState *st);
void     ReplayTest_SetDriverState(const ReplayDriverState *st);

/* FNV-1a over every field the replay compares (~2500: sprites, both teams'
 * TeamGame/TeamGeneralInfo, stats, clock, camera): the gameplay-observable state,
 * defined independently of the sim_state registry. */
uint64_t ReplayTest_ObservableHash(void);

/* Calls fn once per observable field, with its storage. */
void     ReplayTest_ForEachObservable(void (*fn)(void *addr, uint32_t size, void *ctx), void *ctx);

#endif /* SWOS_CHUNKY_BUILD */
#endif /* REPLAY_TEST_H */
