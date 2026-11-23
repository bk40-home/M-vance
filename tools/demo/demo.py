#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kris (bk40-home)
"""Demo videos of an FM-1 firmware (M-Vsynth2, JT-80x0), rendered from the firmware itself.

The firmware runs unchanged on this computer through tests/host.c's film mode: a host script presses buttons, turns
encoders and plays MIDI at exact times; out come the screen (a PPM every 1/30 s) and the sound (a WAV, as the FM-1
sends it over USB). This tool:

  1. expands a .demo file (below) into a host script,
  2. builds the host binary the way tests/run_host.sh does (its cc line is read from it) and runs it,
  3. lays each screen frame out on a 1920x1080 page (the screen x3, captions, the notes: those the script sends and
     the FM-1's lit keys, so the arp's and the sequencer's show; in an fm1-kit theme)
     and pipes the frames to ffmpeg, which adds a scope and a spectrum of the audio and writes an MP4.

    python3 tools/demo/demo.py tools/demo/tour.demo                 -> build/demo/tour.mp4
    python3 tools/demo/demo.py tools/demo/tour.demo -o my.mp4 --keep  (--keep: leave the frames, WAV and host script)
    python3 tools/demo/demo.py tools/demo/tour.demo --script-only     (print the host script; no build, no video)

The same file serves every firmware that has tests/host.c's film mode and tests/run_host.sh (M-Vsynth2, JT-80x0, formerly M-vance).
Needs: cc, ffmpeg, Python 3 with Pillow and numpy (as tests/run_host.sh). Runs in WSL as the other tools do.

.demo files: one event a line, `TIME COMMAND ARGS`. TIME is seconds (`12.5`) or relative to the line before (`+0.5`).
`#` at the start of a line or after a space starts a comment (D#4 is a note). Notes are names (C3 = MIDI 48, F#2,
Bb4) or MIDI numbers. Settings (no TIME):

    name TEXT                   the label over the caption (default: the binary's name from tests/run_host.sh)
    theme NAME                  an fm1-kit theme (third_party/fm1-kit/web/theme.css): sea, ember, amber, ... (sea)
    footer TEXT                 the line along the bottom

Events:

    title TEXT [| SUB]          a full title card (until the next title, caption or clear)
    caption TEXT [| SUB]        the caption beside the screen
    clear                       no title or caption
    note NOTES DUR [VEL]        NOTES (comma separated: a chord) over MIDI for DUR seconds
    seq STEP NOTES [REPEAT] [GATE] [VEL]
                                a sequence, one step every STEP seconds; `-` a rest; GATE a fraction of the step (0.5)
    pc N                        program change to N, counted from 1 as the screen counts (a Program Change of N - 1)
    cc CC VALUE                 a MIDI CC
    ccramp CC FROM TO DUR       a CC moved smoothly from FROM to TO over DUR seconds
    turn KNOB N [DUR]           an encoder turned N detents (+ clockwise), one detent at a time over DUR seconds
                                (default 2 ms apart: a quick turn). KNOB: KNOB1..KNOB4, PRESETS, SELECT, ALGORITHM.
                                The firmware speeds up a quick turn as it does a hand's: JT-80x0 steps x4 while
                                detents come < 40 ms apart (mv_ui.c); M-Vsynth2 doesn't at 2 ms. For one step a
                                detent on JT-80x0, give a DUR that spaces them 40 ms or more.
                                DUR 0: all N detents in one pass (JT-80x0: exactly N, as a jump; M-Vsynth2 steps x2
                                on a wide range)
    press BUTTON                a panel button tapped. BUTTON: SEL ENV LFO EDIT GLO HOME SAVE ARP FX SEQ PLAY REC OCT- OCT+
    hold BUTTON DUR             a panel button held for DUR seconds
    host LINE                   a raw tests/host.c script line at TIME (its own time field left out)
    end                         the video ends here
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import wave
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
FPS = 30
W, H = 1920, 1080
TURN_GAP = 0.002        # s between a turn's detents when no DUR is given (a quick hand turn; one a 1 ms pass)

# the FM-1's encoders and buttons as tests/host.c numbers them (firmware/src/platform.c, the FM-1 enums)
ENC = {"SELECT": 0, "ALGORITHM": 1, "KNOB1": 2, "KNOB2": 3, "KNOB3": 4, "KNOB4": 5, "PRESETS": 6}
BTN = {"OCT-": 0, "OCT+": 1, "FX": 2, "SEL": 3, "ENV": 4, "LFO": 5, "EDIT": 6, "GLO": 7, "HOME": 8, "SAVE": 9,
       "ARP": 10, "SEQ": 11, "PLAY": 12, "REC": 13}

FONT = ROOT / "assets/fonts/dejavu/DejaVuSansCondensed-Bold.ttf"
THEME_CSS = ROOT / "third_party/fm1-kit/web/theme.css"
DEFAULT_FOOTER = "Rendered by the firmware itself (tests/host.c): screen and sound as the engine makes them"

NOTE_RE = re.compile(r"^([A-Ga-g])([#b]?)(-?\d)$")
SEMI = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


class DemoError(Exception):
    pass


@dataclass(frozen=True)
class Theme:
    """The fm1-kit's tokens (theme.css): bg, surf, line, dim, text, theme (the screen's colour), accent."""
    bg: str = "#0B0F10"
    surf: str = "#141A1C"
    raised: str = "#232C2F"
    line: str = "#34424A"
    dim: str = "#8FA0A6"
    text: str = "#E8F0F0"
    theme: str = "#00C8A0"
    accent: str = "#FFB454"


def load_theme(name: str, css: Path = THEME_CSS) -> Theme:
    """A theme by name from the kit's theme.css; SEA (built in) when the kit or the name isn't there."""
    if not css.exists():
        if name != "sea":
            print(f"demo: {css.relative_to(ROOT)} not found (git submodule update --init): theme sea", file=sys.stderr)
        return Theme()
    m = re.search(r'\[data-theme="%s"\]\s*\{([^}]*)\}' % re.escape(name), css.read_text())
    if not m:
        raise DemoError(f"no theme {name!r} in {css.relative_to(ROOT)}")
    tok = dict(re.findall(r"--([a-z-]+):\s*(#[0-9A-Fa-f]{6})", m.group(1)))
    return Theme(**{k: tok[k] for k in Theme.__dataclass_fields__ if k in tok})


def note_num(s: str) -> int:
    """A note name (C3 = 48, as the FM-1's F3 = 53) or a MIDI number."""
    if s.isdigit():
        n = int(s)
    else:
        m = NOTE_RE.match(s)
        if not m:
            raise DemoError(f"not a note: {s!r}")
        n = 12 * (int(m.group(3)) + 1) + SEMI[m.group(1).upper()] + {"#": 1, "b": -1, "": 0}[m.group(2)]
    if not 0 <= n <= 127:
        raise DemoError(f"note out of range: {s!r}")
    return n


@dataclass
class Caption:
    t: float
    kind: str           # "title", "caption" or "clear"
    text: str = ""
    sub: str = ""


@dataclass
class Demo:
    host: list = field(default_factory=list)       # (ms, seq, line without its time)
    notes: list = field(default_factory=list)      # (on s, off s, note)
    captions: list = field(default_factory=list)
    end: float = 0.0
    name: str = ""
    theme: str = "sea"
    footer: str = DEFAULT_FOOTER

    def ev(self, t: float, line: str) -> None:
        self.host.append((max(1, round(t * 1000)), len(self.host), line))

    def midi_note(self, t: float, dur: float, n: int, vel: int) -> None:
        self.ev(t, f"midi 0x90 {n} {vel}")
        self.ev(t + dur, f"midi 0x80 {n} 0")
        self.notes.append((t, t + dur, n))

    def host_script(self) -> str:
        lines = [f"{ms} {line}" for ms, _, line in sorted(self.host)]
        lines.append(f"{round(self.end * 1000)} end")
        return "\n".join(lines) + "\n"


SETTINGS = ("name", "theme", "footer")


def parse(text: str) -> Demo:
    d = Demo()
    t = 0.0
    for no, raw in enumerate(text.splitlines(), 1):
        line = re.split(r"(?:^|\s)#", raw, maxsplit=1)[0].strip()     # (a # inside a note name, D#4, stays)
        if not line:
            continue
        try:
            tok, _, rest = line.partition(" ")
            if tok in SETTINGS:
                setattr(d, tok, rest.strip())
                continue
            t = t + float(tok[1:]) if tok.startswith("+") else float(tok)
            cmd, _, rest = rest.strip().partition(" ")
            rest = rest.strip()
            a = rest.split()
            if cmd in ("title", "caption"):
                text_, _, sub = rest.partition("|")
                d.captions.append(Caption(t, cmd, text_.strip(), sub.strip()))
            elif cmd == "clear":
                d.captions.append(Caption(t, "clear"))
            elif cmd == "note":
                vel = int(a[2]) if len(a) > 2 else 100
                for n in a[0].split(","):
                    d.midi_note(t, float(a[1]), note_num(n), vel)
            elif cmd == "seq":
                step, steps = float(a[0]), a[1].split(",")
                rep = int(a[2]) if len(a) > 2 else 1
                gate = float(a[3]) if len(a) > 3 else 0.5
                vel = int(a[4]) if len(a) > 4 else 100
                for r in range(rep):
                    for i, s in enumerate(steps):
                        if s != "-":
                            d.midi_note(t + (r * len(steps) + i) * step, step * gate, note_num(s), vel)
            elif cmd == "pc":
                n = int(a[0])
                if not 1 <= n <= 128:
                    raise DemoError("pc takes 1..128")
                d.ev(t, f"midi 0xC0 {n - 1} 0")
            elif cmd == "cc":
                d.ev(t, f"midi 0xB0 {int(a[0])} {int(a[1])}")
            elif cmd == "ccramp":
                cc, v0, v1, dur = int(a[0]), int(a[1]), int(a[2]), float(a[3])
                steps = max(1, abs(v1 - v0))
                for i in range(steps + 1):
                    d.ev(t + dur * i / steps, f"midi 0xB0 {cc} {round(v0 + (v1 - v0) * i / steps)}")
            elif cmd == "turn":
                # one detent at a time, as a hand turn reaches the firmware: M-Vsynth2 steps x2 when several land
                # in one pass (mvsynth2.c), so all at once a turn of 40 would move 80
                enc, n = ENC[a[0].upper()], int(a[1])
                dur = float(a[2]) if len(a) > 2 else TURN_GAP * (abs(n) - 1)
                if dur == 0:                    # DUR 0: one pass
                    d.ev(t, f"enc {enc} {n}")
                    continue
                sign = 1 if n > 0 else -1
                for i in range(abs(n)):
                    d.ev(t + (dur * i / (abs(n) - 1) if abs(n) > 1 else 0.0), f"enc {enc} {sign}")
            elif cmd == "press":
                d.ev(t, f"btn {BTN[a[0].upper()]}")
            elif cmd == "hold":
                d.ev(t, f"btndown {BTN[a[0].upper()]}")
                d.ev(t + float(a[1]), f"btnup {BTN[a[0].upper()]}")
            elif cmd == "host":
                d.ev(t, rest)
            elif cmd == "end":
                d.end = t
            else:
                raise DemoError(f"unknown command {cmd!r}")
        except (DemoError, KeyError, IndexError, ValueError) as e:
            raise DemoError(f"line {no}: {raw.strip()!r}: {e}") from None
    if d.end <= 0:
        raise DemoError("no `end` line")
    return d


# ---- running the firmware ----

def host_build(run_host_sh: Path = ROOT / "tests/run_host.sh") -> tuple[str, list[str]]:
    """The binary's name and its sources, from tests/run_host.sh's FM-1 build (N=...; cc ... firmware/src/*.c)."""
    text = run_host_sh.read_text()
    name = re.search(r"^N=(\w+)", text, re.M)
    srcs = re.findall(r"firmware/src/[\w.-]+\.c", next((ln for ln in text.splitlines() if ln.startswith("cc ")), ""))
    if not name or not srcs:
        raise DemoError(f"can't read the host build from {run_host_sh.relative_to(ROOT)}")
    return name.group(1), srcs


def build_host(out: Path, name: str, srcs: list[str]) -> Path:
    """Build the firmware for this computer, as tests/run_host.sh does (the FM-1 build)."""
    exe = out / name
    cmd = ["cc", "-O2", "-Wall", "-Wno-unused-function", "-DDINGHY_HOST", '-DAPP_VER="demo"',
           "-Ifirmware/hal", "-Ifirmware/src", "-Ifirmware/gen", "-o", str(exe), *srcs]
    subprocess.run(cmd, cwd=ROOT, check=True)
    return exe


def run_host(exe: Path, script: Path, base: Path) -> None:
    r = subprocess.run([str(exe), str(base), str(script), str(FPS)], cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise DemoError("the host run failed")


def wav_peak(path: Path) -> float:
    with wave.open(str(path)) as w:
        a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return float(np.abs(a.astype(np.int32)).max()) / 32768.0 if a.size else 0.0


# ---- the page ----

def font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONT), size)


