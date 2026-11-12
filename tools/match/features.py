# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match (WP1): conditioning (Stage 0) and the measures D1..D8 (proposal section 5).

  c = condition(audio, sr)                     any rate, mono or stereo -> 44.1 kHz, onset found, level normalised
  f = features(c, f0=hz)                       {"d1": .., "d2": .., ..}: named arrays, all phase-blind

Every measure works on the conditioned take: cut at the onset (so target and candidate line up), loudness
normalised (each side's RMS over its sounding part = 1; the level difference is kept in c["gain_db"] for AMP LEVEL,
fitted last). Units are chosen so the measures are comparable in loss.py: dB for levels, semitones for pitch and
brightness.

Engine-neutral: nothing here knows a synth. numpy + scipy only.
"""
from math import gcd
from typing import Dict, Optional

import numpy as np
from scipy.signal import resample_poly, welch

SR = 44100                              # Hz: every measure works at this rate
FLOOR_DB = -80.0                        # dB re the normalised RMS: the floor every level measure is clipped to
EPS = 1e-12                             # power floor before a log (never reached above FLOOR_DB)

# Stage 0
ONSET_ENV_MS = 1.0                      # ms: the max-abs envelope the onset is found on
ONSET_DB = -50.0                        # dB re the take's envelope peak: the onset is the first crossing
ACTIVE_DB = -60.0                       # dB re peak: frames above this count as sounding (for the normalising RMS)
KEYOFF_DB = -20.0                       # dB re peak: key-off estimate = the last 10 ms frame this loud (no gate known)
ONE_SIDED_DB = 40.0                     # dB: a channel this much quieter than the other is taken as empty (the live
                                        #  one is used on both sides)
TAIL_MS = 50.0                          # ms: the end of the take whose level (re the peak) says if it was cut short
# noise floor of a recording (renders have none: they end in digital silence)
NOISE_WIN_MS = 300.0                    # ms: the quiet stretch the floor is measured on
NOISE_BELOW_PEAK_DB = 30.0              # dB: the stretch must be at least this far below the peak...
NOISE_FLAT_DB = 3.0                     # dB: ...this flat (spread of its 10 ms levels)...
NOISE_ABOVE_DB = 3.0                    # dB: ...and this far above FLOOR_DB (not digital silence)
NOISE_MARGIN_DB = 6.0                   # dB: measures are clipped this far above the measured noise

# D1: log-mel spectrogram
D1_NFFT, D1_HOP = 2048, 512             # samples (46 ms window, 11.6 ms hop)
MEL_BANDS = 64                          # bands
MEL_LO, MEL_HI = 30.0, 16000.0          # Hz
# D2: fine-time log-mel of the attack
D2_NFFT, D2_HOP = 512, 128              # samples (11.6 ms window, 2.9 ms hop)
D2_MS = 300.0                           # ms from the onset
# D3: loudness envelope
D3_MS = 10.0                            # ms: RMS frame
# D4: centroid track (on the D1 STFT)
D4_GATE_DB = -50.0                      # dB: frames quieter than this get no say in the centroid loss
# D5: pitch track (YIN)
D5_DECIM = 4                            # YIN runs at SR / 4 = 11025 Hz (enough for D5_FMAX; 16x less work)
D5_W = 512                              # samples at SR / D5_DECIM: YIN integration window (46 ms)
D5_HOP = 128                            # samples at SR / D5_DECIM: hop (11.6 ms, as D1)
D5_FMIN, D5_FMAX = 30.0, 2000.0         # Hz: pitch search range
D5_THRESH = 0.15                        # YIN absolute threshold (cumulative-mean-normalised difference)
D5_VOICED = 0.35                        # a frame is voiced if its CMNDF minimum is below this...
D5_LIVE_DB = -50.0                      # ...and its level is within this of the take's loudest frame (a 16-bit
                                        #  tail's last bit, -70 dB after normalising, can look periodic)
# D6: wobble spectrum
D6_LO, D6_HI, D6_N = 0.2, 20.0, 48      # Hz, Hz, points: log-spaced grid the track spectra are sampled on
D6_START_MS = 150.0                     # ms after the onset: the wobble section starts (attack left out)
# D7: harmonic profile
D7_NPERSEG = 8192                       # samples: Welch segment (5.4 Hz resolution)
D7_NHARM = 40                           # harmonics
D7_START_MS, D7_MAX_MS = 100.0, 1500.0  # ms after the onset: the sustain window's start and longest length
D7_CLUSTER_N = 10                       # harmonics the cluster width is measured on
D7_CLUSTER_DB = 10.0                    # dB below a harmonic's peak: its cluster's edges
# D8: stereo and echo
D8_LAG_LO_MS, D8_LAG_HI_MS = 50.0, 1250.0   # ms: envelope autocorrelation lags


def midi_hz(note: float) -> float:
    """MIDI note -> Hz (A4 = 69 = 440 Hz)."""
    return 440.0 * 2.0 ** ((note - 69.0) / 12.0)


# --- Stage 0 ---------------------------------------------------------------------------------------------------

def condition(audio: np.ndarray, sr: int = SR, onset_s: Optional[float] = None,
              keyoff_s: Optional[float] = None) -> Dict:
    """Stage 0. audio: (frames,) or (frames, channels), any rate. onset_s / keyoff_s: the key-down and key-up times
    in the given audio when known (a render), else found here (onset) or estimated (key-off, WP2 refines it).
    Returns {"stereo": (n, 2), "mono": (n,), "onset_s", "keyoff_s" (from the onset), "gain_db" (the normalising
    gain: the take's level is -gain_db dB re RMS 1), "dur_s", "one_sided" (None, or "left only" / "right only": a
    channel was empty and the live one is used twice), "tail_db" (the level of the last TAIL_MS re the peak: near
    0 means the take was cut off while still sounding)}."""
    x = np.asarray(audio, dtype=np.float64)
    if x.ndim == 1:
        x = x[:, None]
    if x.shape[1] == 1:
        x = np.repeat(x, 2, axis=1)
    x = x[:, :2]
    one_sided = None
    lr = np.sqrt(np.mean(x ** 2, axis=0)) + 1e-12       # (a recording with one empty channel: the live one twice)
    if 20 * np.log10(lr[0] / lr[1]) > ONE_SIDED_DB:
        x, one_sided = np.repeat(x[:, :1], 2, axis=1), "left only"
    elif 20 * np.log10(lr[1] / lr[0]) > ONE_SIDED_DB:
        x, one_sided = np.repeat(x[:, 1:], 2, axis=1), "right only"
    if sr != SR:                                        # polyphase resample to 44.1 kHz
        g = gcd(int(sr), SR)
        x = resample_poly(x, SR // g, int(sr) // g, axis=0)
    mono = x.mean(axis=1)
    if onset_s is None:
        onset_s = find_onset(mono)
    i0 = int(round(onset_s * SR))
    x, mono = x[i0:], mono[i0:]
    db = rms_db(mono, D3_MS, ref=1.0)
    peak = float(db.max()) if len(db) else FLOOR_DB
    active = db > peak + ACTIVE_DB
    k = int(D3_MS * SR / 1000)
    sound = np.concatenate([mono[i * k:(i + 1) * k] for i in np.nonzero(active)[0]]) if active.any() else mono
    rms = float(np.sqrt(np.mean(sound ** 2))) if len(sound) else 0.0
    gain = 1.0 / rms if rms > 0 else 1.0
    if keyoff_s is None:
        loud = np.nonzero(db > peak + KEYOFF_DB)[0]
        keyoff_s = (loud[-1] + 1) * D3_MS / 1000.0 if len(loud) else len(mono) / SR
    else:
        keyoff_s = keyoff_s - onset_s
    nt = max(1, int(TAIL_MS / D3_MS))
    tail_db = float(db[-nt:].max() - peak) if len(db) >= nt else 0.0
    return {"stereo": x * gain, "mono": mono * gain, "onset_s": float(onset_s), "keyoff_s": float(keyoff_s),
            "gain_db": 20.0 * np.log10(gain), "dur_s": len(mono) / SR, "one_sided": one_sided,
            "tail_db": tail_db}


def find_onset(mono: np.ndarray) -> float:
    """Seconds to the first 1 ms block whose peak is within ONSET_DB of the take's loudest block (0 if silent)."""
    k = max(1, int(ONSET_ENV_MS * SR / 1000))
    n = len(mono) // k
    if n == 0:
        return 0.0
    env = np.abs(mono[:n * k]).reshape(n, k).max(axis=1)
    pk = env.max()
    if pk <= 0:
        return 0.0
    return float(np.argmax(env >= pk * 10 ** (ONSET_DB / 20)) * k / SR)


