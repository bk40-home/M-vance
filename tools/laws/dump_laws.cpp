// SPDX-License-Identifier: GPL-3.0-only
// dump_laws.cpp — print the JT-8000's measured JP laws, byte by byte, as JSON (PC only, float).
//
// The M-vance has no FPU, so its laws are fixed-point tables. They are not retyped: this program calls the JT
// float code itself (Kris's JpEnvLaw / JpLfoRate / JpModLaw / JpFilter, MIT, from the Jteensy8000 tree) and
// tools/gen_tables.py turns its output into firmware/gen/mv_tables.h. One source of truth for both synths.
//
//   g++ -std=c++17 -O2 -I$JT_SRC tools/laws/dump_laws.cpp $JT_SRC/core/dsp/{JpEnvLaw,JpLfoRate,JpModLaw,JpFilter}.cpp
//   (tools/gen_tables.py does this when JT_SRC is set; JT_SRC = .../Jteensy8000/src)
#include <cstdio>
#include "core/dsp/JpEnvLaw.h"
#include "core/dsp/JpLfoRate.h"
#include "core/dsp/JpModLaw.h"
#include "core/dsp/JpFilter.h"
#include <cmath>

using namespace JT;

static void row(const char *name, double (*f)(int), bool last = false)
{
    std::printf("  \"%s\": [", name);
    for (int b = 0; b < 128; ++b)
        std::printf("%.9g%s", f(b), b < 127 ? ", " : "");
    std::printf("]%s\n", last ? "" : ",");
}

int main()
{
    std::printf("{\n");
    // envelope time constant (s): attack, decay, release, LFO1 fade — byte / 127 is the knob position
    row("env_tau_s", [](int b) { return (double)JpEnvLaw::tauFromNorm(b / 127.0f); });
    row("env_attack_target", [](int) { return (double)JpEnvLaw::kAttackTarget; });
    row("env_off_level", [](int) { return (double)JpEnvLaw::kOffLevel; });
    // LFO rates (Hz)
    row("lfo1_hz", [](int b) { return (double)JpLfoRate::hzFromNorm(b / 127.0f); });
    row("lfo2_hz", [](int b) { return (double)JpLfoRate::lfo2HzFromNorm(b / 127.0f); });
    // depth curves for a signed byte (64 = 0), normalised as the JT does: |1.0| == JP 63
    row("mod_cents", [](int b) { return (double)JpModLaw::depthCents((b - 64) / 63.0f); });
    row("amp_index", [](int b) { return (double)JpModLaw::ampIndex((b - 64) / 63.0f); });
    // JP filter (JpFilter): cutoff at each knob code (Hz), damping R = 1/2Q and the -24 / -12 output trims per
    // resonance byte, the R5a level compensation as log2(gain) per cutoff code above 64 (it is linear in the
    // code, -24 modes only), and the HP/BP ceiling at the FM-1's rate (the LP ceiling is 0.49 fs)
    row("flt_cut_hz", [](int b) { return (double)JpFilter::cutoffHz((float)b); });
    row("flt_r", [](int b) { return (double)JpFilter::damping((float)b); });
    row("flt_trim24", [](int b) { return (double)JpFilter::outputTrim(JpFilter::Mode::LP24, (float)b); });
    row("flt_trim12", [](int b) { return (double)JpFilter::outputTrim(JpFilter::Mode::LP12, (float)b); });
    row("flt_lc_log2_per_code", [](int b) {
        return (double)std::log2(JpFilter::levelComp(JpFilter::Mode::LP24, (float)b, 112.0f)) / 48.0; });
    row("flt_ceil_hpbp_hz", [](int) { return (double)JpFilter::ceilingHz(JpFilter::Mode::HP24, 44100.0f); });
    row("flt_steps_per_oct", [](int) {      // the cutoff law's slope, read back from the law itself
        return 64.0 / std::log2((double)JpFilter::cutoffHz(64.0f) / JpFilter::cutoffHz(0.0f)); }, true);
    std::printf("}\n");
    return 0;
}
