/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance home screen (Kris 3 Nov: H1, the keyboard replaced by a scope, the home knobs; the logo's drawing,
 * tools/gen_logo.py, fm1-port/mvance-logo-homescreen-proposal-2026-11-03.md).
 *
 *   y   0- 47  header   seven sawtooth waves in phase at the centre, fanning out to both sides (the Super Saw's
 *                       detune made visible, the centre one brightest), a plate with M-VANCE: once a return HOME
 *   y  48- 99  program  PROGRAM nn (EDITED), the category chip from the name's two letters (LD LEAD, PD PAD ..),
 *                       the name big without them; a layered program: DUAL / SPLIT C4 in the chip, the UPPER and
 *                       the LOWER name a line each
 *   y 102-129  knobs    KNOB1..4 on HOME: CUTOFF, RESO, DELAY (send level, the program's), REVERB (the global
 *                       LEVEL); the one turned white for 3 s. Edited through mv_ui's own path (PAGES[0])
 *   y 132-143  voices   a bar per voice, its amp envelope in 8 steps; voices above the VOICES limit dimmed
 *   y 146-239  scope    the output (L+R)/2 every 2nd frame, from a rising zero crossing, 240 points (10.9 ms),
 *                       scaled to the window's peak (x16 at most, so silence stays flat); down to the screen's
 *                       foot since the meter and the status moved to the DEBUG page (GLO, mv_debug.h; Kris 3 Nov)
 *
 * Main loop only (never the ISR); each part is drawn only when what it shows changed. #included into mvance.c
 * after the colours (the unity build: it draws with gfx.c and reads mv_ui.c's PAGES / col_value). */
#pragma once
#include <stdint.h>
#include "mv_voice.h"

#define MV_SCOPE_N 512u                         /* the scope ring (int16, a power of 2): 23 ms at 2 frames a point */

void home_header(void);
/* the program: n, its (UPPER) name, edited; layered (mode DUAL / SPLIT): the chip says it, both names in a line each */
void home_program(uint32_t n, const uint8_t *name, int edited, uint32_t mode, const uint8_t *lower, uint32_t split);
/* the four knob cells; touched: the column being edited (0..3) or -1. Returns nothing: the caller keys it */
void home_knobs(const mv_engine_t *e, int touched);
uint32_t home_knobs_key(const mv_engine_t *e, int touched);   /* what home_knobs would show (redraw when it moves) */
void home_voices(const mv_engine_t *e);
uint32_t home_voices_key(const mv_engine_t *e);
/* the scope from the ring (written by the ISR; w = the next write index): returns 1 when it drew something other
 * than the flat line (the caller then keeps refreshing; after silence it draws the flat line once and stops) */
int home_scope(const int16_t *ring, uint32_t w);
/* the boot question (A1, Kris 3 Nov): this firmware's factory bank is new, the programs saved are the old one's.
 * Drawn once, under the header: SAVE loads the new bank (all 64 replaced), HOME keeps the saved programs */
void home_bank_ask(void);
/* the program transfer (mv_xfer.h): RECEIVING (n messages taken, the last into slot) or SENDING (n sent, the program
 * going out); full: the whole screen (else the counts only) */
void home_xfer(int sending, uint32_t n, uint32_t slot, int full);
