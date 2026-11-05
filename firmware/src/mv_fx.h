/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance effects: the JP-8080's MULTI-FX and DELAY, and the bus limiter, ported to fixed point from the JT-8000's
 * FxChain (Jteensy8000 src/core/dsp/FxChain.*, the "JPFX" chain). Kris 1 Nov 2026: FX first, all 12 measured types,
 * DISTORTION from the JT drive, the 5 delay types (fm1-port/mvance-fx-arp-proposal-2026-11-01.md, 1a 2a 3a).
 *
 * Chain (the JT's order: drive -> tone -> modulation -> delay -> limiter), on the summed voice bus:
 *   bus (mono) -> [DISTORTION]  -> TONE (mv_voice.c) -> [MULTI-FX: mono in, stereo out] -> [DELAY: stereo]
 *              -> out L R -> the FX limiter (mv_voice.c: an mv_lim, always) -> REVERB -> the output limiter
 *   Each stage runs only when it does something; with none running the bus goes straight out, mono, exactly as
 *   before this module.
 *
 * MULTI-FX (byte 61 type 0..11, byte 62 level): the JP pair engine measured from the JE (chorus-fx/jp-noise-
 *   capture-analysis-2026-10-23.md), the JT's kJpModTypes:
 *     W = g [ x(t - c - d) - x(t - c + d) ] + g3 x(t - 2c),  d = D x LFO (tri / sine / unipolar tri / static)
 *     wet high-pass (1 or 2 one-poles), L = x + level^2 W, R = x - level^2 W   (mono wet, R inverted, as measured)
 *   DEEP PHASING adds feedback of W into the line. Level 0: the stage is off.
 * DISTORTION (byte 61 = 12, JP-8080 only, not measurable on the JE): the JT drive stage, its Amount = LEVEL / 127
 *   (Kris 2 Nov: LEVEL sets the drive; the JT importer had it fixed at 100 %); LEVEL 0 = off. PROVISIONAL.
 * DELAY (bytes 63 type, 64 time, 65 feedback, 66 level): the JT's two-line delay and its JP import law
 *   (JP8000SyxImporter.cpp, signed off 19 Sep): type -> the JT preset (PAN L->R, PAN R->L, PAN STEREO, MONO SHORT,
 *   MONO LONG; only PAN L->R confirmed on the JE), L time = time byte / 127 x the type's maximum (625 ms panning,
 *   156 ms short, 1250 ms mono long; JP manual), at least 10 ms, R = L x the preset's R/L ratio; feedback = byte /
 *   127 (at most 0.99), wet = byte / 127 added to the dry at unity: a SEND, as the JP's Multi-FX measured (Kris 2
 *   Nov, 1b; the JT crossfades, dry = 1 - wet). The tap moves to a new time on the JT's slewed glide (X2). Level 0:
 *   the stage is off.
 * LIMITER: no longer here (Kris 5 Nov, 1a: fm1-port/mvance-azs-crackle-diagnosis-2026-11-05.md). The JT's block-rate
 *   brick wall (ceiling 0.97, a clamp until its 128-frame block ended, then a snap down, release 0.029 a block)
 *   turned a hot bus (PATCH GAIN +12 dB on both DUAL parts) into hard clips and gain steps, the crackle. The engine
 *   now runs M-vance's look-ahead limiter (mv_lim.h: 16 frames late, gain ramped, never a clamp) on this output.
 *
 * Memory: the lines live in the platform's pool (.pool, zeroed at boot): the delay 2 x 16-bit lines of 1303 ms
 * (the longest tap: PAN L->R's R at 1302.1 ms) = 230 KB at 44.1 kHz, the Multi-FX one 32-bit line of 50 ms (9 KB).
 * One instance (the lines are this module's statics).
 * Fixed point: the chain works in Q20 (1.0 = 2^20: 11 bits of headroom over the Q15 bus); the delay lines hold Q13
 * (+-4.0: Kris 5 Nov, 3a; Q14's +-2.0 clipped what a hot bus wrote, before any limiter; 6 dB less resolution, still
 * ~84 dB under full scale).
 * CPU: on the ISR, per frame: Multi-FX 2-3 interpolated reads + 1-2 one-poles, delay 2 reads + 2 writes;
 * parameters are rebuilt only when their patch bytes change (mv_fx_set, once a control block). */
#pragma once
#include <stdint.h>

#ifndef MV_FX_FS
#define MV_FX_FS 44100u                         /* = MV_FS (checked in mv_fx.c); the compare builds it at 48000 */
#endif
#define MV_FX_MLEN (MV_FX_FS * 50u / 1000u + 2u)       /* Multi-FX line: 50 ms + 2 (the JT's kModLen) */
#define MV_FX_DLEN (MV_FX_FS * 1303u / 1000u + 3u)     /* delay lines: 1303 ms + the interpolation pair */

typedef struct {
    uint8_t key[6];                             /* bytes 61..66 the caches were built from (mv_fx_set) */
    uint8_t on_drive, on_mfx, on_dly;           /* the stages running */
    uint8_t m_shape, m_poles, m_type;           /* Multi-FX: LFO shape, high-pass poles, the type built */
    /* DISTORTION: the amount's constants (from the LEVEL byte) and the states (Q20) */
    int32_t dr_gain, dr_bias, dr_bout, dr_mk;   /* shaper gain, bias, f(bias), makeup (Q16, Q20, Q20, Q16) */
    int32_t dr_pdep, dr_mix;                    /* pre-shelf depth, post top-cut blend (Q16) */
    int32_t dr_pre, dr_post, dr_dcx, dr_dcy, dr_u1, dr_f1;
    uint32_t dr_e[3];                           /* error feedback of the pre / post one-poles and the DC blocker */
    /* MULTI-FX */
    uint32_t m_w;                               /* write index */
    uint32_t m_ph, m_inc;                       /* LFO phase / step (Q32 cycles) */
    int32_t m_c, m_d, m_c2, m_cap;              /* centre, depth, 2 x centre, the last readable tap (samples, Q16) */
    int32_t m_g, m_g3, m_fb, m_wet;             /* tap gain, 2c-tap gain, feedback, level^2 (Q16) */
    uint32_t m_hpg;                             /* wet high-pass one-pole G (Q31) */
    int32_t m_hs[2], m_lastw;                   /* high-pass states, the last W (feedback), Q20 */
    uint32_t m_he[2];                           /* high-pass error feedback */
    /* DELAY */
    uint32_t d_w;                               /* write index */
    int32_t d_tl, d_tr, d_cl, d_cr;             /* tap targets / where the taps are (samples, Q12) */
    int32_t d_fb, d_wet;                        /* Q16 */
    uint32_t d_mute;                            /* frames still to write without playing (a type change / start) */
    uint32_t d_slew;                            /* tap glide one-pole coefficient (Q31) */
    uint32_t quiet;                             /* frames in a row in and out silent (the tail has ended) */
} mv_fx_t;

void mv_fx_init(mv_fx_t *f);
/* control rate: the patch's FX bytes (b = the 248-byte patch) into the caches, rebuilt only when they changed */
void mv_fx_set(mv_fx_t *f, const uint8_t *b);
/* DISTORTION, in place on the mono bus (Q15), before TONE; nothing when it is off */
void mv_fx_drive(mv_fx_t *f, int32_t *bus, uint32_t n);
/* MULTI-FX, DELAY: the mono bus (Q15) added into out (L R L R, Q15, not limited: the engine's FX limiter follows).
 * With no stage on: the bus to both */
void mv_fx_out(mv_fx_t *f, const int32_t *bus, int32_t *out, uint32_t n);
/* a stage is on and its tail has not died away: the engine keeps rendering (with no voices) while it rings */
int mv_fx_ringing(const mv_fx_t *f);
