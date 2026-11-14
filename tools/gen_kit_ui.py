#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The screen kit's tables: tools/gen_kit_ui.py -> firmware/gen/mv_kit_gen.h (Kris 14 Nov 2026,
fm1-port/mvance-kit-ui-plan-2026-11-14.md). Taken from M-Vsynth2's tools/gen_ui.py (the same fonts and knob
tables, byte for byte); M-vance adds two filter shapes (HP24, BP24: the JP filter's -24 dB slopes).

  python3 tools/gen_kit_ui.py        write the header (the fonts come from assets/fonts/dejavu/)
  python3 tools/gen_kit_ui.py --sample   also build/ui-font-sample.png: every face, to look at

What it writes (all const, in flash):
  FONT_LAB, FONT_VAL, FONT_TTL, FONT_BIG   DejaVu Sans Condensed Bold at 10, 13, 15 and 22 px, 4-bit coverage, in
                                           gfx.c's felucca_font_t form (proportional: an advance and a width a glyph)
  KNOB_ANG, KNOB_RING, KNOB_CAP            the knob's 48 x 48 cell (centre 24, 24): a pixel's angle on the 270-degree
                                           sweep (0..240, 255 off it), the coverage of the track ring (r 18.5, 3 px)
                                           and the modulation ring (r 22.5, 1.5 px), of the cap (r 12.5) and its rim
  UI_SIN                                   sin, 256 a turn, Q14
  UI_EXP                                   exp(-4.6 t), t 0..1 in 64 steps, Q14 (the envelopes' curves)
  UI_FLT                                   the filter's response by type (LP12, LP24, HP12, BP12, HP24, BP24) and resonance
                                           (8 steps), in dB (int8), every 2 px from 220 px below the cutoff to 220
                                           above it, 22 px an octave: a drawing of the response, not the engine's own

The fonts are DejaVu (Bitstream Vera licence, assets/fonts/dejavu/LICENSE). Coverage is PIL's anti-aliasing,
4 bits. Run it again only when a size or a table changes: the header is committed."""
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "firmware/gen/mv_kit_gen.h"
FONTDIR = ROOT / "assets/fonts/dejavu"

# name, file, px size, tracking (px added to each advance), what it is for
FONTS = [
    ("FONT_LAB", "DejaVuSansCondensed-Bold.ttf", 10, 1, "labels, chips, the footer (capitals)"),
    ("FONT_VAL", "DejaVuSansCondensed-Bold.ttf", 13, 0, "values under the knobs, list rows"),
    ("FONT_TTL", "DejaVuSansCondensed-Bold.ttf", 15, 0, "page titles"),
    ("FONT_BIG", "DejaVuSansCondensed-Bold.ttf", 22, 0, "the patch name, the readout of the knob in hand"),
]
FIRST, LAST = 32, 126
PAD = 1


def font_tables(name, file, size, track):
    from PIL import Image, ImageDraw, ImageFont
    f = ImageFont.truetype(str(FONTDIR / file), size)
    asc, desc = f.getmetrics()
    h = asc + desc
    adv, bw, off, data = [], [], [], []
    for c in range(FIRST, LAST + 1):
        ch = chr(c)
        a = int(round(f.getlength(ch))) + track
        w = a + 2 * PAD + 2
        im = Image.new("L", (w, h), 0)
        ImageDraw.Draw(im).text((PAD, 0), ch, font=f, fill=255)
        px = im.load()
        right = 0
        for x in range(w):
            if any(px[x, y] > 8 for y in range(h)):
                right = x + 1
        adv.append(a)
        bw.append(right)
        off.append(len(data))
        bpr = (right + 1) // 2
        for y in range(h):
            row = [0] * bpr
            for x in range(right):
                v = (px[x, y] * 15 + 127) // 255
                row[x // 2] |= v << 4 if x % 2 == 0 else v
            data.extend(row)
    assert len(data) < 65536, name
    return h, asc, adv, bw, off, data


def knob_tables():
    N, C = 48, 24.0
    ang, ring, cap = [], [], []
    SS = 4
    for y in range(N):
        for x in range(N):
            cx, cy = x + 0.5 - C, y + 0.5 - C
            d = math.degrees(math.atan2(cx, -cy))       # 0 at 12 o'clock, clockwise
            ang.append(int(round((d + 135.0) * 240.0 / 270.0)) if -135.0 <= d <= 135.0 else 255)
            cov = [0, 0, 0, 0]
            for sy in range(SS):
                for sx in range(SS):
                    px = x + (sx + 0.5) / SS - C
                    py = y + (sy + 0.5) / SS - C
                    r = math.hypot(px, py)
                    cov[0] += 17.0 <= r <= 20.0
                    cov[1] += 21.75 <= r <= 23.25
                    cov[2] += r <= 12.5
                    cov[3] += 11.75 <= r <= 12.75
            q = [min(15, (v * 15 + 8) // (SS * SS)) for v in cov]
            ring.append(q[0] << 4 | q[1])
            cap.append(q[2] << 4 | q[3])
    return ang, ring, cap


def filter_tables():
    """dB at dx px from the cutoff (22 px an octave), for 8 resonance steps"""
    out = []
    for t in range(6):
        for r in range(8):
            q = 0.707 * 2 ** (r * 0.6)                  # the SVF's Q: 0.7 .. 13 (a peak of about 22 dB)
            k = 3.7 * r / 7.0                           # the ladder's feedback: 0 .. near its edge
            row = []
            for i in range(221):
                w = 2.0 ** ((i * 2 - 220) / 22.0)
                s = complex(0, w)
                if t == 0:
                    hmag = abs(1 / (s * s + s / q + 1))
                elif t == 1:
                    g = 1 / (1 + s / 1.0)
                    hmag = abs(g ** 4 / (1 + k * g ** 4)) * (1 + k)
                elif t == 2:
                    hmag = abs(s * s / (s * s + s / q + 1))
                elif t == 3:
                    hmag = abs((s / q) / (s * s + s / q + 1))
                elif t == 4:                            # HP24: the LP24 ladder's mirror (s -> 1/s)
                    g = 1 / (1 + 1 / s)
                    hmag = abs(g ** 4 / (1 + k * g ** 4)) * (1 + k)
                else:                                   # BP24: two BP12 in series, its peak back to 0 dB
                    b = (s / q) / (s * s + s / q + 1)
                    hmag = abs(b * b)
                db = 20 * math.log10(max(hmag, 1e-9))
                row.append(max(-64, min(24, int(round(db)))))
            out.append(row)
    return out


def carr(ctype, name, vals, per=24):
    lines = []
    for i in range(0, len(vals), per):
        lines.append("    " + ", ".join(str(v) for v in vals[i:i + per]) + ",")
    return f"static const {ctype} {name}[{len(vals)}] = {{\n" + "\n".join(lines) + "\n};\n"


def main():
    parts = ["/* SPDX-License-Identifier: GPL-3.0-only */",
             "/* GENERATED by tools/gen_kit_ui.py: do not edit. The screen's fonts and tables (the tool's header says what",
             " * each is). Fonts: DejaVu Sans Condensed Bold (Bitstream Vera licence, assets/fonts/dejavu/LICENSE). */",
             "#pragma once", "#include <stdint.h>", ""]
    sizes = []
    for name, file, size, track, what in FONTS:
        h, asc, adv, bw, off, data = font_tables(name, file, size, track)
        parts.append(f"/* {name}: {what}. {size} px, line {h}, baseline {asc} */")
        parts.append(f"#define {name}_ASC {asc}")
        parts.append(carr("uint8_t", f"{name}_DATA", data, 32))
        parts.append(carr("uint16_t", f"{name}_OFF", off))
        parts.append(carr("uint8_t", f"{name}_ADV", adv, 32))
        parts.append(carr("uint8_t", f"{name}_BW", bw, 32))
        parts.append(f"static const felucca_font_t {name} = {{ {h}, {PAD}, {FIRST}, {LAST}, {name}_ADV, {name}_BW, "
                     f"{name}_OFF, {name}_DATA }};\n")
        sizes.append(len(data) + 4 * len(adv))
    ang, ring, cap = knob_tables()
    parts.append("/* the knob: 48 x 48 a cell, centre (24, 24) */")
    parts.append("#define KNOB_N 48")
    parts.append(carr("uint8_t", "KNOB_ANG", ang, 48))
    parts.append(carr("uint8_t", "KNOB_RING", ring, 48))
    parts.append(carr("uint8_t", "KNOB_CAP", cap, 48))
    parts.append(carr("int16_t", "UI_SIN", [int(round(16384 * math.sin(2 * math.pi * i / 256))) for i in range(256)], 16))
    parts.append(carr("int16_t", "UI_EXP", [int(round(16384 * math.exp(-4.6 * i / 63))) for i in range(64)], 16))
    flt = filter_tables()
    parts.append("/* the filter's drawing: [type 0..5: LP12 LP24 HP12 BP12 HP24 BP24][resonance 0..7][221]: dB at (i - 110) x 2 px from the cutoff */")
    parts.append("static const int8_t UI_FLT[6][8][221] = {")
    for t in range(6):
        parts.append("  {")
        for r in range(8):
            parts.append("    {" + ", ".join(str(v) for v in flt[t * 8 + r]) + "},")
        parts.append("  },")
    parts.append("};\n")
    OUT.write_text("\n".join(parts))
    total = sum(sizes) + 3 * 48 * 48 + 512 + 128 + 6 * 8 * 221
    print(f"{OUT.relative_to(ROOT)}: fonts {sum(sizes)} B, all {total} B of flash")
    if "--sample" in sys.argv:
        sample()


def sample():
    from PIL import Image, ImageDraw, ImageFont
    im = Image.new("RGB", (480, 200), (11, 15, 16))
    d = ImageDraw.Draw(im)
    y = 6
    for name, file, size, track, what in FONTS:
        f = ImageFont.truetype(str(FONTDIR / file), size)
        d.text((6, y), f"{name} RES 52 LP24 2.31 Hz DRY/WET KEYSHFT Glass Pad", font=f, fill=(232, 240, 240))
        y += size + 10
    p = ROOT / "build/ui-font-sample.png"
    p.parent.mkdir(exist_ok=True)
    im.save(p)
    print("sample ->", p)


if __name__ == "__main__":
    main()
