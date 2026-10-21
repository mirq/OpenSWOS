/*
 * Amiga SWOS RE Project
 * SWOS Amiga C/68000 Reversed by Ben Baker (https://github.com/benbaker76)
 * SWOS DOS Reversed by Zlatko Karakaš (https://github.com/zlatkok)
 */
#include "random.h"
#include "deterministic.h"
#include "game_types.h"

#ifdef SWOS_CHUNKY_BUILD
/* Recorded-game replay pins BOTH generators to the recording's
 * fixedRandomValue (docs/TESTS.md 5c) — without it nothing is reproducible.
 * -1 (the default) leaves the real PRNGs alone. Defined in replay_test.c. */
extern int _replayFixedRandom;
#define REPLAY_FIXED_RAND() do { \
    if (_replayFixedRandom >= 0) return (uint16_t)_replayFixedRandom; \
} while (0)
#else
#define REPLAY_FIXED_RAND() do { } while (0)
#endif

/* ---- External globals (all live in SWOS2.s) ---- */
extern uint8_t _kRandomTable[];        /* 257-byte lookup table */
extern uint8_t _seed;                  /* primary PRNG seed/counter (in code section) */
extern uint8_t _randXorIndex;          /* primary XOR key index (adjacent to seed) */
extern uint8_t _randXorKey;            /* primary XOR key */
extern uint8_t _seed2;                 /* secondary PRNG seed/counter (BSS) */
extern uint8_t _randXorIndex2;         /* secondary XOR key index (BSS) */
extern uint8_t _randXorKey2;           /* secondary XOR key (BSS) */
extern uint8_t *_savedRandStatePtr;    /* pointer into savedRandState buffer */
extern uint16_t _panicReasonCode;
extern void PanicHaltLoop(void) __attribute__((noreturn));

/* ================================================================
 * Rand  (SWOS2.s line 18116)
 *
 * Primary PRNG used during gameplay.
 * Returns 0-255 in d0.
 * ================================================================ */
__attribute__((used, externally_visible))
uint16_t Rand(void)
{
    uint16_t d0;

    REPLAY_FIXED_RAND();
    d0 = _seed & 0xFFu;

    if (d0 == 0)
    {
        _randXorIndex++;
        _randXorKey = _kRandomTable[_randXorIndex & 0xFF];
    }

    d0 = (uint16_t)((_kRandomTable[d0] ^ _randXorKey) & 0xFFu);
    _seed++;
    return d0;
}

/* ================================================================
 * Rand2  (SWOS2.s line 18067)
 *
 * Secondary PRNG (independent state from Rand).
 * Returns 0-255 in d0.
 * ================================================================ */
__attribute__((used, externally_visible))
uint16_t Rand2(void)
{
    uint16_t d0;

    REPLAY_FIXED_RAND();
    d0 = _seed2 & 0xFFu;

    if (d0 == 0)
    {
        _randXorIndex2++;
        _randXorKey2 = _kRandomTable[_randXorIndex2 & 0xFF];
    }

    d0 = (uint16_t)((_kRandomTable[d0] ^ _randXorKey2) & 0xFFu);
    _seed2++;
    return d0;
}

/* ================================================================
 * GetRandomWord  (SWOS2.s line 18053)
 *
 * Combines two Rand2 calls to produce a full 16-bit random value.
 * Low byte from first call, high byte from second call.
 * ================================================================ */
__attribute__((used, externally_visible))
uint16_t GetRandomWord(void)
{
    uint16_t lo = Rand2();
    uint16_t hi = Rand2();
    return (uint16_t)((hi << 8) | lo);
}

/* ================================================================
 * SaveRand2State  (SWOS2.s line 18008)
 *
 * Pushes secondary PRNG state (3 bytes: seed2, randXorIndex2,
 * randXorKey2) onto the savedRandState stack-style buffer.
 * Panics if the buffer overflows into kRandomTable.
 * ================================================================ */
extern uint8_t _savedRandState[];

__attribute__((used, externally_visible))
void SaveRand2State(void)
{
    uint8_t *ptr = _savedRandStatePtr;

    *ptr++ = _seed2;
    *ptr++ = _randXorIndex2;
    *ptr++ = _randXorKey2;
    _savedRandStatePtr = ptr;

    /* Original asm compared against kRandomTable because on the Amiga
     * savedRandState and kRandomTable are adjacent in the BSS — walking
     * past the 15-byte save buffer would land in the RNG table and the
     * check catches it. On SDL they're in unrelated linker locations,
     * so compare against the explicit end of savedRandState. */
    if (ptr >= _savedRandState + 15)
    {
        _panicReasonCode = 0x9793;
        PanicHaltLoop();
    }
}