def noise_floor(c: Dict) -> Optional[Dict]:
    """The noise floor of a conditioned recording, per measure, or None when it has none (a render: its tail is
    digital silence) or no quiet stretch to measure it on. The quietest NOISE_WIN_MS stretch counts as noise if it is
    NOISE_BELOW_PEAK_DB under the peak, flat within NOISE_FLAT_DB and above digital silence. Returned: {"d3": dB,
    "d1": (64,) dB, "d2": (64,) dB, "d4": dB (the centroid gate), "level": dB} with NOISE_MARGIN_DB added. Pass it
    to features() for the target AND every candidate, so both are clipped at the same floor (silence on one side
    and noise on the other then compare as equal)."""
    mono = c["mono"]
    d3 = rms_db(mono, D3_MS)
    w = int(NOISE_WIN_MS / D3_MS)
    if len(d3) < 2 * w:
        return None
    cs = np.concatenate([[0.0], np.cumsum(d3)])
    means = (cs[w:] - cs[:-w]) / w
    i = int(np.argmin(means))
    seg_db = d3[i:i + w]
    lvl = float(means[i])
    if lvl > d3.max() - NOISE_BELOW_PEAK_DB or np.ptp(seg_db) > 2 * NOISE_FLAT_DB or lvl < FLOOR_DB + NOISE_ABOVE_DB:
        return None
    k = int(D3_MS * SR / 1000)
    seg = mono[i * k:(i + w) * k]
    m1 = to_db(power_spec(seg, D1_NFFT, D1_HOP) @ mel_bank(D1_NFFT).T)
    m2 = to_db(power_spec(seg, D2_NFFT, D2_HOP) @ mel_bank(D2_NFFT).T)
    p4 = power_spec(seg, D1_NFFT, D1_HOP)
    f = np.fft.rfftfreq(D1_NFFT, 1.0 / SR)
    band = (f >= MEL_LO) & (f <= MEL_HI)
    return {"level": lvl, "d3": lvl + NOISE_MARGIN_DB, "d1": np.median(m1, axis=0) + NOISE_MARGIN_DB,
            "d2": np.median(m2, axis=0) + NOISE_MARGIN_DB,
            "d4": float(np.median(to_db(p4[:, band].sum(axis=1)))) + NOISE_MARGIN_DB}


