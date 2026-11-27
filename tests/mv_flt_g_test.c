/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the JP filter's cutoff -> g (firmware/src/mv_filter.c mv_flt_cutoff, Kris 25 Nov 2026: the P1 finding).
 * Every cutoff code from the 5 Hz floor to the ceiling, LP and HP/BP: the pole frequency atan(g) fs / pi within
 * gen_tables.py's own limits of the ideal tan curve (0.1 cent below 16 kHz, 1 below 20 kHz, 10 to the ceiling), and g
 * never falling as the code rises. Built twice by run_unit.sh: as the FM-1 (44.1 kHz) and -DMV_RATE_RT at 48 kHz (the
 * VIBE's table, whose last entry is clamped). Host only (double to measure). */
#include <math.h>
#include <stdio.h>
#include "mv_patch.c"
#include "mv_pitch.c"
#include "mv_osc.c"
#include "mv_env.c"
#include "mv_lfo.c"
#include "mv_filter.c"
#ifdef MV_RATE_RT
#include "mv_fx.c"
#include "mv_reverb.c"
#include "mv_rate.c"
#define FS_TEST 48000u
#define G_N MV_FLT_G_N
#else
#define FS_TEST 44100u
#define G_N (sizeof MV_FLT_G_Q26 / sizeof MV_FLT_G_Q26[0])
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    const double PI = 3.141592653589793, fs = FS_TEST, spo = MV_FLT_CODE_PER_OCT_Q16 / 65536.0;
    double f64, worst[3] = {0, 0, 0}, wf[3] = {0, 0, 0}, floor_c = 0;
    int mode;
#ifdef MV_RATE_RT
    CHECK(mv_rate_set(FS_TEST) == 1, "rate %u", FS_TEST);
#endif
    f64 = atan(MV_FLT_G_Q26[(64 * 256 - MV_FLT_G_CODE0_Q8) / 64] / 67108864.0) * fs / PI;   /* code 64's frequency */
    for (mode = 0; mode < 2; mode++) {
        mv_flt_t f = {0};
        int32_t c, prev = -1, falls = 0;
        mv_flt_set(&f, mode ? MV_FLT_HP12 : MV_FLT_LP24, 0, 64);
        for (c = MV_FLT_FLOOR_Q8; c <= f.ceil_q8; c++) {
            double fc = f64 * pow(2.0, (c / 256.0 - 64.0) / spo), fg, ce;
            int k;
            mv_flt_cutoff(&f, c);
            if (c == f.ceil_q8) fc = atan(f.ceil_g / 67108864.0) * fs / PI;   /* (the ceiling's own, exact) */
            fg = f.g > 0 ? atan(f.g / 67108864.0) * fs / PI : 1e-9;
            ce = fabs(1200.0 * log2(fg / fc));
            k = fc < 16000.0 ? 0 : fc < 20000.0 ? 1 : 2;
            if (fc < 20.0) { if (ce > floor_c) floor_c = ce; continue; }   /* (5..20 Hz: g is a few hundred LSB, its
                                                   rounding alone is ~0.2 cent – inaudible, reported apart) */
            if (ce > worst[k]) { worst[k] = ce; wf[k] = fc; }
            if (f.g < prev) falls++;
            prev = f.g;
        }
        CHECK(falls == 0, "%s: g falls %d times as the cutoff rises", mode ? "HP/BP" : "LP", falls);
        CHECK(f.g > 0 && (uint32_t)f.g == f.ceil_g, "%s: at the ceiling, exactly its g", mode ? "HP/BP" : "LP");
    }
    CHECK(worst[0] <= 0.1, "20 Hz..16 kHz: %.3f cents at %.0f Hz", worst[0], wf[0]);
    CHECK(floor_c <= 0.5, "5..20 Hz: %.3f cents", floor_c);
    CHECK(worst[1] <= 1.0, "16..20 kHz: %.3f cents", worst[1]);
    CHECK(worst[2] <= 10.0, "20 kHz..ceiling: %.3f cents", worst[2]);
    printf("mv_flt_g_test %u Hz: pole within %.3f / %.3f / %.3f cents (20 Hz-16k / -20k / -ceiling; 5-20 Hz %.3f); %d checks, %d failed\n",
           FS_TEST, worst[0], worst[1], worst[2], floor_c, checks, fails);
    (void)G_N;
    return fails != 0;
}
