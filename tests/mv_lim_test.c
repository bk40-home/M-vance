/* SPDX-License-Identifier: GPL-3.0-only */
/* mv_lim (the output limiter, firmware/src/mv_lim.c): below the ceiling a block passes untouched, 16 frames late; a
 * loud one never passes 0.97 FS; the gain moves in ramps (no step); it releases in about 100 ms; a held block is
 * let out when the engine rests.
 *   cc -std=c99 -O2 -Wall -Ifirmware/src tests/mv_lim_test.c -o mv_lim_test -lm && ./mv_lim_test */
#include <math.h>
#include <stdio.h>
#include "mv_lim.c"
#define PI 3.14159265358979323846   /* (PI is not C99) */

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                  printf(__VA_ARGS__); printf("\n"); } } while (0)
#define N 44096u                                /* (whole 16-frame blocks: 1 s) */
static int32_t in[2u * N], out[2u * N];

/* the signal through the limiter a block at a time; out[] = what plays */
static void through(mv_lim_t *l, uint32_t frames)
{
    uint32_t k, i;
    for (k = 0; k + MV_LIM_N <= frames; k += MV_LIM_N) {
        int32_t b[2u * MV_LIM_N];
        for (i = 0; i < 2u * MV_LIM_N; i++) b[i] = in[2u * k + i];
        mv_lim_block(l, b);
        for (i = 0; i < 2u * MV_LIM_N; i++) out[2u * k + i] = b[i];
    }
}

int main(void)
{
    mv_lim_t l;
    uint32_t i, same = 1, over = 0;
    int32_t peak = 0;
    double worst_dg = 0, g_prev = -1;
    /* 1: a sine at 0.9 FS: untouched, exactly 16 frames late */
    for (i = 0; i < N; i++) in[2u * i] = in[2u * i + 1u] = (int32_t)lrint(0.9 * 32767 * sin(2 * PI * 440 * i / 44100.0));
    mv_lim_init(&l); through(&l, N);
    for (i = MV_LIM_N; i < N; i++) same &= (uint32_t)(out[2u * i] == in[2u * (i - MV_LIM_N)]);
    CHECK(same, "below the ceiling: the same samples, 16 frames late");
    /* 2: the sine at 3 x FS, in bursts (on 0.2 s, off 0.2 s): never past 0.97 FS, the gain moving in ramps */
    for (i = 0; i < N; i++) {
        double a = (i / 8820u) % 2u ? 0.0 : 3.0;
        in[2u * i] = in[2u * i + 1u] = (int32_t)lrint(a * 32767 * sin(2 * PI * 220 * i / 44100.0));
    }
    mv_lim_init(&l); through(&l, N);
    for (i = 0; i < N; i++) {
        int32_t a = out[2u * i] < 0 ? -out[2u * i] : out[2u * i];
        if (a > peak) peak = a;
        over += (uint32_t)(a > 31784);
        if (i >= MV_LIM_N) {                    /* the gain applied: out / in (where in is big enough to tell) */
            int32_t x = in[2u * (i - MV_LIM_N)];
            if (x > 20000 || x < -20000) {
                double g = (double)out[2u * i] / x;
                if (g_prev >= 0 && fabs(g - g_prev) > worst_dg) worst_dg = fabs(g - g_prev);
                g_prev = g;
            } else g_prev = -1;
        }
    }
    CHECK(over == 0u, "3 x FS: %u samples past 0.97 FS (peak %d)", over, peak);
    CHECK(peak > 30000, "3 x FS: limited to the ceiling, not below it (peak %d)", peak);
    /* a ramp over a block moves the gain at most (1 - 0.323) / 16 = 0.042 a sample at 3 x FS; a step would be 0.68 */
    CHECK(worst_dg < 1.0 / 16.0, "the gain ramps: its largest step between samples %.4f", worst_dg);
    /* 3: release: after the loud burst, a quiet sine comes back to gain 1 within ~100 ms of the burst's end */
    for (i = 0; i < N; i++) {
        double a = i < 4410u ? 3.0 : 0.5;
        in[2u * i] = in[2u * i + 1u] = (int32_t)lrint(a * 32767 * sin(2 * PI * 220 * i / 44100.0));
    }
    mv_lim_init(&l); through(&l, N);
    {
        uint32_t back = 0;
        for (i = 4410u + MV_LIM_N; i < N && !back; i++) {
            int32_t x = in[2u * (i - MV_LIM_N)];
            if ((x > 10000 || x < -10000) && fabs((double)out[2u * i] / x - 1.0) < 0.001) back = i;
        }
        CHECK(back && back - 4410u < 4850u, "release: gain 1 again %.0f ms after the burst", (back - 4410.0) / 44.1);
    }
    /* 4: the held block: sound then silence -> pending until the next (silent) block lets it out */
    {
        int32_t b[2u * MV_LIM_N];
        mv_lim_init(&l);
        for (i = 0; i < 2u * MV_LIM_N; i++) b[i] = 1000;
        mv_lim_block(&l, b);
        CHECK(mv_lim_pending(&l) && b[0] == 0, "a block with sound is held (silence out first)");
        for (i = 0; i < 2u * MV_LIM_N; i++) b[i] = 0;
        mv_lim_block(&l, b);
        CHECK(!mv_lim_pending(&l) && b[0] == 1000 && b[31] == 1000, "the next (silent) block lets it out");
    }
    printf("mv_lim_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
