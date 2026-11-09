#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""M-vance factory clips from MIDI files (Kris 7 Nov 2026: "each track as one sequence", loadable into UPPER or LOWER).

    python3 tools/mid2clip.py DIR [-o firmware/gen/mv_fclips_local.h] [--list tools/clips/clips.txt]
    python3 tools/mid2clip.py --check DIR          (the table only: what each track becomes, nothing written)

Every track with notes in every .mid under DIR becomes one read-only clip, in path order. The firmware shows them after
the 12 user clips on SEQ 3/3 (F1, F2 ..): LOAD puts one into the selected track, as a user clip.

  timing   the file's ticks rescaled to the sequencer's 96 a quarter (most files are 96 already: exact); 4/4 bars
  loop     (Kris 7 Nov, 3a) from the bar of the track's first note: the shortest section that repeats exactly (bar by
           bar: note starts, lengths, pitches) through the whole part and is heard twice at least, else the whole part, in whole bars, 32 at most
           (a longer part: its first 32). No limit on notes: the firmware plays factory clips from flash. A note-off
           past the loop's end ends at its last tick
  overrides  tools/clips/clips.txt (git-ignored, as the files), a line a track:  path | track | start | bars
           path relative to DIR; track its name or number (1 = the first with notes, blank: all); start the first bar
           (1-based); bars the loop. Notes outside the loop fold into it (a pickup that spills past the end comes back
           to the start); '#' starts a comment
  events   note-offs before note-ons at the same tick (a repeated note retriggers, never cut by its own end); a note
           a tick long at least; the velocity as in the file. Coded as the firmware reads them (mv_seq.h FLASH): a
           varint of (ticks since the last event << 1 | velocity follows), note | on << 7, then the velocity when it
           changed (note-ons; 100 at the start): about 2 bytes an event
  ROOT     the first note-on (as a recorded track's: TRANSPOSE's reference)
  name     16 characters: the song (the part after " - " in the file name, else the folder, else the file) and the
           track's name (its index when the name is a DAW's "MIDI Out")

