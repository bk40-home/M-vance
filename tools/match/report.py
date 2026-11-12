# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match (WP5): report.html, one static file (pictures and sound embedded, nothing to fetch): an A/B player
at matched level, spectrogram pair, loudness / brightness / pitch overlays, harmonic bars, the score per measure,
the parameter table with the panel's names and values, the warnings of section 2, and the run log.

  write_report(path, name, target_c, match_c, target_f, match_f, patch, params, loss, parts, seed, log, files)
"""
import base64
import html
import io
import wave
from pathlib import Path
from typing import Dict, List

import numpy as np

import features as F
import loss as L

GOOD_LOSS, FAIR_LOSS = 8.0, 16.0        # total loss: "close" below GOOD_LOSS, "fair" below FAIR_LOSS, else "poor"
PLAY_RMS_DB = -20.0                     # dBFS: both players are set to this RMS (A/B at matched level)
FIG_W = 9.0                             # inches: figure width


def value_text(p: Dict, v: int) -> str:
    """A byte as the panel shows it (enum names, signed values, OSC2 RANGE and FINE)."""
    d = p.get("disp", "num")
    if d == "enum" and p.get("names") and v < len(p["names"]):
        return p["names"][v]
    if d == "signed":
        return f"{v - p.get('centre', 0):+d}"
    if d == "cent":
        return f"{v - p.get('centre', 50):+d} c"
    if d == "range":
        return "-WIDE" if v == 0 else "+WIDE" if v >= 50 else f"{v - 25:+d} st"
    return str(v)


def _png(fig) -> str:
    """A matplotlib figure as an <img> with the PNG inline."""
    import matplotlib.pyplot as plt
    buf = io.BytesIO()
    fig.savefig(buf, format="png", dpi=100, bbox_inches="tight")
    plt.close(fig)
    return f'<img alt="" src="data:image/png;base64,{base64.b64encode(buf.getvalue()).decode()}">'


def _wav_b64(stereo: np.ndarray) -> str:
    """Stereo float audio at PLAY_RMS_DB RMS, as a 16-bit WAV data URI."""
    x = np.asarray(stereo, dtype=np.float64)
    rms = float(np.sqrt(np.mean(x ** 2))) or 1.0
    x = np.clip(x * 10 ** (PLAY_RMS_DB / 20) / rms, -1, 1)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(F.SR)
        w.writeframes((x * 32767).astype("<i2").tobytes())
    return "data:audio/wav;base64," + base64.b64encode(buf.getvalue()).decode()


def figures(tf: Dict, mf: Dict) -> List[str]:
    """The pictures: spectrograms, overlays, harmonics."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    out = []
    n = min(len(tf["d1"]), len(mf["d1"]))
    fig, ax = plt.subplots(1, 2, figsize=(FIG_W, 3.2), sharey=True)
    for a, d, t in ((ax[0], tf["d1"][:n], "sample"), (ax[1], mf["d1"][:n], "match")):
        a.imshow(d.T, origin="lower", aspect="auto", vmin=F.FLOOR_DB, vmax=max(tf["d1"].max(), mf["d1"].max()),
                 extent=[0, n * F.D1_HOP / F.SR, 0, F.MEL_BANDS], cmap="magma")
        a.set_title(f"{t}: log-mel spectrogram (D1)")
        a.set_xlabel("s from the onset")
    ax[0].set_ylabel("mel band (30 Hz .. 16 kHz)")
    out.append(_png(fig))
    fig, ax = plt.subplots(3, 1, figsize=(FIG_W, 6.5), sharex=True)
    t3 = np.arange(len(tf["d3"])) * F.D3_MS / 1000
    m3 = np.arange(len(mf["d3"])) * F.D3_MS / 1000
    ax[0].plot(t3, tf["d3"], label="sample", lw=1.2)
    ax[0].plot(m3, mf["d3"], label="match", lw=1.0)
    ax[0].set_ylabel("loudness dB (D3)")
    ax[0].legend(loc="upper right")
    t4 = np.arange(len(tf["d4"])) * F.D1_HOP / F.SR
    m4 = np.arange(len(mf["d4"])) * F.D1_HOP / F.SR
    ax[1].plot(t4, 2 ** np.where(tf["d4db"] > -50, tf["d4"], np.nan), lw=1.2)
    ax[1].plot(m4, 2 ** np.where(mf["d4db"] > -50, mf["d4"], np.nan), lw=1.0)
    ax[1].set_yscale("log")
    ax[1].set_ylabel("brightness Hz (D4)")
    r5 = F.SR / F.D5_DECIM / F.D5_HOP
    for f_, lw in ((tf, 1.2), (mf, 1.0)):
        ax[2].plot(np.arange(len(f_["d5"])) / r5, np.where(f_["d5v"], f_["d5"] / 100, np.nan), lw=lw)
    ax[2].set_ylabel("pitch, MIDI note (D5)")
    ax[2].set_xlabel("s from the onset")
    fig.tight_layout()
    out.append(_png(fig))
    fig, ax = plt.subplots(figsize=(FIG_W, 2.8))
    k = np.arange(1, F.D7_NHARM + 1)
    ax.bar(k - 0.2, tf["d7"][:F.D7_NHARM] - F.FLOOR_DB, 0.4, bottom=F.FLOOR_DB, label="sample")
    ax.bar(k + 0.2, mf["d7"][:F.D7_NHARM] - F.FLOOR_DB, 0.4, bottom=F.FLOOR_DB, label="match")
    ax.set_xlabel("harmonic")
    ax.set_ylabel("dB re the strongest (D7)")
    ax.legend()
    out.append(_png(fig))
    return out


