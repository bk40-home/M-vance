/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance home screen: implementation (layout and timing in mv_home.h). */
#include "mv_home.h"

/* ---- the header: the saw fan and the plate (the same numbers as tools/gen_logo.py) */
#define FAN_P0 64                               /* the period, px */
#define FAN_DET 30                              /* the detune a strand, 1/1000 of the period */
#define FAN_VOFF 3                              /* each strand this much lower the further from the centre one */
#define FAN_Y0 4
#define FAN_AMP 30

void home_header(void)
{
    static const uint16_t *const COL[4] = {&C_HI, &C_AMB, &C_GRAY, &C_DIM};   /* by distance from the centre saw */
    int32_t d, s, k, x, px = 0, py = 0;
    int32_t tw = text_w(&FONT_L, APP_NAME), pw = tw + 20, plx = (240 - pw) / 2;
    cv_begin(240, 48, C_BLACK);
    for (d = 3; d >= 0; d--)                    /* dimmest first: the centre saw lands on top */
        for (s = d ? -1 : 1; s <= 1; s += 2) {
            k = 3 + s * d;
            for (x = 0; x < 240; x++) {
                int32_t u = (x - 120) * (1000 + (k - 3) * FAN_DET);           /* +-131000: int32 */
                int32_t f = (u * 256) / (1000 * FAN_P0) + 128;               /* (once a HOME: the divides are fine) */
                int32_t y;
                f = ((f % 256) + 256) % 256;                                 /* 0..255 through the period */
                y = FAN_Y0 + d * FAN_VOFF + FAN_AMP - ((FAN_AMP * f) >> 8);
                if (x) {
                    if (y > py + FAN_AMP / 2) cv_line(x, py, x, y, *COL[d]);   /* the drop: straight down */
                    else cv_line(px, py, x, y, *COL[d]);
                }
                px = x; py = y;
            }
        }
    cv_rect(plx, 0, pw, 48, C_BLACK);           /* the plate over the middle, its two orange rules, the name */
    cv_rect(plx, 7, pw, 1, C_ACC);
    cv_rect(plx, 41, pw, 1, C_ACC);
    cv_text(plx + 10, 9, &FONT_L, APP_NAME, C_ACC);
    cv_blit(0, 0);
}

/* ---- the program: PROGRAM nn, EDITED, the category chip, the name big */

/* the category from the name's first two letters (tools/bank/factory.txt's prefixes); 0 when there is none */
static const char *category(const uint8_t *nm)
{
    static const char *const CODE[11] = {"LD", "PD", "BS", "BA", "PL", "AR", "BR", "ST", "BL", "FX", "SQ"};
    static const char *const NAME[11] = {"LEAD", "PAD", "BASS", "BASS", "PLUCK", "ARP", "BRASS", "STRINGS", "BELL", "FX",
                                         "SEQ"};
    uint32_t i;
    if (nm[2] != ' ') return 0;                 /* (the prefix and a space: "LD Saw Crown") */
    for (i = 0; i < 11u; i++)
        if (nm[0] == (uint8_t)CODE[i][0] && nm[1] == (uint8_t)CODE[i][1]) return NAME[i];
    return 0;
}

/* a name into s without its category prefix, trailing spaces gone */
static void name_of(const uint8_t *name, char *s)
{
    uint32_t i, j = 0, from = category(name) ? 3u : 0u;
    for (i = from; i < MV_PROG_NAME; i++) s[j++] = (char)(name[i] >= 32u && name[i] <= 126u ? name[i] : '?');
    while (j && s[j - 1u] == ' ') j--;          /* (the JP name's trailing spaces) */
    s[j] = 0;
}

/* "LOCAL OFF" (x 96) fits left of the chip? A SPLIT chip (SPLIT C#4) is the widest and reaches left past it; the
 * category chips and DUAL do not. Short form otherwise */
static int cat_fits_local(const uint8_t *name, uint32_t mode)
{
    const char *c = mode == MV_KEY_DUAL ? "DUAL" : category(name);
    if (mode == MV_KEY_SPLIT) return 0;
    return !c || 96 + text_w(&FONT_S, "LOCAL OFF") + 4 <= 232 - (text_w(&FONT_S, c) + 10);
}

