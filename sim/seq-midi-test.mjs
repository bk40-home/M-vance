// SPDX-License-Identifier: GPL-3.0-only
// sim/seq-midi.js against tools/mid2clip.py: random MIDI files (several divisions, running status, note-on 0 as off,
// repeated pitches, pickups, unended notes, names, SysEx and program changes) read and looped by both; the JS must
// give the tool's events exactly. Then the page's trim and writeMid. Then the list (Kris 21 Nov): a folder of files
// and random clips.txt lists, with and without #!only: the JS picks the tool's parts, in its order, with its names,
// loops and events.
//   node sim/seq-midi-test.mjs            (from the repo root: needs python3 for the tool's side)
import * as M from './seq-midi.js';
import * as S from './seq-model.js';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';

let checks = 0, fails = 0;
const check = (ok, what) => { checks++; if (!ok) { fails++; console.log('FAIL', what); } };
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

// ---- a random MIDI file ----
let seed = 12345;
const rnd = (n) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
const vlq = (v) => { const o = [v & 127]; while ((v = Math.floor(v / 128))) o.unshift((v & 127) | 128); return o; };
function track(div, name, kind) {
  const ev = [];                           // [tick, bytes]
  const q = div / 4, bars = 1 + rnd(kind === 'long' ? 40 : 9), pat = 1 + rnd(4), pick = rnd(3) === 0;
  const cell = Array.from({ length: pat * 16 }, () => (rnd(3) ? null : { n: 36 + rnd(30), d: 1 + rnd(6), v: 30 + rnd(97) }));
  const off0 = rnd(3) * 4 * div;           // (some parts start a bar or two in)
  for (let s = 0; s < bars * 16; s++) {
    const c = kind === 'random' ? (rnd(3) ? null : { n: 36 + rnd(30), d: 1 + rnd(6), v: 30 + rnd(97) }) : cell[s % cell.length];
    if (!c) continue;
    const t = off0 + s * q;
    ev.push([t, [0x90, c.n, c.v]]);
    if (rnd(25)) ev.push([t + c.d * q - (rnd(2) ? 1 : 0), rnd(2) ? [0x80, c.n, 64] : [0x90, c.n, 0]]);   // (one in 25 left on)
  }
  if (pick) ev.push([Math.max(0, off0 - q), [0x90, 60, 90]], [off0 + q, [0x80, 60, 0]]);
  // automation: a volume set once (dropped: a setting), mod wheel (not the sound's), a cutoff sweep, a reso wobble
  ev.push([0, [0xb0, 7, 100]]);
  if (rnd(2)) for (let k = 0; k < 40; k++) ev.push([off0 + k * (rnd(3) + 1) * 3, [0xb0, 1, k]]);
  if (rnd(3)) for (let k = 0; k < 60 + rnd(60); k++) ev.push([off0 + k * (1 + rnd(9)), [0xb0, 74, (k * 3 + rnd(4)) & 127]]);
  if (rnd(2)) for (let k = 0; k < 20; k++) ev.push([off0 + k * q * 2, [0xb0, 71, k % 2 ? 90 : 20]]);
  if (rnd(2)) ev.push([0, [0xc0, 5]]);
  if (rnd(3) === 0) ev.push([q, [0xf0, 3, 0x7e, 0x01, 0xf7]]);
  ev.sort((a, b) => a[0] - b[0]);
  const out = [];
  if (name) out.push(0, 0xff, 3, ...vlq(name.length), ...Array.from(name, (c) => c.charCodeAt(0)));
  let last = 0, rs = 0;
  for (const [t, by] of ev) {
    out.push(...vlq(t - last)); last = t;
    if (by[0] === 0xf0) { out.push(0xf0, ...vlq(by.length - 1), ...by.slice(1)); rs = 0; continue; }
    if (by[0] === rs && rnd(2)) out.push(...by.slice(1)); else { out.push(...by); rs = by[0]; }   // running status
  }
  out.push(0, 0xff, 0x2f, 0);
  return [0x4d, 0x54, 0x72, 0x6b, (out.length >>> 24) & 255, (out.length >>> 16) & 255, (out.length >>> 8) & 255, out.length & 255, ...out];
}
function file(k) {
  const div = [96, 480, 192, 120, 96][k % 5], n = 1 + rnd(3), kinds = ['pattern', 'random', 'long', 'pattern'];
  const trs = [];
  if (n > 1) trs.push(track(div, 'tempo', 'empty').slice(0, 8).concat([]));   // (a conductor track: no notes)
  for (let i = 0; i < n; i++) trs.push(track(div, rnd(2) ? `Part ${i + 1}` : '', kinds[rnd(4)]));
  const fix = (t) => (t.length === 8 ? [0x4d, 0x54, 0x72, 0x6b, 0, 0, 0, 4, 0, 0xff, 0x2f, 0] : t);
  const all = trs.map(fix);
  return new Uint8Array([0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 1, 0, all.length, div >> 8, div & 255, ...all.flat()]);
}

