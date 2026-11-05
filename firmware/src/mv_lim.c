/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance output limiter: implementation (the method in mv_lim.h). */
#include "mv_lim.h"
#include "mv_fixed.h"

#define OL_ONE (1 << 30)                       /* gain 1.0 (Q30) */
#define OL_CEIL 31784                          /* 0.97 x 32767 (Q15) */
#define OL_REL 3893443                         /* the release a block: 2^30 x 16 / 4410 (0 -> 1 in 100 ms at 44.1 kHz) */
#define OL_SH 4                                /* log2(MV_LIM_N): the ramp's step */

void mv_lim_init(mv_lim_t *l)
{
    uint32_t i;
    for (i = 0; i < 2u * MV_LIM_N; i++) l->held[i] = 0;
    l->g = OL_ONE; l->tgt = OL_ONE; l->pending = 0;
}

void mv_lim_block(mv_lim_t *l, int32_t *out)
{
    uint32_t i, peak = 0, nz = 0;
    int32_t tgt, end, step, g;
    for (i = 0; i < 2u * MV_LIM_N; i++) {       /* the new block's peak (and whether it has any sound) */
        uint32_t a = (uint32_t)(out[i] < 0 ? -out[i] : out[i]);
        if (a > peak) peak = a;
        nz |= a;
    }
    tgt = peak > OL_CEIL ? (int32_t)mv_udiv64_32((uint64_t)OL_CEIL << 30, peak) : OL_ONE;
    /* the held block's end gain: within its own target and the next one's, released by at most a step */
    end = l->g + OL_REL;
    if (end > l->tgt) end = l->tgt;
    if (end > tgt) end = tgt;
    step = (end - l->g) >> OL_SH;
    g = l->g;
    for (i = 0; i < MV_LIM_N; i++) {            /* play the held block, the new one takes its place */
        int32_t nl = out[2u * i], nr = out[2u * i + 1u];
        if (g == OL_ONE && step == 0) {
            out[2u * i] = l->held[2u * i]; out[2u * i + 1u] = l->held[2u * i + 1u];
        } else {
            g = i + 1u == MV_LIM_N ? end : g + step;   /* (the last frame lands exactly on end) */
            out[2u * i] = (int32_t)(((int64_t)l->held[2u * i] * g) >> 30);
            out[2u * i + 1u] = (int32_t)(((int64_t)l->held[2u * i + 1u] * g) >> 30);
        }
        l->held[2u * i] = nl; l->held[2u * i + 1u] = nr;
    }
    l->g = end;
    l->tgt = tgt;
    l->pending = (uint8_t)(nz != 0u);
}
