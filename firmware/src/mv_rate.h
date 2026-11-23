/* SPDX-License-Identifier: GPL-3.0-only */
/* JT-80x0's audio rate chosen at start (Kris 23 Nov 2026, the VIBE FM port P1: claude/jt80x0-vibe-port-proposal-2026-11-23.md).
 *
 * The FM-1 never defines MV_RATE_RT: its rate is 44.1 kHz, fixed, and its tables are firmware/gen/mv_tables.h's
 * constants, compiled exactly as before. The VIBE FM build defines MV_RATE_RT. Then every item that has the rate in it
 * (the tables gen_tables.py / gen_rv_tables.py make for 44.1 and 48 kHz, suffixed _44K / _48K, and the lengths and
 * steps the engine used to fold at compile time) is read through mv_rate, the set mv_rate_set() picked:
 *
 *     mv_rate_set(fs)      once, BEFORE mv_engine_init (the engine takes its steps and coefficients at init and on a
 *                          change of a byte): 1 = taken (44100, 48000), 0 = no table set for fs (mv_rate unchanged,
 *                          44.1 kHz until a call succeeds). The caller runs the engine at a rate it takes.
 *
 * The names stay the engine's (MV_FS, MV_ENV_COEF ...), so the engine's code reads the same on both builds. Pure C,
 * no hardware: the generated tables of both rates are in mv_tables.h / mv_rv_tables.h, and mv_rate.c (included last
 * by mv_dsp.c) gathers them. */
#pragma once
#include <stdint.h>

#ifdef MV_RATE_RT
#define MV_RATE_FS_MAX 48000u                   /* the highest rate with tables: the lines are sized for it */

typedef struct {
    uint32_t fs;                                /* audio frames per second */
    const uint32_t *inc_top, *env_coef, *lfo1_inc, *lfo2_inc;   /* mv_tables.h, this rate's */
    const uint32_t *flt_g, *noise_g, *porta, *tone_bass_a, *tone_treb_a;
    uint32_t flt_g_n;                           /* entries in flt_g */
    int32_t flt_ceil_lp_q8;                     /* the LP ceiling, 0.49 fs, as a cutoff code (Q8) */
    uint32_t flt_g_ceil_hb, jfbk_split, jfbk_hpa, xmod_step, glide_snap;
    uint32_t fx_mlen, fx_dlen;                  /* mv_fx's line lengths at this rate (the lines: MV_RATE_FS_MAX) */
    const uint32_t *rv_hidamp, *rv_lodamp, *rv_mlp, *rv_mhp;    /* mv_rv_tables.h, this rate's */
} mv_rate_t;

extern const mv_rate_t *mv_rate;                /* the set in use (mv_rate.c); 44.1 kHz until mv_rate_set */
int mv_rate_set(uint32_t fs);

#define MV_FS                (mv_rate->fs)
#define MV_INC_TOP           (mv_rate->inc_top)
#define MV_ENV_COEF          (mv_rate->env_coef)
#define MV_LFO1_INC          (mv_rate->lfo1_inc)
#define MV_LFO2_INC          (mv_rate->lfo2_inc)
#define MV_FLT_G_Q26         (mv_rate->flt_g)
#define MV_FLT_G_N           (mv_rate->flt_g_n)
#define MV_FLT_CEIL_LP_Q8    (mv_rate->flt_ceil_lp_q8)
#define MV_FLT_G_CEIL_HB_Q26 (mv_rate->flt_g_ceil_hb)
#define MV_JFBK_SPLIT_INC    (mv_rate->jfbk_split)
#define MV_JFBK_HPA_Q31      (mv_rate->jfbk_hpa)
#define MV_TONE_BASS_A_Q31   (mv_rate->tone_bass_a)
#define MV_TONE_TREB_A_Q31   (mv_rate->tone_treb_a)
#define MV_NOISE_G_Q26       (mv_rate->noise_g)
#define MV_PORTA_Q31         (mv_rate->porta)
#define XMOD_STEP_Q32        (mv_rate->xmod_step)   /* (mv_voice.c folds them from MV_FS on the FM-1) */
#define GLIDE_SNAP           (mv_rate->glide_snap)
#endif
