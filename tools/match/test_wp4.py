#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match WP4 acceptance: the whole search (fit.match: Stages 1 to 4) on 5 factory programs, one of each
kind the proposal names, matched from their own renders (note and gate given, as WP6 Level A).

  python3 tools/match/test_wp4.py [--budget N] [--workers N] [--out DIR]   (default DIR: build/match/wp4)

  kinds    Super Saw pad, saw bass, pulse lead, pluck with filter envelope, a patch with delay: the first single
           factory program of each (rules in PICKS)
  1        final D1 under 3 dB, and lower than after Stage 2, for each
  2        the same seed gives the same patch (the first program run twice)

DIR/wp4.csv has each program's D1 after Stage 2 and at the end, its score, renders and time, and DIR/<slot>.syx
the patch found. Exit status 1 if a check fails. Targets are the proposal's (WP4): do not edit them to pass.
"""
import argparse
import csv
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import engine_mvance as em                             # noqa: E402
import fit                                             # noqa: E402
import jp_patch                                        # noqa: E402
from test_wp0 import note_for, VEL, GATE_MS, TOTAL_MS  # noqa: E402

D1_MAX = 3.0                            # dB: final D1 must be under this
# (kind, rule on a single factory program): name prefix, OSC1 WAVE (byte 30), F-ENV DEPTH (46), DELAY LEVEL (66)
PICKS = [("Super Saw pad", lambda p: p["name"].startswith("Pad") and p["patch"][30] == 0),
         ("saw bass", lambda p: p["name"].startswith("Bass") and p["patch"][30] == 5),
         ("pulse lead", lambda p: p["name"].startswith("Lead") and p["patch"][30] == 4),
         ("pluck with filter envelope", lambda p: p["name"].startswith("Pluck") and p["patch"][46] >= 96),
         ("patch with delay", lambda p: p["patch"][66] >= 64)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", type=Path, default=em.ROOT)
    ap.add_argument("--budget", type=int, default=8000)
    ap.add_argument("--workers", type=int, default=None)
    ap.add_argument("--out", type=Path)
    a = ap.parse_args()
    root = a.root.resolve()
    out = (a.out or root / "build/match/wp4").resolve()
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.perf_counter()
    singles = [p for p in em.load_factory(root) if p["perf"][0] == 0]
    chosen = []
    for kind, rule in PICKS:
        p = next(q for q in singles if rule(q) and q not in [c for _, c in chosen])
        chosen.append((kind, p))
    rows, ok_all, first = [], True, None
    with em.MvanceEngine(root) as eng:
        for kind, p in chosen:
            note = note_for(p["name"])
            x = eng.render(p["patch"], note, VEL, GATE_MS, TOTAL_MS)
            t1 = time.perf_counter()
            res = fit.match(x, eng.SAMPLE_RATE, note=note, gate_ms=GATE_MS, budget=a.budget, seed_rng=1,
                            workers=a.workers, engine=eng)
            res["problem"].close()
            if first is None:
                first = (p, x, note, res["patch"])
            d1 = res["parts"]["d1"]
            ok = d1 < D1_MAX and d1 < res["d1_stage2"]
            ok_all &= ok
            jp_patch.write_syx(out / f"{p['slot']:02d}.syx", jp_patch.temp_patch_syx(res["patch"]))
            rows.append({"kind": kind, "slot": p["slot"], "name": p["name"], "d1_stage2": f"{res['d1_stage2']:.2f}",
                         "d1": f"{d1:.2f}", "loss": f"{res['loss']:.2f}", "renders": res["renders"],
                         "seconds": f"{time.perf_counter() - t1:.0f}", "ok": ok})
            print(f"  [{'PASS' if ok else 'FAIL'}] 1 {kind:27s} {p['slot']:2d} {p['name']:10s} D1 after Stage 2 "
                  f"{res['d1_stage2']:5.2f} dB -> final {d1:5.2f} dB (target < {D1_MAX}), score {res['loss']:.2f}, "
                  f"{res['renders']} renders, {time.perf_counter() - t1:.0f} s", flush=True)
            for line in res["log"]:
                print("       " + line)
        p, x, note, patch = first
        res = fit.match(x, eng.SAMPLE_RATE, note=note, gate_ms=GATE_MS, budget=a.budget, seed_rng=1,
                        workers=a.workers, engine=eng)
        res["problem"].close()
        same = res["patch"] == patch
        ok_all &= same
        print(f"  [{'PASS' if same else 'FAIL'}] 2 same seed, same patch ({p['name']} run twice)")
    with open(out / "wp4.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"WP4: {'PASS' if ok_all else 'FAIL'} ({time.perf_counter() - t0:.0f} s) -> {out}")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
