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
  the list tools/clips/clips.txt (git-ignored, as the files), a line a track:  path | track | start | bars [| name]
           path relative to DIR; track its name or number (1 = the first with notes, blank: all); start the first bar
           (1-based); bars the loop. Notes outside the loop fold into it (a pickup that spills past the end comes back
           to the start). start and bars both blank: the loop above. name (optional, 16 characters at most): the
           clip's name, else the one below. '#' starts a comment
  #!only   (Kris 21 Nov: mvance-factory-clip-picker-proposal-2026-11-21.md, 2a 3a 4a) a line "#!only" in the list:
           only the parts the list names are built, in the list's order (F1 = its first line), not every track in
           DIR. A line naming no file or part in DIR is reported and skipped; a part listed twice is built once. The
           editor page's Factory clips dialog writes such a list. Without it, every track, in path order, as before
  events   note-offs before note-ons at the same tick (a repeated note retriggers, never cut by its own end); a note
           a tick long at least; the velocity as in the file. Coded as the firmware reads them (mv_seq.h FLASH): a
           varint of (ticks since the last event << 1 | velocity follows), note | on << 7, then the velocity when it
           changed (note-ons; 100 at the start): about 2 bytes an event
  ROOT     the first note-on (as a recorded track's: TRANSPOSE's reference)
  automation (Kris 20 Nov) a track's CCs that move a sound parameter (tools/gen_seq_ccs.py: mv_cc_apply's table)
           become points (mv_seq.h AUTOMATION): on the 4-tick grid, the last value of each 8 ticks a CC (at the
           first tick of those 8 it was moved in), a repeated value dropped, a CC that never moves dropped (a setting,
           as a file's volume at its start); inside the loop as the notes are (folded
           by an override). Coded as a note-on whose velocity byte has bit 7 set (velocity | 128 always follows)
  name     16 characters: the song (the part after " - " in the file name, else the folder, else the file) and the
           track's name (its index when the name is a DAW's "MIDI Out")

The output holds third-party transcriptions: it is firmware/gen/mv_fclips_local.h, which git ignores. The tracked
firmware/gen/mv_fclips.h includes it when it is there and is an empty set otherwise (the public repo).
Standard library only."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_seq_ccs import sound_ccs               # noqa: E402  (the CCs that move the sound)

PPQN = 96                                       # the sequencer's ticks a quarter (mv_seq.h MV_SEQ_PPQN)
BAR = 4 * PPQN
MAX_BARS = 32                                   # LENGTH's top (mv_seq.h MV_SEQ_MAXBAR)
NAME = 16
PT_GRID = 4                                     # an automation point's tick: a multiple of this (MV_SEQ_PT_GRID)
PT_GAP = 8                                      # one point a CC per this many ticks at most (MV_SEQ_PT_GAP)
PT = 0x80                                       # a point's velocity flag (MV_SEQ_PT)


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


def read_ccs(path):
    """[[(tick, cc, value)]] a track (the same tracks as read_mid): the CCs of every channel, in file order"""
    b = open(path, 'rb').read()
    hl, _fmt, ntrk, _div = struct.unpack('>IHHH', b[4:14])
    i, tracks = 8 + hl, []
    while i + 8 <= len(b) and len(tracks) < ntrk:
        tag, ln = b[i:i + 4], struct.unpack('>I', b[i + 4:i + 8])[0]
        i += 8
        if tag != b'MTrk':
            i += ln
            continue
        j, end, t, rs, cc = i, i + ln, 0, 0, []
        while j < end:
            d, j = vlq(b, j)
            t += d
            st = b[j]
            if st == 0xFF:
                ml, k = vlq(b, j + 2)
                j = k + ml
                continue
            if st in (0xF0, 0xF7):
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
            if hi == 0xB0:
                cc.append((t, b[j], b[j + 1]))
            j += 2
        tracks.append(cc)
        i = end
    return tracks