SCREEN_XY, SCALE = (120, 180), 3                    # the 240x240 screen drawn 720x720
RIGHT_X, RIGHT_W = 960, 840
SCOPE_Y, SCOPE_H = 500, 150
FREQ_Y, FREQ_H = 690, 120
KEYS_Y, KEYS_H, KEY_LO, KEY_HI = 840, 84, 36, 95    # C2..B6: 35 white keys, 24 px each
FM1_KEY0, FM1_NKEY = 53, 27                         # the FM-1's keys: F3 .. G5 at OCT 0


def key_leds(log: Path) -> list[set[int]]:
    """Each frame's lit FM-1 keys as notes, from tests/host.c's film log (ms, keys held, key LEDs, button LEDs, ..):
    the firmware lights a key while a voice holds its note (a key, MIDI, the arp, the sequencer). OCT 0 assumed."""
    out = []
    for line in log.read_text().splitlines() if log.exists() else []:
        f = line.split()
        bits = int(f[1]) | int(f[2]) if len(f) > 2 else 0
        out.append({FM1_KEY0 + k for k in range(FM1_NKEY) if bits >> k & 1})
    return out


def wrap(draw: ImageDraw.ImageDraw, text: str, f: ImageFont.FreeTypeFont, width: int) -> list[str]:
    out, cur = [], ""
    for word in text.split():
        trial = f"{cur} {word}".strip()
        if draw.textlength(trial, font=f) <= width or not cur:
            cur = trial
        else:
            out.append(cur)
            cur = word
    return out + ([cur] if cur else [])


