/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance screen kit: implementation (the bands, the API and the rules in mv_kit.h). From M-Vsynth2's vs_ui.c
 * (14 Nov 2026): the themes, the drawing, the text, the bands and the knob cell, with what tied them to M-Vsynth2's
 * app (its settings, its engine, its screen state) passed in instead. Kept to vs_ui.c's names and numbers, so the
 * two compare line by line until fm1-kit takes both (fm1-port/mvance-kit-ui-plan-2026-11-14.md, decision 1a).
 *
 * Also here, for the graphs (mv_kui.c): the plot area (PX0 .. PY1), a curve's rows (yv), the waves (w_sin ..),
 * grid, plot, chip and the knob in hand's readout. Static: the unity build gives them to mv_kui.c, nobody else.
 *
 * Main loop only. Integers only: no float, no 64-bit division. */
#include "mv_kit.h"
#include "mv_kit_gen.h"                         /* fonts, knob tables, UI_SIN, UI_EXP, UI_FLT (tools/gen_kit_ui.py) */

#define PX0 KIT_PX0                             /* (vs_ui.c's short names for the plot, inside the kit's files) */
#define PW KIT_PW
#define PY0 KIT_PY0
#define PY1 KIT_PY1

/* ---- themes (decision 3a: AMBER first, so a settings record from before reads it) */
typedef struct {
    const char *name;
    uint16_t bg, surf, raised, line, dim, text, theme, accent, warn;
} kit_theme_def_t;
static const kit_theme_def_t KIT_THEMES[KIT_NTHEME] = {
    {"AMBER", RGB(14, 10, 6), RGB(26, 20, 12), RGB(44, 34, 20), RGB(74, 56, 32), RGB(168, 144, 112), RGB(245, 232, 214),
     RGB(255, 128, 0), RGB(255, 236, 200), RGB(255, 77, 61)},   /* M-vance's orange (#ff8000), the JP-8080's display */
    {"SEA", RGB(11, 15, 16), RGB(20, 26, 28), RGB(35, 44, 47), RGB(52, 66, 74), RGB(143, 160, 166), RGB(232, 240, 240),
     RGB(0, 200, 160), RGB(255, 180, 84), RGB(255, 140, 40)},
    {"EMBER", RGB(16, 13, 11), RGB(27, 22, 19), RGB(45, 37, 32), RGB(70, 58, 50), RGB(168, 152, 140), RGB(243, 234, 226),
     RGB(255, 122, 61), RGB(92, 214, 255), RGB(255, 209, 102)},
    {"ICE", RGB(10, 13, 18), RGB(19, 24, 34), RGB(33, 42, 56), RGB(51, 64, 90), RGB(142, 155, 179), RGB(230, 236, 247),
     RGB(77, 184, 255), RGB(255, 209, 102), RGB(255, 140, 40)},
    {"ORCHID", RGB(14, 11, 18), RGB(24, 20, 31), RGB(40, 34, 51), RGB(63, 54, 82), RGB(162, 151, 181), RGB(238, 232, 247),
     RGB(208, 123, 255), RGB(125, 255, 176), RGB(255, 140, 40)},
    {"CRT", RGB(6, 10, 6), RGB(13, 20, 13), RGB(24, 36, 24), RGB(39, 64, 42), RGB(127, 168, 132), RGB(216, 245, 216),
     RGB(124, 255, 107), RGB(255, 255, 255), RGB(255, 170, 40)},
    {"PAPER", RGB(242, 239, 232), RGB(231, 227, 217), RGB(251, 250, 246), RGB(189, 183, 169), RGB(92, 88, 79), RGB(26, 25, 23),
     RGB(6, 97, 79), RGB(168, 51, 10), RGB(168, 51, 10)},
};

static kit_tokens_t KT;                         /* the theme in use (vs_ui.c's T) */
static kit_keys_t KK;                           /* what each band shows */
static uint32_t kit_theme_key = ~0u;

const char *kit_theme_name(uint32_t i) { return KIT_THEMES[i < KIT_NTHEME ? i : 0u].name; }

