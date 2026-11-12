#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match WP0 acceptance: the render harness (engine_mvance.py) and the patch writer (jp_patch.py).

  python3 tools/match/test_wp0.py [--corpus DIR] [--speed-renders N]

  F   jp_patch forms: every message's checksum right; the user form (02 bb pp 00) and the perf form (as
      tools/make_bank.py writes) parsed back by the firmware's own mv_sysex_parse (tools/mid2bank.c, built into a
      temporary folder) give back every byte sent: the perf form of the 64 factory programs reproduces
      firmware/gen/mv_factory.h's MV_FACTORY, MV_FACTORY_LO and MV_FACTORY_PERF
  A1  each SINGLE factory program: render() of its bytes == the program-change render, bit for bit
      (target 63 of 63; program 13, Lead 01, is DUAL and excluded). With --corpus (a make_corpus.py folder: its
      manifest.csv, .wav and .syx) also: the temp form == its .syx and render() == its .wav, bit for bit
  A2  the same render() call twice gives the same bytes (and a second engine instance too)
  A3  at least 40 renders a second on one core for a 4 s take (renders run one after another: one host process,
      single threaded, at a time)
  A4  a short-release patch (INIT, AMP ENV RELEASE 0) decays below -80 dBFS after key-off and stays there (proves
      the reverb is off on a fresh host)