// ---- the tool's view of each file (python3 tools/mid2clip.py's own functions) ----
const dir = mkdtempSync(join(tmpdir(), 'seqmid-'));
const N = 80, files = [];
for (let k = 0; k < N; k++) { const f = join(dir, `f${k}.mid`); const b = file(k); writeFileSync(f, b); files.push([f, b]); }
const py = `
import json, sys
sys.path.insert(0, 'tools')
import mid2clip as m
out = []
for p in sys.argv[1:]:
    div, tracks = m.read_mid(p)
    parts = [(nm, ev) for nm, ev in tracks if any(e[1] for e in ev)]
    r = []
    ccs = m.read_ccs(p)
    snd = m.sound_ccs()
    for (nm, ev), cc in [(t, c) for t, c in zip(tracks, ccs) if any(e[1] for e in t[1])]:
        notes = m.notes_of(ev, div)
        if not notes: continue
        pts = m.points_of(cc, div, snd)
        bars, events, kept, span, how = m.clip_of(notes, None, pts)
        o = m.clip_of(notes, (1, 3), pts)
        r.append({'name': nm, 'notes': notes, 'points': pts, 'bars': bars, 'events': events, 'span': span, 'how': how,
                  'over': [o[0], o[1]]})
    out.append(r)
print(json.dumps(out))`;
const tool = JSON.parse(execFileSync('python3', ['-c', py, ...files.map((f) => f[0])], { maxBuffer: 1 << 28 }).toString());
let parts = 0, valid = 0, rep = 0, withPts = 0;
files.forEach(([, b], k) => {
  const js = M.readMid(b), t = tool[k];
  check(js.length === t.length, `f${k}: ${t.length} parts`);
  js.forEach((p, i) => {
    const T = t[i]; if (!T) return;
    parts++;
    check(p.name === T.name && same(p.notes, T.notes), `f${k}.${i}: notes as the tool reads them`);
    check(same(p.points, T.points), `f${k}.${i}: automation points as the tool makes them (${T.points.length})`);
    if (T.points.length) withPts++;
    const c = M.clipOf(p.notes, null, p.points), L = M.autoLoop(p.notes);
    check(c.bars === T.bars && L.span === T.span && L.how === T.how && same(c.events, T.events), `f${k}.${i}: the loop and events as the tool (${T.how} ${T.span}->${T.bars})`);
    if (T.how === 'repeat') rep++;
    const o = M.clipOf(p.notes, { start: 1, bars: 3 }, p.points);
    check(o.bars === T.over[0] && same(o.events, T.over[1]), `f${k}.${i}: a clips.txt override (bar 2, 3 bars) as the tool`);
    const tc = M.toolClip(p.notes, '', p.points);
    // (at one tick, events of different pitches may come in either order: the same to the sequencer)
    const rank = (e) => (e.v & S.PT ? 1 : e.v ? 2 : 0);
    const canon = (ev) => ev.slice().sort((x, y) => x.t - y.t || rank(x) - rank(y) || x.n - y.n);
    if (!S.validate(tc)) {
      valid++;
      check(same(S.withPoints(S.eventsOf(S.notesOf(tc), tc.bars), S.pointsOf(tc), tc.bars), canon(tc.events)), `f${k}.${i}: the tool's clip survives notes + points -> events`);
      check(same(M.trim(p.notes, { start: L.start, bars: L.bars }, p.points).events, canon(tc.events)), `f${k}.${i}: trim at the auto loop = the tool's clip`);
    }
  });
});
check(parts > 100 && rep > 10 && withPts > 30, `coverage: ${parts} parts, ${rep} repeats found, ${withPts} with automation, ${valid} fit a track as they are`);
console.log(`coverage: ${parts} parts, ${rep} repeats, ${withPts} with automation, ${valid} fit a track`);

