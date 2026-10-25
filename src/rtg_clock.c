/*
 * rtg_clock.c -- the clock, the wait and the cache of the Amiga RTG builds
 * (include/rtg_clock.h says what differs and why). Empty everywhere else.
 */
#if defined(SWOS_AMIGA_RTG_BUILD)

#include "rtg_clock.h"

#pragma pack(push, 2)
#include <exec/types.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <proto/exec.h>
#ifdef SWOS_WARPOS_BUILD
#include <powerpc/powerpc.h>
#include <powerpc/powerpc_protos.h>
#else
#include <proto/timer.h>
#endif
#pragma pack(pop)

#ifdef SWOS_WARPOS_BUILD

uint64_t RtgClock_NowUs(void)
{
    struct timeval tv;
    GetSysTimePPC(&tv);
    return (uint64_t)tv.tv_secs * 1000000u + tv.tv_micro;
}

/* A wait that never leaves the PPC. */
void RtgClock_WaitUs(uint32_t us)
{
    WaitTime(0, (ULONG)us);
}

void RtgClock_Flush(const void *addr, uint32_t len)
{
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)31;
    uintptr_t end = ((uintptr_t)addr + len + 31) & ~(uintptr_t)31;
    for (; a < end; a += 32)
        __asm__ __volatile__ ("dcbf 0,%0" : : "r" (a) : "memory");
    __asm__ __volatile__ ("sync" ::: "memory");
}

void RtgClock_Shutdown(void) { }

#else /* the 68060 */

/* timer.device, opened on first use: TimerBase for ReadEClock, and the request
 * the waits go through. UNIT_MICROHZ: the wait is as fine as the timer allows
 * (UNIT_VBLANK would round to the native display's 20 ms). */
struct Device *TimerBase;
static struct MsgPort     *s_port;
static struct timerequest *s_req;
static int                 s_open, s_failed;
static ULONG               s_efreq;         /* E-clock ticks a second (709,379 on a PAL machine) */
static uint64_t            s_base;          /* the E-clock at the first call */

static int timer_open(void)
{
    struct EClockVal ev;
    if (s_open)
        return 1;
    if (s_failed)
        return 0;
    s_failed = 1;
    if (!(s_port = CreateMsgPort()))
        return 0;
    if (!(s_req = (struct timerequest *)CreateIORequest(s_port, sizeof *s_req)))
        return 0;
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)s_req, 0) != 0)
        return 0;
    TimerBase = s_req->tr_node.io_Device;
    s_efreq = ReadEClock(&ev);
    s_base = ((uint64_t)ev.ev_hi << 32) | ev.ev_lo;
    s_open = 1;
    s_failed = 0;
    return 1;
}

uint64_t RtgClock_NowUs(void)
{
    struct EClockVal ev;
    uint64_t t;
    if (!timer_open())
        return 0;
    ReadEClock(&ev);
    t = (((uint64_t)ev.ev_hi << 32) | ev.ev_lo) - s_base;
    /* Whole seconds and the rest apart: t * 1e6 alone overflows after ~300 days of
     * E-clock, and this way each product stays small. */
    return (t / s_efreq) * 1000000u + ((t % s_efreq) * 1000000u) / s_efreq;
}

void RtgClock_WaitUs(uint32_t us)
{
    if (!timer_open() || us == 0)
        return;
    s_req->tr_node.io_Command = TR_ADDREQUEST;
    s_req->tr_time.tv_secs    = us / 1000000u;
    s_req->tr_time.tv_micro   = us % 1000000u;
    DoIO((struct IORequest *)s_req);
}

/* One processor: AHI's hook and mixer run on this same 68060, through the same
 * cache. Only the compiler must not keep a value in a register across this. */
void RtgClock_Flush(const void *addr, uint32_t len)
{
    (void)addr; (void)len;
    __asm__ __volatile__ ("" ::: "memory");
}

void RtgClock_Shutdown(void)
{
    if (s_open)
        CloseDevice((struct IORequest *)s_req);
    if (s_req)
        DeleteIORequest((struct IORequest *)s_req);
    if (s_port)
        DeleteMsgPort(s_port);
    s_open = 0;
    s_req = NULL;
    s_port = NULL;
    TimerBase = NULL;
}

#endif
#endif /* SWOS_AMIGA_RTG_BUILD */