void home_program(uint32_t n, const uint8_t *name, int edited, uint32_t mode, const uint8_t *lower, uint32_t split,
                  int local_off)
{
    static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char num[3], s[MV_PROG_NAME + 4], chip[12];
    const char *cat = category(name);
    const felucca_font_t *f = &FONT_L;
    cv_begin(240, 52, C_BLACK);
    num[0] = (char)('0' + (n + 1u) / 10u); num[1] = (char)('0' + (n + 1u) % 10u); num[2] = 0;
    cv_text(8, 0, &FONT_S, "PROGRAM", C_GRAY);
    cv_text(72, 0, &FONT_S, num, C_HI);
    if (local_off) cv_text(96, 0, &FONT_S, cat_fits_local(name, mode) ? "LOCAL OFF" : "L.OFF", C_ACC);
    else if (edited) cv_text(96, 0, &FONT_S, "EDITED", C_ACC);
    if (mode == MV_KEY_DUAL) cat = "DUAL";      /* a layered program: its mode in the chip (SPLIT with its key) */
    else if (mode == MV_KEY_SPLIT) {
        uint32_t i = 0;
        const char *c;
        for (c = "SPLIT "; *c; c++) chip[i++] = *c;
        for (c = NN[split % 12u]; *c; c++) chip[i++] = *c;
        chip[i++] = split < 12u ? '-' : (char)('0' + split / 12u - 1u);
        if (split < 12u) chip[i++] = '1';
        chip[i] = 0;
        cat = chip;
    }
    if (cat) {                                  /* the chip: an outline (text on black: no fringe) */
        int32_t w = text_w(&FONT_S, cat) + 10, x = 232 - w;
        cv_rect(x, 0, w, 1, C_AMB); cv_rect(x, 15, w, 1, C_AMB); cv_rect(x, 0, 1, 16, C_AMB); cv_rect(x + w - 1, 0, 1, 16, C_AMB);
        cv_text(x + 5, 0, &FONT_S, cat, C_HI);
    }
    if (mode == MV_KEY_SINGLE) {                /* the name big */
        name_of(name, s);
        if (text_w(f, s) > 236) f = &FONT_S;    /* 15 large letters fit; a 16-letter name without a prefix does not */
        cv_text((240 - text_w(f, s)) / 2, f == &FONT_L ? 18 : 26, f, s, edited ? C_ACC : C_KEY);
    } else {                                    /* layered: UPPER and LOWER, a line each (U, L in the margin) */
        cv_text(8, 18, &FONT_S, "U", C_AMB);
        name_of(name, s); cv_text(24, 18, &FONT_S, s, edited ? C_ACC : C_KEY);
        cv_text(8, 34, &FONT_S, "L", C_AMB);
        name_of(lower, s); cv_text(24, 34, &FONT_S, s, edited ? C_ACC : C_KEY);
    }
    cv_blit(0, 48);
}

/* ---- the home knobs: PAGES[0] (mv_ui.c) says what each edits; col_value gives the text and the bar */
/* (the labels: HOME_LABEL, next to PAGES in mv_ui.c, so the cheat sheet's dump has them too) */

uint32_t home_knobs_key(const mv_engine_t *e, int touched)
{
    uint32_t k, h = 2166136261u, mode;
    int32_t fill;
    char v[8];
    for (k = 0; k < 4u; k++) {
        col_value(e, PAGES[0].p[k], 0, v, &mode, &fill);
        h = (h ^ (uint32_t)fill) * 16777619u;
    }
    return h ^ (uint32_t)(touched + 1) << 28;
}

void home_knobs(const mv_engine_t *e, int touched)
{
    uint32_t k, mode;
    int32_t fill;
    char v[8];
    cv_begin(240, 28, C_BLACK);
    for (k = 0; k < 4u; k++) {
        int32_t x = (int32_t)k * 60;
        int sel = touched == (int)k;
        col_value(e, PAGES[0].p[k], 0, v, &mode, &fill);
        if (k) cv_rect(x, 2, 1, 24, C_LINE);    /* the rule between the cells */
        cv_text(x + 4, 0, &FONT_S, HOME_LABEL[k], sel ? C_WHITE : C_GRAY);
        cv_text(x + 56 - text_w(&FONT_S, v), 12, &FONT_S, v, sel ? C_WHITE : C_HI);
        cv_rect(x + 4, 25, 52, 2, C_LINE);      /* the bar: 52 px for 0..1000 */
        if (fill > 0) cv_rect(x + 4, 25, 52 * fill / 1000, 2, sel ? C_ACC : C_AMB);
    }
    cv_blit(0, 102);
}

