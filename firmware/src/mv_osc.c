/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance oscillators: see mv_osc.h. Constants carry the JT float value they come from. */
#include "mv_osc.h"
#include "mv_jvtri.h"                            /* the reduced JvTRI table (60 KB, flash) */

/* ---- polyBLEP for a jump of height 2 (from +1 to -1 or back), in Q15 of the unit residual.
 * d = phase since the jump (Q32, wrapping): within one frame after it, -(1 - d/inc)^2; within one frame before
 * it (d close to 2^32), +(1 - (2^32 - d)/inc)^2; else 0. The fraction uses one 32-bit division: inc >> 15 is
 * the step in Q17 units, so d / (inc >> 15) is d / inc in Q15. */
static inline int32_t blep(uint32_t d, uint32_t inc, uint32_t incq)
{
    uint32_t x;
    if (d < inc) {
        x = d / incq;
        if (x > 32767u) x = 32767u;
        x = 32768u - x;
        return -(int32_t)((x * x) >> 15);
    }
    d = 0u - d;                                 /* distance to the jump ahead */
    if (d <= inc) {
        x = d / incq;
        if (x > 32767u) x = 32767u;
        x = 32768u - x;
        return (int32_t)((x * x) >> 15);
    }
    return 0;
}

/* Law constants, from OscCore.cpp (JT float value in the comment), in Q16 so the per-block maths keeps
 * ~1e-5 precision: the port then sits 60..90 dB under the float signal (tools/compare/cmp_osc.cpp). */
/* ---- JvSAW: beta(k) = 1.2118 - 4.70 k, k >= 0.0334; G = 0.6034671; B3 scale 4pi^2/3 = 13.1594725 */
#define SAW_G_Q16     39549                     /* 0.6034671 */
#define SAW_KMIN_Q20  35022                     /* 0.0334 (Q20) */
#define SAW_B0_Q16    79417                     /* 1.2118 */
#define SAW_BK_Q16    308019                    /* 4.70 */
#define SAW_SCALE_Q16 520437                    /* 13.1594725 * 0.6034671 (the B3 scale folded with G) */
/* ---- Tri-mod: g = 1.635 - 0.967 (k - 1/2), clamped 1.375 .. 2.02 */
#define TRI_GMID_Q16  107151                    /* 1.635 */
#define TRI_GK_Q16    63373                     /* 0.967 */
#define TRI_GMIN_Q16  90112                     /* 1.375 */
#define TRI_GMAX_Q16  132383                    /* 2.02 */
/* ---- SQR width: 0.5 + 0.5 * 0.944 * k, floor 50 % (JpPwm), cap 95 % (OscCore::setShape) */
#define PW_SPAN_Q31   2027224432u               /* 0.944 * 2^31 */
#define PW_MAX_Q32    4080218931u               /* 0.95 * 2^32 */

