/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance screens on the screen kit (MV_UI_KIT 1; Kris 14 Nov 2026: fm1-port/mvance-kit-ui-plan-2026-11-14.md,
 * decisions 1a .. 10a). What each screen shows; mv_kit.c draws it. Nothing here edits the patch or the settings: the
 * panel (mv_ui.c) still does all of that, so the pages, the knobs, the edits and the MIDI are the same as with the
 * panel's own screens.
 *
 *   pages      the header (the section's chip, the page's title and a pip a page of the section, the part tag U / L /
 *              L+U, a cell a voice); a graph of the page's values (the knob in hand's label and value large at its top
 *              left for 1.5 s, 8a); a knob cell a column (the kind from the value: a number's arc, a signed one's from
 *              the centre, a list's segments (up to 10 names, else an arc), OFF / ON a switch); the footer (PROG nn,
 *              the name, orange-red when edited; the tempo, or LOCAL OFF)
 *   HOME       the name, the program and its category (or DUAL / SPLIT and both names), the play mode, ARP, the
 *              octave; the scope while something sounds, the M-VANCE saw fan when nothing does (7a); CUTOFF RESO DELAY
 *              REVERB as knobs
 *   ROLL       the header, then the piano roll (mv_roll.c, in the theme's colours) over the panel and the cells (6b)
 *   overlays   PART SELECT, CLEAR TRACK?, TOO LONG: a card over the panel; the page's knobs stay
 *   others     SAVE, CLIP (the name, its cursor, what SAVE will do), DEBUG (the meter as bars), the transfer, the
 *              factory bank's question, and the programs' list for 2 s after PRESETS turns (10a)
 *   colour     GLOBAL 2/2: THEME (seven) and COLOUR (THEME, or BY TYPE: the colour of the name's category)
 *
 * Main loop only. #included into mvance.c just before app_main (the unity build): it reads the app's state (eng, ui,
 * set, prog, the scope's ring, the clock, the transfer) and mv_ui.c's page table and texts directly, as mv_home.c
 * and mv_tiles.c do. Integers only. */
#pragma once
#include <stdint.h>
#include "mv_kit.h"
#include "mv_ui.h"
#if MV_DEBUG
#include "mv_debug.h"
#endif

void mv_kui_draw(void);                         /* what changed on the screen in view, every pass */
void mv_kui_full(void);                         /* the whole screen again at the next mv_kui_draw (a page change) */
void mv_kui_prog_turned(void);                  /* PRESETS turned: the programs' list shows for 2 s */
#if MV_DEBUG
void mv_kui_debug(const mv_debug_t *d);         /* DEBUG's numbers (4 a second, while it shows) */
#endif
#if defined(DINGHY_WEBSIM)
/* the simulator's tile of page t->page, drawn whole into the canvas the screen calls go to (sim/mv_tiles.c points them
 * at its scratch); the live screen's band keys are kept */
void mv_kui_tile(const mv_ui_t *t);
#endif
