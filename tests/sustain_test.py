#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""JT-80x0 (FM-1): the sustain pedal (fk_sustain, Kris 25 Nov 2026), through the whole app on the host (tests/host.c).
Program A-1 (Pluck 01: amp SUSTAIN 127, RELEASE 12). A key (F3) and a MIDI note (E4) go down, the pedal (CC 64) comes
down, both are let go, and the pedal comes up at 2.0 s. The same run without the pedal must have died away by 1.0 s;
with it, the two notes ring on until 2.0 s, and are gone by 2.6 s. Run from the repo root (tests/run_unit.sh does)."""
import os, struct, subprocess, sys, tempfile
OUT = os.environ.get("OUT", "build/unit")
exe = f"{OUT}/mvance-sus"
subprocess.run(["cc", "-O2", "-w", "-DDINGHY_HOST", '-DAPP_VER="dev"', "-Ifirmware/hal", "-Ifirmware/src", "-Ifirmware/gen",
                "-o", exe, "firmware/src/mvance.c", "firmware/src/mv_dsp.c"], check=True)
SCRIPT = """200 keys 1
200 midi 0x90 64 100
{pedal_on}
600 keys 0
600 midi 0x80 64 0
{pedal_off}
3200 end
"""
def run(pedal):
    with tempfile.TemporaryDirectory() as td:
        sc = os.path.join(td, "s.txt")
        open(sc, "w").write(SCRIPT.format(pedal_on="400 midi 0xB0 64 127" if pedal else "",
                                          pedal_off="2000 midi 0xB0 64 0" if pedal else ""))
        subprocess.run([exe, os.path.join(td, "o"), sc], check=True, capture_output=True)
        w = open(os.path.join(td, "o.wav"), "rb").read()
    ch = struct.unpack("<H", w[22:24])[0]; fs = struct.unpack("<I", w[24:28])[0]
    n = (len(w) - 44) // 2
    x = struct.unpack(f"<{n}h", w[44:44 + 2 * n])
    def rms(a, b):
        s = x[int(a * fs) * ch:int(b * fs) * ch]
        return (sum(v * v for v in s) / max(len(s), 1)) ** 0.5
    return rms(0.25, 0.55), rms(1.0, 1.9), rms(2.6, 3.1)
held, ring, after = run(True)
_, dry, _ = run(False)
print(f"sustain: held {held:.0f}, pedal down after let-go {ring:.0f} (without the pedal {dry:.0f}), pedal up {after:.0f} (RMS)")
ok = held > 300 and ring > 0.3 * held and dry < 0.02 * held and after < 0.02 * held
print("sustain_test:", "ok" if ok else "FAILED")
sys.exit(0 if ok else 1)
