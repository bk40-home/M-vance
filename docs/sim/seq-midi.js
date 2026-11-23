// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance sequences: MIDI files in and out for the editor page (Kris 20 Nov 2026: fm1-port/mvance-seq-editor-proposal-
// 2026-11-20.md, 4a 7a). No DOM: node tests it (sim/seq-midi-test.mjs) against tools/mid2clip.py.
//
//   readMid      a standard MIDI file (format 0 or 1): its tracks' names and notes, at 96 ticks a quarter
//   autoLoop     tools/mid2clip.py's loop, ported line for line (clip_of with no override): from the bar of the first
//                note, the shortest exact repeat heard twice, else the whole part, 32 bars at most. The same events
//                as the tool, so an import here and a factory clip made from the same file agree
//   trim         the page's own cut of a part: from a start bar, LENGTH bars; notes outside dropped or folded in (as
//                the tool's clips.txt override), then transposed and quantized; a note is cut at the loop's end, as
//                the tool does. With autoLoop's start and bars (drop, no transpose / quantize): the tool's clip
//   writeMid     a clip as a one-track MIDI file at 96 a quarter (the loop's end as the track's end), for a DAW or for
//                mid2clip (a factory clip: no 128-note limit)
//   automation   a part's CCs that move the sound (seq-ccs.js) become points, as the tool's points_of / clip_of
//                (Kris 20 Nov); writeMid writes points back as CCs
//   the list     tools/clips/clips.txt as the tool reads it (Kris 21 Nov: mvance-factory-clip-picker-proposal-
//                2026-11-21.md, 2a 3a 4a): parseList / writeList, #!only, a part's name (nameOf: the tool's name_of),
//                the folder's path order (toolOrder) and selectParts: which parts the tool builds, in F order (collect)
import { PPQN, BAR, MAXBAR, QGRID, PT, PT_GRID, eventsOf, withPoints, pointsOf, notesOf as clipNotes } from './seq-model.js?v=20261123211538';
import { SOUND_CCS } from './seq-ccs.js?v=20261123211538';
const PT_GAP = 8;                          // one point a CC per this many ticks at most (MV_SEQ_PT_GAP)

// ---- reading (mid2clip.py read_mid + notes_of) ----
function vlq(b, i) { let v = 0; for (;;) { const c = b[i++]; v = (v * 128) + (c & 127); if (!(c & 128)) return [v, i]; } }
const u32 = (b, i) => ((b[i] << 24) >>> 0) + (b[i + 1] << 16) + (b[i + 2] << 8) + b[i + 3];
const u16 = (b, i) => (b[i] << 8) | b[i + 1];

// { div, tracks: [{ name, ev: [[tick, on, note, vel]] }] } (throws on what is not a MIDI file)
export function parseMid(b) {
  if (String.fromCharCode(...b.subarray(0, 4)) !== 'MThd') throw new Error('not a MIDI file');
  const hl = u32(b, 4), ntrk = u16(b, 10), div = u16(b, 12);
  if (div & 0x8000) throw new Error('SMPTE time division: not supported');
  let i = 8 + hl;
  const tracks = [];
  while (i + 8 <= b.length && tracks.length < ntrk) {
    const tag = String.fromCharCode(...b.subarray(i, i + 4)), ln = u32(b, i + 4);
    i += 8;
    if (tag !== 'MTrk') { i += ln; continue; }
    let j = i, t = 0, rs = 0, name = '';
    const end = i + ln, ev = [], cc = [];
    while (j < end) {
      let d; [d, j] = vlq(b, j); t += d;
      const st = b[j];
      if (st === 0xff) {                  // meta: the track's name kept
        const ty = b[j + 1]; let ml, k; [ml, k] = vlq(b, j + 2);
        if (ty === 3) name = String.fromCharCode(...b.subarray(k, k + ml)).trim();
        j = k + ml; continue;
      }
      if (st === 0xf0 || st === 0xf7) { let ml, k; [ml, k] = vlq(b, j + 1); j = k + ml; continue; }   // SysEx: skipped
      if (st & 0x80) { rs = st; j++; }
      const hi = rs & 0xf0;
      if (hi === 0xc0 || hi === 0xd0) { j++; continue; }
      const d1 = b[j], d2 = b[j + 1]; j += 2;
      if (hi === 0xb0) cc.push([t, d1, d2]);
      if (hi === 0x90 && d2) ev.push([t, 1, d1, d2]);
      else if (hi === 0x80 || hi === 0x90) ev.push([t, 0, d1, 0]);
    }
    tracks.push({ name, ev, cc });
    i = end;
  }
  return { div, tracks };
}
const idiv = (a, b) => Math.floor(a / b);
// [on, off, note, vel] at 96 a quarter (an unended note: a 1/16)
export function notesOfMid(ev, div) {
  const out = [], held = new Map();
  const sorted = ev.map((e, k) => [e, k]).sort((x, y) => x[0][0] - y[0][0] || x[0][1] - y[0][1] || x[1] - y[1]).map((x) => x[0]);
  for (const [t0, on, n, v] of sorted) {
    const t = div !== PPQN ? idiv(t0 * PPQN, div) : t0;
    if (on) {
      if (held.has(n)) { const [a, va] = held.get(n); held.delete(n); out.push([a, Math.max(t, a + 1), n, va]); }
      held.set(n, [t, v]);
    } else if (held.has(n)) { const [a, va] = held.get(n); held.delete(n); out.push([a, Math.max(t, a + 1), n, va]); }
  }
  for (const [n, [a, va]] of held) out.push([a, a + PPQN / 4, n, va]);
  return out.sort(cmpTuple);
}
function cmpTuple(x, y) { for (let i = 0; i < x.length; i++) if (x[i] !== y[i]) return x[i] < y[i] ? -1 : 1; return 0; }