/* ================================================================
 * RestoreRand2State  (SWOS2.s line 18025)
 *
 * Pops secondary PRNG state (3 bytes) from the savedRandState
 * stack-style buffer, restoring seed2/randXorIndex2/randXorKey2.
 * ================================================================ */
__attribute__((used, externally_visible))
void RestoreRand2State(void)
{
    uint8_t *ptr = _savedRandStatePtr;

    ptr--;
    _randXorKey2 = *ptr;
    ptr--;
    _randXorIndex2 = *ptr;
    ptr--;
    _seed2 = *ptr;
    _savedRandStatePtr = ptr;
}

/* ================================================================
 * Deterministic Mode Initialization
 *
 * Seeds both PRNGs from DETERMINISTIC_SEED so every game sequence
 * is reproducible. Called from _start() before any game logic.
 * ================================================================ */
#ifdef DETERMINISTIC_MODE
extern uint16_t randomSeed;
extern uint8_t  newRandomSeedCopy;
extern uint8_t  savedRandState[];
uint16_t deterministicTick = 0;

__attribute__((used, externally_visible))
void InitDeterministicMode(void)
{
    /* Primary PRNG */
    seed = (uint8_t)(DETERMINISTIC_SEED & 0xFF);
    randXorIndex = (uint8_t)((DETERMINISTIC_SEED >> 8) & 0xFF);
    randXorKey = kRandomTable[randXorIndex];

    /* Secondary PRNG */
    seed2 = (uint8_t)((DETERMINISTIC_SEED >> 4) & 0xFF);
    randXorIndex2 = (uint8_t)((DETERMINISTIC_SEED >> 12) & 0xFF);
    randXorKey2 = kRandomTable[randXorIndex2];

    /* Seeding helpers */
    randomSeed = DETERMINISTIC_SEED;
    newRandomSeedCopy = (uint8_t)(DETERMINISTIC_SEED & 0xFF);

    /* Reset save stack */
    savedRandStatePtr = savedRandState;

    deterministicTick = 0;

#ifdef DETERMINISTIC_DUMP_FRAME
    /* Reset frame log on each init — armed from InitGameRestoreTeams */
    rngLogCount = 0;
    rngDumpFrameCounter = 0;
    rngDumpActive = 1;
#endif
}
#endif

/* ================================================================
 * RNG Dump — writes log to DH0:rng_dump.bin after N frames
 * ================================================================ */
#ifdef DETERMINISTIC_DUMP_FRAME
uint16_t rngLogCount = 0;
uint8_t  rngLogRng[RNG_LOG_MAX];
uint8_t  rngLogVal[RNG_LOG_MAX];
uint16_t rngDumpFrameCounter = 0;
uint16_t rngDumpActive = 0;

/* Per-frame state log: 72 bytes per frame (36 x int16_t) */
#define FRAME_LOG_FIELDS 36
#define FRAME_LOG_MAX DETERMINISTIC_DUMP_FRAME
static int16_t frameLog[FRAME_LOG_MAX * FRAME_LOG_FIELDS];

static uint16_t frameLogCount = 0;

extern void PanicHaltLoop(void) __attribute__((noreturn));

/* Inline DOS calls for file dump (dosBase from Resload) */
extern uint32_t S_DosBase;
extern uint32_t _S_VBR;
extern uint32_t S_SavedInt2;
extern void Int2KeyboardHandler(void);
extern Sprite ballSprite;
extern Sprite goalie1Sprite;
extern Sprite team1Player2Sprite;
extern Sprite goalie2Sprite;
extern Sprite team2Player2Sprite;
extern uint16_t currentGameTick;
extern TeamGame topTeamInGame;
extern TeamGame bottomTeamInGame;
extern Sprite **team1SpritesTable;
extern Sprite **team2SpritesTable;
extern TeamGeneralInfo topTeamData;
extern TeamGeneralInfo bottomTeamData;
#define LEVEL_2_VEC 0x68
#define MODE_NEWFILE 1006

static inline uint32_t dumpDosOpen(const uint8_t *name, uint32_t mode) {
    register void *_a6 __asm("a6") = (void *)S_DosBase;
    register const uint8_t *_d1 __asm("d1") = name;
    register uint32_t _d2 __asm("d2") = mode;
    register uint32_t _d0 __asm("d0");
    __asm volatile ("jsr -30(%%a6)" : "=r"(_d0), "+r"(_d1), "+r"(_d2) : "r"(_a6) : "a0","a1","memory");
    return _d0;
}
static inline int32_t dumpDosWrite(uint32_t handle, const void *buf, uint32_t len) {
    register void *_a6 __asm("a6") = (void *)S_DosBase;
    register uint32_t _d1 __asm("d1") = handle;
    register const void *_d2 __asm("d2") = buf;
    register uint32_t _d3 __asm("d3") = len;
    register int32_t _d0 __asm("d0");
    __asm volatile ("jsr -48(%%a6)" : "=r"(_d0), "+r"(_d1), "+r"(_d2), "+r"(_d3) : "r"(_a6) : "a0","a1","memory");
    return _d0;
}
static inline void dumpDosClose(uint32_t handle) {
    register void *_a6 __asm("a6") = (void *)S_DosBase;
    register uint32_t _d1 __asm("d1") = handle;
    register uint32_t _d0 __asm("d0");
    __asm volatile ("jsr -36(%%a6)" : "=r"(_d0), "+r"(_d1) : "r"(_a6) : "a0","a1","memory");
}

