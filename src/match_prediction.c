/*
 * match_prediction.c -- each player's half of PRED and SEEN, swapped over the lobby's link
 * after an online match. See include/match_prediction.h.
 *
 * Nothing here runs during play: a half is offered when the match's .SRP has been
 * written, and crosses while the players are back in the lobby. A half that never
 * arrives (the other player left) costs only that half: the file keeps the one it has.
 */
#include "match_prediction.h"

#if defined(SWOS_CHUNKY_BUILD) && defined(SWOS_NETPLAY)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "endian_helpers.h"
#include "miniz_swos.h"
#include "p2p_link.h"
#include "replay_v2.h"      /* ReplayRec_AddPeerPrediction */

enum {
    CH_PRED        = 4,             /* the lobby has 0..2, the match report 3 */
    PRED_HEAD      = 20,
    PRED_VERSION   = 2,             /* 2: the READ bytes too (the two builds are the same) */
    PRED_MSG_MAX   = 120 << 10,     /* under the link's 128 KB a message */
};

static struct {
    uint8_t      *msg;              /* ours, ready to send */
    uint32_t      msgLen;
    int32_t       sent;
    int32_t       player;
    uint8_t      *peerBuf;          /* theirs: PRED then SEEN */
    SrpPrediction peer;
    int32_t       havePeer;
    int32_t       added;            /* theirs is in the files */
    uint16_t      staleGen;         /* the last match's message, still on the channel */
} s_mp;

void MatchPrediction_Begin(void)
{
    uint32_t len;
    uint16_t gen = 0;
    free(s_mp.msg);
    free(s_mp.peerBuf);
    memset(&s_mp, 0, sizeof s_mp);
    /* The link keeps the newest message on a channel until a newer one completes: the
     * last match's half would pass for this one's. */
    if (P2PLink_State() == P2P_CONNECTED && P2PLink_Received(CH_PRED, &len, &gen) != NULL)
        s_mp.staleGen = gen;
}

/* The message for `own`, with its SEEN or without. NULL when it does not fit. */
static uint8_t *encode(int32_t player, const SrpPrediction *own, int32_t withSeen, uint32_t *outLen)
{
    uint32_t seenLen = withSeen ? own->seenLen : 0;
    uint32_t readLen = own->read ? own->readFrames * SRP_READ_BYTES : 0;
    uint32_t rawLen = own->predFrames + readLen + seenLen;
    size_t cap = (size_t)mz_compressBound(rawLen) + PRED_HEAD, n;
    uint8_t *raw = (uint8_t *)malloc(rawLen + 1), *msg = (uint8_t *)malloc(cap);

    if (!raw || !msg) {
        free(raw);
        free(msg);
        return NULL;
    }
    memcpy(raw, own->pred, own->predFrames);
    if (readLen)
        memcpy(raw + own->predFrames, own->read, readLen);
    if (seenLen)
        memcpy(raw + own->predFrames + readLen, own->seen, seenLen);
    n = tdefl_compress_mem_to_mem(msg + PRED_HEAD, cap - PRED_HEAD, raw, rawLen,
                                  tdefl_create_comp_flags_from_zip_params(
                                      MZ_BEST_COMPRESSION, -MZ_DEFAULT_WINDOW_BITS,
                                      MZ_DEFAULT_STRATEGY));
    free(raw);
    if (n == 0 || PRED_HEAD + n > PRED_MSG_MAX) {
        free(msg);
        return NULL;
    }
    memcpy(msg, "SWPD", 4);
    msg[4] = PRED_VERSION;
    msg[5] = (uint8_t)player;
    msg[6] = (uint8_t)own->delay;
    msg[7] = own->balance;
    BE_WRITE_U32(msg + 8, own->predFrames);
    BE_WRITE_U32(msg + 12, seenLen);
    BE_WRITE_U32(msg + 16, readLen / SRP_READ_BYTES);
    *outLen = (uint32_t)(PRED_HEAD + n);
    return msg;
}

void MatchPrediction_Offer(int32_t player, const SrpPrediction *own)
{
    free(s_mp.msg);
    s_mp.msg = encode(player, own, 1, &s_mp.msgLen);
    if (!s_mp.msg) {
        s_mp.msg = encode(player, own, 0, &s_mp.msgLen);
        if (s_mp.msg)
            fprintf(stderr, "[replay] this player's SEEN is too big for the link: the other "
                            "player's replay gets its PRED only\n");
    }
    s_mp.player = player;
    s_mp.sent = 0;
}

const SrpPrediction *MatchPrediction_Peer(void)
{
    return s_mp.havePeer ? &s_mp.peer : NULL;
}

static void take(const uint8_t *msg, uint32_t len)
{
    uint32_t frames, seenLen, rawLen, readFrames, readLen;
    uint8_t *raw;

    if (len < PRED_HEAD || memcmp(msg, "SWPD", 4) != 0 || msg[4] != PRED_VERSION)
        return;
    frames = BE_READ_U32(msg + 8);
    seenLen = BE_READ_U32(msg + 12);
    readFrames = BE_READ_U32(msg + 16);
    if (frames > 0x1000000u || seenLen > 64u << 20 || readFrames > 0x1000000u)
        return;
    readLen = readFrames * SRP_READ_BYTES;
    rawLen = frames + readLen + seenLen;
    raw = (uint8_t *)malloc(rawLen + 1);
    if (!raw)
        return;
    if (tinfl_decompress_mem_to_mem(raw, rawLen, msg + PRED_HEAD, len - PRED_HEAD, 0) != rawLen) {
        free(raw);
        return;
    }
    free(s_mp.peerBuf);
    s_mp.peerBuf = raw;
    s_mp.peer.pred = raw;
    s_mp.peer.predFrames = frames;
    s_mp.peer.seen = raw + frames + readLen;
    s_mp.peer.seenLen = seenLen;
    s_mp.peer.read = readLen ? raw + frames : NULL;
    s_mp.peer.readFrames = readFrames;
    s_mp.peer.delay = msg[6];
    s_mp.peer.balance = msg[7];
    s_mp.havePeer = 1;
    fprintf(stderr, "[replay] player %d's prediction data arrived: %u frames, %u bytes of SEEN\n",
            (int)msg[5], frames, seenLen);
}

void MatchPrediction_Poll(void)
{
    const uint8_t *msg;
    uint32_t len;
    uint16_t gen;

    if (P2PLink_State() != P2P_CONNECTED)
        return;
    if (s_mp.msg && !s_mp.sent && P2PLink_Send(CH_PRED, s_mp.msg, s_mp.msgLen) == 0)
        s_mp.sent = 1;
    if (!s_mp.havePeer && (msg = P2PLink_Received(CH_PRED, &len, &gen)) != NULL &&
        gen != s_mp.staleGen)
        take(msg, len);
    /* Added once our own file exists; if theirs came first, the file was written with it
     * (replay_v2.c write_recording) and this finds nothing to add. */
    if (s_mp.havePeer && s_mp.msg && !s_mp.added) {
        s_mp.added = 1;
        ReplayRec_AddPeerPrediction(&s_mp.peer);
    }
}

#else   /* no online matches */

void MatchPrediction_Begin(void) {}
void MatchPrediction_Offer(int32_t player, const SrpPrediction *own) { (void)player; (void)own; }
const SrpPrediction *MatchPrediction_Peer(void) { return NULL; }
void MatchPrediction_Poll(void) {}

#endif
