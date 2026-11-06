/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance piano roll: implementation (layout, data and redraw rules in mv_roll.h). Main loop only. */
#include "mv_roll.h"

#define RL_Y0 26                                /* the rows: y 26 .. 209 */
#define RL_RH 8
#define RL_H ((int32_t)MV_ROLL_ROWS * RL_RH)    /* 184 */
#define RL_X0 26                                /* the grid: x 26 .. 234 (16 x 13, + its last line) */
#define RL_CW 13
#define RL_KX 17                                /* the keys in the strip (x 0 .. 25): from x 17 */
#define RL_FOOT_Y 220u
#define RL_STEP_TK (MV_SEQ_PPQN / 4u)           /* a column: a 1/16, 24 ticks */
#define RL_BLUE RGB(90, 170, 255)               /* LOWER under UPPER (BOTH) */
#define RL_LANE RGB(24, 24, 24)                 /* a white key's row */
#define RL_GSTEP RGB(40, 40, 40)                /* a 1/16 line */
#define RL_GBEAT RGB(96, 96, 96)                /* a beat line */
#define RL_CROW RGB(64, 64, 64)                 /* the line under a C */
#define RL_WKEY RGB(120, 120, 120)              /* the strip's keys */
static const uint8_t RL_BLACK[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};

/* what is shown now: the track(s), the bar, the column playing */
typedef struct {
    uint32_t trk, both;                         /* the selected track (MV_PART_*); BOTH: the other drawn too */
    uint32_t len[2], bar, nbar;                 /* each track's loop (ticks); the bar shown (of trk's), its bars */
    int32_t col;                                /* the 1/16 column playing (-1: stopped, or another bar) */
    uint32_t lo;                                /* the lowest row's note */
} rl_view_t;

static void note_txt(char *s, uint32_t n)       /* "C#3" (C4 = 60) */
{
    static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const char *c;
    uint32_t i = 0;
    for (c = NN[n % 12u]; *c; c++) s[i++] = *c;
    if (n < 12u) { s[i++] = '-'; s[i++] = '1'; }
    else s[i++] = (char)('0' + n / 12u - 1u);
    s[i] = 0;
}

void mv_roll_init(mv_roll_t *r)
{
    r->ver = 0; r->len[0] = r->len[1] = 0; r->valid = 0; r->nsp = 0;
    r->grid_key = r->keys_key = r->foot_key = ~0u; r->ph = -1; r->shown = 0xFF;
}

/* the spans from a copy of the tracks' events, if they changed since the last (1: made anew). The copy is read
 * through a volatile pointer and kept only if seq.ver is the same after it as before: the ISR may write in between
 * (a key recorded), never half way under it (the ISR runs to the end before the main loop goes on) */
static int roll_snap(mv_roll_t *r, const mv_engine_t *e)
{
    static mv_seq_ev_t ev[MV_SEQ_NEV];          /* (1 KB: the copy, one track at a time) */
    static uint16_t open[128];                  /* a note-on not yet ended: its tick (0xFFFF none) */
    uint32_t v = e->seq.ver, k, i, pass, ns = 0;
    uint32_t L0 = mv_seq_len(e->seqb, 0), L1 = mv_seq_len(e->seqb, 1);
    if (r->valid && v == r->ver && L0 == r->len[0] && L1 == r->len[1]) return 0;
    for (k = 0; k < 2u; k++) {
        const volatile mv_seq_ev_t *src = e->seq.t[k].ev;
        uint32_t n = e->seq.t[k].n, len = k ? L1 : L0, now = e->seq.t[k].tick;
        if (n > MV_SEQ_NEV) return 0;           /* (never: the ISR caps it) */
        for (i = 0; i < n; i++) { ev[i].tick = src[i].tick; ev[i].note = src[i].note; ev[i].vel = src[i].vel; }
        if (e->seq.ver != v) return 0;          /* written meanwhile: the next pass tries again */
        for (i = 0; i < 128u; i++) open[i] = 0xFFFFu;
        /* two passes round the loop: a note-off before its note-on in the list ends a note held over the loop's end
         * (its end then lies in the second pass: b = tick + len). Events past the loop (LENGTH shortened): not shown */
        for (pass = 0; pass < 2u; pass++)
            for (i = 0; i < n; i++) {
                uint32_t note = ev[i].note & 127u, t = ev[i].tick;
                if (t >= len) continue;
                if (ev[i].vel) {
                    if (pass) continue;         /* (the second pass only closes) */
                    if (open[note] != 0xFFFFu && ns < MV_SEQ_NEV) {   /* the same note again: the last one ends */
                        r->sp[ns].a = open[note]; r->sp[ns].b = (uint16_t)(t > open[note] ? t : open[note] + 1u);
                        r->sp[ns].note = (uint8_t)note; r->sp[ns].trk = (uint8_t)k; ns++;
                    }
                    open[note] = (uint16_t)t;
                } else if (open[note] != 0xFFFFu) {
                    uint32_t b = t + pass * len;
                    if (b <= open[note]) b = open[note] + 1u;   /* (a tick long at least) */
                    if (ns < MV_SEQ_NEV) {
                        r->sp[ns].a = open[note]; r->sp[ns].b = (uint16_t)b;
                        r->sp[ns].note = (uint8_t)note; r->sp[ns].trk = (uint8_t)k; ns++;
                    }
                    open[note] = 0xFFFFu;
                }
            }
        for (i = 0; i < 128u; i++)              /* a key still held while recording: up to the playhead */
            if (open[i] != 0xFFFFu && ns < MV_SEQ_NEV) {
                uint32_t b = now >= open[i] ? now : now + len;
                r->sp[ns].a = open[i]; r->sp[ns].b = (uint16_t)(b > open[i] ? b : open[i] + 1u);
                r->sp[ns].note = (uint8_t)i; r->sp[ns].trk = (uint8_t)k; ns++;
            }
    }
    r->nsp = ns; r->ver = v; r->len[0] = (uint16_t)L0; r->len[1] = (uint16_t)L1; r->valid = 1;
    return 1;
}

