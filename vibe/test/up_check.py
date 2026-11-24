#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""JT-80x0 on VIBE FM, P2b's check (Kris 24 Nov 2026): the .wasm at 2x / 4x a rate against the engine at that rate.
  up_check.py <audio.wav (float, from wasmtest)> <ref.raw (ref.c)> <k>
1. Every k-th sample of the .wasm's output is the engine's own, exactly (the half-band's even phase is a copy), at one
   fixed delay. 2. Images: over a held stretch, the energy above 1.1 x the engine's Nyquist against all of it."""
import struct, sys
import numpy as np

wav, raw, k = sys.argv[1], sys.argv[2], int(sys.argv[3])
b = open(wav, "rb").read()
assert b[:4] == b"RIFF" and struct.unpack("<H", b[20:22])[0] == 3, 'wasmtest: a float WAV ("float": true)'
fs = struct.unpack("<I", b[24:28])[0]
host = np.frombuffer(b[44:], dtype="<f4").reshape(-1, 2)
eng = np.fromfile(raw, dtype="<f4").reshape(-1, 2)
n = min(len(eng) - 200, len(host) // k - 200)
found = None
for d in range(0, 4 * 128):                     # the delay, in host frames
    idx = d + k * np.arange(n)
    if idx[-1] < len(host) and np.array_equal(host[idx], eng[:n]):
        found = d
        break
if found is None:
    print(f"{fs} Hz: the engine's samples NOT found in the output"); sys.exit(1)
print(f"{fs} Hz (x{k}): one sample in {k} = the engine's, exactly ({n} frames, delay {found} frames = {found / fs * 1e3:.2f} ms)")
# images: frames 0.3 .. 1.3 s (the two first notes held), Hann window
seg = host[int(0.3 * fs):int(1.3 * fs), 0].astype(float)
spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
f = np.fft.rfftfreq(len(seg), 1 / fs)
ny = fs / k / 2
img = 10 * np.log10(spec[f > 1.1 * ny].sum() / spec.sum() + 1e-30)
print(f"{fs} Hz (x{k}): energy above {1.1 * ny / 1e3:.1f} kHz: {img:.1f} dB of all")
sys.exit(0 if img < -80.0 else 1)