# --- helpers ---------------------------------------------------------------------------------------------------

def frames(x: np.ndarray, n: int, hop: int) -> np.ndarray:
    """(count, n) view of x in frames of n every hop samples (the end zero-padded to a whole frame)."""
    if len(x) < n:
        x = np.pad(x, (0, n - len(x)))
    count = 1 + (len(x) - n + hop - 1) // hop
    x = np.pad(x, (0, (count - 1) * hop + n - len(x)))
    return np.lib.stride_tricks.sliding_window_view(x, n)[::hop][:count]


def power_spec(x: np.ndarray, nfft: int, hop: int) -> np.ndarray:
    """|STFT|^2 with a Hann window, scaled so a full-scale sine's peak bin is about 1. Shape (frames, nfft/2+1)."""
    w = np.hanning(nfft)
    return np.abs(np.fft.rfft(frames(x, nfft, hop) * w, axis=1)) ** 2 * (2.0 / w.sum()) ** 2


_MEL_CACHE: Dict = {}


def mel_bank(nfft: int) -> np.ndarray:
    """(MEL_BANDS, nfft/2+1) triangular filters, HTK mel scale, MEL_LO..MEL_HI, each summing to 1 (a band with
    no bin under it, at small FFT sizes, takes its nearest bin)."""
    if nfft in _MEL_CACHE:
        return _MEL_CACHE[nfft]
    mel = lambda f: 2595.0 * np.log10(1.0 + f / 700.0)
    imel = lambda m: 700.0 * (10 ** (m / 2595.0) - 1.0)
    edges = imel(np.linspace(mel(MEL_LO), mel(MEL_HI), MEL_BANDS + 2))
    freqs = np.fft.rfftfreq(nfft, 1.0 / SR)
    fb = np.zeros((MEL_BANDS, len(freqs)))
    for b in range(MEL_BANDS):
        lo, mid, hi = edges[b], edges[b + 1], edges[b + 2]
        up = (freqs - lo) / (mid - lo)
        down = (hi - freqs) / (hi - mid)
        fb[b] = np.clip(np.minimum(up, down), 0.0, None)
        if fb[b].sum() == 0:
            fb[b, np.argmin(np.abs(freqs - mid))] = 1.0
        fb[b] /= fb[b].sum()
    _MEL_CACHE[nfft] = fb
    return fb