The output holds third-party transcriptions: it is firmware/gen/mv_fclips_local.h, which git ignores. The tracked
firmware/gen/mv_fclips.h includes it when it is there and is an empty set otherwise (the public repo).
Standard library only."""
import os
import struct
import sys

PPQN = 96                                       # the sequencer's ticks a quarter (mv_seq.h MV_SEQ_PPQN)
BAR = 4 * PPQN
MAX_BARS = 32                                   # LENGTH's top (mv_seq.h MV_SEQ_MAXBAR)
NAME = 16


def vlq(b, i):
    v = 0
    while True:
        c = b[i]
        i += 1
        v = (v << 7) | (c & 127)
        if not c & 128:
            return v, i


def read_mid(path):
    """(division, [(track name, [(tick, on, note, velocity)])]) of a standard MIDI file (format 0 or 1)"""
    b = open(path, 'rb').read()
    if b[:4] != b'MThd':
        raise ValueError('not a MIDI file')
    hl, _fmt, ntrk, div = struct.unpack('>IHHH', b[4:14])
    if div & 0x8000:
        raise ValueError('SMPTE time division: not supported')
    i, tracks = 8 + hl, []
    while i + 8 <= len(b) and len(tracks) < ntrk:
        tag, ln = b[i:i + 4], struct.unpack('>I', b[i + 4:i + 8])[0]
        i += 8
        if tag != b'MTrk':
            i += ln
            continue
        j, end, t, rs, ev, name = i, i + ln, 0, 0, [], ''
        while j < end:
            d, j = vlq(b, j)
            t += d
            st = b[j]
            if st == 0xFF:                      # meta: the track's name kept
                ty = b[j + 1]
                ml, k = vlq(b, j + 2)
                if ty == 3:
                    name = b[k:k + ml].decode('latin1').strip()
                j = k + ml
                continue
            if st in (0xF0, 0xF7):              # SysEx: skipped
                ml, k = vlq(b, j + 1)
                j = k + ml
                continue
            if st & 0x80:
                rs = st
                j += 1
            hi = rs & 0xF0
            if hi in (0xC0, 0xD0):
                j += 1
                continue
            d1, d2 = b[j], b[j + 1]
            j += 2
            if hi == 0x90 and d2:
                ev.append((t, 1, d1, d2))
            elif hi in (0x80, 0x90):
                ev.append((t, 0, d1, 0))
        tracks.append((name, ev))
        i = end
    return div, tracks


def notes_of(ev, div):
    """(on tick, off tick, note, velocity) at 96 a quarter, from on / off events (an unended note: a 1/16)"""
    out, held = [], {}
    for t, on, n, v in sorted(ev, key=lambda e: (e[0], e[1])):
        t = t * PPQN // div if div != PPQN else t
        if on:
            if n in held:                       # the same note again before its end: the last one ends here
                a, va = held.pop(n)
                out.append((a, max(t, a + 1), n, va))
            held[n] = (t, v)
        elif n in held:
            a, va = held.pop(n)
            out.append((a, max(t, a + 1), n, va))
    for n, (a, va) in held.items():
        out.append((a, a + PPQN // 4, n, va))
    return sorted(out)


def period(notes, start, nbars):
    """the shortest P (bars, P <= nbars / 2) with every bar equal to bar i mod P (note starts, lengths, pitches),
    else nbars"""
    bars = [[] for _ in range(nbars)]
    for a, b, n, _v in notes:
        i = (a - start) // BAR
        if 0 <= i < nbars:
            bars[i].append(((a - start) % BAR, b - a, n))
    bars = [tuple(sorted(x)) for x in bars]
    for p in range(1, nbars // 2 + 1):          # (heard twice at least: a part whose last bar is its first again
        if all(bars[i] == bars[i % p] for i in range(nbars)):   # is not a repeat of all but that bar)
            return p
    return nbars


def clip_of(notes, over=None):
    """(bars, events [(tick, note, vel)], notes kept, bars of the part, how) for one track's notes; over: an
    override (start bar 0-based, bars): its notes fold into the loop"""
    span_bars = -(-(max(b for _, b, _, _ in notes) - notes[0][0] // BAR * BAR) // BAR)
    if over:
        start, bars = over[0] * BAR, over[1]
        loop = bars * BAR
        kept = [(start + (a - start) % loop, b - a, n, v) for a, b, n, v in notes]   # folded into the loop
        kept = [(a, a + d, n, v) for a, d, n, v in kept]
        how = 'override'
    else:
        start = notes[0][0] // BAR * BAR
        bars = period(notes, start, span_bars)
        how = 'repeat' if bars < span_bars else 'whole'
        if bars > MAX_BARS:
            bars, how = MAX_BARS, f'first {MAX_BARS}'
        loop = bars * BAR
        kept = [x for x in notes if start <= x[0] < start + loop]
    ev = []
    for a, b, n, v in kept:
        ta, tb = a - start, min(b - start, loop - 1)
        if tb <= ta:
            tb = ta + 1 if ta + 1 < loop else ta   # (a note at the very end: as short as it can be)
        ev.append((ta, 1, n, v))
        ev.append((tb, 0, n, 0))
    ev.sort(key=lambda e: (e[0], e[1]))         # note-offs (0) before note-ons (1) at a tick
    return bars, [(t, n, v) for t, _on, n, v in ev], len(kept), span_bars, how


def code(events):
    """events coded as the firmware reads them (mv_seq.h FLASH)"""
    out, last, vel = bytearray(), 0, 100
    for t, n, v in events:
        chg = 1 if v and v != vel else 0
        d = (t - last) << 1 | chg
        while True:
            out.append((d & 127) | (128 if d > 127 else 0))
            d >>= 7
            if not d:
                break
        out.append(n | (128 if v else 0))
        if chg:
            out.append(v)
            vel = v
        last = t
    return bytes(out)


def read_list(path):
    """the override list: {(path, track): (start bar 0-based, bars)}; track '' = every track of the file"""
    over = {}
    if not path or not os.path.exists(path):
        return over
    for ln in open(path, encoding='utf-8'):
        ln = ln.split('#', 1)[0].strip()
        if not ln:
            continue
        f = [x.strip() for x in ln.split('|')]
        if len(f) != 4:
            raise ValueError(f'{path}: "{ln}": want path | track | start | bars')
        start, bars = int(f[2]), int(f[3])
        if start < 1 or not 1 <= bars <= MAX_BARS:
            raise ValueError(f'{path}: "{ln}": start from 1, bars 1..{MAX_BARS}')
        over[(f[0].replace('\\', '/'), f[1].lower())] = (start - 1, bars)
    return over


def name_of(path, root, track_name, index, ntracks):
    rel = os.path.relpath(path, root)
    stem = os.path.splitext(os.path.basename(rel))[0]
    folder = os.path.dirname(rel)
    if ' - ' in stem:
        song = stem.split(' - ', 1)[1]
    elif folder:
        song = os.path.basename(folder)
    else:
        song = stem
    song = song.split('(')[0].strip()
    tn = track_name if track_name and not track_name.upper().startswith('MIDI OUT') else (
        str(index) if ntracks > 1 else '')
    if not tn and folder and stem.lower() not in song.lower():
        tn = stem                               # (a folder of one part a file: the file names the part)
    tn = tn.upper()
    song = song.upper()
    room = NAME - (len(tn) + 1 if tn else 0)
    s = (song[:max(room, 4)].rstrip() + (' ' + tn if tn else ''))[:NAME]   # (no double space at the cut)
    return ''.join(c if 32 <= ord(c) <= 95 and c not in '"\\' else ' ' for c in s)


def collect(root, over=None):
    over = over or {}
    clips = []
    for d, _subs, files in sorted(os.walk(root)):
        _subs.sort()
        for f in sorted(files):
            if not f.lower().endswith('.mid'):
                continue
            p = os.path.join(d, f)
            try:
                div, tracks = read_mid(p)
            except (ValueError, IndexError, struct.error) as e:
                print(f'skipped {p}: {e}', file=sys.stderr)
                continue
            parts = [(nm, ev) for nm, ev in tracks if any(e[1] for e in ev)]
            rel = os.path.relpath(p, root).replace(os.sep, '/')
            for k, (nm, ev) in enumerate(parts):
                notes = notes_of(ev, div)
                if not notes:
                    continue
                o = over.get((rel, (nm or '').lower())) or over.get((rel, str(k + 1))) or over.get((rel, ''))
                bars, events, kept, span, how = clip_of(notes, o)
                clips.append({'name': name_of(p, root, nm, k + 1, len(parts)), 'bars': bars,
                              'root': next(n for _t, n, v in events if v),   # (the first note-on)
                              'events': events, 'code': code(events), 'kept': kept, 'all': len(notes),
                              'span': span, 'how': how,
                              'src': rel + (f' [{nm or k + 1}]' if len(parts) > 1 else '')})
    return clips


def emit(clips, out):
    lines = ['/* SPDX-License-Identifier: GPL-3.0-only */',
             '/* M-vance factory clips: made by tools/mid2clip.py from local MIDI files. Third-party transcriptions: this',
             ' * file is git-ignored (firmware/gen/mv_fclips.h includes it when it is there). Each clip: its name, LENGTH',
             ' * in bars, ROOT, events, their coded bytes (mv_seq.h FLASH: played straight from here). */',
             '#pragma once', '#include <stdint.h>', '']
    total = 0
    for i, c in enumerate(clips):
        b = list(c['code'])
        total += len(b)
        rows = [', '.join(str(x) for x in b[j:j + 24]) for j in range(0, len(b), 24)]
        lines.append(f'static const uint8_t MV_FCLIP_EV{i}[{len(b)}] = {{   /* {c["src"]} */')
        lines += ['    ' + r + ',' for r in rows]
        lines.append('};')
    lines.append('')
    lines.append(f'#define MV_FCLIP_N {len(clips)}u')
    lines.append('static const mv_fclip_t MV_FCLIPS[MV_FCLIP_N] = {')
    for i, c in enumerate(clips):
        lines.append(f'    {{"{c["name"]}", {c["bars"]}, {c["root"]}, {len(c["events"])}, {len(c["code"])}, MV_FCLIP_EV{i}}},')
    lines.append('};')
    lines.append(f'/* {len(clips)} clips, {total} bytes of events */')
    open(out, 'w', newline='\n').write('\n'.join(lines) + '\n')
    return total


def main(argv):
    check = '--check' in argv
    args = [a for a in argv if a != '--check']
    out = 'firmware/gen/mv_fclips_local.h'
    lst = 'tools/clips/clips.txt'
    for flag in ('-o', '--list'):
        if flag in args:
            k = args.index(flag)
            if flag == '-o':
                out = args[k + 1]
            else:
                lst = args[k + 1]
            del args[k:k + 2]
    if len(args) != 1:
        print(__doc__)
        return 2
    clips = collect(args[0], read_list(lst))
    for i, c in enumerate(clips):
        note = {'whole': '', 'repeat': f'  (repeats: {c["span"]} bars -> {c["bars"]})',
                'override': f'  (list: {c["span"]} bars -> {c["bars"]})'}.get(c['how'], f'  ({c["how"]} of {c["span"]})')
        print(f'F{i + 1:<3} {c["name"]:16s} {c["bars"]:2d} bar {c["kept"]:4d} notes {len(c["code"]):5d} B  {c["src"]}{note}')
    if check:
        return 0
    n = emit(clips, out)
    print(f'{out}: {len(clips)} clips, {n} bytes of events')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
