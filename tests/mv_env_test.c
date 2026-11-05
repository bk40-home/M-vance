/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of firmware/src/mv_env.c against the JP law in double precision (the JT's EnvGen JP stage model
 * with tau from docs/ref/jt_laws.json, as baked into MV_ENV_COEF). Host only: the firmware has no float. */
#include <math.h>
#include <stdio.h>
#include "mv_env.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

static const double BLK = (double)MV_CTL / MV_FS;    /* seconds per control block */
static double tau_of(int b) { return -BLK / log(MV_ENV_COEF[b] / 2147483648.0); }

int main(void)
{
    int b, n;
    double worst = 0;
    /* each knob byte: the fixed-point attack, decay and release track the double recursion with the same tau
     * to 1e-5 of full, and the stage timings land where the law says (attack full at ln(5) tau, release ends at
     * ln(1000) tau) within one block */
    for (b = 0; b < 128; b += 1) {
        mv_env_t e = {0};
        double t = tau_of(b), c = exp(-BLK / t), x = 0;
        int na = 0, nr = 0;
        mv_env_set(&e, (uint32_t)b, (uint32_t)b, 64, (uint32_t)b);
        mv_env_on(&e);
        for (n = 0; e.stage == MV_ENV_ATTACK && n < 200000; n++) {
            double err;
            mv_env_tick(&e);
            x = 1.25 - (1.25 - x) * c; if (x > 1) x = 1;
            err = fabs(e.level / 1073741824.0 - x);
            if (err > worst) worst = err;
            na++;
        }
        CHECK(fabs(na * BLK - log(5.0) * t) <= BLK * 1.01, "byte %d attack %d blocks, want %.1f", b, na, log(5.0) * t / BLK);
        for (n = 0; e.stage == MV_ENV_DECAY && n < 2000000; n++) {
            double err;
            mv_env_tick(&e);
            x = e.sus / 1073741824.0 + (x - e.sus / 1073741824.0) * c;
            err = fabs(e.level / 1073741824.0 - x);     /* (the parking step snaps by < 1e-4: by design) */
            if (e.stage == MV_ENV_DECAY && err > worst) worst = err;
        }
        CHECK(e.stage == MV_ENV_SUSTAIN && e.level == e.sus, "byte %d decay parks at sustain", b);
        mv_env_off(&e);
        x = e.level / 1073741824.0;
        for (n = 0; e.stage == MV_ENV_RELEASE && n < 2000000; n++) {
            double err;
            mv_env_tick(&e);
            x *= c;
            if (e.stage == MV_ENV_RELEASE) { err = fabs(e.level / 1073741824.0 - x); if (err > worst) worst = err; }
            nr++;
        }
        /* from sustain 64/127 to -60 dB: ln(1000 * 64/127) tau */
        /* (to 1 block or 0.01 %: near -60 dB the Q30 level is ~2^20, so rounding moves the end by a few blocks
         *  out of 10^5 on the 10 s+ releases) */
        CHECK(fabs(nr * BLK - log(1000.0 * 64 / 127) * t) <= fmax(BLK * 1.01, 1e-4 * nr * BLK), "byte %d release %d blocks, want %.1f",
              b, nr, log(1000.0 * 64 / 127) * t / BLK);
        CHECK(e.stage == MV_ENV_IDLE && e.level == 0, "byte %d release ends idle at 0", b);
    }
    printf("env: worst %.2e of full vs double, 128 bytes x A/D/R\n", worst);
    CHECK(worst < 1e-5, "precision %.2e", worst);
    {   /* gates: retrigger from the current level, sustain follows an edit, fade and kill */
        mv_env_t e = {0};
        int32_t l;
        mv_env_set(&e, 60, 60, 127, 60);
        mv_env_on(&e); for (n = 0; n < 20; n++) mv_env_tick(&e);
        l = e.level; mv_env_off(&e); mv_env_tick(&e);
        CHECK(e.level < l && e.level > l / 2, "release starts at the current level");
        l = e.level; mv_env_on(&e); mv_env_tick(&e);
        CHECK(e.level > l, "retrigger charges from the current level");
        for (n = 0; n < 5000 && e.stage != MV_ENV_SUSTAIN; n++) mv_env_tick(&e);
        CHECK(e.stage == MV_ENV_SUSTAIN && e.level == MV_ENV_FULL_Q30, "sustain 127 = full");
        mv_env_set(&e, 60, 60, 0, 60); mv_env_tick(&e);
        CHECK(e.level == 0, "sustain edit followed while holding");
        mv_env_on(&e); mv_env_tick(&e); mv_env_fade(&e);
        CHECK(mv_env_tick(&e) == 0 && !mv_env_active(&e), "fade: one block to 0, idle");
        mv_env_on(&e); mv_env_tick(&e); mv_env_kill(&e);
        CHECK(e.level == 0 && !mv_env_active(&e), "kill");
        mv_env_off(&e); CHECK(!mv_env_active(&e), "key up on an idle envelope stays idle");
    }
    printf("mv_env_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
