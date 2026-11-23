#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The rate chosen at start (firmware/src/mv_rate.h, MV_RATE_RT: the VIBE FM build; Kris 23 Nov 2026) against the FM-1's
# fixed 44.1 kHz: the host app (tests/host.c) built both ways runs every script in tests/scripts/, and each WAV must be
# the same, bit for bit. Then the VIBE build at 44.1 kHz sounds exactly as the FM-1.
#   tests/run_rate_compare.sh
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-build/rate}
mkdir -p "$OUT"
I="-Ifirmware/hal -Ifirmware/src -Ifirmware/gen"
cc -O2 -Wall -Wno-unused-function -DDINGHY_HOST -DAPP_VER='"dev"' $I -o "$OUT/fm1" firmware/src/mvance.c firmware/src/mv_dsp.c
cc -O2 -Wall -Wno-unused-function -DDINGHY_HOST -DMV_RATE_RT -DAPP_VER='"dev"' $I -o "$OUT/rt" firmware/src/mvance.c firmware/src/mv_dsp.c
same=0; diff=0
for s in tests/scripts/*.txt; do
    b=$(basename "$s" .txt)
    "$OUT/fm1" "$OUT/fm1-$b" "$s" >/dev/null 2>&1
    "$OUT/rt" "$OUT/rt-$b" "$s" >/dev/null 2>&1
    if cmp -s "$OUT/fm1-$b.wav" "$OUT/rt-$b.wav"; then same=$((same + 1)); else diff=$((diff + 1)); echo "DIFF $b"; fi
done
echo "rate compare: $same scripts the same, $diff differ"
[ "$diff" -eq 0 ]
