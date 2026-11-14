/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance screens on the screen kit: implementation (what each screen shows: mv_kui.h; the drawing: mv_kit.c).
 *
 * Reads, from mvance.c: eng, ui, set, prog, scope_ring, scope_w, bank_ask, clk_bpm, midi_src, quiet(); with MV_XFER
 * xfer, xfer_slots, xfer_busy(); with MV_SEQ roll. From mv_ui.c: PAGES, NPAGES, HOME_LABEL, col_label, col_value,
 * choice_name, page_tag, ui_part, ui_patch, btn_name, midi_bit, the P_ and BAR_ names. From mv_home.c: category,
 * name_of. Writes only the screen. */
#include "mv_kui.h"

enum { G_NONE, G_LFO1, G_LFO2, G_OSC1, G_OSC2, G_MIX, G_PENV, G_FENV, G_AENV, G_FILT, G_AMP, G_TONE, G_MFX, G_DELAY,
       G_REVERB, G_VOICE, G_UNISON, G_PERF1, G_PERF2, G_GLOB1, G_GLOB2, G_MIDI, G_DEBUG, G_ARP, G_SEQ, G_CLIP, G_SAVE,
       G_HOME, G_OVL, G_LIST, G_BANK, G_XFER };

static struct {
    uint8_t full;                               /* the whole screen at the next draw */
    uint8_t roll_all;                           /* the roll's every part at its next draw */
    uint8_t xfer_was;                           /* the transfer's screen showed at the last draw */
    uint8_t edited;                             /* the program differs from its slot (this draw) */
    uint32_t prog_t;                            /* PRESETS last turned (fm1_ms; 0 never) */
    const mv_ui_t *u;                           /* the panel being drawn: the live one, or a tile's copy */
    uint32_t hot;                               /* its column in hand (0..3), 0xFF none */
    char la[40], lb[40];                        /* a screen's text lines for its panel (SAVE, CLIP, overlay ..) */
#if MV_DEBUG
    mv_debug_t dbg;                             /* DEBUG's numbers, and a count of their updates */
    uint32_t dbg_n;
#endif
} KU;

void mv_kui_full(void) { KU.full = 1; }
void mv_kui_prog_turned(void) { KU.prog_t = fm1_ms ? fm1_ms : 1u; }
#if MV_DEBUG
void mv_kui_debug(const mv_debug_t *d) { KU.dbg = *d; KU.dbg_n++; }
#endif

/* ---- text */
static char *ks(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *ku(char *p, uint32_t v)
{
    char t[10];
    uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10u);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}
