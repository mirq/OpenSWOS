/*
 * Amiga SWOS RE Project
 * SWOS Amiga C/68000 Reversed by Ben Baker (https://github.com/benbaker76)
 * SWOS DOS Reversed by Zlatko Karakaš (https://github.com/zlatkok)
 */
#include "sfx_crowd.h"
#include "dos_sound.h"
#include "audio.h"
#include "sfx_ids.h"

#define ST_PENALTY 14

/* ---- External globals (BSS, absolute addressing in asm) ---- */
extern uint16_t _endOfHalfWriteOnly;
extern uint16_t _goalCounter;
extern uint16_t _sfxQueueWriteIdx;
extern uint16_t _crowdEventFlags;
extern uint16_t _sfxQueueCount;
extern uint16_t _trainingGame;
extern uint16_t _gameState;
extern uint16_t _penaltiesState[1];
extern uint16_t _patternsGoalCounter;
extern uint16_t _showFansCounter;
extern volatile uint16_t _currentGameTick;
extern void WaitForVSync(void);   /* game_utils.c — pumps audio + events on SDL */

/* ---- Embedded data labels in SWOS2.s code section ---- */
extern uint16_t _pendingSampleId3_4;
extern uint16_t _crowdChantsOn;
extern int16_t _lastKickSampleTick;
extern uint8_t _audioDbgFlagA;
extern uint16_t _crowdSamples[4];
extern uint16_t _currentCrowdSample;
extern uint16_t _fadeInChantTimer;
extern uint16_t _nextChantTimer;
extern uint16_t _fadeOutChantTimer;
extern uint16_t _crowdChantVolume;

/* ---- External C/asm functions ---- */
extern uint16_t Rand(void);
/* AudioSetChannelVolume, AudioApplyPresetToChannels, AudioChannelQueueNextWaveIfEnabled from audio.h */

/* ================================================================
 * CrowdResetMatchState  (SWOS2.s line 9083)
 *
 * Clears all crowd/SFX state at the start of a match.
 * ================================================================ */
__attribute__((used, externally_visible))
void CrowdResetMatchState(void)
{
    _patternsGoalCounter = 0;
    _showFansCounter = 0;
    _crowdChantVolume = 0;
    _crowdEventFlags = 0;
    _sfxQueueCount = 0;
    _sfxQueueWriteIdx = 0;
    _goalCounter = 0;
    _crowdEventFlags = 0;   /* original clears this twice */
    _currentCrowdSample = 0xFFFF;
}

/* ================================================================
 * TurnCrowdChantsOn  (SWOS2.s line 9097)
 *
 * Enables crowd chants: resets timers, sets volume to 0, enables flag.
 * ================================================================ */
__attribute__((used, externally_visible))
void TurnCrowdChantsOn(void)
{
    _fadeInChantTimer = 0;
    _nextChantTimer = 0;
    _fadeOutChantTimer = 0;
    _crowdChantsOn = 1;
    AudioSetChannelVolume(2, 0);
    _crowdChantVolume = 0;
}

/* ================================================================
 * TurnCrowdChantsOff  (SWOS2.s line 9110)
 *
 * Disables crowd chants: clears flag and volume.
 * ================================================================ */
__attribute__((used, externally_visible))
void TurnCrowdChantsOff(void)
{
    _crowdChantsOn = 0;
    AudioSetChannelVolume(2, 0);
    _crowdChantVolume = 0;
}