/* ---- the voices: a bar each, the amp envelope (Q30) in 8 steps */
static uint32_t voice_step(const mv_voice_t *v)
{
    int32_t l = v->aenv.level;
    if (!mv_env_active(&v->aenv) || l <= 0) return 0;
    l >>= 27;                                   /* Q30 full = 8 */
    return l < 1 ? 1u : l > 8 ? 8u : (uint32_t)l;   /* sounding: at least a sliver */
}

uint32_t home_voices_key(const mv_engine_t *e)
{
    uint32_t k, h = e->nv;
    for (k = 0; k < MV_NV; k++) h = h << 4 ^ voice_step(&e->v[k]) ^ h >> 28;
    return h;
}

void home_voices(const mv_engine_t *e)
{
    uint32_t k;
    cv_begin(240, 12, C_BLACK);
    for (k = 0; k < MV_NV; k++) {
        int32_t x = 8 + (int32_t)k * 28, l = (int32_t)voice_step(&e->v[k]);
        cv_rect(x, 10, 24, 2, k < e->nv ? C_DIM : C_LINE);   /* the floor: dim above the VOICES limit */
        if (l && k < e->nv) { cv_rect(x, 10 - l, 24, l, C_AMB); cv_rect(x, 10 - l, 24, 1, C_ACC); }
    }
    cv_blit(0, 132);
}

/* ---- the scope: y 146-239, 240 x 94 (50 high while the meter and the status took the foot; Kris 3 Nov).
 * The canvas holds 240 x 64 px (platform.c's CV_MAX: RAM), so the scope goes out in two bands of 47 rows: the same
 * picture drawn twice, shifted by gfx.c's cv_oy, each band clipped to its rows (cv_begin would otherwise cut the
 * strip at 64 rows without a word: what Kris saw on the device, 3 Nov) */
#define SCOPE_Y 146u
#define SCOPE_H 94                              /* the whole strip */
#define SCOPE_BAND 47                           /* rows a band: 2 bands, each 240 x 47 within the canvas */
#define SCOPE_MID (SCOPE_H / 2)                 /* the centre line */
#define SCOPE_AMP (SCOPE_H / 2 - 3)             /* the window's peak lands here: +-44 px, 3 px clear of the edges */
_Static_assert(240 * SCOPE_BAND <= (int)CV_MAX && 2 * SCOPE_BAND == SCOPE_H, "a scope band fits the canvas");