/* a + (b - a) x t / 15, per channel (t 0..15) */
static uint16_t mix565(uint32_t a, uint32_t b, uint32_t t)
{
    const uint32_t u = 15u - t;
    const uint32_t r = (((a >> 11) * u + (b >> 11) * t) * 273u + 2048u) >> 12;
    const uint32_t g = ((((a >> 5) & 63u) * u + ((b >> 5) & 63u) * t) * 273u + 2048u) >> 12;
    const uint32_t bl = (((a & 31u) * u + (b & 31u) * t) * 273u + 2048u) >> 12;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

int kit_theme(uint32_t theme, uint16_t colour)
{
    const kit_theme_def_t *d = &KIT_THEMES[theme < KIT_NTHEME ? theme : 0u];
    const uint32_t key = (theme < KIT_NTHEME ? theme : 0u) << 16 | colour;
    if (key == kit_theme_key) return 0;
    kit_theme_key = key;
    KT.bg = d->bg; KT.surf = d->surf; KT.raised = d->raised; KT.line = d->line; KT.dim = d->dim; KT.text = d->text;
    KT.accent = d->accent; KT.warn = d->warn;
    KT.theme = colour ? colour : d->theme;
    KT.fill = mix565(KT.surf, KT.theme, 3);
    KT.grid = mix565(KT.surf, KT.line, 8);
    KT.rel = mix565(KT.surf, KT.theme, 7);
    return 1;
}

void kit_full(void)
{
    uint32_t k;
    lcd_fill(0, 0, 240, 240, KT.bg);
    KK.head = KK.panel = KK.foot = 0;
    for (k = 0; k < 4u; k++) KK.cell[k] = 0;
    for (k = 0; k < 5u; k++) KK.row[k] = 0;
}

/* ---- drawing on the canvas (gfx.c: cv_px, cv_w, cv_h, cv_oy; screen y = canvas y - cv_oy) */
static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

static void pxa(int32_t x, int32_t y, uint16_t c, uint32_t a)   /* a pixel at coverage a (0..15) */
{
    uint32_t i;
    y += cv_oy;
    if (!a || (uint32_t)x >= cv_w || (uint32_t)y >= cv_h) return;
    i = (uint32_t)y * cv_w + (uint32_t)x;
    cv_px[i] = a >= 15u ? swap16(c) : swap16(mix565(swap16(cv_px[i]), c, a));
}

static void fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { if (w > 0 && h > 0) cv_rect(x, y, w, h, c); }

/* a rounded rectangle, its corners anti-aliased (r up to h / 2: a pill) */
static void rrect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    int32_t i, j;
    if (w <= 0 || h <= 0) return;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    fill(x + r, y, w - 2 * r, h, c);
    fill(x, y + r, r, h - 2 * r, c);
    fill(x + w - r, y + r, r, h - 2 * r, c);
    for (j = 0; j < r; j++)
        for (i = 0; i < r; i++) {               /* a corner pixel: its distance (Q4) from the corner circle's centre */
            const int32_t dx = (r - i) * 16 - 8, dy = (r - j) * 16 - 8;
            const int32_t d = (int32_t)isqrt32((uint32_t)(dx * dx + dy * dy)), cv = r * 16 + 8 - d;
            const uint32_t a = cv <= 0 ? 0u : cv >= 16 ? 15u : (uint32_t)cv * 15u / 16u;
            pxa(x + i, y + j, c, a); pxa(x + w - 1 - i, y + j, c, a);
            pxa(x + i, y + h - 1 - j, c, a); pxa(x + w - 1 - i, y + h - 1 - j, c, a);
        }
}
static void rframe(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t border, uint16_t inside)
{
    rrect(x, y, w, h, r, border);
    rrect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, inside);
}

/* a disc (Q4 centre and radius) */
static void disc(int32_t cx, int32_t cy, int32_t rq, uint16_t c)
{
    int32_t x, y;
    for (y = (cy - rq) >> 4; y <= (cy + rq) >> 4; y++)
        for (x = (cx - rq) >> 4; x <= (cx + rq) >> 4; x++) {
            const int32_t dx = x * 16 + 8 - cx, dy = y * 16 + 8 - cy;
            const int32_t cv = rq + 8 - (int32_t)isqrt32((uint32_t)(dx * dx + dy * dy));
            pxa(x, y, c, cv <= 0 ? 0u : cv >= 16 ? 15u : (uint32_t)cv * 15u / 16u);
        }
}

