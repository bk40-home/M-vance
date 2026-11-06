/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance piano roll: SEQ 2/2 (Kris 6 Nov, sequencer phase 2: fm1-port/mvance-sequencer-phase2-proposal-2026-11-06.md,
 * signed off "go with recommendations": 1a .. 8a). The look follows Skiff's STEP roll (illuminus5/skiff, ui_graph.c
 * graph_roll): a key strip with the C names, a lane per note, a bar as 16 sixteenths, notes as bars, a playhead.
 *
 *   y  0- 23   the header (mv_ui.c: "SEQ 2/2", the part tag L / U / L+U)
 *   y 26-209   MV_ROLL_ROWS (23) rows of 8 px, the highest note at the top (about 2 octaves); rows of white keys
 *              shaded, a line under each C
 *   x  0- 25   the key strip: the C names (C3 ..), the keys; a key lit while its note sounds
 *   x 26-234   the grid: one bar, 16 columns of 13 px (a 1/16 each), the beats brighter. Notes are drawn where they
 *              really are (the tracks are real time, 96 ticks a beat): from their note-on to their note-off,
 *              13 / 24 px a tick, 2 px at least. A note past the top or the bottom: a 1 px mark on the edge row
 *   notes      the selected track's (PART SELECT / PANEL) orange; with BOTH (a layered program), UPPER's orange and
 *              LOWER's blue under it. Drawn as recorded (6a): the TRANSPOSE offset is on the foot line
 *   playhead   a white line in the column playing, while the transport runs (moved a column at a time: 8 times a
 *              second at 120 BPM, two 13 px strips redrawn)
 *   y 220-235  the foot line: BAR n/m, ROOT, the offset, TRANSPOSE
 *   bar shown  running: the bar the selected track is playing; stopped: KNOB1's
 *   rows       fitted to the notes shown when the page opens or another track is shown, else KNOB2's (u->roll_lo)
 *
 * Main loop only. The ISR owns the notes (mv_seq_t): they are copied, and the copy kept only if seq.ver did not move
 * while copying (the ISR never waits for the main loop, so a copy can only be overtaken, never torn under it). Spans
 * (note-on .. note-off pairs) are made from the copy once per change. Drawing is keyed: the grid when the bar, the
 * rows, the notes or the mode change; the key strip when what sounds changes; the foot line when its text does;
 * otherwise only the playhead's two columns. #included into mvance.c after the colours (it draws with gfx.c). */
#pragma once
#include <stdint.h>
#include "mv_voice.h"
#include "mv_ui.h"

typedef struct {
    uint16_t a, b;                              /* note-on and note-off tick; b past the loop's end: it wraps */
    uint8_t note, trk;                          /* the note (as recorded), the track (MV_PART_*) */
} mv_roll_span_t;

typedef struct {
    uint32_t ver;                               /* seq.ver the spans were made from */
    uint16_t len[2];                            /* and each track's loop then (LENGTH moves them too) */
    uint8_t valid;                              /* spans made at least once */
    mv_roll_span_t sp[MV_SEQ_NEV];              /* both tracks' notes: 128 a track at most */
    uint32_t nsp;
    uint32_t grid_key, keys_key, foot_key;      /* what the grid, the key strip, the foot line show */
    int32_t ph;                                 /* the playhead's column drawn (-1 none) */
    uint8_t shown;                              /* the track(s) shown (trk | both << 1): a change fits the rows */
} mv_roll_t;

void mv_roll_init(mv_roll_t *r);
/* the page (not its header: mv_ui_draw): what changed; all: everything (the page was just entered, the screen
 * cleared). u: the bar and rows shown (the fit is done here, so u->roll_lo may move) */
void mv_roll_draw(mv_roll_t *r, mv_ui_t *u, const mv_engine_t *e, int all);
