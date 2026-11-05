/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance filter: implementation (model, formats and cost in mv_filter.h). */
#include "mv_filter.h"
#include "mv_fixed.h"
#include "mv_tables.h"

#define LIN_Q15  (4 * 32768)                    /* linear up to 4 x full scale (JT kJpSatLinear) */
#define KNEE_Q15 (8 * 32768)                    /* the 8 x knee (JT kJpSatKnee = 1 / kVoiceGain) */

/* tanh(t), t = 0..4 in 1/16 steps, Q15: the output shoulder (overload only; tanh(4) = 0.9993, then 1) */
static const uint16_t TANH_Q15[65] = {
    0, 2045, 4075, 6073, 8025, 9919, 11743, 13486, 15143, 16706, 18173, 19542, 20813, 21986, 23066, 24054,
    24956, 25776, 26519, 27191, 27797, 28341, 28830, 29268, 29660, 30010, 30322, 30600, 30847, 31067, 31262, 31435,
    31589, 31726, 31846, 31953, 32048, 32132, 32206, 32271, 32329, 32381, 32426, 32466, 32501, 32532, 32560, 32584,
    32606, 32625, 32642, 32657, 32670, 32681, 32691, 32700, 32708, 32715, 32721, 32727, 32732, 32736, 32740, 32743,
    32746};

static int cascade(uint32_t m) { return m == MV_FLT_LP24 || m == MV_FLT_HP24 || m == MV_FLT_BP24; }

/* 2^(-x) for x >= 0 in Q16, result Q30: the R5a gain (control plane) */
static int32_t exp2n_q30(int32_t x)
{
    uint32_t i = (uint32_t)x >> 8, fr = (uint32_t)x & 0xFFu;    /* x in 1/256 steps of an octave */
    uint32_t v;
    if (i >= 256u * 30u) return 0;
    v = MV_EXP2N_Q31[i & 0xFFu];
    v -= (uint32_t)(((uint64_t)(v - MV_EXP2N_Q31[(i & 0xFFu) + 1u]) * fr) >> 8);
    return (int32_t)((v >> 1) >> (i >> 8));                       /* Q31 -> Q30, then the whole octaves */
}

void mv_flt_set(mv_flt_t *f, uint32_t mode, uint32_t reso, uint32_t cut)
{
    if (mode > MV_FLT_BP12) mode = MV_FLT_BP12;
    if (reso > 127u) reso = 127u;
    if (cut > 127u) cut = 127u;
    f->mode = (uint8_t)mode;
    f->r = MV_FLT_R_Q31[reso];
    if (mode == MV_FLT_LP24 || mode == MV_FLT_LP12) { f->ceil_q8 = MV_FLT_CEIL_LP_Q8; f->ceil_g = MV_FLT_G_CEIL_LP_Q26; }
    else                                            { f->ceil_q8 = MV_FLT_CEIL_HB_Q8; f->ceil_g = MV_FLT_G_CEIL_HB_Q26; }
    if (cascade(mode)) {
        f->mid = (int32_t)MV_FLT_T24_Q30[reso];
        /* R5a: -log2 gain = per-code slope x codes above 64 (0 at or below: not calculated) */
        f->out = (cut > 64u && MV_FLT_LC_Q16[reso] < 0) ? exp2n_q30(-MV_FLT_LC_Q16[reso] * (int32_t)(cut - 64u))
                                                       : (int32_t)MV_ENV_FULL_Q30;
    } else {
        f->mid = (int32_t)MV_ENV_FULL_Q30;
        f->out = (int32_t)MV_FLT_T12_Q30[reso];
    }
}

void mv_flt_reset(mv_flt_t *f) { f->s1a = f->s2a = f->s1b = f->s2b = 0; f->eba = f->ela = f->ebb = f->elb = 0; }

/* d, a d and the direct-form coefficients for integrator gain g (Q26) and the damping already in f->r */
static void flt_coefs(mv_flt_t *f, uint32_t g)
{
    uint32_t sh = 0;
    uint64_t den;
    f->g = (int32_t)g;
    /* d = 1 / (1 + 2 R g + g^2): the denominator in Q26 needs up to 37 bits; normalised to 32 for the division */
    den = (1ull << 26) + (((uint64_t)f->r * (uint64_t)g) >> 30) + (((uint64_t)g * (uint64_t)g) >> 26);
    while (den >> 32) { den >>= 1; sh++; }
    f->d = (int32_t)mv_udiv64_32(1ull << (56 - sh), (uint32_t)den);           /* Q30 */
    /* a d, a = 2R + g (Q26) */
    f->ad = (int32_t)((((uint64_t)(f->r >> 4) + (uint64_t)g) * (uint32_t)f->d) >> 26);
    f->hi = g > (1 << 26);
    if (f->hi) {                                /* the direct form's coefficients (all <= 1) */
        f->gd = (int32_t)(((uint64_t)g * (uint32_t)f->d) >> 26);
        f->ggd = (int32_t)(((uint64_t)g * (uint32_t)f->gd) >> 26);
        f->r2 = (int32_t)f->r;                  /* R in Q31 is 2R in Q30: the same bits */
    }
}

