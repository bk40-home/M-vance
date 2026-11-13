#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The simulator's tiles (sim/mv_tiles.c) and its SysEx link (Part B) tested natively: the app + sim/mv_websim.c built with cc, with
# tests/websim_stub/emscripten.h in place of Emscripten's (no emcc needed). Exit 0 = all checks passed.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
cc -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -DDINGHY_WEBSIM -DAPP_VER='"sim"' \
   -Itests/websim_stub -Ifirmware/hal -Ifirmware/src -Ifirmware/gen \
   -o build/host/websim_tiles firmware/src/mvance.c firmware/src/mv_dsp.c tests/websim_tiles_test.c
build/host/websim_tiles
cc -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -DDINGHY_WEBSIM -DAPP_VER='"sim"' \
   -Itests/websim_stub -Ifirmware/hal -Ifirmware/src -Ifirmware/gen \
   -o build/host/websim_link firmware/src/mvance.c firmware/src/mv_dsp.c tests/websim_link_test.c
build/host/websim_link
