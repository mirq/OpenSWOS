/*
 * Amiga SWOS RE Project
 * SWOS Amiga C/68000 Reversed by Ben Baker (https://github.com/benbaker76)
 * SWOS DOS Reversed by Zlatko Karakaš (https://github.com/zlatkok)
 */
#include "image_cmd.h"
#include "jag_graphics.h"
#include "game_types.h"
#include "display.h"
#include "intro_chip_ram.h"   /* the sprite pool, for its end */
#ifdef SWOS_CHUNKY_BUILD
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv — SWOS_DEBUG_SPRITE_LOAD */
int _debug_sprite_load = 0;  /* flip to 1 in gdb, or set SWOS_DEBUG_SPRITE_LOAD=1 */
__attribute__((constructor))
static void unbuf_stderr(void) { setbuf(stderr, NULL); }
#endif

/* ---- Byte-packed field reads ----
 * The .inc command-stream files store byte-packed pairs (xOff/yOff,
 * w/h, dstX/dstY) as native uint16_t constants where the Amiga
 * "first param" (originally read by `move.b (a0)+,d1`) is the HIGH
 * byte of the value and the "second param" is the LOW byte.
 *
 * Reading these via byte indices (`a0[0]`, `a0[1]`) gives different
 * results on LE vs BE because the same uint16 constant occupies the
 * two memory bytes in opposite order. Shifting the native uint16
 * value works the SAME on both hosts because the operation is on the
 * integer value, not on the byte positions.
 *
 * The original code had ad-hoc `a0[1]/a0[0]` reads with comments
 * explaining "LE host = byte 1 is high byte", which produced correct
 * results on x86 SDL but swapped pairs on BE Amiga — visible as
 * sprite extraction with mismatched w/h, dstX/dstY, etc., and
 * particularly as garbled menu glyphs (a glyph "blit at (x,y) of
 * size (w,h)" with w and h transposed extracts the wrong character
 * from the sheet). */
#define PACKED_HI(p)     ((uint16_t)((*(const uint16_t *)(p) >> 8) & 0xFFu))
#define PACKED_LO(p)     ((uint16_t)(*(const uint16_t *)(p) & 0xFFu))
#define PACKED_HI_S8(p)  ((int16_t)(int8_t)PACKED_HI(p))
#define PACKED_LO_S8(p)  ((int16_t)(int8_t)PACKED_LO(p))

/* ---- External globals (all live in SWOS2.s) ----
 * imageStreamCursor, frontbufferBasePtr, spriteGraphicsPtr are pointers stored
 * as uint32_t in asm. We declare them matching other C files and cast in use.
 */
extern uint8_t *_imageStreamCursor;           /* matches typed storage in swos_data_typed_stubs.c */
extern uint8_t _frontbufferMaskBuffer[];
extern volatile uintptr_t _frontbufferBasePtr;         /* actually uint8_t*, match display.c */
extern uint16_t _panicReasonCode;
extern uint8_t *_spriteGraphicsPtr;          /* actually uint32_t*[], match game_utils.c */
extern int16_t _currentSpriteIndex;
extern volatile uint16_t _playGame;
extern volatile uint16_t _playHighlightFlag;
extern uint16_t _useIndividualPlayerSkinColor;
extern uint32_t _imageStreamBytesLeft;
extern uint8_t _menuInsBank[];
#ifdef SWOS_CHUNKY_BUILD
extern uint8_t _rawAssetLoadBuffer[];
#endif

/* ---- External functions ---- */
extern void PanicHaltLoop(void) __attribute__((noreturn));
extern int16_t LoadFileEx(uint8_t *filename, uint8_t *dest, uint32_t *outFileSize);

/* Current sprite-bank bitplane depth. Auto-detected from each 0x7000/0x8000
 * file-load's decompressed size (SDL build only). A single image script can
 * load both 4bpp sheets (cjc*, team kits) and the 8bpp soccer_s.raw, so the
 * depth must track per-bank rather than the compile-time NUM_PLANES. Stays
 * NUM_PLANES on the Amiga build (only the SDL load branch updates it), so the
 * Amiga bitplane path is byte-identical. */
static uint16_t s_bankSrcPlanes = NUM_PLANES;

#ifdef SWOS_CHUNKY_BUILD
/* Set for the banks the Jaguar tier provides, cleared for every other: their
 * colours are the Jaguar sheets' own and have to be placed (JagGfx_SheetRemap).
 * Like s_bankSrcPlanes, it follows the bank being loaded, not the build. */
static const uint8_t *s_jagRemap;
#endif