static char *k2(char *p, uint32_t v) { *p++ = (char)('0' + v / 10u % 10u); *p++ = (char)('0' + v % 10u); *p = 0; return p; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
/* a program's name, trailing spaces gone (all 16 characters: the category prefix kept) */
static void kname(const uint8_t *nm, char *s)
{
    uint32_t i, n = 0;
    for (i = 0; i < MV_PROG_NAME; i++) s[i] = (char)(nm[i] >= 32u && nm[i] <= 126u ? nm[i] : '?');
    for (i = 0; i < MV_PROG_NAME; i++) if (s[i] != ' ') n = i + 1u;
    s[n] = 0;
}

/* ---- what the panel edits: a patch byte of the part PANEL says (ui_patch), raw, 0..127, or signed */
static int32_t PV(uint32_t b) { return mv_param_get(ui_patch(&eng, b), MV_PARAM_AT[b]); }
static int32_t PN(uint32_t b)
{
    const mv_param_t *d = &MV_PARAMS[MV_PARAM_AT[b]];
    return d->max > d->min ? (PV(b) - d->min) * 127 / (d->max - d->min) : 0;
}
static int32_t PS(uint32_t b) { return PV(b) - MV_PARAMS[MV_PARAM_AT[b]].centre; }
static const char *PC(uint32_t b) { const char *c = choice_name(b, (uint32_t)PV(b)); return c ? c : ""; }
static uint32_t hot_p(void) { return KU.hot < 4u ? PAGES[KU.u->page].p[KU.hot] : P_NONE; }
static uint32_t edit_part(void) { int both; return ui_part(&eng, 16u, &both); }   /* (16: any per-part byte) */

/* ---- the knob cells */

/* a choice's count and the one chosen (0: not a choice) */
static uint32_t kui_choice(uint32_t p, uint32_t *idx)
{
    const mv_ui_t *u = KU.u;
    int both;
    *idx = 0;
    if (IS_PATCH(p)) {
        const mv_param_t *d = &MV_PARAMS[MV_PARAM_AT[p]];
        const int32_t v = PV(p);
        if (!choice_name(p, (uint32_t)v)) return 0;
        *idx = (uint32_t)(v - d->min);
        return (uint32_t)(d->max - d->min + 1);
    }
    switch (p) {
    case P_PF + MV_PF_MODE: *idx = eng.perf[MV_PF_MODE]; return 3;
    case P_PF + MV_PF_VSPLIT: *idx = eng.perf[MV_PF_VSPLIT]; return 7;
    case P_PF + MV_PF_ARP: *idx = eng.perf[MV_PF_ARP] & 3u; return 4;
    case P_PANEL: *idx = u->panel; return 3;
    case P_LOCAL: case P_CLKSRC: case P_CLKOUT: *idx = midi_bit(u->midi, p); return 2;
    case P_LSYNC: *idx = eng.perf[MV_PF_SYNC(ui_part(&eng, p, &both))]; return MV_LFO1_SYNC_N;
    case P_CSYNC: *idx = eng.perf[MV_PF_CSYNUP]; return MV_CSYNC_N;
    case P_DSYNC: *idx = eng.perf[MV_PF_DSYNUP]; return MV_DSYNC_N;
    case P_SV + 3: *idx = u->sv_mode; return 3;
    case P_THEME: *idx = u->theme; return KIT_NTHEME;
    case P_COLOUR: *idx = u->colour; return 2;
    case P_RV + MV_RV_FREEZE: *idx = eng.rvb[MV_RV_FREEZE] & 1u; return 2;
#if MV_SEQ
    case P_CL + 3: *idx = u->cl_mode; return 2;
#endif
    default: break;
    }
    if (p >= P_ARP && p < P_ARP + MV_ARP_N && choice_name(p, eng.arpb[p - P_ARP])) {
        const uint32_t k = p - P_ARP;
        *idx = (uint32_t)(eng.arpb[k] - MV_ARP_MIN[k]);
        return (uint32_t)(MV_ARP_MAX[k] - MV_ARP_MIN[k] + 1u);
    }
    if (p >= P_SQ && p < P_SQ + MV_SQ_N && choice_name(p, eng.seqb[p - P_SQ])) {
        const uint32_t k = p - P_SQ;
        *idx = (uint32_t)(eng.seqb[k] - MV_SQ_MIN[k]);
        return (uint32_t)(MV_SQ_MAX[k] - MV_SQ_MIN[k] + 1u);
    }
    return 0;
}

/* a number shown without a bar (MIDI CH, SPLIT, SAVE's TO ..): its value and range, for the arc (0: none) */
static int kui_raw(uint32_t p, int32_t *v, int32_t *lo, int32_t *hi)
{
    const mv_ui_t *u = KU.u;
    int both;
    *lo = 0;
    if (IS_PATCH(p)) {
        const mv_param_t *d = &MV_PARAMS[MV_PARAM_AT[p]];
        *v = PV(p); *lo = d->min; *hi = d->max;
        return 1;
    }
    switch (p) {
    case P_MIDICH: *v = set.ch; *hi = 16; return 1;
    case P_PF + MV_PF_SPLIT: *v = eng.perf[MV_PF_SPLIT]; *hi = 127; return 1;
    case P_PF + MV_PF_CHLO: *v = eng.perf[MV_PF_CHLO]; *hi = 16; return 1;
    case P_PF + MV_PF_CHUP: *v = eng.perf[MV_PF_CHUP]; *hi = 16; return 1;
    case P_SV + 0: *v = u->sv_to; *hi = MV_NPROG - 1; return 1;
    case P_SV + 1: *v = u->sv_pos; *hi = MV_PROG_NAME - 1; return 1;
    case P_SV + 2: *v = eng.patch.b[u->sv_pos % MV_PROG_NAME]; *lo = 32; *hi = 125; return 1;
    case P_SQLEN: {
        const uint32_t k = MV_SQ_LEN(ui_part(&eng, 0u, &both));
        *v = eng.seqb[k]; *lo = MV_SQ_MIN[k]; *hi = MV_SQ_MAX[k];
        return 1;
    }
#if MV_SEQ
    case P_CL + 0: *v = u->cl_sel; *hi = (int32_t)MV_CLIP_ALL - 1; return 1;
    case P_CL + 1: *v = u->cl_pos; *hi = MV_CLIP_NAME - 1; return 1;
    case P_CL + 2: *v = (uint8_t)u->cl_name[u->cl_pos % MV_CLIP_NAME]; *lo = 32; *hi = 125; return 1;
#endif
    default: return 0;
    }
}

/* knob p's cell (lab: its label, 0 the page's own; hot: in hand). The kind comes from what col_value shows: a bar
 * from the bottom is an arc, from the middle an arc from the centre; a choice is a switch (OFF / ON), segments (up to
 * 10 names) or an arc (more); a number with no bar an arc over its range */
static void kui_knob(uint32_t p, const char *lab, int hot, kit_knob_t *k)
{
    uint32_t mode, n, idx;
    int32_t f, v, lo, hi;
    k->hot = (uint8_t)hot; k->mod = 0; k->n = 0; k->idx = 0; k->v = 0; k->c = 0; k->m0 = 0; k->m1 = 0;
    if (p == P_NONE || p == P_DEBUG) { k->w = KIT_W_NONE; k->lab = ""; k->val[0] = 0; return; }
    k->lab = lab ? lab : col_label(&eng, p);
    col_value(&eng, p, set.ch, k->val, &mode, &f);
    if (mode == BAR_UP) { k->w = KIT_W_VALUE; k->v = (int16_t)(clampi(f, 0, 1000) * 3840 / 1000); }
    else if (mode == BAR_MID) { k->w = KIT_W_CENTRE; k->c = 1920; k->v = (int16_t)(1920 + clampi(f, -1000, 1000) * 1920 / 1000); }
    else if ((n = kui_choice(p, &idx)) != 0u) {
        const char *a = choice_name(p, 0), *b = choice_name(p, 1);
        if (n == 2u && a && b && ((streq(a, "OFF") && streq(b, "ON")) || (streq(a, "ON") && streq(b, "OFF")))) {
            k->w = KIT_W_SWITCH; k->v = streq(k->val, "ON") ? 3840 : 0;
        } else if (n <= 10u) { k->w = KIT_W_CHOICE; k->n = (uint8_t)n; k->idx = (uint8_t)(idx < n ? idx : n - 1u); }
        else { k->w = KIT_W_VALUE; k->v = kit_ang((int32_t)idx, 0, (int32_t)n - 1); }
    } else if (kui_raw(p, &v, &lo, &hi)) { k->w = KIT_W_VALUE; k->v = kit_ang(v, lo, hi); }
    else k->w = KIT_W_READ;
    if (p == 41u) {                             /* CUTOFF: the span the filter envelope's ENV DEPTH adds */
        const int32_t a = PS(46);
        if (a) { k->mod = 1; k->m0 = k->v; k->m1 = kit_ang(PV(41) + a, 0, 127); }
    }
}
static int hot_of(const mv_ui_t *u, uint32_t c) { return u->last == c && fm1_ms - u->t_touch < 1500u; }   /* (8a) */

static uint32_t kui_voices(void)                /* the header's voice cells: 2 held, 1 releasing, 0 idle */
{
    uint32_t k, vs = 0;
    for (k = 0; k < MV_NV; k++) vs = vs << 2 | (eng.v[k].gate ? 2u : mv_env_active(&eng.v[k].aenv) ? 1u : 0u);
    return vs;
}

/* COLOUR BY TYPE: the colour of the name's category (mv_home.c category); 0 the theme's own */
static uint16_t kui_colour(void)
{
    static const char *const CAT[10] = {"LEAD", "PAD", "BASS", "PLUCK", "ARP", "BRASS", "STRINGS", "BELL", "FX", "SEQ"};
    static const uint16_t COL[10] = {RGB(255, 90, 90), RGB(77, 163, 255), RGB(72, 209, 122), RGB(255, 210, 77),
                                     RGB(92, 214, 255), RGB(255, 140, 40), RGB(208, 123, 255), RGB(64, 224, 208),
                                     RGB(255, 105, 180), RGB(180, 240, 80)};
    const char *c;
    uint32_t i;
    if (!ui.colour || !(c = category(eng.patch.b))) return 0;
    for (i = 0; i < 10u; i++)
        if (streq(c, CAT[i])) return ui.theme == KIT_LIGHT ? mix565(COL[i], 0, 6) : COL[i];   /* (darker on PAPER) */
    return 0;
}

/* ---- the footer of a page: PROG nn, the name (WARN: edited), the tempo (the clock's when one comes) or LOCAL OFF */
static void kui_foot(void)
{
    char l[12], nm[MV_PROG_NAME + 1u], r[16], *p;
    uint16_t rc = KT.dim;
    p = ks(l, "PROG "); k2(p, ui.cur + 1u);
    kname(eng.patch.b, nm);
    if (ui.midi & MV_MIDI_LOCALOFF) { ks(r, "LOCAL OFF"); rc = KT.warn; }
    else if (clk_bpm) { p = ku(r, clk_bpm); ks(p, " BPM"); rc = KT.accent; }
    else { p = ku(r, eng.tempo); ks(p, " BPM"); }
    kit_foot(l, nm, KU.edited ? KT.warn : KT.text, r, rc);
}

/* ---- the graphs (one cycle or picture of the values: never the engine's own sound) */

static void g_env(uint32_t kind)
{
    /* PITCH ENV: attack, decay to 0, DEPTH signed (from the middle line); FILTER / AMP ENV: A D S R */
    const int pe = kind == G_PENV;
    const uint32_t a0 = pe ? 28u : kind == G_FENV ? 47u : 54u, hb = hot_p();
    const int32_t A = 2 + PN(a0) * 48 / 127, D = 4 + PN(a0 + 1u) * 50 / 127;
    const int32_t S = pe ? 0 : PN(a0 + 2u), R = pe ? 0 : 4 + PN(a0 + 3u) * 50 / 127;
    const int32_t HOLD = pe ? 0 : 44, base = pe ? (PY0 + PY1) / 2 : PY1 - 1;
    const int32_t amp = pe ? PS(27) * 40 / 64 : PY1 - PY0 - 6;
    const int32_t ys = base * 16 - amp * S * 16 / 127;
    const int hot = hb >= a0 && hb <= a0 + (pe ? 1u : 3u);
    int32_t x, x1 = A, x2 = A + D, x3 = x2 + HOLD, x4 = x3 + R, i;
    grid(8, 4);
    if (pe) fill(PX0, base, PW, 1, KT.line);
    for (x = 0; x <= PW; x++) {
        int32_t y;
        if (x <= x1) y = base * 16 - amp * 16 * x / (x1 ? x1 : 1);
        else if (x <= x2) { const int32_t t = (x - x1) * 63 / (x2 - x1); y = ys + (base * 16 - amp * 16 - ys) * UI_EXP[t] / 16384; }
        else if (x <= x3) y = ys;
        else if (x <= x4) { const int32_t t = (x - x3) * 63 / (x4 - x3 ? x4 - x3 : 1); y = base * 16 + (ys - base * 16) * UI_EXP[t] / 16384; }
        else y = base * 16;
        yv[x] = (int16_t)y;
    }
    plot(PW + 1, KT.theme, 1, base * 16 + 8, 0);
    if (hot) {                                  /* the segment in hand, in ACCENT */
        const uint32_t s = hb - a0;
        const int32_t h0 = s == 0u ? 0 : s == 1u ? x1 : s == 2u ? x2 : x3, h1 = s == 0u ? x1 : s == 1u ? x2 : s == 2u ? x3 : x4;
        curve(yv + h0, (uint32_t)(h1 - h0 + 1), PX0 + h0, 32, KT.accent, 0);
    }
    {   /* the corners, and the segments' letters */
        static const char *const L4[4] = {"A", "D", "S", "R"};
        const int32_t xs[5] = {0, x1, x2, x3, x4};
        for (i = 1; i < (pe ? 3 : 4); i++) disc((PX0 + xs[i]) * 16 + 8, yv[xs[i]], 40, KT.text);
        for (i = 0; i < (pe ? 2 : 4); i++)
            if (xs[i + 1] - xs[i] >= 8)
                txt_fit(PX0 + (xs[i] + xs[i + 1]) / 2, PY1 - 12, 20, &FONT_LAB, L4[i], hot && (int32_t)(hb - a0) == i ? KT.accent : KT.dim, 1);
    }
}

static void g_filter(void)
{
    static const uint8_t SH[3][2] = {{2, 4}, {3, 5}, {0, 1}};   /* [HPF BPF LPF][-12 -24] -> UI_FLT's shape */
    const uint32_t sh = SH[clampi(PV(39), 0, 2)][PV(40) & 1], hb = hot_p();
    const int32_t rq = PN(42) * 7 * 16 / 127, r0 = rq >> 4, fr = rq & 15, r1 = r0 < 7 ? r0 + 1 : 7;
    const int32_t y0 = PY0 + 22, amt = PS(46);  /* 0 dB; a pixel a dB */
    int32_t pass, x;
    char s[16], *p;
    grid(8, 4);
    for (pass = 0; pass < 2; pass++) {          /* 0: where ENV DEPTH takes the cutoff (dashed), 1: the cutoff */
        const int32_t cx = PN(41) * PW / 127 + (pass ? 0 : amt * 2 * PW / 127);
        if (!pass && !amt) continue;
        for (x = 0; x <= PW; x++) {
            const int32_t d = x - cx + 220, i = d >> 1;
            int32_t db;
            if (i < 0) db = UI_FLT[sh][r0][0];
            else if (i >= 220) db = UI_FLT[sh][r0][220];
            else {
                const int32_t a = UI_FLT[sh][r0][i] * (16 - fr) + UI_FLT[sh][r1][i] * fr;
                const int32_t b = UI_FLT[sh][r0][i + 1] * (16 - fr) + UI_FLT[sh][r1][i + 1] * fr;
                db = (d & 1) ? (a + b) / 2 : a;
                db /= 16;
            }
            yv[x] = (int16_t)clampi(y0 * 16 - db * 16, PY0 * 16, PY1 * 16);
        }
        if (pass) {
            plot(PW + 1, hb == 41u || hb == 42u ? KT.accent : KT.theme, 1, PY1 * 16, 0);
            fill(PX0 + clampi(cx, 0, PW - 1), PY0, 1, PY1 - PY0, KT.dim);
        } else curve(yv, PW + 1, PX0, 16, KT.accent, 1);
    }
    p = ks(s, PC(39)); *p++ = ' '; ks(p, PC(40));
    chip(PX0 + PW, PY0 - 4, s, KT.theme, 2);
}

static void g_lfo(uint32_t n)
{
    /* LFO1: TRI SAW SQR RANDOM (the JP's); LFO2: a sine. Both run all the time: the dot is the part's phase now */
    const uint32_t part = edit_part(), wave = n ? 4u : (uint32_t)PV(16) & 3u;
    const int32_t mid = (PY0 + PY1) / 2, amp = 36;
    char s[16], v[8];
    uint32_t mode;
    int32_t x, f;
    grid(8, 4);
    fill(PX0, mid, PW, 1, KT.line);
    for (x = 0; x <= PW; x++) {
        const uint32_t ph = (uint32_t)x * 65535u / PW;
        const int32_t w = wave == 0u ? w_tri(ph) : wave == 1u ? w_saw(ph) : wave == 2u ? w_pulse(ph, 32768u)
                        : wave == 3u ? w_rnd(ph >> 13) : w_sin(ph);   /* (RANDOM: 8 steps a cycle) */
        yv[x] = (int16_t)(mid * 16 - w * amp * 16 / 16384);
    }
    plot(PW + 1, KT.theme, 1, mid * 16 + 8, 0);
    {
        const uint32_t ph = (n ? eng.part[part].lfo2.ph : eng.part[part].lfo1.ph) >> 16;
        const int32_t px = (int32_t)(ph * PW / 65535u);
        fill(PX0 + px, PY0, 1, PY1 - PY0, KT.dim);
        disc((PX0 + px) * 16 + 8, yv[px], 56, KT.accent);
    }
    if (!n) {
        x = chip(PX0 + PW, PY0 - 4, PC(24), KT.theme, 2);   /* LFO1 & ENV DEST */
        col_value(&eng, 17u, set.ch, v, &mode, &f);          /* RATE 1 (or its SYNC note) */
        ks(ks(s, "RATE "), v);
    } else {
        x = chip(PX0 + PW, PY0 - 4, PC(20), KT.theme, 2);   /* LFO2 DEPTH SELECT */
        ku(ks(s, "RATE "), (uint32_t)PV(19));
    }
    chip(PX0 + PW - x - 4, PY0 - 4, s, KT.dim, 2);
}

/* OSC1 by its wave (SSAW TRIMOD NOISE FBK SQR SAW TRI), Q14; c1 c2: CTRL1 CTRL2 (0..127) */
static int32_t w_jp1(uint32_t w, uint32_t ph, int32_t c1, int32_t c2)
{
    switch (w) {
    case 1: {                                   /* TRI MOD: the triangle folded by CTRL1 */
        int32_t t = w_tri(ph) * (64 + c1) / 64;
        if (t > 16384) t = 32768 - t;
        if (t < -16384) t = -32768 - t;
        return clampi(t, -16384, 16384);
    }
    case 2: return w_rnd(ph >> 9);              /* NOISE */
    case 3: return (w_saw(ph) * (127 - c2) + w_saw(ph * (2u + (uint32_t)c1 / 32u)) * c2) / 127;   /* FEEDBACK */
    case 4: return w_pulse(ph, 32768u + (uint32_t)c1 * 30000u / 127u);   /* SQR: CTRL1 the width */
    case 6: return w_tri(ph);
    default: return w_saw(ph);                  /* SUPER SAW's middle saw, SAW */
    }
}
static int32_t w_jp2(uint32_t w, uint32_t ph, int32_t c1)   /* OSC2: SQR SAW TRI NOISE */
{
    switch (w & 3u) {
    case 0: return w_pulse(ph, 32768u + (uint32_t)c1 * 30000u / 127u);
    case 1: return w_saw(ph);
    case 2: return w_tri(ph);
    default: return w_rnd(ph >> 9);
    }
}
/* OSC2's cycles a cycle of OSC1, Q8: RANGE (semitones; its ends WIDE: drawn 2 octaves), FINE ignored */
static uint32_t osc2_ratio(void)
{
    static const uint16_t SEMI[12] = {256, 271, 287, 304, 323, 342, 362, 384, 406, 431, 456, 483};
    int32_t s = clampi(PV(35) - 25, -24, 24), oct = 0;
    uint32_t r;
    while (s < 0) { s += 12; oct--; }
    while (s >= 12) { s -= 12; oct++; }
    r = SEMI[s];
    r = oct >= 0 ? r << (oct > 4 ? 4 : oct) : r >> (-oct > 4 ? 4 : -oct);
    return r < 128u ? 128u : r > 2048u ? 2048u : r;
}

static void g_osc(uint32_t kind)
{
    const int32_t mid = (PY0 + PY1) / 2 + 3, amp = 30;
    const uint32_t w1 = (uint32_t)PV(30), w2 = (uint32_t)PV(33), r2 = osc2_ratio(), cyc = 2u;
    const int32_t c1 = PN(31), c2 = PN(32), d1 = PN(37), xm = PN(22), bal = PS(23);
    int32_t x, j;
    char s[16], v[8];
    uint32_t mode;
    int32_t f;
    grid(8, 4);
    fill(PX0, mid, PW, 1, KT.line);
    if (kind == G_OSC1 && w1 == 0u) {           /* SUPER SAW: six more saws drifting by DETUNE, as bright as MIX */
        static const int8_t CP[6] = {-5, -3, -1, 1, 3, 5};
        for (j = 0; j < 6; j++) {
            for (x = 0; x <= PW; x++) {
                const uint32_t ph = (uint32_t)(x * (int32_t)cyc * 65536 / PW + CP[j] * (c1 + 4) * x / 4);
                yv[x] = (int16_t)(mid * 16 - w_saw(ph) * amp * 16 / 16384 * 4 / 5);
            }
            curve(yv, PW + 1, PX0, 16, mix565(KT.surf, KT.theme, 2u + (uint32_t)c2 / 16u), 0);
        }
    }
    if (kind == G_OSC2) {                       /* OSC1, faint, under OSC2 */
        for (x = 0; x <= PW; x++) yv[x] = (int16_t)(mid * 16 - w_jp1(w1, (uint32_t)x * cyc * 65536u / PW, c1, c2) * amp * 16 / 16384);
        curve(yv, PW + 1, PX0, 16, mix565(KT.surf, KT.theme, 6), 0);
    }
    for (x = 0; x <= PW; x++) {
        uint32_t ph = (uint32_t)x * cyc * 65536u / PW;
        const uint32_t p2 = PV(34) ? (ph & 65535u) * r2 >> 8 : ph * r2 >> 8;   /* SYNC: OSC2 restarts with OSC1 */
        int32_t v1;
        if (kind == G_OSC2) v1 = w_jp2(w2, p2, d1);
        else if (kind == G_MIX) {               /* the mix: BALANCE, RING (OSC1 x OSC2), X-MOD (OSC2 bends OSC1) */
            int32_t o2 = w_jp2(w2, p2, d1), o1;
            ph += (uint32_t)(o2 * xm / 127 / 2);
            o1 = w_jp1(w1, ph, c1, c2);
            if (PV(21)) o2 = o1 * o2 / 16384;
            v1 = (o1 * (64 - bal) + o2 * (64 + bal)) / 128;
        } else v1 = w_jp1(w1, ph, c1, c2);
        yv[x] = (int16_t)(mid * 16 - v1 * amp * 16 / 16384);
    }
    plot(PW + 1, KT.theme, 0, 0, 0);
    if (kind == G_OSC1) chip(PX0 + PW, PY0 - 4, PC(30), KT.theme, 2);
    else if (kind == G_OSC2) {
        x = chip(PX0 + PW, PY0 - 4, PC(33), KT.theme, 2);
        col_value(&eng, 35u, set.ch, v, &mode, &f);
        x += chip(PX0 + PW - x - 4, PY0 - 4, v, KT.dim, 2) + 4;
        if (PV(34)) chip(PX0 + PW - x - 4, PY0 - 4, "SYNC", KT.theme, 2);
    } else {
        x = 0;
        if (PV(21)) x += chip(PX0 + PW - x, PY0 - 4, "RING", KT.theme, 2) + 4;
        if (PV(34)) x += chip(PX0 + PW - x, PY0 - 4, "SYNC", KT.theme, 2) + 4;
        if (xm) { ku(ks(s, "X-MOD "), (uint32_t)PV(22)); chip(PX0 + PW - x, PY0 - 4, s, KT.dim, 2); }
    }
}

static void g_amp(void)
{
    /* LEVEL over two cycles of LFO1's tremolo (LFO1 MOD OFF: its depth); PAN below: AUTO sweeps, MANUAL sits */
    const int32_t lvl = PN(51), d1 = PS(52), mode = PV(58), top = PY0 + 12, bot = PY1 - 24;
    int32_t x, px;
    grid(8, 4);
    for (x = 0; x <= PW; x++) {
        const int32_t t = mode == 0 ? (d1 < 0 ? -d1 : d1) * (16384 - w_sin((uint32_t)x * 2u * 65536u / PW)) / 32768 : 0;
        const int32_t l = lvl * (64 - t) / 64;
        yv[x] = (int16_t)(bot * 16 - l * (bot - top) * 16 / 127);
    }
    plot(PW + 1, hot_p() == 51u ? KT.accent : KT.theme, 1, bot * 16 + 8, 0);
    fill(PX0 + 20, PY1 - 8, PW - 40, 2, KT.line);
    fill(PX0 + 20 + (PW - 40) / 2, PY1 - 12, 1, 10, KT.dim);
    txt(PX0 + 6, PY1 - 13, &FONT_LAB, "L", KT.dim);
    txt(PX0 + PW - 12, PY1 - 13, &FONT_LAB, "R", KT.dim);
    px = PX0 + 20 + (PW - 40) / 2;
    if (mode == 1) {                            /* AUTO: the span the pan sweeps */
        const int32_t w = (d1 < 0 ? -d1 : d1) * (PW - 40) / 128;
        fill(px - w, PY1 - 9, 2 * w, 4, KT.theme);
    } else if (mode == 2) px += d1 * (PW - 40) / 128;   /* MANUAL: where it sits */
    disc(px * 16 + 8, (PY1 - 7) * 16, 64, hot_p() == 52u ? KT.accent : KT.theme);
    chip(PX0 + PW, PY0 - 4, PC(58), KT.theme, 2);
}

static void g_tone(void)
{
    /* BASS and TREBLE as a tilt about the middle; GAIN as a chip */
    const int32_t b = PS(59), t = PS(60), mid = (PY0 + PY1) / 2;
    int32_t x;
    grid(8, 4);
    fill(PX0, mid, PW, 1, KT.line);
    for (x = 0; x <= PW; x++) {
        const int32_t s = clampi((x - 50) * 16384 / 120, 0, 16384);   /* 0 at the bass end .. 1 at the treble end */
        yv[x] = (int16_t)(mid * 16 - (b * (16384 - s) + t * s) / 16384 * 36 * 16 / 64);
    }
    plot(PW + 1, KT.theme, 1, mid * 16 + 8, 0);
    txt(PX0 + 2, PY1 - 12, &FONT_LAB, "BASS", KT.dim);
    txt(PX0 + PW - text_w(&FONT_LAB, "TREBLE") - 2, PY1 - 12, &FONT_LAB, "TREBLE", KT.dim);
    chip(PX0 + PW, PY0 - 4, PC(245), KT.theme, 2);
}

static void g_mfx(void)
{
    /* by TYPE: the choruses' delay swing (L and R), the flangers' and phasers' notches, TWIST's sweep, FREEZE's
     * held block, DIST's transfer curve; LEVEL scales it */
    const int32_t ty = PV(61), lv = PN(62), mid = (PY0 + PY1) / 2;
    int32_t x, k;
    grid(8, 4);
    if (ty <= 3) {                              /* SUPER CHORUS: L and R, half a cycle apart */
        const int32_t amp = 4 + lv * 18 / 127;
        for (k = 1; k >= 0; k--) {
            for (x = 0; x <= PW; x++)
                yv[x] = (int16_t)(mid * 16 - w_sin((uint32_t)x * (uint32_t)(1 + ty) * 65536u / PW + (uint32_t)k * 32768u) * amp * 16 / 16384);
            curve(yv, PW + 1, PX0, 24, k ? KT.rel : KT.theme, 0);
        }
        txt(PX0 + 2, PY1 - 12, &FONT_LAB, "DELAY TIME, L AND R", KT.dim);
    } else if (ty <= 8) {                       /* FLANGER (4..6), PHASER (7, 8): a comb of notches */
        const int32_t nn = ty <= 6 ? 6 + (ty - 4) * 3 : 2 + (ty - 7) * 2;
        for (x = 0; x <= PW; x++) {
            const int32_t c = w_sin((uint32_t)x * (uint32_t)nn * 32768u / PW);
            yv[x] = (int16_t)(PY0 * 16 + 8 * 16 + (c < 0 ? -c : c) * (16 + lv / 4) / 32 * (PY1 - PY0 - 12) / 16384 * 16 / 24);
        }
        plot(PW + 1, KT.theme, 1, PY1 * 16, 0);
        txt(PX0 + 2, PY1 - 12, &FONT_LAB, "RESPONSE", KT.dim);
    } else if (ty == 9) {                       /* TWIST: a sweep that speeds up */
        for (x = 0; x <= PW; x++)
            yv[x] = (int16_t)(mid * 16 - w_sin((uint32_t)(x * x) * 40u) * (10 + lv * 26 / 127) * 16 / 16384);
        plot(PW + 1, KT.theme, 0, 0, 0);
    } else if (ty <= 11) {                      /* FREEZE: a slice of sound held and repeated */
        for (x = 0; x <= PW; x++) {
            const uint32_t ph = (uint32_t)(x % 55) * 65536u / 55u * 3u;
            yv[x] = (int16_t)(mid * 16 - w_sin(ph) * (10 + lv * 26 / 127) * 16 / 16384 * (55 - x % 55) / 55);
        }
        plot(PW + 1, KT.theme, 0, 0, 0);
        for (x = 55; x < PW; x += 55) fill(PX0 + x, PY0, 1, PY1 - PY0, KT.dim);
    } else {                                    /* DIST: in to out, LEVEL the drive */
        const int32_t a = 1 + lv / 4, amp = 38;
        fill(PX0, mid, PW, 1, KT.line);
        seg(PX0 * 16, (mid + amp) * 16, (PX0 + PW) * 16, (mid - amp) * 16, 12, KT.line);
        for (x = 0; x <= PW; x++) {
            const int32_t in = x * 32768 / PW - 16384, ax = in < 0 ? -in : in;
            const int32_t v = clampi((1 + a) * in * 1024 / ((16384 + a * ax) >> 4), -16384, 16384);
            yv[x] = (int16_t)(mid * 16 - v * amp * 16 / 16384);
        }
        plot(PW + 1, KT.theme, 0, 0, 0);
    }
    chip(PX0 + PW, PY0 - 4, PC(61), KT.theme, 2);
}

static void g_delay(void)
{
    /* the echoes, L above and R below, by TYPE (PAN L>R / R>L / SHT bounce, MONO S / L both), falling by FEEDBACK,
     * scaled by LEVEL */
    const int32_t ty = PV(63), tm = PN(64), fb = PN(65), wet = PN(66);
    const int32_t mid = (PY0 + PY1) / 2, H = mid - PY0 - 6, dx = (ty == 2 ? 6 : 10) + tm * (ty == 2 ? 20 : 50) / 127;
    int32_t i, g = 127;
    char s[12];
    uint32_t mode;
    int32_t f;
    grid(8, 2);
    txt(PX0 + 2, PY0 + 1, &FONT_LAB, "L", KT.dim);
    txt(PX0 + 2, PY1 - 12, &FONT_LAB, "R", KT.dim);
    fill(PX0 + 12, mid - 2 - H, 4, H, KT.text);
    fill(PX0 + 12, mid + 2, 4, H, KT.text);
    for (i = 1; i < 24; i++) {
        const int32_t x = PX0 + 12 + i * dx, h = H * g / 127 * (16 + wet) / 143;
        const uint16_t c = i < 5 ? KT.theme : KT.rel;
        const int both = ty >= 3, left = ty == 1 ? (i & 1) == 0 : (i & 1) == 1;
        if (x + 4 > PX0 + PW || h < 1) break;
        if (both || left) fill(x, mid - 2 - h, 4, h, c);
        if (both || !left) fill(x, mid + 2, 4, h, c);
        g = g * (fb + 8) / 135;
    }
    i = chip(PX0 + PW, PY0 - 4, PC(63), KT.theme, 2);
    col_value(&eng, 64u, set.ch, s, &mode, &f);   /* TIME, or its SYNC note */
    chip(PX0 + PW - i - 4, PY0 - 4, s, KT.dim, 2);
}

static void g_reverb(void)
{
    /* the plate's tail: SIZE its length, LEVEL its height, HI DAMP how fast its top dies (the fill fades), LOWPASS /
     * HIPASS as chips; FREEZE holds it */
    const int32_t lv = eng.rvb[MV_RV_LEVEL], sz = eng.rvb[MV_RV_SIZE], hd = eng.rvb[MV_RV_HIDAMP];
    const int frz = eng.rvb[MV_RV_FREEZE] != 0u;
    const int32_t base = PY1 - 2, top = PY0 + 14, tau = 20 + sz * 190 / 127;
    char s[16];
    int32_t x, k;
    grid(8, 4);
    for (x = 0; x <= PW; x++) {
        const int32_t t = x * 63 / tau, e = frz ? 16384 : t > 63 ? 0 : UI_EXP[t];
        const int32_t h = e * (lv + 16) / 143 * (base - top) / 16384;
        yv[x] = (int16_t)((base - h) * 16);
    }
    under(yv, PW + 1, PX0, base * 16, mix565(KT.surf, KT.theme, (uint32_t)(5 - hd * 4 / 127)));
    for (x = 2; x < PW; x += 3) {               /* the reflections: dense, uneven */
        const int32_t h = (base * 16 - yv[x]) * (16384 + w_rnd((uint32_t)x)) / 32768 / 16;
        if (h > 0) fill(PX0 + x, base - h, 1, h, KT.rel);
    }
    curve(yv, PW + 1, PX0, 24, KT.theme, 0);
    k = 0;
    if (frz) k += chip(PX0 + PW, PY0 - 4, "FREEZE", KT.accent, 2) + 4;
    ku(ks(s, "LP "), eng.rvb[MV_RV_LOWPASS]);
    k += chip(PX0 + PW - k, PY0 - 4, s, KT.dim, 2) + 4;
    ku(ks(s, "HP "), eng.rvb[MV_RV_HIPASS]);
    chip(PX0 + PW - k, PY0 - 4, s, KT.dim, 2);
}

static void g_voice(void)
{
    /* the voices a note takes (POLY: VOICES; MONO / LEGATO: one), and the glide PORTAMENTO draws between two notes */
    const int mono = PV(71) != 0, porta = PV(69) != 0;
    const int32_t pt = PN(70), nv = (int32_t)(eng.nv < MV_NV ? eng.nv : MV_NV), poly = mono ? 1 : nv;
    char s[12];
    int32_t k, x;
    txt(PX0 + 2, PY0 - 2, &FONT_LAB, "VOICES", KT.dim);
    for (k = 0; k < (int32_t)MV_NV; k++) {
        const int lit = k < poly;
        rframe(PX0 + 2 + k * 13, PY0 + 14, 10, 22, 2, lit ? KT.theme : KT.line, lit ? KT.fill : KT.surf);
    }
    txt(PX0 + 112, PY0 - 2, &FONT_LAB, "GLIDE", KT.dim);
    {
        const int32_t x0 = PX0 + 112, w = PW - 112, lo = PY1 - 10, hi = PY0 + 16, gw = 2 + pt * (w - 24) / 127;
        for (x = 0; x <= w; x++) {
            int32_t y;
            if (!porta) y = x < w / 2 ? lo * 16 : hi * 16;   /* no glide: a step */
            else if (x < 12) y = lo * 16;
            else if (x < 12 + gw) { const int32_t t = (x - 12) * 63 / gw; y = hi * 16 + (lo - hi) * 16 * UI_EXP[t] / 16384; }
            else y = hi * 16;
            yv[x] = (int16_t)y;
        }
        curve(yv, (uint32_t)w + 1u, x0, 24, porta ? KT.theme : KT.dim, 0);
    }
    if (mono) ks(s, PV(72) ? "LEGATO" : "MONO");
    else ku(ks(s, "POLY "), (uint32_t)nv);
    x = chip(PX0 + PW, PY1 - 14, s, KT.theme, 2);
    if (porta) chip(PX0 + PW - x - 4, PY1 - 14, "PORTA", KT.dim, 2);
}

static void g_unison(void)
{
    /* the stack: a line a voice, spaced by DETUNE; the bend's range up and down */
    const int on = PV(243) != 0;
    const int32_t n = on ? (int32_t)(eng.nv < MV_NV ? eng.nv : MV_NV) : 1, det = PV(244), cx = PX0 + PW / 2, sp = 3 + det / 3;
    char s[16], *p;
    int32_t k;
    grid(8, 4);
    fill(cx, PY0, 1, PY1 - PY0, KT.line);
    for (k = 0; k < n; k++) {
        const int32_t x = cx + (2 * k - (n - 1)) * sp / 2;
        seg(x * 16, (PY0 + 14) * 16, x * 16, (PY1 - 18) * 16, 40, on ? (k & 1 ? KT.rel : KT.theme) : KT.dim);
    }
    txt(PX0 + 2, PY1 - 12, &FONT_LAB, "PITCH", KT.dim);
    p = ku(ks(s, "BEND +"), (uint32_t)PV(67)); p = ks(p, " -"); ku(p, (uint32_t)PV(68));
    k = chip(PX0 + PW, PY0 - 4, s, KT.dim, 2);
    if (!on) chip(PX0 + PW - k - 4, PY0 - 4, "UNISON OFF", KT.dim, 2);
}

/* a key's x on the PERFORM keyboard (C1 .. C6, 36 white keys of 6 px) and whether it is black */
static int32_t key_x(uint32_t note, int *blk)
{
    static const uint8_t WI[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    static const uint8_t BK[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    const uint32_t n = note < 36u ? 36u : note > 96u ? 96u : note;
    *blk = BK[n % 12u];
    return PX0 + 2 + (int32_t)((n - 36u) / 12u * 7u + WI[n % 12u]) * 6 + (*blk ? 4 : 0);
}

static void g_perf1(void)
{
    /* the keyboard: UPPER above, LOWER below, over the keys each plays (SINGLE: UPPER all; DUAL: both all; SPLIT:
     * LOWER below the split key, UPPER from it); BALANCE their brightness; the part(s) PANEL edits framed */
    const uint32_t mode = eng.perf[MV_PF_MODE], sp = eng.perf[MV_PF_SPLIT], bal = eng.perf[MV_PF_BAL], pan = KU.u->panel;
    const int32_t ky = PY0 + 30, kh = 26;
    char nm[MV_PROG_NAME + 1u];
    int32_t i, xs, xe, blk;
    for (i = 0; i < 36; i++) fill(PX0 + 2 + i * 6, ky, 5, kh, KT.raised);
    for (i = 36; i <= 96; i++) { const int32_t x = key_x((uint32_t)i, &blk); if (blk) fill(x, ky, 4, kh * 3 / 5, KT.line); }
    xs = PX0 + 2; xe = PX0 + 2 + 36 * 6;
    if (mode == MV_KEY_SPLIT) {
        const int32_t x = key_x(sp, &blk);
        fill(x - 1, PY0 + 4, 2, PY1 - PY0 - 4, KT.accent);
    }
    {   /* UPPER: above the keys */
        const int32_t a = mode == MV_KEY_SPLIT ? key_x(sp, &blk) : xs;
        const uint16_t c = mix565(KT.surf, KT.theme, 4u + bal * 11u / 127u);
        rframe(a, PY0 + 6, xe - a, 20, 3, mode != MV_KEY_SINGLE && pan != MV_UI_LOWER ? KT.accent : c, c);
        kname(eng.patch.b, nm);
        txt_fit(a + 4, PY0 + 8, xe - a - 8, &FONT_LAB, nm, KT.text, 0);
    }
    if (mode != MV_KEY_SINGLE) {                /* LOWER: below */
        const int32_t b = mode == MV_KEY_SPLIT ? key_x(sp, &blk) : xe;
        const uint16_t c = mix565(KT.surf, KT.theme, 4u + (127u - bal) * 11u / 127u);
        rframe(xs, ky + kh + 4, b - xs, 20, 3, pan != MV_UI_UPPER ? KT.accent : c, c);
        kname(eng.lower.b, nm);
        txt_fit(xs + 4, ky + kh + 6, b - xs - 8, &FONT_LAB, nm, KT.text, 0);
    } else txt(xs + 2, ky + kh + 8, &FONT_LAB, "LOWER: NOT PLAYED (SINGLE)", KT.dim);
}

static void g_perf2(void)
{
    /* the voices: LOWER's then UPPER's (VOICES L + U); the arp's part; the parts' channels */
    const uint32_t vs = eng.perf[MV_PF_VSPLIT] < 7u ? eng.perf[MV_PF_VSPLIT] : 3u, nl = vs + 1u;
    char s[24], v[8], *p;
    uint32_t k, mode;
    int32_t f;
    txt(PX0 + 2, PY0 - 2, &FONT_LAB, "VOICES  L + U", KT.dim);
    for (k = 0; k < MV_NV; k++) {
        const int lo = k < nl;
        rframe(PX0 + 2 + (int32_t)k * 20, PY0 + 12, 16, 24, 3, lo ? KT.accent : KT.theme, KT.fill);
        txt(PX0 + 6 + (int32_t)k * 20, PY0 + 18, &FONT_LAB, lo ? "L" : "U", KT.text);
    }
    col_value(&eng, P_PF + MV_PF_ARP, set.ch, v, &mode, &f);
    ks(ks(s, "ARP PLAYS "), v);
    txt(PX0 + 2, PY0 + 48, &FONT_VAL, s, KT.text);
    col_value(&eng, P_PF + MV_PF_CHLO, set.ch, v, &mode, &f);
    p = ks(ks(s, "CH LO "), v);
    col_value(&eng, P_PF + MV_PF_CHUP, set.ch, v, &mode, &f);
    ks(ks(p, "   CH UP "), v);
    txt(PX0 + 2, PY0 + 68, &FONT_LAB, s, KT.dim);
}

static void g_glob1(void)
{
    char s[24], *p;
    int32_t k;
    txt(PX0 + 2, PY0 - 2, &FONT_LAB, "VOICES", KT.dim);
    for (k = 0; k < (int32_t)MV_NV; k++) {
        const int lit = k < (int32_t)eng.nv;
        rframe(PX0 + 2 + k * 13, PY0 + 14, 10, 22, 2, lit ? KT.theme : KT.line, lit ? KT.fill : KT.surf);
    }
    p = ku(s, clk_bpm ? clk_bpm : eng.tempo); ks(p, " BPM");
    txt(PX0 + 120, PY0 - 2, &FONT_LAB, clk_bpm ? "TEMPO: MIDI CLOCK" : "TEMPO", KT.dim);
    txt(PX0 + 120, PY0 + 12, &FONT_BIG, s, clk_bpm ? KT.accent : KT.text);
    p = ks(s, "MIDI IN: ");
    if (set.ch) ku(ks(p, "CHANNEL "), set.ch); else ks(p, "ANY CHANNEL (OMNI)");
    txt(PX0 + 2, PY1 - 14, &FONT_LAB, s, KT.dim);
}

static void g_glob2(void)
{
    /* a swatch a theme: its ground, its surface, its two colours; the one in use framed */
    uint32_t k;
    for (k = 0; k < KIT_NTHEME; k++) {
        const kit_theme_def_t *d = &KIT_THEMES[k];
        const int32_t x = PX0 + (int32_t)(k % 4u) * 55, y = PY0 - 4 + (int32_t)(k / 4u) * 46;
        if (k == KU.u->theme) rrect(x - 2, y - 2, 53, 42, 5, KT.accent);
        rrect(x, y, 49, 38, 4, d->bg);
        rrect(x + 3, y + 3, 43, 16, 3, d->surf);
        disc((x + 13) * 16, (y + 11) * 16, 64, d->theme);
        disc((x + 30) * 16, (y + 11) * 16, 64, d->accent);
        txt_fit(x + 25, y + 23, 46, &FONT_LAB, d->name, d->text, 1);
    }
    txt_fit(PX0 + 192, PY0 + 50, 56, &FONT_LAB, KU.u->colour ? "BY TYPE" : "THEME", KT.dim, 1);
    txt_fit(PX0 + 192, PY0 + 62, 56, &FONT_LAB, "COLOUR", KT.line, 1);
}

/* a block with a name and a state, lit when on */
static void block(int32_t x, int32_t y, int32_t w, const char *a, const char *b, int on)
{
    rframe(x, y, w, 30, 4, on ? KT.theme : KT.line, on ? KT.fill : KT.surf);
    txt_fit(x + w / 2, y + 3, w - 4, &FONT_LAB, a, on ? KT.text : KT.dim, 1);
    txt_fit(x + w / 2, y + 16, w - 4, &FONT_LAB, b, on ? KT.theme : KT.dim, 1);
}
static void arrow(int32_t x0, int32_t x1, int32_t y, int on)
{
    const uint16_t c = on ? KT.theme : KT.line;
    seg(x0 * 16, y * 16, x1 * 16, y * 16, 20, c);
    seg((x1 - 5) * 16, (y - 4) * 16, x1 * 16, y * 16, 20, c);
    seg((x1 - 5) * 16, (y + 4) * 16, x1 * 16, y * 16, 20, c);
}

static void g_midi(void)
{
    /* the paths: the keys to the sound (LOCAL) and to USB, MIDI in to the sound; the clock's source and CLK OUT */
    const int local = !(KU.u->midi & MV_MIDI_LOCALOFF), cint = (KU.u->midi & MV_MIDI_CLKINT) != 0u;
    const int cout = (KU.u->midi & MV_MIDI_CLKOUT) != 0u;
    char s[16], *p;
    block(PX0, PY0 - 4, 56, "KEYS", "FM-1", 1);
    block(PX0 + PW - 64, PY0 - 4, 64, "SOUND", "ENGINE", 1);
    arrow(PX0 + 58, PX0 + PW - 66, PY0 + 6, local);
    txt_fit(PX0 + PW / 2 - 2, PY0 + 12, 90, &FONT_LAB, local ? "LOCAL ON" : "LOCAL OFF", local ? KT.theme : KT.warn, 1);
    block(PX0, PY0 + 36, 56, "MIDI", "USB OUT", 1);
    block(PX0 + PW - 64, PY0 + 36, 64, "MIDI IN", "USB / TRS", 1);
    if (clk_bpm && !cint) { p = ku(ks(s, "CLOCK IN "), clk_bpm); ks(p, " BPM"); }
    else ks(s, cint ? "CLOCK: INT" : "CLOCK: AUTO");
    txt_fit(PX0 + PW / 2 - 2, PY0 + 40, 96, &FONT_LAB, s, clk_bpm && !cint ? KT.accent : KT.dim, 1);
    txt_fit(PX0 + PW / 2 - 2, PY0 + 54, 96, &FONT_LAB, cout ? "CLK OUT ON" : "CLK OUT OFF", cout ? KT.theme : KT.dim, 1);
    seg((PX0 + 28) * 16, (PY0 + 27) * 16, (PX0 + 28) * 16, (PY0 + 35) * 16, 20, KT.theme);   /* the keys to USB */
}

#if MV_DEBUG
static void dbg_bar(int32_t y, const char *lab, uint32_t pc)
{
    const int32_t w = PW - 70, f = (int32_t)(pc > 100u ? 100u : pc) * w / 100;
    char s[8];
    txt(PX0 + 2, y, &FONT_LAB, lab, KT.dim);
    rrect(PX0 + 52, y + 2, w, 8, 3, KT.raised);
    if (f > 0) rrect(PX0 + 52, y + 2, f, 8, 3, pc >= 90u ? KT.warn : KT.theme);
    ks(ku(s, pc), "%");
    txt(PX0 + PW - 2 - text_w(&FONT_LAB, s), y, &FONT_LAB, s, pc >= 90u ? KT.warn : KT.text);
}
static void g_debug(void)
{
    /* the render time of a 5.8 ms buffer: the last 1/4 s's average, the worst of the last 3 s (WARN from 90 %) */
    const mv_debug_t *d = &KU.dbg;
    char s[40], *p;
    dbg_bar(PY0 - 2, "CPU", d->avg_pc);
    dbg_bar(PY0 + 14, "PEAK 3S", d->peak_pc);
    p = ks(s, "OCT ");
    *p++ = d->oct < 0 ? '-' : '+';
    p = ku(p, (uint32_t)(d->oct < 0 ? -d->oct : d->oct));
    p = ks(p, "   CH ");
    p = d->ch ? ku(p, d->ch) : ks(p, "OMNI");
    p = ks(p, "   IN ");
    ks(p, d->src == 1u ? "USB" : d->src == 2u ? "TRS" : "-");
    txt(PX0 + 2, PY0 + 34, &FONT_LAB, s, KT.text);
    p = ks(s, "CLOCK ");
    if (d->bpm) { p = ku(p, d->bpm); ks(p, d->running ? " BPM" : " STOP"); } else ks(p, "-");
    txt(PX0 + 2, PY0 + 50, &FONT_LAB, s, KT.text);
    if (d->prof) {
        p = ku(ks(s, "OSC "), d->osc_pc); p = ku(ks(p, "%  FLT "), d->flt_pc); p = ku(ks(p, "%  ETC "), d->etc_pc); ks(p, "%");
        txt(PX0 + 2, PY0 + 66, &FONT_LAB, s, KT.dim);
    }
    ks(ks(s, "FIRMWARE "), APP_VER);
    txt(PX0 + PW - 2 - text_w(&FONT_LAB, s), PY0 + 66, &FONT_LAB, s, KT.dim);
}
#endif

static void g_arp(void)
{
    /* sixteen steps of the pattern by MODE over OCTAVE octaves of a three-note chord; a step's length GATE, SWING
     * moving the off-beats, CHANCE dimming the steps that may not play */
    const uint32_t mode = eng.arpb[MV_ARP_MODE];
    const int on = eng.arpb[MV_ARP_ON] != 0u;
    const int32_t oct = eng.arpb[MV_ARP_OCT], gate = eng.arpb[MV_ARP_GATE], sw = eng.arpb[MV_ARP_SWING];
    const int32_t ch = eng.arpb[MV_ARP_CHANCE], n = 3 * (oct > 0 ? oct : 1), stw = PW / 16;
    const int32_t hh = (PY1 - PY0 - 8) / n > 2 ? (PY1 - PY0 - 8) / n : 2;
    int32_t i, walk = 0;
    grid(16, 4);
    for (i = 0; i < 16; i++) {
        const int32_t m = n > 1 ? 2 * n - 2 : 1, j = i % m, b = j < n ? j : m - j;   /* a bounce through the notes */
        int32_t st, x, w, k, lo = 0, hi;
        uint16_t c = on ? KT.theme : KT.dim;
        switch (mode) {
        case 1: st = n - 1 - i % n; break;                          /* DN */
        case 2: st = b; break;                                      /* UPDN */
        case 3: st = (int32_t)((uint32_t)w_rnd((uint32_t)i + 7u) & 1023u) % n; break;   /* RND */
        case 5: st = (i / 2) % n; break;                            /* REPEAT */
        case 6: st = n - 1 - b; break;                              /* DNUP */
        case 8: { const int32_t q = i % n; st = q & 1 ? n - 1 - q / 2 : q / 2; break; }   /* CONV */
        case 9: { const int32_t q = i % n, c0 = n / 2; st = q & 1 ? c0 + (q + 1) / 2 : c0 - q / 2; st = clampi(st, 0, n - 1); break; }   /* DIVG */
        case 10: st = i & 1 ? n - 1 : (i / 2) % (n - 1 > 0 ? n - 1 : 1); break;   /* PINKY: the top between */
        case 11: st = i & 1 ? 0 : 1 + (i / 2) % (n - 1 > 0 ? n - 1 : 1); break;   /* THUMB: the bottom between */
        case 12: walk = clampi(walk + ((w_rnd((uint32_t)i + 3u) & 1) ? 1 : -1), 0, n - 1); st = walk; break;   /* WALK */
        default: st = i % n; break;                                 /* UP, ORD, UP+8 */
        }
        x = PX0 + i * stw + 1 + (i & 1 ? sw * stw / 200 : 0);
        w = 1 + (stw - 2) * (gate + 8) / 135;
        if (((uint32_t)w_rnd((uint32_t)i * 13u + 1u) & 127u) >= (uint32_t)ch) c = mix565(KT.surf, c, 6);   /* may not play */
        hi = mode == 13u ? n - 1 : st;          /* CHORD: every note a step */
        lo = mode == 13u ? 0 : st;
        for (k = lo; k <= hi; k++) fill(x, PY1 - 4 - (k + 1) * hh, w, hh - 2 > 2 ? hh - 2 : 2, c);
    }
    if (!on) chip(PX0 + PW, PY0 - 4, "ARP OFF (TAP ARP)", KT.dim, 2);
}

#if MV_SEQ
static void g_seq(void)
{
    /* the two tracks as lanes: their LENGTH in bars, their notes' count, the playhead while running; REC armed in
     * WARN, a muted lane dim */
    uint32_t t;
    char s[24], *p;
    for (t = 0; t < 2u; t++) {
        const uint32_t part = t ? MV_PART_LO : MV_PART_UP, len = mv_seq_len(eng.seqb, part), bars = len / MV_SEQ_BAR;
        const mv_seq_track_t *tr = &eng.seq.t[part];
        const uint32_t nev = tr->rom ? tr->rom_n : tr->n;
        const int muted = eng.seqb[MV_SQ_MUTE(part)] != 0u, armed = (eng.seq_arm >> part & 1u) != 0u;
        const int32_t y = PY0 + (int32_t)t * 42, x0 = PX0 + 16, w = PW - 16;
        uint32_t b;
        txt(PX0 + 2, y + 9, &FONT_TTL, t ? "L" : "U", armed ? KT.warn : KT.dim);
        rframe(x0, y, w, 30, 4, armed ? KT.warn : KT.line, muted ? KT.surf : KT.fill);
        for (b = 1; b < bars && b < 32u; b++) fill(x0 + (int32_t)(b * (uint32_t)w / bars), y + 4, 1, 22, KT.grid);
        if (eng.seq_run && len) {
            const int32_t x = x0 + (int32_t)((eng.seq.ticks % len) * (uint32_t)w / len);
            fill(x, y + 2, 2, 26, KT.accent);
        }
        p = ku(s, bars); p = ks(p, bars == 1u ? " BAR  " : " BARS  "); p = ku(p, nev); ks(p, " EV");
        txt(x0 + 6, y + 9, &FONT_LAB, s, muted ? KT.dim : KT.text);
        if (muted) chip(x0 + w - 4, y + 8, "MUTE", KT.dim, 2);
        else if (armed) chip(x0 + w - 4, y + 8, "REC", KT.warn, 2);
    }
    txt(PX0 + 2, PY1 - 2, &FONT_LAB, eng.seq_run ? "PLAYING" : "STOPPED", eng.seq_run ? KT.theme : KT.dim);
}
#endif

/* SAVE and CLIP: a name, its cursor's character framed (cur: >= n none) */
static void name_cursor(const char *nm, uint32_t n, uint32_t cur, int32_t y)
{
    char c[2] = {0, 0};
    uint32_t i;
    int32_t x, w = 0;
    for (i = 0; i < n; i++) { c[0] = nm[i]; w += text_w(&FONT_BIG, c); }
    x = 120 - w / 2;
    for (i = 0; i < n; i++) {
        int32_t cw;
        c[0] = nm[i];
        cw = text_w(&FONT_BIG, c);
        if (i == cur) { rrect(x - 1, y - 1, cw + 2, 26, 3, KT.fill); fill(x, y + 24, cw, 2, KT.accent); }
        txt(x, y, &FONT_BIG, c, i == cur ? KT.accent : KT.text);
        x += cw;
    }
}

static void g_save(void)
{
    char nm[MV_PROG_NAME];
    uint32_t i;
    for (i = 0; i < MV_PROG_NAME; i++) nm[i] = (char)(eng.patch.b[i] >= 32u && eng.patch.b[i] <= 126u ? eng.patch.b[i] : '?');
    txt(PX0 + 2, PY0 - 4, &FONT_LAB, "NAME", KT.dim);
    name_cursor(nm, MV_PROG_NAME, KU.u->sv_pos, PY0 + 8);
    txt_fit(120, PY0 + 42, 220, &FONT_TTL, KU.la, KT.theme, 1);
    txt_fit(120, PY0 + 62, 220, &FONT_VAL, KU.lb, KT.dim, 1);
}

#if MV_SEQ
static void g_clip(void)
{
    const mv_fclip_t *f = mv_clip_factory(KU.u->cl_sel);
    char nm[20];
    uint32_t n = 0;
    if (f) for (; f->name[n] && n < 16u; n++) nm[n] = f->name[n];
    else for (; n < MV_CLIP_NAME; n++) nm[n] = KU.u->cl_name[n];
    txt(PX0 + 2, PY0 - 4, &FONT_LAB, f ? "FACTORY CLIP" : "CLIP NAME", KT.dim);
    name_cursor(nm, n, f ? 99u : KU.u->cl_pos, PY0 + 8);
    txt_fit(120, PY0 + 46, 220, &FONT_TTL, KU.la, KT.theme, 1);
}
#endif

static void g_ovl(void)
{
    /* PART SELECT, CLEAR TRACK?, TOO LONG TO RECORD: a card over the page (la: the title, lb: the value) */
    static const char *const HINT[3] = {"OCT- L   OCT+ U   BOTH L+U", "REC CLEARS, ANOTHER KEEPS", "OVER 128 NOTES: PLAY ONLY"};
    const uint32_t o = KU.u->ovl;
    rframe(20, PY0 - 4, 200, 82, 6, KT.theme, KT.raised);
    txt_fit(120, PY0 + 2, 190, &FONT_LAB, KU.la, KT.dim, 1);
    txt_fit(120, PY0 + 18, 190, &FONT_BIG, KU.lb, KT.text, 1);
    txt_fit(120, PY0 + 54, 190, &FONT_LAB, o == MV_OV_PART && eng.perf[MV_PF_MODE] == MV_KEY_SINGLE ? "ONE PART ONLY"
                                          : HINT[o == MV_OV_PART ? 0 : o == MV_OV_CLEAR ? 1 : 2], KT.theme, 1);
}

static void g_list(void)
{
    /* the programs around the one playing, for 2 s after PRESETS turns (10a) */
    char s[MV_PROG_NAME + 1u];
    int32_t r;
    for (r = -2; r <= 2; r++) {
        const uint32_t n = (ui.cur + MV_NPROG + (uint32_t)r) % MV_NPROG;
        const int32_t y = PY0 - 6 + (r + 2) * 19;
        if (!r) rframe(PX0 - 4, y - 1, PW + 6, 19, 4, KT.theme, KT.fill);
        k2(s, n + 1u);
        txt(PX0 + 2, y + 1, &FONT_VAL, s, r ? KT.dim : KT.theme);
        kname(mv_prog_get(&prog, n)->b, s);
        txt_fit(PX0 + 30, y + 1, PW - 34, &FONT_VAL, s, r ? KT.text : KT.text, 0);
    }
}

static void g_bank(void)
{
    static const char *const L[5] = {"This firmware has a new factory", "bank. The programs saved are",
                                     "from the old one.", "SAVE  load the new bank (all 64)", "HOME  keep my programs"};
    uint32_t i;
    for (i = 0; i < 5u; i++)
        txt_fit(120, PY0 - 4 + (int32_t)i * 17 + (i >= 3u ? 6 : 0), 220, &FONT_VAL, L[i], i >= 3u ? KT.text : KT.dim, 1);
}

#if MV_XFER
static void g_xfer(void)
{
    txt_fit(120, PY0 + 4, 220, &FONT_BIG, KU.la, KT.text, 1);
    txt_fit(120, PY0 + 36, 220, &FONT_VAL, KU.lb, KT.dim, 1);
    txt_fit(120, PY0 + 60, 220, &FONT_LAB, xfer.tx_n ? "TO THE COMPUTER (USB)" : "FROM THE COMPUTER (USB)", KT.dim, 1);
}
#endif

/* HOME's scope: the ring's last 440 frames (a point every 2nd), from a rising zero crossing so the picture stands
 * still, gained to the box (x16 at most); taken once a draw (kui_scope_take) so both strips show the same trace */
static void kui_scope_take(int32_t mid, int32_t amp)
{
    const uint32_t w = scope_w, M = MV_SCOPE_N - 1u;
    uint32_t i, t = 0;
    int32_t x, pk = 2048;
    for (i = 0; i + (uint32_t)PW + 1u < MV_SCOPE_N; i++)
        if (scope_ring[(w + i) & M] < 0 && scope_ring[(w + i + 1u) & M] >= 0) { t = i + 1u; break; }
    for (x = 0; x < PW; x++) { const int32_t v = scope_ring[(w + t + (uint32_t)x) & M]; if ((v < 0 ? -v : v) > pk) pk = v < 0 ? -v : v; }
    for (x = 0; x < PW; x++) yv[x] = (int16_t)(mid * 16 - scope_ring[(w + t + (uint32_t)x) & M] * amp * 16 / pk);
}

/* the M-VANCE saw fan (the logo: seven saws, the middle one brightest) in HOME's box while nothing sounds (7a) */
static void kui_fan(int32_t y0, int32_t h)
{
    const int32_t A = h - 22, pw = text_w(&FONT_TTL, "M-VANCE") + 16;
    int32_t d, s, x;
    for (d = 3; d >= 0; d--)                    /* dimmest first: the middle saw lands on top */
        for (s = d ? -1 : 1; s <= 1; s += 2) {
            const int32_t k = 3 + s * d;
            for (x = 0; x <= PW; x++) {
                const int32_t u = (x - PW / 2) * (1000 + (k - 3) * 30);
                int32_t f = u * 256 / (1000 * 64) + 128;
                f = ((f % 256) + 256) % 256;
                yv[x] = (int16_t)((y0 + 6 + d * 3 + A - ((A * f) >> 8)) * 16);
            }
            curve(yv, PW + 1, PX0, 20, mix565(KT.raised, KT.theme, (uint32_t)(15 - d * 3)), 0);
        }
    rrect(120 - pw / 2, y0 + h / 2 - 11, pw, 22, 4, KT.raised);
    txt(120 - pw / 2 + 8, y0 + h / 2 - 9, &FONT_TTL, "M-VANCE", KT.theme);
}

static void g_home(void)
{
    static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const uint32_t mode = eng.perf[MV_PF_MODE];
    const char *cat = category(eng.patch.b);
    char s[24], *p;
    int32_t x;
    if (mode == MV_KEY_SINGLE) {
        name_of(eng.patch.b, s);
        txt_fit(PX0 + 2, PY0 - 10, 150, &FONT_BIG, s[0] ? s : "-", KU.edited ? KT.warn : KT.text, 0);
    } else {                                    /* layered: UPPER and LOWER, a line each */
        txt(PX0 + 2, PY0 - 8, &FONT_LAB, "U", KT.dim);
        name_of(eng.patch.b, s); txt_fit(PX0 + 14, PY0 - 10, 140, &FONT_VAL, s, KU.edited ? KT.warn : KT.text, 0);
        txt(PX0 + 2, PY0 + 4, &FONT_LAB, "L", KT.dim);
        name_of(eng.lower.b, s); txt_fit(PX0 + 14, PY0 + 2, 140, &FONT_VAL, s, KU.edited ? KT.warn : KT.text, 0);
    }
    p = k2(ks(s, "PROGRAM "), ui.cur + 1u); *p = 0;
    txt(PX0 + 2, PY0 + 18, &FONT_LAB, s, KT.dim);
    if (ui.midi & MV_MIDI_LOCALOFF) txt(PX0 + 2 + text_w(&FONT_LAB, s) + 8, PY0 + 18, &FONT_LAB, "LOCAL OFF", KT.warn);
    else if (KU.edited) txt(PX0 + 2 + text_w(&FONT_LAB, s) + 8, PY0 + 18, &FONT_LAB, "EDITED", KT.warn);
    if (mode == MV_KEY_DUAL) cat = "DUAL";
    else if (mode == MV_KEY_SPLIT) {            /* SPLIT and its key */
        const uint32_t sp = eng.perf[MV_PF_SPLIT];
        p = ks(ks(s, "SPLIT "), NN[sp % 12u]);
        if (sp < 12u) ks(p, "-1"); else { *p++ = (char)('0' + sp / 12u - 1u); *p = 0; }
        cat = s;
    }
    x = cat ? chip(PX0 + PW, PY0 - 8, cat, KT.theme, 2) + 4 : 0;
    {   /* the play mode */
        char m[12];
        if (PV(243)) ks(m, "UNISON");
        else if (PV(71)) ks(m, PV(72) ? "LEGATO" : "MONO");
        else ku(ks(m, "POLY "), eng.nv);
        chip(PX0 + PW - x, PY0 - 8, m, KT.dim, 2);
    }
    x = chip(PX0 + PW, PY0 + 10, eng.arpb[MV_ARP_ON] ? "ARP ON" : "ARP OFF", eng.arpb[MV_ARP_ON] ? KT.theme : KT.dim, 2);
    if (set.oct) {
        char o[8];
        p = ks(o, "OCT "); *p++ = (int8_t)set.oct < 0 ? '-' : '+'; *p = 0;
        ku(p, (uint32_t)((int8_t)set.oct < 0 ? -(int8_t)set.oct : (int8_t)set.oct));
        chip(PX0 + PW - x - 4, PY0 + 10, o, KT.accent, 2);
    }
    rrect(4, PY0 + 32, 232, PY1 + 6 - (PY0 + 32), 4, KT.raised);
    if (!quiet()) {
        const int32_t mid = PY0 + 32 + (PY1 + 6 - (PY0 + 32)) / 2;
        fill(PX0, mid, PW, 1, KT.line);
        curve(yv, PW, PX0, 24, KT.theme, 0);    /* (taken by kui_scope_take) */
    } else kui_fan(PY0 + 32, PY1 + 6 - (PY0 + 32));
}

/* ---- the panel: the page's graph, then the knob in hand's readout over it (not on screens that say it already) */
static void kui_render(uint32_t kind)
{
    switch (kind) {
    case G_LFO1: g_lfo(0); break;
    case G_LFO2: g_lfo(1); break;
    case G_OSC1: case G_OSC2: case G_MIX: g_osc(kind); break;
    case G_PENV: case G_FENV: case G_AENV: g_env(kind); break;
    case G_FILT: g_filter(); break;
    case G_AMP: g_amp(); break;
    case G_TONE: g_tone(); break;
    case G_MFX: g_mfx(); break;
    case G_DELAY: g_delay(); break;
    case G_REVERB: g_reverb(); break;
    case G_VOICE: g_voice(); break;
    case G_UNISON: g_unison(); break;
    case G_PERF1: g_perf1(); break;
    case G_PERF2: g_perf2(); break;
    case G_GLOB1: g_glob1(); break;
    case G_GLOB2: g_glob2(); break;
    case G_MIDI: g_midi(); break;
#if MV_DEBUG
    case G_DEBUG: g_debug(); break;
#endif
    case G_ARP: g_arp(); break;
#if MV_SEQ
    case G_SEQ: g_seq(); break;
    case G_CLIP: g_clip(); break;
#endif
    case G_SAVE: g_save(); break;
    case G_HOME: g_home(); break;
    case G_OVL: g_ovl(); break;
    case G_LIST: g_list(); break;
    case G_BANK: g_bank(); break;
#if MV_XFER
    case G_XFER: g_xfer(); break;
#endif
    default: break;
    }
    if (kind < G_GLOB2 && kind != G_PERF2 && KU.hot < 4u) {   /* the knob in hand */
        const uint32_t p = hot_p();
        char v[8];
        uint32_t m;
        int32_t f;
        if (p != P_NONE && p != P_DEBUG) { col_value(&eng, p, set.ch, v, &m, &f); readout(col_label(&eng, p), v); }
    }
}

/* the page's graph (G_NONE: none) */
static uint32_t page_kind(const ui_page_t *pg)
{
    const uint32_t p = pg->p[0];
    switch (p) {
    case 16: case P_LSYNC: return G_LFO1;
    case 19: return G_LFO2;
    case 30: return G_OSC1;
    case 33: case 35: return G_OSC2;
    case 21: return G_MIX;
    case 27: return G_PENV;
    case 39: case 43: return G_FILT;
    case 47: return G_FENV;
    case 51: return G_AMP;
    case 54: return G_AENV;
    case 59: return G_TONE;
    case 61: return G_MFX;
    case 63: case P_DSYNC: return G_DELAY;
    case 71: return G_VOICE;
    case 243: return G_UNISON;
    case P_PF + MV_PF_MODE: return G_PERF1;
    case P_PF + MV_PF_VSPLIT: return G_PERF2;
    case P_MIDICH: return G_GLOB1;
    case P_THEME: return G_GLOB2;
    case P_LOCAL: return G_MIDI;
    case P_DEBUG: return G_DEBUG;
    case P_SQLEN: return G_SEQ;
    case P_CL: return G_CLIP;
    case P_SV: return G_SAVE;
    default: break;
    }
    if (p >= P_RV && p < P_RV + MV_RV_N) return G_REVERB;
    if (p >= P_ARP && p < P_ARP + MV_ARP_N) return G_ARP;
    return G_NONE;
}

/* what a graph shows, as a key: the sound's bytes, the global settings, the panel's state, the knob in hand */
static uint32_t state_key(void)
{
    const mv_ui_t *u = KU.u;
    uint32_t i, h = 2166136261u;
    for (i = 0; i < MV_PATCH_BYTES; i++) h = hsh(h, eng.patch.b[i]);
    if (eng.perf[MV_PF_MODE] != MV_KEY_SINGLE) for (i = 0; i < MV_PATCH_BYTES; i++) h = hsh(h, eng.lower.b[i]);
    for (i = 0; i < MV_PF_N; i++) h = hsh(h, eng.perf[i]);
    for (i = 0; i < MV_RV_N; i++) h = hsh(h, eng.rvb[i]);
    for (i = 0; i < MV_ARP_N; i++) h = hsh(h, eng.arpb[i]);
    for (i = 0; i < MV_SQ_N; i++) h = hsh(h, eng.seqb[i]);
    h = hsh(h, (uint32_t)u->panel | (uint32_t)u->midi << 8 | eng.nv << 16 | (uint32_t)eng.tempo << 24);
    h = hsh(h, KU.hot << 8 | u->page);
    return hsh(h, (uint32_t)u->theme << 8 | u->colour);
}

/* SAVE's and CLIP's lines, the overlay's (la, lb), before the panel's key */
static void kui_lines(const ui_page_t *pg)
{
    const mv_ui_t *u = KU.u;
    char *p;
    KU.la[0] = 0; KU.lb[0] = 0;
    if (u->ovl) {
        static const char *const PART[3] = {"UPPER", "LOWER", "BOTH"};
        ks(KU.la, u->ovl == MV_OV_PART ? "PART SELECT" : u->ovl == MV_OV_LONG ? "TOO LONG TO RECORD" : "CLEAR TRACK?");
        ks(KU.lb, u->ovl == MV_OV_PART ? (eng.perf[MV_PF_MODE] == MV_KEY_SINGLE ? "SINGLE" : PART[u->panel % 3u])
                                       : u->ovl_track == MV_PART_LO ? "LOWER" : "UPPER");
        return;
    }
    if (pg->btn == BTN_SAVE) {                  /* what SAVE will do, and what the slot holds (or will) */
        static const char *const WHAT[3] = {"WRITE TO ", "SWAP WITH ", "FACTORY "};
        const uint32_t t = u->sv_to;
        p = k2(ks(KU.la, WHAT[u->sv_mode % 3u]), t + 1u);
        p = ks(KU.lb, u->sv_mode == 2u ? "back to " : u->sv_mode == 1u ? "trades with " : "replaces ");
        kname(u->sv_mode == 2u ? MV_FACTORY[t] : mv_prog_get(u->prog, t)->b, p);
        return;
    }
#if MV_SEQ
    if (pg->p[0] == P_CL) {                     /* CLIP: what SAVE will do (or what the last one came to, 2 s) */
        const mv_clip_info_t *d = u->clips && u->cl_sel < MV_CLIP_N ? &u->clips->dir[u->cl_sel] : 0;
        const mv_fclip_t *f = mv_clip_factory(u->cl_sel);
        const uint32_t t = mv_ui_track(u, &eng), msg = fm1_ms - u->cl_msg_t < 2000u ? u->cl_msg : 0u;
        const char T1[2] = {t == MV_PART_LO ? 'L' : 'U', 0};
        if (msg == MV_CLM_BUSY) ks(KU.la, "ANOTHER SAVE WAITS");
        else if (msg == MV_CLM_RO || (f && u->cl_mode == MV_CL_SAVE)) ks(KU.la, "FACTORY: LOAD ONLY");
        else if (u->clips && u->clips->err) ks(KU.la, "WRITE FAILED");
        else if (u->clips && u->clips->pend == u->cl_sel) ks(KU.la, "WRITTEN AT STOP");
        else if (msg == MV_CLM_LOADED) ks(ks(KU.la, "LOADED > "), T1);
        else if (msg == MV_CLM_EMPTY) ks(KU.la, "EMPTY: NOTHING");
        else if (msg == MV_CLM_LONG) ks(KU.la, "TOO LONG TO SAVE");
        else if (u->cl_mode == MV_CL_SAVE) { p = ks(ks(KU.la, "SAVE "), T1); k2(ks(p, " > "), u->cl_sel + 1u); }
        else if (f) { p = ku(ks(KU.la, "LOAD F"), u->cl_sel - MV_CLIP_N + 1u); ks(ks(p, " > "), T1); }
        else if (d && d->used) { p = k2(ks(KU.la, "LOAD "), u->cl_sel + 1u); ks(ks(p, " > "), T1); }
        else k2(ks(KU.la, "EMPTY "), u->cl_sel + 1u);
    }
#endif
}

/* ---- the screens */

static void kui_head_page(const mv_ui_t *u, const ui_page_t *pg)
{
    char t[16], *p;
    uint32_t j, n = 0, i = 0;
    for (j = 1; j < NPAGES; j++) if (PAGES[j].btn == pg->btn) { if (&PAGES[j] == pg) i = n; n++; }
    p = ks(t, pg->title);
    if (pg->nsub > 1u) { *p++ = ' '; *p++ = (char)('1' + pg->sub); *p++ = '/'; *p++ = (char)('0' + pg->nsub); *p = 0; }   /* "OSC 2 1/2" */
    kit_head(btn_name(pg->btn), pg->btn == BTN_SAVE ? KT.warn : KT.theme, t, n, i, page_tag(u, &eng, pg), kui_voices(), MV_NV);
}

/* a page (live: the screen; else a tile, sim/mv_tiles.c) */
static void kui_page(const mv_ui_t *u, int live)
{
    const ui_page_t *pg = &PAGES[u->page];
    uint32_t kind = page_kind(pg), k, key, ms = 40u;
    kit_knob_t kn;
    KU.u = u;
    KU.hot = 0xFFu;
    for (k = 0; k < 4u; k++) if (hot_of(u, k)) KU.hot = k;
    kui_head_page(u, pg);
#if MV_SEQ
    if (pg->p[0] == P_SQBAR) {                  /* ROLL: the roll over the panel and the cells (6b), its own foot */
        if (live) { mv_roll_draw(&roll, &ui, &eng, KU.roll_all); KU.roll_all = 0; }
        return;
    }
#endif
#if MV_DEBUG
    if (kind == G_DEBUG) {                      /* DEBUG: the numbers as read-only cells */
        static const char *const LAB[4] = {"CPU", "PEAK", "VOICES", "CLOCK"};
        char *p;
        for (k = 0; k < 4u; k++) {
            kn.w = KIT_W_READ; kn.n = 0; kn.idx = 0; kn.hot = 0; kn.mod = 0; kn.v = 0; kn.c = 0; kn.m0 = 0; kn.m1 = 0;
            kn.lab = LAB[k];
            if (k == 0u) ks(ku(kn.val, KU.dbg.avg_pc), "%");
            else if (k == 1u) ks(ku(kn.val, KU.dbg.peak_pc), "%");
            else if (k == 2u) { p = ku(kn.val, KU.dbg.act); *p++ = '/'; ku(p, KU.dbg.nv); }
            else if (KU.dbg.bpm) ku(kn.val, KU.dbg.bpm); else ks(kn.val, "-");
            kit_cell(k, &kn);
        }
    } else
#endif
    for (k = 0; k < 4u; k++) { kui_knob(pg->p[k], 0, hot_of(u, k), &kn); kit_cell(k, &kn); }
    if (live && u->ovl) kind = G_OVL;
    else if (live && KU.prog_t && fm1_ms - KU.prog_t < 2000u) kind = G_LIST;
    kui_lines(pg);
    key = hsh(hstr(hstr(state_key(), KU.la), KU.lb), live ? ui.cur : 0u);
    if (kind == G_LFO1 || kind == G_LFO2) { key = hsh(key, fm1_ms / 80u); ms = 80u; }   /* the dot runs */
#if MV_DEBUG
    if (kind == G_DEBUG) key = hsh(key, KU.dbg_n);
#endif
#if MV_SEQ
    if (kind == G_SEQ) { key = hsh(hsh(key, eng.seq_run ? eng.seq.ticks / 24u + 1u : 0u), eng.seq_arm); ms = 80u; }   /* the playhead */
#endif
    if (kind == G_GLOB1 || kind == G_MIDI) key = hsh(key, clk_bpm);
    kit_panel(kui_render, kind, key, ms);
    if (live) kui_foot();
}

static void kui_home(void)
{
    uint32_t k, kind = G_HOME, key, ms = 80u;
    kit_knob_t kn;
    const ui_page_t *pg = &PAGES[0];
    KU.u = &ui;
    KU.hot = 0xFFu;
    for (k = 0; k < 4u; k++) if (hot_of(&ui, k)) KU.hot = k;
    kit_head("HOME", KT.theme, APP_NAME, 0, 0, 0, kui_voices(), MV_NV);
    for (k = 0; k < 4u; k++) { kui_knob(pg->p[k], HOME_LABEL[k], hot_of(&ui, k), &kn); kit_cell(k, &kn); }
    if (ui.ovl) kind = G_OVL;
    else if (KU.prog_t && fm1_ms - KU.prog_t < 2000u) kind = G_LIST;
    kui_lines(pg);
    key = hsh(hstr(hstr(state_key(), KU.la), KU.lb), ui.cur << 8 | KU.edited | (uint32_t)set.oct << 16);
    if (kind == G_HOME && !quiet()) {           /* sounding: the scope, 12 times a second */
        kui_scope_take(PY0 + 32 + (PY1 + 6 - (PY0 + 32)) / 2, 22);
        key = hsh(key, fm1_ms / 80u);
    } else key = hsh(key, 1u);                  /* (quiet: the fan, drawn once) */
    kit_panel(kui_render, kind, key, ms);
    kui_foot();
}

static void kui_blank_cells(void)
{
    kit_knob_t kn;
    uint32_t k;
    kn.w = KIT_W_NONE; kn.lab = ""; kn.val[0] = 0; kn.n = 0; kn.idx = 0; kn.hot = 0; kn.mod = 0;
    kn.v = 0; kn.c = 0; kn.m0 = 0; kn.m1 = 0;
    for (k = 0; k < 4u; k++) kit_cell(k, &kn);
}

void mv_kui_draw(void)
{
    if (kit_theme(ui.theme, kui_colour())) KU.full = 1;
#if MV_XFER
    {
        const uint8_t x = (uint8_t)(xfer_slots && xfer_busy());
        if (x != KU.xfer_was) { KU.xfer_was = x; KU.full = 1; }   /* a transfer started or ended: all again */
    }
#endif
    if (KU.full) { kit_full(); KU.full = 0; KU.roll_all = 1; }
    KU.u = &ui;
    KU.hot = 0xFFu;
    KU.edited = (uint8_t)mv_prog_edited(&prog, ui.cur, &eng.patch, &eng.lower, eng.perf);
    if (bank_ask) {                             /* the factory bank question (SAVE / HOME answer it) */
        kit_head("BANK", KT.warn, "NEW BANK", 0, 0, 0, kui_voices(), MV_NV);
        kit_panel(kui_render, G_BANK, 1u, 0u);
        kui_blank_cells();
        kit_foot("SAVE", "LOAD NEW", KT.text, "HOME: KEEP", KT.dim);
        return;
    }
#if MV_XFER
    if (KU.xfer_was) {                          /* the transfer: what it does, and how far */
        const uint32_t n = xfer.tx_n ? xfer.tx_n : xfer.rx_n, slot = xfer.tx_n ? (uint32_t)xfer.dump_pp : xfer.last_slot;
        char r[16];
        kit_head("USB", KT.accent, xfer.tx_n ? "SENDING" : "RECEIVING", 0, 0, 0, kui_voices(), MV_NV);
        if (slot < MV_NPROG) k2(ks(KU.la, "PROGRAM "), slot + 1u); else KU.la[0] = 0;
        ks(ku(KU.lb, n), " MESSAGES");
        kit_panel(kui_render, G_XFER, hstr(hstr(2166136261u, KU.la), KU.lb), 0u);
        kui_blank_cells();
        ks(ku(r, n), " MSG");
        kit_foot("XFER", "", KT.text, r, KT.accent);
        return;
    }
#endif
    if (mv_ui_home(&ui)) kui_home();
    else kui_page(&ui, 1);
}

#if defined(DINGHY_WEBSIM)
void mv_kui_tile(const mv_ui_t *t)
{
    const kit_keys_t keep = KK;                 /* the live screen's: kept, so it is not redrawn for nothing */
    uint32_t k;
    KK.head = KK.panel = KK.foot = 0;
    for (k = 0; k < 4u; k++) KK.cell[k] = 0;
    kui_page(t, 0);
    KK = keep;
}
#endif