/* ================================================================
 * PlayCrowdChants  (SWOS2.s line 9122)
 *
 * State machine for crowd chant audio:
 *   - If training game or chants off: return
 *   - If fadeInChantTimer active: fade in
 *   - If nextChantTimer active: cooldown tick
 *   - If fadeOutChantTimer active: fade out
 *   - Otherwise: random chance to start new chant event
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayCrowdChants(void)
{
    uint16_t rnd, vol;

    if (_trainingGame != 0)
        return;
    if (_crowdChantsOn == 0)
        return;

    if (_fadeInChantTimer != 0)
        goto crowd_fadein_tick;
    if (_nextChantTimer != 0)
        goto crowd_cooldown_tick;
    if (_fadeOutChantTimer != 0)
        goto crowd_fadeout_tick;

    /* Idle state: random chance to start a chant */
    rnd = Rand();
    if (rnd < 128)
    {
        /* Start new chant event */
        _crowdChantVolume = 0;
        AudioSetChannelVolume(2, 0);

        /* Pick a crowd sample */
        if (_currentCrowdSample != 0xFFFF)
        {
            rnd = Rand();
            if (rnd >= 0x80)
                goto pick_new_sample;
            /* else keep current sample */
        }
        else
        {
pick_new_sample:
            rnd = Rand();
            rnd &= 6;  /* 0, 2, 4, or 6 */
            _currentCrowdSample = _crowdSamples[rnd >> 1];
        }

        AudioChannelQueueNextWaveIfEnabled(2, _currentCrowdSample);

        _fadeInChantTimer = 128;
        rnd = Rand();
        _nextChantTimer = (uint16_t)((rnd << 1) + 500);
        _fadeOutChantTimer = 128;
        return;
    }
    else
    {
        /* No chant this time, set cooldown */
        rnd = Rand();
        _nextChantTimer = (uint16_t)((rnd << 2) + 0x1F4);
        return;
    }

crowd_cooldown_tick:
    _nextChantTimer--;
    return;

crowd_fadeout_tick:
    _fadeOutChantTimer--;
    vol = _fadeOutChantTimer >> 1;
    _crowdChantVolume = vol;
    AudioSetChannelVolume(2, vol);
    return;

crowd_fadein_tick:
    _fadeInChantTimer--;
    vol = (uint16_t)(0x80 - _fadeInChantTimer) >> 1;
    _crowdChantVolume = vol;
    AudioSetChannelVolume(2, vol);
    return;
}

/* ================================================================
 * MuteAllAudioChannelsWithDelay  (SWOS2.s line 9207)
 *
 * Ramps all-channel volume from 0 to 0x40 in steps of 4,
 * waiting 1 tick per step.
 * ================================================================ */
__attribute__((used, externally_visible))
void MuteAllAudioChannelsWithDelay(void)
{
    uint16_t vol;

    for (vol = 0; vol <= 0x40; vol += 4)
    {
        AudioSetChannelVolume(0xFFFF, vol);
        WaitForVSync();
    }
}

/* ================================================================
 * AudioFadeCrowdDownThenPulseMusic  (SWOS2.s line 9228)
 *
 * Phase 1: Fade crowd channel (ch2) down from crowdChantVolume to 0.
 * Phase 2: Fade music channels (ch0,1,3) down from 0x40 to 0.
 * Final: Set all channels to 0.
 * ================================================================ */
__attribute__((used, externally_visible))
void AudioFadeCrowdDownThenPulseMusic(void)
{
    int16_t vol;

#ifdef SWOS_CHUNKY_BUILD
    /* Arcade attract music (HERO): the reels are match replays. A replay goal
     * would otherwise pulse/dip the shared music player via the all-channels
     * (master) volume — cutting HERO. Skip it while SFX are suppressed. */
    { extern int _gArcadeSfxSuppressed; if (_gArcadeSfxSuppressed) return; }
#endif
    /* Phase 1: Fade crowd channel down */
    vol = (int16_t)_crowdChantVolume;
    if (vol != 0)
    {
        while (vol >= 0)
        {
            AudioSetChannelVolume(2, (uint16_t)vol);
            WaitForVSync();
            vol -= 2;
        }
        AudioSetChannelVolume(2, 0);
    }

    /* Phase 2: Pulse music channels (0, 1, 3) down */
    for (vol = 0x40; vol >= 0; vol -= 2)
    {
        AudioSetChannelVolume(0, (uint16_t)vol);
        AudioSetChannelVolume(1, (uint16_t)vol);
        AudioSetChannelVolume(3, (uint16_t)vol);
        WaitForVSync();
    }

    /* Final: all channels to 0 */
    AudioSetChannelVolume(0xFFFF, 0);
}