// a file's parts (tracks with notes): [{ name, index (1-based among them), notes }]
export function readMid(bytes) {
  const { div, tracks } = parseMid(bytes);
  return tracks.filter((t) => t.ev.some((e) => e[1]))
    .map((t, k) => ({ name: t.name, index: k + 1, notes: notesOfMid(t.ev, div), points: pointsOfMid(t.cc, div) }))
    .filter((p) => p.notes.length);
}
// a track's CCs [[tick, cc, value]] as points [[tick, cc, value]] (mid2clip.py points_of): the sound's CCs only, on
// the grid, the last value of each CC's 8-tick window at its first tick, a repeat dropped, a CC that never moves
// dropped
export function pointsOfMid(ccs, div, sound = SOUND_CCS) {
  const out = [], last = new Map(), win = new Map();
  for (const [t0, c, v] of ccs) {
    if (!(c in sound)) continue;
    const t = idiv(div !== PPQN ? idiv(t0 * PPQN, div) : t0, PT_GRID) * PT_GRID, w = win.get(c);
    if (w && t - w[0] < PT_GAP) { w[1] = v; continue; }
    if (w && w[1] !== last.get(c)) { out.push([w[0], c, w[1]]); last.set(c, w[1]); }
    win.set(c, [t, v]);
  }
  for (const [c, w] of win) if (w[1] !== last.get(c)) out.push([w[0], c, w[1]]);
  const n = new Map();
  for (const [, c] of out) n.set(c, (n.get(c) || 0) + 1);
  return out.filter((x) => n.get(x[1]) > 1).sort(cmpTuple);
}

