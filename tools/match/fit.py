# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match (WP4): Stage 3, the staged fit, and Stage 4, the polish (proposal section 6), behind one call.

  res = match(audio, sr, note, gate_ms=None, budget=8000, seed_rng=1, workers=None)
  res["patch"], res["loss"], res["parts"], res["log"], res["stages"], res["seed"]

Order: Stage 1 (analyse.py), Stage 2 (structure.py: the best 2 layouts), then on each of those Stage 3: the groups
in turn, each first trying every value of its enums / switches one byte at a time, then CMA-ES over its
continuous bytes against the measures that group controls; then the structural choices (waves, filter type and
slope, OSC SHIFT) are tried again on the whole take, now that the envelopes are fitted (Stage 2 judges them on a
static sustain: a pluck's brightness lives in its filter envelope, so its wave only shows once that is in). The better of the two goes to Stage 4: every
continuous byte of G1..G7 together, small steps, full distance. AMP LEVEL last, to the sample's RMS.

Budget: renders (cache hits are free). Stage 2 takes what it takes (about 2700); Stage 3 shares STAGE3_SHARE of
what is left between its groups and both layouts; Stage 4 gets the rest (restarting smaller when it settles). Fixed seed_rng: a run repeats exactly.
"""
import sys
from pathlib import Path
from typing import Dict, List, Optional

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyse as A                                    # noqa: E402
import features as F                                   # noqa: E402
import loss as L                                       # noqa: E402
import structure as S                                  # noqa: E402

W = L.WEIGHTS
# Stage 3 groups: (group, window, measures it is fitted against; section 6 table, section 5 weights)
GROUPS = [
    ("G1", "sustain", {"d7": W["d7"], "d1": W["d1"]}),          # oscillators
    ("G2", "sustain", {"d7": W["d7"], "d1": W["d1"]}),          # filter
    ("G3", "full", {"d3": 1.0}),                                # amp envelope
    ("G4", "full", {"d4": W["d4"], "d1": W["d1"]}),             # filter envelope
    ("G5", "full", {"d5": W["d5"], "d6": W["d6"]}),             # pitch envelope, LFO1
    ("G6", "full", {"d8": W["d8"], "d1": W["d1"]}),             # tone and FX
    ("G7", "full", {"d6": W["d6"], "d1": W["d1"]}),             # voice
]
STRUCTURAL = {"OSC1_WAVE", "OSC2_WAVE", "OSC2_SYNC", "RING", "OSC_SHIFT", "FLT_TYPE", "FLT_SLOPE"}  # set in Stage 2
STAGE3_SHARE = 0.45                     # of the renders left after Stage 2
STAGE3_POP, STAGE3_SIGMA = 10, 0.15     # CMA-ES per group
POLISH_POP, POLISH_SIGMA = 16, 0.05     # Stage 4 (section 6): population, start step
POLISH_RESTARTS = 4                     # Stage 4: when CMA settles with budget left, it restarts from the best with
POLISH_SHRINK = 0.5                     #  its step times this, up to this many times (so a bigger budget is used)
LEVEL_RESERVE = 12                      # renders kept back from the polish for the AMP LEVEL search
LEVEL_KEY = "AMP_LEVEL"
LEVEL_TOL_DB = 0.25                     # dB: the AMP LEVEL search stops this close to the target's RMS
# seed -> starting depth for an LFO the analysis found (byte offset from the centre, 64)
LFO_DEPTH = {"pitch": ("OSC_LFO1", 8), "filter": ("FLT_LFO1", 16), "amp": ("AMP_LFO1", 16)}


def seeded(prob: S.Problem, patch: bytes, seed: Dict, params: Dict) -> bytes:
    """Starting values Stage 3 takes from the seed beyond Stage 2's: an LFO the analysis found, and the effects
    switched on when it heard width or echoes."""
    p = bytearray(patch)
    lfo = seed.get("lfo")
    sb = seed.get("bytes", {})
    if lfo and "LFO1_RATE" in sb:
        p[params["LFO1_RATE"]["offset"]] = sb["LFO1_RATE"]
        key, dep = LFO_DEPTH[lfo["dest"]]
        p[params[key]["offset"]] = 64 + dep
    if seed.get("stereo", {}).get("side_mid_db", -80) > -20:
        p[params["MFX_LEVEL"]["offset"]] = 64
    if seed.get("echo_ms"):
        p[params["DLY_LEVEL"]["offset"]] = 64
        p[params["DLY_FBK"]["offset"]] = 32
    return bytes(p)


def enumerate_bytes(prob: S.Problem, patch: bytes, keys: List[str], params: Dict, window: str,
                    weights: Dict) -> bytes:
    """Coordinate search over enum / switch bytes: each in turn takes the value that scores best."""
    best_p = patch
    best = prob.evaluate([patch], window, weights)[0][0]
    for k in keys:
        q = params[k]
        cands = []
        for v in range(q["min"], q["max"] + 1):
            c = bytearray(best_p)
            c[q["offset"]] = v
            cands.append(bytes(c))
        for c, (l, _) in zip(cands, prob.evaluate(cands, window, weights)):
            if l < best:
                best, best_p = l, c
    return best_p


def stage3(prob: S.Problem, patch: bytes, params: Dict, budget: int, seed_rng: int) -> bytes:
    """The groups in order (GROUPS): enums, then CMA-ES over the continuous bytes, each group within budget / 7."""
    per = max(budget // len(GROUPS), STAGE3_POP * 3)
    for i, (g, window, weights) in enumerate(GROUPS):
        keys = [k for k, q in params.items() if q["group"] == g and q["size"] == 1]
        enums = [k for k in keys if params[k]["kind"] != "cont" and k not in STRUCTURAL]
        conts = [k for k in keys if params[k]["kind"] == "cont"]
        start = prob.renders
        before = prob.evaluate([patch], window, weights)[0][0]
        patch = enumerate_bytes(prob, patch, enums, params, window, weights)
        left = per - (prob.renders - start)
        gens = max(left // STAGE3_POP, 2)
        patch, after = S.cma_fit(prob, patch, conts, params, window, weights, STAGE3_POP, gens, STAGE3_SIGMA,
                                 seed_rng + i, max_renders=max(left, STAGE3_POP * 2))
        prob.note_log(f"stage 3 {g}: {before:.2f} -> {after:.2f}")
    return patch


def fit_level(prob: S.Problem, patch: bytes, params: Dict) -> bytes:
    """AMP LEVEL by bisection to the target's RMS (both measured as condition() normalises them)."""
    q = params[LEVEL_KEY]
    want = -prob.cond["gain_db"]                         # the target's level, dB re RMS 1
    on = prob.em.NOTE_ON_MS / 1000

    def level(v):
        p = bytearray(patch)
        p[q["offset"]] = v
        x = prob.render(bytes(p))
        prob.renders += 1
        c = F.condition(x, onset_s=on, keyoff_s=on + prob.gate_ms / 1000)
        return -c["gain_db"], bytes(p)

    lo, hi = q["min"], q["max"]
    best = None
    for _ in range(8):                                  # (128 values: 7 halvings and a check)
        mid = (lo + hi) // 2
        lv, p = level(mid)
        if best is None or abs(lv - want) < abs(best[0] - want):
            best = (lv, p)
        if abs(lv - want) <= LEVEL_TOL_DB or lo >= hi:
            break
        if lv < want:
            lo = mid + 1
        else:
            hi = mid - 1
    prob.note_log(f"level: AMP LEVEL {best[1][q['offset']]} ({best[0]:.1f} dB, target {want:.1f} dB)")
    return best[1]


def match(audio: np.ndarray, sr: int, note: Optional[float] = None, gate_ms: Optional[float] = None,
          budget: int = 8000, seed_rng: int = 1, workers: Optional[int] = None, engine=None,
          progress=None) -> Dict:
    """The whole search (see the module text). note / gate_ms: known values (else read by the analysis).
    progress: called with each log line as it happens. Returns {"patch", "loss", "parts", "d1_stage2",
    "log", "seed", "renders", "note", "gate_ms", "problem"} (close res["problem"] when done with it)."""
    import engine_mvance as em
    eng = engine or em.MvanceEngine()
    seed = A.analyse(audio, sr, eng, note=note, gate_ms=gate_ms)
    note_f = note if note is not None else seed["note"] + seed["cents"] / 100
    prob = S.Problem(audio, sr, note_f, gate_ms=seed["gate_ms"], workers=workers, engine=eng, budget=budget)
    if progress:
        orig = prob.note_log

        def logged(msg):
            orig(msg)
            progress(prob.log[-1])
        prob.note_log = logged
    prob.note_log(f"stage 1: note {seed['note']} {seed['cents']:+.0f} c, gate {seed['gate_ms']} ms, "
                  f"waves {seed['waves']}")
    params = {p["key"]: p for p in eng.params()}
    two = S.structure_search(prob, seed, seed_rng=seed_rng)
    full = dict(W)
    d1_stage2 = prob.evaluate([two[0]["patch"]], "full", full)[0][1]["d1"]
    prob.note_log(f"stage 2 done: best {two[0]['label']}, full-take D1 {d1_stage2:.2f} dB")
    share = int(prob.left() * STAGE3_SHARE) // max(len(two), 1)
    fitted = []
    revisit = [k for k in ("OSC1_WAVE", "OSC2_WAVE", "FLT_TYPE", "FLT_SLOPE", "OSC_SHIFT") if k in params]
    for i, t in enumerate(two):
        p = stage3(prob, seeded(prob, t["patch"], seed, params), params, share, seed_rng + 10 * (i + 1))
        p = enumerate_bytes(prob, p, revisit, params, "full", full)   # (structure again, with the envelopes in)
        fitted.append((prob.evaluate([p], "full", full)[0][0], p, t["label"]))
        prob.note_log(f"stage 3 done for {t['label']}: {fitted[-1][0]:.2f}")
    fitted.sort(key=lambda t: t[0])
    best_l, patch, label = fitted[0]
    polish = [k for k, q in params.items() if q["group"] in {g for g, _, _ in GROUPS} and q["kind"] == "cont"
              and q["size"] == 1]
    sigma = POLISH_SIGMA
    for r in range(POLISH_RESTARTS + 1):                # (the whole budget, not a fixed number of generations)
        left = prob.left() - LEVEL_RESERVE
        if left < POLISH_POP * 4:
            break
        patch, best_l = S.cma_fit(prob, patch, polish, params, "full", full, POLISH_POP, left // POLISH_POP, sigma,
                                  seed_rng + 100 + r, max_renders=left, label="stage 4 polish")
        sigma *= POLISH_SHRINK
    prob.note_log(f"stage 4 polish: {best_l:.2f}")
    patch = fit_level(prob, patch, params)
    loss, parts = prob.evaluate([patch], "full", full)[0]
    prob.note_log(f"done: loss {loss:.2f}, D1 {parts['d1']:.2f} dB ({label})")
    return {"patch": patch, "loss": loss, "parts": parts, "d1_stage2": d1_stage2, "log": prob.log, "seed": seed,
            "renders": prob.renders, "note": prob.note, "gate_ms": prob.gate_ms, "problem": prob}