/* Dump raw sprite memory for all 22 players at a specific frame */
static void dumpSpriteSnapshot(uint16_t frameNum)
{
    if (frameNum != 490) return;

    volatile uint32_t *vec = (volatile uint32_t *)(_S_VBR + 0x68);
    *vec = S_SavedInt2;

    uint32_t fh = dumpDosOpen((const uint8_t *)"PROGDIR:sprite_snap.bin", 1006);
    if (fh != 0) {
        int16_t p;
        for (p = 0; p < 11; p++)
            dumpDosWrite(fh, (const void *)team1SpritesTable[p], 110);
        for (p = 0; p < 11; p++)
            dumpDosWrite(fh, (const void *)team2SpritesTable[p], 110);
        dumpDosClose(fh);
    }

    *vec = (uint32_t)Int2KeyboardHandler;
}

__attribute__((used, externally_visible))
void RngDumpCheck(void)
{
    if (!rngDumpActive)
        return;

    dumpSpriteSnapshot(frameLogCount);

    /* Log per-frame state */
    if (frameLogCount < FRAME_LOG_MAX) {
        uint16_t idx = frameLogCount * FRAME_LOG_FIELDS;
        /* Ball state */
        frameLog[idx + 0] = (int16_t)currentGameTick;
        frameLog[idx + 1] = ballSprite.x.s.i;
        frameLog[idx + 2] = ballSprite.y.s.i;
        frameLog[idx + 3] = ballSprite.speed;
        frameLog[idx + 4] = ballSprite.destX;
        frameLog[idx + 5] = ballSprite.destY;
        frameLog[idx + 6] = (int16_t)(ballSprite.deltaX.raw >> 16);
        frameLog[idx + 7] = (int16_t)(ballSprite.deltaY.raw >> 16);
        /* All 22 player Y positions (slots 8-29) — using named globals
         * to avoid potential table pointer issues */
        frameLog[idx +  8] = goalie1Sprite.y.s.i;
        frameLog[idx +  9] = team1Player2Sprite.y.s.i;
        {
            /* team1 players 3-11 via table (indices 2-10) */
            int16_t p;
            for (p = 2; p < 11; p++)
                frameLog[idx + 8 + p] = team1SpritesTable[p]->y.s.i;
        }
        frameLog[idx + 19] = goalie2Sprite.y.s.i;
        frameLog[idx + 20] = team2Player2Sprite.y.s.i;
        {
            /* team2 players 3-11 via table (indices 2-10) */
            int16_t p;
            for (p = 2; p < 11; p++)
                frameLog[idx + 19 + p] = team2SpritesTable[p]->y.s.i;
        }
        /* AI state + ball Z + t2p6 dest (slots 30-35) */
        {
            frameLog[idx + 30] = topTeamData.AI_timer;
            frameLog[idx + 31] = bottomTeamData.AI_timer;
            frameLog[idx + 32] = ballSprite.z.s.i;
            frameLog[idx + 33] = (int16_t)((topTeamData.fireThisFrame << 8) |
                                           (uint8_t)bottomTeamData.fireThisFrame);
            frameLog[idx + 34] = team2SpritesTable[6]->destX;
            frameLog[idx + 35] = team2SpritesTable[6]->destY;
        }
        frameLogCount++;
    }

    rngDumpFrameCounter++;
    if (rngDumpFrameCounter < DETERMINISTIC_DUMP_FRAME)
        return;

    /* Swap int2 to OS handler before DOS calls */
    volatile uint32_t *vec = (volatile uint32_t *)(_S_VBR + LEVEL_2_VEC);
    *vec = S_SavedInt2;

    /* Write frame state dump */
    uint32_t fh = dumpDosOpen((const uint8_t *)"PROGDIR:state_dump.bin", MODE_NEWFILE);
    if (fh != 0) {
        dumpDosWrite(fh, frameLog, frameLogCount * FRAME_LOG_FIELDS * 2);
        dumpDosClose(fh);
    }

    /* Restore game int2 handler */
    *vec = (uint32_t)Int2KeyboardHandler;

    /* Halt so user can collect the file */
    panicReasonCode = 0xE000 | (frameLogCount & 0xFFF);
    PanicHaltLoop();
}
#endif