// ---- the loop (mid2clip.py period + clip_of, no override) ----
export function period(notes, start, nbars) {
  const bars = Array.from({ length: nbars }, () => []);
  for (const [a, b, n] of notes) { const i = idiv(a - start, BAR); if (i >= 0 && i < nbars) bars[i].push([(a - start) % BAR, b - a, n]); }
  const key = bars.map((x) => JSON.stringify(x.sort(cmpTuple)));
  for (let p = 1; p <= idiv(nbars, 2); p++) if (key.every((k, i) => k === key[i % p])) return p;
  return nbars;
}
export const spanBars = (notes) => -idiv(-(Math.max(...notes.map((x) => x[1])) - idiv(notes[0][0], BAR) * BAR), BAR);
// { start (bar, 0-based), bars, how, span } the tool would choose
export function autoLoop(notes) {
  const start = idiv(notes[0][0], BAR), span = spanBars(notes);
  let bars = period(notes, start * BAR, span), how = bars < span ? 'repeat' : 'whole';
  if (bars > MAXBAR) { bars = MAXBAR; how = `first ${MAXBAR}`; }
  return { start, bars, how, span };
}
// the tool's events for a part: [[tick, note, vel]] (over: { start, bars } folds, as a clips.txt line; else autoLoop)
export function clipOf(notes, over = null, points = []) {
  let start, bars, kept;
  if (over) {
    start = over.start * BAR; bars = over.bars;
    const loop = bars * BAR;
    kept = notes.map(([a, b, n, v]) => { const s = start + (((a - start) % loop) + loop) % loop; return [s, s + (b - a), n, v]; });
  } else {
    const L = autoLoop(notes); start = L.start * BAR; bars = L.bars;
    kept = notes.filter((x) => x[0] >= start && x[0] < start + bars * BAR);
  }
  const loop = bars * BAR, ev = [];
  for (const [a, b, n, v] of kept) {
    const ta = a - start; let tb = Math.min(b - start, loop - 1);
    if (tb <= ta) tb = ta + 1 < loop ? ta + 1 : ta;
    ev.push([ta, 2, n, v], [tb, 0, n, 0]);
  }
  const seen = new Set();
  for (const [t, c, v] of points) {
    let tp = (over ? start + ((((t - start) % loop) + loop) % loop) : t) - start;
    if (!over && !(tp >= 0 && tp < loop)) continue;
    tp = idiv(tp, PT_GRID) * PT_GRID;
    if (seen.has(`${tp}:${c}`)) continue;
    seen.add(`${tp}:${c}`);
    ev.push([tp, 1, c, PT | v]);
  }
  ev.sort((x, y) => x[0] - y[0] || x[1] - y[1]);   // note-offs, points, note-ons (stable: as Python's sort)
  return { bars, events: ev.map(([t, , n, v]) => [t, n, v]), kept: kept.length };
}

// ---- the page's cut ----
// a clip from a part: opts { start (bar, 0-based), bars, fold (notes outside folded in, else dropped), transpose,
// quant (QUANT index: note starts to the nearest grid line, lengths kept), name }
export function trim(notes, opts, points = []) {
  const { start = 0, bars = 2, fold = false, transpose = 0, quant = 0, name = '' } = opts;
  const s = start * BAR, loop = bars * BAR, g = QGRID[quant] || 0, out = [];
  for (const [a, b, n0, v] of notes) {
    const n = n0 + transpose;
    if (n < 0 || n > 127) continue;       // (transposed off the keyboard: dropped)
    let t = a - s;
    if (t < 0 || t >= loop) { if (!fold) continue; t = ((t % loop) + loop) % loop; }
    if (g) t = Math.round(t / g) * g % loop;
    const len = Math.max(1, Math.min(b - a, loop - 1 - t));   // (cut at the loop's end, as the tool: no wrap)
    out.push({ t, len, n, v });
  }
  const pts = [];                          // the automation, cut as the notes (not transposed, not quantized)
  for (const [a, c, v] of points) {
    let t = a - s;
    if (t < 0 || t >= loop) { if (!fold) continue; t = ((t % loop) + loop) % loop; }
    pts.push({ t, cc: c, v });
  }
  const events = withPoints(eventsOf(out, bars), pts, bars);
  const first = events.find((e) => e.v && !(e.v & PT));
  return { bars, root: first ? first.n : null, quant, name, events };
}
// the tool's own clip for a part (what a factory clip from this file would be), as a page clip
export function toolClip(notes, name = '', points = []) {
  const c = clipOf(notes, null, points), first = c.events.find((e) => e[2] && !(e[2] & PT));
  return { bars: c.bars, root: first ? first[1] : null, quant: 0, name, events: c.events.map(([t, n, v]) => ({ t, n, v })) };
}

