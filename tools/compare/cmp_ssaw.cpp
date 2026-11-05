// SPDX-License-Identifier: GPL-3.0-only
// cmp_ssaw.cpp — the fixed-point Super Saw (mv_ssaw in firmware/src/mv_osc.c) against the JT-8000's float
// SupersawOsc, sample by sample (PC only).
//
// The same seed gives both the same seven start phases (the JT's xorshift, (x >> 8) / 2^24). The float one
// runs at 44.1 kHz here (kSampleRate is a JT build constant, so the frequency is set from our step x 48000 /
// 2^32 to give the same step, as cmp_osc does; its high-pass alpha then differs from ours by the rate, which
// the error shows: see the pass mark). Error = RMS difference in dB re the float output.
// The window is the first 256 frames (env CMP_N): SupersawOsc keeps its phases as float32 and has no way to
// set them, so its rounding drift cannot be re-aligned as in cmp_osc. Over 256 frames every case is at -82 dB
// or better with no wrap frames; at 2048 / 8192 the float drift alone moves wraps by a frame (-48 dB, from
// note 24 up), growing with the window: the reference, not the port (2026-11-01).
//
//   g++ -std=c++17 -O2 -I$JT -Ifirmware/src -Ifirmware/gen tools/compare/cmp_ssaw.cpp
//       $JT/core/dsp/SupersawOsc.cpp -o cmp_ssaw && ./cmp_ssaw [pass dB, default -60]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "core/dsp/SupersawOsc.h"
#include "core/AudioConfig.h"
extern "C" {
#include "mv_fixed.h"
#include "mv_pitch.c"
#include "mv_osc.c"
}

int main(int argc, char **argv)
{
    const double pass = argc > 1 ? atof(argv[1]) : -60.0;
    const int N = getenv("CMP_N") ? atoi(getenv("CMP_N")) : 256, notes[] = {24, 36, 48, 60, 72, 84, 96}, dets[] = {0, 32, 64, 96, 127}, mixes[] = {0, 64, 127};
    int cases = 0, over = 0, wrapsum = 0;
    double worst = -999;
    printf("kSampleRate %.0f (the JT build's rate)\n", (double)JT::kSampleRate);
    for (int mx : mixes) {
        printf("mix %3d  ", mx);
        for (int d : dets) printf(" det%3d", d);
        printf("   (error dB re float)\n");
        for (int n : notes) {
            printf("  n%3d  ", n);
            for (int d : dets) {
                JT::SupersawOsc r;
                mv_ssaw_t s = {};
                const uint32_t seed = 0x1234567u + (uint32_t)n;
                r.seedNoise(seed);
                r.noteOn();
                s.rng = seed | 1u;
                mv_ssaw_key(&s);
                uint32_t inc = mv_pitch_inc(n * 256);
                // the JT steps by hz / kSampleRate: give it the hz that makes the same step
                r.setFrequency((float)((double)inc * JT::kSampleRate / 4294967296.0));
                r.setDetune(d / 127.0f);
                r.setMix(mx / 127.0f);
                mv_ssaw_set(&s, inc, (uint32_t)d, (uint32_t)mx);
                static float ref[65536];
                static int32_t fx[65536];
                r.render(ref, N, nullptr, 0.0f, nullptr);
                mv_ssaw_render(&s, fx, N);
                // A naive saw's wrap lands on one frame or the next depending on the last bit of its phase:
                // float32 and Q32 round differently, so now and then one of the 7 wraps a frame apart and the
                // two differ by a full step (x its gain) for that frame. That is not a port error (both are
                // exact to their own arithmetic) and is counted, not graded: frames whose difference is the
                // size of a saw step are left out of the RMS and reported as "wrap frames".
                double e2 = 0, s2 = 0;
                int wraps = 0;
                for (int i = 0; i < N; i++) {
                    double e = fx[i] / 32768.0 - ref[i];
                    s2 += (double)ref[i] * ref[i];
                    if (std::fabs(e) > 0.02) { wraps++; continue; }
                    e2 += e * e;
                }
                wrapsum += wraps;
                double db = 10 * std::log10((e2 + 1e-30) / (s2 + 1e-30));
                printf(" %6.1f", db);
                cases++;
                if (db > worst) worst = db;
                if (db > pass) over++;
            }
            printf("\n");
        }
    }
    printf("cmp_ssaw: %d cases, worst %.1f dB, %d above the pass mark %.0f dB; %d wrap frames in %d (a frame early or "
           "late on one saw)\n", cases, worst, over, pass, wrapsum, cases * N);
    return over != 0;
}
