/*
 * p2p_menu.h -- the MULTIPLAYER menu (SDL builds). See src/p2p_menu.c.
 */
#ifndef P2P_MENU_H
#define P2P_MENU_H

#ifdef SWOS_CHUNKY_BUILD

#include "swos_types.h"

/* The JSON-only menu behind the main menu's MULTIPLAYER button (docs/MENUS.md §10). */
#define P2P_MENU_NAME     "p2p/p2pMenu"
#define P2P_MENU_ENTRIES  15

/* Register the menu's handlers with the JSON loader. Idempotent; Show calls it. */
void P2PMenu_Init(void);
void P2PMenu_Show(void);

#endif /* SWOS_CHUNKY_BUILD */
#endif /* P2P_MENU_H */