void mv_osc_shape(mv_osc_t *o, uint32_t wave, uint32_t ctrl1, int32_t mod_q15)
{
    /* the knob as 0..2^20 (byte / 127, Q20: the JT's float knob to ~1e-6), plus the modulation, clamped */
    int32_t k = mv_clamp((int32_t)((ctrl1 > 127u ? 127u : ctrl1) * 1048576u / 127u) + mod_q15 * 32, 0, 1048576);
    if (wave == MV_W_SQR) {
        uint32_t pw = 0x80000000u + (uint32_t)(((uint64_t)(uint32_t)k * PW_SPAN_Q31) >> 20);   /* Q32 */
        if (pw > PW_MAX_Q32) pw = PW_MAX_Q32;
        o->pw = pw;
        o->dc = (int32_t)(pw >> 16) - 32768;    /* 2 pw - 1, Q15: the DC the width puts on the pulse */
    } else if (wave == MV_W_SAW) {
        int32_t beta;
        if (k < SAW_KMIN_Q20) k = SAW_KMIN_Q20;
        beta = SAW_B0_Q16 - (int32_t)(((int64_t)SAW_BK_Q16 * k) >> 20);            /* Q16 */
        o->saw_c = (int32_t)(((int64_t)beta * SAW_SCALE_Q16) >> 16);               /* Q16 */
    } else if (wave == MV_W_JVTRI) {
        /* SHAPE on the JT's axis: frame position k x 32 (Q16), then the two kept frames around it */
        int32_t pos = k * 2;                    /* Q20 x 32 >> 4 = Q16 on 0..32 */
        uint32_t j = 0, lv = 0;
        while (j < MV_JVTRI_FRAMES - 2u && pos >= (int32_t)MV_JVTRI_KEEP[j + 1] << 16) j++;
        /* Q14: the per-frame products (sample step x fraction) then stay inside 32 bits */
        o->tm = (int32_t)(((pos - ((int32_t)MV_JVTRI_KEEP[j] << 16)) / (MV_JVTRI_KEEP[j + 1] - MV_JVTRI_KEEP[j])) >> 2);
        o->tm = mv_clamp(o->tm, 0, 16384);
        /* mip level: the first whose top harmonic stays under fs/2 at this step (the JT's choice) */
        while (lv + 1u < MV_JVTRI_LEVELS && (uint64_t)MV_JVTRI_HARM[lv] * o->inc > 0x80000000ull) lv++;
        o->tlog2 = MV_JVTRI_LOG2[lv];
        o->ta = MV_JVTRI + j * MV_JVTRI_SPF + MV_JVTRI_OFF[lv];
        o->tb = o->ta + MV_JVTRI_SPF;
    } else {
        int32_t g = TRI_GMID_Q16 - (int32_t)(((int64_t)TRI_GK_Q16 * (k - 524288)) >> 20);
        uint32_t q, r, inv;
        g = mv_clamp(g, TRI_GMIN_Q16, TRI_GMAX_Q16);
        o->tri_g = g;
        /* 1/g in Q28 by long division in 32-bit steps (no 64-bit division on the chip): 2^44 / g16 */
        q = 0x80000000u / (uint32_t)g;          /* 2^31 / g16 = 1/g in Q15 */
        r = 0x80000000u - q * (uint32_t)g;      /* remainder, < g < 2^18 */
        inv = (q << 13) + ((r << 13) / (uint32_t)g);   /* Q28 */
        /* the wrap phases (OscCore: (1 -+ 1/g)/4, (3 -+ 1/g)/4), Q32 = Q28 / 4 << 4 = Q28 << 2 */
        o->tri_p[0] = (0x10000000u - inv) << 2;
        o->tri_p[1] = (0x10000000u + inv) << 2;
        o->tri_p[2] = (0x30000000u - inv) << 2;
        o->tri_p[3] = (0x30000000u + inv) << 2;
    }
}

/* The three waves' frame loops, with the phase step as a parameter: STEP advances ph for frame i. One body
 * for the plain, the X-MOD (per-frame step) and the sync-slave (per-frame reset) renders, so the waves cannot
 * drift apart between them. polyBLEP uses the base step in all three, as the JT does (OscCore: _inc). */
