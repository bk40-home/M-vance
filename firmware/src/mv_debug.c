/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance DEBUG page: implementation (what it shows and why in mv_debug.h). */
#include "mv_debug.h"

#define DBG_ROWS 9u
#define DBG_Y0 30                               /* the first row (below the header, 24 px) */
#define DBG_DY 22                               /* a row: FONT_S is 16 high, 6 px between */
#define DBG_VX 112                              /* the values' column */

/* ---- small text helpers (no printf on the device) */
static char *dbg_str(char *p, const char *a) { while (*a) *p++ = *a++; return p; }
static char *dbg_u(char *p, uint32_t v)         /* unsigned decimal, no leading zeros */
{
    char t[10];
    uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10u);
    while (n) *p++ = t[--n];
    return p;
}
static uint32_t dbg_key(const char *s, uint16_t c)   /* FNV-1a of the text and the colour */
{
    uint32_t h = 2166136261u ^ c;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

/* one row: the label grey, the value in colour c; drawn only when it changed */
static void dbg_row(uint32_t i, const char *label, const char *val, uint16_t c, uint32_t *drawn)
{
    uint32_t k = dbg_key(val, c);
    if (k == drawn[i]) return;
    drawn[i] = k;
    cv_begin(240, 16, C_BLACK);
    cv_text(8, 0, &FONT_S, label, C_GRAY);
    cv_text(DBG_VX, 0, &FONT_S, val, c);
    cv_blit(0, (uint32_t)(DBG_Y0 + (int32_t)i * DBG_DY));
}

void mv_debug_draw(const mv_debug_t *d, int all)
{
    static uint32_t drawn[DBG_ROWS];
    char s[32], *p;
    uint32_t i;
    if (all) for (i = 0; i < DBG_ROWS; i++) drawn[i] = 0;   /* (0: no row's text hashes to it in practice) */

    p = dbg_u(s, d->avg_us); p = dbg_str(p, " us  "); p = dbg_u(p, d->avg_pc); p = dbg_str(p, " %"); *p = 0;
    dbg_row(0, "CPU", s, C_HI, drawn);
    p = dbg_u(s, d->peak_pc); p = dbg_str(p, " %"); *p = 0;
    dbg_row(1, "PEAK 3S", s, d->peak_pc >= 90u ? C_ACC : C_HI, drawn);   /* orange from 90 %: a click is near */
    p = dbg_u(s, d->act); p = dbg_str(p, " / "); p = dbg_u(p, d->nv); *p = 0;
    dbg_row(2, "VOICES", s, C_HI, drawn);
    p = s; *p++ = d->oct < 0 ? '-' : '+'; p = dbg_u(p, (uint32_t)(d->oct < 0 ? -d->oct : d->oct)); *p = 0;
    dbg_row(3, "OCTAVE", s, C_HI, drawn);
    if (d->ch) { p = dbg_u(s, d->ch); *p = 0; } else dbg_str(s, "OMNI")[0] = 0;
    dbg_row(4, "MIDI CH", s, C_HI, drawn);
    dbg_str(s, d->src == 2u ? "TRS" : d->src == 1u ? "USB" : "-")[0] = 0;
    dbg_row(5, "NOTES IN", s, C_HI, drawn);
    if (d->bpm) { p = dbg_u(s, d->bpm); p = dbg_str(p, d->running ? " BPM" : " stop"); *p = 0; }
    else dbg_str(s, "-")[0] = 0;
    dbg_row(6, "CLOCK", s, C_HI, drawn);
    if (d->prof) {                              /* MV_PROFILE builds: where the time goes */
        p = dbg_str(s, "O"); p = dbg_u(p, d->osc_pc); p = dbg_str(p, " F"); p = dbg_u(p, d->flt_pc);
        p = dbg_str(p, " E"); p = dbg_u(p, d->etc_pc); p = dbg_str(p, " %"); *p = 0;
        dbg_row(7, "PROFILE", s, C_ACC, drawn);
    }
    dbg_row(8, "FIRMWARE", APP_VER, C_DIM, drawn);
}
