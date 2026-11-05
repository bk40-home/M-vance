/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance LFOs, fixed point: the JT-8000's block-rate Lfo (Kris Bishop, MIT) with the JP rate tables and the
 * JP LFO1 FADE.
 *
 *   waves     TRIANGLE 1 - 4|p - 1/2|, SAWTOOTH 2p - 1, SQUARE p < 1/2 ? +1 : -1, RANDOM held and redrawn at
 *             each wrap (the JP's LFO1 list, byte 16; the JT formulas and xorshift32)
 *   rate      MV_LFO1_INC / MV_LFO2_INC[byte] (firmware/gen/mv_tables.h, from the JT's JpLfoRate)
 *   sync      LFO1 only (Kris 5 Nov): with the part's LFO1 SYNC on (mv_perf.h MV_PF_SYNCUP / LO, the JP's list) one
 *             cycle lasts the note value at the tempo (the JT's TempoClock law: freqForMode), free running, and the
 *             RATE byte is ignored, as on the JP. mv_lfo_sync_inc gives the step (main loop: a divide); the engine
 *             puts it in place of the table's (mv_voice.c). LFO2 has no sync on the JP.
 *   fade      LFO1 only: the envelope ATTACK generator (mv_env law) on the depth, restarted by a key down.
 *             The unit wave and the gain are kept apart, as the JT does: the depth law (JpModLaw) is applied
 *             to depth x gain, not to the wave.
 *
 * Runs once per control block (MV_CTL frames): one add and a few compares; the caller ticks an LFO only when
 * one of its depths is non-zero (do not calculate if not required). Free running and shared by all voices. */
#pragma once
#include <stdint.h>

enum { MV_LFO_TRI, MV_LFO_SAW, MV_LFO_SQR, MV_LFO_RND };

typedef struct {
    uint32_t ph;                                /* phase, Q32 */
    uint32_t inc;                               /* phase step per control block, Q32 */
    uint32_t rng;                               /* xorshift32 state (never 0) */
    int32_t held;                               /* RANDOM: the held value, Q15 */
    int32_t unit;                               /* the last block's wave, Q15 (-32767..32767) */
    int32_t gain;                               /* fade gain, Q30 (full = 2^30) */
    uint32_t fade_c;                            /* fade per-block multiplier, Q31 (MV_ENV_COEF) */
    uint8_t wave, fading;
} mv_lfo_t;

void mv_lfo_init(mv_lfo_t *l, uint32_t seed);
void mv_lfo1_set(mv_lfo_t *l, uint32_t wave, uint32_t rate, uint32_t fade);   /* bytes 16, 17, 18 */
void mv_lfo2_set(mv_lfo_t *l, uint32_t wave, uint32_t rate);                  /* byte 19; wave: see the plan */
void mv_lfo_key(mv_lfo_t *l);                   /* key down: restart the fade (LFO1) from 0 */
/* main loop: the per-block phase step (Q32) of an LFO1 synced to note value sync (1..22, mv_perf.h MV_LFO1_SYNC_N)
 * at a quarter note of beat samples (the engine's arp_beat: TEMPO or the MIDI clock); 0 for OFF or no tempo */
uint32_t mv_lfo_sync_inc(uint32_t sync, uint32_t beat);
int32_t mv_lfo_tick(mv_lfo_t *l);               /* advance one control block, returns the unit wave (Q15) */
