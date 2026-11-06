#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the M-vance online simulator (WebAssembly). Needs Emscripten (emcc) on PATH.
# First generate the public bank the firmware embeds (if you haven't):
#   python3 tools/make_bank.py tools/bank/public.txt          # -> firmware/gen/mv_factory_public.h
# Then, from the repo root:
#   sh sim/build.sh
# Output lands in docs/sim/ (mvance.js, mvance.wasm, index.html, sim.js, audio-worklet.js). Serve docs/ and
# open the page in Chrome or Edge, e.g.:
#   python3 -m http.server --directory docs 8000    # then http://localhost:8000/sim/
set -e
cd "$(dirname "$0")/.."
python3 tools/gen_params.py                            # firmware/gen/mv_params.h  (so a standalone sim build works)
python3 tools/gen_tables.py                            # firmware/gen/mv_tables.h
OUT=docs/sim
mkdir -p "$OUT"
cp sim/index.html sim/sim.js sim/audio-worklet.js "$OUT"/
mkdir -p "$OUT/bank"                                   # the bank editor (linked from the sim page)
cp sim/bank/bank-model.js sim/bank/editor.js sim/bank/editor.html "$OUT/bank/"
emcc -O2 -DDINGHY_WEBSIM -DAPP_VER='"sim"' \
    -Ifirmware/hal -Ifirmware/src -Ifirmware/gen \
    firmware/src/mvance.c firmware/src/mv_dsp.c \
    -sASYNCIFY -sEXIT_RUNTIME=0 -sALLOW_MEMORY_GROWTH=1 \
    -sMODULARIZE=1 -sEXPORT_NAME=MVance -sENVIRONMENT=web \
    -sEXPORTED_RUNTIME_METHODS=cwrap,ccall,HEAPF32,HEAPU16 \
    -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_wsim_pull,_wsim_fb,_wsim_keys,_wsim_btn,_wsim_enc,_wsim_midi \
    -o "$OUT/mvance.js"
echo "built -> $OUT/ (mvance.js, mvance.wasm, index.html, sim.js, audio-worklet.js)"
