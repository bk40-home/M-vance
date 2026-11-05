#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# M-vance ports against the JT-8000 float code, sample by sample (PC only; needs the Jteensy8000 tree):
#   JT_SRC=/path/to/Jteensy8000/src OUT=/some/dir sh tests/run_compare.sh
# cmp_osc: SQR / JvSAW / Tri-mod vs OscCore (pass -60 dB);  cmp_filter: the six JP filter modes vs JpFilter's
# maths in double (pass -50 dB); cmp_ssaw: Super Saw vs SupersawOsc (pass -60 dB);
# cmp_jfbk: feedback osc vs JpFeedbackOsc (pass -60 dB); cmp_fx: Multi-FX / DISTORTION / delay / limiter vs FxChain
# (pass -50 dB, at the JT's 48 kHz); cmp_rv: the plate vs PlateReverb (pass -40 dB re its wet, 48 kHz). Each prints its grid and exits non-zero past its pass mark.
set -e
cd "$(dirname "$0")/.."
: "${JT_SRC:?set JT_SRC to .../Jteensy8000/src}"
OUT=${OUT:-build/compare}
mkdir -p "$OUT"
CX="-std=c++17 -O2 -Wall -I$JT_SRC -Ifirmware/src -Ifirmware/gen"
g++ $CX tools/compare/cmp_osc.cpp "$JT_SRC/core/dsp/OscCore.cpp" -o "$OUT/cmp_osc"
g++ $CX tools/compare/cmp_filter.cpp "$JT_SRC/core/dsp/JpFilter.cpp" -o "$OUT/cmp_filter"
g++ $CX tools/compare/cmp_ssaw.cpp "$JT_SRC/core/dsp/SupersawOsc.cpp" -o "$OUT/cmp_ssaw"
g++ $CX tools/compare/cmp_jfbk.cpp "$JT_SRC/core/dsp/JpFeedbackOsc.cpp" -o "$OUT/cmp_jfbk"
g++ $CX -DJT_TESTING tools/compare/cmp_fx.cpp "$JT_SRC/core/dsp/FxChain.cpp" -o "$OUT/cmp_fx"
g++ $CX tools/compare/cmp_rv.cpp "$JT_SRC/core/dsp/PlateReverb.cpp" -o "$OUT/cmp_rv"
"$OUT/cmp_osc" | tail -1
"$OUT/cmp_filter" | tail -1
"$OUT/cmp_ssaw" | tail -1
"$OUT/cmp_jfbk" | tail -1
"$OUT/cmp_fx" | tail -1
"$OUT/cmp_rv" | tail -1