#define WAVE_LOOPS(STEP)                                                                               \
    if (wave == MV_W_SQR) {                                                                            \
        uint32_t pw = o->pw;                                                                           \
        int32_t dc = o->dc;                                                                            \
        for (i = 0; i < n; i++) {                                                                      \
            int32_t v;                                                                                 \
            STEP;                                                                                      \
            v = ph < pw ? 32768 : -32768;                                                              \
            v += blep(ph, inc, incq);           /* rising edge at 0 */                                 \
            v -= blep(ph - pw, inc, incq);      /* falling edge at pw */                               \
            out[i] = v - dc;                                                                           \
        }                                                                                              \
    } else if (wave == MV_W_SAW) {                                                                     \
        int32_t c = o->saw_c;                                                                          \
        for (i = 0; i < n; i++) {                                                                      \
            int32_t saw, x, b3;                                                                        \
            STEP;                                                                                      \
            x = (int32_t)(ph >> 16);            /* phase, Q16 (0..65535) */                            \
            saw = x - 32768 - blep(ph, inc, incq);              /* 2ph - 1 (Q15), band-limited */      \
            /* B3 = ph (ph - 1/2)(ph - 1): Q16 * Q16 * Q16 = Q48 in 64 bits, kept as Q30 (|B3| <= 0.048) */ \
            b3 = (int32_t)((((int64_t)x * (x - 32768)) * (x - 65536)) >> 18);                         \
            out[i] = ((saw * SAW_G_Q16) >> 16) - (int32_t)(((int64_t)c * b3) >> 31);   /* -> Q15 */   \
        }                                                                                              \
    } else if (wave == MV_W_JVTRI) {                                                                   \
        const int16_t *ta = o->ta, *tb = o->tb;                                                        \
        const int32_t tm = o->tm;                                                                      \
        const uint32_t lg = o->tlog2, sh = 32u - lg, mask = (1u << lg) - 1u;                          \
        (void)incq;                                                                                    \
        for (i = 0; i < n; i++) {                                                                      \
            uint32_t ix, nx;                                                                           \
            int32_t fr, va, vb;                                                                        \
            STEP;                                                                                      \
            ix = ph >> sh; nx = (ix + 1u) & mask;                                                      \
            fr = (int32_t)((ph << lg) >> 18);   /* position within the sample step, Q14 */             \
            va = ta[ix] + (((ta[nx] - ta[ix]) * fr) >> 14);   /* (|diff| < 2^16, x 2^14: 32-bit) */  \
            vb = tb[ix] + (((tb[nx] - tb[ix]) * fr) >> 14);                                            \
            out[i] = va + (((vb - va) * tm) >> 14);   /* between the two SHAPE frames */               \
        }                                                                                              \
    } else {                                                                                           \
        int32_t g = o->tri_g;                                                                          \
        uint32_t pa = o->tri_p[0], pb = o->tri_p[1], pc = o->tri_p[2], pd = o->tri_p[3];             \
        for (i = 0; i < n; i++) {                                                                      \
            int32_t tri, v;                                                                            \
            STEP;                                                                                      \
            tri = ph < 0x80000000u ? (int32_t)(ph >> 15) - 32768 : 98304 - (int32_t)(ph >> 15);       \
            v = (int32_t)(((int64_t)tri * g) >> 16);   /* g * tri, Q15 (|v| < 3) */                   \
            /* wrap into -1..+1, decided by the phase against the same four jump phases the            \
             * band-limiting uses, never by the value (a value test can disagree by a frame and leave  \
             * a full-height spike) */                                                                 \
            if (ph < pa) v += 65536;            /* rising, below -1 */                                 \
            else if (ph >= pb && ph < pc) v -= 65536;   /* above +1 (both sides of the peak) */        \
            else if (ph >= pd) v += 65536;      /* falling, below -1 */                                \
            v -= blep(ph - pa, inc, incq) + blep(ph - pb, inc, incq);   /* jumps of -2 */             \
            v += blep(ph - pc, inc, incq) + blep(ph - pd, inc, incq);   /* jumps of +2 */             \
            out[i] = v;                                                                                \
        }                                                                                              \
    }

void mv_osc_render(mv_osc_t *o, uint32_t wave, int32_t *out, uint32_t n)
{
    uint32_t ph = o->ph, inc = o->inc, incq = (inc >> 15) | 1u, i;
    WAVE_LOOPS(ph += inc)
    o->ph = ph;
}

void mv_osc_render_fm(mv_osc_t *o, uint32_t wave, int32_t *out, uint32_t n, const int32_t *dinc)
{
    /* through-zero: a negative step runs the phase backwards; Q32 wraps both ways (and over several cycles a
     * frame) by itself, so no fold is needed (the JT's floorf) */
    uint32_t ph = o->ph, inc = o->inc, incq = (inc >> 15) | 1u, i;
    WAVE_LOOPS(ph += inc + (uint32_t)dinc[i])
    o->ph = ph;
}

void mv_osc_render_sync(mv_osc_t *o, uint32_t wave, int32_t *out, uint32_t n, const uint32_t *reset)
{
    uint32_t ph = o->ph, inc = o->inc, incq = (inc >> 15) | 1u, i;
    WAVE_LOOPS(ph = reset[i] == MV_NO_RESET ? ph + inc : reset[i])
    o->ph = ph;
}

/* ==== Super Saw (law and cost in mv_osc.h) ==== */
#include "mv_tables.h"

/* Szabo table 1: each voice's frequency offset at full detune, Q31 (centre 0) */
static const int32_t SSAW_OFF_Q31[7] = {
    -236272873, -135043199, -41926526, 0, 42761145, 133499137, 230752315};   /* -0.11002313 .. 0.10745242 */

void mv_ssaw_key(mv_ssaw_t *s)
{
    uint32_t i, x = s->rng ? s->rng : 0xA5A5A5A5u;
    for (i = 0; i < 7u; i++) {                  /* the JT: (x >> 8) / 2^24 -> the same phases in Q32 */
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        s->ph[i] = (x >> 8) << 8;
    }
    s->rng = x;
}

