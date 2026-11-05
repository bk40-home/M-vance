#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The M-vance logo: assets/logo.svg / logo.png (the wordmark) and assets/icon.svg / icon.png (square).

  python3 tools/gen_logo.py        (needs Pillow)

The picture is the home screen's header: seven sawtooth waves, in phase in the middle and fanning out to both sides
(the Super Saw's seven detuned saws), the centre one brightest; over them a plate with "M-VANCE" in the FM-1's own
large font (firmware/gen/felucca_font.h, Terminus, SIL OFL), pixel for pixel. The firmware draws the same thing with
the same numbers (mvance.c, draw_home): change one, change both.
"""
import re
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
BG, PLATE = "#0d0b09", "#000000"
ACC = "#ff8000"                                        # C_ACC, the M-vance orange
STRAND = {0: "#ffa628", 1: "#e17808", 2: "#aa5200", 3: "#6e3200"}   # the AMBER palette, centre saw brightest
P0, DET, VOFF = 64, 30, 3                              # period (px of the 240 px screen), detune /1000 a strand, offset


def font_l():
    """FONT_L's glyphs as {char: [(x, y, alpha 1..15)]}."""
    src = (ROOT / "firmware/gen/felucca_font.h").read_text()

    def arr(name):
        m = re.search(r"%s\[\d+\] = \{(.*?)\};" % name, src, re.S)
        return [int(x, 0) for x in re.findall(r"0x[0-9A-Fa-f]+|\d+", m.group(1))]
    adv, bw, off, data = arr("FONT_L_ADV"), arr("FONT_L_BW"), arr("FONT_L_OFF"), arr("FONT_L_DATA")
    h, pad, first = 32, 4, 32
    glyphs = {}
    for ch in "M-VANCE":
        g = ord(ch) - first
        w, bpr, px = bw[g], (bw[g] + 1) // 2, []
        for y in range(h):
            for x in range(w):
                v = data[off[g] + y * bpr + x // 2]
                v = (v & 15) if x & 1 else v >> 4
                if v:
                    px.append((x - pad, y, v))
        glyphs[ch] = (adv[g], px)
    return glyphs


def saws(width, height, y0, amp):
    """The seven strands as polylines, dimmest first (in screen px of a 240-wide strip, scaled by the caller)."""
    out = []
    for d in (3, 2, 1, 0):
        for k in ((3 - d, 3 + d) if d else (3,)):
            pts, prev = [], None
            for x in range(width + 1):
                u = (x - width / 2) * (1000 + (k - 3) * DET)
                f = ((u * 256) / (1000 * P0) + 128) % 256
                y = y0 + abs(k - 3) * VOFF + amp - amp * f / 256
                if prev is not None and y > prev[1] + amp / 2:   # the drop: straight down
                    pts.append((x, prev[1])); pts.append((x, y))
                else:
                    pts.append((x, y))
                prev = (x, y)
            out.append((STRAND[d], pts))
    return out


def wordmark_rects(glyphs, text="M-VANCE"):
    """The text's pixels: (x, y) in font px, solid where the font's alpha is 6 or more (crisp, as on the screen)."""
    x0, rects = 0, []
    for ch in text:
        a, px = glyphs[ch]
        rects += [(x0 + x, y) for x, y, v in px if v >= 6]
        x0 += a
    return x0, rects


def build(kind):
    glyphs = font_l()
    tw, pix = wordmark_rects(glyphs)
    if kind == "logo":                                  # 240 x 56 screen px, drawn x4
        W, H, S, y0, amp = 240, 56, 4, 6, 40
        pw, ph = tw + 24, 40
    else:                                               # icon: 128 x 128, x4: the fan large, the M alone at 2x
        W, H, S, y0, amp = 128, 128, 4, 30, 60
        tw, pix = wordmark_rects(glyphs, "M")
        pix = [(2 * x + dx, 2 * y + dy) for x, y in pix for dx in (0, 1) for dy in (0, 1)]
        tw *= 2
        pw, ph = tw + 16, 76
    px0, py0 = (W - pw) / 2, (H - ph) / 2
    # SVG
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W * S} {H * S}" width="{W * S}" height="{H * S}">',
           f'  <!-- M-vance {kind}: tools/gen_logo.py (GPL-3.0-only; the letters: FM-1 FONT_L, Terminus, SIL OFL) -->',
           f'  <rect width="{W * S}" height="{H * S}" rx="{0 if kind == "logo" else 26 * S}" fill="{BG}"/>']
    for col, pts in saws(W, H, y0, amp):
        svg.append(f'  <polyline fill="none" stroke="{col}" stroke-width="{S * 0.9:.1f}" stroke-linejoin="miter" '
                   f'points="{" ".join(f"{x * S:.1f},{y * S:.1f}" for x, y in pts)}"/>')
    svg.append(f'  <rect x="{px0 * S}" y="{py0 * S}" width="{pw * S}" height="{ph * S}" fill="{PLATE}"/>')
    for yy in (py0, py0 + ph - 1):
        svg.append(f'  <rect x="{px0 * S}" y="{yy * S}" width="{pw * S}" height="{S}" fill="{ACC}"/>')
    th = 32 if kind == "logo" else 64
    tx, ty = (W - tw) / 2, py0 + (ph - th) / 2 + 1
    svg.append(f'  <g fill="{ACC}">' + "".join(f'<rect x="{(tx + x) * S}" y="{(ty + y) * S}" width="{S}" height="{S}"/>'
                                                for x, y in pix) + "</g>")
    svg.append("</svg>")
    (ROOT / f"assets/{kind}.svg").write_text("\n".join(svg) + "\n")
    # PNG, the same, drawn at 4x then the polylines smoothed by drawing at 4x again and scaling down
    Z = 4
    im = Image.new("RGB", (W * S * Z, H * S * Z), BG)
    dr = ImageDraw.Draw(im)
    if kind == "icon":                                  # the rounded square, the corners transparent
        im = Image.new("RGBA", im.size, (0, 0, 0, 0)); dr = ImageDraw.Draw(im)
        dr.rounded_rectangle([0, 0, im.width - 1, im.height - 1], radius=26 * S * Z, fill=BG)
    for col, pts in saws(W, H, y0, amp):
        dr.line([(x * S * Z, y * S * Z) for x, y in pts], fill=col, width=round(S * Z * 0.9), joint="curve")
    dr.rectangle([px0 * S * Z, py0 * S * Z, (px0 + pw) * S * Z - 1, (py0 + ph) * S * Z - 1], fill=PLATE)
    for yy in (py0, py0 + ph - 1):
        dr.rectangle([px0 * S * Z, yy * S * Z, (px0 + pw) * S * Z - 1, (yy + 1) * S * Z - 1], fill=ACC)
    for x, y in pix:
        dr.rectangle([(tx + x) * S * Z, (ty + y) * S * Z, (tx + x + 1) * S * Z - 1, (ty + y + 1) * S * Z - 1], fill=ACC)
    im.resize((W * S, H * S), Image.LANCZOS).save(ROOT / f"assets/{kind}.png")
    print(f"gen_logo: assets/{kind}.svg, assets/{kind}.png ({W * S} x {H * S})")


if __name__ == "__main__":
    build("logo")
    build("icon")