// ---- trim, transpose, quantize ----
const notes = [[384, 432, 60, 100], [480, 500, 62, 90], [0, 48, 59, 80], [1600, 1650, 64, 70]];   // a pickup, bar 2.., bar 5
let c = M.trim(notes, { start: 1, bars: 2 });
check(same(c.events.filter((e) => e.v).map((e) => [e.t, e.n]), [[0, 60], [96, 62]]) && c.root === 60, 'trim: bars 2-3, the rest dropped');
c = M.trim(notes, { start: 1, bars: 2, fold: true });
check(c.events.filter((e) => e.v).length === 4 && !S.validate(c), 'trim fold: all four, inside the loop');
c = M.trim(notes, { start: 1, bars: 2, transpose: 12 });
check(c.root === 72, 'transpose +12');
c = M.trim([[384 + 10, 384 + 40, 60, 100], [384 + 50, 384 + 70, 61, 100]], { start: 1, bars: 1, quant: 1 });
check(same(c.events.filter((e) => e.v).map((e) => e.t), [0, 48]), 'quantize 1/16: 10 -> 0, 50 -> 48');
c = M.trim([[0, 30, 127, 100]], { start: 0, bars: 1, transpose: 5 });
check(c.events.length === 0, 'transposed off the keyboard: dropped');

// ---- points: the wire, the model ----
{
  const c = { bars: 1, root: 60, quant: 0, name: '', events: S.withPoints(S.eventsOf([{ t: 0, len: 48, n: 60, v: 100 }], 1), [{ t: 0, cc: 74, v: 10 }, { t: 200, cc: 74, v: 99 }, { t: 383, cc: 71, v: 5 }], 1) };
  check(S.validate(c) === null && S.noteCount(c) === 1 && S.pointCount(c) === 3 && c.events[0].v === (S.PT | 10) && c.events[1].v === 100, 'points: valid, not notes, after the offs and before the ons');
  check(c.events.find((e) => e.n === 71).t === 380, 'a point snapped down onto the 4-tick grid');
  const got = [];
  const rd = new S.SeqReader({ onClip: (tt, x) => got.push(x) });
  rd.expect(0); const ms = S.writeMsgs(0, c); [ms.at(-1), ...ms.slice(0, -1)].forEach((m) => rd.feed(m));
  check(got.length === 1 && same(got[0].events, c.events), 'points round-trip the wire');
  check(same(S.notesOf(c).length, 1) && same(S.pointsOf(c).map((p) => p.v), [10, 99, 5]), 'notesOf leaves points out; pointsOf has them');
}

// ---- writeMid -> readMid ----
const W = M.trim(notes, { start: 0, bars: 8, name: 'X' }, [[0, 74, 10], [96, 74, 60], [400, 74, 120]]);
const back = M.readMid(M.writeMid(W, 'X'));
check(back.length === 1 && same(back[0].points, [[0, 74, 10], [96, 74, 60], [400, 74, 120]]), 'writeMid -> readMid: the points as CCs and back');
check(back.length === 1 && back[0].name === 'X' && same(back[0].notes, S.notesOf(W).map((x) => [x.t, x.t + x.len, x.n, x.v])), 'writeMid -> readMid: the same notes');
{
  const rank = (e) => (e.v & S.PT ? 1 : e.v ? 2 : 0);
  const canon = (ev) => ev.slice().sort((x, y) => x.t - y.t || rank(x) - rank(y) || x.n - y.n);
  check(same(canon(M.clipOf(back[0].notes, { start: 0, bars: 8 }, back[0].points).events.map(([t, n, v]) => ({ t, n, v }))), W.events), "writeMid -> the tool's override: the same clip");
}

