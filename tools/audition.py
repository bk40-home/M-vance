#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Hear the factory bank before flashing it (Kris 3 Nov, B3): each program played on the host build (tests/host.c,
the firmware's own engine) to build/audition/NN_<name>.wav, and a line each of level and brightness.

  python3 tools/audition.py            all 64 (after tools/make_bank.py: the bank in firmware/gen/mv_factory.h)
  python3 tools/audition.py 4 7 16     some

Linux / macOS / WSL (it builds the host with cc). A program is selected with MIDI program change and played over
MIDI: a phrase C4 E4 G4 C5, then a C major chord held 2 s; basses (BS) two octaves lower, FX one note held 4 s,
a SPLIT program its lower and its upper side together.
  peak / rms   dBFS of the whole take (the bank's programs should sit within a few dB of each other)
  bright       spectral centroid in Hz (a rough "how bright": a pad ~1-2 kHz, a lead higher, a sub bass low)
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = Path(os.environ.get("AUDITION_OUT", ROOT / "build/audition"))   # (AUDITION_OUT: elsewhere)


def names():
    """[(name, mode, split)] for the 64, from firmware/gen/mv_factory.h."""
    txt = (ROOT / "firmware/gen/mv_factory.h").read_text()
    up = txt.split("MV_FACTORY[MV_NPROG]")[1].split("};")[0]
    rows = re.findall(r"\{([^{}]*)\}", up)
    perf = re.findall(r"\{([^{}]*)\}", txt.split("MV_FACTORY_PERF[MV_NPROG]")[1].split("};")[0])
    out = []
    for r, p in zip(rows, perf):
        r = re.sub(r"/\*.*?\*/", "", r, flags=re.S)     # (the comment with the slot and the name)
        b = [int(x) for x in r.split(",") if x.strip()]
        q = [int(x) for x in p.split(",") if x.strip()]
        out.append(("".join(chr(c) for c in b[:16]).strip(), q[0], q[1]))
    return out


def script(n, name, mode, split):
    ev = [f"10 midi 0xC0 {n} 0"]                        # program change (channel 1; OMNI takes it)
    t, base = 200, 36 if name.startswith("BS") else 60
    if mode == 2:                                       # SPLIT: a note each side of the split
        ev += [f"{t} midi 0x90 {split - 17} 100", f"{t} midi 0x90 {split + 12} 100",
               f"{t + 2500} midi 0x80 {split - 17} 0", f"{t + 2500} midi 0x80 {split + 12} 0"]
        t += 4500
    elif name.startswith("FX"):
        ev += [f"{t} midi 0x90 {base} 100", f"{t + 4000} midi 0x80 {base} 0"]
        t += 6000
    else:
        for k in (0, 4, 7, 12):                         # the phrase
            ev += [f"{t} midi 0x90 {base + k} 100", f"{t + 350} midi 0x80 {base + k} 0"]
            t += 400
        for k in (0, 4, 7):                             # the chord (a mono patch plays its last note)
            ev.append(f"{t} midi 0x90 {base + k} 100")
        for k in (0, 4, 7):
            ev.append(f"{t + 2000} midi 0x80 {base + k} 0")
        t += 4000
    return "\n".join(ev + [f"{t} end"]) + "\n"


def measure(path):
    with wave.open(str(path)) as w:
        ch, n, raw = w.getnchannels(), w.getnframes(), w.readframes(w.getnframes())
    s = struct.unpack(f"<{n * ch}h", raw)
    m = [(s[i] + s[i + 1]) / 2 for i in range(0, len(s), ch)] if ch == 2 else list(s)
    pk = max(abs(x) for x in m) or 1
    rms = (sum(x * x for x in m) / len(m)) ** 0.5 or 1
    import math
    # brightness: zero-crossing rate as a cheap centroid stand-in (no numpy needed)
    zc = sum(1 for i in range(1, len(m)) if (m[i - 1] < 0) != (m[i] < 0))
    hz = zc / 2 / (len(m) / 44100)
    return 20 * math.log10(pk / 32768), 20 * math.log10(rms / 32768), hz


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / "mvance-audition"
    subprocess.run(["cc", "-O2", "-Wno-unused-function", "-DDINGHY_HOST", '-DAPP_VER="audition"', "-Ifirmware/hal",
                    "-Ifirmware/src", "-Ifirmware/gen", "-o", str(exe), "firmware/src/mvance.c",
                    "firmware/src/mv_dsp.c"], cwd=ROOT, check=True)
    progs = names()
    want = [int(a) for a in sys.argv[1:]] or range(1, 65)
    print(" #  name              peak   rms   bright")
    for s in want:
        name, mode, split = progs[s - 1]
        stem = f"{s:02d}_" + re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_")
        with tempfile.TemporaryDirectory() as tmp:     # the script, and the take (the host also writes its screens
            sc = Path(tmp) / "play.txt"                #  there: only the .wav is kept)
            sc.write_text(script(s - 1, name, mode, split))
            subprocess.run([str(exe), str(Path(tmp) / "take"), str(sc)], cwd=ROOT, check=True, capture_output=True)
            shutil.copyfile(Path(tmp) / "take.wav", OUT / (stem + ".wav"))
        pk, rms, hz = measure(OUT / (stem + ".wav"))
        print(f"{s:2d}  {name:16s} {pk:6.1f} {rms:6.1f} {hz:7.0f}")


if __name__ == "__main__":
    main()
