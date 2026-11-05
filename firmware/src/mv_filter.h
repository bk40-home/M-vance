/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance filter, fixed point: the JP-8000 filter as fitted to the JE-8086 by the JT-8000's JpFilter (Kris
 * Bishop, MIT), all six modes, checked against it sample by sample (tools/compare/cmp_filter.cpp).
 *
 *   section   Zavalishin TPT state-variable filter, R = 1/2Q:  hp = d x - (a d) s1 - d s2,  bp = g hp + s1,
 *             lp = g bp + s2  (a = 2R + g, d = 1 / (1 + 2Rg + g^2), both per block, so no divide per frame)
 *   -12       one section, output x (R/R0)^0.1
 *   -24       two identical sections in cascade, output x R/R0 x the R5a level compensation. The R/R0 trim is
 *             applied BETWEEN the sections (the filter is linear, so the result is the same): the first
 *             section's resonant peak (up to ~590 x) is brought back to ~1 x before the second rings, so the
 *             states never need more than 13 bits of headroom.
 *   cutoff    a knob CODE (the JP law is exponential: one code = 1/11.96 octave), so modulation in octaves adds
 *             codes and needs no exp; g = tan(pi fc / fs) from MV_FLT_G_Q26 every quarter code. Clamped to
 *             5 Hz .. 0.49 fs (LP) or 12.4 kHz (HP/BP, where the JE's HPF saturates), as the JT.
 *   output    linear to 4 x full scale, then a tanh shoulder into the 8 x knee (JT FilterSection H2b), so an
 *             ordinary signal is untouched and only a resonant overload is bent.
 *
 * Above g = 1 (~11 kHz) the section runs in its direct form (bp = g d (x - s2) + d s1, lp = s2 + g^2 d (x - s2)
 * + g d s1, hp = x - 2R bp - lp): the same filter, but the hp rounding is no longer multiplied by g twice
 * (g reaches 16 at code 127, so that form lost ~48 dB at the top). Below it the usual form is the precise one.
 * Integrators keep the bits their g products shift out and add them back next frame (first-order error
 * feedback): their rounding noise is pushed away from DC, where a low cutoff's 1/g gain would lift it to the
 * signal's level (5..20 Hz cutoffs: from -10 dB to < -60 dB re the float filter). 2 adds a section.
 * Formats: states Q18 (+-8192 x full scale), g Q26 (up to ~32 at 0.49 fs), d and a d Q30 (both <= 1), trims
 * Q30. Audio in and out Q15 in int32 (the output may exceed +-1, up to the 8 x knee; the voice gain follows).
 * Cost per frame: 5 multiplies a section (64-bit products), the -24 modes 10 + 1. Per block: one table
 * interpolation and one 32-step division. Float JT's 1e-7 dither is a denormal guard: not needed here. */
#pragma once
#include <stdint.h>

enum { MV_FLT_LP24, MV_FLT_LP12, MV_FLT_HP24, MV_FLT_HP12, MV_FLT_BP24, MV_FLT_BP12 };   /* the JT's order */

typedef struct {
    int32_t s1a, s2a, s1b, s2b;                 /* section states (BP, LP), Q18; b = second section (-24) */
    uint32_t eba, ela, ebb, elb;                /* the integrators' remainders (error feedback, see the .c) */
    int32_t g, d, ad;                           /* per-block coefficients: g Q26, d and a d Q30 */
    int32_t gd, ggd, r2;                        /* g > 1 only: g d, g^2 d and 2R, all Q30 (see the .c) */
    uint8_t hi;                                 /* 1 when g > 1 (cutoff above ~11 kHz): the direct form runs */
    uint32_t r;                                 /* damping R, Q31 (from the resonance byte) */
    int32_t mid, out;                           /* trims, Q30: between the sections (-24) and at the output */
    int32_t ceil_q8;                            /* the mode's cutoff ceiling, as a code (Q8) */
    uint32_t ceil_g;                            /* g exactly at that ceiling (Q26) */
    uint8_t mode;
} mv_flt_t;

/* mode, resonance and cutoff KNOB bytes (the R5a level compensation follows the knob, not the modulation).
 * Control plane: on a patch edit. Does not clear the states (a mode change should: mv_flt_reset). */
void mv_flt_set(mv_flt_t *f, uint32_t mode, uint32_t reso, uint32_t cut);
void mv_flt_reset(mv_flt_t *f);
/* the modulated cutoff for the next block, as a code in Q8 (knob byte * 256 + modulation in codes) */
void mv_flt_cutoff(mv_flt_t *f, int32_t code_q8);
/* one -12 low-pass section with given g (Q26) and R (Q31), no trims: the NOISE wave's own filter. Per block */
void mv_flt_lp(mv_flt_t *f, uint32_t g_q26, uint32_t r_q31);
/* filter n frames in place (Q15 in, Q15 out up to the 8 x knee) */
void mv_flt_render(mv_flt_t *f, int32_t *buf, uint32_t n);
