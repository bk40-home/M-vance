/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance envelope: implementation (law, formats and cost in mv_env.h). */
#include "mv_env.h"
#include "mv_fixed.h"
#include "mv_tables.h"

#define SETTLE_Q30 107374                       /* 1e-4 of full: decay "arrived" at sustain (-80 dB) */

void mv_env_set(mv_env_t *e, uint32_t a, uint32_t d, uint32_t s, uint32_t r)
{
    e->ca = MV_ENV_COEF[a > 127u ? 127u : a];
    e->cd = MV_ENV_COEF[d > 127u ? 127u : d];
    e->cr = MV_ENV_COEF[r > 127u ? 127u : r];
    /* byte / 127 in Q30 as byte * (2^30 / 127): 32-bit, no division on the chip; 127 lands exactly on full */
    e->sus = s >= 127u ? MV_ENV_FULL_Q30 : (int32_t)(s * 8454660u);
}

void mv_env_on(mv_env_t *e)   { e->stage = MV_ENV_ATTACK; }
void mv_env_off(mv_env_t *e)  { if (e->stage != MV_ENV_IDLE) e->stage = MV_ENV_RELEASE; }
void mv_env_fade(mv_env_t *e) { if (e->stage != MV_ENV_IDLE) e->stage = MV_ENV_FADE; }
void mv_env_kill(mv_env_t *e) { e->stage = MV_ENV_IDLE; e->level = 0; }

int32_t mv_env_tick(mv_env_t *e)
{
    int32_t l = e->level, gap;
    switch (e->stage) {
    case MV_ENV_IDLE:
    case MV_ENV_SUSTAIN:                        /* holds; follows a sustain edit at once (as the JT) */
        if (e->stage == MV_ENV_SUSTAIN) l = e->sus;
        break;
    case MV_ENV_ATTACK:                         /* charge toward 1.25, stop at full */
        l = MV_ENV_TARGET_Q30 - mv_mul_q31r(MV_ENV_TARGET_Q30 - l, e->ca);
        if (l >= MV_ENV_FULL_Q30) { l = MV_ENV_FULL_Q30; e->stage = MV_ENV_DECAY; }
        break;
    case MV_ENV_DECAY:                          /* exponential to sustain */
        gap = mv_mul_q31r(l - e->sus, e->cd);
        if (gap < SETTLE_Q30 && gap > -SETTLE_Q30) { l = e->sus; e->stage = MV_ENV_SUSTAIN; }
        else l = e->sus + gap;
        break;
    case MV_ENV_RELEASE:                        /* exponential to 0, ends at -60 dB */
        l = mv_mul_q31r(l, e->cr);
        if (l < MV_ENV_OFF_Q30) { l = 0; e->stage = MV_ENV_IDLE; }
        break;
    default:                                    /* MV_ENV_FADE: the voice ramps to 0 across this block */
        l = 0; e->stage = MV_ENV_IDLE;
        break;
    }
    e->level = l;
    return l;
}
