/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance pitch: see mv_pitch.h. */
#include "mv_pitch.h"

uint32_t mv_pitch_inc(int32_t pitch_q8)
{
    uint32_t p = (uint32_t)mv_clamp(pitch_q8, 0, MV_PITCH_MAX_Q8);
    uint32_t n = p >> 8, oct = n / 12u, s = n - oct * 12u;        /* 32-bit division by a constant: fine */
    uint32_t top = (uint32_t)(((uint64_t)MV_INC_TOP[s] * MV_FINE_Q30[p & 255u]) >> 30);
    return top >> (11u - oct);                                    /* the table is octave 11 (notes 132..143) */
}

int32_t mv_shift_q8(uint32_t shift_byte)
{
    return ((int32_t)(shift_byte > 4u ? 4u : shift_byte) - 2) * 12 * 256;
}

int32_t mv_osc2_q8(uint32_t range_byte, uint32_t fine_byte)
{
    if (fine_byte > 100u) fine_byte = 100u;
    if (range_byte == 0u) return MV_WIDE_NEG_Q8[fine_byte];       /* -WIDE */
    if (range_byte >= 50u) return MV_WIDE_POS_Q8[fine_byte];      /* +WIDE */
    /* -24..+24 semitones, then +-50 cents: cents * 256 / 100 (exact for the JP's whole cents to 1/256 st) */
    return ((int32_t)range_byte - 25) * 256 + (((int32_t)fine_byte - 50) * 256) / 100;
}

int32_t mv_bend_q8(int32_t bend14, uint32_t up_semis, uint32_t down_semis)
{
    int32_t b = mv_clamp(bend14, -8192, 8191);
    uint32_t r = b >= 0 ? up_semis : down_semis;
    if (r > 24u) r = 24u;
    /* full bend = r semitones: +8191 reaches r * 256 * 8191/8192, -8192 exactly -r * 256 (as MIDI's own asymmetry) */
    return (b * (int32_t)(r * 256u)) / 8192;
}