void mv_ssaw_set(mv_ssaw_t *s, uint32_t inc, uint32_t detune, uint32_t mix)
{
    uint32_t i, pos, fr, depth, m, w;
    uint64_t den;
    int32_t c, sd;
    if (detune > 127u) detune = 127u;
    if (mix > 127u) mix = 127u;
    if (s->key_ok && s->key_inc == inc && s->key_det == detune && s->key_mix == mix) return;   /* nothing moved */
    s->key_inc = inc; s->key_det = (uint8_t)detune; s->key_mix = (uint8_t)mix; s->key_ok = 1;
    /* depth: the LUT at detune / 127 over 255 steps (index Q8), linear between entries; 0 is exact unison */
    pos = detune * 255u * 256u / 127u;
    i = pos >> 8; fr = pos & 255u;
    depth = i >= 255u ? MV_SSAW_DETUNE_Q24[255] :
            MV_SSAW_DETUNE_Q24[i] + (uint32_t)(((uint64_t)(MV_SSAW_DETUNE_Q24[i + 1] - MV_SSAW_DETUNE_Q24[i]) * fr) >> 8);
    for (i = 0; i < 7u; i++) {                  /* inc x (1 + offset x depth) */
        int64_t d = ((int64_t)SSAW_OFF_Q31[i] * depth) >> 24;   /* Q31 */
        s->inc[i] = inc + (uint32_t)(int32_t)(((int64_t)inc * d) >> 31);
    }
    /* gains (Szabo polynomials, JP law), m = mix / 127 in Q16 */
    m = mix * 65536u / 127u;
    c = 65395 - (int32_t)(((int64_t)36285 * m) >> 16);                       /* -0.55366 m + 0.99785, Q16 */
    sd = 2908 + (int32_t)(((int64_t)84155 * m) >> 16)
       - (int32_t)(((((int64_t)48342 * m) >> 16) * m) >> 16);               /* -0.73764 m^2 + 1.2841 m + 0.044372 */
    s->gc = mv_clamp(c, 0, 65536) >> 1;         /* Q15 */
    s->gs = mv_clamp(sd, 0, 65536) >> 1;
    /* high-pass alpha = 1 / (1 + w), w = 2 pi f / fs = 2 pi inc / 2^32: Q31 = 2^61 / (2^30 + w 2^30), w 2^30 =
     * 2 pi inc / 4; the denominator normalised to 32 bits for the division (f >= 1 Hz, as the JT) */
    w = inc < 97392u ? 97392u : inc;            /* 1 Hz */
    den = (1ull << 30) + ((uint64_t)w * 1686629713u >> 30);   /* 2 pi / 4 in Q30 = 1686629713 */
    {
        uint32_t sh = 0;
        while (den >> 32) { den >>= 1; sh++; }
        s->hpa = mv_udiv64_32(1ull << (61 - sh), (uint32_t)den);
    }
}

void mv_ssaw_render(mv_ssaw_t *s, int32_t *out, uint32_t n)
{
    /* The six side saws share one gain, so they are summed first and multiplied once; the seven are written out
     * (no inner loop). Same maths as the JT's sum of v_k x gain_k: the rounding moves from each term to the sum
     * (finer, not coarser: the sum is taken in Q24 from exact integer products). */
    uint32_t i, p0 = s->ph[0], p1 = s->ph[1], p2 = s->ph[2], p3 = s->ph[3], p4 = s->ph[4], p5 = s->ph[5], p6 = s->ph[6];
    const uint32_t i0 = s->inc[0], i1 = s->inc[1], i2 = s->inc[2], i3 = s->inc[3], i4 = s->inc[4], i5 = s->inc[5],
                   i6 = s->inc[6];
    const int32_t gc = s->gc, gs = s->gs;
    const uint32_t a = s->hpa;
    int32_t hx = s->hx, hy = s->hy;
    for (i = 0; i < n; i++) {
        /* naive saws 2 ph - 1 in Q15, read before the step: centre, and the six sides together (+-6 x 2^15) */
        int32_t c = (int32_t)(p3 >> 16) - 32768;
        int32_t sd = (int32_t)(p0 >> 16) + (int32_t)(p1 >> 16) + (int32_t)(p2 >> 16)
                   + (int32_t)(p4 >> 16) + (int32_t)(p5 >> 16) + (int32_t)(p6 >> 16) - 6 * 32768;
        int32_t sum, y;
        p0 += i0; p1 += i1; p2 += i2; p3 += i3; p4 += i4; p5 += i5; p6 += i6;
        sum = (int32_t)(((int64_t)c * gc + (int64_t)sd * gs) >> 6);   /* Q30 -> Q24 */
        /* one-pole high-pass: y = a (y + x - x_prev), Q24, rounded */
        hy = (int32_t)(((int64_t)a * ((int64_t)hy + sum - hx) + (1ll << 30)) >> 31);
        hx = sum;
        /* x 0.71 (Q15 23265) and Q24 -> Q15, then the +-3 bound */
        y = (int32_t)(((int64_t)hy * 23265) >> 24);
        out[i] = mv_clamp(y, -3 * 32768, 3 * 32768);
    }
    s->ph[0] = p0; s->ph[1] = p1; s->ph[2] = p2; s->ph[3] = p3; s->ph[4] = p4; s->ph[5] = p5; s->ph[6] = p6;
    s->hx = hx; s->hy = hy;
}

