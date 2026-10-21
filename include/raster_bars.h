/*
 * raster_bars.h -- where the Amiga's match frame spends its time, drawn on the screen.
 *
 * Build with  make -f Makefile.amiga EXTRA_CFLAGS=-DRASTER_BARS  (never in a release).
 * Each phase of the match frame sets COLOR00 as it starts, and the beam paints the
 * lines it is drawing in that colour: the border strip right of the picture becomes
 * stacked bands, one per phase, as tall as that phase took. The band that is still
 * coloured at the bottom of the screen, or a frame whose bands run past it, is a frame
 * over budget -- the game then shows every other frame and runs slow.
 *
 * Only the border: this build shifts the picture's colours to the other half of the
 * palette (BPLCON4, display.c's RB_BANK), so the grass is no longer COLOR00. The copper
 * reloads the palette at the top of every frame and leaves BPLCON3 on bank 0, so a CPU
 * write to COLOR00 lands in the border's colour for the rest of that frame.
 */
#ifndef RASTER_BARS_H
#define RASTER_BARS_H

#if defined(RASTER_BARS) && !defined(SWOS_CHUNKY_BUILD)
#include "display.h"
/* Never in the first lines of a frame: the copper is loading the palette there, a bank at
 * a time, and a write between its bank selects lands in THAT bank's colour 0 -- the grass,
 * since this build swaps the halves. The load is done by line 8 (still vertical blank). */
#define RASTER(rgb12)   do { \
        while (((*(volatile uint32_t *)&_custom->vposr >> 8) & 0x1FFu) < 8u) {} \
        _custom->color[0] = (uint16_t)(rgb12); } while (0)
#else
#define RASTER(rgb12)   ((void)0)
#endif

/* The phases, 12-bit 0x0RGB. */
#define RASTER_UNDRAW   0x0F00  /* red: undraw last frame's bobs, panel        */
#define RASTER_SCROLL   0x0FF0  /* yellow: camera + pitch scroll (tile blits)  */
#define RASTER_SIM      0x00F0  /* green: the match -- ball, players, AI       */
#define RASTER_CROWD    0x0F0F  /* magenta: animated crowd tiles               */
#define RASTER_SPRITES  0x000F  /* blue: drawing the bobs                       */
#define RASTER_HUD      0x00FF  /* cyan: bench, stats, result                  */
#define RASTER_IDLE     0x0000  /* black: done, waiting for the vertical blank */

#endif /* RASTER_BARS_H */
