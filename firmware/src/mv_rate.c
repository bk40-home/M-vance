/* SPDX-License-Identifier: GPL-3.0-only */
/* JT-80x0's audio rate chosen at start: the sets (see mv_rate.h). MV_RATE_RT only (the VIBE FM build); included last
 * by mv_dsp.c, where mv_tables.h and mv_rv_tables.h have put both rates' tables in scope. */
#include "mv_rate.h"
#include "mv_build.h"                           /* MV_DLY_MS: the delay lines' length */

#ifdef MV_RATE_RT
/* one rate's set: K is the tables' suffix (44K, 48K), R the rate. The line lengths are mv_fx.h's formulas at R */
#define MV_RATE_ROW(K, R) { R, MV_INC_TOP_##K, MV_ENV_COEF_##K, MV_LFO1_INC_##K, MV_LFO2_INC_##K, \
    MV_FLT_G_Q26_##K, MV_NOISE_G_Q26_##K, MV_PORTA_Q31_##K, MV_TONE_BASS_A_Q31_##K, MV_TONE_TREB_A_Q31_##K, \
    MV_FLT_G_N_##K, MV_FLT_CEIL_LP_Q8_##K, MV_FLT_G_CEIL_HB_Q26_##K, MV_JFBK_SPLIT_INC_##K, MV_JFBK_HPA_Q31_##K, \
    MV_XMOD_STEP_Q32_##K, MV_GLIDE_SNAP_##K, (R) * 50u / 1000u + 2u, (R) * MV_DLY_MS / 1000u + 3u, \
    RV_HIDAMP_##K, RV_LODAMP_##K, RV_MLP_##K, RV_MHP_##K }

static const mv_rate_t MV_RATES[2] = { MV_RATE_ROW(44K, 44100u), MV_RATE_ROW(48K, 48000u) };
_Static_assert(48000u <= MV_RATE_FS_MAX, "mv_rate: a set above MV_RATE_FS_MAX would overrun mv_fx's lines");

const mv_rate_t *mv_rate = &MV_RATES[0];

int mv_rate_set(uint32_t fs)
{
    uint32_t k;
    for (k = 0; k < sizeof MV_RATES / sizeof MV_RATES[0]; k++)
        if (MV_RATES[k].fs == fs) { mv_rate = &MV_RATES[k]; return 1; }
    return 0;
}
#endif