def warnings(seed: Dict, loss: float) -> List[str]:
    """Section 2's limits, as they apply to this sample."""
    w = ["Matched from one note: key follow, velocity, bend, portamento, legato, LFO2 and anything on the mod wheel "
         "stay at their INIT values.",
         "Reverb is global on M-vance, not in the patch: reverb in the sample reads as a longer release or a delay.",
         "Many patches make the same sound: judge by ear and by the score, not by how the bytes look."]
    if loss >= FAIR_LOSS:
        w.insert(0, "The score is poor: this sound may be outside what M-vance can make (two oscillators, one filter, "
                    "three envelopes, LFOs, chorus, delay), or the sample is not one dry held note.")
    if seed.get("one_sided"):
        w.insert(0, f"The recording had sound on one channel only ({seed['one_sided']}): that channel was used on "
                    "both sides. (On the JE-8086 this comes from the performance's output settings.)")
    if seed.get("ends_early"):
        w.insert(0, f"The recording stops while the sound is still playing (its last 50 ms is only "
                    f"{-seed.get('tail_db', 0):.0f} dB below the peak): the release could not be heard, so it is a "
                    "guess. Record a few seconds past the key-off.")
    if seed.get("noise_floor_db") is not None:
        w.append(f"The recording has a noise floor at about {seed['noise_floor_db']:.0f} dBFS: both sides were "
                 "compared only above it.")
    if not seed.get("keyoff_found", True):
        w.append("No key-off was found in the sample (it may die away while held): the gate was guessed. Give "
                 "--gate-ms if you know it.")
    if seed.get("waves", {}).get("SUPER_SAW", 0) > 0.5:
        w.append("Super Saw: its oscillators start at random phases, so it never lines up sample for sample; the "
                 "measures are phase-blind.")
    if seed.get("echo_ms"):
        w.append(f"An echo at about {seed['echo_ms']} ms was heard: delay was fitted.")
    return w