def to_db(p: np.ndarray) -> np.ndarray:
    """Power -> dB, clipped to FLOOR_DB."""
    return np.maximum(10.0 * np.log10(p + EPS), FLOOR_DB)


def rms_db(x: np.ndarray, ms: float, ref: float = 1.0) -> np.ndarray:
    """RMS in dB re ref per ms-long frame (no overlap), clipped to FLOOR_DB."""
    k = int(ms * SR / 1000)
    n = len(x) // k
    if n == 0:
        return np.zeros(0)
    return to_db(np.mean(x[:n * k].reshape(n, k) ** 2, axis=1) / ref ** 2)


# --- the measures ----------------------------------------------------------------------------------------------

def d1_logmel(mono: np.ndarray, p: Optional[np.ndarray] = None) -> np.ndarray:
    """D1: log-mel spectrogram, (frames, 64) dB, FFT 2048, hop 512. p: that STFT's power if already made."""
    p = power_spec(mono, D1_NFFT, D1_HOP) if p is None else p
    return to_db(p @ mel_bank(D1_NFFT).T)


def d2_attack(mono: np.ndarray) -> np.ndarray:
    """D2: fine-time log-mel of the first D2_MS ms, (frames, 64) dB, FFT 512, hop 128."""
    return to_db(power_spec(mono[:int(D2_MS * SR / 1000)], D2_NFFT, D2_HOP) @ mel_bank(D2_NFFT).T)


def d3_loudness(mono: np.ndarray) -> np.ndarray:
    """D3: RMS in 10 ms frames, dB re the normalised level."""
    return rms_db(mono, D3_MS)


def d4_centroid(mono: np.ndarray, p: Optional[np.ndarray] = None):
    """D4: spectral centroid per D1 frame in log2 Hz (MEL_LO..MEL_HI), and each frame's level in dB (the loss
    ignores frames below D4_GATE_DB). p: the D1 STFT's power if already made."""
    p = power_spec(mono, D1_NFFT, D1_HOP) if p is None else p
    f = np.fft.rfftfreq(D1_NFFT, 1.0 / SR)
    band = (f >= MEL_LO) & (f <= MEL_HI)
    pw, fb = p[:, band], f[band]
    tot = pw.sum(axis=1)
    cen = np.where(tot > 0, (pw * fb).sum(axis=1) / np.maximum(tot, EPS), MEL_LO)
    return np.log2(np.clip(cen, MEL_LO, MEL_HI)), to_db(tot)


