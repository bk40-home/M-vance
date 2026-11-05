#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# M-vance unit tests on this computer (no FM-1, no JieLi toolchain): the patch table and I/O, then each DSP part
# as it is ported. Strict flags: what passes here also has to pass the chip's C compiler.
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-build/unit}
mkdir -p "$OUT"
CF="-std=c99 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware/src -Ifirmware/gen -Ifirmware/hal"
python3 tools/gen_params.py
python3 tools/gen_tables.py
git diff --quiet -- firmware/gen/ 2>/dev/null || echo "note: firmware/gen/ changed: commit it"
# the device build's source checks that need no JieLi toolchain (register literals, volatile casts, asm outside
# hal/): caught here first. The ROM-constant check reads the chip's disassembly, so only ./build.sh can run it.
python3 -c "import sys; sys.path.insert(0, 'tools'); import build; e = build.mmio_check(); print(*e, sep='\n'); sys.exit(1 if e else 0)"
echo "mmio_check: ok"
for t in mv_fixed_test mv_patch_test mv_pitch_test mv_env_test mv_lfo_test mv_jvtri_test mv_arp_test mv_lim_test mv_prog_test mv_voice_test; do
    cc $CF -o "$OUT/$t" "tests/$t.c" -lm
    "$OUT/$t"
done
python3 tests/xfer_test.py                     # the program transfer (mv_xfer.h) on the host build
# the USB audio output (firmware/src/uac_out.c: integer port of felucca-bsp's, against its float law) and the USB
# descriptors with it (firmware/src/usb.c; -Wno-int-to-pointer-cast: hal/'s register addresses on a 64-bit computer)
cc $CF -D_DEFAULT_SOURCE -Wdouble-promotion -o "$OUT/uac_out_test" tests/uac_out_test.c -lm
"$OUT/uac_out_test"
cc -std=c99 -O1 -Wall -Wextra -Werror -Wno-unused-function -Wno-int-to-pointer-cast -o "$OUT/uac_desc_test" tests/uac_desc_test.c
"$OUT/uac_desc_test"