/* a line a..b (Q4), w wide (Q4), round ends */
static void seg(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t w, uint16_t c)
{
    const int32_t dx = bx - ax, dy = by - ay, hw = w / 2;
    const int32_t L = (int32_t)isqrt32((uint32_t)(dx * dx + dy * dy));
    int32_t x, y, ux, uy, x0, x1, y0, y1;
    if (L < 2) { disc(ax, ay, hw, c); return; }
    ux = dx * 4096 / L; uy = dy * 4096 / L;
    x0 = ((ax < bx ? ax : bx) - hw - 16) >> 4; x1 = ((ax > bx ? ax : bx) + hw + 16) >> 4;
    y0 = ((ay < by ? ay : by) - hw - 16) >> 4; y1 = ((ay > by ? ay : by) + hw + 16) >> 4;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            const int32_t rx = x * 16 + 8 - ax, ry = y * 16 + 8 - ay;
            const int32_t along = (rx * ux + ry * uy) >> 12;
            int32_t d, cv;
            if (along < 0) d = (int32_t)isqrt32((uint32_t)(rx * rx + ry * ry));
            else if (along > L) { const int32_t ex = x * 16 + 8 - bx, ey = y * 16 + 8 - by; d = (int32_t)isqrt32((uint32_t)(ex * ex + ey * ey)); }
            else { d = (rx * uy - ry * ux) >> 12; if (d < 0) d = -d; }
            cv = hw + 8 - d;
            pxa(x, y, c, cv <= 0 ? 0u : cv >= 16 ? 15u : (uint32_t)cv * 15u / 16u);
        }
}

/* a column's span lo..hi (Q4 rows), its ends anti-aliased */
static void vspan(int32_t x, int32_t lo, int32_t hi, uint16_t c)
{
    int32_t r;
    if (lo < 0) lo = 0;
    for (r = lo >> 4; r * 16 < hi; r++) {
        const int32_t t = r * 16 > lo ? r * 16 : lo, b = r * 16 + 16 < hi ? r * 16 + 16 : hi;
        if (b > t) pxa(x, r, c, (uint32_t)(b - t) * 15u / 16u);
    }
}

/* a curve: yq[i] (Q4 screen rows) at column x0 + i, i < n; stroke w (Q4). dash: draw 4 columns of every 8 */
static void curve(const int16_t *yq, uint32_t n, int32_t x0, int32_t w, uint16_t c, int dash)
{
    uint32_t i;
    for (i = 0; i + 1u < n; i++) {
        const int32_t a = yq[i], b = yq[i + 1u];
        if (dash && (i & 4u)) continue;
        vspan(x0 + (int32_t)i, (a < b ? a : b) - w / 2, (a > b ? a : b) + w / 2 + 8, c);
    }
}
/* the area between a curve and a row (Q4), in c */
static void under(const int16_t *yq, uint32_t n, int32_t x0, int32_t base, uint16_t c)
{
    uint32_t i;
    for (i = 0; i + 1u < n; i++) {
        const int32_t y = (yq[i] + yq[i + 1u]) / 2 + 8;
        if (y < base) vspan(x0 + (int32_t)i, y, base, c);
        else vspan(x0 + (int32_t)i, base, y, c);
    }
}

/* text: f's coverage blended onto what is there; returns the end x. txt_track: px added to each advance (a long
 * value's -1, kit_cell) */
static int32_t txt_track;
static int32_t txt(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    for (; *s; s++) {
        const uint32_t gi = glyph(f, (uint8_t)*s), w = f->bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = f->data + f->off[gi];
        uint32_t gx, gy;
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                const uint32_t v = gd[gy * bpr + gx / 2u];
                pxa(x - f->pad + (int32_t)gx, y + (int32_t)gy, c, (gx & 1u) ? (v & 15u) : (v >> 4));
            }
        x += f->adv[gi] + txt_track;
    }
    return x;
}
/* text cut to w px (an ellipsis if it is cut), aligned 0 left, 1 centre, 2 right on x */
static void txt_fit(int32_t x, int32_t y, int32_t w, const felucca_font_t *f, const char *s, uint16_t c, int align)
{
    char b[32];
    uint32_t n = 0;
    int32_t tw = text_w(f, s);
    if (tw > w) {
        const int32_t dots = text_w(f, "..");
        tw = dots;
        while (s[n] && n < sizeof b - 3u && tw + f->adv[glyph(f, (uint8_t)s[n])] <= w) { tw += f->adv[glyph(f, (uint8_t)s[n])]; b[n] = s[n]; n++; }
        b[n++] = '.'; b[n++] = '.'; b[n] = 0;
        s = b;
    }
    txt(align == 1 ? x - tw / 2 : align == 2 ? x - tw : x, y, f, s, c);
}
/* a chip: text on RAISED, right-aligned on x (align 2) or from x (0); returns its width */
static int32_t chip(int32_t x, int32_t y, const char *s, uint16_t c, int align)
{
    const int32_t w = text_w(&FONT_LAB, s) + 8;
    if (align == 2) x -= w;
    rrect(x, y, w, 14, 3, KT.raised);
    txt(x + 4, y + 1, &FONT_LAB, s, c);
    return w;
}