/* ================================================================
 * AudioMuteAllWithFrameRamp  (SWOS2.s line 9282)
 *
 * Ramps all-channel volume from 0x40 down to 0 in steps of 2,
 * waiting 1 tick per step. Final: set volume to 0.
 * ================================================================ */
__attribute__((used, externally_visible))
void AudioMuteAllWithFrameRamp(void)
{
    int16_t vol;

#ifdef SWOS_CHUNKY_BUILD
    /* Don't mute the shared player during arcade attract music (HERO) — a replay
     * half-time/end event must not silence the title song. */
    { extern int _gArcadeSfxSuppressed; if (_gArcadeSfxSuppressed) return; }
#endif
    for (vol = 0x40; vol >= 0; vol -= 2)
    {
        AudioSetChannelVolume(0xFFFF, (uint16_t)vol);
        WaitForVSync();
    }

    AudioSetChannelVolume(0xFFFF, 0);
}

/* ================================================================
 * PlayEnqueuedSamples  (SWOS2.s line 9353)
 *
 * Timer-driven SFX dispatcher. Decrements counters and triggers
 * samples when they reach zero.
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayEnqueuedSamples(void)
{
    DOS_SOUND(DOS_EV_TICK, (int16_t)_penaltiesState[0] == -1);
    if (_endOfHalfWriteOnly != 0)
    {
        _endOfHalfWriteOnly--;
        if (_endOfHalfWriteOnly == 0)
        {
            PlayPenaltySample();
            _sfxQueueWriteIdx = 0xFA;
        }
    }

    if (_goalCounter != 0)
    {
        _goalCounter--;
        if (_goalCounter == 0)
            PlaySample4IfNotTraining();
    }

    if (_sfxQueueWriteIdx != 0)
    {
        _sfxQueueWriteIdx--;
        if (_sfxQueueWriteIdx == 0)
            PlaySample7IfNotTraining();
    }

    if (_crowdEventFlags != 0)
        _crowdEventFlags--;

    if (_sfxQueueCount != 0)
    {
        _sfxQueueCount--;
        if (_sfxQueueCount == 0)
            PlayCornerKickSample();
    }

    PlayCrowdChants();
}

/* ================================================================
 * PlayPenaltySample  (SWOS2.s line 9386)
 *
 * If not training: play sample preset 6.
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayPenaltySample(void)
{
    if (_trainingGame != 0)
        return;
    PlaySample(SFX_PRESET_PENALTY);
}

/* ================================================================
 * PlaySample7IfNotTraining  (SWOS2.s line 9396)
 *
 * If not training: play sample preset 7.
 * ================================================================ */
__attribute__((used, externally_visible))
void PlaySample7IfNotTraining(void)
{
    if (_trainingGame != 0)
        return;
    PlaySample(SFX_PRESET_NOT_TRAINING_7);
}

/* ================================================================
 * PlaySfxSample16  (SWOS2.s line 9406, sub_107260)
 *
 * Plays SFX_BALL_BOUNCE_1 (default ball bounce) on channel 3,
 * gated through GateKickSampleAndCrowd.
 * ================================================================ */
__attribute__((used, externally_visible))
void PlaySfxSample16(void)
{
    DOS_SOUND(DOS_EV_BOUNCE, 0);            /* the PC game's PlayBallBounceSample */
    GateKickSampleAndCrowd(3, SFX_BALL_BOUNCE_1);
}

