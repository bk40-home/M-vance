#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# JT-80x0 on VIBE FM, P2's check (Kris 23 Nov 2026): the .wasm (sh vibe/build.sh first) against the engine built natively,
# at 44.1 and 48 kHz – the same events, the same output hash. Needs the SDK (VIBE_SDK), Node and a C compiler.
set -e
cd "$(dirname "$0")/../.."
SDK=${VIBE_SDK:-$(pwd)/../vibe-fm-sdk}
command -v node >/dev/null 2>&1 || { echo "vibe/test/run.sh: Node is needed (vibe-check, wasmtest): install Node 22, e.g. nvm install 22"; exit 1; }
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
# P2b: twice and four times those rates – the engine at a half / a quarter, doubled (jt_up.h): its samples exactly, every
# k-th, and the images 80 dB down (vibe/test/up_check.py, needs numpy)
for rk in 88200:2 96000:2 176400:4 192000:4; do
    rate=${rk%:*}; k=${rk#*:}
    mkdir -p "$OUT/p2-$rate"
    python3 vibe/test/make_script.py $rate "$OUT/p2-$rate.json" vibe/test/events.h
    cc -std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-function -o "$OUT/ref" vibe/test/ref.c
    "$OUT/ref" "$OUT/ref-$rate.raw" >/dev/null
    node "$SDK/tools/wasmtest.mjs" "$SDK/build/jt80x0.wasm" "$OUT/p2-$rate.json" "$OUT/p2-$rate" $rate >/dev/null
    python3 vibe/test/up_check.py "$OUT/p2-$rate/audio.wav" "$OUT/ref-$rate.raw" $k || fail=1
done
# the 16-frame queue (main.c itself, natively): host blocks of any size = 128-frame blocks, sample for sample
cc -std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-function -I"$SDK/sdk/include" -o "$OUT/blocks" vibe/test/blocks.c
for rate in 44100 96000 192000; do
    "$OUT/blocks" fixed "$OUT/fixed.raw" $rate && "$OUT/blocks" random "$OUT/random.raw" $rate
    if cmp -s "$OUT/fixed.raw" "$OUT/random.raw"; then echo "blocks $rate Hz: random host blocks = 128-frame blocks, sample for sample"; else echo "blocks $rate Hz: DIFFER"; fail=1; fi
done
"$OUT/blocks" rate || fail=1
# P3: the pages, edits, MIDI, sustain (main.c natively: ui_test check), and a played and edited run – the .wasm against
# main.c built natively, the same events at the same blocks, the same hash (44.1 kHz, and 96: through the upsampler)
cc -std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-function -I"$SDK/sdk/include" -o "$OUT/ui_test" vibe/test/ui_test.c -lm
"$OUT/ui_test" check | tail -1 | grep -q " 0 of " && echo "ui: $("$OUT/ui_test" check | tail -1)" || { "$OUT/ui_test" check | tail -20; fail=1; }
# P4: the DAW's parameters (ids frozen in vibe/test/param_ids.txt; each one set from the DAW lands in the engine)
"$OUT/ui_test" daw vibe/test/param_ids.txt | tail -1 | grep -q " 0 of " && echo "$("$OUT/ui_test" daw vibe/test/param_ids.txt | head -2 | tr '\n' ' ')- all set from the DAW land in the engine" || { "$OUT/ui_test" daw vibe/test/param_ids.txt | tail -20; fail=1; }
# P5: saving (main.c natively: ui_test store), and the .wasm restarted on its own files gives back the sound it left
"$OUT/ui_test" store | tail -1 | grep -q " 0 of " && echo "store: $("$OUT/ui_test" store | tail -1)" || { "$OUT/ui_test" store | tail -20; fail=1; }
ST=$(mktemp -d "$(pwd)/$OUT/store.XXXXXX")
mkdir -p "$ST/a" "$ST/b" "$ST/o1" "$ST/o2" "$ST/o3"
python3 vibe/test/make_store_scripts.py "$ST" "$ST/a" "$ST/b"
node "$SDK/tools/wasmtest.mjs" "$SDK/build/jt80x0.wasm" "$ST/s1.json" "$ST/o1" 48000 >/dev/null
h2=$(node "$SDK/tools/wasmtest.mjs" "$SDK/build/jt80x0.wasm" "$ST/s2.json" "$ST/o2" 48000 | sed -n 's/.*"out_hash":"\([0-9a-f]*\)".*/\1/p')
h3=$(node "$SDK/tools/wasmtest.mjs" "$SDK/build/jt80x0.wasm" "$ST/s3.json" "$ST/o3" 48000 | sed -n 's/.*"out_hash":"\([0-9a-f]*\)".*/\1/p')
files=$(cd "$ST/a" && find . -type f | sort | tr '\n' ' ')
if [ -n "$h2" ] && [ "$h2" = "$h3" ]; then echo "store: the .wasm restarted on its files = the same edits made fresh ($h2; files: $files)"; else echo "store: DIFFER (restarted $h2, fresh $h3; files: $files)"; fail=1; fi
for rate in 44100 96000; do
    mkdir -p "$OUT/ui-$rate"
    "$OUT/ui_test" script $rate "$OUT/ui-$rate.json"
    want=$("$OUT/ui_test" hash $rate | sed -n 's/.*out_hash \([0-9a-f]*\).*/\1/p')
    got=$(node "$SDK/tools/wasmtest.mjs" "$SDK/build/jt80x0.wasm" "$OUT/ui-$rate.json" "$OUT/ui-$rate" $rate | sed -n 's/.*"out_hash":"\([0-9a-f]*\)".*/\1/p')
    if [ "$want" = "$got" ] && [ -n "$got" ]; then echo "$rate Hz, played and edited: the .wasm = main.c natively ($got)"; else echo "$rate Hz, played and edited: DIFFER (.wasm $got, native $want)"; fail=1; fi
done
# P6a: the sequencer (main.c natively: ui_test seq), and a run recorded and then led by a host that starts mid-song and
# loops (wasmtest's "daw") – the .wasm against main.c natively, the same hash (44.1 kHz, and 96: through the upsampler)
"$OUT/ui_test" seq | tail -1 | grep -q " 0 of " && echo "seq: $("$OUT/ui_test" seq | tail -1)" || { "$OUT/ui_test" seq | tail -20; fail=1; }
for rate in 44100 96000; do
    mkdir -p "$OUT/seq-$rate"
    "$OUT/ui_test" seqscript $rate "$OUT/seq-$rate.json"
    want=$("$OUT/ui_test" seqhash $rate | sed -n 's/.*out_hash \([0-9a-f]*\).*/\1/p')
    got=$(node "$SDK/tools/wasmtest.mjs" "$SDK/build/jt80x0.wasm" "$OUT/seq-$rate.json" "$OUT/seq-$rate" $rate | sed -n 's/.*"out_hash":"\([0-9a-f]*\)".*/\1/p')
    if [ "$want" = "$got" ] && [ -n "$got" ]; then echo "$rate Hz, the sequencer led by a host: the .wasm = main.c natively ($got)"; else echo "$rate Hz, the sequencer led by a host: DIFFER (.wasm $got, native $want)"; fail=1; fi
done
[ $fail = 0 ]
