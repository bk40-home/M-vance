// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance sequences on the web: the SysEx of the sequencer's area 04 (firmware/src/mv_seqx.h) and the clip model the
// editor page edits (Kris 20 Nov 2026: fm1-port/mvance-seq-editor-proposal-2026-11-20.md, "go with recommended").
// Pure functions and one small class, no DOM: node tests it (sim/seq-model-test.mjs).
//
//   a clip     { bars 1..32, root 0..126 | null, quant 0..4, name (6 characters, a user clip's), events }
//   an event   { t: tick in the loop (96 a quarter), n: note 0..127, v: velocity 1..127, 0 = note-off }
//              kept in the track's order: by tick; at the same tick a note-off before a note-on (so a note that
//              ends where the next of its pitch starts stays one off, one on). A note held over the loop's end has
//              its note-off at the start (round the loop), as the FM-1 records it.
//   a note     { t, len, n, v } the roll's view of an on / off pair (notesOf / eventsOf convert)
//   a point    automation (Kris 20 Nov: mv_seq.h AUTOMATION): an event { t, n: its CC, v: 0x80 | value } (PT set).
//              On the 4-tick grid (PT_GRID); never a note (notesOf, noteCount, ROOT leave it out); shares the 256.
//              The roll edits notes, the automation lane points (pointsOf / withPoints convert); on the wire a point
//              is 60 | t >> 9, (t >> 2) & 7F, CC, value
//   tt         what an address names: 00 / 01 the live track UPPER / LOWER, 10 + k user clip k + 1, 20 + k factory
//              F(k + 1)
// Writes are events first (32 a message), then the head, which commits them: a message is 140 bytes, under the
// FM-1's one-message USB buffer; send them GAP_MS apart (fm1-midi.js) as every other transfer.

export const PPQN = 96;                   // ticks a quarter (mv_seq.h MV_SEQ_PPQN)
export const BAR = 4 * PPQN;              // ticks a bar (4/4)
export const MAXBAR = 32;                 // LENGTH: 1..32 bars
export const MAXEV = 256;                 // events a track or a user clip holds (128 notes)
export const NUSER = 12;                  // user clips (mv_clip.h MV_CLIP_N)
export const HEAD = 32;                   // the head's bytes; events start at this offset
export const CHUNK = 32;                  // events a message
export const FL = { FLASH: 1, RO: 2, EMPTY: 4 };   // head flags
export const QUANTS = ['OFF', '1/16', '1/8', '1/16T', '1/8T'];   // QUANT (MV_SQQ_*)
export const QGRID = [0, PPQN / 4, PPQN / 2, PPQN / 6, PPQN / 3]; // their grids in ticks
export const NOROOT = 0x7f;               // a head's ROOT: none
export const PT = 0x80;                   // an event's velocity flag: an automation point (mv_seq.h MV_SEQ_PT)
export const PT_GRID = 4;                 // a point's tick: a multiple of this
export const isPoint = (e) => (e.v & PT) !== 0;

export const TT = {
  track: (part) => part & 1,              // 0 UPPER, 1 LOWER
  user: (k) => 0x10 + k,                  // k 0..11
  fac: (k) => 0x20 + k,                   // k 0..95
};
export const ttName = (tt) => (tt < 2 ? ['UPPER', 'LOWER'][tt] : tt < 0x20 ? `user ${tt - 0x0f}` : `F${tt - 0x1f}`);

// ---- Roland framing (device 10h, model 00 06): the same as bank-model.js / fm1-midi.js ----
const rsum = (bytes) => { let s = 0; for (const b of bytes) s += b; return (128 - (s % 128)) % 128; };
export const addrOf = (tt, off) => [0x04, tt & 0x7f, (off >> 7) & 0x7f, off & 0x7f];
export function dt1(addr, data) {
  const body = [...addr, ...data.map((b) => b & 0x7f)];
  return new Uint8Array([0xf0, 0x41, 0x10, 0x00, 0x06, 0x12, ...body, rsum(body), 0xf7]);
}
export function rq1(addr, size) {
  const body = [...addr, ...size];
  return new Uint8Array([0xf0, 0x41, 0x10, 0x00, 0x06, 0x11, ...body, rsum(body), 0xf7]);
}
// a read of tt: the whole clip, or (head) only its head (a directory)
export const readMsg = (tt, head = false) => rq1(addrOf(tt, 0), head ? [0, 0, 0, HEAD] : [0, 0, 0, 0]);
// a DT1 of area 04 from the device: { tt, off, data } (else null; a bad sum: null)
export function seqMsg(m) {
  if (!m || m.length < 12 || m[0] !== 0xf0 || m[1] !== 0x41 || m[4] !== 0x06 || m[5] !== 0x12 || m[6] !== 0x04) return null;
  if (rsum(Array.from(m.subarray(6, m.length - 2))) !== m[m.length - 2]) return null;
  return { tt: m[7], off: (m[8] << 7) | m[9], data: Array.from(m.subarray(10, m.length - 2)) };
}

