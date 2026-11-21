#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The sequencer's automation on the host build (Kris 20 Nov: fm1-port/mvance-seq-automation-proposal-2026-11-20.md):
a CUTOFF sweep sent as MIDI CC 74 while UPPER's track is armed is recorded as points; disarmed, each loop plays it
back (the sound follows, the same at the same place in each loop); a CC turned during playback takes CUTOFF over until
the loop goes round; STOP puts CUTOFF back to what it was at PLAY; the points read back over SysEx area 04 (on the
4-tick grid, as the wire carries them); a script with no automation is untouched (the WAV regression does that).

  python3 tests/seq_auto_host_test.py [HOST_BINARY]   (default: builds build/host/mvance-seqx)"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
from seq_xfer_test import sysex, addr, head, rq, msgs, run, check   # noqa: E402  (the framing and the runner)
import seq_xfer_test as X                                            # noqa: E402

CUT = 41                                        # CUTOFF's patch byte (CC 74)
LOOP = 2000                                     # ms: 1 bar at 120 BPM


def temp_rq():
    return sysex(0x11, [0x01, 0, 0, 0], [0, 0, 0, 0])


def cutoffs(dump):
    """CUTOFF in each whole upper patch in a dump (01 00 40 00, 248 bytes), in order"""
    return [m[10 + CUT] for m in msgs(dump) if len(m) > 200 and m[6:10] == bytes([1, 0, 0x40, 0])]


def main():
    exe = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/host/mvance-seqx"
    if len(sys.argv) < 2:
        exe.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["cc", "-O2", "-Wno-unused-function", "-DDINGHY_HOST", '-DAPP_VER="seqx"', "-Ifirmware/hal",
                        "-Ifirmware/src", "-Ifirmware/gen", "-o", str(exe), "firmware/src/mvance.c",
                        "firmware/src/mv_dsp.c"], cwd=ROOT, check=True)
    base = [(100, sysex(0x12, addr(0, 0), head(1, 0x7F, 1, 0))),   # UPPER: empty, LENGTH 1 bar
            (200, "midi 0xB0 74 20"),           # CUTOFF 20 before PLAY
            (400, "btn 13")]                    # REC: UPPER armed, the transport from bar 1 (t = 400)
    sweep = [(500 + 20 * i, f"midi 0xB0 74 {30 + 2 * i}") for i in range(41)]   # 30 .. 110 over phase 100..900 ms
    rec = base + sweep + [(2300, "btn 13")]     # REC again: disarmed (still running)
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        # 1. playback: the same CUTOFF at the same place in two loops; another place, another value
        lines = rec + [(400 + 2 * LOOP + 600, temp_rq()), (400 + 3 * LOOP + 600, temp_rq()),
                       (400 + 3 * LOOP + 1500, temp_rq()), (400 + 4 * LOOP + 300, "btn 12"),   # PLAY: stop
                       (400 + 4 * LOOP + 700, temp_rq()), (400 + 4 * LOOP + 900, rq(0)), (9800, "end")]
        _, dump = run(exe, tmp, "auto", lines)
        c = cutoffs(dump)
        check(len(c) == 4, f"four reads of what plays ({len(c)})")
        if len(c) == 4:
            check(c[0] == c[1] and 50 <= c[0] <= 90, f"phase 600 ms, two loops apart: the same CUTOFF ({c[0]}, {c[1]})")
            check(c[2] >= 105, f"phase 1500 ms: the sweep's end ({c[2]})")
            check(c[3] == 20, f"STOP: CUTOFF back to its value at PLAY (20; {c[3]})")
        r = X.reads(dump, 0)
        pts = [(tk, n, v & 0x7F) for tk, n, v in (r[-1][1] if r else []) if v & 0x80]
        check(r and len(pts) >= 10 and len(pts) == len(r[-1][1]) and all(n == 74 for _, n, _ in pts), f"the track holds the sweep's points ({len(pts)})")
        check(all(tk % 4 == 0 for tk, _, _ in pts) and pts == sorted(pts), "on the 4-tick grid, in order")
        check(pts and max(v for _, _, v in pts) == 110 and min(v for _, _, v in pts) <= 34, "from 30-ish to 110")
        check(r and r[-1][0][3] == 0x7F, "no ROOT (points are no notes)")
        # 2. takeover: CC 74 = 5 in loop 3 (not armed): the points leave it until the loop goes round
        lines = rec + [(400 + 2 * LOOP + 200, "midi 0xB0 74 5"), (400 + 2 * LOOP + 600, temp_rq()),
                       (400 + 3 * LOOP + 600, temp_rq()), (9000, "end")]
        _, dump = run(exe, tmp, "take", lines)
        c = cutoffs(dump)
        check(len(c) == 2 and c[0] == 5, f"a CC during playback takes CUTOFF over ({c[:1]})")
        check(len(c) == 2 and 50 <= c[1] <= 90, f"the next loop: the points again ({c[1:]})")
        # 3. a write from the page with points: played (the wire's point form)
        pts2 = [(0, 74, 10), (192, 74, 100)]
        ev = []
        for tk, n, v in pts2:
            ev += [0x60 | tk >> 9, tk >> 2 & 0x7F, n, v]
        lines = [(100, sysex(0x12, addr(0, 32), ev)), (140, sysex(0x12, addr(0, 0), head(1, 0x7F, 1, 2))),
                 (300, "btn 12"), (300 + 250, temp_rq()), (300 + 1250, temp_rq()), (300 + 1500, rq(0)), (2400, "end")]
        _, dump = run(exe, tmp, "page", lines)
        c = cutoffs(dump)
        check(c == [10, 100], f"page-written points played at their places ({c})")
        r = X.reads(dump, 0)
        check(r and r[-1][1] == [(0, 74, 0x80 | 10), (192, 74, 0x80 | 100)], "and read back as written")
    print(f"seq_auto_host: {X.checks} checks, {X.fails} failed")
    return 1 if X.fails else 0


if __name__ == "__main__":
    sys.exit(main())
