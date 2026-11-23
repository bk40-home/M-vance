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
for t in mv_fixed_test mv_patch_test mv_pitch_test mv_env_test mv_lfo_test mv_jvtri_test mv_arp_test mv_seq_test mv_seq_auto_test mv_clip_test mv_lim_test mv_prog_test mv_prog1_test mv_voice_test; do
    cc $CF -o "$OUT/$t" "tests/$t.c" -lm
    "$OUT/$t"
done
# the rate chosen at start (mv_rate.h, the VIBE FM build): 44.1 / 48 kHz in seconds and hertz (Kris 23 Nov 2026)
cc $CF -DMV_RATE_RT -o "$OUT/mv_rate_test" tests/mv_rate_test.c -lm
"$OUT/mv_rate_test"
python3 tests/xfer_test.py                     # the program transfer (mv_xfer.h) on the host build
python3 tests/seq_xfer_test.py                 # the tracks and clips over SysEx (mv_seqx.h, Kris 20 Nov) on the host build
python3 tests/seq_auto_host_test.py            # the sequencer's automation: recorded, played, takeover, STOP (Kris 20 Nov)
# the patch library page (docs/library.html, Kris 15 Nov): fm1-kit's model, and M-vance's adapter against the firmware's
# factory bank and clamps, the pretend FM-1, and the host build's own dump (xfer_test.py writes it). Needs Node
if command -v node >/dev/null 2>&1; then
    node third_party/fm1-kit/library/test/model_test.js
    node third_party/fm1-kit/web/test/fm1ota_test.mjs .   # the firmware page's installer, and as tools/fm1_install.py
    python3 tools/gen_library_page.py --data "$OUT/library_data.json" >/dev/null
    node tests/library_test.js "$OUT/library_data.json"
    node sim/bank/bank-test.mjs && node sim/bank/link-test.mjs   # (the SysEx and link code the editor and library use)
    node sim/seq-model-test.mjs build/host/mvance-seqx   # the Sequences panel's SysEx, and against the host build (Kris 20 Nov)
    node sim/seq-midi-test.mjs                    # its MIDI import: the same loops and events as tools/mid2clip.py
    node sim/mirror-test.mjs build/host/mvance-seqx   # Link FM-1: what it plays, mirrored into the sim (Kris 22 Nov)
else
    echo "note: the library tests skipped (no node)"
fi
python3 tests/mid2clip_test.py                 # the factory clips tool (tools/mid2clip.py): loops, coding
python3 tests/fx_sync_import_test.py           # DELAY / CHORUS SYNC through the three importers (Kris 7 Nov)
# the USB audio output (firmware/src/uac_out.c: integer port of felucca-bsp's, against its float law) and the USB
# descriptors with it (firmware/src/usb.c; -Wno-int-to-pointer-cast: hal/'s register addresses on a 64-bit computer)
cc $CF -D_DEFAULT_SOURCE -Wdouble-promotion -o "$OUT/uac_out_test" tests/uac_out_test.c -lm
"$OUT/uac_out_test"
cc -std=c99 -O1 -Wall -Wextra -Werror -Wno-unused-function -Wno-int-to-pointer-cast -o "$OUT/uac_desc_test" tests/uac_desc_test.c
"$OUT/uac_desc_test"
