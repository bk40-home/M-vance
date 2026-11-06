#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The program transfer on the host build (mv_xfer.h; Kris 4 Nov): a bank sent into the slots comes back, byte for
byte, in a dump; one targeted slot leaves the others alone; the temporary area is not saved; flash waits for the end.

  python3 tests/xfer_test.py [HOST_BINARY]   (default: builds build/host/mvance-xfer; needs build/mvance-factory.syx
                                              from tools/make_bank.py, or build/mvance-public.syx without the
                                              local bank)
Linux / macOS / WSL. Exit 0: all checks passed."""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
checks = fails = 0


def check(ok, what):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print("FAIL", what)


def msgs(b):
    """the F0..F7 messages of a byte string"""
    return [m.group(0) for m in re.finditer(rb"\xF0[^\xF7]*\xF7", b)]


def dt1(addr, body, dev=0x10):
    s = (128 - (sum(addr) + sum(body)) % 128) % 128
    return bytes([0xF0, 0x41, dev, 0x00, 0x06, 0x12] + list(addr) + list(body) + [s, 0xF7])


def rq1(addr, size):
    s = (128 - (sum(addr) + sum(size)) % 128) % 128
    return bytes([0xF0, 0x41, 0x10, 0x00, 0x06, 0x11] + list(addr) + list(size) + [s, 0xF7])


def renumber(m, pp):
    """a performance message moved to performance pp (its sum again)"""
    b = bytearray(m)
    b[7] = pp
    b[-2] = (128 - sum(b[6:-2]) % 128) % 128
    return bytes(b)


def run(exe, tmp, name, script, files):
    for fn, data in files.items():
        (tmp / fn).write_bytes(data)
    (tmp / (name + ".txt")).write_text(script.replace("$T", str(tmp)))
    out = subprocess.run([str(exe), str(tmp / name), str(tmp / (name + ".txt"))], capture_output=True, text=True,
                         check=True).stdout
    dump = tmp / (name + ".syx")
    return out, dump.read_bytes() if dump.exists() else b""


def main():
    exe = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/host/mvance-xfer"
    if len(sys.argv) < 2:
        exe.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["cc", "-O2", "-Wno-unused-function", "-DDINGHY_HOST", '-DAPP_VER="xfer"', "-Ifirmware/hal",
                        "-Ifirmware/src", "-Ifirmware/gen", "-o", str(exe), "firmware/src/mvance.c",
                        "firmware/src/mv_dsp.c"], cwd=ROOT, check=True)
    all64 = rq1([0x03, 0, 0, 0], [0, 64, 0, 0])
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        # the factory as the device actually holds it: patches are canonicalised on load, so a raw source .syx can
        # differ at inert/unmapped bytes (e.g. 130/131); dumping the untouched factory gives the device's own bytes,
        # the right baseline for a transfer round-trip (everything the firmware models comes back byte for byte).
        _, facdump = run(exe, tmp, "fac", "100 syx $T/rq.syx\n4000 end\n", {"rq.syx": all64})
        fac = msgs(facdump)
        check(len(fac) == 256, f"the factory dump: 4 messages a program ({len(fac)})")
        # a different bank: each program one slot on (64 -> 1), so every slot changes
        alt = [renumber(m, (m[7] + 1) % 64) for m in fac]
        alt.sort(key=lambda m: m[7])
        # 1. a whole bank in, a dump of all 64 out: the same bytes
        out, dump = run(exe, tmp, "bank", f"100 syx $T/alt.syx\n9000 syx $T/rq.syx\n16000 end\n",
                        {"alt.syx": b"".join(alt), "rq.syx": all64})
        check(dump == b"".join(alt), f"bank in, dump out: the same {len(b''.join(alt))} bytes (got {len(dump)})")
        saves = [ln for ln in out.splitlines() if "saved" in ln]
        first = min((int(ln.split()[0]) for ln in saves if "blob" in ln), default=0)
        check(first > 100 + 30 * 255, f"flash waits for the transfer's end (first blob at {first} ms)")
        check(sum("blob" in ln for ln in saves) == 8 and sum("slot 0" in ln for ln in saves) == 1,
              "all 8 blobs and the perf slot written once")
        # 2. one targeted slot: performance 9 (program 10) only; the others dump as the factory's
        one = [renumber(m, 9) for m in alt if m[7] == 20]   # (program 21's of the alt bank into slot 10)
        out, dump = run(exe, tmp, "one", f"100 syx $T/one.syx\n2000 syx $T/rq.syx\n9000 end\n",
                        {"one.syx": b"".join(one), "rq.syx": all64})
        d = msgs(dump)
        exp = [m for m in fac]
        exp[36:40] = one
        check(d == exp, "a targeted slot: slot 10 the new one, the other 63 untouched")
        # 3. the temporary area: heard, not saved; a dump of it answers with 01 00 ..
        tmp_msgs = [bytes(bytearray(m[:6]) + bytes([0x01, 0x00]) + m[8:]) for m in alt if m[7] == 5]
        tmp_msgs = [dt1(list(m[6:10]), m[10:-2]) for m in tmp_msgs]
        out, dump = run(exe, tmp, "temp", f"100 syx $T/t.syx\n1000 syx $T/rq.syx\n5000 end\n",
                        {"t.syx": b"".join(tmp_msgs), "rq.syx": rq1([0x01, 0, 0, 0], [0, 0, 0x10, 0])})
        d = msgs(dump)
        check(d == tmp_msgs, "the temporary area: what plays comes back as it was sent")
        check("saved" not in out.replace("settings 0 saved", ""), "an audition writes nothing to flash")
        # 4. a bad checksum: refused, nothing written
        bad = bytearray(alt[1]); bad[-2] ^= 1
        out, dump = run(exe, tmp, "bad", f"100 syx $T/b.syx\n1000 syx $T/rq.syx\n4000 end\n",
                        {"b.syx": bytes(bad), "rq.syx": rq1([0x03, 0, 0, 0], [0, 1, 0, 0])})
        check(msgs(dump) == fac[0:4], "a bad checksum: refused (slot 1 as the factory's)")
    print(f"xfer_test: {checks} checks, {fails} failed")
    return fails != 0


if __name__ == "__main__":
    sys.exit(main())
