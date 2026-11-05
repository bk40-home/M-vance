// SPDX-License-Identifier: GPL-3.0-only
// cmp_fx.cpp — the fixed-point effects (firmware/src/mv_fx.c) against the JT-8000's float FxChain, sample by sample
// (PC only; built with -DJT_TESTING for the JT's debug accessors). Both run at the JT's 48 kHz (mv_fx.c built with MV_FX_FS 48000). The JT chain is set up the way its
// JP importer sets it from the same patch bytes (JP8000SyxImporter.cpp: Multi-FX type t -> engine type 11 + t,
// level / 127; delay type -> kDelayTypeMap, time byte / 127 x the type's maximum, feedback and level / 127), tone
// flat, dry 0 / JPFX 1 (the patch defaults). Two signed-off M-vance changes (Kris 2 Nov) are built into the reference:
//   DISTORTION: drive Amount = LEVEL / 127 (the importer fixes it at 1): setDriveAmount(level / 127);
//   DELAY as a SEND (1b): ours = in + m d, the JT's = (1 - m) in + m d, so the reference is the JT's output plus
//   m x the same chain with the delay off (the delay's input; the limiter stays idle at these levels).
// Input: a mono test signal (a detuned saw pair and a noise burst, about -12 dBFS, then silence for the tails).
// Error = RMS of (ours - JT) over both sides, in dB re the JT output's RMS (over the 3 s, tails included), and the
// worst single sample in dBFS. Pass mark: argv[1] (default -50 dB, as cmp_filter).
// What is left (2026-11-02, phase re-aligned; error feedback in the recursive filters): -57 .. -85 dB, worst
// samples -63 dBFS or lower. The largest are the short delays with feedback: the 16-bit lines (Q14, truncated toward
// zero so a tail ends) against the JT's float lines, compounded each repeat. Next, the JT's own float32 positions:
// its read index is a float up to 2400 (Multi-FX) / 62500 (delay), steps of 0.0002 / 0.004 of a sample.
//
//   g++ -std=c++17 -O2 -I$JT -Ifirmware/src -Ifirmware/gen tools/compare/cmp_fx.cpp $JT/core/dsp/FxChain.cpp
//       -o cmp_fx && ./cmp_fx [pass dB]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "core/dsp/FxChain.h"
#include "core/AudioConfig.h"
#define MV_FX_FS 48000u
// The JT keeps the Multi-FX LFO phase in float32 (_jpPhase += _jpInc): at these rates the increment (1.5e-6 for
// SC SLW) is near the float's step at 0.5 (6e-8), so every add rounds by a few % and the JT's LFO runs off its
// stated rate. Our Q32 phase is exact. To grade the engine and not that rounding, env CMP_FLOAT_PHASE=1 replaces
// our phase with the JT's float accumulation, step for step (the cmp_osc idea: re-align, then compare). On by
// default; CMP_FLOAT_PHASE=0 shows the exact phase against the JT's drifting one (-46 dB on the slow types).
static float g_fph, g_finc;
static int g_float_phase;
static uint32_t cmp_phase(uint32_t ph)
{
    if (!g_float_phase) return ph;
    g_fph += g_finc; if (g_fph >= 1.0f) g_fph -= 1.0f;
    return (uint32_t)((double)g_fph * 4294967296.0);
}
#define MV_FX_CMP_PHASE(ph) cmp_phase(ph)
extern "C" {
#include "mv_fixed.h"
#include "mv_fx.c"
}

static const int N = 48000 * 3;                 // 3 s: 1 s of sound, 2 s of tail
static const uint8_t DLY_MAP[5] = {2, 3, 4, 0, 1};   // JP type byte -> kDelayPresets index (kDelayTypeMap - 1)
static const float DLY_MAX[5] = {625.f, 625.f, 156.f, 156.f, 1250.f};

