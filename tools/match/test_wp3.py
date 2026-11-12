#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match WP3 acceptance: the structure search (structure.py, Stage 2) on renders of the 63 single factory
programs. The note played and the gate are given (as WP6 Level A gives them); the rest of the seed comes from
analyse.py.

  python3 tools/match/test_wp3.py [--only N ..] [--workers N] [--out DIR]   (default DIR: build/match/wp3)

  wave    the true OSC1 wave is the OSC1 wave of one of the best 2 layouts, target 80 %
  filter  the best layout's filter type is the true one, target 90 %

Misses are listed by program name; DIR/structure.csv has every program's two layouts and scores. Exit status 1 if
a target is missed. The targets are the proposal's (WP3): do not edit them to make a run pass.
"""
import argparse
import csv
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyse as A                                    # noqa: E402
import engine_mvance as em                             # noqa: E402
import structure as S                                  # noqa: E402
from test_wp0 import note_for, VEL, GATE_MS, TOTAL_MS  # noqa: E402

T_WAVE, T_FILTER = 0.80, 0.90           # fractions that must be reached
GATE_FROM_ONSET_MS = GATE_MS            # ms: the key is held this long after the onset in every take


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", type=Path, default=em.ROOT)
    ap.add_argument("--only", type=int, nargs="*", help="program slots to run (default: every single)")
    ap.add_argument("--workers", type=int, default=None)
    ap.add_argument("--out", type=Path)
    a = ap.parse_args()
    root = a.root.resolve()
    out = (a.out or root / "build/match/wp3").resolve()
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.perf_counter()
    singles = [p for p in em.load_factory(root) if p["perf"][0] == 0 and (not a.only or p["slot"] in a.only)]
    rows = []
    with em.MvanceEngine(root) as eng:
        for p in singles:
            P, note = p["patch"], note_for(p["name"])
            x = eng.render(P, note, VEL, GATE_MS, TOTAL_MS)
            seed = A.analyse(x, eng.SAMPLE_RATE, eng, note=note, gate_ms=GATE_FROM_ONSET_MS)
            prob = S.Problem(x, eng.SAMPLE_RATE, note, gate_ms=GATE_FROM_ONSET_MS, engine=eng, workers=a.workers)
            try:
                best = S.structure_search(prob, seed)
            finally:
                prob.close()
            waves = [b["patch"][S.B["OSC1_WAVE"]] for b in best]
            row = {"slot": p["slot"], "name": p["name"], "osc1": P[S.B["OSC1_WAVE"]], "flt": P[S.B["FLT_TYPE"]],
                   "found_osc1": " ".join(map(str, waves)), "found_flt": best[0]["patch"][S.B["FLT_TYPE"]],
                   "best": best[0]["label"], "loss": f"{best[0]['loss']:.2f}",
                   "second": best[1]["label"] if len(best) > 1 else "", "renders": prob.renders,
                   "seconds": f"{time.perf_counter() - prob.t0:.1f}"}
            row["wave_ok"] = P[S.B["OSC1_WAVE"]] in waves
            row["flt_ok"] = row["found_flt"] == row["flt"]
            rows.append(row)
            print(f"  {p['slot']:2d} {p['name']:11s} osc1 {row['osc1']} found {row['found_osc1']:4s} "
                  f"{'ok ' if row['wave_ok'] else 'MISS'}  filter {row['flt']} found {row['found_flt']} "
                  f"{'ok ' if row['flt_ok'] else 'MISS'}  loss {row['loss']:>6s}  {row['renders']} renders "
                  f"{row['seconds']} s", flush=True)
    with open(out / "structure.csv", "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    wave = sum(r["wave_ok"] for r in rows) / len(rows)
    flt = sum(r["flt_ok"] for r in rows) / len(rows)
    ok = wave >= T_WAVE and flt >= T_FILTER
    print(f"WP3 on {len(rows)} programs -> {out / 'structure.csv'}")
    print(f"  [{'PASS' if wave >= T_WAVE else 'FAIL'}] wave   true OSC1 wave in the best 2: {wave:.0%} (target {T_WAVE:.0%})")
    print(f"     misses: {', '.join(r['name'] for r in rows if not r['wave_ok']) or 'none'}")
    print(f"  [{'PASS' if flt >= T_FILTER else 'FAIL'}] filter best layout's type right: {flt:.0%} (target {T_FILTER:.0%})")
    print(f"     misses: {', '.join(r['name'] for r in rows if not r['flt_ok']) or 'none'}")
    print(f"WP3: {'PASS' if ok else 'FAIL'} ({time.perf_counter() - t0:.0f} s)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
