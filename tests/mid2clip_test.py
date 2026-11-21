#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/mid2clip.py on made-up notes (no MIDI files needed): the loop it picks (Kris 7 Nov, 3a), the fold of an
override, and the coding the firmware reads (mv_seq.h FLASH, 4a). Then the list (tools/clips/clips.txt) on small MIDI
files written here: names, a blank loop, #!only (Kris 21 Nov: mvance-factory-clip-picker-proposal-2026-11-21.md, 2a
3a 4a). tests/run_unit.sh runs it."""
import os
import sys
import tempfile

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

# ---- the list (Kris 21 Nov): small MIDI files written here ----
def vq(v):
    o = [v & 127]
    v >>= 7
    while v:
        o.insert(0, (v & 127) | 128)
        v >>= 7
    return o


def mid(path, tracks):
    """a format-1 file at 96 a quarter; tracks: [(name or None, [(tick, note, length)])]"""
    body = b''
    for name, ns in tracks:
        ev = []
        for t, n, d in ns:
            ev += [(t, 1, [0x90, n, 100]), (t + d, 0, [0x80, n, 0])]
        ev.sort(key=lambda e: (e[0], e[1]))
        tr = bytearray()
        if name:
            tr += bytes([0, 0xFF, 3] + vq(len(name))) + name.encode()
        last = 0
        for t, _o, by in ev:
            tr += bytes(vq(t - last) + by)
            last = t
        tr += bytes([0, 0xFF, 0x2F, 0])
        body += b'MTrk' + len(tr).to_bytes(4, 'big') + bytes(tr)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(b'MThd' + (6).to_bytes(4, 'big') + bytes([0, 1, 0, len(tracks), 0, 96]) + body)


def bars_of(n, k=4):
    """k bars of quarter notes, all note n"""
    return [(i * 96, n, 48) for i in range(4 * k)]


tmp = tempfile.mkdtemp()
D = os.path.join(tmp, 'Midi')
mid(os.path.join(D, 'artist - song.mid'), [(None, []), ('Lead', bars_of(60)), ('Bass', bars_of(36)), ('Pad', bars_of(48))])
mid(os.path.join(D, 'two.mid'), [('MIDI Out', bars_of(62)), ('MIDI Out', bars_of(64, 2))])
mid(os.path.join(D, 'folder', 'arp.mid'), [('Arp', bars_of(70, 2))])


def lst(text):
    p = os.path.join(tmp, 'clips.txt')
    open(p, 'w').write(text)
    return m.read_list(p)


every = m.collect(D)
check([c['src'] for c in every] == ['artist - song.mid [Lead]', 'artist - song.mid [Bass]', 'artist - song.mid [Pad]',
                                    'two.mid [MIDI Out]', 'two.mid [MIDI Out]', 'folder/arp.mid'], f'no list: every part, path order: {[c["src"] for c in every]}')
L = lst('# a comment\nartist - song.mid | bass |  |  | BIG BASS\ntwo.mid | 2 | 1 | 1\n')
check(not L['only'] and L['lines'][0]['over'] is None and L['lines'][0]['name'] == 'BIG BASS', f'read: a blank loop, a name: {L}')
c = m.collect(D, L)
check(len(c) == 6 and c[1]['name'] == 'BIG BASS' and c[4]['bars'] == 1 and c[4]['how'] == 'override'
      and [x['name'] for x in c[:1] + c[2:4]] == [x['name'] for x in every[:1] + every[2:4]], 'no #!only: every part; a line renames or loops its own')
for bad in ('a.mid | | 1 |\n', 'a.mid | | 1\n', 'a.mid | | | | 12345678901234567\n', 'a.mid | | 0 | 4\n'):
    try:
        lst(bad)
        check(False, f'refused: {bad!r}')
    except ValueError:
        check(True, '')
L = lst('#!only\nfolder/arp.mid |  |  |\ntwo.mid | 2 |  |  | second\nartist - song.mid | bass | 2 | 1\n'
        'artist - song.mid | 1 |  |\nartist - song.mid | BASS |  |\ngone.mid |  |  |\ntwo.mid | Lead |  |\n')
notes = []
c = m.collect(D, L, notes)
check(L['only'] and [x['src'] for x in c] == ['folder/arp.mid', 'two.mid [MIDI Out]', 'artist - song.mid [Bass]', 'artist - song.mid [Lead]'],
      f'#!only: the listed parts, the list\'s order: {[x["src"] for x in c]}')
check(c[1]['events'][0][1] == 64 and c[1]['name'] == 'SECOND' and c[2]['how'] == 'override' and c[2]['bars'] == 1 and c[3]['name'] == every[0]['name'],
      f'#!only: name (upper case), loop, the tool\'s name: {[(x["name"], x["how"]) for x in c]}')
kinds = [w for w, _t in notes]
check(kinds.count('missing') == 2 and kinds.count('twice') == 1, f'reported: no file, no part, twice: {notes}')
check(sorted(t for w, t in notes if w == 'unlisted') == ['artist - song.mid [3: Pad]', 'two.mid [1: MIDI Out]'], f'not listed: {notes}')
check(m.collect(D, lst('#!only\n')) == [], '#!only and no lines: nothing')
check([x['src'] for x in m.collect(D, lst('#!only\nartist - song.mid |  |  |\n'))] == [x['src'] for x in every[:3]], '#!only, a file with blank track: its every part, in its order')

print(f'mid2clip_test: {checks} checks, {fails} failed')
sys.exit(1 if fails else 0)
