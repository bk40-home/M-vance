/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance output limiter (Kris 2 Nov 2026, C1: fm1-port/mvance-crackle-diagnosis-2026-11-02.md). The last stage of the
 * engine, after the reverb, always on: dense chords, resonance and patch gain took the bus to 1.5 .. 3 x full scale,
 * which the platform hard-clipped (the crackles). Now nothing passes 0.97 of full scale, and below it nothing changes.
 *
 * A look-ahead peak limiter on the engine's 16-frame blocks (MV_CTL):
 *   each block's target gain = min(1, ceiling / its peak);
 *   a block plays one block LATE (16 frames, 0.36 ms), with its gain ramped linearly from where the last one ended to
 *     min(its own target, the next block's target, the last gain + the release step):
 *   both ends are within its own target, so no sample of it passes the ceiling, and the gain never steps (a ramp
 *   over 16 frames). Release: linear, from 0 to 1 in about 100 ms (when the level falls, the gain comes back up).
 *   At gain 1 and no ramp, a block is only copied (the usual case: 32 words).
 * Ceiling: 0.97 of the Q15 full scale (the JT's kLimCeiling); MASTER and the platform's clamp come after it.
 * RAM: one held block (2 x 16 int32). ISR only. */
#pragma once
#include <stdint.h>

#define MV_LIM_N 16u                            /* the block (= MV_CTL; a power of two) */

typedef struct {
    int32_t held[2u * MV_LIM_N];                /* the block waiting to play (L R, Q15) */
    int32_t g;                                  /* the gain the last block played ended at (Q30) */
    int32_t tgt;                                /* the held block's own target gain (Q30) */
    uint8_t pending;                            /* the held block holds sound (play it even when the engine rests) */
} mv_lim_t;

void mv_lim_init(mv_lim_t *l);
/* out: a new block of MV_LIM_N stereo frames (L R, Q15; zeros when silent). On return it holds the block before
 * (limited) and the new one waits for the next call */
void mv_lim_block(mv_lim_t *l, int32_t *out);
/* the held block still has sound in it: the engine passes a silent block to let it out */
static inline int mv_lim_pending(const mv_lim_t *l) { return l->pending; }
