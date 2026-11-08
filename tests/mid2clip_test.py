#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/mid2clip.py on made-up notes (no MIDI files needed): the loop it picks (Kris 7 Nov, 3a), the fold of an
override, and the coding the firmware reads (mv_seq.h FLASH, 4a). tests/run_unit.sh runs it."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
import mid2clip as m  # noqa: E402

BAR = m.BAR
fails = checks = 0


def check(ok, what):
    global fails, checks
    checks += 1
    if not ok:
        fails += 1
        print('FAIL', what)


def decode(b):
    """the firmware's reader (mv_seq_rd_next) in Python: [(tick, note, vel)]"""
    out, i, t, vel = [], 0, 0, 100
    while i < len(b):
        d = sh = 0
        while True:
            d |= (b[i] & 127) << sh
            sh += 7
            i += 1
            if not b[i - 1] & 128:
                break
        t += d >> 1
        n, on = b[i] & 127, b[i] >> 7
        i += 1
        if d & 1:
            vel = b[i]
            i += 1
        out.append((t, n, vel if on else 0))
    return out


def notes(bars_of):
    """notes (start, end, note, vel) from one bar pattern per bar: [(offset, length, note)]"""
    return [(i * BAR + a, i * BAR + a + d, n, 100) for i, p in enumerate(bars_of) for a, d, n in p]


A = [(0, 48, 36), (192, 48, 43)]
B = [(0, 96, 38)]
# a 2-bar figure 4 times: 2 bars
bars, ev, kept, span, how = m.clip_of(notes([A, B] * 4))
check(bars == 2 and how == 'repeat' and span == 8 and kept == 3, f'repeat: {bars} {how} {span} {kept}')
# the same with its last bar changed (bar 8 = bar 1 again: heard once, not a repeat): the whole 8
bars, ev, kept, span, how = m.clip_of(notes([A, B] * 3 + [A, A]))
check(bars == 8 and how == 'whole', f'whole: {bars} {how}')
# a 3-bar figure (not a power of two) over 9 bars: 3
bars, *_ , how = m.clip_of(notes([A, B, B] * 3))
check(bars == 3 and how == 'repeat', f'3 bars: {bars} {how}')
# 40 bars of changing notes: the first 32
bars, ev, kept, span, how = m.clip_of(notes([[(0, 48, 40 + i % 37)] for i in range(40)]))
check(bars == 32 and kept == 32 and how.startswith('first'), f'cap: {bars} {kept} {how}')
# a part starting in bar 3: its loop starts there (tick 0 = bar 3's start)
bars, ev, *_ = m.clip_of(notes([[], [], A, B, A, B]))
check(bars == 2 and ev[0][0] == 0, f'late start: {bars} {ev[:1]}')
# an override: 2 bars from bar 1, a note in bar 3 folds back to bar 1 (as Ayla's pickup)
bars, ev, kept, span, how = m.clip_of(notes([A, B, [(96, 48, 50)]]), (0, 2))
check(bars == 2 and how == 'override' and (96, 50, 100) in ev, f'fold: {bars} {how} {ev}')
# a note-off past the loop's end ends at its last tick; note-offs before note-ons at a tick
bars, ev, *_ = m.clip_of([(0, 3 * BAR, 36, 100), (BAR, BAR + 10, 40, 100)], (0, 2))
check(ev[-1] == (BAR * 2 - 1, 36, 0), f'clipped end: {ev}')
ev2 = m.clip_of([(0, 96, 36, 100), (96, 192, 36, 90)])[1]
check(ev2[1] == (96, 36, 0) and ev2[2] == (96, 36, 90), f'off before on: {ev2}')
# the coding: deltas over 127 (2-byte varints), velocity only when it changes, back to the events exactly
evs = [(0, 36, 100), (5, 36, 0), (300, 40, 64), (300, 41, 64), (2000, 40, 0), (12000, 41, 0)]
b = m.code(evs)
check(decode(b) == evs, f'round trip: {decode(b)}')
check(len(b) == 2 + 2 + 4 + 2 + 3 + 4, f'size: {len(b)}')

print(f'mid2clip_test: {checks} checks, {fails} failed')
sys.exit(1 if fails else 0)
