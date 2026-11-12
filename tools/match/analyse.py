# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match (WP2): Stage 1, the analysis. Reads a one-note sample and writes the seed the search starts from
(proposal section 4: "the breakdown by fundamentals").

  seed = analyse(audio, sr, engine, note=None, gate_ms=None)       a dict, as seed.json
  python3 tools/match/analyse.py sample.wav [--note N] [--gate-ms N] [--out seed.json]

What it reads, and from which measure:
  note       median pitch of the sustain (D5), MIDI note and cents offset; an octave up when the odd harmonics of
             that pitch are missing (a sub oscillator an octave down reads as the lower octave)
  gate       key-off: where the loudness leaves its last plateau and falls (or --gate-ms)
  amp env    attack (onset to peak), decay (to 1/e of the way to sustain), sustain (level at key-off re the peak),
             release (dB slope after key-off): as times, then as bytes through the engine's laws (engine.invert)
  filter     a two-slope fit to the harmonic levels of the sustain: the knee is the cutoff seed, the extra slope
             past it the 12 / 24 dB guess, a bump at the knee the resonance flag; a weak fundamental the
             HPF / BPF flag
  waves      scores 0..1 for SAW, SQUARE, TRIANGLE, SUPER SAW, NOISE from the harmonic profile (D7)
  wobble     the strongest peak of the loudness / centroid / pitch wobble spectra (D6): LFO rate and destination
  echo       the strongest peak of the envelope autocorrelation (D8): delay lag
  stereo     side / mid energy and L/R correlation (D8)
  flags      sync (a formant bump above the 3rd harmonic), inharmonic (energy between the harmonics: ring / X-MOD)

