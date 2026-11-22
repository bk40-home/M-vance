// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance editor page: a lane's automation (Kris 20 Nov 2026: fm1-port/mvance-seq-automation-proposal-2026-11-20.md,
// 8a), under its piano roll. One CC at a time (the panel's list): its points as dots joined by the steps the FM-1
// plays (a value holds until the next point, round the loop). Bars and beats line up with the roll above.
//
//   click on nothing      a point there (the grid; the value from the height)
//   drag on nothing       draw: a point a grid step along the drag (a sweep)
//   drag a point          move it (time on the grid, value)
//   alt + click a point   delete it            Delete / Backspace   delete the selected point
//   Ctrl+Z / Ctrl+Y       undo / redo (the panel's)
// Like the roll it never changes the clip: each finished gesture calls opts.onEdit(points, what) with every CC's
// points ({ t, cc, v }), and the panel makes the clip (seq-model.js withPoints) and keeps the undo.
import { BAR, PPQN, PT_GRID, pointsOf } from './seq-model.js?v=20261122212620';

const KEYW = 34;                           // the label strip: as the roll's key strip, so the bars line up
const R = 4;                               // a point's radius (px)

export class AutoLane {
  // opts: onEdit(points, what), onKey('undo' | 'redo'), grid() ticks, cc() the CC shown, name(cc)
  constructor(canvas, opts) {
    this.cv = canvas;
    this.opts = { onEdit: () => {}, onKey: () => {}, grid: () => PPQN / 4, cc: () => 74, name: (c) => `CC ${c}`, ...opts };
    this.clip = null; this.pts = []; this.sel = -1; this.drag = null; this.preview = null;
    this.listen();
  }
  geo() {
    const W = this.cv.clientWidth || 600, H = this.cv.clientHeight || 70, len = (this.clip ? this.clip.bars : 1) * BAR;
    return { W, H, len, xs: (W - KEYW) / len };
  }
  step() { return Math.max(PT_GRID, Math.round((this.opts.grid() || PT_GRID) / PT_GRID) * PT_GRID); }
  xToTick(x) { const g = this.geo(); return Math.max(0, Math.min(g.len - 1, (x - KEYW) / g.xs)); }
  yToVal(y) { const g = this.geo(); return Math.max(0, Math.min(127, Math.round(127 * (1 - (y - 3) / (g.H - 6))))); }
  valToY(v) { const g = this.geo(); return 3 + (1 - v / 127) * (g.H - 6); }
  mine() { const cc = this.opts.cc(); return (this.preview || this.pts).map((p, i) => ({ ...p, i })).filter((p) => p.cc === cc); }
  hit(x, y) {                              // the shown CC's point under (x, y): its index in pts, or -1
    const g = this.geo();
    for (const p of this.mine()) if (Math.abs(KEYW + p.t * g.xs - x) <= R + 1 && Math.abs(this.valToY(p.v) - y) <= R + 1) return p.i;
    return -1;
  }
  draw(clip) {
    if (clip !== this.clip) { this.clip = clip; this.pts = clip ? pointsOf(clip) : []; if (this.sel >= this.pts.length) this.sel = -1; }
    this.paint();
  }
  paint() {
    const cv = this.cv, dpr = (typeof window !== 'undefined' && window.devicePixelRatio) || 1, { W, H, len, xs } = this.geo();
    if (cv.width !== Math.round(W * dpr) || cv.height !== Math.round(H * dpr)) { cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr); }
    const g = cv.getContext('2d'), cs = getComputedStyle(cv), col = (k, d) => (cs.getPropertyValue(k).trim() || d);
    const bg = col('--bg', '#111'), raised = col('--raised', '#26262a'), line = col('--line', '#333'), dim = col('--dim', '#999'),
      text = col('--text', '#ddd'), theme = col('--theme', '#e8a33d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.fillStyle = bg; g.fillRect(0, 0, W, H);
    g.fillStyle = raised; g.fillRect(KEYW, 0, W - KEYW, H);
    if (!this.clip) return;
    for (let t = 0; t <= len; t += PPQN) { g.fillStyle = t % BAR ? line : dim; g.globalAlpha = t % BAR ? 0.4 : 0.8; g.fillRect(KEYW + t * xs, 0, 1, H); }
    g.globalAlpha = 1;
    const cc = this.opts.cc(), all = this.preview || this.pts, mine = this.mine().sort((a, b) => a.t - b.t);
    g.fillStyle = dim; g.font = '10px system-ui'; g.textBaseline = 'top';
    g.fillText(String(cc), 4, 4);
    for (const p of all) if (p.cc !== cc) { g.fillStyle = dim; g.globalAlpha = 0.5; g.fillRect(KEYW + p.t * xs, H - 3, 1, 3); }   // (other CCs: ticks)
    g.globalAlpha = 1;
    if (mine.length) {                     // the steps the FM-1 plays: each value held to the next, round the loop
      g.strokeStyle = theme; g.lineWidth = 1.5; g.beginPath();
      let v = mine[mine.length - 1].v;     // (from the loop's start: the last point's value, held round)
      g.moveTo(KEYW, this.valToY(v));
      for (const p of mine) { const x = KEYW + p.t * xs; g.lineTo(x, this.valToY(v)); g.lineTo(x, this.valToY(p.v)); v = p.v; }
      g.lineTo(W, this.valToY(v));
      g.stroke();
      for (const p of mine) {
        g.fillStyle = p.i === this.sel ? text : theme;
        g.beginPath(); g.arc(KEYW + p.t * xs, this.valToY(p.v), R, 0, 2 * Math.PI); g.fill();
      }
    } else { g.fillStyle = dim; g.fillText(`${this.opts.name(cc)}: no points (click or drag to draw)`, KEYW + 8, H / 2 - 5); }
  }
  commit(pts, what) { this.preview = null; this.opts.onEdit(pts, what); }
  // the shown CC's point at tick t set to v (one a CC a tick), on a copy of pts
  put(pts, t, v) {
    const cc = this.opts.cc(), out = pts.filter((p) => !(p.cc === cc && p.t === t));
    out.push({ t, cc, v });
    return out;
  }
  listen() {
    const cv = this.cv;
    cv.tabIndex = 0;
    const pos = (e) => { const r = cv.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
    const snap = (x) => Math.floor(this.xToTick(x) / this.step()) * this.step();
    cv.addEventListener('pointerdown', (e) => {
      if (!this.clip || e.button !== 0) return;
      cv.focus();
      const [x, y] = pos(e), h = this.hit(x, y);
      if (h >= 0 && e.altKey) { this.sel = -1; this.commit(this.pts.filter((_, i) => i !== h), 'delete a point'); return; }
      if (h >= 0) { this.sel = h; this.drag = { mode: 'move', i: h, moved: false }; }
      else { this.drag = { mode: 'draw', moved: false, pts: this.put(this.pts, snap(x), this.yToVal(y)) }; this.preview = this.drag.pts; }
      this.paint();
    });
    cv.addEventListener('pointermove', (e) => {
      const d = this.drag;
      if (!d) return;
      const [x, y] = pos(e);
      d.moved = true;
      if (d.mode === 'move') {
        const p = this.pts[d.i], np = this.pts.filter((_, i) => i !== d.i);
        this.preview = this.put(np, snap(x), this.yToVal(y));
        d.to = { t: snap(x), v: this.yToVal(y), cc: p.cc };
      } else { d.pts = this.put(d.pts, snap(x), this.yToVal(y)); this.preview = d.pts; }
      this.paint();
    });
    cv.addEventListener('pointerup', () => {
      const d = this.drag;
      if (!d) return;
      this.drag = null;
      if (d.mode === 'draw') { this.commit(d.pts, d.moved ? 'draw automation' : 'add a point'); return; }
      if (d.moved && this.preview) { this.commit(this.preview, 'move a point'); return; }
      this.preview = null; this.paint();
    });
    cv.addEventListener('keydown', (e) => {
      const k = e.key, ctrl = e.ctrlKey || e.metaKey;
      if (ctrl && (k === 'z' || k === 'Z')) { e.preventDefault(); this.opts.onKey(e.shiftKey ? 'redo' : 'undo'); return; }
      if (ctrl && (k === 'y' || k === 'Y')) { e.preventDefault(); this.opts.onKey('redo'); return; }
      if ((k === 'Delete' || k === 'Backspace') && this.sel >= 0) {
        e.preventDefault();
        const s = this.sel; this.sel = -1;
        this.commit(this.pts.filter((_, i) => i !== s), 'delete a point');
      }
    });
  }
}
