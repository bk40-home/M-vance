#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the M-vance online simulator (WebAssembly). Needs Emscripten (emcc) on PATH.
# First generate the public bank the firmware embeds (if you haven't):
#   python3 tools/make_bank.py tools/bank/public.txt          # -> firmware/gen/mv_factory_public.h
# Then, from the repo root:
#   sh sim/build.sh
# Output lands in docs/sim/ (mvance.js, mvance.wasm, index.html, sim.js, pages.js, audio-worklet.js, editor.html, editor.js).
# The patch library (docs/library.html) is not built here: python3 tools/gen_library_page.py. Serve docs/ and
# open the page in Chrome or Edge, e.g.:
#   python3 -m http.server --directory docs 8000    # then http://localhost:8000/sim/
set -e
cd "$(dirname "$0")/.."
python3 tools/gen_params.py                            # firmware/gen/mv_params.h  (so a standalone sim build works)
python3 tools/gen_tables.py                            # firmware/gen/mv_tables.h
OUT=docs/sim
mkdir -p "$OUT"
cp sim/index.html sim/sim.js sim/pages.js sim/engine.js sim/audio-worklet.js "$OUT"/
cp sim/mirror.js "$OUT"/                                  # Link FM-1: what it plays, mirrored into the sim (Kris 22 Nov)
cp sim/editor.html sim/editor.js "$OUT"/                 # the sound editor (Kris 15 Nov; the banks: docs/library.html)
python3 tools/gen_seq_ccs.py -o sim/seq-ccs.js >/dev/null   # the sound's CCs (automation), from mv_params.h
cp sim/seq-model.js sim/seq-midi.js sim/seq-roll.js sim/seq-auto.js sim/seq-ccs.js sim/seq-panel.js sim/seq-picker.js "$OUT"/   # its Sequences panel (Kris 20 Nov: mv_seqx.h; the Factory clips dialog, 21 Nov)
cp third_party/fm1-kit/web/theme.css third_party/fm1-kit/web/theme.js third_party/fm1-kit/web/count.js third_party/fm1-kit/web/sim.css "$OUT"/   # the look and the page counter (fm1-kit)
BUILD=$(date +%Y%m%d%H%M%S)                            # the build stamp: index.html and sim.js ask for
sed -i "s/__BUILD__/$BUILD/g" "$OUT/index.html" "$OUT/sim.js" "$OUT/editor.html" "$OUT/editor.js" "$OUT/seq-midi.js" "$OUT/seq-roll.js" "$OUT/seq-auto.js" "$OUT/seq-panel.js" "$OUT/seq-picker.js" "$OUT/mirror.js"   # ?v=<stamp>, so no stale cached script survives
mkdir -p "$OUT/bank"                                   # the SysEx code the editor imports; the old bank editor (no
                                                       # longer linked: retire it once the library and editor pass)
cp sim/bank/bank-model.js sim/bank/fm1-midi.js sim/bank/link.js sim/bank/editor.js sim/bank/editor.html "$OUT/bank/"
sed -i "s/__BUILD__/$BUILD/g" "$OUT/bank/editor.html" "$OUT/bank/editor.js"   # (the editor too: Part B)
emcc -O2 -DDINGHY_WEBSIM -DAPP_VER='"sim"' \
    -Ifirmware/hal -Ifirmware/src -Ifirmware/gen \
    firmware/src/mvance.c firmware/src/mv_dsp.c \
    -sASYNCIFY -sEXIT_RUNTIME=0 -sALLOW_MEMORY_GROWTH=1 \
    -sMODULARIZE=1 -sEXPORT_NAME=MVance -sENVIRONMENT=web \
    -sEXPORTED_RUNTIME_METHODS=cwrap,ccall,UTF8ToString,HEAPU8,HEAPF32,HEAPU16 \
    -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_wsim_pull,_wsim_fb,_wsim_keys,_wsim_btn,_wsim_enc,_wsim_midi,\
_wsim_sysex_in,_wsim_npages,_wsim_page_tile,_wsim_page_title,_wsim_page_btn,_wsim_page_sub,_wsim_live_page,_wsim_page_key,_wsim_tile,_wsim_goto \
    -o "$OUT/mvance.js"
echo "built -> $OUT/ (mvance.js, mvance.wasm, index.html, sim.js, pages.js, audio-worklet.js)"