def static_page(th: Theme, label: str, footer: str) -> Image.Image:
    im = Image.new("RGB", (W, H), th.bg)
    d = ImageDraw.Draw(im)
    x, y = SCREEN_XY
    s = 240 * SCALE
    d.rounded_rectangle((x - 24, y - 24, x + s + 24, y + s + 24), radius=22, fill=th.surf, outline=th.raised, width=2)
    d.text((x - 24, y + s + 40), "FM-1 SCREEN  ·  240 × 240, EVERY PIXEL × 3", font=font(18), fill=th.dim)
    d.text((RIGHT_X, 132), label.upper(), font=font(22), fill=th.theme)
    for name, yy, hh in (("OUTPUT", SCOPE_Y, SCOPE_H), ("SPECTRUM", FREQ_Y, FREQ_H)):
        d.text((RIGHT_X, yy - 28), name, font=font(16), fill=th.dim)
        d.rounded_rectangle((RIGHT_X, yy, RIGHT_X + RIGHT_W, yy + hh), radius=10, fill=th.surf, outline=th.raised)
    d.text((RIGHT_X, KEYS_Y - 28), "NOTES", font=font(16), fill=th.dim)
    d.text((96, 1012), footer, font=font(18), fill=th.dim)
    return im


def white_keys() -> list[int]:
    return [n for n in range(KEY_LO, KEY_HI + 1) if n % 12 in (0, 2, 4, 5, 7, 9, 11)]


