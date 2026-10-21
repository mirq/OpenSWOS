/*
 * credits_menu.h -- the CREDITS screen (SDL builds). See src/credits_menu.c; the Amiga
 * AGA screen is menus.c CreditsAgaDraw, and both draw include/credits_text.h.
 */
#ifndef CREDITS_MENU_H
#define CREDITS_MENU_H

#ifdef SWOS_CHUNKY_BUILD

#include "swos_types.h"

/* The JSON-only menu behind the main menu's CREDITS button (docs/MENUS.md §10). */
#define CREDITS_MENU_NAME     "openswos/creditsMenu"
#define CREDITS_MENU_ENTRIES  2

/* Register the menu's handlers with the JSON loader. Idempotent; Show calls it. */
void CreditsMenu_Init(void);
/* Lend the photos their palette entries, show the screen, give them back. */
void CreditsMenu_Show(void);

#endif /* SWOS_CHUNKY_BUILD */
#endif /* CREDITS_MENU_H */