/* ================================================================
 * PlayGoodPassSample  (SWOS2.s line 9414)
 *
 * Plays SFX_BALL_PASSED_OR_TACKLED (pass or sliding-tackle ball
 * thump) on channel 3, gated through GateKickSampleAndCrowd. The
 * "good pass" name is the original asm symbol; in practice the
 * sample also plays for tackles, hence the renamed wave id.
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayGoodPassSample(void)
{
    GateKickSampleAndCrowd(3, SFX_BALL_PASSED_OR_TACKLED);
}

/* ================================================================
 * PlayKickSample  (SWOS2.s line 9422)
 *
 * Alternates between SFX_BALL_KEEPER_SAFE_2 (0x25, keeper saves)
 * and SFX_BALL_SHOT_HEADER_LOW (0x28, low shot / flying header)
 * based on currentGameTick bit 1, then routes through
 * GateKickSampleAndCrowd on channel 3. Despite the asm-derived
 * "kick" name, the per-tick alternation makes this fire on a mix
 * of shot/save events depending on game state.
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayKickSample(void)
{
    DOS_SOUND(DOS_EV_KICK, 0);
    uint16_t sfxId = SFX_BALL_KEEPER_SAFE_2;

    /* btst #1,(currentGameTick+1) tests bit 1 of the low byte */
    if (((uint8_t)_currentGameTick) & 0x02)
        sfxId = SFX_BALL_SHOT_HEADER_LOW;

    GateKickSampleAndCrowd(3, sfxId);
}

/* ================================================================
 * GateKickSampleAndCrowd  (SWOS2.s line 9432)
 *
 * Gates a ball/kick/shot sample through the audioDbgFlagA bit 7
 * check and the lastKickSampleTick comparison: when the gate is
 * armed, only allow the next sample if the previous one was a
 * crowd reaction (CROWD_KEEPERSAVES "aaah" or CROWD_NEARMISS
 * "oooh"). This pacing rule prevents back-to-back ball thumps
 * from drowning out the crowd response. If the gate passes,
 * HandleKickSampleAndCrowd does the actual trigger.
 *
 * Asm name kept for back-port traceability — these IDs aren't
 * strictly "kicks" any more (we now know the wave-ids include
 * passes, tackles, headers, saves) but the function symbol still
 * matches SWOS2.s line 12017.
 * ================================================================ */
__attribute__((used, externally_visible))
void GateKickSampleAndCrowd(uint16_t channel, uint16_t sfxId)
{
    /* If audioDbgFlagA bit 7 is set, apply gating: only allow kicks when
     * the last triggered SFX was CROWD_KEEPERSAVES (0x0F "aaah") or
     * CROWD_NEARMISS (0x12 "oooh") — both are crowd reactions that
     * should "unlock" the next kick sample. */
    if (_audioDbgFlagA & 0x80)
    {
        if (_lastKickSampleTick != SFX_CROWD_KEEPERSAVES && _lastKickSampleTick != SFX_CROWD_NEARMISS)
            return;
    }

    HandleKickSampleAndCrowd(channel, sfxId);
}

/* ================================================================
 * PlayRefereeWhistleSample  (SWOS2.s line 9444)
 *
 * Plays SFX_WHISTLE_KICKOFF (0x31, kickoff whistle — minute 0/45
 * or after a goal) on channel 3 via HandleKickSampleAndCrowd.
 * "Referee whistle" is the original asm symbol; the actual sample
 * is specifically the kickoff whistle.
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayRefereeWhistleSample(void)
{
    DOS_SOUND(DOS_EV_WHISTLE, 0);
    HandleKickSampleAndCrowd(3, SFX_WHISTLE_KICKOFF);
}

/* ================================================================
 * PlayFoulWhistleSample  (SWOS2.s line 9452)
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayFoulWhistleSample(void)
{
    DOS_SOUND(DOS_EV_FOUL_WHISTLE, 0);
    HandleKickSampleAndCrowd(3, SFX_WHISTLE_FOUL_1);
}

/* ================================================================
 * PlayEndGameWhistleSample  (SWOS2.s line 9457)
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayEndGameWhistleSample(void)
{
    DOS_SOUND(DOS_EV_END_WHISTLE, 0);
    HandleKickSampleAndCrowd(3, SFX_WHISTLE_ENDGAME);
}

/* ================================================================
 * PlaySample1IfNotTraining  (SWOS2.s line 9464)
 * ================================================================ */
