/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance fixed-point helpers. The FM-1's chip has no FPU and no libgcc: integers only, and no 64-bit division
 * (64-bit multiplies are fine). Formats used across the engine:
 *   Q15  audio and levels, +-32767 = +-1 (Dinghy's out[] is Q15)      Q30  envelope levels (1.25 fits below 2^31)
 *   Q31  per-block multipliers (< 1)                                  Q32  wrapping phase (one cycle = 2^32)
 *   Q8   pitch in semitones (MIDI note * 256)
 * The helpers are static inline: the hot loops call them per frame, and Dinghy's unity build sees them all. */
#pragma once
#include <stdint.h>

static inline int32_t mv_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t mv_sat16(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }

/* a * b >> n with a 64-bit product (no overflow of the intermediate), rounding toward minus infinity */
static inline int32_t mv_mul_q15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
static inline int32_t mv_mul_q30(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 30); }
static inline int32_t mv_mul_q31(int32_t a, uint32_t b) { return (int32_t)(((int64_t)a * (int64_t)b) >> 31); }
/* the same, rounded to nearest: for recursions (envelopes) where a floor every block would add up to a bias */
static inline int32_t mv_mul_q31r(int32_t a, uint32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b + 0x40000000) >> 31);
}

/* a x g (g Q31) for a recursive filter, with ERROR FEEDBACK: the fraction the shift drops is carried into the next
 * product (*e, 0 .. 2^31 - 1, one per filter state, starting at 0). A plain floor biases every pass by -1/2 LSB (a
 * DC blocker's loop gain turns that into a steady offset); plain rounding leaves a dead band (R x y rounds back to y
 * below 1 / (2 (1 - R)) LSB, so the state stops short of 0 for ever). Carried, it decays as the float filter does,
 * to exactly 0. One extra add a call. (mv_fx.c, mv_reverb.c) */
static inline int32_t mv_mul_q31ef(int32_t a, uint32_t g, uint32_t *e)
{
    int64_t acc = (int64_t)a * g + *e;
    int32_t r = (int32_t)(acc >> 31);
    *e = (uint32_t)(acc - ((int64_t)r << 31));
    return r;
}

/* linear interpolation between a and b, t in Q16 (0..65536) */
static inline int32_t mv_lerp_q16(int32_t a, int32_t b, uint32_t t)
{
    return a + (int32_t)(((int64_t)(b - a) * (int64_t)t) >> 16);
}

/* a value moving to its target in equal steps over a block of n frames: the step, so a control-rate value
 * (amp, cutoff) reaches the audio loop without zipper noise. n is a power of two (MV_CTL), so a shift. */
static inline int32_t mv_ramp_step(int32_t from, int32_t to, uint32_t log2n) { return (to - from) >> log2n; }

/* n / d for a 64-bit n, as two 16-bit quotient digits on the chip's 32 / 32 divide (Hacker's Delight, divlu: the
 * divisor normalised, each digit estimated from its top half and corrected at most twice). The chip has no 64-bit
 * divide and no libgcc to supply one. The quotient must fit 32 bits (n >> 32 < d) and d must not be 0. Exactly
 * floor(n / d), as the shift-and-subtract it replaced (tests/mv_fixed_test.c checks the two agree), at ~2 divides
 * and ~20 operations instead of 32 steps. Control rate. */
static inline uint32_t mv_udiv64_32(uint64_t n, uint32_t d)
{
    uint32_t u1 = (uint32_t)(n >> 32), u0 = (uint32_t)n, s = 0, vn1, vn0, un32, un10, un1, un0, q1, q0, rhat, un21;
    while (!(d & 0x80000000u)) { d <<= 1; s++; }   /* normalise (no clz on the chip without libgcc) */
    vn1 = d >> 16; vn0 = d & 0xFFFFu;
    un32 = s ? (u1 << s) | (u0 >> (32u - s)) : u1;
    un10 = u0 << s;
    un1 = un10 >> 16; un0 = un10 & 0xFFFFu;
    q1 = un32 / vn1; rhat = un32 - q1 * vn1;
    while (q1 >= 65536u || q1 * vn0 > (rhat << 16) + un1) {
        q1--; rhat += vn1;
        if (rhat >= 65536u) break;
    }
    un21 = (un32 << 16) + un1 - q1 * d;
    q0 = un21 / vn1; rhat = un21 - q0 * vn1;
    while (q0 >= 65536u || q0 * vn0 > (rhat << 16) + un0) {
        q0--; rhat += vn1;
        if (rhat >= 65536u) break;
    }
    return (q1 << 16) + q0;
}