Engine-neutral except through engine.invert (None when a backend has no law): seeds are physical first, bytes
second.
"""
import argparse
import json
import sys
from pathlib import Path
from typing import Dict, Optional

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import features as F                                   # noqa: E402

SMOOTH_MS = 30.0                        # ms: loudness smoothing for the envelope reading
ATTACK_PEAK_DB = 1.0                    # dB: (decay) the peak frame is the first this close to the loudest
ATTACK_LOOK_MS = 300.0                  # ms: "the loudest" is the loudest of the next this long (a slow drift
                                        #  later in the held note must not read as a slow attack)
ATTACK_3DB = 3.0                        # dB: the attack is timed to the first point this close to the loudest...
ATTACK_3DB_TAUS = 0.8346                # ...which the attack law (charge toward 1.25) reaches at ln(1.25 / (1.25 -
                                        #  10^(-3/20))) = 0.835 tau, against ln 5 = 1.609 tau to full: time to full =
                                        #  t(-3 dB) x 1.609 / 0.835
RELEASE_FIT_DB = 20.0                   # dB: the release slope is fitted over at most this fall...
RELEASE_FIT_MS = 400.0                  # ms: ...or this long...
RELEASE_REBOUND_DB = 2.0                # dB: ...or until the level rises this far above its lowest so far (an echo)
RELEASE_BACK_MS = 20.0                  # ms: the fall is looked for from this long before the key-off
ATTACK_MIN_S = 0.003                    # s: shorter than this reads as attack 0
ATTACK_ENV_MS = 2.0                     # ms: RMS frame of the fine envelope the attack is read on
KEYOFF_SPAN_MS = 300.0                  # ms: the slope before and after a candidate key-off is averaged over this
KEYOFF_MIN_BEND = 4.0                   # dB/s: the slope must steepen by at least this at a key-off
KEYOFF_LINE_DB = 1.0                    # dB: refined key-off = the last 2 ms frame within this (or 3x the plateau's own
                                        #  ripple about its line, if more) of the plateau's line
KEYOFF_LIVE_DB = 40.0                   # dB below the peak: a key-off must happen while the sound is louder than this
NEPER_DB = 8.685889638                  # dB in one neper (an exponential's fall in one tau)
DECAY_E = 0.36787944                    # 1/e: the decay time is where (level - sus) is this fraction of (peak - sus)
NOTE_OFFSETS = (-12, 0, 5, 7, 12, 19, 24)   # semitones from the tracked pitch: the played-note candidates
NOTE_HARMS = 10                         # harmonics a note candidate is scored on
NOTE_NEAR_DB = 6.0                      # dB: the lowest candidate scoring within this of the best is the note
NOTE_FUND_DB = 25.0                     # dB: a candidate's fundamental must be within this of the strongest peak
KNEE_MIN_DROP = 6.0                     # dB/oct: the extra slope past a knee needed to call it a low pass
RESO_BUMP_DB = 3.0                      # dB: a bump at the knee this high -> resonance flag
WOBBLE_MIN = {"amp": 0.5, "filter": 0.15, "pitch": 0.05}   # dB / semitones / semitones: smallest wobble reported
ECHO_MIN = 0.3                          # envelope autocorrelation peak needed to report an echo
SYNC_BUMP_DB = 6.0                      # dB: a harmonic this far above both neighbours' trend (above the 3rd) -> sync
ENDS_EARLY_DB = -30.0                   # dB re the peak: a take whose last 50 ms is louder than this was cut off
INHARM_DB = -30.0                       # dB re the strongest harmonic: noise between harmonics above this, while
                                        #  voiced -> inharmonic


def smooth_db(d3: np.ndarray) -> np.ndarray:
    """D3 (10 ms frames, dB) smoothed over SMOOTH_MS, on the linear scale."""
    k = max(1, int(SMOOTH_MS / F.D3_MS))
    lin = 10 ** (d3 / 20)
    return 20 * np.log10(np.maximum(np.convolve(lin, np.ones(k) / k, mode="same"), 1e-6))


def find_keyoff(db: np.ndarray, mono: Optional[np.ndarray] = None) -> Optional[float]:
    """Key-off in s from the onset. Coarse: where the loudness slope steepens most (the mean slope over the next
    KEYOFF_SPAN_MS minus that over the last), after the peak and while the sound is within KEYOFF_LIVE_DB of it;
    None if it never steepens by KEYOFF_MIN_BEND dB/s (a sound that dies before its key-off, or never stops).
    Fine (mono given): the last 2 ms frame, within a span of the coarse point, still within KEYOFF_LINE_DB (or 3x
    the ripple) of the line fitted to the span before it (the plateau, or a slow decay)."""
    w = int(KEYOFF_SPAN_MS / F.D3_MS)
    if len(db) < 3 * w:
        return None
    slope = np.gradient(db, F.D3_MS / 1000)             # dB / s per frame
    cs = np.concatenate([[0.0], np.cumsum(slope)])
    i = np.arange(w, len(db) - w)
    bend = (cs[i + w] - cs[i]) / w - (cs[i] - cs[i - w]) / w
    ok = (i > int(np.argmax(db))) & (db[i] > db.max() - KEYOFF_LIVE_DB)
    if not ok.any():
        return None
    bend = np.where(ok, bend, 0.0)
    j = int(np.argmin(bend))
    if bend[j] > -KEYOFF_MIN_BEND:
        return None
    coarse = i[j] * F.D3_MS / 1000
    if mono is None:
        return coarse
    span = KEYOFF_SPAN_MS / 1000
    a0 = max(0.0, coarse - 2 * span)
    fine = F.rms_db(mono[int(a0 * F.SR):int((coarse + span) * F.SR)], ATTACK_ENV_MS)
    t = a0 + np.arange(len(fine)) * ATTACK_ENV_MS / 1000
    pre = t <= coarse - span / 2
    if pre.sum() < 5:
        return coarse
    line = np.polyval(np.polyfit(t[pre], fine[pre], 1), t)
    tol = max(KEYOFF_LINE_DB, 3.0 * float(np.std(fine[pre] - line[pre])))
    on = np.nonzero(fine >= line - tol)[0]
    return float(t[on[-1]] + ATTACK_ENV_MS / 1000) if len(on) else coarse


def amp_envelope(db: np.ndarray, keyoff_s: float, mono: np.ndarray) -> Dict:
    """Attack (on a 2 ms envelope of mono), decay, sustain and release (on the smoothed loudness, dB, 10 ms frames)."""
    ko = min(int(keyoff_s * 1000 / F.D3_MS), len(db) - 1)
    pre = db[:max(ko, 1)]
    peak = float(pre.max())
    ipk = int(np.argmax(pre >= peak - ATTACK_PEAK_DB))
    fine = F.rms_db(mono[:int(keyoff_s * F.SR)], ATTACK_ENV_MS) if keyoff_s > 0.01 else np.zeros(1)
    fine = np.convolve(fine, np.ones(3) / 3, mode="same") if len(fine) > 3 else fine
    look = int(ATTACK_LOOK_MS / ATTACK_ENV_MS)         # the max of the next ATTACK_LOOK_MS at each point
    ahead = np.array([fine[i:i + look].max() for i in range(len(fine))]) if len(fine) else fine
    t3 = int(np.argmax(fine >= ahead - ATTACK_3DB)) * ATTACK_ENV_MS / 1000
    attack = t3 * 1.6094379 / ATTACK_3DB_TAUS          # time to full (what engine.invert takes)
    attack = 0.0 if t3 < ATTACK_MIN_S else attack
    lin = 10 ** ((db - peak) / 20)
    sus = float(np.clip(np.median(lin[max(ipk, ko - 5):ko]) if ko > ipk else lin[ko], 0, 1))
    decay = None
    if sus < 0.98 and ko > ipk:
        rel = (lin[ipk:ko] - sus) / max(1 - sus, 1e-6)
        j = np.nonzero(rel <= DECAY_E)[0]
        decay = (j[0] if len(j) else ko - ipk) * F.D3_MS / 1000
    release = None
    back = RELEASE_BACK_MS / 1000                       # (a key-off read a little late must not miss a fast fall)
    tail = F.rms_db(mono[int(max(keyoff_s - back, 0) * F.SR):], ATTACK_ENV_MS)
    if len(tail) > 4:
        tail = np.convolve(tail, np.ones(3) / 3, mode="valid")
        head = tail[:int(2 * back * 1000 / ATTACK_ENV_MS) + 1]
        drop = np.nonzero(head < head.max() - 1.0)[0]  # the fall starts 1 dB below the level before it
        tail = tail[max(int(drop[0]) - 1, 0):] if len(drop) else tail
        low, n = tail[0], len(tail)
        for i in range(1, len(tail)):
            low = min(low, tail[i])
            if tail[0] - tail[i] >= RELEASE_FIT_DB or i * ATTACK_ENV_MS >= RELEASE_FIT_MS or \
                    tail[i] > low + RELEASE_REBOUND_DB:
                n = i
                break
        n = max(n, 3)
        t = np.arange(n) * ATTACK_ENV_MS / 1000
        slope = np.polyfit(t, tail[:n], 1)[0]           # dB / s (negative)
        release = float(NEPER_DB / -slope) if slope < -1e-3 else 30.0
    return {"attack_s": attack, "decay_tau_s": decay, "sustain": sus, "release_tau_s": release,
            "peak_db": peak}


def filter_fit(d7: np.ndarray, f0: float) -> Dict:
    """Two-slope fit (dB against octaves) to the harmonic levels: the knee, the slope past it, a bump at it."""
    lev = d7[:F.D7_NHARM]
    k = np.arange(1, F.D7_NHARM + 1)
    ok = (lev > F.FLOOR_DB + 1) & (k * f0 < F.MEL_HI)
    if ok.sum() < 4:
        return {"cutoff_hz": float(f0 * 2), "slope_db_oct": None, "pole_guess": 24, "reso": False, "lowpass": True}
    x, y = np.log2(k[ok]), lev[ok]
    best = None
    for j in range(1, len(x) - 1):                      # knee after point j
        a = np.polyfit(x[:j + 1], y[:j + 1], 1) if j >= 1 else (0.0, y[0])
        pre = np.polyval(a, x)
        post = y[j:] - pre[j:]
        s2 = np.polyfit(x[j:] - x[j], post, 1)[0] if len(post) > 1 else 0.0
        fit = np.concatenate([pre[:j], pre[j:] + s2 * (x[j:] - x[j])])
        err = float(np.mean((fit - y) ** 2))
        if best is None or err < best[0]:
            best = (err, j, s2, a[0])
    _, j, s2, s1 = best
    lowpass = s2 < -KNEE_MIN_DROP
    knee = float(f0 * 2 ** x[j]) if lowpass else float(min(f0 * F.D7_NHARM, F.MEL_HI))
    bump = float(y[j] - np.polyval(np.polyfit(x[:j + 1], y[:j + 1], 1) if j else (0, y[0]), x[j])) if j else 0.0
    return {"cutoff_hz": knee, "slope_db_oct": float(s2), "source_slope_db_oct": float(s1),
            "pole_guess": 24 if s2 < -18 else 12, "reso": bool(bump > RESO_BUMP_DB), "lowpass": bool(lowpass)}


def wave_scores(d7: np.ndarray, voiced_frac: float) -> Dict[str, float]:
    """0..1 scores (section 4) from the harmonic profile: odd / even balance, slope of the first harmonics,
    cluster width, noise floor, how much was voiced."""
    lev = d7[:F.D7_NHARM]
    oe, noise, width = d7[F.D7_NHARM + 1], d7[F.D7_NHARM + 2], d7[F.D7_NHARM + 3]
    k = np.arange(1, 9)
    good = lev[:8] > F.FLOOR_DB + 1
    slope = np.polyfit(np.log2(k[good]), lev[:8][good], 1)[0] if good.sum() > 2 else -6.0
    g = lambda v, c, s: float(np.exp(-((v - c) / s) ** 2))
    oddness = 1 / (1 + np.exp(-(oe - 10) / 3))          # odd harmonics clearly louder than even
    sc = {"SAW": (1 - oddness) * g(slope, -6, 4),
          "SQUARE": oddness * g(slope, -6, 4),
          "TRIANGLE": oddness * g(slope, -12, 4),
          "SUPER_SAW": (1 - oddness) * (1 / (1 + np.exp(-(width - 60) / 15))),
          "NOISE": (1 - voiced_frac) * (1 / (1 + np.exp(-(noise + 25) / 5)))}
    return {k2: round(float(v), 3) for k2, v in sc.items()}


def wobble(d6: np.ndarray) -> Optional[Dict]:
    """The strongest wobble across the three tracks, if above its threshold: {"dest", "rate_hz", "depth"}."""
    grid = np.geomspace(F.D6_LO, F.D6_HI, F.D6_N)
    use = grid >= 0.5                                   # (below 0.5 Hz: the envelope's own shape, not an LFO)
    best = None
    for i, dest in enumerate(("amp", "filter", "pitch")):
        j = int(np.argmax(np.where(use, d6[i], 0)))
        rel = d6[i][j] / WOBBLE_MIN[dest]
        if d6[i][j] >= WOBBLE_MIN[dest] and (best is None or rel > best[0]):
            best = (rel, {"dest": dest, "rate_hz": float(grid[j]), "depth": float(d6[i][j])})
    return best[1] if best else None


def _sustain_psd(mono: np.ndarray, keyoff_s: float):
    """(freqs, power dB) of the sustain window D7 uses."""
    from scipy.signal import welch
    a = int(F.D7_START_MS * F.SR / 1000)
    b = int(min(max(keyoff_s * 1000, F.D7_START_MS + 200), F.D7_START_MS + F.D7_MAX_MS) * F.SR / 1000)
    seg = mono[a:b] if b - a >= 256 else mono[:max(256, len(mono))]
    f, p = welch(seg, F.SR, nperseg=min(F.D7_NPERSEG, len(seg)))
    return f, 10 * np.log10(p + F.EPS)


def harmonic_score(f: np.ndarray, pdb: np.ndarray, f0: float, n: int = NOTE_HARMS) -> float:
    """Mean level (dB) of the strongest bin near each of the first n harmonics of f0 (below MEL_HI)."""
    df = f[1] - f[0]
    lv = []
    for k in range(1, n + 1):
        fk = k * f0
        if fk > F.MEL_HI:
            break
        lo, hi = int(max(1, (fk - f0 / 6) / df)), int(min(len(pdb) - 1, (fk + f0 / 6) / df + 1))
        if hi > lo:
            lv.append(pdb[lo:hi].max())
    return float(np.mean(lv)) if lv else -300.0


def pick_note(mono: np.ndarray, keyoff_s: float, tracked: Optional[float]) -> float:
    """The played note (fractional MIDI): among candidates related to the tracked pitch (NOTE_OFFSETS) and to the
    strongest spectral peak (it divided by 1..6), the lowest whose first harmonics score within NOTE_NEAR_DB of the
    best candidate's. (A pitch tracker reads the lowest common period: a sub oscillator or a fifth below.)"""
    f, pdb = _sustain_psd(mono, keyoff_s)
    band = (f > 25) & (f < 5000)
    fp = float(f[band][np.argmax(pdb[band])])
    cands = [12 * np.log2(fp / k / 440) + 69 for k in range(1, 7)]
    if tracked is not None:
        cands += [tracked + o for o in NOTE_OFFSETS]
    cands = sorted({round(n, 2) for n in cands if 12 <= n <= 108})
    top = float(pdb[band].max())
    cands = [n for n in cands if harmonic_score(f, pdb, F.midi_hz(n), 1) >= top - NOTE_FUND_DB] or cands
    scores = [harmonic_score(f, pdb, F.midi_hz(n)) for n in cands]
    best = max(scores)
    near = [n for n, s2 in zip(cands, scores) if s2 >= best - NOTE_NEAR_DB]
    note = min(near)
    if tracked is not None:                             # keep the tracker's fine tuning when it is the same note
        for o in NOTE_OFFSETS:
            if abs(tracked + o - note) < 0.5:
                return tracked + o
    return note