def points_of(ccs, div, sound):
    """automation points (tick, cc, value) at 96 a quarter from a track's CCs: only the sound's (sound: {cc: ..});
    on the 4-tick grid; each CC's events grouped by 8-tick window (from its first move in it): the window's last value
    at its first tick; a value equal to the CC's last point dropped; a CC left with one point (it never moves: a
    setting such as a file's volume at its start) dropped. Sorted by tick, then CC"""
    out, last, win = [], {}, {}
    for t0, c, v in ccs:
        if c not in sound:
            continue
        t = (t0 * PPQN // div if div != PPQN else t0) // PT_GRID * PT_GRID
        w = win.get(c)
        if w is not None and t - w[0] < PT_GAP:
            w[1] = v                            # (inside its window: the later value wins)
            continue
        if w is not None and w[1] != last.get(c):
            out.append((w[0], c, w[1]))
            last[c] = w[1]
        win[c] = [t, v]
    for c, w in win.items():
        if w[1] != last.get(c):
            out.append((w[0], c, w[1]))
    n = {}
    for _t, c, _v in out:
        n[c] = n.get(c, 0) + 1
    return sorted(x for x in out if n[x[1]] > 1)   # (a CC that never moves: a setting, as a file's volume
                                                   # at its start, not automation: dropped)


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


def clip_of(notes, over=None, points=()):
    """(bars, events [(tick, note, vel)], notes kept, bars of the part, how) for one track's notes; over: an
    override (start bar 0-based, bars): its notes fold into the loop. points: its automation (points_of), kept
    inside the loop as the notes are (folded by an override; a tick off the grid after a fold: down onto it), and
    their events after the note-offs and before the note-ons at a tick"""
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
        ev.append((ta, 2, n, v))
        ev.append((tb, 0, n, 0))
    seen = set()
    for t, c, v in points:
        tp = (start + (t - start) % loop if over else t) - start
        if not over and not 0 <= tp < loop:
            continue
        tp = tp // PT_GRID * PT_GRID
        if (tp, c) in seen:                     # (two folded onto one tick: the first kept)
            continue
        seen.add((tp, c))
        ev.append((tp, 1, c, PT | v))
    ev.sort(key=lambda e: (e[0], e[1]))         # note-offs (0), points (1), note-ons (2) at a tick
    return bars, [(t, n, v) for t, _k, n, v in ev], len(kept), span_bars, how


def code(events):
    """events coded as the firmware reads them (mv_seq.h FLASH)"""
    out, last, vel = bytearray(), 0, 100
    for t, n, v in events:
        if v & PT:                              # a point: note | 128, its flagged value always follows (the notes'
            d = (t - last) << 1 | 1             # running velocity untouched)
            while True:
                out.append((d & 127) | (128 if d > 127 else 0))
                d >>= 7
                if not d:
                    break
            out.append(n | 128)
            out.append(v)
            last = t
            continue
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


ONLY = '#!only'                                 # the list's line for "only these parts" (2a)


def clean_name(name):
    """a clip's name as the firmware shows it: upper case, the characters it has (32..95, no quote or backslash)"""
    return ''.join(c if 32 <= ord(c) <= 95 and c not in '"\\' else ' ' for c in name.upper())


def read_list(path):
    """the list: {'only': the #!only line is in it, 'lines': [{path, track (lower case; '' every track), over
    ((start bar 0-based, bars) or None: the tool's loop), name (or None), text (the line, for messages)}]}"""
    lst = {'only': False, 'lines': []}
    if not path or not os.path.exists(path):
        return lst
    for ln in open(path, encoding='utf-8'):
        if ln.strip().lower() == ONLY:
            lst['only'] = True
            continue
        ln = ln.split('#', 1)[0].strip()
        if not ln:
            continue
        f = [x.strip() for x in ln.split('|')]
        if len(f) not in (4, 5):
            raise ValueError(f'{path}: "{ln}": want path | track | start | bars [| name]')
        if f[2] == '' and f[3] == '':
            over = None
        else:
            try:
                start, bars = int(f[2]), int(f[3])
            except ValueError:
                raise ValueError(f'{path}: "{ln}": start and bars: two numbers, or both blank') from None
            if start < 1 or not 1 <= bars <= MAX_BARS:
                raise ValueError(f'{path}: "{ln}": start from 1, bars 1..{MAX_BARS}')
            over = (start - 1, bars)
        name = f[4] if len(f) == 5 and f[4] else None
        if name is not None and len(name) > NAME:
            raise ValueError(f'{path}: "{ln}": a name of {NAME} characters at most')
        lst['lines'].append({'path': f[0].replace('\\', '/'), 'track': f[1].lower(), 'over': over,
                             'name': clean_name(name) if name else None, 'text': ln})
    return lst


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
    return clean_name(s)


def mid_files(root):
    """{path relative to root ('/' between folders): path} of every .mid under root, in the tool's order (path order)"""
    out = {}
    for d, _subs, files in sorted(os.walk(root)):
        for f in sorted(files):
            if f.lower().endswith('.mid'):
                p = os.path.join(d, f)
                out[os.path.relpath(p, root).replace(os.sep, '/')] = p
    return out


def parts_of(p, sound):
    """a file's parts (tracks with notes): [(name, index 1-based among them, notes, points)]; raises on a bad file"""
    div, tracks = read_mid(p)
    ccs = read_ccs(p)
    parts = [(nm, ev, cc) for (nm, ev), cc in zip(tracks, ccs) if any(e[1] for e in ev)]
    out = []
    for k, (nm, ev, cc) in enumerate(parts):
        notes = notes_of(ev, div)
        if notes:
            out.append((nm, k + 1, notes, points_of(cc, div, sound)))
    return out, len(parts)


def make_clip(p, root, rel, nm, k, nparts, notes, pts, over, name):
    bars, events, kept, span, how = clip_of(notes, over, pts)
    return {'name': name or name_of(p, root, nm, k, nparts), 'bars': bars,
            'root': next(n for _t, n, v in events if v and not v & PT),   # (the first note-on)
            'events': events, 'code': code(events), 'kept': kept, 'all': len(notes),
            'span': span, 'how': how,
            'src': rel + (f' [{nm or k}]' if nparts > 1 else '')}


def collect(root, lst=None, notes=None):
    """the clips, in F order. lst: read_list's. notes (a list, or None): ('missing' | 'twice' | 'unlisted', text) for
    the lines that named nothing, the parts listed twice, and (#!only) the parts in root the list leaves out"""
    lst = lst or {'only': False, 'lines': []}
    notes = notes if notes is not None else []
    sound = sound_ccs()
    files = mid_files(root)
    cache = {}

    def parts(rel):
        if rel not in cache:
            try:
                cache[rel] = parts_of(files[rel], sound)
            except (ValueError, IndexError, struct.error) as e:
                print(f'skipped {files[rel]}: {e}', file=sys.stderr)
                cache[rel] = ([], 0)
        return cache[rel]

    clips = []
    if not lst['only']:                         # every track, in path order; a line changes its loop / name
        line = {}
        for ln in lst['lines']:
            line[(ln['path'], ln['track'])] = ln
        for rel, p in files.items():
            ps, n = parts(rel)
            for nm, k, ns, pts in ps:
                ln = line.get((rel, (nm or '').lower())) or line.get((rel, str(k))) or line.get((rel, ''))
                clips.append(make_clip(p, root, rel, nm, k, n, ns, pts, ln and ln['over'], ln and ln['name']))
        return clips
    seen = set()                                # (2a, 3a) the listed parts, in the list's order
    for ln in lst['lines']:
        rel = ln['path']
        if rel not in files:
            notes.append(('missing', f'"{ln["text"]}": no {rel} in {root}'))
            continue
        ps, n = parts(rel)
        hit = [x for x in ps if not ln['track'] or (x[0] or '').lower() == ln['track'] or str(x[1]) == ln['track']]
        if not hit:
            notes.append(('missing', f'"{ln["text"]}": {rel} has no part "{ln["track"]}"'))
        for nm, k, ns, pts in hit:
            if (rel, k) in seen:
                notes.append(('twice', f'"{ln["text"]}": {rel} [{nm or k}] is listed above: built once'))
                continue
            seen.add((rel, k))
            clips.append(make_clip(files[rel], root, rel, nm, k, n, ns, pts, ln['over'], ln['name']))
    for rel in files:                           # (what the list leaves out: --check shows it)
        ps, n = parts(rel)
        for nm, k, *_ in ps:
            if (rel, k) not in seen:            # (the part's number too: two parts may share a name)
                notes.append(('unlisted', rel + (f' [{k}{": " + nm if nm else ""}]' if n > 1 else '')))
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
    lst_read = read_list(lst)
    notes = []
    clips = collect(args[0], lst_read, notes)
    if lst_read['only']:
        print(f'{lst}: #!only: {len(clips)} parts listed', file=sys.stderr)
    for what, text in notes:
        if what != 'unlisted':
            print(f'{lst}: {text}', file=sys.stderr)
    for i, c in enumerate(clips):
        note = {'whole': '', 'repeat': f'  (repeats: {c["span"]} bars -> {c["bars"]})',
                'override': f'  (list: {c["span"]} bars -> {c["bars"]})'}.get(c['how'], f'  ({c["how"]} of {c["span"]})')
        print(f'F{i + 1:<3} {c["name"]:16s} {c["bars"]:2d} bar {c["kept"]:4d} notes {len(c["code"]):5d} B  {c["src"]}{note}')
    if check:
        left = [t for w, t in notes if w == 'unlisted']
        if left:
            print(f'not listed ({len(left)}): ' + '; '.join(left))
        return 0
    n = emit(clips, out)
    print(f'{out}: {len(clips)} clips, {n} bytes of events')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
