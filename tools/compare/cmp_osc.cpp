// SPDX-License-Identifier: GPL-3.0-only
// cmp_osc.cpp — the M-vance fixed-point oscillators against the JT-8000 float OscCore they are ported from.
//
// Both render the same note from the same phase with the same per-frame step (the float one at its own 48 kHz,
// given the frequency that makes its step equal ours), so any difference is the port. Reported per case as
// the error relative to the float signal, in dB (more negative is closer). PC only.
//
//   g++ -std=c++17 -O2 -I$JT_SRC -Ifirmware/src -Ifirmware/gen tools/compare/cmp_osc.cpp
//       $JT_SRC/core/dsp/OscCore.cpp -o build/cmp_osc && build/cmp_osc [pass_dB]
// The float oscillator keeps its phase in a float, which drifts by rounding over thousands of frames; to compare
// the waveshapes and not that drift, its phase is set to ours every SEG frames (both start each segment equal).
// SEG = 64 (env CMP_SEG overrides): at 1024 the float drift alone reads -50 dB on note 24, at 64 it is gone
// and every case sits at -72 dB or better, i.e. the residual at 1024 was the reference, not the port.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "core/dsp/OscCore.h"
extern "C" {
#include "mv_pitch.c"
#include "mv_osc.c"
}

int main(int argc, char **argv)
{
    const double pass = argc > 1 ? atof(argv[1]) : -60.0;
    const int N = 8192;
    static float ref[N];
    static int32_t fx[N];
    struct { uint32_t mv; JT::Wave jt; const char *name; } waves[] = {
        {MV_W_SQR, JT::Wave::BlPulse, "SQR"}, {MV_W_SAW, JT::Wave::JpVarSaw, "JvSAW"},
        {MV_W_TRIMOD, JT::Wave::JpTriMod, "TRIMOD"}};
    const int notes[] = {24, 36, 48, 60, 72, 84, 96, 108};
    const uint32_t ctrls[] = {0, 16, 32, 64, 96, 112, 127};
    double worst = -999;
    int bad = 0, cases = 0;
    for (auto &w : waves) {
        std::printf("%-7s", w.name);
        for (uint32_t c1 : ctrls) std::printf("  c1 %3u", c1);
        std::printf("   (error dB re float)\n");
        for (int note : notes) {
            std::printf("  n%3d ", note);
            for (uint32_t c1 : ctrls) {
                mv_osc_t o = {};
                o.inc = mv_pitch_inc(note * 256);
                mv_osc_shape(&o, w.mv, c1, 0);
                JT::OscCore r;
                r.setWave(w.jt);
                r.setJpPwmClamp(true);
                r.setFrequency((float)((double)o.inc / 4294967296.0 * JT::kSampleRate));
                // the JT's knob -> shape: SQR via shape_dc = 0.944 byte/127 (width 0.5 + 0.5 dc), the
                // SHAPE waves via byte / 127 (importer, OscSection::setShape01)
                const float s = (w.mv == MV_W_SQR) ? 0.5f + 0.5f * 0.944f * c1 / 127.0f : c1 / 127.0f;
                r.setShape(s);
                const int SEG = getenv("CMP_SEG") ? atoi(getenv("CMP_SEG")) : 64;   // N must be a multiple
                o.ph = 0;
                for (int k = 0; k < N; k += SEG) {      // re-align the float phase to ours each segment
                    r.resetPhase((float)((double)o.ph / 4294967296.0));
                    r.render(ref + k, SEG, nullptr, 0.0f, nullptr, nullptr);
                    mv_osc_render(&o, w.mv, fx + k, SEG);
                }
                double se = 0, sr = 0;
                for (int i = 0; i < N; ++i) {
                    const double e = fx[i] / 32768.0 - ref[i];
                    se += e * e;
                    sr += (double)ref[i] * ref[i];
                }
                const double db = 10.0 * std::log10((se + 1e-30) / (sr + 1e-30));
                if (db > worst) worst = db;
                if (db > pass) ++bad;
                ++cases;
                std::printf("  %6.1f", db);
            }
            std::printf("\n");
        }
    }
    std::printf("cmp_osc: %d cases, worst %.1f dB, %d above the pass mark %.0f dB\n", cases, worst, bad, pass);
    return bad != 0;
}