// ---- the list: the page's selectParts against the tool's collect (Kris 21 Nov) ----
{
  const root = mkdtempSync(join(tmpdir(), 'seqlist-'));
  const rels = ['a - Song (remix).mid', 'b.MID', 'Z.mid', 'sub/x.mid', 'sub dir/y.mid', 'sub/deeper/z.mid', 'sub/a - b - c.mid',
    'Long Folder Name Here/lead.mid', 'Long Folder Name Here/long folder name here bass.mid', 'two parts.mid', 'notes.txt'];
  const files = new Map();
  rels.forEach((r, k) => {
    const p = join(root, r); mkdirSync(dirname(p), { recursive: true });
    if (r.endsWith('.txt')) { writeFileSync(p, 'not midi'); return; }
    const b = file(k * 7 + 3); writeFileSync(p, b);
    files.set(r, M.readMid(b));
  });
  const order = M.toolOrder(rels), F = new Map(order.map((r) => [r, files.get(r)]));
  check(!order.includes('notes.txt') && order.length === rels.length - 1, `toolOrder: the .mid files (${order.length})`);
  const listPath = join(root, '..', `list-${process.pid}.txt`);
  const tool = (txt) => {
    writeFileSync(listPath, txt);
    const py2 = `
import json, sys
sys.path.insert(0, 'tools')
import mid2clip as m
notes = []
c = m.collect(sys.argv[1], m.read_list(sys.argv[2]), notes)
print(json.dumps({'clips': [[x['src'], x['name'], x['bars'], x['how'], x['events']] for x in c], 'kinds': [w for w, _t in notes]}))`;
    return JSON.parse(execFileSync('python3', ['-c', py2, root, listPath], { maxBuffer: 1 << 28 }).toString());
  };
  const mine = (txt) => {
    const sel = M.selectParts(F, M.parseList(txt));
    return { clips: sel.items.map((it) => { const c = M.itemClip(it); return [it.rel + (it.nparts > 1 ? ` [${it.part.name || it.part.index}]` : ''), it.name, c.bars, it.over ? 'override' : M.autoLoop(it.part.notes).how, c.events]; }),
      kinds: sel.notes.map((n) => n[0]) };
  };
  const cmpRun = (txt, what) => {
    const t = tool(txt), j = mine(txt);
    check(t.clips.length === j.clips.length, `${what}: ${t.clips.length} clips (page ${j.clips.length})`);
    t.clips.forEach((c, i) => check(same(c, j.clips[i]), `${what}: F${i + 1} ${c[0]} ${c[1]}: page ${j.clips[i] && j.clips[i].slice(0, 4)}`));
    check(same(t.kinds, j.kinds), `${what}: the notes (${t.kinds.join(',')} / ${j.kinds.join(',')})`);
    return t;
  };
  const all = cmpRun('', 'no list');
  check(all.clips.length > 15, `the folder: ${all.clips.length} parts`);
  // random lists: parts by name, by number or the whole file; a loop or not; a name or not; a missing file and part
  for (let run = 0; run < 12; run++) {
    const lst = { only: run % 3 !== 0, lines: [] };
    for (const [rel, parts] of F) for (const p of parts) {
      if (rnd(3) === 0) continue;
      const how = rnd(4), track = how === 0 && p.name ? p.name : how === 1 ? '' : String(p.index);
      lst.lines.push({ path: rel, track: track.toLowerCase(), over: rnd(3) ? null : { start: rnd(3), bars: 1 + rnd(8) },
        name: rnd(3) ? null : M.cleanName(`clip ${run}-${p.index}`), text: '' });
    }
    for (let i = lst.lines.length - 1; i > 0; i--) { const j = rnd(i + 1); [lst.lines[i], lst.lines[j]] = [lst.lines[j], lst.lines[i]]; }
    lst.lines.push({ path: 'gone.mid', track: '', over: null, name: null }, { path: order[0], track: 'no such part', over: null, name: null });
    const txt = M.writeList(lst, [`random list ${run}`]);
    check(same(M.parseList(txt).lines.map((l) => [l.path, l.track, l.over, l.name]), lst.lines.map((l) => [l.path, l.track, l.over, l.name])), `list ${run}: writeList -> parseList`);
    cmpRun(txt, `list ${run}${lst.only ? ' (#!only)' : ''}`);
  }
  for (const bad of ['a.mid | | 1 |', 'a.mid | | 1', 'a.mid | | | | 12345678901234567', 'a.mid | | 0 | 4', 'a.mid | | x | 4']) {
    let threw = false; try { M.parseList(bad); } catch { threw = true; }
    check(threw, `parseList refuses "${bad}" (as the tool)`);
  }
  check(M.listable('a - b.mid') && !M.listable('x | y') && !M.listable('take #2'), 'listable: | and # cannot go on a line');
}

console.log(`seq-midi: ${checks} checks, ${fails} failed`);
process.exit(fails ? 1 : 0);
