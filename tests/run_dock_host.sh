#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The fm1-dock module (dock/) on this computer (tests/dock_host_test.c): built with dock/module.cfg's switches against
# fm1-dock's own headers (an fm1-dock checkout: FM1_DOCK, default ../fm1-dock), driven through its descriptor by a
# stand-in host; then started again on the first run's flash. Also prints the module's RAM (its .data + .bss, this
# computer's 64-bit sizes: the chip's are a little smaller) against fm1-dock's 272 KiB. Kris 12 Nov.
#   sh tests/run_dock_host.sh            -> build/dock/dock.wav
set -e
cd "$(dirname "$0")/.."
DOCK=${FM1_DOCK:-../fm1-dock}
[ -f "$DOCK/firmware/dock/hb_module.h" ] || { echo "no fm1-dock checkout at $DOCK (FM1_DOCK=...)"; exit 1; }
OUT=build/dock
mkdir -p "$OUT"
CFG=$(sed -n 's/^cflags *= *//p' dock/module.cfg)
F="-std=gnu99 -Wall -Wextra -Werror -Wdouble-promotion -Wno-unused-function -fno-builtin -I$DOCK/firmware/dock -I$DOCK/modules/lib -Idock -Ifirmware/src -Ifirmware/gen -Ifirmware/hal $CFG"
cc $F -Os -c dock/mvance_module.c -o "$OUT/mvance_module.o"
cc $F -O2 -c firmware/src/mv_dsp.c -o "$OUT/mv_dsp.o"
python3 - "$OUT/mvance_module.o" "$OUT/mv_dsp.o" <<'PY'
import subprocess, sys
ram = 0
for o in sys.argv[1:]:
    for ln in subprocess.run(["size", "-A", o], capture_output=True, text=True).stdout.splitlines():
        f = ln.split()
        if len(f) >= 2 and (f[0].startswith(".bss") or f[0].startswith(".data") or f[0] == ".pool") and not f[0].startswith(".data.rel.ro"):
            ram += int(f[1])
print(f"module RAM (.data + .bss, 64-bit): {ram} B of 278528 (272 KiB): {278528 - ram} B free")
sys.exit(1 if ram > 278528 else 0)
PY
cc -std=gnu99 -O1 -Wall -Wextra -Werror -I"$DOCK/firmware/dock" -o "$OUT/dock_host_test" tests/dock_host_test.c "$OUT/mvance_module.o" "$OUT/mv_dsp.o"
(cd "$OUT" && ./dock_host_test dock.wav && ./dock_host_test dock.wav reload)
