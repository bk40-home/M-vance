#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# tools/demo/demo.py's .demo parser: what reaches the host script.   python3 tests/demo_test.py   (exit 0 = passed)
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/demo"))
import demo  # noqa: E402

fails = 0


def ok(c, what):
    global fails
    print(("ok   " if c else "FAIL ") + what)
    fails += not c


def lines(src):
    return [ln.split(" ", 1)[1] for ln in demo.parse(src + "\n9 end\n").host_script().splitlines()[:-1]]


ok(demo.note_num("C3") == 48 and demo.note_num("F3") == 53 and demo.note_num("D#4") == 63
   and demo.note_num("Bb1") == 34 and demo.note_num("60") == 60, "note names (C3 = 48, the FM-1's F3 = 53)")
ok(lines("1 note D#4 0.5 # a comment") == ["midi 0x90 63 100", "midi 0x80 63 0"], "# in a note name; # after a space")
ok(lines("1 pc 41") == ["midi 0xC0 40 0"], "pc counts slots as the screen does (041 -> 40)")
t = lines("1 turn KNOB2 -3")
ok(t == ["enc 3 -1"] * 3, "a turn goes one detent at a time (no x2 fast turn)")
ok(lines("1 turn SELECT 17 0") == ["enc 0 17"], "DUR 0: every detent in one pass")
s = demo.parse("1 turn KNOB3 5 1.0\n9 end\n").host_script().splitlines()
ok([int(x.split()[0]) for x in s[:5]] == [1000, 1250, 1500, 1750, 2000], "a turn over DUR spreads its detents evenly")
ok(lines("1 seq 0.5 C2,-,E2") == ["midi 0x90 36 100", "midi 0x80 36 0", "midi 0x90 40 100", "midi 0x80 40 0"],
   "seq: a step each STEP, - a rest")
ok(lines("1 press SEL\n+0.5 hold HOME 1") == ["btn 3", "btndown 8", "btnup 8"], "buttons, relative times")
d = demo.parse("name JT-80x0\ntheme amber\nfooter a  ·  b # c\n1 end\n")
ok((d.name, d.theme, d.footer) == ("JT-80x0", "amber", "a  ·  b"), "settings: name, theme, footer")
name, srcs = demo.host_build()
ok(name and srcs and all((demo.ROOT / s).exists() for s in srcs), f"the host build from tests/run_host.sh ({name})")
th = demo.load_theme("sea")
ok(th.theme.startswith("#") and len(th.bg) == 7, "a kit theme (sea)")
try:
    demo.parse("1 note C3 1\n")
    ok(False, "no end line is an error")
except demo.DemoError:
    ok(True, "no end line is an error")
print("failed" if fails else "all passed")
sys.exit(1 if fails else 0)