/* ==== Feedback oscillator (law and cost in mv_osc.h) ==== */
#define JF_INV_SPAN_Q16 ((int32_t)((65536ll * 65536ll) / MV_JFBK_MIX_SPAN_Q16))   /* 1 / 0.30 (compile time) */
static const uint8_t JF_FB_AXIS[8] = {0, 32, 48, 64, 80, 96, 112, 127};          /* feedback byte rows */
static const uint8_t JF_H_AXIS[9] = {0, 16, 32, 48, 64, 80, 96, 112, 127};       /* harmonics byte columns */

/* bilinear read of an 8 x 9 Q16 grid at (feedback, harmonics) bytes: the JT's, in knob bytes (byte / 127 on
 * both its axes and its inputs cancels). Control rate. */
static int32_t jf_bilinear(const int32_t *t, uint32_t fb, uint32_t h)
{
    uint32_t r = 0, c = 0;
    int32_t fr, fc, top, bot;
    while (r < 6u && fb > JF_FB_AXIS[r + 1]) r++;
    while (c < 7u && h > JF_H_AXIS[c + 1]) c++;
    fr = mv_clamp((int32_t)(((int32_t)fb - JF_FB_AXIS[r]) * 65536) / (JF_FB_AXIS[r + 1] - JF_FB_AXIS[r]), 0, 65536);
    fc = mv_clamp((int32_t)(((int32_t)h - JF_H_AXIS[c]) * 65536) / (JF_H_AXIS[c + 1] - JF_H_AXIS[c]), 0, 65536);
    top = t[r * 9 + c] + (int32_t)(((int64_t)(t[r * 9 + c + 1] - t[r * 9 + c]) * fc) >> 16);
    bot = t[(r + 1) * 9 + c] + (int32_t)(((int64_t)(t[(r + 1) * 9 + c + 1] - t[(r + 1) * 9 + c]) * fc) >> 16);
    return top + (int32_t)(((int64_t)(bot - top) * fr) >> 16);
}

/* 2^x for x = 0..~5.1 in Q16, result Q16: whole octaves by shift, the fraction from MV_EXP2N_Q31 (2^f =
 * 2 x 2^-(1 - f)), linear between its 1/256-octave entries */
static uint32_t jf_exp2_q16(int32_t x)
{
    uint32_t i = (uint32_t)x >> 16, k = 65536u - ((uint32_t)x & 0xFFFFu);   /* 1 - f, Q16 (1..65536) */
    uint32_t j = k >> 8, fr = k & 255u, v;
    v = j >= 256u ? MV_EXP2N_Q31[256] : MV_EXP2N_Q31[j] - (uint32_t)(((uint64_t)(MV_EXP2N_Q31[j] - MV_EXP2N_Q31[j + 1]) * fr) >> 8);
    return (v >> 14) << i;                      /* Q31 x 2 -> Q16 (>> 15, x 2), then the octaves */
}

void mv_jfbk_init(mv_jfbk_t *f, uint32_t seed)
{
    f->ph = f->psi = 0; f->inc = f->inner = 0; f->blocks = 65536;
    f->pull = 65536; f->chaos = f->wander = 0; f->saw_g = 46811; f->blk_g = 0;   /* 1 / 1.4 = kSawLevel */
    f->walk = 0; f->hx = f->hy = 0; f->hpa = MV_JFBK_HPA_Q31;
    f->rng = seed | 1u;
    f->key_h = f->key_f = f->key_c5 = 0xFF;
}

void mv_jfbk_key(mv_jfbk_t *f, uint32_t ph0)
{
    f->ph = ph0; f->psi = 0;                    /* inner falling edge on the master wrap */
    f->hx = f->hy = 0; f->walk = 0;
}

