/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance DEBUG page (Kris 3 Nov: the footer's grey meter and status lines off every screen, onto a page of their
 * own on GLO; the space they took goes to the pages' columns and the home scope).
 *
 *   reached    GLO, after GLOBAL (and ALGORITHM, like every page). The knobs do nothing here
 *   y  0- 23   the header (mv_ui.c: "DEBUG", GLO)
 *   y 30-      a row per line, the label grey on the left, the value bright from x 112:
 *              CPU       average render time of a 5.8 ms buffer over the last 1/4 s, us and %
 *              PEAK 3S   the worst buffer over the last 3 s, % (orange from 90 %: a click is near)
 *              VOICES    sounding / the VOICES limit
 *              OCTAVE    the keys' octave (also on the OCT LEDs)
 *              MIDI CH   the channel (OMNI)
 *              NOTES IN  where notes came from last: USB / TRS (- none yet)
 *              CLOCK     an incoming MIDI clock's tempo: "120 BPM" running, "120 stop" stopped, "-" none
 *              PROFILE   MV_PROFILE builds only: OSC / FLT / ETC share of the buffer (else not drawn)
 *              FIRMWARE  APP_VER
 *
 * Main loop only (never the ISR). Each row is keyed: redrawn only when its text or colour changes, so a page left
 * open costs a hash a row 4 times a second. #included into mvance.c after the colours and BUF_US (the unity build:
 * it draws with gfx.c). */
#pragma once
#include <stdint.h>

/* what the page shows: filled by mvance.c (it owns the ISR's timing and the MIDI state) */
typedef struct {
    uint32_t avg_us;                            /* average render time of a buffer (us) */
    uint32_t avg_pc, peak_pc;                   /* the average, and the 3 s peak, in % of the buffer */
    uint32_t act, nv;                           /* voices sounding, the VOICES limit */
    int32_t oct;                                /* -2..+2 */
    uint32_t ch;                                /* 0 = OMNI, 1..16 */
    uint32_t src;                               /* notes came from: 0 none yet, 1 USB, 2 TRS */
    uint32_t bpm, running;                      /* an incoming clock's tempo (0 = none), and START / STOP */
    int prof;                                   /* 1: the PROFILE row (MV_PROFILE builds) */
    uint32_t osc_pc, flt_pc, etc_pc;            /* its shares, % of the buffer */
} mv_debug_t;

/* the rows that changed since the last call; all: every row (the page was just entered: the screen is clear) */
void mv_debug_draw(const mv_debug_t *d, int all);
