#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Sound match: a recording of ONE held note in, an M-vance (JP-8080) patch out.

  python3 tools/match/match.py sample.wav                    one sample
  python3 tools/match/match.py my_samples/                   every .wav in a folder
  python3 tools/match/match.py sample.wav --note 60          the note played, if you know it, as a MIDI number:
                                                             60 = middle C (Ableton calls it C3)

Options: --note N   --gate-ms N (how long the key was held)   --budget N (renders, default 8000)
         --seed N (same seed, same result)   --workers N (CPU cores; default all)   --out DIR

For each sample it writes a folder (default build/match/out/<sample name>/) with:
  report.html      OPEN THIS: listen to the sample and the match side by side, the score, pictures, the patch
  <name>.syx       the patch, as JP-8080 user patch 1 (one message, 02 00 00 00): open it in the bank editor
                   (docs/sim/bank/editor.html: Add source parts, drag it into a slot, play it on the FM-1, send it); the
                   firmware and the JT-8000's importer read it too
  match.wav        the match, played on the M-vance engine;  target.wav  the sample, from its first sound
  seed.json        what the analysis read from the sample;  log.txt  the search, stage by stage

The sample: one note, held, then let go, with its release tail; dry if possible (no reverb); WAV, any rate.
Run it in WSL (or Linux / macOS) from the M-vance folder. Needs: cc, python3 with numpy, scipy, cma, soundfile and
matplotlib (pip install numpy scipy cma soundfile matplotlib).
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import deps                                            # noqa: E402
deps.require()                                         # (stops here, with the fix, if a package is missing)
import engine_mvance as em                             # noqa: E402
import features as F                                   # noqa: E402
import fit                                             # noqa: E402
import jp_patch                                        # noqa: E402
import report                                          # noqa: E402


def write_wav16(path: Path, stereo: np.ndarray):
    """Float stereo (-1..1) as a 16-bit 44.1 kHz WAV."""
    import soundfile as sf
    sf.write(str(path), np.clip(stereo, -1, 1), F.SR, subtype="PCM_16")


def windows_path(p: Path) -> str:
    """The path as Windows shows it when this runs in WSL (wslpath), else as it is."""
    if shutil.which("wslpath"):
        try:
            return subprocess.run(["wslpath", "-w", str(p)], capture_output=True, text=True, check=True).stdout.strip()
        except subprocess.CalledProcessError:
            pass
    return str(p)


def _say(line: str):
    """The default log: the terminal."""
    print(line, flush=True)


def run_one(wav: Path, out: Path, a, eng, log=_say) -> dict:
    """Match one sample and write its folder. a: note, gate_ms, budget, seed, workers (argparse or the same
    attributes). log: called with each progress line (default: print)."""
    import soundfile as sf
    x, sr = sf.read(str(wav), always_2d=True)
    name = re.sub(r"[^A-Za-z0-9 _-]+", " ", wav.stem).strip()[:16] or "MATCH"
    out.mkdir(parents=True, exist_ok=True)
    log(f"== {wav.name}  ({len(x) / sr:.1f} s, {sr} Hz) -> {out}")
    t0 = time.perf_counter()
    res = fit.match(x, sr, note=a.note, gate_ms=a.gate_ms, budget=a.budget, seed_rng=a.seed, workers=a.workers,
                    engine=eng, progress=lambda line: log("   " + line))
    prob = res["problem"]
    try:
        patch = jp_patch.with_name(res["patch"], name)
        syx = re.sub(r"[^A-Za-z0-9_-]+", "_", wav.stem).strip("_")[:40] + ".syx"   # (named after the sample)
        jp_patch.write_syx(out / syx, jp_patch.user_patch_syx(patch, 0))            # (Kris 12 Nov, A1: one file,
                                                                                     #  the form every tool reads)
        y = prob.render(patch)
        on = em.NOTE_ON_MS / 1000
        mc = F.condition(y, onset_s=on, keyoff_s=on + prob.gate_ms / 1000)
        tc = prob.cond
        write_wav16(out / "match.wav", y[int(on * F.SR):])
        level = 10 ** (-tc["gain_db"] / 20)              # (the sample back at its own level)
        write_wav16(out / "target.wav", tc["stereo"] * level)
        (out / "seed.json").write_text(json.dumps(res["seed"], indent=2) + "\n")
        (out / "log.txt").write_text("\n".join(res["log"]) + "\n")
        tf = prob.windows["full"]["target"]
        mf = F.features(dict(mc, mono=mc["mono"][:len(tc["mono"])], stereo=mc["stereo"][:len(tc["mono"])]),
                        f0=prob.f0, floor=prob.floor)
        files = {"report.html": "this page",
                 syx: "the patch: open it in the bank editor (Add source parts), drag it into a slot, "
                      "play it on the FM-1 (▶) and send it",
                 "match.wav / target.wav": "the match and the sample as sound files",
                 "seed.json / log.txt": "what the analysis read; the search step by step"}
        report.write_report(out / "report.html", name, tc, mc, tf, mf, patch, eng.params(), res["loss"],
                            res["parts"], res["seed"], res["log"], files)
    finally:
        prob.close()
    verdict = "close" if res["loss"] < report.GOOD_LOSS else "fair" if res["loss"] < report.FAIR_LOSS else "poor"
    log(f"   score {res['loss']:.2f} ({verdict}), D1 {res['parts']['d1']:.2f} dB, {res['renders']} renders, "
        f"{time.perf_counter() - t0:.0f} s")
    log(f"   OPEN: {windows_path(out / 'report.html')}")
    return {"wav": wav.name, "loss": res["loss"], "d1": res["parts"]["d1"], "out": out, "verdict": verdict, "syx": syx,
            "renders": res["renders"], "seconds": time.perf_counter() - t0, "name": name}


def main():
    ap = argparse.ArgumentParser(description="Sound match: one held note (WAV) in, an M-vance patch out.",
                                 epilog="Then open report.html in the output folder.")
    ap.add_argument("input", type=Path, help="a .wav file, or a folder of them")
    ap.add_argument("--note", type=float, help="the MIDI note played, if known (60 = middle C = Ableton's C3)")
    ap.add_argument("--gate-ms", type=float, help="how long the key was held, in ms, if known")
    ap.add_argument("--budget", type=int, default=8000, help="renders the search may use (default 8000)")
    ap.add_argument("--seed", type=int, default=1, help="random seed: the same seed gives the same patch")
    ap.add_argument("--workers", type=int, default=None, help="CPU cores to use (default: all)")
    ap.add_argument("--out", type=Path, help="output folder (default: build/match/out/<sample name>)")
    a = ap.parse_args()
    wavs = sorted(a.input.glob("*.wav")) + sorted(a.input.glob("*.WAV")) if a.input.is_dir() else [a.input]
    if not wavs or not all(w.exists() for w in wavs):
        sys.exit(f"match: no .wav found at {a.input}")
    done = []
    with em.MvanceEngine() as eng:
        for w in wavs:
            out = (a.out / w.stem if a.out and len(wavs) > 1 else a.out) if a.out else \
                em.ROOT / "build/match/out" / re.sub(r"[^A-Za-z0-9_-]+", "_", w.stem)
            print()
            done.append(run_one(w, out.resolve(), a, eng))
    if len(done) > 1:
        print("\nsummary")
        for d in done:
            print(f"  {d['loss']:6.2f}  D1 {d['d1']:5.2f} dB  {d['wav']}")


if __name__ == "__main__":
    main()
