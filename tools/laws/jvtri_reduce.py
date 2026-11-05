#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""How the M-vance JvTRI table was cut from the JT's 231 KB to 60 KB (Kris, 1 Nov 2026: "reduce by as much as you
can"). PC only, needs numpy. Not part of the build: tools/gen_tables.py uses the result (JVTRI_KEEP, JVTRI_LENS).

  python3 tools/laws/jvtri_reduce.py $JT_SRC/data/akwf/JpMorph/JpMorph_VTri.h

Reference: the JT's own read of the full table (OscCore::renderTable: linear in the cycle, linear between the two
nearest SHAPE frames). Metric: the error of the HARMONIC MAGNITUDES (1..the level's highest harmonic) against
that reference, in dB, worst over every SHAPE byte and every mip level. The frames are captured from audio, so
neighbours differ a little in phase: a waveform-difference metric counts that and reads -26 dB where the ear
hears no change; the magnitudes are what is heard.
Yardstick: moving SHAPE by ONE knob step changes the reference spectrum by -32 dB (median), -29 dB at most.
Search: drop, one at a time, the frame whose loss costs least, while the worst error stays within one knob
step (-29 dB); then shorten the mip levels where their harmonics allow. Result (2026-11-01):
  frames  [0, 1, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 30, 32]  (16 of 33)
  lengths [512, 512, 256, 256, 128, 128, 128] (from 1024 1024 512 256 256 256 256: every level still holds more than
          3 samples per kept harmonic, as the JT's own level 3 does)
  60 KB, worst -29.3 dB (within one knob step); the shorter levels alone -45.5 dB.
  Next steps cost more than they save: 52 KB (-29.0, interpolation images 10 dB up), 42 KB (-26.5)."""
import json, re, sys
import numpy as np

F, SPF = 33, 3584
OFF = [0, 1024, 2048, 2560, 2816, 3072, 3328]
LEN = [1024, 1024, 512, 256, 256, 256, 256]
H = [183, 160, 80, 40, 20, 10, 5]
P = 1024
PH = np.arange(P) / P


def load(path):
    src = open(path).read()
    body = src[src.index("kData[118272]"):]
    blk = re.sub(r"//[^\n]*", "", body[body.index("{") + 1:body.index("};")])
    return np.array([int(x) for x in re.findall(r"-?\d+", blk)], dtype=float).reshape(F, SPF)


def read(tab):                                  # the JT's linear read, at P phases
    L = len(tab)
    pos = PH * L
    i = np.floor(pos).astype(int) % L
    f = pos - np.floor(pos)
    return tab[i] + (tab[(i + 1) % L] - tab[i]) * f


def resample(tab, L2):                          # band-limited (the level's harmonics all fit)
    if L2 == len(tab):
        return tab.copy()
    X = np.fft.rfft(tab)
    Y = np.zeros(L2 // 2 + 1, complex)
    k = min(len(X), len(Y))
    Y[:k] = X[:k]
    Y[-1] = Y[-1].real
    return np.round(np.fft.irfft(Y, L2) * L2 / len(tab))


def mags(x, h):
    return np.abs(np.fft.rfft(x))[1:h + 1]


def worst(data, keep, lens, levels=range(7), bytes_=range(128)):
    w = -999.0
    for l in levels:
        for b in bytes_:
            pos = b / 127 * (F - 1)
            a0 = min(int(pos), F - 2)
            r = read(data[a0, OFF[l]:OFF[l] + LEN[l]])
            r = r + (read(data[a0 + 1, OFF[l]:OFF[l] + LEN[l]]) - r) * (pos - a0)
            j = max(i for i in range(len(keep) - 1) if keep[i] <= pos + 1e-9) if pos < F - 1 else len(keep) - 2
            a, c = keep[j], keep[j + 1]
            ra = read(resample(data[a, OFF[l]:OFF[l] + LEN[l]], lens[l]))
            rc = read(resample(data[c, OFF[l]:OFF[l] + LEN[l]], lens[l]))
            y = ra + (rc - ra) * (pos - a) / (c - a)
            R, Y = mags(r, H[l]), mags(y, H[l])
            w = max(w, 10 * np.log10(np.sum((Y - R) ** 2) / np.sum(R ** 2) + 1e-30))
    return w


def main():
    data = load(sys.argv[1])
    lens0 = [512, 512, 512, 256, 256, 256, 256]
    keep = list(range(F))
    while True:                                 # greedy frame drop (levels 0 / 2 / 4 for speed, all checked after)
        best = None
        for i in range(1, len(keep) - 1):
            k2 = keep[:i] + keep[i + 1:]
            bs = [b for b in range(128) if keep[i - 1] <= b / 127 * (F - 1) <= keep[i + 1]]
            e = worst(data, k2, lens0, (0, 2, 4), bs)
            if best is None or e < best[0]:
                best = (e, i)
        if best is None or best[0] > -29.0:
            break
        keep.pop(best[1])
    for lens in (lens0, [512, 512, 256, 256, 128, 128, 128], [512, 512, 256, 128, 128, 64, 64]):
        print(json.dumps(keep), lens, "%.1f KB" % (len(keep) * sum(lens) * 2 / 1024), "worst %.1f dB" % worst(data, keep, lens))


if __name__ == "__main__":
    main()