static void roll_view(const mv_ui_t *u, const mv_engine_t *e, rl_view_t *w)
{
    const mv_seq_track_t *T;
    w->trk = mv_ui_track(u, e);
    w->both = e->perf[MV_PF_MODE] != MV_KEY_SINGLE && u->panel == MV_UI_BOTH;
    w->len[0] = mv_seq_len(e->seqb, 0); w->len[1] = mv_seq_len(e->seqb, 1);
    w->nbar = w->len[w->trk] / MV_SEQ_BAR;
    T = &e->seq.t[w->trk];
    if (e->seq_live && T->len) {                /* running: the bar the track plays, and its column */
        uint32_t len = T->len, cur = (T->tick + len - 1u) % len;   /* (tick: the next to play) */
        w->bar = cur / MV_SEQ_BAR;
        w->col = (int32_t)((cur % MV_SEQ_BAR) / RL_STEP_TK);
        if (w->bar >= w->nbar) w->bar = 0;      /* (LENGTH just shortened) */
    } else {
        w->bar = u->roll_bar < w->nbar ? u->roll_bar : w->nbar - 1u;
        w->col = -1;
    }
    w->lo = u->roll_lo;
}

/* the rows fitted to the notes the page shows (all of the track's, not only this bar's: steady as the bar moves) */
static void roll_fit(mv_roll_t *r, mv_ui_t *u, const rl_view_t *w)
{
    uint32_t i, mn = 127, mx = 0, any = 0;
    int32_t lo;
    for (i = 0; i < r->nsp; i++)
        if (r->sp[i].trk == w->trk || w->both) {
            if (r->sp[i].note < mn) mn = r->sp[i].note;
            if (r->sp[i].note > mx) mx = r->sp[i].note;
            any = 1;
        }
    if (!any) lo = 48;                          /* nothing yet: C3 .. A#4 */
    else if (mx - mn >= MV_ROLL_ROWS) lo = (int32_t)mn;   /* wider than the rows: from the lowest */
    else lo = (int32_t)(mn + mx) / 2 - (int32_t)MV_ROLL_ROWS / 2;
    if (lo < 0) lo = 0;
    if (lo > 128 - (int32_t)MV_ROLL_ROWS) lo = 128 - (int32_t)MV_ROLL_ROWS;
    u->roll_lo = (uint8_t)lo;
    u->roll_fit = 0;
}

