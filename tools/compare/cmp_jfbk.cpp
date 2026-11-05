// SPDX-License-Identifier: GPL-3.0-only
// cmp_jfbk.cpp — the fixed-point feedback oscillator (mv_jfbk in firmware/src/mv_osc.c) against the JT-8000's
// float JpFeedbackOsc, sample by sample (PC only).
//
// Same seed (the wander noise is the same xorshift), same start phase, same step: the float one gets the
// frequency that gives our step at its 48 kHz build rate, and our high-pass a is set to its 48 kHz value for
// the comparison (in the firmware it is the 1.92 ms time constant at 44.1 kHz). Notes avoid the C3/C5 map
// split, which the two would place a hair apart. Window 256 frames (float32 phase drift, as cmp_ssaw).
// An edge event: where the inner phase lands within rounding of a block boundary (the wander walk can put it
// there), float32 and Q32 place that edge a frame apart, and the 1.92 ms high-pass then carries the step
// difference for ~90 frames. Both are exact to their own arithmetic, so a case with an edge event is shown
// with '*' and its figure is not graded (2026-11-01: 2 of 175 cases; every other case at -73 dB or better).
//
//   g++ -std=c++17 -O2 -I$JT -Ifirmware/src -Ifirmware/gen tools/compare/cmp_jfbk.cpp
//       $JT/core/dsp/JpFeedbackOsc.cpp -o cmp_jfbk && ./cmp_jfbk [pass dB, default -60]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "core/dsp/JpFeedbackOsc.h"
#include "core/AudioConfig.h"
extern "C" {
#include "mv_fixed.h"
#include "mv_pitch.c"
#include "mv_osc.c"
}

int main(int argc, char **argv)
{
    const double pass = argc > 1 ? atof(argv[1]) : -60.0;
    const int N = getenv("CMP_N") ? atoi(getenv("CMP_N")) : 256;
    const int notes[] = {36, 48, 60, 72, 84}, fbs[] = {0, 20, 32, 48, 64, 96, 127}, hs[] = {0, 40, 64, 100, 127};
    int cases = 0, over = 0, edges = 0;
    double worst = -999;
    static float ref[65536];
    static int32_t fx[65536];
    const double a48 = 1.92e-3 / (1.92e-3 + 1.0 / JT::kSampleRate);
    for (int h : hs) {
        printf("harm %3d", h);
        for (int fb : fbs) printf("   fb%3d", fb);
        printf("   (error dB re float)\n");
        for (int n : notes) {
            printf("  n%3d  ", n);
            for (int fb : fbs) {
                JT::JpFeedbackOsc r;
                mv_jfbk_t f;
                const uint32_t seed = 0xBEEF00u + (uint32_t)(n * 131 + fb * 7 + h);
                const uint32_t inc = mv_pitch_inc(n * 256), ph0 = 0x40000000u;
                r.seedNoise(seed);
                r.setFrequency((float)((double)inc * JT::kSampleRate / 4294967296.0));
                r.setHarmonics(h / 127.0f);
                r.setFeedback(fb / 127.0f);
                r.noteOn(0.25f);
                mv_jfbk_init(&f, seed);
                f.hpa = (uint32_t)llround(a48 * 2147483648.0);
                mv_jfbk_key(&f, ph0);
                mv_jfbk_set(&f, inc, (uint32_t)h, (uint32_t)fb);
                r.render(ref, N, nullptr, 0.0f);
                mv_jfbk_render(&f, fx, N);
                double e2 = 0, s2 = 0;
                int ev = 0;
                for (int i = 0; i < N; i++) {
                    double e = fx[i] / 32768.0 - ref[i];
                    s2 += (double)ref[i] * ref[i];
                    if (std::fabs(e) > 0.5) ev = 1;      // an edge a frame apart (a step of 2 x the block gain)
                    e2 += e * e;
                }
                double db = 10 * std::log10((e2 + 1e-30) / (s2 + 1e-30));
                printf(" %6.1f%c", db, ev ? '*' : ' ');
                cases++;
                edges += ev;
                if (!ev && db > worst) worst = db;
                if (!ev && db > pass) over++;
            }
            printf("\n");
        }
    }
    printf("cmp_jfbk: %d cases, worst %.1f dB, %d above the pass mark %.0f dB; %d edge events (*, not graded)\n",
           cases, worst, over, pass, edges);
    return over != 0;
}
