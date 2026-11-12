# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match (WP3): the search core (target, render cache, worker pool, budget, CMA-ES) and Stage 2, the
structure search (proposal section 6).

  prob = Problem(audio, sr, note=60, gate_ms=None)       the target, conditioned and measured once
  best = structure_search(prob, seed)                    the best 2 layouts, each a short-fitted patch
  prob.close()

Stage 2: with the seeded amp envelope and filter, and no modulation or FX, render
  OSC1 wave (7) x CTRL1 (0, 64, 127) x OSC2 (off, or SQR / SAW / TRI at 0, +7, +12, -12)      273 layouts
on the sustain only, scored on D7 + D1. The best 5 are also tried at each filter type x slope (6) and at OSC SHIFT
-1 / +1 octave; each then gets a short fit (CMA-ES, pop 12, 40 generations) of its continuous oscillator and
filter bytes; the best 2 are kept. Sync, ring and X-MOD layouts are added when Stage 1 flags them or the best
sustain D1 is still poor.

Engine-neutral apart from the layout table (STRUCT_*), which names M-vance / JP-8080 bytes: it uses the Engine
interface, so a second backend would bring its own table.
"""
import os
import sys
import time
from multiprocessing import get_context
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import features as F                                   # noqa: E402
import loss as L                                       # noqa: E402

SUSTAIN_MAX_S = 1.0                     # s: the sustain window structure scoring uses (from the onset)
TAIL_MS = 50                            # ms: rendered after the key-up in a sustain-window take
MAX_TOTAL_S = 8.0                       # s: longest take rendered (a longer target is cut to this)
W_STRUCT = {"d7": 1.0, "d1": 1.0}       # weights of the structure score (section 6: D7 + D1, sustain only)
KEEP_LAYOUTS = 5                        # layouts carried from the enumeration into the short fits
KEEP_FINAL = 2                          # layouts Stage 2 hands on
SHORT_POP, SHORT_GENS = 12, 40          # short fit: CMA-ES population and generations
SHORT_SIGMA = 0.15                      # short fit: start step (in 0..1 units)
POOR_D1 = 6.0                           # dB: best sustain D1 above this -> sync / ring / X-MOD layouts are added
PROGRESS_EVERY = 400                    # renders: a labelled fit logs its best this often

# M-vance / JP-8080 bytes of the layout table
B = {"OSC1_WAVE": 30, "OSC1_C1": 31, "OSC1_C2": 32, "OSC2_WAVE": 33, "OSC2_SYNC": 34, "OSC2_RANGE": 35,
     "OSC2_FINE": 36, "OSC2_C1": 37, "OSC2_C2": 38, "RING": 21, "XMOD": 22, "BALANCE": 23, "FLT_TYPE": 39,
     "FLT_SLOPE": 40, "CUTOFF": 41, "RESO": 42, "FENV_DEPTH": 46, "AENV_A": 54, "AENV_D": 55, "AENV_S": 56,
     "AENV_R": 57, "OSC_SHIFT": 73, "AMP_LEVEL": 51}
STRUCT_C1 = (0, 64, 127)                # OSC1 CTRL1 values tried
STRUCT_OSC2_WAVES = (0, 1, 2)           # SQR, SAW, TRI
STRUCT_RANGES = {0: 25, 7: 32, 12: 37, -12: 13}    # semitones -> OSC2 RANGE byte (25 = 0)
BAL_OSC1_ONLY, BAL_BOTH = 0, 64         # BALANCE: 0 = OSC1 alone, 64 = both full
SHIFT_CENTRE = 2                        # OSC SHIFT byte of 0 octaves (0..4 = -2..+2)
SHORT_FIT = ["OSC1_C1", "OSC1_C2", "OSC2_C1", "OSC2_C2", "OSC2_FINE", "BALANCE", "CUTOFF", "RESO"]

# --- worker side (one engine per process) ---------------------------------------------------------------------
_W: Dict = {}


def _worker_init(host: str, root: str, settings: Dict):
    """Pool initialiser: an engine on the shared host binary, and the target measures by window."""
    import engine_mvance as em
    _W["eng"] = em.MvanceEngine(root, host=host)
    _W.update(settings)


def _features_of(audio: np.ndarray, gate_ms: int, f0: float, need) -> Dict:
    """Condition a render (onset and key-up known) and measure it."""
    on = _W["note_on_ms"] / 1000
    c = F.condition(audio, onset_s=on, keyoff_s=on + gate_ms / 1000)
    return F.features(c, f0=f0, need=need, floor=_W.get("floor"))  # (the TARGET's noise floor, if it has one)


def _evaluate(job: Tuple[bytes, str, Dict]):
    """(patch, window, weights) -> (loss, parts): render the take the window needs and score it."""
    patch, window, weights = job
    win = _W["windows"][window]
    x = _W["eng"].render(patch, _W["note"], _W["vel"], win["gate_ms"], win["total_ms"])
    need = [k for k, v in weights.items() if v]
    cf = _features_of(x, win["gate_ms"], _W["f0"], need)
    return L.loss(win["target"], cf, weights)


# --- the problem ----------------------------------------------------------------------------------------------

class Problem:
    """A target sample and everything needed to score patches against it: measures by window ("full": the
    whole take; "sustain": the first SUSTAIN_MAX_S s up to the key-off), a render cache keyed by patch and window
    and weights, a process pool (workers), and a render count against a budget."""

    def __init__(self, audio: np.ndarray, sr: int, note: float, gate_ms: Optional[float] = None,
                 vel: int = 100, workers: Optional[int] = None, root: Optional[Path] = None,
                 budget: int = 8000, engine=None):
        import engine_mvance as em
        self.em = em
        self.engine = engine or em.MvanceEngine(root)
        self._own_engine = engine is None
        self.note_f = float(note)
        self.note = int(round(note))                    # rendered note (the engine takes whole notes)
        self.f0 = F.midi_hz(self.note_f)
        self.vel = vel
        c = F.condition(audio, sr)
        if gate_ms is not None:
            c["keyoff_s"] = gate_ms / 1000
        self.cond = c
        self.floor = F.noise_floor(c)                    # (a recording's noise; None for a clean take)
        self.gate_ms = int(round(c["keyoff_s"] * 1000))
        dur = min(c["dur_s"], MAX_TOTAL_S)
        full_total = em.NOTE_ON_MS + int(dur * 1000)
        self.gate_ms = max(1, min(self.gate_ms, full_total - em.NOTE_ON_MS - 1))
        sus = int(min(c["keyoff_s"], SUSTAIN_MAX_S) * 1000)
        self.windows = {"full": {"gate_ms": self.gate_ms, "total_ms": full_total},
                        "sustain": {"gate_ms": max(sus, 50), "total_ms": em.NOTE_ON_MS + max(sus, 50) + TAIL_MS}}
        for name, w in self.windows.items():            # the target's measures for each window
            n = int((w["total_ms"] - em.NOTE_ON_MS) / 1000 * F.SR)
            cw = dict(c, mono=c["mono"][:n], stereo=c["stereo"][:n], keyoff_s=w["gate_ms"] / 1000)
            if name == "sustain":                       # (the target cut at its sustain window's end)
                k = int(w["gate_ms"] / 1000 * F.SR)
                cw = dict(cw, mono=cw["mono"][:k], stereo=cw["stereo"][:k])
            w["target"] = F.features(cw, f0=self.f0, floor=self.floor)
        self.budget = budget
        self.renders = 0
        self.cache: Dict = {}
        self.log: List[str] = []
        self.t0 = time.perf_counter()
        n = workers if workers is not None else (os.cpu_count() or 1)
        settings = {"windows": {k: {kk: vv for kk, vv in w.items()} for k, w in self.windows.items()},
                    "note": self.note, "vel": vel, "f0": self.f0, "note_on_ms": em.NOTE_ON_MS, "floor": self.floor}
        self.pool = None
        if n > 1:
            self.pool = get_context("fork" if sys.platform != "win32" else "spawn").Pool(
                n, _worker_init, (str(self.engine.host), str(self.engine.root), settings))
        else:
            _W["eng"] = self.engine
            _W.update(settings)
        self.workers = n

    def evaluate(self, patches: Sequence[bytes], window: str = "full",
                 weights: Optional[Dict[str, float]] = None) -> List[Tuple[float, Dict[str, float]]]:
        """The (loss, parts) of each patch; repeats come from the cache and do not count against the budget."""
        w = dict(weights or L.WEIGHTS)
        wkey = tuple(sorted(w.items()))
        keys = [(bytes(p), window, wkey) for p in patches]
        todo = list(dict.fromkeys(k for k in keys if k not in self.cache))
        if todo:
            jobs = [(k[0], window, w) for k in todo]
            res = self.pool.map(_evaluate, jobs) if self.pool else [_evaluate(j) for j in jobs]
            for k, r in zip(todo, res):
                self.cache[k] = r
            self.renders += len(todo)
        return [self.cache[k] for k in keys]

    def render(self, patch: bytes, window: str = "full") -> np.ndarray:
        """One take of the patch (the window's gate and length), not cached (for output)."""
        w = self.windows[window]
        return self.engine.render(bytes(patch), self.note, self.vel, w["gate_ms"], w["total_ms"])

    def left(self) -> int:
        """Renders left in the budget."""
        return self.budget - self.renders

    def note_log(self, msg: str):
        """A line in the run log (with the elapsed time and renders so far)."""
        line = f"{time.perf_counter() - self.t0:7.1f} s {self.renders:6d} renders  {msg}"
        self.log.append(line)

    def close(self):
        """Stop the workers and the engine."""
        if self.pool:
            self.pool.close()
            self.pool.join()
            self.pool = None
        if self._own_engine:
            self.engine.close()


# --- CMA-ES over a set of bytes -------------------------------------------------------------------------------

def cma_fit(prob: Problem, base: bytes, keys: Sequence[str], params: Dict[str, Dict], window: str,
            weights: Dict[str, float], pop: int, gens: int, sigma: float, seed: int,
            max_renders: Optional[int] = None, label: Optional[str] = None) -> Tuple[bytes, float]:
    """Optimise the bytes named by keys (each in its own range, mapped to 0..1: byte = round(min + x (max - min)))
    from base with CMA-ES. Stops after gens generations, when the budget (or max_renders) is spent, or when CMA
    stops. Returns (best patch, its loss). Deterministic for a given seed. label: if given, a log line every
    PROGRESS_EVERY renders (a long fit shows it is moving)."""
    import cma
    prm = [params[k] for k in keys]
    if not prm:
        return base, prob.evaluate([base], window, weights)[0][0]

    def to_patch(x):
        p = bytearray(base)
        for q, v in zip(prm, x):
            p[q["offset"]] = int(round(q["min"] + min(max(v, 0.0), 1.0) * (q["max"] - q["min"])))
        return bytes(p)

    x0 = [(base[q["offset"]] - q["min"]) / max(q["max"] - q["min"], 1) for q in prm]
    best_p = base
    best = prob.evaluate([base], window, weights)[0][0]
    start = prob.renders
    if len(prm) == 1:                                   # (CMA needs 2+ dimensions: a 1-D line search instead)
        q = prm[0]
        vals = list(range(q["min"], q["max"] + 1))
        cand = [to_patch([(v - q["min"]) / max(q["max"] - q["min"], 1)]) for v in vals]
        for p, (l, _) in zip(cand, prob.evaluate(cand, window, weights)):
            if l < best:
                best, best_p = l, p
        return best_p, best
    es = cma.CMAEvolutionStrategy(x0, sigma, {"popsize": pop, "bounds": [0, 1], "seed": seed, "maxiter": gens,
                                              "verbose": -9, "tolfun": 1e-4, "tolx": 1e-3})
    while not es.stop():
        if prob.left() < pop or (max_renders is not None and prob.renders - start >= max_renders):
            break
        xs = es.ask()
        ps = [to_patch(x) for x in xs]
        ls = [r[0] for r in prob.evaluate(ps, window, weights)]
        es.tell(xs, ls)
        i = int(np.argmin(ls))
        if ls[i] < best:
            best, best_p = ls[i], ps[i]
        if label and prob.renders // PROGRESS_EVERY != (prob.renders - len(ps)) // PROGRESS_EVERY:
            prob.note_log(f"{label}: best so far {best:.2f}")
    return best_p, best


# --- Stage 2 ---------------------------------------------------------------------------------------------------

def base_patch(prob: Problem, seed: Dict) -> bytes:
    """INIT with the seed's amp envelope, cutoff and filter guesses; FX and modulation stay at INIT (off)."""
    p = bytearray(prob.engine.init_patch())
    sb = seed.get("bytes", {})
    for k in ("AENV_A", "AENV_D", "AENV_S", "AENV_R", "CUTOFF"):
        if k in sb:
            p[B[k]] = sb[k]
    flt = seed.get("filter", {})
    p[B["FLT_SLOPE"]] = 1 if flt.get("pole_guess", 24) == 24 else 0
    p[B["FLT_TYPE"]] = 2                                # LPF first; the type is enumerated on the best layouts
    if flt.get("reso"):
        p[B["RESO"]] = 64
    return bytes(p)


def layouts(base: bytes, extra: bool) -> List[Tuple[str, bytes]]:
    """The Stage 2 layouts on base: (label, patch). extra: sync, ring and X-MOD layouts too."""
    out = []
    for w1 in range(7):
        for c1 in STRUCT_C1:
            p = bytearray(base)
            p[B["OSC1_WAVE"]], p[B["OSC1_C1"]], p[B["BALANCE"]] = w1, c1, BAL_OSC1_ONLY
            out.append((f"osc1 {w1} c1 {c1} osc2 off", bytes(p)))
            for w2 in STRUCT_OSC2_WAVES:
                for st, rb in STRUCT_RANGES.items():
                    q = bytearray(p)
                    q[B["OSC2_WAVE"]], q[B["OSC2_RANGE"]], q[B["BALANCE"]] = w2, rb, BAL_BOTH
                    out.append((f"osc1 {w1} c1 {c1} osc2 {w2} {st:+d}", bytes(q)))
    if extra:
        for w1 in (4, 5):                               # SQR / SAW: sync, ring and X-MOD apply to them
            for rb in (32, 37, 44):
                p = bytearray(base)
                p[B["OSC1_WAVE"]], p[B["OSC2_WAVE"]], p[B["OSC2_RANGE"]] = w1, 1, rb
                s = bytearray(p)
                s[B["OSC2_SYNC"]], s[B["BALANCE"]] = 1, 127
                out.append((f"osc1 {w1} sync osc2 range {rb}", bytes(s)))
                r = bytearray(p)
                r[B["RING"]], r[B["BALANCE"]] = 1, 127
                out.append((f"osc1 {w1} ring osc2 range {rb}", bytes(r)))
                x = bytearray(p)
                x[B["XMOD"]], x[B["BALANCE"]] = 64, 64
                out.append((f"osc1 {w1} xmod osc2 range {rb}", bytes(x)))
    return out


def structure_search(prob: Problem, seed: Dict, keep: int = KEEP_FINAL, seed_rng: int = 1,
                     short_gens: int = SHORT_GENS) -> List[Dict]:
    """Stage 2 (see the module text). Returns up to keep {"label", "patch", "loss"}, best first."""
    params = {p["key"]: p for p in prob.engine.params()}
    base = base_patch(prob, seed)
    flags = seed.get("flags", {})
    lay = layouts(base, bool(flags.get("sync") or flags.get("inharmonic")))
    res = prob.evaluate([p for _, p in lay], "sustain", W_STRUCT)
    ranked = sorted(zip([r[0] for r in res], [r[1] for r in res], lay), key=lambda t: t[0])
    if ranked[0][1].get("d1", 0) > POOR_D1 and not (flags.get("sync") or flags.get("inharmonic")):
        more = layouts(base, True)[len(lay):]           # (the extra layouts come after the plain ones)
        if more:
            r2 = prob.evaluate([p for _, p in more], "sustain", W_STRUCT)
            ranked = sorted(ranked + list(zip([r[0] for r in r2], [r[1] for r in r2], more)), key=lambda t: t[0])
    prob.note_log(f"stage 2: {len(ranked)} layouts, best {ranked[0][2][0]} ({ranked[0][0]:.2f})")
    top = []
    for loss0, _, (label, p) in ranked[:KEEP_LAYOUTS]:
        variants = []                                   # filter type x slope, and OSC SHIFT -1 / +1
        for t in range(3):
            for s in range(2):
                q = bytearray(p)
                q[B["FLT_TYPE"]], q[B["FLT_SLOPE"]] = t, s
                variants.append((f"{label} flt {t}/{s}", bytes(q)))
        for sh in (SHIFT_CENTRE - 1, SHIFT_CENTRE + 1):
            q = bytearray(p)
            q[B["OSC_SHIFT"]] = sh
            variants.append((f"{label} shift {sh - SHIFT_CENTRE:+d}", bytes(q)))
        vr = prob.evaluate([v for _, v in variants], "sustain", W_STRUCT)
        cands = [(loss0, label, p)] + [(r[0], lab, v) for r, (lab, v) in zip(vr, variants)]
        top.append(min(cands, key=lambda t: t[0]))
    out = []
    for i, (l0, label, p) in enumerate(top):
        keys = [k for k in SHORT_FIT if p[B["BALANCE"]] != BAL_OSC1_ONLY or not k.startswith("OSC2")]
        bp, bl = cma_fit(prob, p, keys, params, "sustain", W_STRUCT, SHORT_POP, short_gens, SHORT_SIGMA,
                         seed_rng + i)
        out.append({"label": label, "patch": bp, "loss": bl, "loss_before": l0})
        prob.note_log(f"stage 2 short fit {i + 1}: {label}: {l0:.2f} -> {bl:.2f}")
    out.sort(key=lambda d: d["loss"])
    return out[:keep]