void mv_jfbk_set(mv_jfbk_t *f, uint32_t inc, uint32_t harmonics, uint32_t feedback)
{
    uint32_t c5;
    if (harmonics > 127u) harmonics = 127u;
    if (feedback > 127u) feedback = 127u;
    if (inc > 1932735283u) inc = 1932735283u;   /* the JT's 0.45 fs */
    c5 = inc >= MV_JFBK_SPLIT_INC;
    f->inc = inc;
    if (f->key_h != harmonics || f->key_f != feedback || f->key_c5 != c5) {   /* the law: only on a change */
        f->key_h = (uint8_t)harmonics; f->key_f = (uint8_t)feedback; f->key_c5 = (uint8_t)c5;
        if (!feedback) {                        /* zero feedback: the exact JP base, saw -> high-pass */
            f->blocks = 65536; f->pull = 65536; f->chaos = 0; f->wander = 0; f->saw_g = 46811; f->blk_g = 0;
        } else {
            int32_t l2 = jf_bilinear(c5 ? MV_JFBK_L2B_C5_Q16 : MV_JFBK_L2B_C3_Q16, feedback, harmonics), m;
            f->blocks = jf_exp2_q16(l2 < 0 ? 0 : l2);   /* never fewer than one block */
            f->pull = mv_clamp(jf_bilinear(MV_JFBK_PULL_Q16, feedback, harmonics), 0, 65536);
            f->chaos = jf_bilinear(MV_JFBK_CHAOS_Q16, feedback, harmonics);
            f->wander = (int32_t)(((int64_t)jf_bilinear(MV_JFBK_WANDER_Q16, feedback, harmonics)
                                   * MV_JFBK_WANDER_STEP_Q16) >> 16);
            /* mix = clamp((fb / 127 - start) / span, 0, 1): saw -> blocks */
            m = (int32_t)(((int64_t)((int32_t)(feedback * 65536u / 127u) - MV_JFBK_MIX_START_Q16)
                           * JF_INV_SPAN_Q16) >> 16);    /* x 1 / span: no 64-bit division on the chip */
            m = mv_clamp(m, 0, 65536);
            f->blk_g = m;
            f->saw_g = (int32_t)(((int64_t)(65536 - m) * 46811) >> 16);   /* (1 - mix) x 1 / 1.4 */
        }
    }
    {   /* the inner step follows the pitch every block, Nyquist-guarded */
        uint64_t in = ((uint64_t)f->blocks * inc) >> 16;
        f->inner = in > MV_JFBK_MAX_INNER ? MV_JFBK_MAX_INNER : (uint32_t)in;
    }
}

void mv_jfbk_render(mv_jfbk_t *f, int32_t *out, uint32_t n)
{
    uint32_t ph = f->ph, psi = f->psi, inc = f->inc, inner = f->inner, i;
    const int32_t pull = f->pull, chaos = f->chaos, wander = f->wander, sg = f->saw_g, bg = f->blk_g;
    const uint32_t a = f->hpa;
    int32_t hx = f->hx, hy = f->hy;
    for (i = 0; i < n; i++) {
        uint32_t old = ph;
        int32_t v, blk;
        ph += inc;
        if (ph < old) {                         /* master wrap: pull psi toward a whole inner cycle */
            int32_t e = (int32_t)psi;           /* signed distance to the nearest boundary, Q32 */
            psi -= (uint32_t)(int32_t)(((int64_t)pull * e) >> 16);
            if (wander > 0) {                   /* leaky random walk (drift cells only) */
                uint32_t x = f->rng;
                int64_t d;
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                f->rng = x;
                d = (((int64_t)wander * (int32_t)x) >> 15) - ((f->walk * MV_JFBK_WANDER_LEAK_Q16) >> 16);
                f->walk += d;
                psi += (uint32_t)d;
            }
        }
        /* inner rate, modulated by the output in the chaos cells (chaos Q16 x out Q15 = Q31) */
        psi += chaos ? inner + (uint32_t)(int32_t)(((int64_t)inner * (((int64_t)chaos * (hy >> 9)) >> 15)) >> 16) : inner;
        blk = psi < 0x80000000u ? -32768 : 32768;
        v = (int32_t)(((int64_t)sg * ((int32_t)(ph >> 16) - 32768) + (int64_t)bg * blk) >> 16);   /* Q15 */
        /* fixed-time one-pole high-pass, Q24, rounded */
        hy = (int32_t)(((int64_t)a * ((int64_t)hy + (v << 9) - hx) + (1ll << 30)) >> 31);
        hx = v << 9;
        out[i] = hy >> 9;
    }
    f->ph = ph; f->psi = psi; f->hx = hx; f->hy = hy;
}
