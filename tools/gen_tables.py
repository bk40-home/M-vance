#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate firmware/gen/mv_tables.h: the fixed-point law tables of the M-vance engine (the chip has no FPU).

  JT_SRC=/path/to/Jteensy8000/src python3 tools/gen_tables.py   refresh the JT laws, then the tables
  python3 tools/gen_tables.py                                    the tables from the cached laws

The JP laws come from the JT-8000's own float code (tools/laws/dump_laws.cpp calls JpEnvLaw, JpLfoRate and
JpModLaw), cached in docs/ref/jt_laws.json so a build needs neither the JT tree nor Python. The pitch tables
are plain maths. Every table is checked against its float source here; the script fails past the limits."""
import json, math, os, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LAWS = ROOT / "docs/ref/jt_laws.json"
OUT = ROOT / "firmware/gen/mv_tables.h"
FS = 44100            # the FM-1's audio rate (Dinghy: FS)
CTL = 16              # frames per control block: envelopes, LFOs, coefficients update here


def fail(msg):
    sys.exit(f"gen_tables: {msg}")


def refresh_laws():
    jt = os.environ.get("JT_SRC")
    if not jt:
        if not LAWS.exists():
            fail("no docs/ref/jt_laws.json and no JT_SRC to make it")
        return
    jt = Path(jt)
    with tempfile.TemporaryDirectory() as td:
        exe = Path(td) / "dump_laws"
        srcs = [str(jt / f"core/dsp/{n}.cpp") for n in ("JpEnvLaw", "JpLfoRate", "JpModLaw", "JpFilter")]
        subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", f"-I{jt}", "-o", str(exe),
                        str(ROOT / "tools/laws/dump_laws.cpp"), *srcs], check=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    laws = json.loads(out)
    # the Super Saw detune LUT (Szabo, v1 byte-for-byte) is a private table in SupersawOsc.cpp: read it from the
    # source text rather than retype 256 numbers
    import re
    src = (jt / "core/dsp/SupersawOsc.cpp").read_text(encoding="utf-8")
    m = re.search(r"kDetuneLut\[kLutSize\][^=]*=\s*\{(.*?)\};", src, re.S)
    lut = [float(x.rstrip("f")) for x in re.findall(r"[-0-9.]+f", m.group(1))] if m else []
    if len(lut) != 256:
        fail(f"SupersawOsc.cpp: detune LUT has {len(lut)} entries")
    laws["ssaw_detune_lut"] = lut
    # the JFBK maps (JE-8086 grids, 8 feedback rows x 9 harmonics columns) and constants, from JpFeedbackOsc.cpp
    src = (jt / "core/dsp/JpFeedbackOsc.cpp").read_text(encoding="utf-8")
    for name in ("kLog2BlocksC3", "kLog2BlocksC5", "kLockPull", "kChaos", "kWander"):
        m = re.search(name + r"\[kFbRows\]\[kHarmCols\][^=]*=\s*\{(.*?)\n\};", src, re.S)
        vals = [float(x.rstrip("f")) for x in re.findall(r"-?\d+\.\d+f", m.group(1))] if m else []
        if len(vals) != 72:
            fail(f"JpFeedbackOsc.cpp: {name} has {len(vals)} values")
        laws["jfbk_" + name] = vals
    # the JP tone law (FxChain.cpp, 2026-10-23 delivery): corners and the step -> gain constants
    src2 = (jt / "core/dsp/FxChain.cpp").read_text(encoding="utf-8")
    for name in ("kToneBassHz", "kToneTrebHz", "kBassBoostA", "kTrebK"):
        m = re.search(r"constexpr float " + name + r"\s*=\s*([-0-9.e]+)f", src2)
        if not m:
            fail(f"FxChain.cpp: {name} not found")
        laws["tone_" + name] = float(m.group(1))
    for name in ("kAnchorSplitHz", "kMixFbStart", "kMixFbSpan", "kHpTau", "kMaxInnerInc", "kWanderStep", "kWanderLeak"):
        m = re.search(r"constexpr float " + name + r"\s*=\s*([-0-9.e]+)f", src)
        if not m:
            fail(f"JpFeedbackOsc.cpp: {name} not found")
        laws["jfbk_" + name] = float(m.group(1))
    out = json.dumps(laws, indent=1)
    LAWS.write_text(out, encoding="utf-8")
    print(f"gen_tables: laws refreshed from {jt}")
    gen_jvtri(jt)


# JvTRI, reduced (tools/laws/jvtri_reduce.py, Kris 1 Nov 2026: 231 KB -> 60 KB, within one SHAPE knob step)
JVTRI_KEEP = [0, 1, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 30, 32]   # of the JT's 33 SHAPE frames
JVTRI_LENS = [512, 512, 256, 256, 128, 128, 128]                          # samples per cycle, per mip level
JVTRI_OUT = ROOT / "firmware/gen/mv_jvtri.h"


def gen_jvtri(jt):
    """The reduced JvTRI table from the JT's JpMorph_VTri.h (needs numpy; only with JT_SRC, else the committed
    firmware/gen/mv_jvtri.h stays as it is)."""
    import re
    import numpy as np
    src = (jt / "data/akwf/JpMorph/JpMorph_VTri.h").read_text(encoding="utf-8")
    hdr = {k: [int(x) for x in re.findall(r"\d+", re.search(k + r"\[kLevels\]\s*=\s*\{([^}]*)\}", src).group(1))]
           for k in ("kLevelOffset", "kLevelLen", "kLevelHarm")}
    frames = int(re.search(r"kFrames = (\d+)", src).group(1))
    spf = int(re.search(r"kSamplesPerFrame = (\d+)", src).group(1))
    body = src[src.index("kData["):]
    blk = re.sub(r"//[^\n]*", "", body[body.index("{") + 1:body.index("};")])
    data = np.array([int(x) for x in re.findall(r"-?\d+", blk)], dtype=float)
    if len(data) != frames * spf or frames != 33 or len(hdr["kLevelLen"]) != 7:
        fail(f"JpMorph_VTri.h: unexpected layout ({len(data)} samples, {frames} frames)")
    data = data.reshape(frames, spf)
    out = []
    for f in JVTRI_KEEP:
        for lv in range(7):
            tab = data[f, hdr["kLevelOffset"][lv]:hdr["kLevelOffset"][lv] + hdr["kLevelLen"][lv]]
            L2 = JVTRI_LENS[lv]
            if L2 != len(tab):                  # band-limited: the level's harmonics all fit below L2 / 2
                if hdr["kLevelHarm"][lv] >= L2 // 2:
                    fail(f"JvTRI level {lv}: {hdr['kLevelHarm'][lv]} harmonics do not fit {L2} samples")
                X = np.fft.rfft(tab)
                Y = np.zeros(L2 // 2 + 1, complex)
                Y[:L2 // 2 + 1] = X[:L2 // 2 + 1]
                Y[-1] = Y[-1].real
                tab = np.fft.irfft(Y, L2) * L2 / len(tab)
            out += [int(max(-32768, min(32767, round(v)))) for v in tab]
    offs = [sum(JVTRI_LENS[:i]) for i in range(7)]
    W = ["/* SPDX-License-Identifier: GPL-3.0-only */",
         "/* GENERATED by tools/gen_tables.py from the JT-8000's data/akwf/JpMorph/JpMorph_VTri.h (JE-8086 capture,",
         " * Kris Bishop): the JvTRI SHAPE morph, reduced 231 KB -> %d KB (tools/laws/jvtri_reduce.py says how). */" % (len(out) * 2 // 1024),
         "#pragma once", "#include <stdint.h>", "",
         f"#define MV_JVTRI_FRAMES {len(JVTRI_KEEP)}u",
         f"#define MV_JVTRI_SPF {sum(JVTRI_LENS)}u             /* samples per frame (all levels) */",
         "#define MV_JVTRI_LEVELS 7u",
         "/* the JT frame (0..32) each kept frame was, so SHAPE maps onto the same axis */",
         "static const uint8_t MV_JVTRI_KEEP[%d] = {%s};" % (len(JVTRI_KEEP), ", ".join(map(str, JVTRI_KEEP))),
         "static const uint16_t MV_JVTRI_OFF[7] = {%s};" % ", ".join(map(str, offs)),
         "static const uint8_t MV_JVTRI_LOG2[7] = {%s};      /* log2 of each level's length */" % ", ".join(str(L.bit_length() - 1) for L in JVTRI_LENS),
         "static const uint16_t MV_JVTRI_HARM[7] = {%s};    /* highest harmonic kept, per level */" % ", ".join(map(str, hdr["kLevelHarm"])),
         "static const int16_t MV_JVTRI[%d] = {" % len(out)]
    for i in range(0, len(out), 16):
        W.append("    " + ", ".join(map(str, out[i:i + 16])) + ",")
    W.append("};")
    JVTRI_OUT.write_text("\n".join(W) + "\n", encoding="utf-8")
    print(f"gen_tables: {JVTRI_OUT.relative_to(ROOT)} ({len(out) * 2 // 1024} KB)")


def q(x, bits):
    return int(round(x * (1 << bits)))


def main():
    refresh_laws()
    L = json.loads(LAWS.read_text(encoding="utf-8"))
    tau, l1, l2, cents, ampi = L["env_tau_s"], L["lfo1_hz"], L["lfo2_hz"], L["mod_cents"], L["amp_index"]
    target, off = L["env_attack_target"][0], L["env_off_level"][0]

    # pitch: the top octave (notes 132..143) as phase steps per frame; lower notes shift right; 256 fine steps
    inc_top = [q(440.0 * 2 ** ((n - 69) / 12) / FS, 32) for n in range(132, 144)]
    fine = [q(2 ** (k / (256 * 12)), 30) for k in range(256)]
    if max(inc_top) * max(fine) >> 30 >= 1 << 32:
        fail("pitch step overflows")
    # envelopes: per-block multiplier of an exponential stage, Q31 (decay/release toward the target; attack
    # toward target 1.25, the same tau)
    coef = [q(math.exp(-CTL / (t * FS)), 31) if t > 0 else 0 for t in tau]
    coef = [min(c, (1 << 31) - 1) for c in coef]
    for b in range(128):            # the fixed tau must stay within 0.1 % of the float one
        c = coef[b] / (1 << 31)
        tf = -CTL / (FS * math.log(c)) if 0 < c < 1 else 0
        if abs(tf - tau[b]) > 0.001 * tau[b] + 1e-6:
            fail(f"env byte {b}: tau {tf} vs {tau[b]}")
    # LFOs: phase step per control block, Q32
    lfo1 = [q(h * CTL / FS, 32) for h in l1]
    lfo2 = [q(h * CTL / FS, 32) for h in l2]
    # depths: LFO -> pitch peak in Q8 semitones (cents * 256 / 100); pitch env uses twice this
    mod_q8 = [int(round(c * 2.56)) for c in cents]
    # FILTER ENV depth: the same JpModLaw curve (shape confirmed on the pitch env, scene 17 block E), scaled so
    # +63 opens the cutoff by 124 codes (JE scene 17 block D: cutoff tracked through the decay cells, Kris 2 Nov, option 1).
    # In cutoff codes Q8, so mv_voice adds it straight to the code with no semitone -> code multiply.
    FENV_CODES_AT_63 = 124.0
    fenv_q8 = [int(round(c / cents[127] * FENV_CODES_AT_63 * 256)) for c in cents]
    if max(abs(v) for v in fenv_q8) > 32767:
        fail("MV_FENV_Q8 overflows int16")
    amp_q15 = [max(-32767, min(32767, q(a, 15))) for a in ampi]

    # OSC2 RANGE +WIDE / -WIDE: FINE/WIDE (0..100) then sets the pitch in half-semitone steps up to +-4 octaves.
    # The JE table spells the law out ("2 oct 3.5 s"): parse it rather than guess it.
    import re
    je = json.loads(re.sub(r",(\s*[\]}])", r"\1", (ROOT / "docs/ref/parameterDescriptions_je.json").read_text()))

    def wide(name):
        out = []
        for t in je["valuelists"][name]:
            m = re.fullmatch(r"(-?)(\d+) oct(?: ([\d.]+) s)?", t.strip())
            if not m:
                fail(f"{name}: cannot read {t!r}")
            st = 12 * int(m.group(2)) + (float(m.group(3)) if m.group(3) else 0.0)
            out.append(int(round((-st if m.group(1) else st) * 256)))
        if len(out) != 101:
            fail(f"{name}: {len(out)} entries")
        return out
    wide_pos, wide_neg = wide("osc2RangeWidePos"), wide("osc2RangeWideNeg")

    # JP filter (JpFilter): the cutoff as a knob CODE (modulation adds codes: steps-per-octave x octaves) ->
    # integrator gain g = tan(pi fc / fs), Q26 (g reaches ~32 at the 0.49 fs ceiling), every quarter code from the
    # 5 Hz floor (JT FilterSection) to code 128; quadratic between entries (checked as the pole frequency: < 0.1 cent below 16 kHz,
    # < 1 below 20 kHz, < 10 in the last stretch to 0.49 fs where tan runs to its pole; a high-Q ring is
    # ~20 Hz wide, so a linear table's 0.5 cent visibly detuned it against the float filter). The ceilings and the floor
    # are codes in Q8, so the voice clamps a modulated cutoff with two compares and no exp.
    spo = L["flt_steps_per_oct"][0]
    f64 = L["flt_cut_hz"][64]
    def code_of(hz):
        return 64 + spo * math.log2(hz / f64)
    if abs(code_of(L["flt_cut_hz"][0])) > 1e-3 or abs(code_of(L["flt_cut_hz"][127]) - 127) > 1e-3:
        fail("filter cutoff law is not one exponential")
    g_lo = -18.0                                    # code of the first entry (below the 5 Hz floor)
    # entries up to the first quarter code at or above the LP ceiling (0.49 fs; the next one would pass fs/2)
    g_n = int(math.ceil((code_of(0.49 * FS) - g_lo) * 4)) + 1
    g_tab = [q(math.tan(math.pi * f64 * 2 ** ((g_lo + i / 4 - 64) / spo) / FS), 26) for i in range(g_n)]
    if g_tab[-1] >= 1 << 32:
        fail("filter g overflows")
    worst_g = 0.0
    for i in range(g_n - 1):                        # quadratic through entries j..j+2, j = min(i, n - 3)
        j = min(i, g_n - 3)
        for t in (0.125, 0.375, 0.625, 0.875):
            c = g_lo + (i + t) / 4
            fc = f64 * 2 ** ((c - 64) / spo)
            if 5.0 <= fc <= 0.49 * FS:
                u = i + t - j
                g0, g1, g2 = g_tab[j], g_tab[j + 1], g_tab[j + 2]
                gi = (g0 + u * (g1 - g0) + u * (u - 1) / 2 * (g2 - 2 * g1 + g0)) / (1 << 26)
                ce = abs(1200 * math.log2(math.atan(gi) * FS / math.pi / fc))   # pole frequency, cents
                lim = 0.1 if fc < 16000 else 1.0 if fc < 20000 else 10.0
                if ce > lim:
                    fail(f"filter g interpolation {ce:.3f} cents at {fc:.0f} Hz")
                worst_g = max(worst_g, ce if fc < 16000 else 0)
    flt_floor = q(code_of(5.0), 8)
    flt_ceil_lp = q(code_of(0.49 * FS), 8)
    flt_ceil_hb = q(code_of(L["flt_ceil_hpbp_hz"][0]), 8)
    flt_r = [q(r, 31) for r in L["flt_r"]]
    flt_t24 = [q(t, 30) for t in L["flt_trim24"]]
    flt_t12 = [q(t, 30) for t in L["flt_trim12"]]
    flt_lc = [q(x, 16) for x in L["flt_lc_log2_per_code"]]
    exp2n = [min(q(2 ** (-i / 256), 31), 0xFFFFFFFF) for i in range(257)]
    # PORTAMENTO TIME (byte 70): the JT's glide law (SynthCore::glideRateFromNorm, not JP-measured): T = 2 ms x
    # 2500^(byte / 127), a fixed fraction r = 1 / (T x 375) of the remaining distance per JT block (48 kHz / 128),
    # clamped to 1 (instant). Per M-vance control block (MV_CTL at FS) the same time constant: 1 - (1 - r)^(375 /
    # blocks per second). Q31.
    porta = []
    for b in range(128):
        ms = 2.0 * (5000.0 / 2.0) ** (b / 127)
        r = min(1.0, 1.0 / (ms / 1000.0 * 375.0))
        rm = 1.0 - (1.0 - r) ** (375.0 / (FS / CTL)) if r < 1.0 else 1.0
        porta.append(min(q(rm, 31), (1 << 31) - 1))
    # NOISE wave's resonant low pass (Kris 1 Nov 2026, option 1a: PROVISIONAL until a capture refines it).
    #   cutoff (CTRL1): scene 17 fit fc = 14.4 kHz x 2^((c1 - 127) / 10.6), held under 0.45 fs; g = tan(pi fc / fs)
    #   resonance (CTRL2): R = 1/2Q log-linear through 3 anchors: 0 -> 0.4802 (the main filter's R0), 64 -> 0.05
    #   (Q 10: the measured +18..+23 dB peak), 127 -> 0.002 (Q 250: "self-oscillates" on the JE; kept linear and
    #   bounded here)
    NZ_ANCH = [(0, 0.480235), (64, 0.05), (127, 0.002)]
    nz_g, nz_r = [], []
    for b in range(128):
        fc = min(14400.0 * 2 ** ((b - 127) / 10.6), 0.45 * FS)
        nz_g.append(q(math.tan(math.pi * fc / FS), 26))
        (b0, r0), (b1, r1) = (NZ_ANCH[0], NZ_ANCH[1]) if b <= 64 else (NZ_ANCH[1], NZ_ANCH[2])
        nz_r.append(q(math.exp(math.log(r0) + (b - b0) / (b1 - b0) * (math.log(r1) - math.log(r0))), 31))
    jf = {k[5:]: v for k, v in L.items() if k.startswith("jfbk_")}
    # tone (FxChain): per step (byte - 64) the TPT one-pole gain G = g / (1 + g), g = tan(pi fc / fs), Q31, and
    # the shelf delta (plateau gain - 1), Q16. bass fc = 445 / max(Gp, 1), treble fc = 951 x max(Gp, 1)
    to = {k[5:]: v for k, v in L.items() if k.startswith("tone_")}
    def tpt(fc):
        g = math.tan(math.pi * fc / FS)
        return g / (1 + g)
    tb_a, tb_d, tt_a, tt_d = [], [], [], []
    for b in range(128):
        st = b - 64
        mm = st / 64
        gb = (1 + mm) if st < 0 else 1 / (1 - to["kBassBoostA"] * mm)
        gt = 1 + to["kTrebK"] * abs(st) / 64
        gt = 1 / gt if st < 0 else gt
        tb_a.append(q(tpt(to["kToneBassHz"] / max(gb, 1)), 31)); tb_d.append(q(gb - 1, 16))
        tt_a.append(q(tpt(to["kToneTrebHz"] * max(gt, 1)), 31)); tt_d.append(q(gt - 1, 16))
    ssaw_lut = [q(x, 24) for x in L["ssaw_detune_lut"]]

    W = []
    w = W.append
    w("/* SPDX-License-Identifier: GPL-3.0-only */")
    w("/* GENERATED by tools/gen_tables.py (laws from the JT-8000 float code via tools/laws/dump_laws.cpp, cached in")
    w(" * docs/ref/jt_laws.json). Do not edit. Fixed point because the FM-1's chip has no FPU. */")
    w("#pragma once")
    w("#include <stdint.h>")
    w("")
    w(f"#define MV_FS {FS}u                    /* audio frames per second */")
    w(f"#define MV_CTL {CTL}u                      /* frames per control block */")
    w(f"#define MV_ENV_TARGET_Q30 {q(target, 30)}  /* attack charges toward {target} x full (Q30), stops at full */")
    w(f"#define MV_ENV_FULL_Q30 {1 << 30}")
    w(f"#define MV_ENV_OFF_Q30 {q(off, 30)}        /* a release below {off} (-60 dB) ends */")
    w("")

    def table(ctype, name, vals, comment, per=8):
        w(f"/* {comment} */")
        w(f"static const {ctype} {name}[{len(vals)}] = {{")
        for i in range(0, len(vals), per):
            w("    " + ", ".join(f"{v}" + ("u" if ctype.startswith("uint") else "") for v in vals[i:i + per]) + ",")
        w("};")
    table("uint32_t", "MV_INC_TOP", inc_top, "phase step per frame (Q32) of notes 132..143; note n: >> (11 - n / 12)")
    table("uint32_t", "MV_FINE_Q30", fine, "2^(k / 3072): 1/256 semitone steps (Q30)")
    table("uint32_t", "MV_ENV_COEF", coef, "per-block multiplier exp(-CTL / (tau(byte) fs)), Q31: one tau table for "
          "attack, decay, release and LFO1 fade (JpEnvLaw)")
    table("uint32_t", "MV_LFO1_INC", lfo1, "LFO1 phase step per control block (Q32), JpLfoRate::hzFromNorm")
    table("uint32_t", "MV_LFO2_INC", lfo2, "LFO2 phase step per control block (Q32), JpLfoRate::lfo2HzFromNorm")
    table("int16_t", "MV_MOD_Q8", mod_q8, "LFO -> pitch peak for a signed depth byte (64 = 0), Q8 semitones "
          "(JpModLaw::depthCents); the pitch envelope reaches twice this")
    table("int16_t", "MV_FENV_Q8", fenv_q8, "FILTER ENV depth byte (64 = 0) -> cutoff codes at full envelope, Q8: "
          "the JpModLaw curve scaled to +63 = 124 codes (JE scene 17)")
    table("int16_t", "MV_AMP_IDX_Q15", amp_q15, "LFO -> amp (tremolo) index for a signed depth byte, Q15 "
          "(JpModLaw::ampIndex, clamped to +-1)")
    table("int16_t", "MV_WIDE_POS_Q8", wide_pos, "OSC2 RANGE +WIDE: FINE/WIDE byte -> Q8 semitones (JE osc2RangeWidePos)")
    table("int16_t", "MV_WIDE_NEG_Q8", wide_neg, "OSC2 RANGE -WIDE: FINE/WIDE byte -> Q8 semitones (JE osc2RangeWideNeg)")
    w("")
    w(f"#define MV_FLT_CODE_PER_OCT_Q16 {q(spo, 16)}  /* cutoff knob codes per octave ({spo:.5f}) */")
    w(f"#define MV_FLT_G_CODE0_Q8 {q(g_lo, 8)}         /* code of MV_FLT_G_Q26[0]; entries every quarter code (Q8 / 64) */")
    w(f"#define MV_FLT_FLOOR_Q8 {flt_floor}           /* 5 Hz */")
    w(f"#define MV_FLT_CEIL_LP_Q8 {flt_ceil_lp}        /* LP: 0.49 fs */")
    w(f"#define MV_FLT_G_CEIL_LP_Q26 {q(math.tan(math.pi * 0.49), 26)}u  /* g exactly at the LP ceiling */")
    w(f"#define MV_FLT_G_CEIL_HB_Q26 {q(math.tan(math.pi * L['flt_ceil_hpbp_hz'][0] / FS), 26)}u  /* g exactly at the HP/BP ceiling */")
    w(f"#define MV_FLT_CEIL_HB_Q8 {flt_ceil_hb}        /* HP / BP: {L['flt_ceil_hpbp_hz'][0]} Hz (where the JE's HPF saturates) */")
    table("uint32_t", "MV_FLT_G_Q26", g_tab, "JP filter integrator gain tan(pi fc / fs), Q26, every quarter cutoff code")
    table("uint32_t", "MV_FLT_R_Q31", flt_r, "JP filter section damping R = 1/2Q per resonance byte, Q31 (JpFilter::damping)")
    table("uint32_t", "MV_FLT_T24_Q30", flt_t24, "JP filter -24 output trim R/R0 per resonance byte, Q30")
    table("uint32_t", "MV_FLT_T12_Q30", flt_t12, "JP filter -12 output trim (R/R0)^0.1 per resonance byte, Q30")
    table("int32_t", "MV_FLT_LC_Q16", flt_lc, "JP filter R5a level compensation, -24 modes: log2(gain) per cutoff "
          "code above 64, per resonance byte (Q16; 0 up to 116)")
    table("uint32_t", "MV_SSAW_DETUNE_Q24", ssaw_lut, "Super Saw detune depth per detune position (Szabo, from "
          "the JT's SupersawOsc.cpp kDetuneLut), Q24, 256 entries over 0..1")
    w("")
    w("/* JFBK (JpFeedbackOsc): the JE-8086 grids, 8 feedback rows (bytes 0 32 48 64 80 96 112 127) x 9 harmonics")
    w(" * columns (0 16 32 48 64 80 96 112 127), row-major, Q16; and its constants at this rate */")
    for name, key in (("MV_JFBK_L2B_C3_Q16", "kLog2BlocksC3"), ("MV_JFBK_L2B_C5_Q16", "kLog2BlocksC5"),
                      ("MV_JFBK_PULL_Q16", "kLockPull"), ("MV_JFBK_CHAOS_Q16", "kChaos"),
                      ("MV_JFBK_WANDER_Q16", "kWander")):
        table("int32_t", name, [q(x, 16) for x in jf[key]], f"JpFeedbackOsc {key}", per=9)
    w(f"#define MV_JFBK_SPLIT_INC {q(jf['kAnchorSplitHz'] / FS, 32)}u   /* {jf['kAnchorSplitHz']} Hz: the C5 map at or above */")
    w(f"#define MV_JFBK_MIX_START_Q16 {q(jf['kMixFbStart'], 16)}")
    w(f"#define MV_JFBK_MIX_SPAN_Q16 {q(jf['kMixFbSpan'], 16)}")
    w(f"#define MV_JFBK_HPA_Q31 {q(jf['kHpTau'] / (jf['kHpTau'] + 1 / FS), 31)}u   /* tau {jf['kHpTau']} s at fs */")
    w(f"#define MV_JFBK_MAX_INNER {q(jf['kMaxInnerInc'], 32)}u")
    w(f"#define MV_JFBK_WANDER_STEP_Q16 {q(jf['kWanderStep'], 16)}")
    w(f"#define MV_JFBK_WANDER_LEAK_Q16 {q(jf['kWanderLeak'], 16)}")
    table("uint32_t", "MV_TONE_BASS_A_Q31", tb_a, "tone BASS (byte 59): TPT one-pole gain of the shelf's low pass, Q31")
    table("int32_t", "MV_TONE_BASS_D_Q16", tb_d, "tone BASS: plateau gain - 1, Q16 (-1 .. +21.2)")
    table("uint32_t", "MV_TONE_TREB_A_Q31", tt_a, "tone TREBLE (byte 60): TPT one-pole gain, Q31 (HP = x - LP)")
    table("int32_t", "MV_TONE_TREB_D_Q16", tt_d, "tone TREBLE: plateau gain - 1, Q16")
    table("uint32_t", "MV_NOISE_G_Q26", nz_g, "NOISE filter (provisional): integrator gain by CTRL1, Q26 "
          "(scene 17 cutoff law)")
    table("uint32_t", "MV_NOISE_R_Q31", nz_r, "NOISE filter (provisional): damping R = 1/2Q by CTRL2, Q31")
    table("uint32_t", "MV_PORTA_Q31", porta, "PORTAMENTO TIME: fraction of the remaining pitch distance per control "
          "block, Q31 (the JT's glide law, 2 ms .. 5 s; 2^31 - 1 = instant)")
    table("uint32_t", "MV_EXP2N_Q31", exp2n, "2^(-i/256), i = 0..256, Q31 (mv_exp2n_q31)")
    OUT.write_text("\n".join(W) + "\n", encoding="utf-8")
    print(f"gen_tables: {OUT.relative_to(ROOT)} (fs {FS}, block {CTL})")


if __name__ == "__main__":
    main()