def d5_pitch(mono: np.ndarray):
    """D5: YIN pitch every 11.6 ms (as D1's hop), in cents (MIDI note x 100; 0 where unvoiced), and a voiced flag.
    Runs on the take decimated by D5_DECIM (anti-alias filtered)."""
    sr = SR / D5_DECIM
    mono = resample_poly(mono, 1, D5_DECIM)
    tmin, tmax = int(sr / D5_FMAX), int(sr / D5_FMIN)
    n = D5_W + tmax                                     # each frame: the window and every lag
    fr_all = frames(mono, n, D5_HOP)
    e_all = np.mean(fr_all[:, :D5_W] ** 2, axis=1)
    live = (e_all > 10 ** (FLOOR_DB / 10)) & (e_all > e_all.max() * 10 ** (D5_LIVE_DB / 10))
                                                        # (only sounding frames are tracked; silence: unvoiced)
    cents = np.zeros(len(fr_all))
    voiced = np.zeros(len(fr_all), dtype=bool)
    if not live.any():
        return cents, voiced
    fr = fr_all[live]
    nfft = 1 << int(np.ceil(np.log2(n + D5_W)))
    a = np.fft.rfft(fr[:, :D5_W], nfft, axis=1)
    b = np.fft.rfft(fr, nfft, axis=1)
    r = np.fft.irfft(np.conj(a) * b, nfft, axis=1)[:, :tmax + 1]       # r[tau] = sum_j x[j] x[j + tau]
    sq = np.concatenate([np.zeros((len(fr), 1)), np.cumsum(fr ** 2, axis=1)], axis=1)
    e0 = sq[:, D5_W][:, None]                                           # sum of x[j]^2, j < W
    taus = np.arange(tmax + 1)
    et = sq[:, taus + D5_W] - sq[:, taus]                               # sum of x[j + tau]^2, j < W
    d = np.maximum(e0 + et - 2.0 * r, 0.0)
    d[:, 0] = 0.0
    cm = np.cumsum(d[:, 1:], axis=1) / np.arange(1, tmax + 1)
    cmndf = np.ones_like(d)
    cmndf[:, 1:] = d[:, 1:] / np.maximum(cm, EPS)
    seg = cmndf[:, tmin:tmax]
    below = seg < D5_THRESH
    first = np.where(below.any(axis=1), np.argmax(below, axis=1), np.argmin(seg, axis=1))
    rows = np.arange(len(seg))
    idx = first.copy()                                  # walk on to the bottom of that dip
    for _ in range(64):
        nxt = np.minimum(idx + 1, seg.shape[1] - 1)
        go = seg[rows, nxt] < seg[rows, idx]
        if not go.any():
            break
        idx = np.where(go, nxt, idx)
    t = idx + tmin
    l = cmndf[rows, np.maximum(t - 1, 1)]
    c = cmndf[rows, t]
    rr = cmndf[rows, np.minimum(t + 1, tmax)]
    den = l - 2 * c + rr
    ok = np.abs(den) > EPS
    shift = np.where(ok, 0.5 * (l - rr) / np.where(ok, den, 1.0), 0.0)
    tau = t + np.clip(shift, -1.0, 1.0)
    v = c < D5_VOICED
    voiced[live] = v
    cents[live] = np.where(v, 100.0 * (69.0 + 12.0 * np.log2(sr / tau / 440.0)), 0.0)
    return cents, voiced


def _track_spectrum(track: np.ndarray, rate: float) -> np.ndarray:
    """Amplitude spectrum of a (detrended, Hann-windowed) control track, on the D6 grid: a sine of amplitude A at
    a grid frequency reads about A."""
    if len(track) < 8:
        return np.zeros(D6_N)
    t = np.arange(len(track))
    y = track - np.polyval(np.polyfit(t, track, 1), t)
    w = np.hanning(len(y))
    nfft = max(1024, 1 << int(np.ceil(np.log2(len(y) * 4))))
    amp = np.abs(np.fft.rfft(y * w, nfft)) * 2.0 / w.sum()
    f = np.fft.rfftfreq(nfft, 1.0 / rate)
    return np.interp(np.geomspace(D6_LO, D6_HI, D6_N), f, amp)


