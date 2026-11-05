/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of firmware/src/mv_pitch.c against double-precision maths (host only: the firmware has no float). */
#include <math.h>
#include <stdio.h>
#include "mv_pitch.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    int32_t p;
    double worst = 0, worst_at = 0;
    /* every 1/256 semitone from note 12 (16 Hz) to the top: error of the phase step, in cents. Low notes lose
     * bits to the octave shift, so the bound is relative to their tiny steps (<0.05 cent above note 24). */
    for (p = 12 * 256; p <= MV_PITCH_MAX_Q8; p++) {
        double f = 440.0 * pow(2.0, (p / 256.0 - 69.0) / 12.0);
        double want = f / MV_FS * 4294967296.0, got = mv_pitch_inc(p);
        double c = fabs(1200.0 * log2(got / want));
        if (p >= 24 * 256 && c > worst) { worst = c; worst_at = p / 256.0; }
        if (c > (p < 24 * 256 ? 0.5 : 0.05)) { CHECK(0, "note %.3f off by %.3f cents", p / 256.0, c); break; }
    }
    printf("pitch: worst %.4f cents (note %.2f), notes 24..143\n", worst, worst_at);
    CHECK(mv_pitch_inc(-500) == mv_pitch_inc(0) && mv_pitch_inc(1 << 20) == mv_pitch_inc(MV_PITCH_MAX_Q8), "clamps");
    CHECK(mv_shift_q8(0) == -24 * 256 && mv_shift_q8(2) == 0 && mv_shift_q8(4) == 24 * 256 && mv_shift_q8(9) == 24 * 256,
          "OSC SHIFT");
    CHECK(mv_osc2_q8(25, 50) == 0, "OSC2 unison");
    CHECK(mv_osc2_q8(1, 50) == -24 * 256 && mv_osc2_q8(49, 50) == 24 * 256, "OSC2 range ends");
    CHECK(mv_osc2_q8(37, 50) == 12 * 256, "OSC2 +12");
    CHECK(mv_osc2_q8(25, 100) == 128 && mv_osc2_q8(25, 0) == -128, "OSC2 fine +-50 cents = +-128 Q8");
    CHECK(mv_osc2_q8(25, 75) == 64, "OSC2 +25 cents");
    CHECK(mv_osc2_q8(50, 100) == 48 * 256 && mv_osc2_q8(0, 0) == -48 * 256, "WIDE reaches +-4 octaves");
    CHECK(mv_osc2_q8(50, 0) == 0 && mv_osc2_q8(0, 100) == 0, "WIDE centre ends at 0");
    CHECK(mv_bend_q8(0, 2, 2) == 0, "no bend");
    CHECK(mv_bend_q8(-8192, 2, 12) == -12 * 256, "full down 12");
    CHECK(mv_bend_q8(8191, 2, 12) == (8191 * 512) / 8192, "full up 2");
    CHECK(mv_bend_q8(4096, 24, 0) == 12 * 256, "half up of 24");
    CHECK(mv_bend_q8(-4096, 24, 0) == 0, "down range 0");
    printf("mv_pitch_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