// ---- writing ----
function vlqOut(v) { const o = [v & 127]; while ((v = Math.floor(v / 128))) o.unshift((v & 127) | 128); return o; }
// a clip as a format-0 file, 96 a quarter, on channel 1; the track ends at the loop's end (a DAW's clip length)
export function writeMid(clip, name = '') {
  const tr = [], len = clip.bars * BAR;
  let last = 0;
  const put = (t, bytes) => { tr.push(...vlqOut(t - last), ...bytes); last = t; };
  if (name) put(0, [0xff, 0x03, ...vlqOut(name.length), ...Array.from(name, (c) => c.charCodeAt(0) & 127)]);
  put(0, [0xff, 0x58, 4, 4, 2, 24, 8]);   // 4/4
  // notes laid out flat (one over the loop's end is cut at the end: a file has no "round the loop")
  const flat = [];
  const notes = clipNotes(clip);
  for (const x of notes) { flat.push([x.t, 1, x.n, x.v]); flat.push([Math.min(x.t + x.len, len), 0, x.n, 0]); }
  for (const p of pointsOf(clip)) flat.push([p.t, 0.5, p.cc, p.v]);   // (points as CCs: after the offs, before the ons)
  flat.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  for (const [t, on, n, v] of flat) put(t, on === 0.5 ? [0xb0, n, v] : [on ? 0x90 : 0x80, n, on ? v : 0]);
  put(len, [0xff, 0x2f, 0]);
  const hd = [0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0, PPQN];
  const L = tr.length;
  return new Uint8Array([...hd, 0x4d, 0x54, 0x72, 0x6b, (L >>> 24) & 255, (L >>> 16) & 255, (L >>> 8) & 255, L & 255, ...tr]);
}