/* ================================================================
 * blitDispatch  (SWOS2.s line 6481)
 *
 * Builds a SpriteGraphics entry in the image stream, copies/generates
 * pixel data and mask data.
 *
 * Parameters (matching asm register assignments):
 *   spriteIndex  (d0)  - index into sprite table
 *   xOffset      (d1)  - source x offset (pixels, shifted to byte)
 *   yOffset      (d2)  - source y offset (pixels, multiplied by stride)
 *   width        (d3)  - source pixel width
 *   height       (d4)  - source height in lines
 *   maskSel      (d5)  - mask selector: 0=no mask, >0=sprite index, <0=inline compose
 *   dstX         (d6)  - destination X offset
 *   dstY         (d7)  - destination Y offset
 *   src          (a1)  - source buffer pointer (NULL = checkerboard fill)
 *   planesFlag   (a6)  - 0=single plane mask, 1=multi-plane mask
 * ================================================================ */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
static void blitDispatch(uint16_t spriteIndex, uint16_t xOffset, uint16_t yOffset,
                         uint16_t width, uint16_t height, uint16_t maskSel,
                         int16_t dstX, int16_t dstY,
                         uint8_t *src, uint32_t planesFlag)
{
    SpriteGraphics *headerBase;  /* pointer to SpriteGraphics header */
    uint8_t *pixelPtr;           /* working write cursor for pixel/mask data */
    uint16_t wquads;             /* width in 16-pixel tiles */
#ifndef SWOS_CHUNKY_BUILD
    int32_t rowSkip;             /* bytes to skip between source rows */
    uint16_t i, j, p;
    uint16_t *wp;
    /* Real bitplane depth this sprite is STORED at (4 for a 16-colour sprite, 8 for the
     * soccer_s/replay bank). Was hard NUM_PLANES(8) — zero-padding every 16-colour sprite
     * to 8 planes doubled the sprite pool and overflowed _imageCmdWorkBuf into
     * _spriteGraphics. Every sprite-DATA reader derives the same value via spriteNumPlanes. */
    uint16_t sp = spriteNumPlanes((int16_t)spriteIndex);
#endif

    /* Panic if mask selector is zero */
    if (maskSel == 0) {
        _panicReasonCode = 0x00C6;
        PanicHaltLoop();
    }

    /* ---- Sprite index filtering (game mode only) ---- */
    if (_playGame != 0 && _useIndividualPlayerSkinColor == 0) {
        if (spriteIndex >= 0x148 && spriteIndex <= 0x1AC) return;
        if (spriteIndex >= 0x1AD && spriteIndex <= 0x211) return;
        if (spriteIndex >= 0x277 && spriteIndex <= 0x2DB) return;
        if (spriteIndex >= 0x2DC && spriteIndex <= 0x340) return;
        if (spriteIndex >= 0x37B && spriteIndex <= 0x3B4) return;
        if (spriteIndex >= 0x3EF && spriteIndex <= 0x428) return;
    }

    /* ---- Write sprite table entry pointer ---- */
    headerBase = (SpriteGraphics *)_imageStreamCursor;
    ((SpriteGraphics **)_spriteGraphicsPtr)[spriteIndex] = headerBase;
#ifdef SWOS_CHUNKY_BUILD
    {
        extern int _debug_sprite_load;
        if (_debug_sprite_load) {
            fprintf(stderr, "[sprite] extract idx=0x%03X (%u) w=%u h=%u cx=%d cy=%d src=%s\n",
                    spriteIndex, spriteIndex, width, height, (int)dstX, (int)dstY,
                    src == (uint8_t *)0 ? "CHECKERBOARD" : "buf");
        }
    }
#endif

    /* ---- Write SpriteGraphics header ---- */
    headerBase->ordinal       = (int16_t)spriteIndex;
    _currentSpriteIndex        = (int16_t)spriteIndex;
    headerBase->centerX       = dstX;
    headerBase->centerY       = dstY;
    headerBase->pixWidth      = (int16_t)width;
    headerBase->nlines        = (int16_t)height;
    headerBase->type          = 0;
    if (planesFlag != 0)
        headerBase->type      = 0xFF;  /* sne sets 0xFF when condition true */

    /* Adjust source pointer for x/y offset (if src != NULL).
     * Source row stride is the bank's own depth (s_bankSrcPlanes * 40),
     * NOT the compile-time IMAGE_ROW_STRIDE_BYTES — a 4bpp bank on an
     * 8-plane build has a 160-byte row, not 320. On the Amiga build
     * s_bankSrcPlanes == NUM_PLANES so this equals IMAGE_ROW_STRIDE_BYTES. */
    if (src != (uint8_t *)0) {
        uint32_t byteOff = (uint32_t)((xOffset >> 3) & 0xFFFE);
        uint32_t lineOff = (uint32_t)yOffset * (uint32_t)(BITPLANE_ROW_BYTES * s_bankSrcPlanes);
        src += byteOff + lineOff;
    }

    /* ---- Calculate tile dimensions ---- */
    wquads = (uint16_t)((width + 15) >> 4);
    headerBase->wquads = (int16_t)wquads;

    /* nlinesDiv4 << 8 | nlines/height encoding:
       asm does: lsl.w #6,d0; add.w d3,d0; add.w d3,d0
       → d0 = height * 64 + wquads * 2 */
    {
        uint16_t val = (uint16_t)(height << 6) + wquads + wquads;
        headerBase->bltsize = val;
    }

    /* sprData pointer = start of payload (just past the header) */
    headerBase->sprData = (uint8_t *)(headerBase + 1);
    pixelPtr = (uint8_t *)(headerBase + 1);

#ifdef SWOS_CHUNKY_BUILD
    /* SDL build: decode the Amiga bitplane source into chunky 8bpp.
     * Each output byte = palette index (0..NUM_COLOURS-1); index 0 is
     * treated as transparent by the renderers. Row stride =
     * wquads * 16 = pixel width padded to a 16-pixel multiple. We do
     * NOT store a separate mask (transparency is implicit via index 0).
     * The `type` field is left 0; `maskPtr` is set to 0. */
    headerBase->maskPtr = 0;
    {
        uint16_t pitch = wquads * 16;
        /* Chunky output is one byte per pixel regardless of planesFlag.
         * The Amiga planesFlag=1 path stored bitplane-interleaved data
         * (height × NUM_PLANES plane-rows), but here we gather bits from
         * all planes of each source row into a single chunky row — so
         * always emit exactly `height` rows. Using height*NUM_PLANES here
         * both wastes buffer space and walks past the sprite's source
         * data into the next sprite's rows. */
        uint16_t outRows = height;
        uint8_t *out = pixelPtr;

        if (src == (uint8_t *)0) {
            /* Checkerboard fill (matches Amiga 0x5555 pattern); write
             * palette index 5 in the set cells, 0 elsewhere. */
            for (uint16_t r = 0; r < outRows; r++) {
                for (uint16_t c = 0; c < pitch; c++)
                    out[r * pitch + c] = ((c >> 1) & 1u) ? 0u : 5u;
            }
        } else {
            /* Source is bitplane-interleaved: s_bankSrcPlanes slabs of 40
             * bytes each per scanline (4 for sprite sheets, 8 for soccer_s).
             * For each output pixel, gather bit (7 - (px & 7)) of byte
             * (px >> 3) from each plane, stack them into a palette index
             * (0..15 for 4bpp, 0..255 for 8bpp). */
            uint16_t srcPlanes = s_bankSrcPlanes;
            uint32_t srcRowStride = (uint32_t)BITPLANE_ROW_BYTES * srcPlanes;
            for (uint16_t r = 0; r < outRows; r++) {
                const uint8_t *rowSrc = src + (size_t)r * srcRowStride;
                uint8_t *rowDst = out + (size_t)r * pitch;
                for (uint16_t px = 0; px < pitch; px++) {
                    uint8_t bit = (uint8_t)(7 - (px & 7));
                    uint8_t byte = (uint8_t)(px >> 3);
                    uint8_t idx = 0;
                    for (uint16_t p = 0; p < srcPlanes; p++) {
                        uint8_t planeByte = rowSrc[p * BITPLANE_ROW_BYTES + byte];
                        idx |= (uint8_t)(((planeByte >> bit) & 1u) << p);
                    }
                    /* A Jaguar bank is drawn in its own 256 colours; put them
                     * where they live in the palette (jag_graphics.c). The kit
                     * block and the low 16 pass through -- the per-team recolour
                     * is what turns those into a team's colours. */
                    rowDst[px] = s_jagRemap ? s_jagRemap[idx] : idx;
                }
            }
        }

        /* maskSel / planesFlag don't affect the chunky pixel buffer —
         * transparency is implicit via palette index 0 and the blitter
         * reads one chunky byte per pixel from [sprData, sprData + h*pitch).
         * Just advance the stream cursor past this sprite's data. */
        (void)maskSel;
        (void)planesFlag;
        pixelPtr = out + (size_t)outRows * pitch;
        goto update_cursor;
    }
#else
    /* ---- Copy or fill pixel data (Amiga bitplane path) ---- */
    if (src == (uint8_t *)0) {
        /* Fill with checkerboard pattern 0x5555 — `sp` plane rows (was NUM_PLANES). */
        rowSkip = (int32_t)(BITPLANE_ROW_BYTES - (int32_t)wquads * 2);
        wp = (uint16_t *)pixelPtr;
        {
            uint16_t totalRows = (uint16_t)(height * sp);
            for (i = 0; i < totalRows; i++) {
                for (j = 0; j < wquads; j++) {
                    *wp++ = 0x5555;
                }
            }
        }
        pixelPtr = (uint8_t *)wp;
    } else {
        /* Store the sprite at its REAL depth `sp` (4 for a 16-colour sprite; 8 for the
         * soccer_s/replay bank). Copy the source bank's own `srcPlanes` interleaved rows;
         * zero-pad if the bank is shallower than `sp`, or skip source rows deeper than `sp`.
         * Normally srcPlanes == sp. This DROPS the old blanket zero-pad-to-8 that doubled
         * the pool — every sprite-DATA reader derives the same `sp` via spriteNumPlanes. */
        rowSkip = (int32_t)(BITPLANE_ROW_BYTES - (int32_t)wquads * 2);
        {
            uint16_t *srcW = (uint16_t *)src;
            uint16_t srcPlanes = s_bankSrcPlanes;
            uint16_t r;
            wp = (uint16_t *)pixelPtr;
            for (r = 0; r < height; r++) {
                for (p = 0; p < sp; p++) {
                    if (p < srcPlanes) {
                        for (j = 0; j < wquads; j++) {
                            *wp++ = *srcW++;
                        }
                        srcW = (uint16_t *)((uint8_t *)srcW + rowSkip);
                    } else {
                        for (j = 0; j < wquads; j++) {
                            *wp++ = 0;
                        }
                    }
                }
                /* Skip source plane rows beyond `sp` (source deeper than stored). */
                for (; p < srcPlanes; p++)
                    srcW = (uint16_t *)((uint8_t *)srcW + (int32_t)wquads * 2 + rowSkip);
            }
            pixelPtr = (uint8_t *)wp;
        }
    }

    /* ---- Mask data generation ---- */
    /* Clear mask pointer initially */
    headerBase->maskPtr = 0;

    if (maskSel == 0) {
        /* No mask — jump to cursor update */
        goto update_cursor;
    }

    if ((int16_t)maskSel > 0) {
        /* Positive maskSel: copy mask pointer from another sprite */
        SpriteGraphics *otherSprite = ((SpriteGraphics **)_spriteGraphicsPtr)[maskSel];
        headerBase->maskPtr = otherSprite->maskPtr;
        goto update_cursor;
    }

    /* Negative maskSel (bit 15 set): inline mask composition */
    headerBase->maskPtr = pixelPtr;

    if (src == (uint8_t *)0 || maskSel == (uint16_t)0xFFFE) {
        /* Generate right-edge mask */
        uint16_t edgeBits = (uint16_t)(wquads * 16 - (uint16_t)headerBase->pixWidth);
        uint16_t edgeMask = (uint16_t)(0xFFFF << edgeBits);
        uint16_t totalRows = (uint16_t)headerBase->nlines;
        if (planesFlag != 0)
            totalRows *= sp;

        wp = (uint16_t *)pixelPtr;
        for (i = 0; i < totalRows; i++) {
            for (j = 0; j + 1 < wquads; j++) {
                *wp++ = 0xFFFF;
            }
            *wp++ = edgeMask;
        }
        pixelPtr = (uint8_t *)wp;
    } else {
        /* Compose mask by OR'ing bitplanes together */
        uint8_t *pixelData = headerBase->sprData;
        uint16_t nlines = (uint16_t)headerBase->nlines;
        uint16_t planeWordStride = wquads * 2;  /* bytes per plane row */
        uint16_t rowIdx;

        wp = (uint16_t *)pixelPtr;
        for (rowIdx = 0; rowIdx < nlines; rowIdx++) {
            uint16_t planeRepeat;
            uint16_t *rowBase;

            if (planesFlag == 0)
                planeRepeat = 1;
            else
                planeRepeat = sp;

            rowBase = (uint16_t *)pixelData;

            for (i = 0; i < planeRepeat; i++) {
                uint16_t *plane0 = rowBase;
                for (j = 0; j < wquads; j++) {
                    uint16_t accum = *plane0;
                    uint16_t offset = planeWordStride;
                    uint16_t k;
                    for (k = 0; k + 1 < sp; k++) {
                        accum |= *(uint16_t *)((uint8_t *)plane0 + offset);
                        offset += planeWordStride;
                    }
                    *wp++ = accum;
                    plane0++;
                }
            }

            /* Advance pixelData by wquads * sp * 2 bytes per scanline (all planes). */
            pixelData += (uint32_t)wquads * (uint32_t)sp * 2u;
        }
        pixelPtr = (uint8_t *)wp;
    }
#endif /* SWOS_CHUNKY_BUILD */

update_cursor:
    /* ---- Update stream cursor and check bounds ---- */
    _imageStreamCursor = pixelPtr;

#ifdef SWOS_CHUNKY_BUILD
    /* The Amiga check depended on `menuInsBank` living immediately
     * past the end of `imageCmdWorkBuf` in BSS, so any walk past the
     * work-buffer's capacity tripped the panic. On our host the
     * linker places these symbols at unrelated addresses, rendering
     * the comparison nonsensical (menuInsBank could be LOWER than
     * imageCmdWorkBuf). We skip the boundary check; run-away writes
     * will be caught by normal segmentation faults instead. */
    _imageStreamBytesLeft = 0x40000;
#else
    /* The sprite pool's real end (include/intro_chip_ram.h). The original checked the
     * address of whatever label followed the pool in its BSS -- menuInsBank in the
     * menus, imageStreamCursor in a match -- which here are unrelated globals, so an
     * overflow was never caught (2026-10-14). */
    {
        uint8_t *poolEnd = g_introChipRam.spritePool + SPRITE_POOL_BYTES;
        _imageStreamBytesLeft = (uint32_t)(poolEnd - pixelPtr);
        if (pixelPtr >= poolEnd) {
            _panicReasonCode = 0x0008;
            PanicHaltLoop();
        }
    }
#endif
}

