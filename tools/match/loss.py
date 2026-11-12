# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match (WP1): the distance between two feature sets (features.py), as a weighted sum of D1..D8.

  total, parts = loss(target_f, candidate_f, weights)     parts: {"d1": .., ..}, each before weighting

Each part is a mean absolute difference in the measure's own unit: dB (D1, D2, D3, D7), semitones (D4, D5, D6),
or a scaled mix (D8). Where the two takes differ in length the common length is compared. loss(x, x) is 0.
Starting weights are the proposal's (section 5); WP6 tunes them.
"""
from typing import Dict, Optional, Tuple

import numpy as np

WEIGHTS = {"d1": 1.0, "d2": 0.5, "d3": 0.5, "d4": 0.3, "d5": 0.3, "d6": 0.3, "d7": 0.5, "d8": 0.2}
D4_GATE_DB = -50.0                      # dB: centroid frames quieter than this (either side) are not compared
D5_UNVOICED_ST = 12.0                   # semitones: the cost of a target-voiced frame the candidate leaves unvoiced
D5_CAP_ST = 12.0                        # semitones: the largest one-frame pitch error counted
D8_SIDE_SCALE = 0.1                     # D8: per dB of side / mid energy difference
D8_CORR_SCALE = 2.0                     # D8: per unit of L/R correlation difference
D8_ACORR_SCALE = 5.0                    # D8: per unit of mean echo-autocorrelation difference


def _mad(a: np.ndarray, b: np.ndarray) -> float:
    """Mean absolute difference over the common leading length (0 if either is empty)."""
    n = min(len(a), len(b))
    return float(np.mean(np.abs(a[:n] - b[:n]))) if n else 0.0


def part_d4(t: Dict, c: Dict) -> float:
    """D4: centroid difference in semitones over frames where both sides sound (above D4_GATE_DB, or above the
    target's noise floor when it has one)."""
    n = min(len(t["d4"]), len(c["d4"]))
    gate = max(D4_GATE_DB, float(t["d4gate"][0])) if "d4gate" in t else D4_GATE_DB   # (a recording's noise floor)
    m = (t["d4db"][:n] > gate) & (c["d4db"][:n] > gate)
    return float(np.mean(np.abs(t["d4"][:n][m] - c["d4"][:n][m])) * 12.0) if m.any() else 0.0


def part_d5(t: Dict, c: Dict) -> float:
    """D5: pitch difference in semitones over the target's voiced frames (capped; candidate unvoiced: a fixed cost)."""
    n = min(len(t["d5"]), len(c["d5"]))
    tv, cv = t["d5v"][:n], c["d5v"][:n]
    if not tv.any():
        return 0.0
    err = np.where(cv, np.minimum(np.abs(t["d5"][:n] - c["d5"][:n]) / 100.0, D5_CAP_ST), D5_UNVOICED_ST)
    return float(np.mean(err[tv]))


def part_d8(t: Dict, c: Dict) -> float:
    """D8: side / mid energy (dB), L/R correlation and echo autocorrelation differences, scaled and summed."""
    a, b = t["d8"], c["d8"]
    return float(D8_SIDE_SCALE * abs(a[0] - b[0]) + D8_CORR_SCALE * abs(a[1] - b[1]) +
                 D8_ACORR_SCALE * np.mean(np.abs(a[2:] - b[2:])))


PART = {"d1": lambda t, c: _mad(t["d1"], c["d1"]), "d2": lambda t, c: _mad(t["d2"], c["d2"]),
        "d3": lambda t, c: _mad(t["d3"], c["d3"]), "d4": part_d4, "d5": part_d5,
        "d6": lambda t, c: float(np.mean(np.abs(t["d6"] - c["d6"]))),
        "d7": lambda t, c: float(np.mean(np.abs(t["d7"] - c["d7"]))), "d8": part_d8}


def parts(t: Dict, c: Dict, which=None) -> Dict[str, float]:
    """Each measure's distance, unweighted: those in which (default: every one both feature sets hold)."""
    keys = which if which is not None else [k for k in PART if k in t and k in c]
    return {k: PART[k](t, c) for k in keys}


def loss(target: Dict, candidate: Dict, weights: Optional[Dict[str, float]] = None) -> Tuple[float, Dict[str, float]]:
    """(weighted total, unweighted parts). weights: any subset of WEIGHTS' keys (missing ones weigh 0 when a dict
    is given; None = WEIGHTS). Only measures with a weight and present in both sets are compared."""
    w = WEIGHTS if weights is None else weights
    p = parts(target, candidate, [k for k in PART if w.get(k, 0.0) and k in target and k in candidate])
    return float(sum(w.get(k, 0.0) * v for k, v in p.items())), p
