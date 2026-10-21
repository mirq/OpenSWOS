/*
 * intro_chip_ram.h -- the chip RAM the title tune and the unpacked sprites share.
 *
 * The title tune (sound/hero.ins) plays only during the loader intro
 * (ShowLoaderIntroSequence), and every way out of the intro stops the sound
 * (loaderIntroStopMusicAndResetAudio) before it returns. The sprites are unpacked
 * (ProcessImageCommandStream, SWOS.c) only after it has returned. So on the Amiga
 * the two use the SAME chip RAM, one after the other -- as the original did: in
 * SWOS2.s audioInstrumentBank sits directly in front of imageCmdWorkBuf in one
 * BSS run, and the sprite pool was written over the tune's samples. Here that is
 * a union with named members instead of adjacent labels.
 *
 * SDL keeps them apart: its arcade attract loop plays the title tune while a
 * replay is drawn with the sprites, and memory is no constraint there.
 */
#ifndef INTRO_CHIP_RAM_H
#define INTRO_CHIP_RAM_H

#include "swos_types.h"

#ifdef SWOS_CHUNKY_BUILD
/* The SDL RJP1 player is handed the whole bank; the SDL sprite pool is chunky
 * 8bpp and documented to reach ~740 KB (swos_amiga_data_stubs.c). */
#define TITLE_TUNE_SAMPLES_BYTES  (512u * 1024u)
#define SPRITE_POOL_BYTES         0x200000u
typedef struct {
#else
/* hero.ins unpacks to 292,984 bytes, IN PLACE (LoadRNCToBufferNoBlit): the RNC
 * decoder may lift the packed data up to 255 bytes of leeway past the end of the
 * output first (rnc.c), so the buffer is that plus the leeway, rounded up. The
 * Amiga sprite pool is 4-plane (SWOS_SPRITE_4PLANE) but for the running frames. */
#define TITLE_TUNE_SAMPLES_BYTES  0x48000u      /* 288 KB (the union is the larger of the two) */
#define SPRITE_POOL_BYTES         0x4C000u      /* 304 KB: 4-plane sprites, but the in-match
                                                 * running frames 8 planes deep (display.h,
                                                 * spriteIsPadded8), ~265 KB + ~37 KB */
typedef union {
#endif
    uint8_t titleTuneSamples[TITLE_TUNE_SAMPLES_BYTES];  /* during the loader intro */
    uint8_t spritePool[SPRITE_POOL_BYTES];               /* from the sprite unpack on */
} IntroChipRam;

extern IntroChipRam g_introChipRam;

#endif /* INTRO_CHIP_RAM_H */