__attribute__((used, externally_visible))
void PlaySample1IfNotTraining(void)
{
    if (_trainingGame != 0)
        return;
    PlaySample(SFX_PRESET_NOT_TRAINING_1);
}

/* ================================================================
 * PlaySample1  (SWOS2.s line 9471)
 * ================================================================ */
__attribute__((used, externally_visible))
void PlaySample1(void)
{
    PlaySample(SFX_PRESET_NOT_TRAINING_1);
}

/* ================================================================
 * PlaySample2IfNotTraining  (SWOS2.s line 9477)
 *
 * If not training: set pendingSampleId3_4=$3E8, play preset 3.
 * (Original: falls through to PlaySample3 label)
 * ================================================================ */
__attribute__((used, externally_visible))
void PlaySample2IfNotTraining(void)
{
    if (_trainingGame != 0)
        return;
    _pendingSampleId3_4 = 0x3E8;
    PlaySample(SFX_PRESET_NOT_TRAINING_2);
}

/* ================================================================
 * PlaySample4IfNotTraining  (SWOS2.s line 9488)
 *
 * If not training: set pendingSampleId3_4=$190, play preset 4.
 * (Original: falls through to PlaySample4 label)
 * ================================================================ */
__attribute__((used, externally_visible))
void PlaySample4IfNotTraining(void)
{
    if (_trainingGame != 0)
        return;
    _pendingSampleId3_4 = 0x190;
    PlaySample(SFX_PRESET_NOT_TRAINING_4);
}

/* ================================================================
 * PlaySample3IfNotTraining  (SWOS2.s line 9502)
 *
 * If not training: set sfxQueueCount=$A, then GateKick on channel 3
 * with SFX_BALL_BOUNCE_3 (ball bounce variant 3).
 * ================================================================ */
__attribute__((used, externally_visible))
void PlaySample3IfNotTraining(void)
{
    if (_trainingGame != 0)
        return;
    _sfxQueueCount = 0x0A;
    GateKickSampleAndCrowd(3, SFX_BALL_BOUNCE_3);
}

/* ================================================================
 * PlayMissGoalSample  (SWOS2.s line 9514)
 *
 * If not training: set sfxQueueCount=$A, then HandleKick on channel
 * 3 with SFX_BALL_HEADER_STANDING (standing header). The "miss
 * goal" name is the original asm symbol; the actual wave is the
 * standing-header ball thump.
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayMissGoalSample(void)
{
    /* The PC game, at both callers (UpdatePlayers): PlayGoalkeeperSavedComment, PlayMissGoalSample. */
    DOS_SOUND(DOS_EV_KEEPER_SAVED, (int16_t)_penaltiesState[0] == -1);
    DOS_SOUND(DOS_EV_MISS_CROWD, 0);
    if (_trainingGame != 0)
        return;
    _sfxQueueCount = 0x0A;
    HandleKickSampleAndCrowd(3, SFX_BALL_HEADER_STANDING);
}

/* ================================================================
 * PlayPenalty  (SWOS2.s line 9526)
 *
 * If not training: play penalty sample, set sfxQueueWriteIdx=$8C.
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayPenalty(void)
{
    DOS_SOUND(DOS_EV_PENALTY, 0);           /* PlayPenaltyComment */
    if (_trainingGame != 0)
        return;
    PlayPenaltySample();
    _sfxQueueWriteIdx = 0x8C;
}

