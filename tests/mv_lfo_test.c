/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of firmware/src/mv_lfo.c: the waves against the JT's float formulas, the rates against
 * docs/ref/jt_laws.json (via the tables), the LFO1 fade timing and the LFO1 SYNC periods. Host only: the firmware has no float. */
#include <math.h>
#include <stdio.h>
#include "mv_lfo.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

static const double BLK = (double)MV_CTL / MV_FS;

int main(void)
{
    int w, n;
    double worst = 0;
    for (w = MV_LFO_TRI; w <= MV_LFO_SQR; w++) {        /* the waves, block by block, vs the JT formulas */
        mv_lfo_t l;
        mv_lfo_init(&l, 1);
        mv_lfo1_set(&l, (uint32_t)w, 100, 0);
        for (n = 0; n < 20000; n++) {
            double p, want, e;
            mv_lfo_tick(&l);
            p = l.ph / 4294967296.0;
            want = w == MV_LFO_TRI ? 1 - 4 * fabs(p - 0.5) : w == MV_LFO_SAW ? 2 * p - 1 : (p < 0.5 ? 1 : -1);
            e = fabs(l.unit / 32768.0 - want);
            if (e > worst) worst = e;
        }
    }
    printf("lfo: waves within %.1e of the JT formulas\n", worst);
    CHECK(worst < 1.0 / 16384, "wave precision %.2e", worst);
    {   /* RANDOM: held between wraps, changes at wraps, spans both signs */
        mv_lfo_t l;
        int changes = 0, wraps = 0, pos = 0, neg = 0, held_ok = 1;
        mv_lfo_init(&l, 12345);
        mv_lfo1_set(&l, MV_LFO_RND, 127, 0);
        for (n = 0; n < 20000; n++) {
            uint32_t old = l.ph;
            int32_t before = l.unit;
            mv_lfo_tick(&l);
            if (l.ph < old) { wraps++; if (l.unit != before) changes++; }
            else if (n && l.unit != before) held_ok = 0;
            if (l.unit > 8192) pos++;
            if (l.unit < -8192) neg++;
        }
        CHECK(held_ok && wraps > 10 && changes >= wraps - 1 && pos && neg, "random: %d wraps %d changes", wraps, changes);
    }
    {   /* rates: the cycle length in blocks matches the table's Hz (LFO1 and LFO2, every byte) */
        int b;
        double we = 0;
        for (b = 0; b < 128; b++) {
            double hz1 = MV_LFO1_INC[b] / 4294967296.0 / BLK, hz2 = MV_LFO2_INC[b] / 4294967296.0 / BLK;
            CHECK(hz1 > 0 && hz2 > 0 && (b == 0 || (MV_LFO1_INC[b] >= MV_LFO1_INC[b - 1] &&
                  MV_LFO2_INC[b] >= MV_LFO2_INC[b - 1])), "rate tables rise (byte %d)", b);
            /* Q32 step resolution, relative: the slowest rate is the worst */
            if (1.0 / MV_LFO1_INC[b] > we) we = 1.0 / MV_LFO1_INC[b];
            if (1.0 / MV_LFO2_INC[b] > we) we = 1.0 / MV_LFO2_INC[b];
        }
        printf("lfo: rate LFO1 %.3f..%.2f Hz, LFO2 %.3f..%.2f Hz, step resolution %.1e\n",
               MV_LFO1_INC[0] / 4294967296.0 / BLK, MV_LFO1_INC[127] / 4294967296.0 / BLK,
               MV_LFO2_INC[0] / 4294967296.0 / BLK, MV_LFO2_INC[127] / 4294967296.0 / BLK, we);
        CHECK(we < 1e-4, "rate resolution");
    }
    {   /* fade: the attack law on the gain (full at ln(5) tau), restarted by a key, none on LFO2 */
        int f;
        for (f = 0; f < 128; f += 9) {
            mv_lfo_t l;
            double tau = -BLK / log(MV_ENV_COEF[f] / 2147483648.0);
            mv_lfo_init(&l, 1);
            mv_lfo1_set(&l, MV_LFO_TRI, 64, (uint32_t)f);
            mv_lfo_key(&l);
            CHECK(l.gain == 0, "key restarts the fade");
            for (n = 0; l.fading && n < 1000000; n++) mv_lfo_tick(&l);
            CHECK(fabs(n * BLK - log(5.0) * tau) <= fmax(BLK * 1.01, 1e-4 * n * BLK), "fade %d: %d blocks, want %.1f",
                  f, n, log(5.0) * tau / BLK);
            CHECK(l.gain == MV_ENV_FULL_Q30, "fade ends at full");
        }
        {
            mv_lfo_t l;
            mv_lfo_init(&l, 1);
            mv_lfo2_set(&l, MV_LFO_TRI, 64);
            mv_lfo_key(&l);
            CHECK(l.gain == MV_ENV_FULL_Q30 && !l.fading, "LFO2 has no fade");
        }
    }
    {   /* LFO1 SYNC (Kris 5 Nov): one cycle per note value at the tempo (the JT's TempoClock law), all 22 values at
         * 40 / 60 / 120 / 240 BPM; the cycle measured by running the LFO, within one block; OFF / no beat: 0 */
        static const double QUARTERS[23] = {0, 0.25, 1.0 / 3, 0.375, 0.5, 2.0 / 3, 0.75, 1, 4.0 / 3, 1.5, 2, 8.0 / 3, 3,
                                            4, 16.0 / 3, 6, 8, 12, 16, 20, 24, 28, 32};   /* the JE's lFO1Sync */
        static const uint32_t BPM[4] = {40, 60, 120, 240};
        int s, t;
        double worst_rel = 0;
        CHECK(mv_lfo_sync_inc(0, 22050) == 0 && mv_lfo_sync_inc(5, 0) == 0 && mv_lfo_sync_inc(23, 22050) == 0,
              "sync OFF / no beat / out of range: 0");
        for (t = 0; t < 4; t++)
            for (s = 1; s < 23; s++) {
                uint32_t beat = MV_FS * 60u / BPM[t], inc = mv_lfo_sync_inc((uint32_t)s, beat);
                double want = QUARTERS[s] * beat / MV_CTL, got = 4294967296.0 / inc;   /* blocks a cycle */
                mv_lfo_t l;
                uint32_t old;
                long k = 0;
                CHECK(inc > 0, "sync %d at %u BPM: no step", s, BPM[t]);
                if (fabs(got - want) / want > worst_rel) worst_rel = fabs(got - want) / want;
                CHECK(fabs(got - want) / want <= 0.5 / inc * 1.001, "sync %d at %u BPM: not the rounded step", s,
                      BPM[t]);                          /* (the step is a whole number: exact to half of one) */
                mv_lfo_init(&l, 1);                     /* run it: wraps after the cycle, within a block */
                mv_lfo1_set(&l, MV_LFO_SAW, 0, 0);
                l.inc = inc;
                do { old = l.ph; mv_lfo_tick(&l); k++; } while (l.ph >= old && k < 10000000L);
                CHECK(fabs((double)k - want) <= 1.0, "sync %d at %u BPM: a cycle in %ld blocks, want %.2f", s,
                      BPM[t], k, want);
            }
        {   /* 1/4 at 120 BPM = 0.5 s; 1/4D (slot 37's) = 0.75 s; 4 MEAS at 120 = 8 s */
            uint32_t beat = MV_FS / 2u;
            CHECK(fabs(4294967296.0 / mv_lfo_sync_inc(7, beat) * BLK - 0.5) < BLK, "1/4 at 120 BPM: 0.5 s");
            CHECK(fabs(4294967296.0 / mv_lfo_sync_inc(9, beat) * BLK - 0.75) < BLK, "1/4D at 120 BPM: 0.75 s");
            CHECK(fabs(4294967296.0 / mv_lfo_sync_inc(18, beat) * BLK - 8.0) < BLK, "4 MEAS at 120 BPM: 8 s");
        }
        printf("LFO1 SYNC: 22 values x 4 tempos, step error %.1e of the cycle\n", worst_rel);
    }
    printf("mv_lfo_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
