#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""DELAY SYNC and CHORUS SYNC RATE on import (Kris 7 Nov: fm1-port/mvance-fx-sync-proposal-2026-11-07.md, 2a): a JP
performance's part blocks (03 pp 10 / 11) carry DELAY SYNC at offset 4, LFO1 SYNC at 5 and CHORUS SYNC RATE at 6.
All three importers must put them in the same performance bytes (mv_perf.h: 7 / 8 LFO1, 9 / 10 DELAY, 11 / 12
CHORUS; UPPER then LOWER):
  tools/mid2bank.c         (the firmware's parser: the factory header)
  tools/make_bank.py       (bank_patches: the factory list's sources)
  sim/bank/bank-model.js   (the bank editor; docs/bank.html holds the same code) -- skipped when there is no node

The bank is made here from the first performance of tools/bank/mvance.syx (its common and two patches), without its
M-vance bytes (03 pp 70, which would override the parts), plus part blocks:
  P1  upper D 7 (1/4)  L 9 (1/4D)  C 23 (LFO1)      lower D 10 (1/2)  L 0  C 13 (1/1)   (whole blocks, offset 0)
  P2  upper L 22, C 1, from a block that starts at offset 5 (no DELAY SYNC: stays OFF);
      lower D 11, L 23, C 24: each one past its range, so OFF
  python3 tests/fx_sync_import_test.py     (Linux / macOS / WSL; exit 0: all checks passed)"""
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import make_bank  # noqa: E402

checks = fails = 0


def check(ok, what):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print("FAIL", what)


def dt1(addr, body):
    s = (-sum(addr) - sum(body)) & 0x7F
    return bytes([0xF0, 0x41, 0x10, 0x00, 0x06, 0x12] + list(addr) + list(body) + [s, 0xF7])


def perf_bank():
    src = (ROOT / "tools/bank/mvance.syx").read_bytes()
    msgs = [m.group(0) for m in re.finditer(rb"\xF0[^\xF7]*\xF7", src)]
    p1 = [m for m in msgs if m[6] == 0x03 and m[7] == 0x00 and m[8] in (0x00, 0x40, 0x41, 0x42, 0x43)]
    out = bytearray()
    for pp in (0, 1):                                   # the same common and patches as P1 and P2
        for m in p1:
            out += dt1([0x03, pp, m[8], m[9]], m[10:-2])
    part = [0] * 8                                      # a whole part block: offsets 0..7
    up, lo = list(part), list(part)
    up[4:7] = [7, 9, 23]
    lo[4:7] = [10, 0, 13]
    out += dt1([0x03, 0, 0x10, 0], up) + dt1([0x03, 0, 0x11, 0], lo)
    out += dt1([0x03, 1, 0x10, 5], [22, 1]) + dt1([0x03, 1, 0x11, 4], [11, 23, 24])
    return bytes(out)


#              7  8   9  10  11  12   (LFO1 up lo, DELAY up lo, CHORUS up lo)
WANT = {1: [9, 0, 7, 10, 23, 13], 2: [22, 0, 0, 0, 1, 0]}

with tempfile.TemporaryDirectory() as td:
    syx = Path(td) / "parts.syx"
    syx.write_bytes(perf_bank())

    # 1. tools/mid2bank.c (built into the temporary folder: build/ and firmware/gen/ are left alone)
    exe = Path(td) / "mid2bank"
    subprocess.run(["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Ifirmware/src", "-Ifirmware/gen",
                    "tools/mid2bank.c", "-o", str(exe)], cwd=ROOT, check=True)
    hdr = subprocess.run([str(exe), str(syx)], cwd=ROOT, check=True, capture_output=True, text=True).stdout
    rows = re.search(r"MV_FACTORY_PERF\[MV_NPROG\]\[16\] = \{(.*?)\n\};", hdr, re.S)
    check(rows is not None, "mid2bank: the MV_FACTORY_PERF table")
    if rows:
        perf = [[int(x) for x in re.findall(r"\d+", r)] for r in re.findall(r"\{([^{}]*)\}", rows.group(1))]
        for p, want in WANT.items():
            check(perf[p - 1][7:13] == want, f"mid2bank P{p}: bytes 7..12 {perf[p - 1][7:13]}, want {want}")

    # 2. tools/make_bank.py: the part blocks as the list's sources see them ([DELAY, LFO1, CHORUS] a part)
    s = make_bank.bank_patches(syx)["_sync"]
    for p, w in WANT.items():
        check(s.get(f"P{p}A") == [w[2], w[0], w[4]], f"make_bank P{p}A: {s.get(f'P{p}A')}")
        check(s.get(f"P{p}B") == [w[3], w[1], w[5]], f"make_bank P{p}B: {s.get(f'P{p}B')}")

    # 3. sim/bank/bank-model.js: parsed, then written as a bank with M-vance's bytes (03 pp 70) and read back
    if shutil.which("node"):
        js = ("import { parseBankFile, makeBankSyx } from %s;\n"
              "import { readFileSync } from 'node:fs';\n"
              "const P = parseBankFile(new Uint8Array(readFileSync(%s))).filter((i) => i.kind === 'P');\n"
              "const B = Array.from({ length: 64 }, (_, i) => P[i] || P[0]);   // (a bank is 64: the rest P1 again)\n"
              "const Q = parseBankFile(makeBankSyx(B)).filter((i) => i.kind === 'P');\n"
              "console.log(JSON.stringify(Q.map((q) => Array.from(q.ext).slice(7, 13))));\n"
              % (json.dumps((ROOT / "sim/bank/bank-model.js").as_uri()), json.dumps(str(syx))))
        (Path(td) / "t.mjs").write_text(js)
        got = json.loads(subprocess.run(["node", str(Path(td) / "t.mjs")], check=True, capture_output=True,
                                        text=True).stdout)
        for p, want in WANT.items():
            check(got[p - 1] == want, f"bank-model P{p}: bytes 7..12 {got[p - 1]}, want {want}")
    else:
        print("fx_sync_import_test: no node, bank-model.js not checked")

print(f"fx_sync_import_test: {checks} checks, {fails} failed")
sys.exit(1 if fails else 0)