/* ================================================================
 * PlayGameStateWhistleSample  (SWOS2.s line 9537)
 *
 * If not training:
 *   - If gameState != ST_PENALTY: play penalty sample
 *   - If gameState == ST_PENALTY && sfxQueueWriteIdx == 0: play penalty sample
 *   - Set sfxQueueWriteIdx = $96
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayGameStateWhistleSample(void)
{
    if (_trainingGame != 0)
        return;

    if (_gameState == ST_PENALTY)
    {
        if (_sfxQueueWriteIdx != 0)
            goto queue_whistle;
    }

    PlayPenaltySample();

queue_whistle:
    _sfxQueueWriteIdx = 0x96;
}

/* ================================================================
 * PlayGoalReplayBottomSample  (SWOS2.s line 9554)
 *
 * If not training: calls PlaySample2IfNotTraining.
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayGoalReplayBottomSample(void)
{
    if (_trainingGame != 0)
        return;
    PlaySample2IfNotTraining();
}

/* ================================================================
 * PlayGoalKickOffSample  (SWOS2.s line 9563)
 *
 * If not training: HandleKick on channel 3 with SFX_CROWD_NEARMISS
 * ("oooh, just wide"). If the gate plays it (return 0), arm the
 * crowd-event window with crowdEventFlags = $39. The asm name
 * "GoalKickOff" is misleading — the wave id is the near-miss
 * crowd reaction, not a kickoff.
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayGoalKickOffSample(void)
{
    /* The PC game (CheckIfBallOutOfPlay): PlayNearMissComment, PlayMissGoalSample. */
    DOS_SOUND(DOS_EV_NEAR_MISS, (int16_t)_penaltiesState[0] == -1);
    DOS_SOUND(DOS_EV_MISS_CROWD, 0);
    if (_trainingGame != 0)
        return;
    if (HandleKickSampleAndCrowd(3, SFX_CROWD_NEARMISS) != 0)
        return;
    _crowdEventFlags = 0x39;
}

/* ================================================================
 * PlayCornerKickSample  (SWOS2.s line 9579)
 *
 * If not training: HandleKick on channel 3 with
 * SFX_CROWD_KEEPERSAVES ("aaah" keeper saves a shot). If the gate
 * plays it (return 0), arm the crowd-event window with
 * crowdEventFlags = $32. The asm name "CornerKick" is misleading —
 * the wave id is the keeper-saves crowd reaction.
 * ================================================================ */
__attribute__((used, externally_visible))
void PlayCornerKickSample(void)
{
    if (_trainingGame != 0)
        return;
    if (HandleKickSampleAndCrowd(3, SFX_CROWD_KEEPERSAVES) != 0)
        return;
    _crowdEventFlags = 0x32;
}

/* ================================================================
 * HandleKickSampleAndCrowd  (SWOS2.s line 9606)
 *
 * If sfxId == $16, always play.
 * Otherwise, if crowdEventFlags != 0, block (return non-zero).
 * Stores sfxId to lastKickSampleTick, calls CrowdTickJump,
 * returns 0 (played).
 * ================================================================ */
__attribute__((used, externally_visible))
int16_t HandleKickSampleAndCrowd(uint16_t mode, uint16_t sfxId)
{
    /* SFX_WHISTLE_ENDGAME bypasses the crowd-event gate so the final whistle
     * always plays regardless of any in-flight crowd reaction. */
    if (sfxId != SFX_WHISTLE_ENDGAME)
    {
        if (_crowdEventFlags != 0)
            return 1;  /* blocked */
    }

    _lastKickSampleTick = (int16_t)sfxId;
    AudioChannelQueueNextWaveIfEnabled(mode, sfxId);
    return 0;  /* played */
}

/* ================================================================
 * PlaySample  (SWOS2.s line 9622)
 *
 * Wrapper for AudioApplyPresetToChannels.
 * ================================================================ */
__attribute__((used, externally_visible))
void PlaySample(uint16_t preset)
{
    AudioApplyPresetToChannels(preset);
}
