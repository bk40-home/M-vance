/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the JvTRI reader in firmware/src/mv_osc.c against the same reduced table (firmware/gen/mv_jvtri.h)
 * read in double precision the JT's way (renderTable: linear in the cycle, linear between the two SHAPE frames,
 * mip level by pitch). This checks the PORT; how far the 60 KB table is from the JT's full one is
 * tools/laws/jvtri_reduce.py's figure (within one SHAPE knob step). Host only. */
#include <math.h>
#include <stdio.h>
#include "mv_pitch.c"
#include "mv_osc.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

static double rd(const int16_t *t, uint32_t lg, double ph)
{
    double L = (double)(1u << lg), pos = ph * L, f = pos - floor(pos);
    uint32_t i = (uint32_t)pos & ((1u << lg) - 1u), n = (i + 1u) & ((1u << lg) - 1u);
    return t[i] + (t[n] - t[i]) * f;
}

int main(void)
{
    static const int notes[] = {24, 36, 48, 60, 72, 84, 96, 108};
    double worst = -999;
    uint32_t ni, b, lvseen = 0;
    for (ni = 0; ni < 8; ni++)
        for (b = 0; b < 128; b += 7) {
            mv_osc_t o = {0};
            int32_t out[512];
            double e2 = 0, s2 = 0, pos, m, db;
            uint32_t i, j = 0, lv = 0, ph0;
            o.inc = mv_pitch_inc(notes[ni] * 256);
            o.ph = ph0 = 0x12345678u;
            mv_osc_shape(&o, MV_W_JVTRI, b, 0);
            mv_osc_render(&o, MV_W_JVTRI, out, 512);
            /* the reference: SHAPE b / 127 on the 0..32 axis, the kept frames around it, the JT's level choice */
            pos = b / 127.0 * 32.0;
            while (j < MV_JVTRI_FRAMES - 2u && pos >= MV_JVTRI_KEEP[j + 1]) j++;
            m = (pos - MV_JVTRI_KEEP[j]) / (MV_JVTRI_KEEP[j + 1] - MV_JVTRI_KEEP[j]);
            while (lv + 1u < MV_JVTRI_LEVELS && MV_JVTRI_HARM[lv] * (o.inc / 4294967296.0) > 0.5) lv++;
            lvseen |= 1u << lv;
            for (i = 0; i < 512; i++) {
                double ph = fmod((ph0 + (double)o.inc * (i + 1)) / 4294967296.0, 1.0);
                const int16_t *A = MV_JVTRI + j * MV_JVTRI_SPF + MV_JVTRI_OFF[lv];
                double va = rd(A, MV_JVTRI_LOG2[lv], ph), vb = rd(A + MV_JVTRI_SPF, MV_JVTRI_LOG2[lv], ph);
                double r = va + (vb - va) * m, e = out[i] - r;
                e2 += e * e; s2 += r * r;
            }
            db = 10 * log10((e2 + 1e-30) / (s2 + 1e-30));
            if (db > worst) worst = db;
            CHECK(db < -70, "note %d shape %u: %.1f dB", notes[ni], b, db);
        }
    printf("jvtri: fixed-point reader vs double, 8 notes x 19 shapes: worst %.1f dB; levels used 0x%02x\n", worst, lvseen);
    CHECK(lvseen == 0x7Fu || lvseen >= 0x1Fu, "the mip levels are reached across the notes (0x%02x)", lvseen);
    printf("mv_jvtri_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
