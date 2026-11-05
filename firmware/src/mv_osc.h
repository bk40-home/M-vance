/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance oscillators, fixed point: the JP-8080's closed-form waves, ported from the JT-8000's OscCore
 * (Kris Bishop, MIT) and checked against it sample by sample (tools/compare/cmp_osc.cpp).
 *
 *   SQR (PWM)     band-limited pulse, JP width law: width = 50 % + 47.2 % * CTRL1 / 127 (95 % cap), the
 *                 DC of the width removed (the JP's AC-coupled pulse)
 *   SAW (JvSAW)   measured SHAPE law: y = G * (2ph - 1) - beta(k) * 4pi^2/3 * G * ph (ph - 1/2)(ph - 1)
 *   TRIANGLE MOD  measured law: y = wrap(g(k) * tri(ph)), four wrap jumps a cycle, each band-limited
 *   TRIANGLE (JvTRI)  the JT's measured SHAPE morph table, reduced to 60 KB (firmware/gen/mv_jvtri.h): mip level
 *                 by pitch, linear in the cycle, linear between the two SHAPE frames around k (the JT's
 *                 renderTable), on the JT's 33-frame axis
 * Band limiting is polyBLEP (one 32-bit division per edge, only within a frame of it), as on the JT.
 *
 * Phase is Q32 (one cycle = 2^32, wraps by itself); output Q15. A wave's shape is set once a block
 * (mv_osc_shape); render writes n frames into out[] (=, not +=). */
#pragma once
#include <stdint.h>
#include "mv_fixed.h"

typedef struct {
    uint32_t ph;                                /* phase, Q32 */
    uint32_t inc;                               /* phase step per frame, Q32 (mv_pitch_inc) */
    /* per-block shape state, set by mv_osc_shape for the wave in use */
    uint32_t pw;                                /* SQR: width as a phase, Q32 */
    int32_t dc;                                 /* SQR: the width's DC (Q15), subtracted */
    int32_t saw_c;                              /* JvSAW: beta * 4pi^2/3 * G, Q16 */
    int32_t tri_g;                              /* Tri-mod: gain g, Q16 */
    uint32_t tri_p[4];                          /* Tri-mod: the four wrap phases, Q32 */
    const int16_t *ta, *tb;                     /* JvTRI: this level of the two frames around SHAPE */
    int32_t tm;                                 /* JvTRI: position between them, Q14 */
    uint32_t tlog2;                             /* JvTRI: log2 of the level's length */
} mv_osc_t;

enum { MV_W_SQR, MV_W_SAW, MV_W_TRIMOD, MV_W_JVTRI };   /* the waves mv_osc_render draws */

/* shape for a wave from its CTRL1 byte (0..127) plus a signed modulation in Q15 of the knob's range
 * (LFO1 via CTRL2: 0 = none). Control rate; for JvTRI after inc is set (the mip level follows the pitch). */
void mv_osc_shape(mv_osc_t *o, uint32_t wave, uint32_t ctrl1, int32_t mod_q15);
void mv_osc_render(mv_osc_t *o, uint32_t wave, int32_t *out, uint32_t n);
/* X-MOD carrier: the step of frame i is inc + dinc[i] (any sign: through-zero FM runs the phase backwards) */
void mv_osc_render_fm(mv_osc_t *o, uint32_t wave, int32_t *out, uint32_t n, const int32_t *dinc);
/* hard-sync slave: frame i restarts at phase reset[i] (the master wrapped inside it), MV_NO_RESET = run on.
 * The reset itself is not band-limited, as on the JT (JT_SYNC_ANTIALIAS 0). */
#define MV_NO_RESET 0xFFFFFFFFu
void mv_osc_render_sync(mv_osc_t *o, uint32_t wave, int32_t *out, uint32_t n, const uint32_t *reset);

/* ---- Super Saw (OSC1 wave 0): the JT-8000's SupersawOsc (Szabo 2010 + the JP mix law measured on the JE-8086),
 * fixed point and checked against it sample by sample (tools/compare/cmp_ssaw.cpp).
 *   seven NAIVE saws (their aliasing is the JP's "air", Szabo 3.4), at f x (1 + offset_i x depth(DETUNE)),
 *   offsets Szabo table 1, depth from the 256-entry detune LUT (MV_SSAW_DETUNE_Q24);
 *   gains: centre -0.55366 m + 0.99785, EACH side -0.73764 m^2 + 1.2841 m + 0.044372 (m = MIX), no compensation;
 *   a one-pole high-pass at the note's own frequency (Szabo 3.3), then x 0.71 and a +-3 safety bound.
 * Random phases on every key (required: the per-note variation is the sound). DETUNE = CTRL1, MIX = CTRL2.
 * Cost a frame: 7 phase adds, 6 adds, 2 multiplies for the gains, then the high-pass (2). Sum and high-pass Q24. */
typedef struct {
    uint32_t ph[7], inc[7];                     /* phases and steps, Q32 */
    int32_t gc, gs;                             /* centre / side gain, Q15 */
    uint32_t hpa;                               /* high-pass alpha = 1 / (1 + 2 pi f / fs), Q31 */
    int32_t hx, hy;                             /* high-pass state: last input and output, Q24 */
    uint32_t rng;                               /* xorshift32 for the key phases */
    uint32_t key_inc;                           /* the step / DETUNE / MIX the law was computed for: set skips */
    uint8_t key_det, key_mix, key_ok;           /* the work when none of them moved (key_ok 0: never computed) */
} mv_ssaw_t;

void mv_ssaw_key(mv_ssaw_t *s);                 /* key down: seven random phases */
/* per block: the centre step (mv_pitch_inc) and the DETUNE / MIX bytes */
void mv_ssaw_set(mv_ssaw_t *s, uint32_t inc, uint32_t detune, uint32_t mix);
void mv_ssaw_render(mv_ssaw_t *s, int32_t *out, uint32_t n);   /* writes n frames, Q15 (up to +-3 x) */

/* ---- Feedback oscillator (OSC1 wave 3, "JFBK"): the JT-8000's JpFeedbackOsc (JP-flavoured, sign-off option 1,
 * 2026-10-23), fixed point, checked against it sample by sample (tools/compare/cmp_jfbk.cpp).
 *   master saw (naive) at the note; an inner two-level "block" oscillator at N x the note, N from the JE-8086
 *   grids by FEEDBACK x HARMONICS (C3 map below F#4, C5 map at or above: nearest anchor, D3a), pulled toward the
 *   master at each master wrap (lock), with a leaky random walk (wander) and output-driven rate (chaos) in the
 *   cells where the JE never settles; saw -> blocks crossfade over FEEDBACK 10..40 %; a fixed 1.92 ms one-pole
 *   high-pass (the exact JP base at zero feedback). HARMONICS = CTRL1, FEEDBACK = CTRL2.
 * The law (five bilinear reads, one exp2) runs only when FEEDBACK, HARMONICS or the anchor changes. Per frame:
 * two phases, a compare, the high-pass, a few multiply-adds. */
typedef struct {
    uint32_t ph, psi;                           /* master and inner phase, Q32 */
    uint32_t inc, inner, blocks;                /* steps per frame (Q32); inner cycles per master cycle (Q16) */
    int32_t pull, chaos, wander, saw_g, blk_g;  /* law, Q16 (wander: cycles per master wrap) */
    int64_t walk;                               /* accumulated wander, Q32 cycles (leaky, may exceed a cycle) */
    int32_t hx, hy;                             /* high-pass state, Q24 */
    uint32_t hpa;                               /* high-pass a (Q31): MV_JFBK_HPA_Q31 at this rate */
    uint32_t rng;                               /* xorshift32 for the wander */
    uint8_t key_h, key_f, key_c5;               /* what the law was computed for (0xFF: not yet) */
} mv_jfbk_t;

void mv_jfbk_init(mv_jfbk_t *f, uint32_t seed);
void mv_jfbk_key(mv_jfbk_t *f, uint32_t ph0);  /* key down: master at ph0, inner aligned, high-pass cleared */
void mv_jfbk_set(mv_jfbk_t *f, uint32_t inc, uint32_t harmonics, uint32_t feedback);   /* per block */
void mv_jfbk_render(mv_jfbk_t *f, int32_t *out, uint32_t n);   /* writes n frames, Q15 */
