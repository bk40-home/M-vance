/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance reverb: the JT-8000's plate (Jteensy8000 src/core/dsp/PlateReverb.*, the Dattorro tank of hexefx / Piotr
 * Zapart, MIT, tuned by Kris), ported to fixed point. Kris 2 Nov 2026 (fm1-port/mvance-reverb-arp-proposal-2026-11-
 * 02.md): R1b the JT plate at about half its RAM, R2b after the FX chain and its limiter (the JT's order), R3a the
 * settings global (not in the JP patch: the JP-8080 has no reverb), R4a its pages after DELAY on the FX button.
 *
 * Flow (per frame, the JT's processBlock):
 *   mono = (L + R) / 2 -> 4 input allpass diffusers (142 107 379 277, gain 0.42425)
 *   -> two tank halves, each: + the other half's filtered output x decay -> a modulated allpass (1800 / 2656,
 *      +-8 samples of a 0.8 Hz triangle, opposite phases) -> a delay (3720 / 4217) -> LP (HI DAMP) -> HP (LO DAMP)
 *   -> wet L = d0[266] + d0[2974] - d1[1913], wet R = d1[353] + d1[3627] - d0[1228], x 0.3
 *   -> wet LP (LOWPASS) / HP (HIPASS) -> out = in + LEVEL x wet (the JT's soft clip after it is left out: it jumps
 *   from 1.0 to 0.5 as it starts; the output limiter mv_lim.h follows instead, Kris 2 Nov C2a)
 *   FREEZE: decay 1, the damping off, no input: the tail holds.
 * Laws: the JT's setters, by knob value (tools/gen_rv_tables.py -> firmware/gen/mv_rv_tables.h). LEVEL is linear;
 *   0 = off (the JT's auto-bypass), so with LEVEL 0 nothing is calculated.
 * Memory (the pool): the JT plate's pool is 23418 floats (91.5 KB), but 11025 of them are its predelay, which has
 *   no setter and stays at 0 (unused). Here: no predelay, the tank lines 16-bit (Q14, +-2.0: the tank peaks under 1.8 x
 *   its input) = 24.8 KB, the diffusers 32-bit = 3.6 KB: 28.4 KB in all. The lines' lengths are the JT's, so
 *   the sound is: only the storage is smaller.
 * Fixed point: Q20 inside (as mv_fx). Lines written truncated toward zero and the one-poles with error feedback, so
 *   a tail decays to exact silence (no limit cycle, no offset) and the engine can rest after it.
 * CPU: per frame 4 + 2 allpasses, 2 interpolated reads, 6 taps, 4-8 one-poles: on the ISR only while LEVEL > 0 and
 *   (with no voices) while its tail rings. */
#pragma once
#include <stdint.h>

#ifndef MV_RV_FS
#define MV_RV_FS 44100u                         /* = MV_FS; the compare builds it at 48000 */
#endif

enum { MV_RV_LEVEL, MV_RV_SIZE, MV_RV_HIDAMP, MV_RV_LODAMP, MV_RV_LOWPASS, MV_RV_HIPASS, MV_RV_FREEZE, MV_RV_N };

typedef struct {
    uint8_t key[MV_RV_N];                       /* the settings the caches were built from (mv_rv_set) */
    uint8_t on;                                 /* LEVEL > 0 */
    int32_t mix;                                /* LEVEL / 127 (Q16) */
    int32_t decay;                              /* SIZE (Q30), 1.0 when frozen */
    uint32_t hid, lod, mlp, mhp;                /* one-pole coefficients (Q31), 0 = bypass */
    int32_t in_g;                               /* input gain (Q16): 1, or 0 when frozen */
    uint32_t ph, inc;                           /* the tank's triangle LFO (Q32 cycles) */
    uint16_t dw[4], aw[2], tw[2];               /* write indices: diffusers, tank allpasses, tank delays */
    int32_t fb[2];                              /* the tank halves' filtered outputs, carried a frame (Q20) */
    int32_t tlp[2], thp[2], olp[2], ohp[2];     /* one-pole states: tank LP / HP, wet LP / HP (Q20) */
    uint32_t e[8];                              /* their error feedback */
    uint32_t quiet;                             /* frames in a row the wet has been silent */
} mv_rv_t;

void mv_rv_init(mv_rv_t *r);
/* control rate: the 7 settings (MV_RV_*, 0..127; FREEZE 0 / 1) into the caches, rebuilt only when they changed */
void mv_rv_set(mv_rv_t *r, const volatile uint8_t *v);
/* in place on the stereo out (L R L R, Q15), after the FX chain: out = in + LEVEL x wet. Nothing when off */
void mv_rv_process(mv_rv_t *r, int32_t *out, uint32_t n);
/* on, and its tail has not died away (the engine keeps rendering while it rings) */
int mv_rv_ringing(const mv_rv_t *r);