void mv_flt_lp(mv_flt_t *f, uint32_t g_q26, uint32_t r_q31)
{
    f->mode = MV_FLT_LP12;
    f->r = r_q31;
    f->mid = f->out = (int32_t)MV_ENV_FULL_Q30;  /* no trim: the level is the noise source's own */
    flt_coefs(f, g_q26);
}

void mv_flt_cutoff(mv_flt_t *f, int32_t code_q8)
{
    enum { N = sizeof MV_FLT_G_Q26 / sizeof MV_FLT_G_Q26[0] };
    uint32_t i, j;
    int32_t u, g0, g1, g2;
    int64_t g;
    code_q8 = mv_clamp(code_q8, MV_FLT_FLOOR_Q8, f->ceil_q8) - MV_FLT_G_CODE0_Q8;   /* >= 0 */
    /* quadratic through entries j..j+2 (quarter codes; j stops 3 short of the end), u = position from j, Q6 */
    i = (uint32_t)code_q8 >> 6;
    j = i < N - 3u ? i : N - 3u;
    u = code_q8 - (int32_t)(j << 6);
    g0 = (int32_t)MV_FLT_G_Q26[j]; g1 = (int32_t)MV_FLT_G_Q26[j + 1u]; g2 = (int32_t)MV_FLT_G_Q26[j + 2u];
    g = g0 + (((int64_t)u * (g1 - g0)) >> 6) + (((int64_t)u * (u - 64) * ((int64_t)g2 - 2 * (int64_t)g1 + g0)) >> 13);
    if (code_q8 >= f->ceil_q8 - MV_FLT_G_CODE0_Q8) g = f->ceil_g;   /* at the ceiling: exact, not the Q8 code */
    flt_coefs(f, (uint32_t)g);
}

/* one section on x (Q18): the three taps, the states committed. eb / el carry the low 26 bits each g product
 * shifts out into the next frame (error feedback): the integrators then add exactly, on average.
 * hp = d x - a d s1 - d s2 is taken as d (x - s2) - a d s1: one 64-bit product fewer, the same integers. */
#define SECTION(x, s1, s2, eb, el, hp, bp, lp, WHP)                                                    \
    do {                                                                                               \
        int64_t t_;                                                                                    \
        hp = (int32_t)(((int64_t)d * ((x) - (s2)) - (int64_t)ad * (s1) + (1 << 29)) >> 30);           \
        t_ = (int64_t)g * hp + eb;                                                                     \
        eb = (uint32_t)t_ & 0x3FFFFFFu;                                                                \
        bp = (int32_t)(t_ >> 26) + (s1);                                                               \
        t_ = (int64_t)g * bp + el;                                                                     \
        el = (uint32_t)t_ & 0x3FFFFFFu;                                                                \
        lp = (int32_t)(t_ >> 26) + (s2);                                                               \
        s1 = 2 * bp - (s1);                                                                            \
        s2 = 2 * lp - (s2);                                                                            \
    } while (0)

/* the direct form (g > 1): bp and lp straight from x and the states, coefficients <= 1, so nothing is
 * amplified by g; hp (HP modes only, WHP) from the SVF identity x = hp + 2R bp + lp. 30-bit remainders. */
#define SECTION_HI(x, s1, s2, eb, el, hp, bp, lp, WHP)                                                 \
    do {                                                                                               \
        int64_t t_;                                                                                    \
        int32_t u_ = (x) - (s2);                /* fits 32 bits (|x|, |s2| < 2^30): 32 x 32 products */ \
        t_ = (int64_t)gd * u_ + (int64_t)d * (s1) + eb;                                                \
        eb = (uint32_t)t_ & 0x3FFFFFFFu;                                                               \
        bp = (int32_t)(t_ >> 30);                                                                      \
        t_ = (int64_t)ggd * u_ + (int64_t)gd * (s1) + el;                                              \
        el = (uint32_t)t_ & 0x3FFFFFFFu;                                                               \
        lp = (int32_t)(t_ >> 30) + (s2);                                                               \
        if (WHP) hp = (x) - lp - (int32_t)(((int64_t)r2 * bp + (1 << 29)) >> 30);                      \
        s1 = 2 * bp - (s1);                                                                            \
        s2 = 2 * lp - (s2);                                                                            \
    } while (0)

/* One section over a block, in place (Q18 in, the chosen tap out, Q18). A loop per form and tap, each holding
 * only its own four states and its coefficients: with both -24 sections in one loop the chip ran out of
 * registers and spilled 64-bit values to the stack every frame (164 instructions a frame, 2026-11-01 profile). */