def write_report(path: Path, name: str, tc: Dict, mc: Dict, tf: Dict, mf: Dict, patch: bytes, params: List[Dict],
                 loss: float, parts: Dict, seed: Dict, log: List[str], files: Dict[str, str]) -> Path:
    """Write report.html (see the module text). tc / mc: conditioned sample / match; tf / mf: their measures."""
    verdict = "close" if loss < GOOD_LOSS else "fair" if loss < FAIR_LOSS else "poor"
    rows = []
    for p in params:
        if p["group"] == "fixed" or p["size"] != 1:
            continue
        v = patch[p["offset"]]
        changed = v != p["default"]
        rows.append(f"<tr class='{'ch' if changed else ''}'><td>{html.escape(p['section'])}</td>"
                    f"<td>{html.escape(p['label'])}</td><td><b>{html.escape(value_text(p, v))}</b></td>"
                    f"<td>{v}</td><td>{html.escape(value_text(p, p['default']))}</td><td>{p['offset']}</td></tr>")
    score = "".join(f"<tr><td>{k.upper()}</td><td>{parts.get(k, 0):.2f}</td><td>{L.WEIGHTS[k]}</td>"
                    f"<td>{L.WEIGHTS[k] * parts.get(k, 0):.2f}</td></tr>" for k in L.WEIGHTS)
    pics = figures(tf, mf)
    fl = "".join(f"<li><code>{html.escape(k)}</code>: {html.escape(v)}</li>" for k, v in files.items())
    doc = f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><title>Sound match: {html.escape(name)}</title>
<style>
body {{ font: 15px/1.45 system-ui, sans-serif; max-width: 960px; margin: 2em auto; padding: 0 1em; color: #222 }}
h1 {{ font-size: 1.5em }} h2 {{ font-size: 1.15em; margin-top: 1.8em; border-bottom: 1px solid #ddd }}
.v {{ font-size: 1.25em; padding: .6em 1em; border-radius: 6px; display: inline-block }}
.close {{ background: #e3f4e1 }} .fair {{ background: #fff4d6 }} .poor {{ background: #fde2e1 }}
table {{ border-collapse: collapse }} td, th {{ padding: 3px 10px; border-bottom: 1px solid #eee; text-align: left }}
tr.ch td {{ background: #f3f7ff }} img {{ max-width: 100%; margin: .6em 0 }} audio {{ width: 100% }}
pre {{ background: #f7f7f7; padding: .8em; font-size: 12.5px; overflow-x: auto }}
</style></head><body>
<h1>Sound match: {html.escape(name)}</h1>
<p class="v {verdict}">Score <b>{loss:.2f}</b> ({verdict}) &middot; spectrogram distance D1 <b>{parts.get('d1', 0):.2f} dB</b>
&middot; note {seed.get('note')} &middot; key held {seed.get('gate_ms')} ms</p>
<h2>Listen (both at the same loudness)</h2>
<p>Sample</p><audio controls src="{_wav_b64(tc['stereo'])}"></audio>
<p>Match (M-vance)</p><audio controls src="{_wav_b64(mc['stereo'])}"></audio>
<h2>Files</h2><ul>{fl}</ul>
<h2>Warnings</h2><ul>{''.join(f'<li>{html.escape(x)}</li>' for x in warnings(seed, loss))}</ul>
<h2>Pictures</h2>{''.join(pics)}
<h2>Score per measure</h2>
<table><tr><th>measure</th><th>distance</th><th>weight</th><th>counts</th></tr>{score}</table>
<p>D1 / D2 / D3 / D7 in dB, D4 / D5 / D6 in semitones. Lower is closer; 0 is identical.</p>
<h2>The patch (searched bytes; shaded = changed from INIT)</h2>
<table><tr><th>section</th><th>knob</th><th>value</th><th>byte</th><th>INIT</th><th>offset</th></tr>{''.join(rows)}</table>
<h2>Run log</h2><pre>{html.escape(chr(10).join(log))}</pre>
</body></html>
"""
    path = Path(path)
    path.write_text(doc, encoding="utf-8")
    return path
