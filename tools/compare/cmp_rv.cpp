// SPDX-License-Identifier: GPL-3.0-only
// cmp_rv.cpp — the fixed-point plate (firmware/src/mv_reverb.c) against the JT-8000's float PlateReverb, sample by
// sample (PC only). Both at the JT's 48 kHz (mv_reverb.c built with MV_RV_FS 48000: its 48 kHz knob tables). The JT
// gets the same settings as norm = value / 127 (its setters; reverb.mix = LEVEL / 127), no predelay (its default).
// Input: stereo, a detuned saw pair and a noise burst (about -12 dBFS) for 1 s, then 3 s of tail.
// Error = RMS of (ours - JT) over both sides in dB re the RMS of the JT's WET part (its output minus the input: the
// dry passes through both untouched, so grading against the whole output would flatter the port). Pass: argv[1]
// (default -40 dB). The tank LFO's phase is the JT's float accumulation (MV_RV_CMP_PHASE; CMP_FLOAT_PHASE=0: exact).
// Result (2026-11-02): -45 dB at SIZE 127 (decay 0.9995, endless), -48 .. -56 dB elsewhere, worst samples -53 dBFS
// (SIZE 127) / -68 dBFS or lower. It is the 16-bit tank lines (Q14): one bit more or less moves every case by 6 dB.
//
//   g++ -std=c++17 -O2 -I$JT -Ifirmware/src -Ifirmware/gen tools/compare/cmp_rv.cpp $JT/core/dsp/PlateReverb.cpp
//       -o cmp_rv && ./cmp_rv [pass dB]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "core/dsp/PlateReverb.h"
#include "core/AudioConfig.h"
#define MV_RV_FS 48000u
static float g_ph, g_inc;
static int g_float_phase;
static uint32_t cmp_phase(uint32_t ph)
{
    if (!g_float_phase) return ph;
    g_ph += g_inc; if (g_ph >= 1.0f) g_ph -= 1.0f;   // triangleLFO's accumulation, in float
    return (uint32_t)((double)g_ph * 4294967296.0);
}
#define MV_RV_CMP_PHASE(ph) cmp_phase(ph)
extern "C" {
#include "mv_fixed.h"
#include "mv_reverb.c"
}

static const int N = 48000 * 4;

static double run(const uint8_t *s, const std::vector<int32_t> &inl, const std::vector<int32_t> &inr, const char *name)
{
    static std::vector<float> pool(JT::PlateReverb::kPoolFloats);
    JT::PlateReverb jt;
    jt.begin(pool.data());
    jt.setSize(s[MV_RV_SIZE] / 127.0f); jt.setHiDamp(s[MV_RV_HIDAMP] / 127.0f); jt.setLoDamp(s[MV_RV_LODAMP] / 127.0f);
    jt.setLowpass(s[MV_RV_LOWPASS] / 127.0f); jt.setHipass(s[MV_RV_HIPASS] / 127.0f); jt.setFreeze(s[MV_RV_FREEZE] != 0);
    const float mix = s[MV_RV_LEVEL] / 127.0f;
    static mv_rv_t r;
    mv_rv_init(&r);
    for (auto &v : rv_diff) v = 0;
    for (auto &v : rv_tapf0) v = 0;
    for (auto &v : rv_tapf1) v = 0;
    for (auto &v : rv_tdl0) v = 0;
    for (auto &v : rv_tdl1) v = 0;
    mv_rv_set(&r, s);
    g_ph = 0.0f; g_inc = 0.8f / JT::kSampleRate;
    double se = 0, sw = 0, worst = 0;
    std::vector<float> L(128), R(128);
    std::vector<int32_t> out(2 * 16);
    for (int k = 0; k < N; k += 128) {
        for (int i = 0; i < 128; i++) { L[i] = inl[k + i] / 32768.0f; R[i] = inr[k + i] / 32768.0f; }
        jt.processBlock(L.data(), R.data(), 128, mix, mix);
        for (int j = 0; j < 128; j += 16) {
            for (int i = 0; i < 16; i++) { out[2 * i] = inl[k + j + i]; out[2 * i + 1] = inr[k + j + i]; }
            mv_rv_process(&r, out.data(), 16);
            for (int i = 0; i < 16; i++) {
                const int f = k + j + i;
                double el = out[2 * i] / 32768.0 - L[j + i], er = out[2 * i + 1] / 32768.0 - R[j + i];
                double wl = L[j + i] - inl[f] / 32768.0, wr = R[j + i] - inr[f] / 32768.0;
                se += el * el + er * er; sw += wl * wl + wr * wr;
                if (fabs(el) > worst) worst = fabs(el);
                if (fabs(er) > worst) worst = fabs(er);
            }
        }
    }
    double db = 10 * log10(se / (sw > 0 ? sw : 1));
    printf("%-44s err %7.1f dB re wet   worst sample %6.1f dBFS\n", name, db, 20 * log10(worst > 0 ? worst : 1e-9));
    return db;
}

int main(int argc, char **argv)
{
    const double pass = argc > 1 ? atof(argv[1]) : -40.0;
    g_float_phase = !getenv("CMP_FLOAT_PHASE") || atoi(getenv("CMP_FLOAT_PHASE"));
    std::vector<int32_t> inl(N, 0), inr(N, 0);
    uint32_t rng = 7;
    double p1 = 0, p2 = 0;
    for (int i = 0; i < 48000; i++) {
        p1 += 110.0 / 48000; p2 += 164.8 / 48000;
        double a = 0.25 * ((p1 - floor(p1)) * 2 - 1), b = 0.25 * ((p2 - floor(p2)) * 2 - 1);
        rng = rng * 1664525u + 1013904223u;
        double nz = (i >= 24000 && i < 26400) ? 0.3 * ((int32_t)rng / 2147483648.0) : 0;
        inl[i] = (int32_t)lrint((a + 0.5 * b + nz) * 32768 * 0.5);
        inr[i] = (int32_t)lrint((0.5 * a + b + nz) * 32768 * 0.5);
    }
    static const struct { uint8_t s[MV_RV_N]; const char *name; } C[] = {
        {{64, 0, 0, 0, 0, 0, 0}, "LEVEL 64 SIZE 0"},
        {{64, 64, 0, 0, 0, 0, 0}, "LEVEL 64 SIZE 64"},
        {{127, 100, 0, 0, 0, 0, 0}, "LEVEL 127 SIZE 100"},
        {{127, 127, 0, 0, 0, 0, 0}, "LEVEL 127 SIZE 127 (endless)"},
        {{100, 90, 64, 0, 0, 0, 0}, "SIZE 90 HI DAMP 64"},
        {{100, 90, 127, 0, 0, 0, 0}, "SIZE 90 HI DAMP 127"},
        {{100, 90, 0, 64, 0, 0, 0}, "SIZE 90 LO DAMP 64"},
        {{100, 90, 40, 40, 64, 0, 0}, "SIZE 90 DAMPS 40 LOWPASS 64"},
        {{100, 90, 40, 40, 0, 64, 0}, "SIZE 90 DAMPS 40 HIPASS 64"},
        {{100, 90, 40, 40, 64, 64, 0}, "SIZE 90 DAMPS 40 LP 64 HP 64"},
        {{100, 90, 0, 0, 0, 0, 1}, "FREEZE (from silence: holds nothing)"},
    };
    double worst = -999;
    for (const auto &c : C) worst = fmax(worst, run(c.s, inl, inr, c.name));
    printf("worst %.1f dB (pass %.0f): %s\n", worst, pass, worst <= pass ? "PASS" : "FAIL");
    return worst <= pass ? 0 : 1;
}
