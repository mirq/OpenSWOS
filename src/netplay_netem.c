/*
 * netplay_netem.c -- a misbehaving network on the netplay socket. See
 * include/netplay_netem.h for the conditions it models and how to ask for them.
 *
 * This file is only the game's end of it: the environment variables, the clock,
 * and GGPO's socket. What happens to a packet -- the queue, the dice, the
 * profiles -- is include/netem_core.h, which the matchmaking server's relay uses
 * too, so a profile name means the same link on both.
 *
 * A packet that needs none of this is not touched at all: Netem_Send returns 1
 * and GGPO's own sendto runs, so "delay 0" really is no delay and not "one poll
 * later".
 */
#include "netplay_netem.h"

#ifdef SWOS_SDL_BUILD
#ifdef SWOS_NETPLAY

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <SDL.h>

#include "netem_core.h"

static int32_t   s_ready;
static NetemLink s_link;
static NetemLink s_lobby;           /* SWOS_NETEM_LINK=1: the same, in front of p2p_link */
static char      s_desc[320];
static char      s_summary[256];

static const char *env_str(const char *name)
{
    const char *v = getenv(name);
    return (v && v[0]) ? v : NULL;
}

static int32_t env_int(const char *name, int32_t dflt)
{
    const char *v = env_str(name);
    return v ? (int32_t)atoi(v) : dflt;
}

void Netem_Init(void)
{
    const NetemProfile *p = NULL;
    const char *want;
    NetemConfig c;
    uint32_t seed;

    if (s_ready)
        return;
    s_ready = 1;

    want = env_str("SWOS_NETEM_PROFILE");
    if (want) {
        p = NetemCore_FindProfile(want);
        if (!p) {
            int32_t i, n;
            const NetemProfile *all = NetemCore_Profiles(&n);
            fprintf(stderr, "[netem] no such profile '%s' -- have:", want);
            for (i = 0; i < n; i++)
                fprintf(stderr, " %s", all[i].name);
            fputc('\n', stderr);
        }
    }

    /* The profile, then every knob the environment sets on top of it. */
    NetemCore_ConfigFromProfile(&c, p);
    c.delayMs      = env_int("SWOS_NETEM_DELAY_MS",      c.delayMs);
    c.jitterMs     = env_int("SWOS_NETEM_JITTER_MS",     c.jitterMs);
    c.lossPct      = env_int("SWOS_NETEM_LOSS_PCT",      c.lossPct);
    c.reorderPct   = env_int("SWOS_NETEM_REORDER_PCT",   c.reorderPct);
    c.reorderMs    = env_int("SWOS_NETEM_REORDER_MS",    c.reorderMs);
    c.dupPct       = env_int("SWOS_NETEM_DUP_PCT",       c.dupPct);
    c.dupCount     = env_int("SWOS_NETEM_DUP_COUNT",     c.dupCount);
    c.throttlePct  = env_int("SWOS_NETEM_THROTTLE_PCT",  c.throttlePct);
    c.throttleMs   = env_int("SWOS_NETEM_THROTTLE_MS",   c.throttleMs);
    c.throttleDrop = env_int("SWOS_NETEM_THROTTLE_DROP", 0);
    c.tamperPct    = env_int("SWOS_NETEM_TAMPER_PCT",    0);
    c.tamperBits   = env_int("SWOS_NETEM_TAMPER_BITS",   1);

    /* Asking for a window without saying how often one opens is the easy
     * mistake, and a window that never opens is not a test: give it a rate. */
    if (c.throttlePct <= 0 && env_str("SWOS_NETEM_THROTTLE_MS"))
        c.throttlePct = 5;

    seed = (uint32_t)env_int("SWOS_NETEM_SEED", 0);
    if (!seed)
        seed = (uint32_t)SDL_GetTicks() * 2654435761u + 1u;

    NetemCore_Init(&s_link, &c, seed);
    if (env_int("SWOS_NETEM_LINK", 0))
        NetemCore_Init(&s_lobby, &c, seed * 2654435761u + 7u);   /* its own dice */
    NetemCore_Describe(&s_link.cfg, p ? p->name : NULL, s_desc, sizeof s_desc);
}

int Netem_Enabled(void)
{
    Netem_Init();
    return s_link.enabled;
}

const char *Netem_Describe(void)
{
    Netem_Init();
    return s_desc;
}

const char *Netem_Summary(void)
{
    Netem_Init();
    NetemCore_Summary(&s_link, s_summary, sizeof s_summary);
    return s_summary;
}

/* NetemEmit for GGPO's socket: the context is the socket itself. */
static void emit_sendto(void *ctx, const uint8_t *data, int32_t len,
                        const void *dst, int32_t dstLen, int32_t flags)
{
    int sock = (int)(intptr_t)ctx;
    sendto(sock, (const char *)data, (size_t)len, flags,
           (const struct sockaddr *)dst, (socklen_t)dstLen);
}

int Netem_Send(int sock, const char *buf, int len, int flags,
               const struct sockaddr *dst, int dstLen)
{
    Netem_Init();
    if (!s_link.enabled)
        return 1;                       /* send it yourself */
    return (int)NetemCore_Send(&s_link, SDL_GetTicks(), (const uint8_t *)buf, len,
                               dst, dstLen, flags, emit_sendto, (void *)(intptr_t)sock);
}

void Netem_Pump(int sock)
{
    Netem_Init();
    NetemCore_Pump(&s_link, SDL_GetTicks(), emit_sendto, (void *)(intptr_t)sock);
}

int Netem_LinkSend(int sock, const char *buf, int len, int flags,
                   const struct sockaddr *dst, int dstLen)
{
    Netem_Init();
    if (!s_lobby.enabled)
        return 1;
    return (int)NetemCore_Send(&s_lobby, SDL_GetTicks(), (const uint8_t *)buf, len,
                               dst, dstLen, flags, emit_sendto, (void *)(intptr_t)sock);
}

void Netem_LinkPump(int sock)
{
    Netem_Init();
    if (s_lobby.enabled)
        NetemCore_Pump(&s_lobby, SDL_GetTicks(), emit_sendto, (void *)(intptr_t)sock);
}

#else  /* !SWOS_NETPLAY */

void Netem_Init(void) {}
int  Netem_Enabled(void) { return 0; }
const char *Netem_Describe(void) { return ""; }
const char *Netem_Summary(void) { return ""; }
int  Netem_Send(int sock, const char *buf, int len, int flags,
                const struct sockaddr *dst, int dstLen)
{
    (void)sock; (void)buf; (void)len; (void)flags; (void)dst; (void)dstLen;
    return 1;
}
void Netem_Pump(int sock) { (void)sock; }
int  Netem_LinkSend(int sock, const char *buf, int len, int flags,
                    const struct sockaddr *dst, int dstLen)
{
    (void)sock; (void)buf; (void)len; (void)flags; (void)dst; (void)dstLen;
    return 1;
}
void Netem_LinkPump(int sock) { (void)sock; }

#endif /* SWOS_NETPLAY */
#endif /* SWOS_SDL_BUILD */
