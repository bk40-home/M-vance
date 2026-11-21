#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The sequence transfer on the host build (mv_seqx.h; Kris 20 Nov, fm1-port/mvance-seq-editor-proposal-2026-11-20.md):
the editor page writes and reads the live tracks and the user clips over SysEx area 04, and reads the factory clips.

  python3 tests/seq_xfer_test.py [HOST_BINARY]   (default: builds build/host/mvance-seqx)
Linux / macOS / WSL. Exit 0: all checks passed."""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
checks = fails = 0
BAR = 384


def check(ok, what):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print("FAIL", what)


def msgs(b):
    return [m.group(0) for m in re.finditer(rb"\xF0[^\xF7]*\xF7", b)]


def sysex(cmd, addr, body):
    s = (128 - (sum(addr) + sum(body)) % 128) % 128
    return bytes([0xF0, 0x41, 0x10, 0x00, 0x06, cmd] + list(addr) + list(body) + [s, 0xF7])


def addr(tt, off):
    return [0x04, tt, off >> 7 & 0x7F, off & 0x7F]


def head(bars, root, quant, n, name=b"      ", flags=0):
    h = [ord("C"), 2, bars, root, quant, flags, n & 0x7F, n >> 7 & 0x7F] + list(name[:6].ljust(6))
    return h + [0] * (32 - len(h))


def write(tt, events, bars=2, root=48, quant=1, name=b"      ", n=None):
    """the messages of a write: events 32 a message, then the head (n: the head's count, if not the events')"""
    out = []
    for i in range(0, len(events), 32):
        body = []
        for t, nt, v in events[i:i + 32]:
            body += [t >> 7, t & 0x7F, nt, v]
        out.append(sysex(0x12, addr(tt, 32 + 4 * i), body))
    out.append(sysex(0x12, addr(tt, 0), head(bars, root, quant, len(events) if n is None else n, name)))
    return out


def rq(tt):
    return sysex(0x11, addr(tt, 0), [0, 0, 0, 0])


def reads(dump, tt):
    """the reads of tt in a dump: a list of (head, events), each from a head to its last event"""
    got, cur = [], None
    for m in msgs(dump):
        if len(m) < 12 or m[5] != 0x12 or m[6] != 0x04 or m[7] != tt:
            continue
        off = m[8] << 7 | m[9]
        d = list(m[10:-2])
        if off == 0:
            cur = (d, [])
            got.append(cur)
        elif cur is not None:
            check(off == 32 + 4 * len(cur[1]), f"tt {tt:02X}: events in order (offset {off})")
            for i in range(0, len(d), 4):      # (an automation point: 60 | tick >> 9, tick >> 2: its velocity | 80)
                if d[i] >= 0x60:
                    cur[1].append(((d[i] & 0x1F) << 9 | d[i + 1] << 2, d[i + 2], 0x80 | d[i + 3]))
                else:
                    cur[1].append((d[i] << 7 | d[i + 1], d[i + 2], d[i + 3]))
    return got


def run(exe, tmp, name, lines):
    """a script of (ms, message bytes or a script line)"""
    txt = []
    for i, (t, m) in enumerate(lines):         # a message as a one-message .syx (a script line holds 255 characters)
        if isinstance(m, bytes):
            (tmp / f"{name}-{i}.syx").write_bytes(m)
            txt.append(f"{t} syx {tmp / f'{name}-{i}.syx'}")
        else:
            txt.append(f"{t} {m}")
    (tmp / (name + ".txt")).write_text("\n".join(txt) + "\n")
    out = subprocess.run([str(exe), str(tmp / name), str(tmp / (name + ".txt"))], capture_output=True, text=True,
                         check=True).stdout
    dump = tmp / (name + ".syx")
    return out, dump.read_bytes() if dump.exists() else b""


def paced(t0, ms, step=40):
    return [(t0 + i * step, m) for i, m in enumerate(ms)]


def notes(spec):
    """[(on tick, len, note, vel)] -> events sorted as a track keeps them (offs before ons at the same tick)"""
    ev = []
    for t, ln, nt, v in spec:
        ev.append((t, nt, v))
        ev.append((t + ln, nt, 0))
    return sorted(ev, key=lambda e: (e[0], e[2] != 0))


def fclips():
    """the factory clips the host build has (firmware/gen/mv_fclips_local.h, if any): [(name, bars, root, n, events)]"""
    p = ROOT / "firmware/gen/mv_fclips_local.h"
    if not p.exists():
        return []
    src = p.read_text()
    arrs = {m.group(1): [int(x) for x in re.sub(r"/\*.*?\*/", " ", m.group(2), flags=re.S).split(",") if x.strip()]
            for m in re.finditer(r"MV_FCLIP_EV(\d+)\[\d+\] = \{(.*?)\};", src, re.S)}
    out = []
    for m in re.finditer(r'\{"([^"]*)", (\d+), (\d+), (\d+), (\d+), MV_FCLIP_EV(\d+)\}', src):
        name, bars, root, n, nb, k = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), m.group(6)
        b, pos, tick, vel, ev = arrs[k][:nb], 0, 0, 100, []
        while pos < len(b):
            d = sh = 0
            while True:
                x = b[pos]; pos += 1
                d |= (x & 127) << sh; sh += 7
                if not (x & 128) or pos >= len(b) or sh >= 28:
                    break
            if pos >= len(b):
                break
            tick += d >> 1
            x = b[pos]; pos += 1
            if d & 1 and pos < len(b):
                vel = b[pos]; pos += 1
            ev.append((tick, x & 127, (vel if vel else 1) if x & 128 else 0))
        out.append((name, bars, root, n, ev))
    return out


def main():
    exe = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/host/mvance-seqx"
    if len(sys.argv) < 2:
        exe.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["cc", "-O2", "-Wno-unused-function", "-DDINGHY_HOST", '-DAPP_VER="seqx"', "-Ifirmware/hal",
                        "-Ifirmware/src", "-Ifirmware/gen", "-o", str(exe), "firmware/src/mvance.c",
                        "firmware/src/mv_dsp.c"], cwd=ROOT, check=True)
    A = notes([(0, 48, 48, 100), (96, 48, 51, 90), (192, 48, 55, 80), (384, 96, 60, 127)])
    B = notes([(i * 96, 40, 36 + (i % 4) * 3, 64 + i) for i in range(32)])   # 8 bars of quarters: 64 events
    W = [(0, 50, 0), (300, 50, 100)]            # a note held over the loop's end (its off first): as recorded
    BIG = notes([(i * 24, 12, 40 + i % 24, 100) for i in range(128)])        # 128 notes: a full track (256 events)
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        # 1. a live track written, then read back: the same (UPPER 2 bars, LOWER 4 bars while running)
        lines = paced(100, write(0, A))
        lines += [(600, "midi 0xFA 0 0")]       # START: the sequencer runs
        lines += paced(700, write(1, B, bars=8, root=36, quant=2))
        lines += [(1200, rq(0)), (1500, rq(1)), (2500, "end")]
        out, dump = run(exe, tmp, "tracks", lines)
        r0, r1 = reads(dump, 0), reads(dump, 1)
        check(len(r0) == 1 and r0[0][1] == A, "UPPER: read back as written")
        check(len(r0) == 1 and r0[0][0][2] == 2 and r0[0][0][3] == 48 and r0[0][0][6] == len(A), "UPPER: LENGTH 2, ROOT 48, n")
        check(len(r1) == 1 and r1[0][1] == B, "LOWER (written while running): read back as written")
        check(len(r1) == 1 and r1[0][0][2] == 8 and r1[0][0][3] == 36, "LOWER: LENGTH 8, ROOT 36")
        # (after the boot's own settings save at ~2 s: a write at 3 s saves them again, 2 s on, once silent)
        out, _ = run(exe, tmp, "keep", paced(3000, write(1, W, bars=1)) + [(8000, "end")])
        base, _ = run(exe, tmp, "keep0", [(8000, "end")])
        check(out.count("settings") == base.count("settings") + 1, "a track's LENGTH is kept (the settings saved, once silent)")

        # 2. a full 256-event track; a wrap note; n 0 clears
        lines = paced(100, write(0, BIG, bars=8)) + [(800, rq(0))]
        lines += paced(1200, write(1, W, bars=1)) + [(1400, rq(1))]
        lines += paced(1700, write(1, [], bars=1, root=0x7F)) + [(1900, rq(1)), (2400, "end")]
        _, dump = run(exe, tmp, "full", lines)
        r0, r1 = reads(dump, 0), reads(dump, 1)
        check(r0 and r0[-1][1] == BIG and len(BIG) == 256, "a full track (256 events) round-trips")
        check(len(r1) >= 2 and r1[0][1] == W, "a note held over the loop's end (off first) is taken")
        check(len(r1) >= 2 and r1[-1][1] == [] and r1[-1][0][6] == 0 and r1[-1][0][3] == 0x7F, "n 0: the track cleared, no ROOT")

        # 3. a user clip: written, kept in flash when silent, read back with its name; the directory has it
        lines = paced(100, write(0x12, B, bars=8, root=36, name=b"BASS1")) + [(3000, rq(0x12)), (3400, rq(0x13)),
                                                                                (4000, "end")]
        out, dump = run(exe, tmp, "clip", lines)
        rc, re_ = reads(dump, 0x12), reads(dump, 0x13)
        check("slot sector" in out, "the clip went to flash (once the transfer was over)")
        check(rc and rc[-1][1] == B and bytes(rc[-1][0][8:14]) == b"BASS1 " and bytes(rc[-1][0][16:22]) == b"BASS1 ",
              "user clip 3: read back from flash, named")
        check(re_ and re_[-1][0][5] & 4 and re_[-1][0][6] == 0, "user clip 4: empty")

        # 4. refusals: nothing changes
        good = paced(100, write(0, A))
        bad = [
            ("a factory clip is read only", write(0x20, A)),
            ("events out of order", [write(0, A)[0][:8] + bytes([0, 0x24]) + write(0, A)[0][10:]] + write(0, A)[1:]),
            ("a head's n that does not match", write(0, A, n=len(A) - 2)),
            ("ticks not sorted", write(0, [(96, 50, 100), (0, 50, 0)])),
            ("two note-ons in a row", write(0, [(0, 50, 100), (10, 50, 100), (20, 50, 0), (30, 50, 0)][:3] + [(40, 51, 0)])),
            ("an odd count (a note left on)", write(0, [(0, 50, 100)])),
            ("a tick past the loop", write(0, [(0, 50, 100), (800, 50, 0)], bars=2)),
            ("LENGTH 0", write(0, A, bars=0)),
            ("LENGTH 33", write(0, A, bars=33)),
        ]
        for what, ms in bad:
            lines = good + paced(600, ms) + [(1200, rq(0)), (1600, "end")]
            _, dump = run(exe, tmp, "bad", lines)
            r = reads(dump, 0)
            check(r and r[-1][1] == A, f"refused: {what}")
        # a SAVE waiting: a second clip write is refused (busy) until it is written
        lines = paced(100, write(0x12, A, name=b"ONE")) + paced(400, write(0x13, B, bars=8, name=b"TWO"))
        lines += [(3000, rq(0x13)), (3400, "end")]
        _, dump = run(exe, tmp, "busy", lines)
        r = reads(dump, 0x13)
        check(r and r[-1][0][6] == 0, "a second clip write while the first waits for flash: refused (busy)")

        # 5. factory clips read as the firmware has them
        fc = fclips()
        if fc:
            for k in (0, len(fc) // 2, len(fc) - 1):
                name, bars, root, n, ev = fc[k]
                _, dump = run(exe, tmp, "fac", [(100, rq(0x20 + k)), (3000, "end")])
                r = reads(dump, 0x20 + k)
                ok = bool(r) and r[-1][0][2] == bars and r[-1][0][3] == root and r[-1][0][5] & 3 == 3
                ok = ok and bytes(r[-1][0][16:32]).rstrip() == name.encode() and r[-1][1] == ev[:4000] and len(ev) == n
                check(ok, f"factory F{k + 1} ({name}, {n} events): read as coded")
        else:
            print("note: no local factory clips (firmware/gen/mv_fclips_local.h): F reads not checked")
        hd = sysex(0x11, addr(0x12, 0), [0, 0, 0, 0x20])   # size 32: the head only (the page's directory)
        _, dump = run(exe, tmp, "headonly", paced(100, write(0x12, B, bars=8, name=b"DIR")) + [(3000, hd), (3400, "end")])
        r = reads(dump, 0x12)
        check(r and r[-1][1] == [] and r[-1][0][6] == len(B) & 0x7F and bytes(r[-1][0][8:11]) == b"DIR",
              "RQ1 size 32: the head only (n, name), no events")
        _, dump = run(exe, tmp, "nofac", [(100, rq(0x20 + len(fc))), (500, "end")])
        check(not reads(dump, 0x20 + len(fc)), "a factory clip past the last: no answer")

        # 6. linked (the page reads what plays): a track's change reports both heads, at most every 500 ms
        link = sysex(0x11, [0x01, 0, 0, 0], [0, 0, 0, 0])
        lines = [(100, link)] + paced(600, write(0, A)) + [(2000, "end")]
        _, dump = run(exe, tmp, "link", lines)
        h0 = [h for h, e in reads(dump, 0) if not e]
        h1 = [h for h, e in reads(dump, 1) if not e]
        check(h0 and h1, "linked: a track written -> its heads reported unasked")
        check(h0 and h0[-1][6] == len(A), "the report's head carries the new count")
        _, dump = run(exe, tmp, "nolink", paced(600, write(0, A)) + [(2000, "end")])
        check(not reads(dump, 0) and not reads(dump, 1), "not linked: no reports")

    print(f"seq_xfer: {checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