def d6_wobble(d3: np.ndarray, d4: np.ndarray, d5: np.ndarray, voiced: np.ndarray, keyoff_s: float) -> np.ndarray:
    """D6: wobble spectra of the loudness (dB), centroid (semitones) and pitch (semitones) tracks between
    D6_START_MS and the key-off, (3, D6_N). Unvoiced pitch frames are filled with the voiced median."""
    end_s = max(keyoff_s, D6_START_MS / 1000 + 0.2)
    r3, r45 = 1000.0 / D3_MS, SR / D1_HOP
    s3 = d3[int(D6_START_MS / 1000 * r3):int(end_s * r3)]
    s4 = 12.0 * d4[int(D6_START_MS / 1000 * r45):int(end_s * r45)]
    r5 = SR / D5_DECIM / D5_HOP
    a, b = int(D6_START_MS / 1000 * r5), int(end_s * r5)
    p, v = d5[a:b] / 100.0, voiced[a:b]
    p = np.where(v, p, np.median(p[v]) if v.any() else 0.0) if v.mean() > 0.5 else np.zeros_like(p)
    return np.stack([_track_spectrum(s3, r3), _track_spectrum(s4, r45), _track_spectrum(p, r5)])


def d7_harmonics(mono: np.ndarray, f0: Optional[float], keyoff_s: float, clip: Optional[float] = None) -> np.ndarray:
    """D7: on a Welch spectrum of the sustain (D7_START_MS to the key-off, at most D7_MAX_MS): 40 harmonic levels
    in dB re the strongest (clipped to -80), then the first harmonic's level re the strongest (so "re the first" is
    recoverable), the odd - even level difference (dB, harmonics 2..21), the noise floor between harmonics (dB re the
    strongest) and the mean cluster width of the first 10 (cents). f0 None or unvoiced: 100 Hz grid. clip: harmonic
    levels (dB re the strongest) below this are raised to it (a recording's noise: see features())."""
    a = int(D7_START_MS * SR / 1000)
    b = int(min(max(keyoff_s * 1000, D7_START_MS + 200), D7_START_MS + D7_MAX_MS) * SR / 1000)
    seg = mono[a:b]
    if len(seg) < 256:
        seg = mono[:max(256, len(mono))]
    f, p = welch(seg, SR, nperseg=min(D7_NPERSEG, len(seg)))
    f0 = f0 if f0 and f0 > 0 else 100.0
    df = f[1] - f[0]
    pdb = 10 * np.log10(p + EPS)
    lev = np.full(D7_NHARM, -300.0)
    width = []
    for k in range(1, D7_NHARM + 1):
        fk = k * f0
        if fk > MEL_HI:
            break
        lo, hi = int(max(1, (fk - f0 / 4) / df)), int(min(len(p) - 1, (fk + f0 / 4) / df + 1))
        if hi <= lo:
            continue
        j = lo + int(np.argmax(p[lo:hi]))
        lev[k - 1] = pdb[j]
        if k <= D7_CLUSTER_N:
            lim = pdb[j] - D7_CLUSTER_DB
            l, r = j, j
            while l > lo and pdb[l - 1] > lim:
                l -= 1
            while r < hi - 1 and pdb[r + 1] > lim:
                r += 1
            width.append(1200 * np.log2(max(f[r] + df / 2, EPS) / max(f[l] - df / 2, df / 2)))
    top = lev.max()
    rel = np.maximum(lev - top, FLOOR_DB if clip is None else max(FLOOR_DB, clip))
    gaps = [pdb[int((k + 0.5) * f0 / df)] for k in range(1, D7_NHARM) if (k + 0.5) * f0 < MEL_HI]
    noise = max(float(np.median(gaps)) - top, FLOOR_DB) if gaps else FLOOR_DB
    odd = rel[2:21:2].mean()                            # harmonics 3, 5, .. 21
    even = rel[1:20:2].mean()                           # harmonics 2, 4, .. 20
    return np.concatenate([rel, [rel[0], odd - even, noise, float(np.mean(width)) if width else 0.0]])