static double run(const uint8_t *b, const std::vector<int32_t> &in, const char *name)
{
    static std::vector<float> pool(JT::FxChain::kPoolFloats);
    std::fill(pool.begin(), pool.end(), 0.0f);
    JT::FxChain jt, jn;                         // jn: the same chain, delay off (the delay's input, for the send)
    static std::vector<float> pool2(JT::FxChain::kPoolFloats);
    std::fill(pool2.begin(), pool2.end(), 0.0f);
    jt.begin(pool.data());
    jn.begin(pool2.data());
    for (JT::FxChain *c : {&jt, &jn}) {
        c->setDryMix(0.0f); c->setJpfxMix(1.0f);
        if (b[61] == 12) { c->setDriveAmount(b[62] / 127.0f); c->setModEffect(-1); }
        else if (b[62]) { c->setModEffect(11 + b[61]); c->setModMix(b[62] / 127.0f); }
        else c->setModEffect(-1);
        c->setDelayEffect(-1);
    }
    if (b[66]) {
        float ms = b[64] / 127.0f * DLY_MAX[b[63]];
        jt.setDelayEffect(DLY_MAP[b[63]]);
        jt.setDelayTime(ms < 10.f ? 10.f : ms);
        jt.setDelayFeedback(b[65] / 127.0f);
        jt.setDelayMix(b[66] / 127.0f);
    } else jt.setDelayEffect(-1);

    static mv_fx_t f;
    mv_fx_init(&f);
    for (auto &l : dline) for (auto &s : l) s = 0;
    for (auto &s : mline) s = 0;
    mv_fx_set(&f, b);
    g_fph = 0.0f;                               // prepareJpMod: phase 0, inc = rate / fs in float
    g_finc = b[61] < 12 ? (float)(MFX_TYPES[b[61]].rate_mhz / 1000.0f) / JT::kSampleRate : 0.0f;

    double se = 0, sr = 0, worst = 0;
    const int B = 16;                           // our control block (the JT chain runs its own 128 inside)
    std::vector<float> L(128), R(128), NL(128), NR(128);
    const float m = b[66] / 127.0f;             // the delay send level
    uint32_t mute_prev = 0;                     // the JT's delay mute still owed after the last block
    std::vector<int32_t> bus(B), out(2 * B);
    for (int k = 0; k < N; k += 128) {
        for (int i = 0; i < 128; i++) L[i] = R[i] = NL[i] = NR[i] = in[k + i] / 32768.0f;
        jt.processBlock(L.data(), R.data(), 128);
        jn.processBlock(NL.data(), NR.data(), 128);
        if (b[66]) {
            // While the JT mutes the delay (a type change: one tap's depth) it outputs the dry at unity, which is
            // what ours does too: add the send's dry only to the frames after the mute. The counter is clamped in
            // the first block, which it always covers (>= 10 ms > 128 frames), so: muted = all, or what was owed.
            const uint32_t now = jt.debugDelayMuteCounter();
            const uint32_t muted = now ? 128u : (mute_prev < 128u ? mute_prev : 128u);
            for (uint32_t i = muted; i < 128u; i++) { L[i] += m * NL[i]; R[i] += m * NR[i]; }
            mute_prev = now;
        }
        for (int j = 0; j < 128; j += B) {
            for (int i = 0; i < B; i++) { bus[i] = in[k + j + i]; out[2 * i] = out[2 * i + 1] = 0; }
            mv_fx_drive(&f, bus.data(), B);
            mv_fx_out(&f, bus.data(), out.data(), B);
            for (int i = 0; i < B; i++) {
                double el = out[2 * i] / 32768.0 - L[j + i], er = out[2 * i + 1] / 32768.0 - R[j + i];
                se += el * el + er * er;
                sr += (double)L[j + i] * L[j + i] + (double)R[j + i] * R[j + i];
                if (fabs(el) > worst) worst = fabs(el);
                if (fabs(er) > worst) worst = fabs(er);
            }
        }
    }
    double db = 10 * log10(se / (sr > 0 ? sr : 1));
    printf("%-34s err %7.1f dB   worst sample %6.1f dBFS\n", name, db, 20 * log10(worst > 0 ? worst : 1e-9));
    return db;
}

int main(int argc, char **argv)
{
    const double pass = argc > 1 ? atof(argv[1]) : -50.0;
    g_float_phase = !getenv("CMP_FLOAT_PHASE") || atoi(getenv("CMP_FLOAT_PHASE"));
    static const char *const MT[13] = {"SC SLW", "SC MID", "SC FST", "SC CLR", "FLANGER SLOW", "FLANGER DEEP",
        "FLANGER FAST", "DEEP PHASING", "JET PHASING", "TWISTING", "FREEZE 1", "FREEZE 2", "DISTORTION"};
    static const char *const DT[5] = {"PAN L>R", "PAN R>L", "PAN SHORT", "MONO SHORT", "MONO LONG"};
    std::vector<int32_t> in(N, 0);
    uint32_t rng = 1;
    double p1 = 0, p2 = 0;
    for (int i = 0; i < 48000; i++) {           // 1 s: saws at 110 and 110.7 Hz, plus 50 ms of noise at 0.5 s
        p1 += 110.0 / 48000; p2 += 110.7 / 48000;
        double s = 0.25 * ((p1 - floor(p1)) * 2 - 1) + 0.25 * ((p2 - floor(p2)) * 2 - 1);
        rng = rng * 1664525u + 1013904223u;
        if (i >= 24000 && i < 26400) s += 0.3 * ((int32_t)rng / 2147483648.0);
        in[i] = (int32_t)lrint(s * 32768 * 0.5);
    }
    double worst = -999;
    char name[64];
    uint8_t b[248] = {0};
    for (int t = 0; t < 13; t++)
        for (int lv : {32, 64, 127}) {
            if (t < 12 && lv == 32) continue;
            b[61] = (uint8_t)t; b[62] = (uint8_t)lv; b[66] = 0;
            snprintf(name, sizeof name, "MFX %s lvl %d", MT[t], lv);
            worst = fmax(worst, run(b, in, name));
        }
    for (int d = 0; d < 5; d++)
        for (int tm : {0, 64, 127})
            for (int fb : {0, 100}) {
                b[61] = 0; b[62] = 0; b[63] = (uint8_t)d; b[64] = (uint8_t)tm; b[65] = (uint8_t)fb; b[66] = 100;
                snprintf(name, sizeof name, "DLY %s t%d fb%d", DT[d], tm, fb);
                worst = fmax(worst, run(b, in, name));
            }
    b[61] = 0; b[62] = 100; b[63] = 0; b[64] = 80; b[65] = 90; b[66] = 90;
    worst = fmax(worst, run(b, in, "SC SLW 100 + PAN L>R t80 fb90"));
    printf("worst %.1f dB (pass %.0f): %s\n", worst, pass, worst <= pass ? "PASS" : "FAIL");
    return worst <= pass ? 0 : 1;
}
