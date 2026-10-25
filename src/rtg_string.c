/*
 * rtg_string.c -- memcpy for the 68060 RTG build (docs/RTG060.md). Empty
 * everywhere else.
 *
 * libnix's memcpy is a call to exec's CopyMem -- a library call with its own
 * size and alignment checks on every copy, and the renderer copies row by row
 * (blitter.c). This copies longwords, four to a pass, once the destination is
 * longword-aligned; a misaligned source is the 68060's own business (it reads one
 * in hardware). Defined in the program, it is linked instead of the library's.
 * (libnix's memmove and memcmp are inline byte loops in its headers, which no
 * definition here can replace: a hot caller uses its own -- platform_wos.c's
 * rows_equal.)
 *
 * no-tree-loop-distribute-patterns: gcc would otherwise see the loops as a
 * memcpy and compile them into a call to this very function.
 */
#if defined(SWOS_RTG060_BUILD)

#include <string.h>
#include "swos_types.h"

#define RTG_COPY __attribute__((optimize("no-tree-loop-distribute-patterns")))

static RTG_COPY void copy_up(uint8_t *d, const uint8_t *s, size_t n)
{
    if (n >= 16) {
        uint32_t *dl;
        const uint32_t *sl;
        size_t q;
        while ((uintptr_t)d & 3) {
            *d++ = *s++;
            n--;
        }
        dl = (uint32_t *)d;
        sl = (const uint32_t *)s;
        for (q = n >> 4; q; q--) {
            dl[0] = sl[0];
            dl[1] = sl[1];
            dl[2] = sl[2];
            dl[3] = sl[3];
            dl += 4;
            sl += 4;
        }
        for (q = (n >> 2) & 3; q; q--)
            *dl++ = *sl++;
        d = (uint8_t *)dl;
        s = (const uint8_t *)sl;
        n &= 3;
    }
    while (n--)
        *d++ = *s++;
}

RTG_COPY void *memcpy(void *dst, const void *src, size_t n)
{
    copy_up((uint8_t *)dst, (const uint8_t *)src, n);
    return dst;
}

#endif /* SWOS_RTG060_BUILD */