def d8_stereo(stereo: np.ndarray, d3: np.ndarray) -> np.ndarray:
    """D8: [side / mid energy in dB (floor -80), L/R correlation], then the loudness envelope's normalised
    autocorrelation at lags D8_LAG_LO_MS..D8_LAG_HI_MS (10 ms steps)."""
    l, r = stereo[:, 0], stereo[:, 1]
    mid, side = (l + r) / 2, (l - r) / 2
    sm = to_db(np.sum(side ** 2) / max(np.sum(mid ** 2), EPS))
    lc, rc = l - l.mean(), r - r.mean()
    den_lr = float(np.sqrt(np.dot(lc, lc) * np.dot(rc, rc)))
    corr = float(np.dot(lc, rc) / den_lr) if den_lr > 0 else 1.0
    env = 10 ** (d3 / 20)
    env = env - env.mean()
    lags = np.arange(int(D8_LAG_LO_MS / D3_MS), int(D8_LAG_HI_MS / D3_MS) + 1)
    den = float(np.sum(env ** 2))
    nfft = 1 << int(np.ceil(np.log2(2 * len(env) + 1)))
    full = np.fft.irfft(np.abs(np.fft.rfft(env, nfft)) ** 2, nfft)       # sum_j env[j] env[j + k], every k
    ac = np.where(lags < len(env), full[np.minimum(lags, nfft - 1)] / den, 0.0) if den > 0 else np.zeros(len(lags))
    return np.concatenate([[float(sm), corr], ac])


ALL = ("d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8")   # every measure


def features(c: Dict, f0: Optional[float] = None, need=ALL, floor: Optional[Dict] = None) -> Dict[str, np.ndarray]:
    """The measures in need (default all) of a conditioned take, plus "f0" and "gain_db". f0 (Hz): the pitch D7
    reads harmonics at; None: the median voiced pitch of the sustain (matching passes the target's to the
    candidate, so both are read on one grid). Only what need asks for is computed (D6 brings D3, D4, D5 with it;
    an unknown f0 brings D5). floor: the TARGET's noise_floor() (None: none), applied to this take whichever it is:
    D1, D2, D3 are raised to it, D4 ignores frames below its gate ("d4gate"), and D7's harmonics are raised to the
    target's inter-harmonic noise (+3 dB; added to floor as "d7" the first time, from the target itself)."""
    need = set(need)
    mono, k = c["mono"], c["keyoff_s"]
    out: Dict[str, np.ndarray] = {"gain_db": np.array([c["gain_db"]])}
    if "d6" in need:
        need |= {"d3", "d4", "d5"}
    if "d7" in need and f0 is None:
        need.add("d5")
    p = power_spec(mono, D1_NFFT, D1_HOP) if need & {"d1", "d4"} else None   # (shared by D1 and D4)
    if "d1" in need:
        out["d1"] = d1_logmel(mono, p)
        if floor:
            out["d1"] = np.maximum(out["d1"], floor["d1"])
    if "d2" in need:
        out["d2"] = d2_attack(mono)
        if floor:
            out["d2"] = np.maximum(out["d2"], floor["d2"])
    if need & {"d3", "d8"}:
        out["d3"] = d3_loudness(mono)
        if floor:
            out["d3"] = np.maximum(out["d3"], floor["d3"])
    if "d4" in need:
        out["d4"], out["d4db"] = d4_centroid(mono, p)
        if floor:
            out["d4gate"] = np.array([floor["d4"]])
    if "d5" in need:
        out["d5"], out["d5v"] = d5_pitch(mono)
    if f0 is None and "d5" in out:
        rate = SR / D5_DECIM / D5_HOP                   # D5 frames a second
        a, b = int(D7_START_MS / 1000 * rate), int(max(k, 0.3) * rate)
        v = out["d5v"][a:b]
        f0 = float(440 * 2 ** ((np.median(out["d5"][a:b][v]) / 100 - 69) / 12)) if v.any() else None
    if "d6" in need:
        out["d6"] = d6_wobble(out["d3"], out["d4"], out["d5"], out["d5v"], k)
    if "d7" in need:
        if floor and "d7" not in floor:                 # (the target: its own noise between harmonics)
            floor["d7"] = float(d7_harmonics(mono, f0, k)[D7_NHARM + 2]) + 3.0
        out["d7"] = d7_harmonics(mono, f0, k, clip=floor.get("d7") if floor else None)
    if "d8" in need:
        out["d8"] = d8_stereo(c["stereo"], out["d3"])
    out["f0"] = np.array([f0 or 0.0])
    return out
