#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# JT-80x0 on VIBE FM, P2's check (Kris 23 Nov 2026): the .wasm (sh vibe/build.sh first) against the engine built natively,
# at 44.1 and 48 kHz – the same events, the same output hash. Needs the SDK (VIBE_SDK), Node and a C compiler.
set -e
cd "$(dirname "$0")/../.."
SDK=${VIBE_SDK:-$(pwd)/../vibe-fm-sdk}
OUT=${OUT:-build/vibe}
mkdir -p "$OUT"
fail=0
for rate in 44100 48000; do
    mkdir -p "$OUT/p2-$rate"
    python3 vibe/test/make_script.py $rate "$OUT/p2-$rate.json" vibe/test/events.h
    cc -std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-function -o "$OUT/ref" vibe/test/ref.c
    want=$("$OUT/ref" | sed -n 's/.*out_hash \([0-9a-f]*\).*/\1/p')
    got=$(node "$SDK/tools/wasmtest.mjs" "$SDK/build/jt80x0.wasm" "$OUT/p2-$rate.json" "$OUT/p2-$rate" $rate | sed -n 's/.*"out_hash":"\([0-9a-f]*\)".*/\1/p')
    if [ "$want" = "$got" ] && [ -n "$got" ]; then echo "$rate Hz: the .wasm = the engine, bit for bit ($got)"; else echo "$rate Hz: DIFFER (.wasm $got, native $want)"; fail=1; fi
done
# the 16-frame queue (main.c itself, natively): host blocks of any size = 128-frame blocks, sample for sample
cc -std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-function -I"$SDK/sdk/include" -o "$OUT/blocks" vibe/test/blocks.c
"$OUT/blocks" fixed "$OUT/fixed.raw" && "$OUT/blocks" random "$OUT/random.raw"
if cmp -s "$OUT/fixed.raw" "$OUT/random.raw"; then echo "blocks: random host blocks = 128-frame blocks, sample for sample"; else echo "blocks: DIFFER"; fail=1; fi
"$OUT/blocks" rate || fail=1
[ $fail = 0 ]
