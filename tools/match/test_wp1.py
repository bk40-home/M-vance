#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match WP1 acceptance: the measures (features.py) and the distance (loss.py), on M-vance renders.

  python3 tools/match/test_wp1.py [--out DIR]           (default DIR: <repo>/build/match/wp1)

  1  loss(x, x) is 0, for every single factory program
  2  sweep one byte of a factory patch through its range against the patch's own render, for CUTOFF, RESO,
     A-ENV DECAY, OSC1 CTRL1 and F-ENV DEPTH: every byte value reaching the minimum loss is within +-2 of the true
     byte. The patch for each is the first single factory program where that byte is audible (+-8 changes the
     loss). Each curve is saved as DIR/sweep_<KEY>.png
  3  sensitivity: for every searchable byte (search groups G1..G7, LEVEL), the change in loss for +16 and -16
     (clamped to the byte's range) on 5 factory programs with different OSC1 waves. Written to
     DIR/sensitivity.csv; bytes with no effect on any of them are listed (a report, no pass / fail)

Exit status 1 if 1 or 2 failed. Targets are the proposal's (WP1): do not edit them to make a run pass.
"""
import argparse
import csv
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import engine_mvance as em                             # noqa: E402
import features as F                                   # noqa: E402
import loss as L                                       # noqa: E402
from test_wp0 import note_for, VEL, GATE_MS, TOTAL_MS  # noqa: E402  (the same take as WP0)

SWEEP_KEYS = ["CUTOFF", "RESO", "AENV_D", "OSC1_C1", "FENV_DEPTH"]
SWEEP_TOL = 2                           # bytes: the minimum must lie within this of the true byte
AUDIBLE_STEP = 8                        # bytes: the probe step that decides whether a byte is audible in a patch
AUDIBLE_LOSS = 0.5                      # loss: what the probe must change (both directions) to count as audible
SENS_STEP = 16                          # bytes: the sensitivity step
SENS_PATCHES = 5                        # programs the sensitivity is measured on
NO_EFFECT = 1e-9                        # loss: below this a change counts as no effect


class Scorer:
    """Renders a patch the WP0 way and scores it against a reference patch's render."""

    def __init__(self, eng):
        self.eng = eng
        self.renders = 0

    def feats(self, patch, note):
        x = self.eng.render(patch, note, VEL, GATE_MS, TOTAL_MS)
        self.renders += 1
        c = F.condition(x, onset_s=em.NOTE_ON_MS / 1000, keyoff_s=(em.NOTE_ON_MS + GATE_MS) / 1000)
        return F.features(c, f0=F.midi_hz(note))

    def loss_of(self, ref_f, patch, note):
        return L.loss(ref_f, self.feats(patch, note))[0]


def with_byte(patch, off, v):
    """A copy of patch with byte off set to v."""
    q = bytearray(patch)
    q[off] = v
    return bytes(q)


def check_self(sc, singles):
    """1: loss(x, x) == 0 for every single."""
    worst = 0.0
    for p in singles:
        f = sc.feats(p["patch"], note_for(p["name"]))
        worst = max(worst, L.loss(f, f)[0])
    ok = worst == 0.0
    print(f"  [{'PASS' if ok else 'FAIL'}] 1 loss(x, x): largest {worst} over {len(singles)} singles (target 0)")
    return ok


def audible_patch(sc, singles, prm):
    """The first single where +-AUDIBLE_STEP on the byte both change the loss by AUDIBLE_LOSS or more."""
    off, lo, hi = prm["offset"], prm["min"], prm["max"]
    for p in singles:
        v = p["patch"][off]
        if v - AUDIBLE_STEP < lo or v + AUDIBLE_STEP > hi:
            continue
        note = note_for(p["name"])
        f = sc.feats(p["patch"], note)
        if min(sc.loss_of(f, with_byte(p["patch"], off, v + s), note) for s in (-AUDIBLE_STEP, AUDIBLE_STEP)) \
                >= AUDIBLE_LOSS:
            return p, f
    return None, None


def check_sweeps(sc, singles, params, out):
    """2: the byte sweeps, one PNG each."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    ok_all = True
    for key in SWEEP_KEYS:
        prm = next(p for p in params if p["key"] == key)
        p, f = audible_patch(sc, singles, prm)
        if p is None:
            print(f"  [FAIL] 2 sweep {key}: no single program where it is audible")
            ok_all = False
            continue
        off, true = prm["offset"], p["patch"][prm["offset"]]
        note = note_for(p["name"])
        vals = np.arange(prm["min"], prm["max"] + 1)
        curve = np.array([sc.loss_of(f, with_byte(p["patch"], off, int(v)), note) for v in vals])
        best = vals[curve <= curve.min() + NO_EFFECT]
        ok = bool(np.all(np.abs(best - true) <= SWEEP_TOL))
        ok_all &= ok
        print(f"  [{'PASS' if ok else 'FAIL'}] 2 sweep {key:10s} on {p['slot']:2d} {p['name']:10s}: true {true}, "
              f"minimum at {list(map(int, best))} (loss {curve.min():.3f}); next lowest beyond +-{SWEEP_TOL}: "
              f"{np.min(curve[np.abs(vals - true) > SWEEP_TOL]):.3f}")
        fig, ax = plt.subplots(figsize=(8, 4))
        ax.plot(vals, curve, lw=1.2)
        ax.axvline(true, color="r", ls="--", lw=1, label=f"true byte {true}")
        ax.set_xlabel(f"{key} (byte {off})")
        ax.set_ylabel("loss (weighted, section 5 weights)")
        ax.set_title(f"WP1 sweep: {key} on {p['slot']} {p['name']} (note {note})")
        ax.legend()
        fig.tight_layout()
        fig.savefig(out / f"sweep_{key}.png", dpi=110)
        plt.close(fig)
    return ok_all


def check_sensitivity(sc, singles, params, out):
    """3: the +-16 table."""
    by_wave = {}
    for p in singles:
        by_wave.setdefault(p["patch"][30], p)
    pick = list(by_wave.values())[:SENS_PATCHES]
    refs = [(p, note_for(p["name"]), sc.feats(p["patch"], note_for(p["name"]))) for p in pick]
    rows, none = [], []
    for prm in [q for q in params if q["group"] != em.GROUP_FIXED]:
        off = prm["offset"]
        row = {"offset": off, "key": prm["key"], "group": prm["group"], "kind": prm["kind"]}
        top = 0.0
        for p, note, f in refs:
            v = p["patch"][off]
            for s, tag in ((-SENS_STEP, "minus"), (SENS_STEP, "plus")):
                w = min(max(v + s, prm["min"]), prm["max"])
                d = sc.loss_of(f, with_byte(p["patch"], off, w), note) if w != v else 0.0
                row[f"{p['slot']}_{tag}"] = f"{d:.3f}"
                top = max(top, d)
        row["max"] = f"{top:.3f}"
        rows.append(row)
        if top < NO_EFFECT:
            none.append(f"{off} {prm['key']}")
    with open(out / "sensitivity.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"  3 sensitivity: {len(rows)} searchable bytes x {len(refs)} programs "
          f"({', '.join(str(p['slot']) + ' ' + p['name'] for p, _, _ in refs)}) -> {out / 'sensitivity.csv'}")
    for r in sorted(rows, key=lambda r: -float(r["max"])):
        print(f"       {r['offset']:3d} {r['key']:12s} {r['group']:5s} largest change {float(r['max']):7.3f}")
    print(f"     no effect on any of them: {', '.join(none) if none else 'none'}")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", type=Path, default=em.ROOT)
    ap.add_argument("--out", type=Path)
    a = ap.parse_args()
    root = a.root.resolve()
    out = (a.out or root / "build/match/wp1").resolve()
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.perf_counter()
    singles = [p for p in em.load_factory(root) if p["perf"][0] == 0]
    with em.MvanceEngine(root) as eng:
        sc = Scorer(eng)
        params = eng.params()
        print(f"WP1 on {len(singles)} single programs (vel {VEL}, gate {GATE_MS} ms, total {TOTAL_MS} ms)")
        ok1 = check_self(sc, singles)
        ok2 = check_sweeps(sc, singles, params, out)
        check_sensitivity(sc, singles, params, out)
    print(f"WP1: 1 {'PASS' if ok1 else 'FAIL'}  2 {'PASS' if ok2 else 'FAIL'}  3 written  "
          f"({sc.renders} renders, {time.perf_counter() - t0:.0f} s)")
    return 0 if ok1 and ok2 else 1


if __name__ == "__main__":
    sys.exit(main())