/* ---- the bands */
static uint32_t strlen_u(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static uint32_t hsh(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t hstr(uint32_t h, const char *s) { while (*s) h = hsh(h, (uint8_t)*s++); return h; }

void kit_head(const char *chp, uint16_t chc, const char *title, uint32_t n, uint32_t i, const char *tag,
              uint32_t vs, uint32_t nv)
{
    uint32_t k, key = hsh(hstr(hstr(2166136261u, chp), title), n << 8 | i);
    int32_t x, lim = 186;                       /* the pips stop short of the voices (and of the tag) */
    key = hsh(hsh(hstr(key, tag ? tag : ""), vs), chc);
    if (key == KK.head) return;
    KK.head = key;
    cv_begin(240, KIT_H_HEAD, KT.surf);
    x = 5 + text_w(&FONT_LAB, chp) + 8;
    rframe(5, 4, x - 5, 14, 3, chc, KT.surf);
    txt(9, 5, &FONT_LAB, chp, chc);
    x = txt(x + 6, 2, &FONT_TTL, title, KT.text) + 8;
    if (tag) lim -= text_w(&FONT_LAB, tag) + 12;
    if (n > 1u)
        for (k = 0; k < n && x + 9 < lim; k++, x += 9) rrect(x, 10, 6, 3, 1, k == i ? KT.theme : KT.line);
    if (tag) chip(186, 4, tag, KT.text, 2);     /* the part(s) the page edits: it changes what the knobs do */
    for (k = 0; k < nv; k++) {
        const uint32_t s = vs >> (2u * (nv - 1u - k)) & 3u;
        rrect(190 + (int32_t)k * 6, 6, 4, 10, 1, s == 2u ? KT.theme : s ? KT.rel : KT.line);
    }
    cv_blit(0, KIT_Y_HEAD);
}

void kit_foot(const char *left, const char *name, uint16_t nc, const char *right, uint16_t rc)
{
    const uint32_t key = hsh(hsh(hstr(hstr(hstr(2166136261u, left), name), right), nc), rc);
    int32_t x;
    if (key == KK.foot) return;
    KK.foot = key;
    cv_begin(240, KIT_H_FOOT, KT.bg);
    fill(0, 0, 240, 1, KT.line);
    x = txt(6, 4, &FONT_LAB, left, KT.dim);
    if (*name) txt_fit(x + 6, 2, 226 - (x + 6) - text_w(&FONT_LAB, right) - 6, &FONT_VAL, name, nc, 0);
    txt(234 - text_w(&FONT_LAB, right), 4, &FONT_LAB, right, rc);
    cv_blit(0, KIT_Y_FOOT);
}

int16_t kit_ang(int32_t v, int32_t lo, int32_t hi)
{
    if (hi <= lo) return 0;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int16_t)((v - lo) * 3840 / (hi - lo));
}

/* the cell: 60 x 90, the knob's 48 x 48 at (6, 2), the label at y 54, the value at 68 */
void kit_cell(uint32_t c, const kit_knob_t *k)
{
    const uint16_t ink = k->hot ? KT.accent : KT.theme;
    const int32_t ox = 6, oy = 2;
    uint32_t key = hsh(hsh(hsh(hstr(hstr(2166136261u, k->lab), k->val), (uint32_t)k->w << 24 | (uint32_t)k->n << 16 | (uint32_t)k->idx << 8 | k->hot),
                           (uint32_t)(uint16_t)k->v << 16 | (uint16_t)k->c), (uint32_t)k->mod << 31 ^ (uint32_t)(uint16_t)k->m0 << 16 ^ (uint16_t)k->m1);
    int32_t x, y;
    if (key == KK.cell[c]) return;
    KK.cell[c] = key;
    cv_begin(60, KIT_H_CELL, KT.bg);
    if (c) fill(0, 4, 1, 78, KT.grid);
    if (k->w == KIT_W_NONE) { disc(30 * 16, 26 * 16, 32, KT.line); cv_blit(60u * c, KIT_Y_CELL); return; }
    if (k->w == KIT_W_SWITCH) {
        const int on = k->v > 1920;
        rframe(11, 16, 38, 20, 10, on ? ink : KT.line, on ? KT.fill : KT.bg);
        disc((on ? 39 : 21) * 16, 26 * 16, 6 * 16 + 8, on ? ink : KT.dim);
    } else if (k->w == KIT_W_ROUTE || k->w == KIT_W_READ) {
        disc(18 * 16, 26 * 16, 4 * 16, k->w == KIT_W_ROUTE ? KT.dim : ink);
        disc(42 * 16, 26 * 16, 4 * 16, ink);
        seg(22 * 16, 26 * 16, 38 * 16, 26 * 16, 24, KT.dim);
    } else {
        const int32_t seg_q = k->w == KIT_W_CHOICE && k->n ? 3840 / k->n : 3840;
        const int32_t lo = k->w == KIT_W_CENTRE ? (k->c < k->v ? k->c : k->v) : 0, hi = k->w == KIT_W_CENTRE ? (k->c > k->v ? k->c : k->v) : k->v;
        int32_t a16, deg, sx, sy;
        for (y = 0; y < KNOB_N; y++)
            for (x = 0; x < KNOB_N; x++) {
                const uint32_t i = (uint32_t)(y * KNOB_N + x), a = KNOB_ANG[i];
                const uint32_t rc = KNOB_RING[i] >> 4, mc = KNOB_RING[i] & 15u, cc = KNOB_CAP[i] >> 4, rim = KNOB_CAP[i] & 15u;
                if (a > 240u) {
                    if (cc) { pxa(ox + x, oy + y, KT.raised, cc); if (k->hot && rim) pxa(ox + x, oy + y, KT.accent, rim); }
                    continue;
                }
                a16 = (int32_t)a * 16 + 8;
                if (rc) {
                    uint16_t col = KT.line;
                    if (k->w == KIT_W_CHOICE) {
                        const int32_t s = a16 / seg_q, in = a16 - s * seg_q;
                        if (in >= 40 && in <= seg_q - 40) col = s == k->idx ? ink : KT.line;
                        else col = KT.bg;
                    } else {
                        /* lit lo..hi, its ends a pixel soft (the ring at r 18.5: a unit is 0.36 px) */
                        int32_t f = a16 < lo ? 16 - (lo - a16) / 3 : a16 > hi ? 16 - (a16 - hi) / 3 : 16;
                        if (k->w == KIT_W_VALUE && a16 < lo) f = 16;
                        if (hi == lo) f = 0;
                        if (f > 0) col = mix565(KT.line, ink, (uint32_t)(f > 16 ? 16 : f) * 15u / 16u);
                    }
                    if (col != KT.bg) pxa(ox + x, oy + y, col, rc);
                }
                if (mc && k->mod) {
                    const int32_t m0 = k->m0 < k->m1 ? k->m0 : k->m1, m1 = k->m0 < k->m1 ? k->m1 : k->m0;
                    if (a16 >= m0 && a16 <= m1) pxa(ox + x, oy + y, KT.accent, mc);
                }
                if (cc) { pxa(ox + x, oy + y, KT.raised, cc); if (k->hot && rim) pxa(ox + x, oy + y, KT.accent, rim); }
            }
        if (k->w == KIT_W_CENTRE) seg((ox + 24) * 16, (oy + 1) * 16 + 4, (ox + 24) * 16, (oy + 3) * 16, 20, KT.dim);
        /* the pointer: from r 4.5 to r 11 at the value's angle (-135 .. +135 degrees; UI_SIN: 256 a turn) */
        if (k->w == KIT_W_CHOICE && k->n) deg = -135 + (int32_t)(k->idx * 2u + 1u) * 135 / k->n;
        else deg = -135 + k->v * 270 / 3840;
        {
            const uint32_t ix = (uint32_t)((deg + 360) * 256 / 360) & 255u;
            sx = UI_SIN[ix]; sy = -UI_SIN[(ix + 64u) & 255u];   /* x = sin, y = -cos */
            seg((ox + 24) * 16 + sx * 72 / 16384, (oy + 24) * 16 + sy * 72 / 16384,
                (ox + 24) * 16 + sx * 176 / 16384, (oy + 24) * 16 + sy * 176 / 16384, 34, KT.text);
        }
    }
    txt_fit(30, 54, 58, &FONT_LAB, k->lab, KT.dim, 1);
    if (text_w(&FONT_VAL, k->val) <= 58) txt(30 - text_w(&FONT_VAL, k->val) / 2, 68, &FONT_VAL, k->val, k->hot ? KT.accent : KT.text);
    else {                                      /* a long value: the small face, tighter */
        const int32_t w = text_w(&FONT_LAB, k->val) - (int32_t)strlen_u(k->val);
        txt_track = -1;
        if (w <= 60) txt(30 - w / 2, 70, &FONT_LAB, k->val, k->hot ? KT.accent : KT.text);
        else txt_fit(30, 70, 60, &FONT_LAB, k->val, k->hot ? KT.accent : KT.text, 1);
        txt_track = 0;
    }
    cv_blit(60u * c, KIT_Y_CELL);
}

/* the panel, in two strips of 52 rows, when its key changes (min_ms apart, unless the screen is new) */
int kit_panel(void (*render)(uint32_t kind), uint32_t kind, uint32_t key, uint32_t min_ms)
{
    uint32_t s;
    key = hsh(key, kind);
    if (key == KK.panel || (KK.panel != 0u && fm1_ms - KK.t_panel < min_ms)) return 0;
    KK.panel = key;
    KK.t_panel = fm1_ms;
    for (s = 0; s < 2u; s++) {
        cv_begin(240, KIT_H_PANEL / 2, KT.bg);
        cv_oy = -(int32_t)(KIT_Y_PANEL + s * (KIT_H_PANEL / 2u));
        rrect(4, KIT_Y_PANEL, 232, KIT_H_PANEL, 4, KT.surf);
        render(kind);
        cv_oy = 0;
        cv_blit(0, KIT_Y_PANEL + s * (KIT_H_PANEL / 2u));
    }
    return 1;
}

/* ---- for the graphs (mv_kui.c) */
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* waves: phase Q16 (0..65535) -> Q14 */
static int32_t w_sin(uint32_t ph)
{
    const uint32_t i = ph >> 8 & 255u, f = ph & 255u;
    return (UI_SIN[i] * (int32_t)(256u - f) + UI_SIN[(i + 1u) & 255u] * (int32_t)f) >> 8;
}
static int32_t w_saw(uint32_t ph) { return 16384 - (int32_t)((ph & 65535u) >> 1); }
static int32_t w_tri(uint32_t ph) { ph &= 65535u; return ph < 32768u ? -16384 + (int32_t)ph : 16384 - (int32_t)(ph - 32768u); }
static int32_t w_pulse(uint32_t ph, uint32_t pw) { return (ph & 65535u) < pw ? 16384 : -16384; }
static int32_t w_rnd(uint32_t k) { k = k * 2654435761u; k ^= k >> 15; return (int32_t)(k & 32767u) - 16384; }

static int16_t yv[PW + 1];                      /* a curve's rows, Q4 */
static void plot(uint32_t n, uint16_t c, int fillb, int32_t base, int dash)
{
    if (fillb) under(yv, n, PX0, base, KT.fill);
    curve(yv, n, PX0, 24, c, dash);
}
static void grid(uint32_t cols, uint32_t rows)
{
    uint32_t i;
    for (i = 1; i < cols; i++) fill(PX0 + (int32_t)(PW * i / cols), PY0, 1, PY1 - PY0, KT.grid);
    for (i = 0; i <= rows; i++) fill(PX0, PY0 + (int32_t)((PY1 - PY0) * i / rows), PW, 1, KT.grid);
}
/* the knob in hand, large, at the plot's top left: its label and value */
static void readout(const char *lab, const char *val)
{
    int32_t w = text_w(&FONT_BIG, val);
    if (text_w(&FONT_LAB, lab) > w) w = text_w(&FONT_LAB, lab);
    rrect(PX0 - 2, PY0 - 2, w + 10, 44, 4, KT.surf);
    txt(PX0 + 2, PY0, &FONT_LAB, lab, KT.dim);
    txt(PX0 + 2, PY0 + 12, &FONT_BIG, val, KT.accent);
}