#define DEF_PASS(NAME, SEC, TAP, WHP)                                                                  \
static void NAME(const mv_flt_t *f, int32_t *buf, uint32_t n, int32_t *ps1, int32_t *ps2, uint32_t *peb,  \
                 uint32_t *pel)                                                                        \
{                                                                                                      \
    const int32_t g = f->g, d = f->d, ad = f->ad, gd = f->gd, ggd = f->ggd, r2 = f->r2;                \
    int32_t s1 = *ps1, s2 = *ps2;                                                                      \
    uint32_t eb = *peb, el = *pel, i;                                                                  \
    (void)g; (void)ad; (void)gd; (void)ggd; (void)r2;                                                  \
    for (i = 0; i < n; i++) {                                                                          \
        int32_t x = buf[i], hp = 0, bp, lp;                                                            \
        (void)hp; (void)bp; (void)lp;                                                                  \
        SEC(x, s1, s2, eb, el, hp, bp, lp, WHP);                                                       \
        buf[i] = TAP;                                                                                  \
    }                                                                                                  \
    *ps1 = s1; *ps2 = s2; *peb = eb; *pel = el;                                                        \
}
DEF_PASS(pass_lo_lp, SECTION, lp, 0)
DEF_PASS(pass_lo_hp, SECTION, hp, 0)
DEF_PASS(pass_lo_bp, SECTION, bp, 0)
DEF_PASS(pass_hi_lp, SECTION_HI, lp, 0)
DEF_PASS(pass_hi_hp, SECTION_HI, hp, 1)
DEF_PASS(pass_hi_bp, SECTION_HI, bp, 0)

typedef void (*mv_pass_fn)(const mv_flt_t *, int32_t *, uint32_t, int32_t *, int32_t *, uint32_t *, uint32_t *);

/* the JT's output tail: linear to 4 x, tanh shoulder into the 8 x knee */
static int32_t shoulder(int32_t y)
{
    int32_t a = y < 0 ? -y : y, t, s;
    uint32_t i, fr;
    if (a <= LIN_Q15) return y;
    t = (a - LIN_Q15) >> 7;                     /* u = (|y| - 4) / 4 in 1/16 steps, 6 bits of fraction */
    i = (uint32_t)t >> 6; fr = (uint32_t)t & 63u;
    if (i >= 64u) s = 32767;
    else s = TANH_Q15[i] + (int32_t)(((TANH_Q15[i + 1] - TANH_Q15[i]) * fr) >> 6);
    s = LIN_Q15 + (s << 2);                     /* 4 + 4 tanh(u), Q15 */
    if (s > KNEE_Q15) s = KNEE_Q15;
    return y < 0 ? -s : s;
}

void mv_flt_render(mv_flt_t *f, int32_t *buf, uint32_t n)
{
    /* the pass for this mode's tap and the coefficient form: [hi][tap], tap 0 LP / 1 HP / 2 BP */
    static const mv_pass_fn PASS[2][3] = {{pass_lo_lp, pass_lo_hp, pass_lo_bp}, {pass_hi_lp, pass_hi_hp, pass_hi_bp}};
    const uint32_t m = f->mode, tap = (m == MV_FLT_LP24 || m == MV_FLT_LP12) ? 0u
                                    : (m == MV_FLT_HP24 || m == MV_FLT_HP12) ? 1u : 2u;
    const mv_pass_fn pass = PASS[f->hi ? 1 : 0][tap];
    const int32_t mid = f->mid, out = f->out;
    uint32_t i;
    for (i = 0; i < n; i++) buf[i] *= 8;        /* Q15 -> Q18 */
    pass(f, buf, n, &f->s1a, &f->s2a, &f->eba, &f->ela);
    if (cascade(m)) {                           /* -24: the R/R0 trim between the sections, then the second */
        for (i = 0; i < n; i++) buf[i] = (int32_t)(((int64_t)buf[i] * mid + (1 << 29)) >> 30);
        pass(f, buf, n, &f->s1b, &f->s2b, &f->ebb, &f->elb);
    }
    if (out == (int32_t)MV_ENV_FULL_Q30) {      /* trim 1 (most -24 settings): (x 2^30 + 2^32) >> 33 = (x + 4) >> 3 */
        for (i = 0; i < n; i++) {
            int32_t y = (buf[i] + 4) >> 3;
            buf[i] = (y > LIN_Q15 || y < -LIN_Q15) ? shoulder(y) : y;
        }
    } else {
        for (i = 0; i < n; i++) {               /* output trim (Q30), Q18 -> Q15 rounded, the JT's tail */
            int32_t y = (int32_t)(((int64_t)buf[i] * out + (1ll << 32)) >> 33);
            buf[i] = (y > LIN_Q15 || y < -LIN_Q15) ? shoulder(y) : y;
        }
    }
}
