/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM, P2b (Kris 24 Nov 2026): the 2x upsampler. A host at 88.2 / 96 kHz runs the engine at half its rate
 * (44.1 / 48: the rates it has tables for) and one stage doubles it; 176.4 / 192 kHz, a quarter and two stages.
 * Each stage is a half-band FIR (tools/gen_vibe_up.py -> jt_up_coef.h: 90 dB down from the first image, 0.0003 dB
 * ripple to 20 kHz at 44.1): its outputs alternate between the input itself, delayed (copied: the engine's samples pass
 * bit for bit) and the odd phase, P taps. Delay: P / 2 input frames a stage (0.73 ms at 44.1, stage 1).
 * The audio flow's alone; float, no libc. */
#pragma once
#include "jt_up_coef.h"

#define JT_HB_PMAX 64u
typedef struct {
    float b[2][2u * JT_HB_PMAX];                /* per channel: the last P inputs, twice (a window that never wraps) */
    uint32_t w;                                 /* where the newest is (0 .. P-1, and again at w + P) */
} jt_hb;

/* m input frames (il, ir) -> 2 m output frames (ol, or_): even = the input P / 2 frames back, odd = the P-tap sum */
static void jt_hb_run(jt_hb *s, const float *g, uint32_t p, const float *il, const float *ir, float *ol, float *or_,
                      uint32_t m)
{
    uint32_t i, t;
    for (i = 0; i < m; i++) {
        const float *hl, *hr;
        float al = 0.0f, ar = 0.0f;
        uint32_t w = s->w + 1u == p ? 0u : s->w + 1u;
        s->w = w;
        s->b[0][w] = s->b[0][w + p] = il[i];
        s->b[1][w] = s->b[1][w + p] = ir[i];
        hl = &s->b[0][w + 1u];                  /* hl[0] the oldest .. hl[p - 1] the newest */
        hr = &s->b[1][w + 1u];
        for (t = 0; t < p; t++) { al += g[t] * hl[t]; ar += g[t] * hr[t]; }
        ol[2u * i] = hl[p / 2u - 1u];           /* (between hl[p/2 - 1] and hl[p/2] lies the odd phase's centre) */
        or_[2u * i] = hr[p / 2u - 1u];
        /* the odd phase can overshoot between two samples (the engine's limiter holds its own at 0.97): held to +-1,
         * the SDK's limit. The even phase needs none: it is the engine's sample */
        ol[2u * i + 1u] = al > 1.0f ? 1.0f : al < -1.0f ? -1.0f : al;
        or_[2u * i + 1u] = ar > 1.0f ? 1.0f : ar < -1.0f ? -1.0f : ar;
    }
}
