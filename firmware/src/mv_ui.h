/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance panel: the JP-8080's sections as pages on the FM-1 (Kris, 1 Nov 2026: fm1-port/mvance-panel-layout-
 * proposal-2026-11-01.md, decisions 1-7).
 *
 *   pages     the JE-8086's regions, in the JP-8080's panel order: LFO (3; the third: LFO1 SYNC of the part edited,
 *             Kris 5 Nov), OSC 1, OSC 2 (2), OSC COMMON, PITCH ENV,
 *             FILTER (2), FILTER ENV, AMP, AMP ENV, TONE, MULTI-FX, DELAY, REVERB (2: LEVEL SIZE HI DAMP LO DAMP /
 *             LOWPASS HIPASS FREEZE; global settings, not the patch's, no CC), PLAY MODE (2), GLOBAL (MIDI CH,
 *             VOICES, TEMPO) and ARP (2: global settings)
 *             (MIDI channel, voice limit: not in the patch). KNOB1..4 edit the page's 4 parameters, left to right;
 *             a spare 4th knob stays empty (decision 3)
 *   buttons   a section button steps through its pages, looping (Kris's idea): LFO, EDIT (OSC 1 > OSC 2 > OSC 2 >
 *             OSC COMMON), ENV (PITCH > FILTER > AMP), SEL (FILTER > FILTER > AMP), FX (TONE > MULTI-FX > DELAY >
 *             REVERB > REVERB: the JP's effects section and the M-vance reverb; AMP moved to SEL, Kris 1 Nov 4a;
 *             REVERB 2 Nov R4a), GLO (PLAY MODE > GLOBAL); ARP: a tap the arp on / off (lit when on), a hold its
 *             pages (ARP 1/2 MODE RATE OCTAVE GATE, 2/2 HOLD SWING CHANCE ORDER: Skiff's arp, mv_arp.h);
 *             SAVE (Kris 2 Nov: rename, reorder, edit the programs): its page: TO (the slot), CURSOR and CHAR (the
 *             name, patch bytes 0..15), MODE WRITE (the patch as edited into TO: also a copy to another slot) /
 *             SWAP (the program playing and TO trade places: the reorder) / FACTORY (TO back to the factory set's);
 *             SAVE again does it (the caller: mv_prog), any other button leaves;
 *             HOME: the home screen. ALGORITHM walks every page in panel order. SELECT: single steps on the last
 *             parameter touched (decision 6)
 *   screen    the family's look (the platform's fonts and palette roles: labels grey, values bright, rules dim,
 *             the knob being turned white): a header, 4 columns of 60 px (label, value, a bar for numbers; just
 *             the value for choices, decision 4), a line with the last edit
 *   MIDI out  every edit that has a JP MODE2 CC goes out on it (decision 5), so a DAW records the panel
 *   PERFORM   (GLO, after PLAY MODE; Kris 3 Nov, layer / split) 1/2 MODE (SINGLE / DUAL / SPLIT), SPLIT (a note;
 *             touch it, then press a key: that key), BALANCE, PANEL (the part the pages edit: UPPER / LOWER /
 *             BOTH; the header shows U / L / L+U); 2/2 VOICES (LOWER + UPPER, 1+7 .. 7+1), ARP (the part(s) the
 *             arp plays: AUTO / UPPER / LOWER / BOTH), CH LO, CH UP (GLOBAL or 1..16). TONE, MULTI-FX and
 *             DELAY are the one chain: they always edit UPPER's bytes
 *
 * Main loop only (never the ISR). The patch bytes it writes are read by the engine live. It is #included into
 * mvance.c (the unity build): it draws with the platform's gfx.c and sends with platform.c's midi_send. */
#pragma once
#include <stdint.h>
#include "mv_voice.h"
#include "mv_prog.h"

typedef struct {
    uint8_t page;                               /* 0 = HOME, else an index into the page table */
    uint8_t last;                               /* the column last touched (0..3), 0xFF none */
    uint8_t redraw;                             /* the whole page (header and columns) on the next draw */
    uint32_t t_touch;                           /* fm1_ms of the last edit (the edit line shows it for 3 s) */
    uint32_t t_knob[4];                         /* fm1_ms of each knob's last detent (acceleration) */
    uint32_t drawn[4];                          /* what each column shows (key of label, value, selection) */
    uint32_t drawn_line;                        /* what the edit line shows */
    uint32_t arp_t;                             /* fm1_ms the ARP button went down */
    uint8_t arp_down, arp_did;                  /* it is down; its hold has acted (opened the pages) */
    /* the programs (mv_prog): the bank (names), the program playing, and the SAVE page */
    const mv_prog_t *prog;
    uint8_t cur;                                /* the program playing (0..63; the caller keeps it) */
    uint8_t sv_to, sv_pos, sv_mode;             /* SAVE: the target slot, the name's cursor (0..15), WRITE/SWAP/FACTORY */
    uint8_t act, act_to;                        /* a SAVE asked for (MV_SV_*, 0 none) and its slot: the caller does it */
    uint32_t drawn_sv;                          /* what the SAVE page's two lines show */
    uint8_t panel;                              /* PANEL (PERFORM, Kris 3 Nov): MV_UI_UPPER / LOWER / BOTH, the part(s)
                                                   the pages and the CCs in edit in a layered program */
    uint32_t drawn_tag;                         /* the header's part tag drawn (U / L / L+U) */
    uint8_t learned;                            /* SPLIT learn: a key was taken since SPLIT was last touched */
} mv_ui_t;
enum { MV_SV_NONE, MV_SV_WRITE, MV_SV_SWAP, MV_SV_FACTORY };
enum { MV_UI_UPPER, MV_UI_LOWER, MV_UI_BOTH };   /* PANEL */

void mv_ui_init(mv_ui_t *u, const mv_prog_t *prog);
/* what mv_ui_input reports (bits) */
#define MV_UI_PAGE 1u                           /* the page changed (to HOME: the caller redraws its screen) */
#define MV_UI_SETTING 2u                        /* a setting kept in flash changed (GLOBAL: the MIDI channel) */
#define MV_UI_PROG 4u                           /* SAVE pressed on its page: do u->act on slot u->act_to */
/* buttons pressed / released since the last call (bit = BTN_...), the encoders read here; MIDI channel (0 = omni)
 * by pointer (GLOBAL edits it), out_ch for the CCs. Returns MV_UI_... bits. ARP (Kris 1 Nov, 7a): a tap switches
 * the arp on / off, a hold (0.5 s) opens its pages (held again: the next one) */
uint32_t mv_ui_input(mv_ui_t *u, mv_engine_t *e, uint32_t pressed, uint32_t released, uint8_t *midi_ch,
                     uint32_t out_ch);
static inline int mv_ui_home(const mv_ui_t *u) { return u->page == 0u; }
uint32_t mv_ui_button(const mv_ui_t *u);        /* the button of the page shown (BTN_HOME on HOME): its LED lit */
int mv_ui_debug(const mv_ui_t *u);              /* 1: the DEBUG page shows (GLO): mvance.c draws its rows */
void mv_ui_draw(mv_ui_t *u, const mv_engine_t *e, uint32_t midi_ch);   /* a page (not HOME): what changed */
/* SPLIT learn: PERFORM's SPLIT touched in the last 3 s, so the next key pressed sets the split (mvance.c: it does
 * not play); mv_ui_learned after it: the cell stays lit 3 s, and the keys play again */
int mv_ui_split_learn(const mv_ui_t *u);
void mv_ui_learned(mv_ui_t *u);