def draw_keys(d: ImageDraw.ImageDraw, th: Theme, held: set[int]) -> None:
    whites = white_keys()
    kw = RIGHT_W // len(whites)
    xpos = {}
    for i, n in enumerate(whites):
        x = RIGHT_X + i * kw
        xpos[n] = x
        d.rectangle((x, KEYS_Y, x + kw - 2, KEYS_Y + KEYS_H), fill=th.theme if n in held else th.text)
    for n in range(KEY_LO, KEY_HI + 1):
        if n % 12 in (1, 3, 6, 8, 10):
            x = xpos[n - 1] + kw - kw // 3
            d.rectangle((x, KEYS_Y, x + 2 * kw // 3 - 2, KEYS_Y + KEYS_H * 3 // 5),
                        fill=th.theme if n in held else th.raised)


class Captions:
    """Caption and title layers, made once and faded in."""
    FADE = 0.25

    def __init__(self, caps: list[Caption], th: Theme):
        self.caps = sorted(caps, key=lambda c: c.t)
        self.th = th
        self.cache: dict[int, Image.Image] = {}

    def layer(self, i: int) -> Image.Image:
        if i in self.cache:
            return self.cache[i]
        c, th = self.caps[i], self.th
        im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        if c.kind == "title":
            d.rectangle((0, 0, W, H), fill=th.bg + "F2")
            ft, fs = font(110), font(40)
            tw = d.textlength(c.text, font=ft)
            d.text(((W - tw) / 2, 380), c.text, font=ft, fill=th.text)
            d.rectangle(((W - 160) / 2, 530, (W + 160) / 2, 536), fill=th.theme)
            y = 570
            for ln in wrap(d, c.sub, fs, 1400):
                d.text(((W - d.textlength(ln, font=fs)) / 2, y), ln, font=fs, fill=th.dim)
                y += 54
        elif c.kind == "caption":
            ft, fs = font(56), font(28)
            y = 176
            for ln in wrap(d, c.text, ft, RIGHT_W):
                d.text((RIGHT_X, y), ln, font=ft, fill=th.text)
                y += 66
            y += 10
            for ln in wrap(d, c.sub, fs, RIGHT_W):
                d.text((RIGHT_X, y), ln, font=fs, fill=th.accent)
                y += 38
        self.cache[i] = im
        return im

    def at(self, t: float) -> Image.Image | None:
        i = max((k for k, c in enumerate(self.caps) if c.t <= t), default=None)
        if i is None or self.caps[i].kind == "clear":
            return None
        im = self.layer(i)
        a = (t - self.caps[i].t) / self.FADE
        if a >= 1:
            return im
        faded = im.copy()
        faded.putalpha(im.getchannel("A").point(lambda v: int(v * max(0.0, a))))
        return faded


def compose(demo: Demo, base: Path, out_mp4: Path, label: str) -> int:
    frames = sorted(base.parent.glob(base.name + "-[0-9][0-9][0-9][0-9][0-9].ppm"))
    if not frames:
        raise DemoError("the host run made no frames")
    wav = base.with_suffix(".wav")
    peak = wav_peak(wav)
    gain = min(4.0, 10 ** (-1 / 20) / peak) if peak > 0 else 1.0   # peak to -1 dBFS, at most +12 dB
    th = load_theme(demo.theme)
    page = static_page(th, label, demo.footer)
    caps = Captions(demo.captions, th)
    leds = key_leds(base.parent / (base.name + "-log.txt"))
    mono = "pan=mono|c0=0.5*c0+0.5*c1"
    graph = (
        f"[1:a]volume={gain:.4f},asplit=3[aout][as][af];"
        f"[as]{mono},showwaves=s={RIGHT_W - 20}x{SCOPE_H - 20}:mode=cline:rate={FPS}:"
        f"colors=0x{th.theme[1:]}:draw=full[wv];"
        f"[af]{mono},showfreqs=s={RIGHT_W - 20}x{FREQ_H - 20}:mode=bar:fscale=log:ascale=log:win_size=4096:"
        f"colors=0x{th.accent[1:]}:rate={FPS}[fq];"
        f"[0:v][wv]overlay={RIGHT_X + 10}:{SCOPE_Y + 10}:shortest=1[v1];"
        f"[v1][fq]overlay={RIGHT_X + 10}:{FREQ_Y + 10}:shortest=1,format=yuv420p[v]"
    )
    cmd = ["ffmpeg", "-y", "-loglevel", "error",
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-",
           "-i", str(wav), "-filter_complex", graph, "-map", "[v]", "-map", "[aout]",
           "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-c:a", "aac", "-b:a", "256k",
           "-movflags", "+faststart", "-shortest", str(out_mp4)]
    ff = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    try:
        for k, fpath in enumerate(frames):
            t = k / FPS
            im = page.copy()
            im.paste(Image.open(fpath).convert("RGB").resize((240 * SCALE, 240 * SCALE), Image.NEAREST), SCREEN_XY)
            held = {n for on, off, n in demo.notes if on <= t < off} | (leds[k] if k < len(leds) else set())
            draw_keys(ImageDraw.Draw(im), th, held)
            layer = caps.at(t)
            if layer is not None:
                im.paste(layer, (0, 0), layer)
            ff.stdin.write(im.tobytes())
    finally:
        ff.stdin.close()
        rc = ff.wait()
    if rc:
        raise DemoError("ffmpeg failed")
    return len(frames)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("demo", type=Path, help="a .demo file")
    ap.add_argument("-o", "--out", type=Path, help="the MP4 (default build/demo/<name>.mp4)")
    ap.add_argument("--keep", action="store_true", help="keep the frames, the WAV and the host script")
    ap.add_argument("--script-only", action="store_true", help="print the host script and stop")
    args = ap.parse_args()
    try:
        demo = parse(args.demo.read_text(encoding="utf-8"))
        script = demo.host_script()
        if args.script_only:
            sys.stdout.write(script)
            return 0
        for tool in ("cc", "ffmpeg"):
            if not shutil.which(tool):
                raise DemoError(f"{tool} not found")
        exe_name, srcs = host_build()
        load_theme(demo.theme)                      # (a wrong theme name fails before the run)
        work = ROOT / "build/demo" / args.demo.stem
        if work.exists():
            shutil.rmtree(work)
        work.mkdir(parents=True)
        (work / "host-script.txt").write_text(script)
        exe = build_host(ROOT / "build/demo", exe_name, srcs)
        base = work / "f"
        run_host(exe, work / "host-script.txt", base)
        out = args.out or ROOT / "build/demo" / (args.demo.stem + ".mp4")
        n = compose(demo, base, out, demo.name or exe_name)
        if not args.keep:
            shutil.rmtree(work)
        print(f"video -> {out} ({n} frames, {n / FPS:.1f} s)")
        return 0
    except DemoError as e:
        print(f"demo: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