// ---- the clip ----
export const emptyClip = (bars = 2) => ({ bars, root: null, quant: 1, name: '', events: [] });
export const clipCopy = (c) => ({ ...c, events: c.events.map((e) => ({ ...e })) });
export const clipEqual = (a, b) => !!a && !!b && a.bars === b.bars && a.root === b.root && a.quant === b.quant
  && a.events.length === b.events.length && a.events.every((e, i) => e.t === b.events[i].t && e.n === b.events[i].n && e.v === b.events[i].v);
export const noteCount = (c) => c.events.filter((e) => e.v && !(e.v & PT)).length;
export const pointCount = (c) => c.events.filter(isPoint).length;
const ascii = (s, n) => Array.from((s + ' '.repeat(n)).slice(0, n), (ch) => { const v = ch.charCodeAt(0); return v >= 32 && v <= 125 ? v : 32; });

export function headBytes(c) {
  const n = c.events.length, h = new Array(HEAD).fill(0);
  h[0] = 0x43; h[1] = 2; h[2] = c.bars; h[3] = c.root == null ? NOROOT : c.root; h[4] = c.quant | 0;
  h[6] = n & 0x7f; h[7] = (n >> 7) & 0x7f;
  ascii(c.name || '', 6).forEach((v, i) => { h[8 + i] = v; });
  return h;
}
export function parseHead(d) {
  const name16 = String.fromCharCode(...d.slice(16, 32)).replace(/\s+$/, '');
  return {
    bars: d[2], root: d[3] === NOROOT ? null : d[3], quant: d[4], flags: d[5], n: d[6] | (d[7] << 7),
    ver: d[14] | (d[15] << 7), name: (d[5] & FL.RO ? name16 : String.fromCharCode(...d.slice(8, 14)).replace(/\s+$/, '')),
  };
}

// the messages that write clip c to tt: events (32 a message), then the head
export function writeMsgs(tt, c) {
  const out = [];
  for (let i = 0; i < c.events.length; i += CHUNK) {
    const data = [];
    for (const e of c.events.slice(i, i + CHUNK)) {
      if (e.v & PT) data.push(0x60 | ((e.t >> 9) & 0x1f), (e.t >> 2) & 0x7f, e.n, e.v & 0x7f);   // a point
      else data.push((e.t >> 7) & 0x7f, e.t & 0x7f, e.n, e.v);
    }
    out.push(dt1(addrOf(tt, HEAD + 4 * i), data));
  }
  out.push(dt1(addrOf(tt, 0), headBytes(c)));
  return out;
}

// why the FM-1 would refuse clip c (as mv_seqx.c sx_events_ok and the head's checks), or null: it is good
export function validate(c) {
  const len = c.bars * BAR;
  if (!(c.bars >= 1 && c.bars <= MAXBAR)) return `LENGTH ${c.bars}: 1 to ${MAXBAR} bars`;
  if (c.events.length > MAXEV) return pointCount(c)
    ? `${noteCount(c)} notes and ${pointCount(c)} points: ${c.events.length} events, a track holds ${MAXEV} (trim it, or thin the automation)`
    : `${noteCount(c)} notes: a track holds ${MAXEV / 2} (trim it)`;
  const on = new Map(); let last = 0;
  for (const e of c.events) {
    if (e.t < 0 || e.t >= len) return `a note past the loop's end (bar ${Math.floor(e.t / BAR) + 1})`;
    if (e.t < last) return 'the events are not in time order';
    if (!(e.n >= 0 && e.n <= 127 && e.v >= 0 && e.v <= 255)) return 'a note or velocity out of range';
    last = e.t;
    if (e.v & PT) { if (e.t % PT_GRID) return 'an automation point off the 4-tick grid'; continue; }
    const s = on.get(e.n);
    if (s && s.on === !!e.v) return `note ${e.n}: two ${e.v ? 'note-ons' : 'note-offs'} in a row`;
    on.set(e.n, { on: !!e.v, k: (s ? s.k : 0) + 1 });
  }
  for (const [n, s] of on) if (s.k & 1) return `note ${n}: a note-on without its note-off`;
  return null;
}

