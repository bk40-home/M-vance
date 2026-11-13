#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The bank editor's link on the host build (Kris 13 Nov: fm1-port/mvance-partB-hardware-sync-proposal-2026-11-13.md,
1b 2a 3a). A page that reads what plays (RQ1 01 00) is linked for 30 s; while linked, every change to what plays goes
out as a DT1 of exactly the changed bytes; the page's own writes are not echoed; a read of what plays leaves the screen
alone (a slot read still shows the transfer screen).

  python3 tests/link_test.py [HOST_BINARY]   (default: builds build/host/mvance-link)
Linux / macOS / WSL. Exit 0: all checks passed."""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
checks = fails = 0

# the FM-1 layout (platform.c): buttons and encoders the scripts use
BTN_SEL, BTN_GLO = 3, 7
ENC_KNOB1, ENC_KNOB3, ENC_PRESETS = 2, 4, 6
CUTOFF = 41                                     # FILTER 1/2 KNOB3: patch byte 41 (mv_ui.c PAGES)


def check(ok, what):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print("FAIL", what)


def sysex(cmd, addr, data):
    s = (128 - (sum(addr) + sum(data)) % 128) % 128
    return bytes([0xF0, 0x41, 0x10, 0x00, 0x06, cmd] + list(addr) + list(data) + [s, 0xF7])


RQ_TEMP = sysex(0x11, [1, 0, 0, 0], [0, 0, 0, 0])         # read what plays (links)
RQ_SLOT = sysex(0x11, [3, 0, 0, 0], [0, 0, 0, 0])         # read performance 1 (a slot: the transfer screen)


def sx_line(t, m):
    return f"{t} sx " + " ".join(f"{b:02X}" for b in m) + "\n"


def msgs(b):
    return [m.group(0) for m in re.finditer(rb"\xF0[^\xF7]*\xF7", b)]


def addr(m):
    return tuple(m[6:10])


def data(m):
    return m[10:-2]


def run(exe, tmp, name, lines, end):
    (tmp / (name + ".txt")).write_text("".join(lines) + f"{end} end\n")
    out = subprocess.run([str(exe), str(tmp / name), str(tmp / (name + ".txt"))], capture_output=True, text=True,
                         check=True).stdout
    syx = tmp / (name + ".syx")
    return out, msgs(syx.read_bytes()) if syx.exists() else []


class Sound:
    """what plays, as a page holds it: upper / lower 248 bytes, the 16 performance bytes, built from DT1s to 01 00"""
    def __init__(self):
        self.up, self.lo, self.pf = bytearray(248), bytearray(248), bytearray(16)

    def apply(self, m):
        a = addr(m)
        if a[0] != 1 or a[1] != 0:
            return False
        area, off, d = a[2], a[3], data(m)
        if area in (0x40, 0x41, 0x42, 0x43):
            t = self.up if area < 0x42 else self.lo
            i = (area & 1) * 128 + off
            t[i:i + len(d)] = d
        elif area == 0x70:
            self.pf[off:off + len(d)] = d
        elif area == 0x00:
            pass                                # (common: the name and MODE/SPLIT/BAL; the patch and 70 carry them)
        else:
            return False
        return True


def main():
    exe = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/host/mvance-link"
    if len(sys.argv) <= 1:
        exe.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DDINGHY_HOST", '-DAPP_VER="link"',
                        "-Ifirmware/hal", "-Ifirmware/src", "-Ifirmware/gen", "-o", str(exe),
                        "firmware/src/mvance.c", "firmware/src/mv_dsp.c"], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)

        # 1 not linked: a panel edit sends its CC and no SysEx at all (today's behaviour)
        out, m = run(exe, tmp, "a", [f"500 btn {BTN_SEL}\n", f"800 enc {ENC_KNOB3} -5\n"], 1500)
        check(m == [], "not linked: no SysEx out")
        check(re.search(r"MIDI out B0 74 ", out) is not None, "not linked: CUTOFF's CC (74) still goes out")

        # 2 linked: the read is answered (4 DT1s), then one DT1 for the edit: exactly CUTOFF, its new value
        out, m = run(exe, tmp, "b", [sx_line(500, RQ_TEMP), f"1000 btn {BTN_SEL}\n", f"1300 enc {ENC_KNOB3} -5\n"],
                     2500)
        check(len(m) == 5, f"linked: a 4-message dump then 1 report ({len(m)} messages)")
        s = Sound()
        check(all(s.apply(x) for x in m[:4]), "the dump is what plays (01 00 ..)")
        before = s.up[CUTOFF]
        if len(m) == 5:
            check(addr(m[4]) == (1, 0, 0x40, CUTOFF) and len(data(m[4])) == 1,
                  f"the report is 01 00 40 {CUTOFF:02X}, one byte ({addr(m[4])}, {len(data(m[4]))} bytes)")
            check(data(m[4])[0] == max(0, before - 5), f"CUTOFF {before} -> {data(m[4])[0]} (5 down)")
            check(re.search(r"MIDI out B0 74 ", out) is not None, "linked: the CC goes out too (DAWs unchanged)")

        # 3 the page's own write is applied and not echoed; a 1-byte DT1 changes exactly that byte
        w = sysex(0x12, [1, 0, 0x40, CUTOFF], [17])
        _, m = run(exe, tmp, "c", [sx_line(500, RQ_TEMP), sx_line(1000, w), sx_line(1500, RQ_TEMP)], 2500)
        check(len(m) == 8, f"a page write: no echo, two dumps only ({len(m)} messages)")
        if len(m) == 8:
            s1, s2 = Sound(), Sound()
            for x in m[:4]:
                s1.apply(x)
            for x in m[4:]:
                s2.apply(x)
            diff = [i for i in range(248) if s1.up[i] != s2.up[i]]
            check(diff == [CUTOFF] and s2.up[CUTOFF] == 17, f"1-byte DT1: only byte {CUTOFF} changed, to 17 ({diff})")
            check(s1.lo == s2.lo and s1.pf == s2.pf, "the lower patch and performance bytes untouched")

        # 4 a performance byte (PERFORM 1/2 MODE) goes out on 01 00 70 00
        glo = [f"{1000 + 200 * k} btn {BTN_GLO}\n" for k in range(3)]   # PLAY MODE 1/2, 2/2, PERFORM 1/2
        _, m = run(exe, tmp, "d", [sx_line(500, RQ_TEMP)] + glo + [f"2000 enc {ENC_KNOB1} 1\n"], 3000)
        rep = m[4:]
        check(len(rep) == 1 and addr(rep[0]) == (1, 0, 0x70, 0) and data(rep[0]) == b"\x01",
              f"PERFORM MODE -> DUAL reported as 01 00 70 00 = 01 ({[(addr(x), bytes(data(x))) for x in rep]})")

        # 5 a program change (PRESETS) while linked: the reports bring the page's copy to the new program exactly
        _, m = run(exe, tmp, "e", [sx_line(500, RQ_TEMP), f"1000 enc {ENC_PRESETS} 1\n", sx_line(2000, RQ_TEMP)], 3000)
        s, d2 = Sound(), Sound()
        n_rep = len(m) - 8
        for x in m[:4 + n_rep]:
            s.apply(x)
        for x in m[4 + n_rep:]:
            d2.apply(x)
        check(1 <= n_rep <= 3, f"program change: 1..3 reports (one a region) ({n_rep})")
        check((s.up, s.lo, s.pf) == (d2.up, d2.lo, d2.pf), "dump + reports == a fresh dump of the new program")

        # 6 the link lapses 30 s after the last read; a renewed read keeps it
        _, m = run(exe, tmp, "f", [sx_line(500, RQ_TEMP), f"31000 btn {BTN_SEL}\n", f"31300 enc {ENC_KNOB3} -5\n"],
                   32000)
        check(len(m) == 4, f"lapsed after 30 s: no report ({len(m) - 4})")
        _, m = run(exe, tmp, "g", [sx_line(500, RQ_TEMP), sx_line(20000, RQ_TEMP), f"31000 btn {BTN_SEL}\n",
                                   f"31300 enc {ENC_KNOB3} -5\n"], 32000)
        check(len(m) == 9, f"renewed at 20 s: still linked at 31 s ({len(m)} messages, 9 expected)")

        # 7 3a: a read of what plays leaves the screen alone; a slot read still shows the transfer screen
        run(exe, tmp, "h0", ["900 shot\n"], 1200)
        run(exe, tmp, "h1", [sx_line(500, RQ_TEMP), "900 shot\n"], 1200)
        run(exe, tmp, "h2", [sx_line(500, RQ_SLOT), "900 shot\n"], 1200)
        p0, p1, p2 = (tmp / f"h{k}-900.ppm" for k in range(3))
        ok = p0.exists() and p1.exists() and p2.exists()
        check(ok, "screenshots taken")
        if ok:
            check(p0.read_bytes() == p1.read_bytes(), "a read of what plays: the screen unchanged")
            check(p0.read_bytes() != p2.read_bytes(), "a slot read: the transfer screen shows (as before)")

    print(f"link: {checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