/* a note's piece in the bar shown, as screen x (it may stretch outside: clipped by the strip) */
static void span_piece(const rl_view_t *w, const mv_roll_span_t *s, uint32_t a, uint32_t b, int32_t xs, uint32_t row,
                       uint16_t c)
{
    uint32_t b0 = (w->bar % (w->len[s->trk] / MV_SEQ_BAR)) * MV_SEQ_BAR;   /* the bar's first tick in its loop */
    int32_t x0, x1, y;
    if (b <= b0 || a >= b0 + MV_SEQ_BAR) return;   /* not in this bar */
    x0 = RL_X0 + (int32_t)(((int32_t)a - (int32_t)b0) * RL_CW / (int32_t)RL_STEP_TK);
    x1 = RL_X0 + (int32_t)(((int32_t)b - (int32_t)b0) * RL_CW / (int32_t)RL_STEP_TK);
    if (x1 - x0 < 2) x1 = x0 + 2;
    if (x0 < RL_X0) x0 = RL_X0;
    if (x1 > RL_X0 + 16 * RL_CW) x1 = RL_X0 + 16 * RL_CW;
    y = (int32_t)row * RL_RH;
    cv_rect(x0 + 1 - xs, y + 1, x1 - x0 - 1, RL_RH - 2, c);   /* (in the strip: xs its left edge on screen) */
}

/* the grid's columns from x (screen) for w px: lanes, lines, notes, the playhead */
static void roll_strip(const mv_roll_t *r, const rl_view_t *w, int32_t x, int32_t wd)
{
    uint32_t i, rw, k;
    cv_begin((uint32_t)wd, (uint32_t)RL_H, C_BLACK);
    for (rw = 0; rw < MV_ROLL_ROWS; rw++) {     /* the lanes: white keys shaded, a line under each C */
        uint32_t n = w->lo + MV_ROLL_ROWS - 1u - rw;
        int32_t y = (int32_t)rw * RL_RH;
        if (!RL_BLACK[n % 12u]) cv_rect(0, y, wd, RL_RH, RL_LANE);
        if (n % 12u == 0u) cv_rect(0, y + RL_RH - 1, wd, 1, RL_CROW);
    }
    for (i = 0; i <= 16u; i++) {                /* the 1/16 lines, the beats brighter */
        int32_t gx = RL_X0 + (int32_t)i * RL_CW;
        if (gx >= x && gx < x + wd) cv_rect(gx - x, 0, 1, RL_H, i % 4u ? RL_GSTEP : RL_GBEAT);
    }
    for (k = 0; k < 2u; k++) {                  /* the other track (BOTH) first, under; then the selected one */
        uint32_t trk = k ? w->trk : w->trk ^ 1u;
        if (!k && !w->both) continue;
        for (i = 0; i < r->nsp; i++) {
            const mv_roll_span_t *s = &r->sp[i];
            uint32_t len = w->len[trk], row;
            uint16_t c = k ? C_ACC : RL_BLUE;
            if (s->trk != trk) continue;
            if (s->note < w->lo) row = MV_ROLL_ROWS - 1u;   /* below the rows: a mark on the bottom edge */
            else if (s->note >= w->lo + MV_ROLL_ROWS) row = 0;   /* above: on the top edge */
            else row = w->lo + MV_ROLL_ROWS - 1u - s->note;
            if (s->note < w->lo || s->note >= w->lo + MV_ROLL_ROWS) {
                uint32_t b0 = (w->bar % (len / MV_SEQ_BAR)) * MV_SEQ_BAR, a = s->a, b = s->b > len ? len : s->b;
                if (b > b0 && a < b0 + MV_SEQ_BAR) {
                    int32_t x0 = RL_X0 + (int32_t)(((int32_t)(a > b0 ? a : b0) - (int32_t)b0) * RL_CW / (int32_t)RL_STEP_TK);
                    cv_rect(x0 - x, row ? RL_H - 1 : 0, 4, 1, c);
                }
                continue;
            }
            if (s->b <= len) span_piece(w, s, s->a, s->b, x, row, c);
            else {                              /* held over the loop's end: its two pieces */
                span_piece(w, s, s->a, len, x, row, c);
                span_piece(w, s, 0, s->b - len, x, row, c);
            }
        }
    }
    if (w->col >= 0) {                          /* the playhead: a white line mid-column */
        int32_t px = RL_X0 + w->col * RL_CW + 6;
        if (px >= x && px < x + wd) cv_rect(px - x, 0, 1, RL_H, C_WHITE);
    }
    cv_blit((uint32_t)x, (uint32_t)RL_Y0);
}

/* the 1/16 column c (0..15; the last one with the grid's closing line) */
static void roll_col(const mv_roll_t *r, const rl_view_t *w, int32_t c)
{
    roll_strip(r, w, RL_X0 + c * RL_CW, c == 15 ? RL_CW + 1 : RL_CW);
}