// ---- the list: tools/clips/clips.txt (mid2clip.py read_list, name_of, collect; Kris 21 Nov) ----
export const ONLY = '#!only';              // the list's line for "only these parts" (2a)
export const NAME = 16;                    // a factory clip's name: characters
// a name as the firmware shows it (mid2clip.py clean_name): upper case, characters 32..95, no quote or backslash
export const cleanName = (s) => Array.from(s.toUpperCase(), (c) => { const v = c.codePointAt(0); return v >= 32 && v <= 95 && c !== '"' && c !== '\\' ? c : ' '; }).join('');
// the tool's name for a part (name_of): the song (after " - " in the file's name, else its folder, else the file),
// then the part's name (its number when the file has several and the name is a DAW's "MIDI Out")
export function nameOf(rel, trackName, index, nparts) {
  const slash = rel.lastIndexOf('/'), folder = slash < 0 ? '' : rel.slice(0, slash);
  const base = rel.slice(slash + 1), dot = base.lastIndexOf('.'), stem = dot > 0 ? base.slice(0, dot) : base;
  let song = stem.includes(' - ') ? stem.slice(stem.indexOf(' - ') + 3) : folder ? folder.slice(folder.lastIndexOf('/') + 1) : stem;
  song = song.split('(')[0].trim();
  let tn = trackName && !trackName.toUpperCase().startsWith('MIDI OUT') ? trackName : (nparts > 1 ? String(index) : '');
  if (!tn && folder && !song.toLowerCase().includes(stem.toLowerCase())) tn = stem;   // (a folder of one part a file)
  tn = tn.toUpperCase(); song = song.toUpperCase();
  const room = NAME - (tn ? tn.length + 1 : 0);
  return cleanName((song.slice(0, Math.max(room, 4)).trimEnd() + (tn ? ` ${tn}` : '')).slice(0, NAME));
}
// files' paths (relative to the folder, '/' between) in the tool's order: the folder's own files, then each folder's
// by its path (os.walk, sorted), each folder's files by name
export function toolOrder(rels) {
  const key = (r) => { const i = r.lastIndexOf('/'); return [i < 0 ? '' : `/${r.slice(0, i)}`, r.slice(i + 1)]; };
  const cmp = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
  return rels.filter((r) => r.toLowerCase().endsWith('.mid')).sort((a, b) => { const x = key(a), y = key(b); return cmp(x[0], y[0]) || cmp(x[1], y[1]); });
}
// { only, lines: [{ path, track (lower case; '' every part), over ({ start (0-based), bars } or null: the tool's
// loop), name (or null), text }] } (throws as the tool does on a bad line)
export function parseList(text) {
  const lst = { only: false, lines: [] };
  for (const raw of text.split(/\r\n|\r|\n/)) {
    if (raw.trim().toLowerCase() === ONLY) { lst.only = true; continue; }
    const ln = raw.split('#')[0].trim();
    if (!ln) continue;
    const f = ln.split('|').map((x) => x.trim());
    if (f.length !== 4 && f.length !== 5) throw new Error(`"${ln}": want path | track | start | bars [| name]`);
    let over = null;
    if (f[2] !== '' || f[3] !== '') {
      if (!/^[+-]?\d+$/.test(f[2]) || !/^[+-]?\d+$/.test(f[3])) throw new Error(`"${ln}": start and bars: two numbers, or both blank`);
      const start = +f[2], bars = +f[3];
      if (start < 1 || !(bars >= 1 && bars <= MAXBAR)) throw new Error(`"${ln}": start from 1, bars 1..${MAXBAR}`);
      over = { start: start - 1, bars };
    }
    const name = f.length === 5 && f[4] ? f[4] : null;
    if (name && name.length > NAME) throw new Error(`"${ln}": a name of ${NAME} characters at most`);
    lst.lines.push({ path: f[0].replace(/\\/g, '/'), track: f[1].toLowerCase(), over, name: name ? cleanName(name) : null, text: ln });
  }
  return lst;
}
// a list as clips.txt: the head (comments), then #!only, then a line each
export function writeList(lst, head = []) {
  const out = head.map((h) => `# ${h}`.trimEnd());
  if (lst.only) out.push(ONLY);
  for (const l of lst.lines) {
    const f = [l.path, l.track, l.over ? l.over.start + 1 : '', l.over ? l.over.bars : ''];
    if (l.name) f.push(l.name);
    out.push(f.join(' | ').trimEnd());
  }
  return out.join('\n') + '\n';
}
// what can't go on a line ('|' splits fields, '#' starts a comment): a path or a part name holding one
export const listable = (s) => !/[|#]/.test(s);
// the parts the tool builds from a folder with a list, in F order (mid2clip.py collect). files: Map rel -> readMid's
// parts (in toolOrder); it is { items: [{ rel, part, nparts, over, name (the clip's), line (index, or -1) }], notes:
// [[kind, text]] } (kinds as the tool's: missing, twice, unlisted)
export function selectParts(files, lst) {
  const items = [], notes = [];
  const item = (rel, part, nparts, line, k) => ({ rel, part, nparts, over: line ? line.over : null,
    name: (line && line.name) || nameOf(rel, part.name, part.index, nparts), line: k });
  if (!lst.only) {
    const byKey = new Map();
    lst.lines.forEach((l, k) => byKey.set(`${l.path}\u0000${l.track}`, k));
    for (const [rel, parts] of files) {
      for (const p of parts) {
        const k = [p.name.toLowerCase(), String(p.index), ''].map((t) => byKey.get(`${rel}\u0000${t}`)).find((x) => x !== undefined);
        items.push(item(rel, p, parts.length, k === undefined ? null : lst.lines[k], k === undefined ? -1 : k));
      }
    }
    return { items, notes };
  }
  const seen = new Set();
  lst.lines.forEach((l, k) => {
    const parts = files.get(l.path);
    if (!parts) { notes.push(['missing', `"${l.text}": no ${l.path} in the folder`]); return; }
    const hit = parts.filter((p) => !l.track || p.name.toLowerCase() === l.track || String(p.index) === l.track);
    if (!hit.length) notes.push(['missing', `"${l.text}": ${l.path} has no part "${l.track}"`]);
    for (const p of hit) {
      const key = `${l.path}\u0000${p.index}`;
      if (seen.has(key)) { notes.push(['twice', `"${l.text}": ${l.path} [${p.name || p.index}] is listed above: built once`]); continue; }
      seen.add(key);
      items.push(item(l.path, p, parts.length, l, k));
    }
  });
  for (const [rel, parts] of files) for (const p of parts) {
    if (!seen.has(`${rel}\u0000${p.index}`)) notes.push(['unlisted', rel + (parts.length > 1 ? ` [${p.index}${p.name ? `: ${p.name}` : ''}]` : '')]);
  }
  return { items, notes };
}
// a selected part as the tool will build it: its events and bars (clipOf with the line's loop, or the tool's own)
export const itemClip = (it) => clipOf(it.part.notes, it.over, it.part.points);
// as a page clip (into a lane, to hear it): the tool's events, its name
export function itemPageClip(it) {
  const c = itemClip(it), first = c.events.find((e) => e[2] && !(e[2] & PT));
  return { bars: c.bars, root: first ? first[1] : null, quant: 0, name: it.name, events: c.events.map(([t, n, v]) => ({ t, n, v })) };
}
