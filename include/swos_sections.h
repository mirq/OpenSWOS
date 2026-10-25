/*
 * Amiga SWOS RE Project
 * SWOS Amiga C/68000 Reversed by Ben Baker (https://github.com/benbaker76)
 * SWOS DOS Reversed by Zlatko Karakaš (https://github.com/zlatkok)
 */
#ifndef SWOS_SECTIONS_H
#define SWOS_SECTIONS_H

/* On Amiga the BSSInChip.MEMF_CHIP section is mapped to CHIP RAM by the
 * linker script — required for buffers the copper/bitplane DMA reads.
 * On SDL the section attribute is meaningless and triggers "dangerous
 * relocation" link errors, so collapse to plain alignment. */
#if defined(__m68k__) && !defined(SWOS_RTG060_BUILD)
#define SWOS_CHIP_BSS __attribute__((section("BSSInChip.MEMF_CHIP"), aligned(4)))
#define SWOS_FAST_BSS __attribute__((section("bss"), aligned(4)))
#else
#define SWOS_CHIP_BSS __attribute__((aligned(4)))
#define SWOS_FAST_BSS __attribute__((aligned(4)))
#endif

#endif
