// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance piano roll for the editor page's Sequences panel (Kris 20 Nov 2026: fm1-port/mvance-seq-editor-proposal-
// 2026-11-20.md, 1a and 5a). Draws one clip: a key strip with the C names, a row per note (black keys shaded), bars
// and beats, the notes as bars (brighter = louder; selected ones outlined). A note held over the loop's end is drawn
// in two pieces. The rows fit the notes (2 octaves at least) until the wheel scrolls them (Fit goes back).
//
// Editing (5a), on the canvas (it takes the keyboard focus when clicked):
//   click on nothing      a note there (the grid's length, velocity 100)      drag on nothing   select a box
//   click a note          select it (shift: add / remove)                     drag a note       move the selection
//   drag a note's end     resize the selection                                alt + drag        copy the selection
//   wheel over a note     velocity (the selection, if the note is in it)      wheel elsewhere   scroll the rows
//   Delete / Backspace    delete the selection   Ctrl+A all   arrows: left / right a grid step, up / down a semitone
//   (shift: an octave)    Ctrl+Z undo   Ctrl+Y / Ctrl+Shift+Z redo
// Moves snap to the grid (opts.grid() ticks; 1 = free) and go round the loop. The roll never changes the clip itself:
// each finished gesture calls opts.onEdit(notes, what) with the new notes (seq-model.js notesOf's form), and the panel
// keeps the clip and the undo.
import { BAR, PPQN, notesOf } from './seq-model.js?v=20261121150209';

const KEYW = 34;                           // the key strip's width (CSS px)
const MINROWS = 24;                        // rows shown at least (2 octaves)
const EDGE = 6;                            // px at a note's end that resize it
const BLACK = new Set([1, 3, 6, 8, 10]);   // semitones of the black keys
const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
export const noteName = (n) => `${NAMES[n % 12]}${Math.floor(n / 12) - 1}`;