/* the key strip: the C names, the keys (lit: sounding) */
static void roll_keys(const rl_view_t *w, uint32_t lit)
{
    uint32_t rw;
    char nb[8];
    cv_begin((uint32_t)RL_X0, (uint32_t)RL_H, C_BLACK);
    for (rw = 0; rw < MV_ROLL_ROWS; rw++) {
        uint32_t n = w->lo + MV_ROLL_ROWS - 1u - rw, blk = RL_BLACK[n % 12u];
        int32_t y = (int32_t)rw * RL_RH;
        if (n % 12u == 0u) { note_txt(nb, n); cv_text(0, y - 5, &FONT_S, nb, C_GRAY); }   /* (on its row's line) */
        cv_rect(RL_KX, y, blk ? 5 : 8, RL_RH - 1, lit >> rw & 1u ? C_ACC : blk ? C_DIM : RL_WKEY);
    }
    cv_blit(0, (uint32_t)RL_Y0);
}

void mv_roll_draw(mv_roll_t *r, mv_ui_t *u, const mv_engine_t *e, int all)
{
    rl_view_t w;
    uint32_t key, rw, lit = 0;
    int32_t c;
    if (all) { r->grid_key = r->keys_key = r->foot_key = ~0u; r->ph = -1; }
    (void)roll_snap(r, e);
    roll_view(u, e, &w);
    if ((w.trk | w.both << 1) != r->shown) { r->shown = (uint8_t)(w.trk | w.both << 1); u->roll_fit = 1; }   /* another
                                                   track (PART SELECT): its rows fitted */
    if (u->roll_fit && r->valid) { roll_fit(r, u, &w); w.lo = u->roll_lo; }
    /* the grid: anew when the notes, the bar, the rows or the mode moved; else the playhead's two columns */
    key = ((r->ver * 31u + w.bar) * 31u + w.lo) * 31u + (w.trk | w.both << 1) + (w.len[0] << 4) + (w.len[1] << 9);
    if (key != r->grid_key) {
        r->grid_key = key;
        for (c = 0; c < 16; c++) roll_col(r, &w, c);
        r->ph = w.col;
    } else if (w.col != r->ph) {
        int32_t was = r->ph;
        r->ph = w.col;
        if (was >= 0) roll_col(r, &w, was);
        if (w.col >= 0) roll_col(r, &w, w.col);
    }
    for (rw = 0; rw < MV_ROLL_ROWS; rw++)       /* the keys sounding (either part) */
        if (mv_engine_sounding(e, w.lo + MV_ROLL_ROWS - 1u - rw)) lit |= 1u << rw;
    if ((key = lit * 131u + w.lo) != r->keys_key) { r->keys_key = key; roll_keys(&w, lit); }
    {   /* the foot line: BAR n/m  ROOT F3  +5  TR L (28 characters at most: 240 px of FONT_S) */
        static const char *const TR[MV_SQX_N] = {"OFF", "L", "U", "L+U"};
        const mv_seq_track_t *T = &e->seq.t[w.trk];
        char s[40], nb[8];
        const char *a;
        uint32_t i = 0, x = e->seqb[MV_SQ_XPOSE] < MV_SQX_N ? e->seqb[MV_SQ_XPOSE] : 0u, root = T->root;
        int32_t off = T->xpose, m;
        for (a = "BAR "; *a; a++) s[i++] = *a;
        if (w.bar + 1u >= 10u) s[i++] = '1';
        s[i++] = (char)('0' + (w.bar + 1u) % 10u); s[i++] = '/'; s[i++] = (char)('0' + w.nbar % 10u);
        for (a = "  ROOT "; *a; a++) s[i++] = *a;
        if (root == MV_SEQ_NOROOT) s[i++] = '-';
        else { note_txt(nb, root); for (a = nb; *a; a++) s[i++] = *a; }
        s[i++] = ' '; s[i++] = ' ';
        s[i++] = off < 0 ? '-' : '+';
        m = off < 0 ? -off : off;
        if (m >= 10) s[i++] = (char)('0' + m / 10);
        s[i++] = (char)('0' + m % 10);
        for (a = "  TR "; *a; a++) s[i++] = *a;
        for (a = TR[x]; *a; a++) s[i++] = *a;
        s[i] = 0;                               /* (at most 4+3+7+3+2+3+5+3 = 30, as "BAR 1/8  ROOT C#-1  -24  TR
                                                   L+U": in practice 27) */
        key = 2166136261u;
        for (a = s; *a; a++) key = (key ^ (uint8_t)*a) * 16777619u;
        if (key != r->foot_key) { r->foot_key = key; draw_text_box(0, RL_FOOT_Y, 240, &FONT_S, s, C_AMB, 1); }
    }
}
