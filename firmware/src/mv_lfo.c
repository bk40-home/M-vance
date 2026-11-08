/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance LFOs: implementation (law, formats and cost in mv_lfo.h). */
#include "mv_lfo.h"
#include "mv_fixed.h"
#include "mv_tables.h"

void mv_lfo_init(mv_lfo_t *l, uint32_t seed)
{
    l->ph = 0; l->inc = 0; l->rng = seed ? seed : 0x9E3779B9u;
    l->held = 0; l->unit = 0; l->gain = MV_ENV_FULL_Q30; l->fade_c = 0;
    l->wave = MV_LFO_TRI; l->fading = 0;
}

void mv_lfo1_set(mv_lfo_t *l, uint32_t wave, uint32_t rate, uint32_t fade)
{
    l->wave = (uint8_t)(wave > MV_LFO_RND ? MV_LFO_RND : wave);
    l->inc = MV_LFO1_INC[rate > 127u ? 127u : rate];
    /* the JP fade: always restarts (fade 0 still runs one ~1 ms attack, like an instant envelope attack) */
    l->fade_c = MV_ENV_COEF[fade > 127u ? 127u : fade];
}

void mv_lfo2_set(mv_lfo_t *l, uint32_t wave, uint32_t rate)
{
    l->wave = (uint8_t)(wave > MV_LFO_RND ? MV_LFO_RND : wave);
    l->inc = MV_LFO2_INC[rate > 127u ? 127u : rate];
    l->fade_c = 0;                              /* LFO2 has no fade */
}

/* the JP's LFO1 SYNC list (JE lFO1Sync) as the length of one cycle in MIDI clock ticks, 24 a quarter note (whole
 * numbers for every triplet and dotted value; 4/4 measures) */
static const uint16_t SYNC_TICKS[23] = {
    0,                                          /* OFF */
    6, 8, 9, 12, 16, 18, 24,                    /* 1/16, 1/8T, 1/16D, 1/8, 1/4T, 1/8D, 1/4 */
    32, 36, 48, 64, 72, 96,                     /* 1/2T, 1/4D, 1/2, 1/1T, 1/2D, 1/1 */
    128, 144, 192,                              /* 2/1T, 1/1D, 2/1 */
    288, 384, 480, 576, 672, 768                /* 3 .. 8 MEASURES */
};

uint32_t mv_lfo_sync_inc(uint32_t sync, uint32_t beat)
{
    /* one cycle = ticks x beat / 24 samples = that / MV_CTL blocks: step = 2^32 x MV_CTL x 24 / (ticks x beat),
     * rounded. ticks x beat < 2^26 (768 x a 40 BPM beat, 66 150) and the quotient < 2^32: mv_udiv64_32's terms */
    uint32_t d;
    if (!sync || sync >= 23u || !beat) return 0;
    d = (uint32_t)SYNC_TICKS[sync] * beat;
    return mv_udiv64_32(((uint64_t)MV_CTL * 24u << 32) + d / 2u, d);
}

uint32_t mv_sync_ticks(uint32_t sync)
{
    return sync < 23u ? SYNC_TICKS[sync] : 0u;  /* (SYNC_TICKS[0] = 0: OFF) */
}

void mv_lfo_key(mv_lfo_t *l)
{
    if (l->fade_c) { l->gain = 0; l->fading = 1; }
}

int32_t mv_lfo_tick(mv_lfo_t *l)
{
    uint32_t p, old = l->ph;
    int32_t v;
    if (l->fading) {                            /* the attack generator: charge toward 1.25, stop at full */
        l->gain = MV_ENV_TARGET_Q30 - mv_mul_q31r(MV_ENV_TARGET_Q30 - l->gain, l->fade_c);
        if (l->gain >= MV_ENV_FULL_Q30) { l->gain = MV_ENV_FULL_Q30; l->fading = 0; }
    }
    p = l->ph = old + l->inc;                   /* wraps by itself */
    switch (l->wave) {
    case MV_LFO_TRI:                            /* 1 - 4|p - 1/2| */
        v = 32768 - (int32_t)((p >= 0x80000000u ? p - 0x80000000u : 0x80000000u - p) >> 15);
        break;
    case MV_LFO_SAW:                            /* 2p - 1 */
        v = (int32_t)(p >> 16) - 32768;
        break;
    case MV_LFO_SQR:
        v = p < 0x80000000u ? 32767 : -32767;
        break;
    default:                                    /* RANDOM: redrawn when the phase wraps (xorshift32) */
        if (p < old) {
            uint32_t x = l->rng;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            l->rng = x;
            l->held = (int32_t)x >> 16;
        }
        v = l->held;
        break;
    }
    l->unit = mv_clamp(v, -32767, 32767);
    return l->unit;
}
