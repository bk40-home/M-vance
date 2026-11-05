// SPDX-License-Identifier: GPL-3.0-only
// cmp_filter.cpp — the fixed-point JP filter (firmware/src/mv_filter.c) against the JT-8000's float JpFilter,
// sample by sample (PC only).
//
// The reference is JpFilter's own section maths in DOUBLE precision, with the JT's laws (cutoffHz, damping,
// outputTrim, levelComp, ceilingHz): at high Q and a high cutoff the JT's float filter is itself only ~-42 dB
// from that (float32 states), so comparing to it would grade the reference. The float JpFilter is still run,
// and its own error vs double is printed for the worst case, so the two can be seen side by side.
// Both run at 44.1 kHz on the same input (white noise at -12 dBFS, the scene-14 style excitation, then a
// silent tail so the resonance rings out), same cutoff code and resonance, the float one with
// g = tan(pi fc / fs) and R = JpFilter::damping, its output x outputTrim x levelComp and the JT's H2b tail.
// The error is the RMS difference in dB re the float output's RMS, or re -40 dBFS when the output is quieter
// than that (a 13 Hz low pass of the test noise is ~-70 dBFS: the Q15 output's own LSB, ~-101 dBFS, would
// read as a large relative error there while being the same noise floor the DAC has). Cases: six modes x
// reso 0..127 x cutoff codes.
//
//   g++ -std=c++17 -O2 -I$JT -Ifirmware/src -Ifirmware/gen tools/compare/cmp_filter.cpp
//       $JT/core/dsp/JpFilter.cpp -o cmp_filter && ./cmp_filter [pass dB, default -50]
// Result 2026-10-31: worst -53 dB (LP24 reso 127 cut 127, where the float JT is at -42.6); low cutoffs ~-57 dB
// re -40 dBFS, i.e. ~-97 dBFS absolute: the Q15 output floor. Everything else -60 .. -88 dB.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "core/dsp/JpFilter.h"
extern "C" {
#include "mv_filter.c"
}

using JT::JpFilter;

template <JpFilter::Mode M> static float run(JpFilter &f, float x) { return f.template tick<M>(x); }

static float tickMode(JpFilter &f, int m, float x)
{
    switch (m) {
    case 0: return run<JpFilter::Mode::LP24>(f, x);
    case 1: return run<JpFilter::Mode::LP12>(f, x);
    case 2: return run<JpFilter::Mode::HP24>(f, x);
    case 3: return run<JpFilter::Mode::HP12>(f, x);
    case 4: return run<JpFilter::Mode::BP24>(f, x);
    default: return run<JpFilter::Mode::BP12>(f, x);
    }
}

struct Sec {                    // JpFilter::section in double
    double s1 = 0, s2 = 0, g = 0, a = 0, d = 0;
    void set(double G, double R) { g = G; a = 2 * R + G; d = 1 / (1 + 2 * R * G + G * G); }
    void tick(double x, double &hp, double &bp, double &lp)
    {
        hp = (x - a * s1 - s2) * d; bp = g * hp + s1; lp = g * bp + s2; s1 = 2 * bp - s1; s2 = 2 * lp - s2;
    }
};

static double tail(double y)      // JT FilterSection runBlockJp: linear to 4, tanh shoulder into 8
{
    double a = std::fabs(y);
    if (a > 4.0) { double s = 4.0 + 4.0 * std::tanh((a - 4.0) / 4.0); y = y < 0 ? -s : s; }
    return y < -8.0 ? -8.0 : y > 8.0 ? 8.0 : y;
}

int main(int argc, char **argv)
{
    const double pass = argc > 1 ? atof(argv[1]) : -50.0;   // see the top: -53 at worst (the float JT: -42.6)
    const double FS = 44100.0;
    const int N = 44100, ON = 22050;            // 0.5 s noise, 0.5 s ring-out
    const char *mn[6] = {"LP24", "LP12", "HP24", "HP12", "BP24", "BP12"};
    const int resos[] = {0, 32, 64, 96, 112, 120, 127};
    const int cuts[] = {0, 24, 48, 64, 80, 96, 112, 127};
    std::vector<int32_t> in(N);
    uint32_t rng = 1;
    for (int i = 0; i < N; i++) {
        rng = rng * 1664525u + 1013904223u;
        in[i] = i < ON ? (int32_t)(rng >> 16) / 4 - 8192 : 0;     // +-0.25, Q15
    }
    int cases = 0, over = 0;
    double worst = -999, worst_float = 0;
    for (int m = 0; m < 6; m++) {
        printf("%-5s", mn[m]);
        for (int c : cuts) printf("   cut%3d", c);
        printf("   (error dB re double, rows = reso)\n");
        for (int r : resos) {
            printf("  r%3d", r);
            for (int c : cuts) {
                JpFilter jf;
                mv_flt_t f = {};
                const JpFilter::Mode M = (JpFilter::Mode)m;
                double fc = JpFilter::cutoffHz((float)c), hi = JpFilter::ceilingHz(M, (float)FS);
                if (fc > hi) fc = hi;
                const double G = std::tan(M_PI * fc / FS), R = JpFilter::damping((float)r);
                jf.setCoeffs((float)G, (float)R);
                Sec s1, s2;
                s1.set(G, R); s2.set(G, R);
                double trim = (double)JpFilter::outputTrim(M, (float)r) * JpFilter::levelComp(M, (float)r, (float)c);
                mv_flt_set(&f, (uint32_t)m, (uint32_t)r, (uint32_t)c);
                mv_flt_cutoff(&f, c * 256);
                std::vector<int32_t> buf(in);
                mv_flt_render(&f, buf.data(), N);
                double e2 = 0, sig = 0, ef2 = 0;
                for (int i = 0; i < N; i++) {
                    double x = in[i] / 32768.0, hp, bp, lp;
                    s1.tick(x, hp, bp, lp);
                    double y = m < 2 ? lp : m < 4 ? hp : bp;
                    if (m % 2 == 0) { s2.tick(y, hp, bp, lp); y = m < 2 ? lp : m < 4 ? hp : bp; }
                    y = tail(y * trim);
                    double yf = tail(tickMode(jf, m, (float)x) * (float)trim);
                    double e = buf[i] / 32768.0 - y;
                    e2 += e * e; sig += y * y; ef2 += (yf - y) * (yf - y);
                }
                double ref = sig > N * 1e-4 ? sig : N * 1e-4;            // -40 dBFS floor (see the top)
                double db = 10 * std::log10((e2 + 1e-30) / ref);
                if (db > worst) worst_float = 10 * std::log10((ef2 + 1e-30) / ref);
                printf("  %7.1f", db);
                cases++;
                if (db > worst) worst = db;
                if (db > pass) over++;
            }
            printf("\n");
        }
    }
    printf("cmp_filter: %d cases, worst %.1f dB (the JT float filter: %.1f dB there), %d above the pass mark %.0f dB\n",
           cases, worst, worst_float, over, pass);
    return over != 0;
}
