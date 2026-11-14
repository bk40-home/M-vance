/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance screen kit: the screens' look (knobs, graphs, themes), taken from M-Vsynth2's vs_ui.c (Kris 14 Nov 2026:
 * fm1-port/mvance-kit-ui-plan-2026-11-14.md, decisions 1a .. 10a). This half knows nothing of M-vance: themes,
 * drawing, text, the four bands and the knob cell. mv_kui.c (M-vance's screens) says what goes in them.
 * When fm1-kit exists (decision 1a) this file pair is what it replaces; until then it is a copy, kept to vs_ui.c's
 * names so the two can be compared line by line.
 *
 * Every screen is four bands, the same on every page, so a hand on KNOB 2 always reads the second cell:
 *   header   y   0..21   the section as a chip, the title, a pip a page of the section, the part tag, a cell a voice
 *   panel    y  24..127  a graph of what the page does (mv_kui.c), the knob in hand's readout at its top left
 *   cells    y 130..219  KNOB 1..4, left to right: a knob, its label, its value
 *   footer   y 222..239  the program, the name (WARN: edited), the tempo or the state of things
 * A band goes into gfx.c's canvas (CV_MAX 240 x 64: the panel in two strips of 52 rows, a cell 60 x 90) and out with
 * one DMA, and only when what it shows changed: each band keeps a hash (kit_keys_t). The panel redraws at most every
 * min_ms (kit_panel) while something in it moves.
 *
 * Main loop only (never the ISR). Integers only: no float, no 64-bit division. #included into mvance.c after gfx.c
 * (the unity build: it draws with gfx.c's static canvas) and only when MV_UI_KIT is 1 (mv_build.h). */
#pragma once
#include <stdint.h>

/* the bands (screen rows) and the plot inside the panel */
enum { KIT_Y_HEAD = 0, KIT_H_HEAD = 22, KIT_Y_PANEL = 24, KIT_H_PANEL = 104, KIT_Y_CELL = 130, KIT_H_CELL = 90,
       KIT_Y_FOOT = 222, KIT_H_FOOT = 18 };
enum { KIT_PX0 = 10, KIT_PW = 220, KIT_PY0 = 32, KIT_PY1 = 120 };

/* a knob cell's widget (kit_knob_t.w) */
enum { KIT_W_NONE,                              /* an empty column: a dot */
       KIT_W_VALUE,                             /* the ring lit from its start to the value */
       KIT_W_CENTRE,                            /* the ring lit from the centre (c) to the value, a tick at the top */
       KIT_W_CHOICE,                            /* the ring cut into n segments, idx lit */
       KIT_W_SWITCH,                            /* a switch, not a knob: on when v is past half */
       KIT_W_ROUTE,                             /* a link, no ring (a source or a destination) */
       KIT_W_READ };                            /* read-only: two dots and a link, the value says it */

typedef struct {
    uint8_t w, n, idx, hot, mod;                /* widget; CHOICE: n choices, idx the one; the knob in hand; mod arc */
    int16_t v, c, m0, m1;                       /* angles, 0..3840 along the sweep: value, centre, mod span */
    const char *lab;
    char val[16];
} kit_knob_t;

/* the theme in use: eight colours of the theme, three mixed from them (kit_theme) */
typedef struct {
    uint16_t bg, surf, raised, line, dim, text, theme, accent, warn, fill, grid, rel;
} kit_tokens_t;

/* what each band shows (hashes): a band is drawn only when its key changes. One struct, so the simulator's tiles can
 * keep the live screen's keys while they draw a page of their own (sim/mv_tiles.c) */
typedef struct {
    uint32_t head, panel, cell[4], foot, t_panel, row[5];
} kit_keys_t;

#define KIT_NTHEME 7u                           /* AMBER (M-vance's own, the default), then M-Vsynth2's six */
#define KIT_LIGHT 6u                            /* the light theme (PAPER): colours darken on it */

const char *kit_theme_name(uint32_t i);         /* "AMBER", "SEA" .. (i past the end: the first) */
/* the theme in use; colour: 0 the theme's own THEME colour, else this colour (COLOUR = CATEGORY). 1 when it changed:
 * the caller redraws everything (kit_full) */
int kit_theme(uint32_t theme, uint16_t colour);
void kit_full(void);                            /* the screen cleared to the theme's ground; every band drawn again */
/* the header: chip in colour chc, title, n pips (i lit; n < 2: none), tag (the part edited, 0 none) in white, and
 * a cell a voice: vs holds 2 bits a voice, voice 0 highest (2 held, 1 releasing, 0 idle), nv of them */
void kit_head(const char *chip, uint16_t chc, const char *title, uint32_t n, uint32_t i, const char *tag,
              uint32_t vs, uint32_t nv);
/* the footer: left (dim), a name in nc, right in rc */
void kit_foot(const char *left, const char *name, uint16_t nc, const char *right, uint16_t rc);
void kit_cell(uint32_t c, const kit_knob_t *k); /* knob c's cell (0..3) */
/* the panel: render(kind) draws it (in two strips) when key changed, at most every min_ms (0: at once). 1 drawn */
int kit_panel(void (*render)(uint32_t kind), uint32_t kind, uint32_t key, uint32_t min_ms);
int16_t kit_ang(int32_t v, int32_t lo, int32_t hi);   /* v's angle along the sweep, 0..3840 */