export class Roll {
  // opts: onEdit(notes, what), onKey(name) ('undo' | 'redo'), grid() ticks a step
  constructor(canvas, opts = {}) {
    this.cv = canvas;
    this.opts = { onEdit: () => {}, onKey: () => {}, grid: () => PPQN / 4, ...opts };
    this.lo = 48; this.hi = 72;            // the rows shown: lo .. hi - 1
    this.manual = false;                   // the rows scrolled by hand (not fitted)
    this.clip = null; this.notes = []; this.sel = new Set();   // what is drawn; selected notes (indices)
    this.drag = null;                      // a gesture in progress
    this.preview = null;                   // the notes as the gesture would leave them
    if (opts.onEdit) this.listen();
  }
  // ---- geometry (of the last draw) ----
  geo() {
    const W = this.cv.clientWidth || 600, H = this.cv.clientHeight || 180, len = (this.clip ? this.clip.bars : 1) * BAR;
    return { W, H, len, xs: (W - KEYW) / len, rh: H / (this.hi - this.lo) };
  }
  xToTick(x) { const g = this.geo(); return Math.max(0, Math.min(g.len - 1, (x - KEYW) / g.xs)); }
  yToNote(y) { const g = this.geo(); return Math.max(0, Math.min(127, this.hi - 1 - Math.floor(y / g.rh))); }
  // the note under (x, y) and whether at its end: { i, end } or null
  hit(x, y) {
    const g = this.geo(), n = this.yToNote(y), t = (x - KEYW) / g.xs;
    for (let i = this.notes.length - 1; i >= 0; i--) {
      const a = this.notes[i];
      if (a.n !== n) continue;
      const e = a.t + a.len, pieces = e > g.len ? [[a.t, g.len, false], [0, e - g.len, true]] : [[a.t, e, true]];
      for (const [s, f, last] of pieces) if (t >= s && t < f) return { i, end: last && (f - t) * g.xs <= EDGE };
    }
    return null;
  }
  // the theme's colours, read each draw (the theme can change)
  colours() {
    const cs = getComputedStyle(this.cv), v = (k, d) => (cs.getPropertyValue(k).trim() || d);
    return { bg: v('--bg', '#111'), surf: v('--surf', '#1b1b1d'), raised: v('--raised', '#26262a'), line: v('--line', '#333'),
             dim: v('--dim', '#999'), text: v('--text', '#ddd'), theme: v('--theme', '#e8a33d') };
  }
  fit(notes) {                             // the rows for notes (centred on them, at least MINROWS)
    if (this.manual) return;
    if (!notes.length) { this.lo = 48; this.hi = 72; return; }
    let lo = Math.min(...notes.map((x) => x.n)), hi = Math.max(...notes.map((x) => x.n)) + 1;
    const extra = Math.max(0, MINROWS - (hi - lo));
    lo = Math.max(0, lo - Math.ceil(extra / 2) - 1); hi = Math.min(128, hi + Math.floor(extra / 2) + 1);
    this.lo = lo; this.hi = hi;
  }
  refit() { this.manual = false; this.draw(this.clip); }
  // a new clip to show (the panel's): the selection kept if the notes are the same count, else dropped
  draw(clip) {
    if (clip !== this.clip) {
      const nn = clip ? notesOf(clip) : [];
      if (this.keepSel) { this.sel = new Set(); nn.forEach((x, i) => { if (this.keepSel.has(`${x.t}:${x.n}`)) this.sel.add(i); }); this.keepSel = null; }
      else if (!clip || !this.clip || nn.length !== this.notes.length) this.sel = new Set();
      this.clip = clip; this.notes = nn;
      if (!this.drag) this.fit(nn);
    }
    this.paint();
  }
  paint() {
    const cv = this.cv, dpr = (typeof window !== 'undefined' && window.devicePixelRatio) || 1;
    const { W, H, len, xs, rh } = this.geo();
    if (cv.width !== Math.round(W * dpr) || cv.height !== Math.round(H * dpr)) { cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr); }
    const g = cv.getContext('2d'), c = this.colours(), clip = this.clip;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.fillStyle = c.bg; g.fillRect(0, 0, W, H);
    const yOf = (n) => (this.hi - 1 - n) * rh;
    for (let n = this.lo; n < this.hi; n++) {   // the rows and the key strip
      const black = BLACK.has(n % 12);
      g.fillStyle = black ? c.surf : c.raised; g.fillRect(KEYW, yOf(n), W - KEYW, rh);
      g.fillStyle = black ? '#222' : '#ccc'; g.fillRect(0, yOf(n), KEYW - 2, rh - (rh > 3 ? 0.5 : 0));
      if (n % 12 === 0 && rh >= 7) { g.fillStyle = '#333'; g.font = `${Math.min(10, rh)}px ui-monospace,monospace`; g.textBaseline = 'middle'; g.fillText(noteName(n), 2, yOf(n) + rh / 2); }
    }
    if (!clip) return;
    for (let t = 0; t <= len; t += PPQN) {  // beats and bars
      g.fillStyle = t % BAR ? c.line : c.dim; g.globalAlpha = t % BAR ? 0.5 : 0.9;
      g.fillRect(KEYW + t * xs, 0, 1, H);
    }
    g.globalAlpha = 1;
    const notes = this.preview || this.notes;
    notes.forEach((x, i) => {              // the notes (over the loop's end: two pieces)
      const y = yOf(x.n) + 0.5, h = Math.max(1, rh - 1), e = x.t + x.len;
      const pieces = e > len ? [[x.t, len], [0, e - len]] : [[x.t, e]];
      for (const [s, f] of pieces) {
        g.fillStyle = c.theme; g.globalAlpha = 0.45 + 0.55 * (x.v / 127);
        g.fillRect(KEYW + s * xs, y, Math.max(2, (f - s) * xs), h);
        g.globalAlpha = 1;
        if (this.sel.has(i)) { g.strokeStyle = c.text; g.lineWidth = 1; g.strokeRect(KEYW + s * xs + 0.5, y, Math.max(2, (f - s) * xs) - 1, h); }
      }
    });
    if (this.drag && this.drag.mode === 'box' && this.drag.moved) {
      const d = this.drag;
      g.strokeStyle = c.text; g.setLineDash([3, 3]);
      g.strokeRect(Math.min(d.x0, d.x), Math.min(d.y0, d.y), Math.abs(d.x - d.x0), Math.abs(d.y - d.y0));
      g.setLineDash([]);
    }
    if (!notes.length) { g.fillStyle = c.dim; g.font = '12px system-ui'; g.fillText('empty: click to add a note', KEYW + 8, H / 2); }
  }

  // ---- editing ----
  // a finished gesture: the panel makes the clip from notes and draws it back; the selection follows its notes (the
  // clip's notes come back sorted, so it is kept by start and pitch, not by index)
  commit(notes, what) {
    const len = this.clip.bars * BAR;
    this.keepSel = new Set([...this.sel].filter((i) => notes[i]).map((i) => `${((notes[i].t % len) + len) % len}:${notes[i].n}`));
    this.preview = null;
    this.opts.onEdit(notes, what);
  }
  snap(t) { const s = this.opts.grid() || 1; return Math.round(t / s) * s; }
  // the selection moved by dt ticks and dn semitones (copy: added, the originals kept); round the loop
  moved(dt, dn, copy) {
    const len = this.clip.bars * BAR, out = this.notes.map((x) => ({ ...x }));
    const sel = [...this.sel];
    const nsel = new Set();
    for (const i of sel) {
      const x = this.notes[i], y = { ...x, t: (((x.t + dt) % len) + len) % len, n: Math.max(0, Math.min(127, x.n + dn)) };
      if (copy) { nsel.add(out.length); out.push(y); } else { out[i] = y; nsel.add(i); }
    }
    return { notes: out, sel: nsel };
  }
  resized(dt) {
    const len = this.clip.bars * BAR;
    return this.notes.map((x, i) => (this.sel.has(i) ? { ...x, len: Math.max(1, Math.min(len - 1, x.len + dt)) } : x));
  }
  listen() {
    const cv = this.cv;
    cv.tabIndex = 0;
    const pos = (e) => { const r = cv.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
    cv.addEventListener('pointerdown', (e) => {
      if (!this.clip || e.button !== 0) return;
      cv.focus();
      const [x, y] = pos(e), h = this.hit(x, y);
      if (cv.setPointerCapture && e.pointerId != null) try { cv.setPointerCapture(e.pointerId); } catch { /* (jsdom) */ }
      if (h) {
        if (e.shiftKey) { if (this.sel.has(h.i)) this.sel.delete(h.i); else this.sel.add(h.i); this.paint(); return; }
        if (!this.sel.has(h.i)) this.sel = new Set([h.i]);
        this.drag = { mode: h.end ? 'resize' : 'move', copy: e.altKey, x0: x, y0: y, t0: this.xToTick(x), n0: this.yToNote(y), moved: false };
      } else this.drag = { mode: 'box', x0: x, y0: y, x, y, moved: false, add: e.shiftKey };
      this.paint();
    });
    cv.addEventListener('pointermove', (e) => {
      const d = this.drag;
      if (!d) { if (this.clip) { const [x, y] = pos(e), h = this.hit(x, y); cv.style.cursor = h ? (h.end ? 'ew-resize' : 'move') : 'crosshair'; } return; }
      const [x, y] = pos(e);
      if (Math.abs(x - d.x0) + Math.abs(y - d.y0) > 3) d.moved = true;
      if (!d.moved) return;
      if (d.mode === 'box') { d.x = x; d.y = y; }
      else if (d.mode === 'move') { const r = this.moved(this.snap(this.xToTick(x) - d.t0), this.yToNote(y) - d.n0, d.copy); this.preview = r.notes; d.r = r; }
      else this.preview = this.resized(this.snap(this.xToTick(x) - d.t0));
      this.paint();
    });
    const up = (e) => {
      const d = this.drag;
      if (!d) return;
      this.drag = null;
      const [x, y] = pos(e);
      if (d.mode === 'box') {
        if (!d.moved) {                    // a click on nothing: a note there
          const s = this.opts.grid() || PPQN / 4, t = Math.floor(this.xToTick(x) / s) * s;
          const notes = [...this.notes.map((q) => ({ ...q })), { t, len: s, n: this.yToNote(y), v: 100 }];
          this.sel = new Set([notes.length - 1]);
          this.commit(notes, 'add a note');
          return;
        }
        const t0 = this.xToTick(Math.min(d.x0, x)), t1 = this.xToTick(Math.max(d.x0, x));
        const n0 = this.yToNote(Math.max(d.y0, y)), n1 = this.yToNote(Math.min(d.y0, y));
        if (!d.add) this.sel = new Set();
        this.notes.forEach((q, i) => { if (q.n >= n0 && q.n <= n1 && q.t + q.len > t0 && q.t <= t1) this.sel.add(i); });
        this.paint();
        return;
      }
      if (!d.moved) { this.preview = null; this.paint(); return; }
      if (d.mode === 'move') { const r = d.r || { notes: this.notes, sel: this.sel }; this.sel = r.sel; this.commit(r.notes, d.copy ? 'copy notes' : 'move notes'); }
      else this.commit(this.preview || this.notes, 'resize notes');
    };
    cv.addEventListener('pointerup', up);
    cv.addEventListener('pointercancel', () => { this.drag = null; this.preview = null; this.paint(); });
    cv.addEventListener('wheel', (e) => {
      if (!this.clip) return;
      e.preventDefault();
      const [x, y] = pos(e), h = this.hit(x, y), dir = e.deltaY < 0 ? 1 : -1;
      if (h) {                             // velocity: the selection if the note is in it, else that note
        const which = this.sel.has(h.i) ? this.sel : new Set([h.i]);
        this.commit(this.notes.map((q, i) => (which.has(i) ? { ...q, v: Math.max(1, Math.min(127, q.v + 8 * dir)) } : q)), 'velocity');
        return;
      }
      const span = this.hi - this.lo;      // the rows scrolled
      this.lo = Math.max(0, Math.min(128 - span, this.lo + dir)); this.hi = this.lo + span; this.manual = true;
      this.paint();
    }, { passive: false });
    cv.addEventListener('keydown', (e) => {
      if (!this.clip) return;
      const k = e.key, ctrl = e.ctrlKey || e.metaKey;
      if (ctrl && (k === 'z' || k === 'Z')) { e.preventDefault(); this.opts.onKey(e.shiftKey ? 'redo' : 'undo'); return; }
      if (ctrl && (k === 'y' || k === 'Y')) { e.preventDefault(); this.opts.onKey('redo'); return; }
      if (ctrl && (k === 'a' || k === 'A')) { e.preventDefault(); this.sel = new Set(this.notes.map((_, i) => i)); this.paint(); return; }
      if (!this.sel.size) return;
      if (k === 'Delete' || k === 'Backspace') {
        e.preventDefault();
        const keep = this.notes.filter((_, i) => !this.sel.has(i));
        this.sel = new Set();
        this.commit(keep, 'delete notes');
        return;
      }
      const step = { ArrowLeft: [-(this.opts.grid() || 1), 0], ArrowRight: [this.opts.grid() || 1, 0],
                     ArrowUp: [0, e.shiftKey ? 12 : 1], ArrowDown: [0, e.shiftKey ? -12 : -1] }[k];
      if (step) { e.preventDefault(); const r = this.moved(step[0], step[1], false); this.sel = r.sel; this.commit(r.notes, 'move notes'); }
    });
  }
}