int home_scope(const int16_t *ring, uint32_t w)
{
    static int16_t s[MV_SCOPE_N];
    static uint8_t ys[240];                     /* the trace's row per column (worked out once, drawn in each band) */
    uint32_t i, t = 0, any = 0;
    int32_t x, y, gain, band;
    for (i = 0; i < MV_SCOPE_N; i++) {          /* a snapshot, oldest first (the ISR may write meanwhile: a torn
                                                   sample or two is harmless on a scope) */
        s[i] = ring[(w + i) & (MV_SCOPE_N - 1u)];
        any |= (uint32_t)(s[i] > 64 || s[i] < -64);
    }
    for (i = 0; i + 241u < MV_SCOPE_N; i++)     /* a rising zero crossing, so the picture stands still */
        if (s[i] < 0 && s[i + 1u] >= 0) { t = i + 1u; break; }
    {                                           /* the gain: the window's peak to +-SCOPE_AMP px (auto, so a quiet pad
                                                   shows its shape too), never above x16 (the noise floor stays flat) */
        int32_t pk = 2048, v;
        for (x = 0; x < 240; x++) { v = s[t + (uint32_t)x]; v = v < 0 ? -v : v; if (v > pk) pk = v; }
        gain = (SCOPE_AMP << 16) / pk;          /* Q16 px per sample unit: one divide a frame */
    }
    for (x = 0; x < 240; x++) {                 /* clipped to the strip */
        y = SCOPE_MID - (((int32_t)s[t + (uint32_t)x] * gain) >> 16);
        ys[x] = (uint8_t)(y < 0 ? 0 : y > SCOPE_H - 1 ? SCOPE_H - 1 : y);
    }
    for (band = 0; band < SCOPE_H; band += SCOPE_BAND) {
        cv_begin(240, SCOPE_BAND, C_BLACK);
        cv_oy = -band;                          /* strip row r lands on canvas row r - band (the rest clipped) */
        for (x = 0; x < 240; x += 30)           /* the grid: dotted verticals, the centre line */
            for (y = band & ~1; y < band + SCOPE_BAND; y += 2) cv_pset(x, y, C_LINE);
        cv_rect(0, SCOPE_MID, 240, 1, C_LINE);
        for (x = 1; x < 240; x++) {             /* the trace: a segment a column, only those touching this band */
            int32_t a = ys[x - 1], b = ys[x];
            if ((a < band && b < band) || (a >= band + SCOPE_BAND && b >= band + SCOPE_BAND)) continue;
            cv_line(x - 1, a, x, b, C_HI);
        }
        cv_oy = 0;
        cv_blit(0, SCOPE_Y + (uint32_t)band);
    }
    return (int)any;
}

/* ---- the boot question: a new factory bank (mvance.c asks; SAVE / HOME answer it) */
void home_bank_ask(void)
{
    static const char *const L[5] = {"This firmware has a new", "factory bank. The programs", "saved are from the old one.",
                                     "SAVE  load the new bank", "HOME  keep my programs"};
    uint32_t i;
    lcd_fill(0, 48, 240, 192, C_BLACK);         /* under the header (home_header: y 0-47) */
    draw_text_box(0, 56, 240, &FONT_L, "NEW BANK", C_ACC, 1);
    for (i = 0; i < 3u; i++) draw_text_box(0, 100u + i * 20u, 240, &FONT_S, L[i], C_GRAY, 1);
    draw_text_box(0, 172, 240, &FONT_S, L[3], C_HI, 1);
    draw_text_box(0, 188, 240, &FONT_S, "(all 64 replaced)", C_DIM, 1);
    draw_text_box(0, 216, 240, &FONT_S, L[4], C_HI, 1);
}

/* ---- the program transfer (mv_xfer.h): full once, then the two count lines when they change */
void home_xfer(int sending, uint32_t n, uint32_t slot, int full)
{
    char a[32], b[32], *p;
    uint32_t v;
    if (full) {
        lcd_fill(0, 48, 240, 192, C_BLACK);     /* under the header (home_header: y 0-47, kept) */
        draw_text_box(0, 72, 240, &FONT_L, sending ? "SENDING" : "RECEIVING", C_ACC, 1);
        draw_text_box(0, 200, 240, &FONT_S, sending ? "to the computer (USB)" : "from the computer (USB)", C_DIM, 1);
    }
    p = a;                                      /* "PROGRAM 12" (1-based, as the panel numbers them) */
    for (v = 0; "PROGRAM "[v]; v++) *p++ = "PROGRAM "[v];
    v = (slot < MV_NPROG ? slot : MV_NPROG - 1u) + 1u;
    *p++ = (char)('0' + v / 10u); *p++ = (char)('0' + v % 10u); *p = 0;
    p = b;                                      /* "48 MESSAGES" */
    if (n >= 1000u) *p++ = (char)('0' + n / 1000u % 10u);
    if (n >= 100u) *p++ = (char)('0' + n / 100u % 10u);
    if (n >= 10u) *p++ = (char)('0' + n / 10u % 10u);
    *p++ = (char)('0' + n % 10u);
    for (v = 0; " MESSAGES"[v]; v++) *p++ = " MESSAGES"[v];
    *p = 0;
    draw_text_box(0, 128, 240, &FONT_S, slot < MV_NPROG ? a : "", C_HI, 1);
    draw_text_box(0, 152, 240, &FONT_S, b, C_GRAY, 1);
}
