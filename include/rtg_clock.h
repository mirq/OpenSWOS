/*
 * rtg_clock.h -- what the two Amiga RTG builds (WarpOS, the 68060; docs/WARPOS.md,
 * docs/RTG060.md) do differently below the platform files: the clock, a short
 * wait, and making memory another processor reads current. src/rtg_clock.c.
 *
 *   WarpOS   powerpc.library's GetSysTimePPC and WaitTime, which never leave the
 *            PPC; dcbf, because the 68k does not snoop the PPC's data cache.
 *   68060    timer.device: ReadEClock for the clock (a library call, no I/O) and a
 *            UNIT_MICROHZ request for the wait. One processor, so nothing to flush:
 *            only the compiler is told memory changed.
 */
#ifndef RTG_CLOCK_H
#define RTG_CLOCK_H

#include "swos_types.h"

#if defined(SWOS_AMIGA_RTG_BUILD)

/* Microseconds since an arbitrary start; monotonic. */
uint64_t RtgClock_NowUs(void);
/* Sleep about `us` microseconds (the OS may wake us later; platform_wos.c spins
 * the rest). */
void     RtgClock_WaitUs(uint32_t us);
/* Write `len` bytes at `addr` back to memory, past any copy in this CPU's cache,
 * so the 68k side (AHI's hook, its mixer) sees them -- and drop that copy, so a
 * word the 68k side wrote is read afresh. */
void     RtgClock_Flush(const void *addr, uint32_t len);
void     RtgClock_Shutdown(void);

#endif
#endif /* RTG_CLOCK_H */