// ---- notes <-> events ----
// the notes of a clip: each note-on with the next note-off of its pitch round the loop
export function notesOf(c) {
  const len = c.bars * BAR, byPitch = new Map(), notes = [];
  c.events.forEach((e, i) => { if (e.v & PT) return; if (!byPitch.has(e.n)) byPitch.set(e.n, []); byPitch.get(e.n).push(i); });
  for (const [, idx] of byPitch) {
    for (let j = 0; j < idx.length; j++) {
      const e = c.events[idx[j]];
      if (!e.v) continue;
      let off = null;
      for (let k = 1; k < idx.length && off == null; k++) { const f = c.events[idx[(j + k) % idx.length]]; if (!f.v) off = f; }
      const end = off ? off.t : e.t + 1;
      notes.push({ t: e.t, len: ((end - e.t + len - 1) % len) + 1, n: e.n, v: e.v });
    }
  }
  return notes.sort((a, b) => a.t - b.t || a.n - b.n);
}
// a clip's events from notes (any order, overlaps allowed): each note is cut where the next of its pitch starts
// (round the loop), so every pitch alternates on / off as the FM-1 needs; a note is at least a tick and shorter than
// the loop
export function eventsOf(notes, bars) {
  const len = bars * BAR, byPitch = new Map(), ev = [];
  for (const x of notes) {
    const t = ((Math.round(x.t) % len) + len) % len;
    if (!byPitch.has(x.n)) byPitch.set(x.n, new Map());
    const m = byPitch.get(x.n);                      // (two notes of a pitch at one tick: the longer)
    if (!m.has(t) || m.get(t).len < x.len) m.set(t, { t, len: Math.round(x.len), n: x.n, v: x.v });
  }
  for (const [, m] of byPitch) {
    const list = [...m.values()].sort((a, b) => a.t - b.t);
    list.forEach((x, i) => {
      const next = list.length > 1 ? list[(i + 1) % list.length].t + (i + 1 === list.length ? len : 0) : x.t + len;
      const l = Math.max(1, Math.min(x.len, next - x.t, len - 1));
      ev.push({ t: x.t, n: x.n, v: Math.max(1, Math.min(127, x.v | 0)) }, { t: (x.t + l) % len, n: x.n, v: 0 });
    });
  }
  return ev.sort((a, b) => a.t - b.t || (a.v ? 1 : 0) - (b.v ? 1 : 0) || a.n - b.n);
}

// ---- points <-> events ----
// a clip's automation: [{ t, cc, v (0..127) }] in order
export const pointsOf = (c) => c.events.filter(isPoint).map((e) => ({ t: e.t, cc: e.n, v: e.v & 0x7f }));
// note events (eventsOf's) and points merged in the track's order: at a tick, note-offs, then points, then note-ons;
// points snapped down onto the grid and inside the loop, one a CC a tick (the later wins)
export function withPoints(noteEvents, points, bars) {
  const len = bars * BAR, m = new Map();
  for (const p of points) {
    const t = Math.floor((((Math.round(p.t) % len) + len) % len) / PT_GRID) * PT_GRID;
    m.set(`${t}:${p.cc}`, { t, n: p.cc & 0x7f, v: PT | Math.max(0, Math.min(127, p.v | 0)) });
  }
  const rank = (e) => (e.v & PT ? 1 : e.v ? 2 : 0);
  return [...noteEvents.filter((e) => !(e.v & PT)), ...m.values()].sort((a, b) => a.t - b.t || rank(a) - rank(b) || a.n - b.n);
}

// ---- reading: the messages from the device into clips and heads ----
// expect(tt): a whole read was asked for (its head is followed by its events); any other head (a directory read, a
// linked report) goes to onHead. onClip(tt, clip, head) when a whole read is in
export class SeqReader {
  constructor({ onClip = () => {}, onHead = () => {} } = {}) {
    Object.assign(this, { onClip, onHead });
    this.want = new Map();                // tt -> { head, events } of a whole read in progress (head null: not yet)
  }
  expect(tt) { this.want.set(tt, { head: null, events: [] }); }
  pending(tt) { return this.want.has(tt); }
  feed(m) {
    const s = seqMsg(m);
    if (!s) return false;
    const w = this.want.get(s.tt);
    if (s.off === 0) {
      const h = parseHead(s.data);
      if (w) { w.head = h; w.events = []; if (h.n === 0) this.done(s.tt, w); }   // (a new head: the read began again)
      else this.onHead(s.tt, h);
      return true;
    }
    if (!w || !w.head || s.off !== HEAD + 4 * w.events.length) return true;   // (not ours, or out of step: ignored)
    for (let i = 0; i + 3 < s.data.length; i += 4) {
      const d = s.data;
      if (d[i] >= 0x60) w.events.push({ t: ((d[i] & 0x1f) << 9) | (d[i + 1] << 2), n: d[i + 2], v: PT | d[i + 3] });   // a point
      else w.events.push({ t: (d[i] << 7) | d[i + 1], n: d[i + 2], v: d[i + 3] });
    }
    if (w.events.length >= w.head.n) this.done(s.tt, w);
    return true;
  }
  done(tt, w) {
    this.want.delete(tt);
    const h = w.head;
    this.onClip(tt, { bars: h.bars, root: h.root, quant: h.quant, name: h.name, events: w.events.slice(0, h.n) }, h);
  }
}