/* ================================================================
 * ProcessImageCommandStream  (SWOS2.s line 6253)
 *
 * Image command stream interpreter. Reads opcodes from cmdBuffer,
 * dispatches drawing operations via blitDispatch, and finishes by
 * copying the frontbuffer to the mask buffer.
 *
 * Opcode format: upper 4 bits = type, lower 12 bits = sprite index.
 * 0xFFFF = end of stream.
 *
 * Parameters:
 *   cmdBuffer  (a0) - pointer to the command stream
 *   workBuffer (a3) - destination buffer for sprite data
 * ================================================================ */
__attribute__((used, externally_visible, optimize("no-tree-loop-distribute-patterns")))
void ProcessImageCommandStream(const uint16_t *cmdBuffer, uint8_t *workBuffer)
{
#ifdef SWOS_CHUNKY_BUILD
    /* SWOS_DEBUG_SPRITE_LOAD=1 traces every extract with its hotspot, which is
     * what the onScreen cull reads — useful for comparing our bank's metrics
     * against another release's (docs/TESTS.md). */
    { static int probed; if (!probed) { probed = 1;
        if (getenv("SWOS_DEBUG_SPRITE_LOAD")) _debug_sprite_load = 1; } }
#endif
    const uint8_t *a0 = (const uint8_t *)cmdBuffer;
    uint16_t opcode, opcodeType;
    uint16_t spriteIndex, xOff, yOff, w, h, maskSel;
    int16_t dstX, dstY;
    uint32_t planesFlag;

#ifdef SWOS_CHUNKY_BUILD
    /* On SDL the file-load opcodes stage raw bitplane data into
     * `rawAssetLoadBuffer` so `frontbufferMaskBuffer` can remain in
     * chunky format. All subsequent blitDispatch calls read from the
     * staging buffer. */
    uint8_t *const imageSrcBuf = _rawAssetLoadBuffer;
#else
    uint8_t *const imageSrcBuf = _frontbufferMaskBuffer;
#endif

    _imageStreamCursor = workBuffer;
    /* Default depth until the script's first 0x7000/0x8000 load sets it
     * (every real script loads its bank before drawing from it). */
    s_bankSrcPlanes = NUM_PLANES;

    for (;;) {
        opcode = *(const uint16_t *)a0;
        a0 += 2;

        if (opcode == 0xFFFF)
            break;

        opcodeType = opcode & 0xF000;

        /* ---- Opcode 0x7000: Indirect file load ---- */
        if (opcodeType == 0x7000) {
            const uint8_t *savedA0 = a0 + 4;  /* skip the pointer long */
            /* On 32-bit the slot holds a raw pointer; on 64-bit it
             * holds an index into imageScriptPtrTable (see
             * swos_globals.c::SwosSDL_InitImageScripts for why). */
#if UINTPTR_MAX == 0xFFFFFFFFu
            const uint8_t *filename = *(const uint8_t *const *)a0;
#else
            extern const void *_imageScriptPtrTable[];
            uint32_t idx = (uint32_t)a0[0] | ((uint32_t)a0[1] << 8) |
                           ((uint32_t)a0[2] << 16) | ((uint32_t)a0[3] << 24);
            const uint8_t *filename = (const uint8_t *)_imageScriptPtrTable[idx];
#endif
#ifdef SWOS_CHUNKY_BUILD
            if (_debug_sprite_load)
                fprintf(stderr, "[sprite] LOAD filename=%s\n",
                        filename ? (const char *)filename : "(null)");
#endif

            /* LoadFile with retry until success. SDL build stages the
             * raw bitplane load into `rawAssetLoadBuffer` so the chunky
             * `frontbufferMaskBuffer` isn't clobbered between sprite
             * extractions. */
#ifdef SWOS_CHUNKY_BUILD
            {
                uint32_t bankSize = 0;
                while (LoadFileEx((uint8_t *)filename, _rawAssetLoadBuffer, &bankSize) != 0)
                    ;
                /* Auto-detect this bank's bitplane depth from its decompressed
                 * size (320x256 planar sheet: 40 bytes/plane row * 256 rows).
                 * 4bpp for cjc / team kits, 8bpp for soccer_s. */
                s_bankSrcPlanes = DetectBankPlanes(bankSize, BITPLANE_ROW_BYTES, SPRITE_SHEET_ROWS);
                s_jagRemap = JagGfx_IsBank((const char *)filename) ? JagGfx_SheetRemap() : 0;
            }
#else
            {
                uint32_t bankSize = 0;
                while (LoadFileEx((uint8_t *)filename, _frontbufferMaskBuffer, &bankSize) != 0)
                    ;
                /* Same per-bank depth detect as the SDL branch: 320x256 planar
                 * sheet (40 B/plane row * 256 rows). blitDispatch uses this to
                 * store 4bpp players in planes 0-3 (16-colour hardware sprites)
                 * and 8bpp banks (soccer_s) at full depth. */
                s_bankSrcPlanes = DetectBankPlanes(bankSize, BITPLANE_ROW_BYTES, SPRITE_SHEET_ROWS);
            }
#endif

            a0 = savedA0;
            continue;
        }

        /* ---- Opcode 0x8000: Double-indirect file load ---- */
        if (opcodeType == 0x8000) {
            const uint8_t *savedA0 = a0 + 4;
            /* Same 32/64-bit handling as 0x7000 — the first deref
             * reads from the side table; the second deref is a normal
             * runtime pointer read (the pointed-to data is a real
             * native pointer, e.g. team1Filename which holds a
             * dynamically-set filename address). */
#if UINTPTR_MAX == 0xFFFFFFFFu
            const uint8_t *ptr = *(const uint8_t *const *)a0;
            const uint8_t *filename = *(const uint8_t *const *)ptr;
#else
            extern const void *_imageScriptPtrTable[];
            uint32_t idx = (uint32_t)a0[0] | ((uint32_t)a0[1] << 8) |
                           ((uint32_t)a0[2] << 16) | ((uint32_t)a0[3] << 24);
            const uint8_t *ptr = (const uint8_t *)_imageScriptPtrTable[idx];
            const uint8_t *filename = *(const uint8_t *const *)ptr;
#endif

#ifdef SWOS_CHUNKY_BUILD
            {
                uint32_t bankSize = 0;
                while (LoadFileEx((uint8_t *)filename, _rawAssetLoadBuffer, &bankSize) != 0)
                    ;
                /* Auto-detect this bank's bitplane depth from its decompressed
                 * size (320x256 planar sheet: 40 bytes/plane row * 256 rows).
                 * 4bpp for cjc / team kits, 8bpp for soccer_s. */
                s_bankSrcPlanes = DetectBankPlanes(bankSize, BITPLANE_ROW_BYTES, SPRITE_SHEET_ROWS);
                s_jagRemap = JagGfx_IsBank((const char *)filename) ? JagGfx_SheetRemap() : 0;
            }
#else
            {
                uint32_t bankSize = 0;
                while (LoadFileEx((uint8_t *)filename, _frontbufferMaskBuffer, &bankSize) != 0)
                    ;
                /* Same per-bank depth detect as the SDL branch: 320x256 planar
                 * sheet (40 B/plane row * 256 rows). blitDispatch uses this to
                 * store 4bpp players in planes 0-3 (16-colour hardware sprites)
                 * and 8bpp banks (soccer_s) at full depth. */
                s_bankSrcPlanes = DetectBankPlanes(bankSize, BITPLANE_ROW_BYTES, SPRITE_SHEET_ROWS);
            }
#endif

            a0 = savedA0;
            continue;
        }

        /* ---- Opcode 0xC000: Byte x/y, then shared dispatch A ----
         * Amiga packed xOff/yOff as the high/low bytes of a single BE
         * uint16_t. On our LE host the native byte order is reversed,
         * so byte[1] of the word holds the Amiga-high-byte (xOff) and
         * byte[0] holds the Amiga-low-byte (yOff). */
        if (opcodeType == 0xC000) {
            spriteIndex = opcode & 0x0FFF;
            xOff = PACKED_HI(a0);
            yOff = PACKED_LO(a0);
            a0 += 2;
            goto dispatch_pack_A;
        }

        /* ---- Opcode 0x1000: Word x/y, then shared dispatch A ---- */
        if (opcodeType == 0x1000) {
            spriteIndex = opcode & 0x0FFF;
            xOff = *(const uint16_t *)a0; a0 += 2;
            yOff = *(const uint16_t *)a0; a0 += 2;
            goto dispatch_pack_A;
        }

        /* ---- Opcode 0x5000: Like 0x1000 but maskSel=0xFFFE ----
         * The Amiga asm reads w/h as two consecutive bytes from the same
         * BE-packed uint16_t. On LE the swap matches `dispatch_pack_A`
         * (w is the high byte = a0[1], h is the low byte = a0[0]). The
         * original inline byte-sequential read here would only work on a
         * BE host. */
        if (opcodeType == 0x5000) {
            spriteIndex = opcode & 0x0FFF;
            xOff = *(const uint16_t *)a0; a0 += 2;
            yOff = *(const uint16_t *)a0; a0 += 2;
            w = PACKED_HI(a0);
            h = PACKED_LO(a0);
            a0 += 2;
            maskSel = 0xFFFE;
            planesFlag = 0;
            dstX = 0;
            dstY = 0;
            blitDispatch(spriteIndex, xOff, yOff, w, h, maskSel, dstX, dstY,
                         imageSrcBuf, planesFlag);
            continue;
        }

        /* ---- Opcode 0xD000: Byte x/y, then shared dispatch B ---- */
        if (opcodeType == 0xD000) {
            spriteIndex = opcode & 0x0FFF;
            xOff = PACKED_HI(a0);
            yOff = PACKED_LO(a0);
            a0 += 2;
            goto dispatch_pack_B;
        }

        /* ---- Opcode 0x2000: Word x/y, then shared dispatch B ---- */
        if (opcodeType == 0x2000) {
            spriteIndex = opcode & 0x0FFF;
            xOff = *(const uint16_t *)a0; a0 += 2;
            yOff = *(const uint16_t *)a0; a0 += 2;
            goto dispatch_pack_B;
        }

        /* ---- Opcode 0xE000: Byte x/y, then shared dispatch C ---- */
        if (opcodeType == 0xE000) {
            spriteIndex = opcode & 0x0FFF;
            xOff = PACKED_HI(a0);
            yOff = PACKED_LO(a0);
            a0 += 2;
            goto dispatch_pack_C;
        }

        /* ---- Opcode 0x3000: Word x/y, then shared dispatch C ---- */
        if (opcodeType == 0x3000) {
            spriteIndex = opcode & 0x0FFF;
            xOff = *(const uint16_t *)a0; a0 += 2;
            yOff = *(const uint16_t *)a0; a0 += 2;
            goto dispatch_pack_C;
        }

        /* ---- Opcode 0xA000: Word x/y, planesFlag=1, maskSel=0xFFFF ----
         * Byte-packed w/h and dstX/dstY fields — same BE/LE handling as
         * `dispatch_pack_D`: high byte (Amiga byte[0]) comes from a0[1]
         * on LE, low byte (Amiga byte[1]) from a0[0]. The inline
         * byte-sequential read this replaced was the remaining source of
         * rare "sprite cut in half" on 0xA0## player-skin slots
         * (0xA0E3..0xA111 in menu_color_text_script_cmds.inc) — h was
         * being read as w and vice-versa, so sprites that happened to
         * have h < w were clipped short vertically. */
        if (opcodeType == 0xA000) {
            spriteIndex = opcode & 0x0FFF;
            xOff = *(const uint16_t *)a0; a0 += 2;
            yOff = *(const uint16_t *)a0; a0 += 2;
            w = PACKED_HI(a0);
            h = PACKED_LO(a0);
            a0 += 2;
            dstX = PACKED_HI_S8(a0);
            dstY = PACKED_LO_S8(a0);
            a0 += 2;
            maskSel = 0xFFFF;
            planesFlag = 1;
            blitDispatch(spriteIndex, xOff, yOff, w, h, maskSel, dstX, dstY,
                         imageSrcBuf, planesFlag);
            continue;
        }

        /* ---- Opcode 0xF000: Byte x/y, then shared dispatch D ----
         * Same BE-packing semantics as 0xC/D/E000: in the Amiga asm,
         * `move.b (a0)+,d1 ; move.b (a0)+,d2` reads xOff from the first
         * byte and yOff from the second. On our LE host the uint16_t
         * constants in the .inc land with the bytes swapped, so
         * byte[1] holds the Amiga-high-byte (xOff) and byte[0] holds
         * the Amiga-low-byte (yOff). Previously this opcode read the
         * bytes in the opposite order from 0xC/D/E000, which made
         * every 0xF### player-sprite slot (0x148..) extract the wrong
         * rectangle from the RAW source — producing sprites that were
         * "half right" depending on which row/col the swap landed on. */
        if (opcodeType == 0xF000) {
            spriteIndex = opcode & 0x0FFF;
            xOff = PACKED_HI(a0);
            yOff = PACKED_LO(a0);
            a0 += 2;
            goto dispatch_pack_D;
        }

        /* ---- Opcode 0xB000: Word x/y, then shared dispatch D ---- */
        if (opcodeType == 0xB000) {
            spriteIndex = opcode & 0x0FFF;
            xOff = *(const uint16_t *)a0; a0 += 2;
            yOff = *(const uint16_t *)a0; a0 += 2;
            goto dispatch_pack_D;
        }

        /* ---- Opcode 0x9000: Draw from saved pointer (src=NULL) ---- */
        if (opcodeType == 0x9000) {
            spriteIndex = opcode & 0x0FFF;
            w = *(const uint16_t *)a0; a0 += 2;
            h = *(const uint16_t *)a0; a0 += 2;
            maskSel = 0xFFFF;
            planesFlag = 0;
            dstX = 0;
            dstY = 0;
            blitDispatch(spriteIndex, 0, 0, w, h, maskSel, dstX, dstY,
                         (uint8_t *)0, planesFlag);
            continue;
        }

        /* ---- Unknown opcode ---- */
        _panicReasonCode = 0x0378;
        PanicHaltLoop();

        /* ---- Shared dispatch entry points (using goto) ---- */
    /* dispatch_pack_*: the stream packs [w][h] into one BE uint16_t and
     * optionally [dstX][dstY] into another. On LE the byte order of
     * native uint16_t constants transcribed from the Amiga source is
     * reversed, so what's byte[0] on Amiga lives at byte[1] of our
     * word. Read accordingly. */
    dispatch_pack_A:
        /* Byte w/h, maskSel=0xFFFF, planesFlag=0, dstX=dstY=0 */
        w = PACKED_HI(a0);
        h = PACKED_LO(a0);
        a0 += 2;
        maskSel = 0xFFFF;
        planesFlag = 0;
        dstX = 0;
        dstY = 0;
        blitDispatch(spriteIndex, xOff, yOff, w, h, maskSel, dstX, dstY,
                     imageSrcBuf, planesFlag);
        continue;

    dispatch_pack_B:
        /* Byte w/h + byte dstX/dstY (sign-extended), maskSel=0xFFFF, planesFlag=0 */
        w = PACKED_HI(a0);
        h = PACKED_LO(a0);
        a0 += 2;
        dstX = PACKED_HI_S8(a0);
        dstY = PACKED_LO_S8(a0);
        a0 += 2;
        maskSel = 0xFFFF;
        planesFlag = 0;
        blitDispatch(spriteIndex, xOff, yOff, w, h, maskSel, dstX, dstY,
                     imageSrcBuf, planesFlag);
        continue;

    dispatch_pack_C:
        /* Byte w/h + byte dstX/dstY (sign-extended) + word maskSel, planesFlag=0 */
        w = PACKED_HI(a0);
        h = PACKED_LO(a0);
        a0 += 2;
        dstX = PACKED_HI_S8(a0);
        dstY = PACKED_LO_S8(a0);
        a0 += 2;
        maskSel = *(const uint16_t *)a0; a0 += 2;
        planesFlag = 0;
        blitDispatch(spriteIndex, xOff, yOff, w, h, maskSel, dstX, dstY,
                     imageSrcBuf, planesFlag);
        continue;

    dispatch_pack_D:
        /* Byte w/h + byte dstX/dstY (sign-extended) + word maskSel, planesFlag=1 */
        w = PACKED_HI(a0);
        h = PACKED_LO(a0);
        a0 += 2;
        dstX = PACKED_HI_S8(a0);
        dstY = PACKED_LO_S8(a0);
        a0 += 2;
        maskSel = *(const uint16_t *)a0; a0 += 2;
        planesFlag = 1;
        blitDispatch(spriteIndex, xOff, yOff, w, h, maskSel, dstX, dstY,
                     imageSrcBuf, planesFlag);
        continue;
    }

    /* ---- End of stream: copy frontbuffer to mask buffer ---- *
     * On Amiga this primes the mask buffer for subsequent masked
     * blits. On SDL the mask buffer holds the chunky menu background,
     * not a transparency mask, and the frontbuffer hasn't been drawn
     * to yet at this point — so the copy would just overwrite mask
     * contents with whatever zero-initialised bytes were in the
     * frontbuffer. Skip it. */
#ifndef SWOS_CHUNKY_BUILD
    {
        const uint16_t *srcPtr = (const uint16_t *)_frontbufferBasePtr;
        uint16_t *dstPtr = (uint16_t *)_frontbufferMaskBuffer;
        uint16_t count;
        for (count = 0; count <= FB_COPY_WORDS_MINUS1; count++) {
            *dstPtr++ = *srcPtr++;
        }
    }
#endif
}
