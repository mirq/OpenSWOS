/*
 * rtg_cgx68k.h -- the part of cybergraphics.library and ahi.device the 68060 RTG
 * build calls (docs/RTG060.md), for Bebbo's m68k-amigaos-gcc, which ships the NDK
 * but neither CyberGraphX's headers nor an inline header for AHI.
 *
 * Only what platform_wos.c and audio_wos.c use: the tag and format values are
 * CyberGraphX's published interface (cybergraphics.h 41.x), the offsets its
 * cybergraphics_lib.fd (bias 30). AHI's inline macros ARE in the NDK, under
 * defines/. The WarpOS build includes the SDK's own headers instead.
 */
#ifndef RTG_CGX68K_H
#define RTG_CGX68K_H

#include <exec/types.h>
#include <utility/tagitem.h>
#include <graphics/displayinfo.h>
#include <graphics/rastport.h>

/* ---- cybergraphics.library ------------------------------------------- */

#define CYBERGFXNAME            "cybergraphics.library"

/* GetCyberIDAttr() */
#define CYBRIDATTR_PIXFMT       (0x80000001)
#define CYBRIDATTR_WIDTH        (0x80000002)
#define CYBRIDATTR_HEIGHT       (0x80000003)
#define CYBRIDATTR_DEPTH        (0x80000004)
#define CYBRIDATTR_BPPIX        (0x80000005)

/* BestCModeIDTagList() */
#define CYBRBIDTG_TB            (TAG_USER + 0x50000)
#define CYBRBIDTG_Depth         (CYBRBIDTG_TB + 0)
#define CYBRBIDTG_NominalWidth  (CYBRBIDTG_TB + 1)
#define CYBRBIDTG_NominalHeight (CYBRBIDTG_TB + 2)
#define CYBRBIDTG_MonitorID     (CYBRBIDTG_TB + 3)
#define CYBRBIDTG_BoardName     (CYBRBIDTG_TB + 5)

#define PIXFMT_LUT8             (0UL)
#define RECTFMT_LUT8            (3UL)
#define CTABFMT_XRGB8           (0UL)   /* ULONG [] colour table */

/* LockBitMapTagList() */
#define LBMI_WIDTH              (0x84001001)
#define LBMI_HEIGHT             (0x84001002)
#define LBMI_DEPTH              (0x84001003)
#define LBMI_PIXFMT             (0x84001004)
#define LBMI_BYTESPERPIX        (0x84001005)
#define LBMI_BYTESPERROW        (0x84001006)
#define LBMI_BASEADDRESS        (0x84001007)

extern struct Library *CyberGfxBase;

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif

#define IsCyberModeID(___displayID) \
    LP1(0x36, BOOL, IsCyberModeID, ULONG, ___displayID, d0, \
        , CyberGfxBase)

#define BestCModeIDTagList(___tags) \
    LP1(0x3c, ULONG, BestCModeIDTagList, struct TagItem *, ___tags, a0, \
        , CyberGfxBase)

#define GetCyberIDAttr(___attr, ___id) \
    LP2(0x66, ULONG, GetCyberIDAttr, ULONG, ___attr, d0, ULONG, ___id, d1, \
        , CyberGfxBase)

#define LockBitMapTagList(___bm, ___tags) \
    LP2(0xa8, APTR, LockBitMapTagList, APTR, ___bm, a0, struct TagItem *, ___tags, a1, \
        , CyberGfxBase)

#define UnLockBitMap(___handle) \
    LP1NR(0xae, UnLockBitMap, APTR, ___handle, a0, \
        , CyberGfxBase)

#define WriteLUTPixelArray(___src, ___sx, ___sy, ___smod, ___rp, ___ctab, ___dx, ___dy, ___w, ___h, ___fmt) \
    LP11(0xc6, ULONG, WriteLUTPixelArray, APTR, ___src, a0, UWORD, ___sx, d0, UWORD, ___sy, d1, \
         UWORD, ___smod, d2, struct RastPort *, ___rp, a1, APTR, ___ctab, a2, UWORD, ___dx, d3, \
         UWORD, ___dy, d4, UWORD, ___w, d5, UWORD, ___h, d6, UBYTE, ___fmt, d7, \
         , CyberGfxBase)

/* ---- ahi.device's library calls ---------------------------------------- */

#include <devices/ahi.h>
#include <clib/ahi_protos.h>
#include <defines/ahi.h>        /* the NDK's m68k inline macros, despite the folder */
extern struct Library *AHIBase;

#endif /* RTG_CGX68K_H */
