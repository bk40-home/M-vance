#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match WP2 acceptance: the analysis (analyse.py) on renders of the 63 single factory programs, read blind
(no note, no gate given).

  python3 tools/match/test_wp2.py [--out DIR]           (default DIR: <repo>/build/match/wp2: seeds.csv)

  note      the analysed MIDI note == the played note, target 100 %. Also reported: == the SOUNDING note (played +
            12 x (OSC SHIFT - 2): what any analysis can hear) and octave-equivalent
  attack    A-ENV ATTACK byte within +-12, target 80 %
  release   A-ENV RELEASE byte within +-12, target 80 % (also reported: with the true gate given, as --gate-ms)
  cutoff    CUTOFF seed within +-16, target 70 % of the LPF programs with RESO under 64

Exit status 1 if any target is missed. The targets are the proposal's (WP2): do not edit them to make a run pass.
"""
import argparse
import csv
import sys
import time
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyse as A                                    # noqa: E402
import engine_mvance as em                             # noqa: E402
from test_wp0 import note_for, VEL, GATE_MS, TOTAL_MS  # noqa: E402

T_NOTE, T_ATTACK, T_RELEASE, T_CUTOFF = 1.00, 0.80, 0.80, 0.70     # fractions that must be reached
TOL_ENV, TOL_CUT = 12, 16                                           # bytes
RESO_MAX = 64                                                       # RESO below this for the cutoff check
B_ATTACK, B_RELEASE, B_CUTOFF, B_RESO, B_TYPE, B_SHIFT = 54, 57, 41, 42, 39, 73   # patch bytes
LPF = 2                                                             # FLT TYPE value of the low pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", type=Path, default=em.ROOT)
    ap.add_argument("--out", type=Path)
    a = ap.parse_args()
    root = a.root.resolve()
    out = (a.out or root / "build/match/wp2").resolve()
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.perf_counter()
    singles = [p for p in em.load_factory(root) if p["perf"][0] == 0]
    rows = []
    with em.MvanceEngine(root) as eng:
        for p in singles:
            P, note = p["patch"], note_for(p["name"])
            x = eng.render(P, note, VEL, GATE_MS, TOTAL_MS)
            s = A.analyse(x, eng.SAMPLE_RATE, eng)
            g = A.analyse(x, eng.SAMPLE_RATE, eng, gate_ms=em.NOTE_ON_MS + GATE_MS - s["onset_s"] * 1000)
            b = s["bytes"]
            rows.append({"slot": p["slot"], "name": p["name"], "played": note,
                         "sounding": note + 12 * (P[B_SHIFT] - 2), "note": s["note"], "cents": s["cents"],
                         "gate_ms": s["gate_ms"], "attack": P[B_ATTACK], "attack_seed": b.get("AENV_A"),
                         "release": P[B_RELEASE], "release_seed": b.get("AENV_R"),
                         "release_seed_gate": g["bytes"].get("AENV_R"), "cutoff": P[B_CUTOFF],
                         "cutoff_seed": b.get("CUTOFF"), "flt_type": P[B_TYPE], "reso": P[B_RESO],
                         "waves": " ".join(f"{k}={v}" for k, v in s["waves"].items())})
    with open(out / "seeds.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    def frac(cond, rs):
        return sum(1 for r in rs if cond(r)) / len(rs) if rs else 0.0

    near = lambda k, kk, tol: (lambda r: r[kk] is not None and abs(r[k] - r[kk]) <= tol)
    note_p = frac(lambda r: r["note"] == r["played"], rows)
    note_s = frac(lambda r: r["note"] == r["sounding"], rows)
    note_o = frac(lambda r: (r["note"] - r["sounding"]) % 12 == 0, rows)
    att = frac(near("attack", "attack_seed", TOL_ENV), rows)
    rel = frac(near("release", "release_seed", TOL_ENV), rows)
    rel_g = frac(near("release", "release_seed_gate", TOL_ENV), rows)
    lp = [r for r in rows if r["flt_type"] == LPF and r["reso"] < RESO_MAX]
    cut = frac(near("cutoff", "cutoff_seed", TOL_CUT), lp)
    res = [("note", note_p, T_NOTE, f"== played note {note_p:.0%}; == sounding note {note_s:.0%}; "
                                    f"octave-equivalent {note_o:.0%}"),
           ("attack", att, T_ATTACK, f"within +-{TOL_ENV}: {att:.0%}"),
           ("release", rel, T_RELEASE, f"within +-{TOL_ENV}: {rel:.0%} (true gate given: {rel_g:.0%})"),
           ("cutoff", cut, T_CUTOFF, f"within +-{TOL_CUT}: {cut:.0%} of {len(lp)} LPF programs, RESO < {RESO_MAX}")]
    print(f"WP2 on {len(rows)} single programs, read blind -> {out / 'seeds.csv'}")
    ok = True
    for name, got, target, detail in res:
        good = got >= target
        ok &= good
        print(f"  [{'PASS' if good else 'FAIL'}] {name:8s} {detail} (target {target:.0%})")
    miss = [f"{r['slot']} {r['name']}" for r in rows if r["note"] != r["sounding"]]
    print(f"  sounding-note misses: {', '.join(miss) if miss else 'none'}")
    print(f"WP2: {'PASS' if ok else 'FAIL'} ({time.perf_counter() - t0:.0f} s)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