Prints each check with its numbers and PASS / FAIL; exit status 1 if any failed. The targets are the proposal's
(sound-match-proposal-2026-11-11.md, WP0): do not edit them to make a run pass.
"""
import argparse
import csv
import math
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import jp_patch                                        # noqa: E402  (after the path line)
import engine_mvance as em                             # noqa: E402

# the take every single program is played with (as make_corpus.py, so --corpus compares like with like)
VEL = 100                               # MIDI velocity
GATE_MS = 2000                          # ms: key held
TOTAL_MS = 6300                         # ms: end of take (4 s for the release / delay tail)
NOTE_DEFAULT = 60                       # C4
NOTE_BASS = 36                          # C2, for programs whose name starts "Bass" / "BS"

# acceptance targets (proposal WP0)
A1_EXPECT = 63                          # single programs that must match bit for bit
A2_SLOTS = 5                            # programs rendered twice
A3_MIN_RPS = 40.0                       # renders / s, one core
A3_TOTAL_MS = 4000                      # ms: "a 4 s take"
A3_GATE_MS = 2000                       # ms: key held in it
A4_FLOOR_DB = -80.0                     # dBFS: the tail must fall below this and stay below
A4_GATE_MS = 1000                       # ms: key held
A4_TOTAL_MS = 4000                      # ms: end of take (2.7 s after key-off)
A4_MIN_NOTE_DB = -40.0                  # dBFS: the held note's loudest 10 ms must be above this (a real note)
A4_SETTLE_MS = 1000                     # ms after key-off by which the tail must be below the floor
FRAME_MS = 10                           # ms: RMS frame for A4
AENV_R = 57                             # patch byte: AMP ENV RELEASE (tools/mvance-params.yaml)


def note_for(name):
    """The note a program is played at (make_corpus.py's rule)."""
    return NOTE_BASS if re.match(r"(?i)(bass|bs)\b", name) else NOTE_DEFAULT


def result(ok, label, detail):
    """Print one check's line; returns ok."""
    print(f"  [{'PASS' if ok else 'FAIL'}] {label}: {detail}")
    return ok


def check_forms(root, factory, tmp):
    """F: checksums, and the user and perf forms parsed back by mid2bank (the firmware's mv_sysex_parse)."""
    print("F  jp_patch forms")
    exe = tmp / "mid2bank"
    subprocess.run(["cc", "-std=c99", "-O2", "-Ifirmware/src", "-Ifirmware/gen", "tools/mid2bank.c", "-o", str(exe)],
                   cwd=root, check=True)
    ok = True

    def parse_back(data, label):
        f = tmp / f"{label}.syx"
        jp_patch.write_syx(f, data)
        r = subprocess.run([str(exe), str(f)], cwd=root, check=True, capture_output=True, text=True)
        return em.parse_factory(r.stdout), r.stderr.strip()

    msgs = []
    for p in factory:
        msgs += jp_patch.split_messages(jp_patch.temp_patch_syx(p["patch"]) + jp_patch.user_patch_syx(
            p["patch"], p["slot"] - 1) + jp_patch.perf_syx(p["patch"], p["slot"] - 1, p["lower"], p["perf"]))
    bad = sum(1 for m in msgs if not jp_patch.checksum_ok(m))
    ok &= result(bad == 0, "checksums", f"{len(msgs)} messages, {bad} wrong")

    user = b"".join(jp_patch.user_patch_syx(p["patch"], p["slot"] - 1) for p in factory)
    back, log = parse_back(user, "user")
    diff = [p["slot"] for p, b in zip(factory, back) if b["patch"] != p["patch"]]
    ok &= result(not diff and "64 programs read, 0 bad checksums, 0 bad frames, 0 messages not ours" in log,
                 "user form 02 bb pp 00 -> mid2bank", f"{64 - len(diff)}/64 patches back byte for byte; {log}")

    perf = b"".join(jp_patch.perf_syx(p["patch"], p["slot"] - 1, p["lower"], p["perf"]) for p in factory)
    back, log = parse_back(perf, "perf")
    diff = [p["slot"] for p, b in zip(factory, back)
            if (b["patch"], b["lower"], b["perf"]) != (p["patch"], p["lower"], p["perf"])]
    ok &= result(not diff and "64 programs read, 0 bad checksums, 0 bad frames, 0 messages not ours" in log,
                 "perf form 03 pp 00/40/42/70 -> mid2bank",
                 f"{64 - len(diff)}/64 programs == mv_factory.h (upper, lower, perf bytes){'' if not diff else f'; differ: {diff}'}; {log}")
    return ok


def check_a1(eng, singles, corpus):
    """A1: render() of each single == its program-change render (and == the corpus take, if given)."""
    print(f"A1 render() == program change, {len(singles)} SINGLE programs (note 60, basses 36; vel {VEL}, "
          f"gate {GATE_MS} ms, total {TOTAL_MS} ms)")
    man = {}
    if corpus:
        with open(corpus / "manifest.csv", newline="") as f:
            man = {int(r["slot"]): r for r in csv.DictReader(f)}
    same, miss, c_wav, c_syx, c_n = 0, [], 0, 0, 0
    for p in singles:
        note = note_for(p["name"])
        a = eng.render(p["patch"], note, VEL, GATE_MS, TOTAL_MS)
        b = eng.render_program(p["slot"], note, VEL, GATE_MS, TOTAL_MS)
        if a.shape == b.shape and np.array_equal(a, b):
            same += 1
        else:
            n = min(len(a), len(b))
            d = float(np.max(np.abs(a[:n] - b[:n]))) if n else float("nan")
            miss.append(f"{p['slot']} {p['name']} (max |diff| {d:.2e}, frames {len(a)}/{len(b)})")
        r = man.get(p["slot"])
        if r:
            c_n += 1
            c_syx += (corpus / r["syx"]).read_bytes() == jp_patch.temp_patch_syx(p["patch"])
            c_wav += int(r["note"]) == note and np.array_equal(a, em.read_wav(corpus / r["wav"]))
    ok = result(same == A1_EXPECT and not miss, "bit for bit", f"{same} of {len(singles)} (target {A1_EXPECT})")
    for m in miss:
        print(f"         differs: {m}")
    if corpus:
        ok &= result(c_n == len(singles) and c_syx == c_n, "temp form == corpus .syx", f"{c_syx} of {c_n} "
                     f"({len(singles)} singles)")
        ok &= result(c_n == len(singles) and c_wav == c_n, "render() == corpus .wav", f"{c_wav} of {c_n}")
    return ok


def check_a2(eng, singles, root):
    """A2: the same call twice, and on a second engine, gives the same bytes. Programs with different OSC1 waves
    (the Super Saw's random phases are the likeliest to drift)."""
    by_wave = {}
    for p in singles:
        by_wave.setdefault(p["patch"][30], p)
    pick = list(by_wave.values())[:A2_SLOTS]
    print(f"A2 repeatable: {', '.join(p['name'] for p in pick)} (OSC1 waves {[p['patch'][30] for p in pick]})")
    same = 0
    with em.MvanceEngine(root, host=eng.host) as eng2:
        for p in pick:
            n = note_for(p["name"])
            a = eng.render(p["patch"], n, VEL, GATE_MS, TOTAL_MS)
            b = eng.render(p["patch"], n, VEL, GATE_MS, TOTAL_MS)
            c = eng2.render(p["patch"], n, VEL, GATE_MS, TOTAL_MS)
            same += a.tobytes() == b.tobytes() == c.tobytes()
    return result(same == len(pick), "same bytes", f"{same} of {len(pick)} (twice on one engine, once on another)")


def check_a3(eng, singles, n_renders):
    """A3: renders a second for a 4 s take, one after another on one core."""
    print(f"A3 speed: {n_renders} renders of a {A3_TOTAL_MS} ms take, one at a time "
          f"({os.cpu_count()} CPUs visible; renders are sequential, one single-threaded host each)")
    eng.render(singles[0]["patch"], 60, VEL, A3_GATE_MS, A3_TOTAL_MS)            # (warm the file cache)
    t0 = time.perf_counter()
    for i in range(n_renders):
        p = singles[i % len(singles)]
        eng.render(p["patch"], note_for(p["name"]), VEL, A3_GATE_MS, A3_TOTAL_MS)
    dt = time.perf_counter() - t0
    rps = n_renders / dt
    return result(rps >= A3_MIN_RPS, "renders / s", f"{rps:.1f} (target >= {A3_MIN_RPS:.0f}); {1000 * dt / n_renders:.1f} "
                  f"ms a render, {n_renders} in {dt:.2f} s")


def frame_db(x):
    """RMS dBFS per FRAME_MS frame of a (frames, 2) take, the louder channel; -inf for digital silence."""
    n = em.SAMPLE_RATE * FRAME_MS // 1000
    k = len(x) // n
    rms = np.sqrt(np.mean(x[:k * n].astype(np.float64).reshape(k, n, 2) ** 2, axis=1)).max(axis=1)
    with np.errstate(divide="ignore"):
        return 20 * np.log10(rms)


def check_a4(eng):
    """A4: INIT with AMP ENV RELEASE 0: after key-off the 10 ms RMS falls below -80 dBFS within A4_SETTLE_MS and
    stays below to the end; the peak of the last second is reported too."""
    p = bytearray(eng.init_patch())
    p[AENV_R] = 0
    print(f"A4 short release: INIT, AMP ENV RELEASE 0, note 60 vel {VEL}, gate {A4_GATE_MS} ms, total {A4_TOTAL_MS} ms")
    x = eng.render(bytes(p), 60, VEL, A4_GATE_MS, A4_TOTAL_MS)
    db = frame_db(x)
    off = (em.NOTE_ON_MS + A4_GATE_MS) // FRAME_MS                # frame index of the key-off
    held = float(np.max(db[em.NOTE_ON_MS // FRAME_MS:off]))
    above = np.nonzero(db[off:] >= A4_FLOOR_DB)[0]
    below_ms = (int(above[-1]) + 1) * FRAME_MS if len(above) else 0   # ms after key-off from which all is below
    last = x[-em.SAMPLE_RATE:]
    pk = float(np.max(np.abs(last)))
    pk_db = 20 * math.log10(pk) if pk > 0 else float("-inf")
    tail_max = float(np.max(db[off + A4_SETTLE_MS // FRAME_MS:]))
    ok = held > A4_MIN_NOTE_DB and below_ms <= A4_SETTLE_MS and tail_max < A4_FLOOR_DB
    return result(ok, "tail", f"held note up to {held:.1f} dBFS; below {A4_FLOOR_DB:.0f} dBFS from {below_ms} ms "
                  f"after key-off (limit {A4_SETTLE_MS} ms) to the end; loudest 10 ms after that {tail_max:.1f} dBFS; "
                  f"peak of the last 1 s {pk_db:.1f} dBFS")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", type=Path, default=em.ROOT, help="the M-vance repo (default: this file's)")
    ap.add_argument("--corpus", type=Path, help="a make_corpus.py output folder to compare with as well")
    ap.add_argument("--speed-renders", type=int, default=200, help="renders timed for A3 (default 200)")
    a = ap.parse_args()
    root = a.root.resolve()
    t0 = time.perf_counter()
    factory = em.load_factory(root)
    singles = [p for p in factory if p["perf"][0] == 0]
    print(f"M-vance at {root}: {len(factory)} factory programs, {len(singles)} SINGLE "
          f"(not: {', '.join(str(p['slot']) + ' ' + p['name'] for p in factory if p['perf'][0])})")
    oks = []
    with tempfile.TemporaryDirectory(prefix="wp0") as tmp, em.MvanceEngine(root) as eng:
        print(f"host built: {eng.host}")
        oks.append(check_forms(root, factory, Path(tmp)))
        oks.append(check_a1(eng, singles, a.corpus.resolve() if a.corpus else None))
        oks.append(check_a2(eng, singles, root))
        oks.append(check_a3(eng, singles, a.speed_renders))
        oks.append(check_a4(eng))
    names = ["F", "A1", "A2", "A3", "A4"]
    summary = " ".join(n + (" PASS" if o else " FAIL") for n, o in zip(names, oks))
    print(f"WP0: {summary}  ({time.perf_counter() - t0:.1f} s)")
    return 0 if all(oks) else 1


if __name__ == "__main__":
    sys.exit(main())