def analyse(audio: np.ndarray, sr: int, engine=None, note: Optional[float] = None,
            gate_ms: Optional[float] = None) -> Dict:
    """The seed of a one-note sample (see the module text). engine: for bytes (engine.invert); note / gate_ms:
    known values override the readings (gate_ms from the onset)."""
    c = F.condition(audio, sr)
    floor = F.noise_floor(c)                            # (a recording's noise, so it is not read as sound)
    f = F.features(c, floor=dict(floor) if floor else None)   # (key-off not known yet: condition's estimate)
    db = smooth_db(f["d3"])
    ko = gate_ms / 1000 if gate_ms else find_keyoff(db, c["mono"])
    ko_found = ko is not None
    ko = ko if ko_found else c["keyoff_s"]
    c["keyoff_s"] = ko
    voiced = f["d5v"]
    rate = F.SR / F.D5_DECIM / F.D5_HOP
    a, b = int(0.05 * rate), max(int(ko * rate), int(0.05 * rate) + 1)
    v = voiced[a:b]
    vfrac = float(v.mean()) if len(v) else 0.0
    if note is None:
        note_f = pick_note(c["mono"], ko, float(np.median(f["d5"][a:b][v])) / 100 if v.any() else None)
    else:
        note_f = float(note)
    midi = int(round(note_f))
    f0 = F.midi_hz(note_f)
    f = F.features(c, f0=f0, floor=dict(floor) if floor else None)   # (again: the note's grid, the key-off)
    env = amp_envelope(smooth_db(f["d3"]), ko, c["mono"])
    flt = filter_fit(f["d7"], f0)
    d7 = f["d7"]
    lev = d7[:F.D7_NHARM]
    weak_fund = bool(lev[0] < np.max(lev[1:4]) - 12)
    bumps = [k for k in range(3, 20) if lev[k] > F.FLOOR_DB + 1 and
             lev[k] - max(lev[k - 1], lev[k + 1]) > SYNC_BUMP_DB]
    seed = {
        "note": midi, "cents": round((note_f - midi) * 100, 1), "f0_hz": round(f0, 2),
        "onset_s": round(c["onset_s"], 4), "gate_ms": round(ko * 1000), "keyoff_found": ko_found,
        "total_ms": round(c["dur_s"] * 1000), "level_db": round(-c["gain_db"], 2),
        "amp_env": {k: (round(v2, 4) if isinstance(v2, float) else v2) for k, v2 in env.items()},
        "filter": {"type_guess": "HPF/BPF" if weak_fund else "LPF", **flt, "weak_fundamental": weak_fund},
        "waves": wave_scores(d7, vfrac), "voiced": round(vfrac, 3),
        "lfo": wobble(f["d6"]),
        "echo_ms": None, "stereo": {"side_mid_db": round(float(f["d8"][0]), 2), "lr_corr": round(float(f["d8"][1]), 3)},
        "flags": {"sync": bool(bumps), "inharmonic": bool(vfrac > 0.5 and d7[F.D7_NHARM + 2] > INHARM_DB)},
        "one_sided": c["one_sided"], "tail_db": round(c["tail_db"], 1),
        "ends_early": bool(c["tail_db"] > ENDS_EARLY_DB),
        "noise_floor_db": round(floor["level"] - c["gain_db"], 1) if floor else None,   # (dBFS, as recorded)
    }
    ac = f["d8"][2:]
    j = int(np.argmax(ac))
    if ac[j] >= ECHO_MIN and 0 < j < len(ac) - 1:       # (a peak, not the edge of the window)
        seed["echo_ms"] = int(F.D8_LAG_LO_MS + j * F.D3_MS)
    if engine is not None:
        inv = engine.invert
        e = seed["amp_env"]
        b2 = {"AENV_A": inv("AENV_A", e["attack_s"]), "AENV_S": inv("AENV_S", e["sustain"]),
              "AENV_D": inv("AENV_D", e["decay_tau_s"]) if e["decay_tau_s"] else None,
              "AENV_R": inv("AENV_R", e["release_tau_s"]) if e["release_tau_s"] else None,
              "CUTOFF": inv("CUTOFF", flt["cutoff_hz"]),
              "LFO1_RATE": inv("LFO1_RATE", seed["lfo"]["rate_hz"]) if seed["lfo"] else None}
        seed["bytes"] = {k: v2 for k, v2 in b2.items() if v2 is not None}
    return seed


def main():
    ap = argparse.ArgumentParser(description="Analyse a one-note sample into seed.json (sound-match Stage 1).")
    ap.add_argument("wav", type=Path)
    ap.add_argument("--note", type=float, help="the MIDI note played, if known")
    ap.add_argument("--gate-ms", type=float, help="key held, ms from the onset, if known")
    ap.add_argument("--out", type=Path, help="write seed.json here (default: print)")
    a = ap.parse_args()
    import soundfile as sf
    x, sr = sf.read(str(a.wav), always_2d=True)
    import engine_mvance as em
    eng = em.MvanceEngine.__new__(em.MvanceEngine)      # (laws only: no host build needed to analyse)
    eng.root, eng._laws = em.ROOT, None
    seed = analyse(x, sr, eng, a.note, a.gate_ms)
    txt = json.dumps(seed, indent=2)
    if a.out:
        a.out.write_text(txt + "\n")
    else:
        print(txt)


if __name__ == "__main__":
    main()
