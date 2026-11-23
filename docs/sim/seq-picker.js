// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance editor page: the Factory clips dialog (Kris 21 Nov 2026: fm1-port/mvance-factory-clip-picker-proposal-
// 2026-11-21.md, signed off "go with recommendations": 1a 2a 3a 4a 5a 6a). Chooses which parts of which MIDI files
// tools/mid2clip.py bakes into the firmware as factory clips (F1, F2 ..), and writes that choice as clips.txt.
//
//   the folder   Choose MIDI folder… reads every .mid under it in this browser (nothing is uploaded), in the tool's
//                order; each file's parts (tracks with notes) are listed with a tick, a small roll, their bars and
//                notes, and ▶U / ▶L to hear one
//   the list     the ticked parts, in F order: drag a row (or ↑ ↓) to move it, a name (16 characters; blank: the
//                tool's), a loop (from bar and bars; both blank: the loop the tool finds), ✕ to drop it
//   clips.txt    Open clips.txt… starts from the one you have (its parts, order, names and loops); Save clips.txt
//                downloads it with "#!only" (2a): mid2clip then builds exactly these parts, in this order (3a). With
//                no list opened, every part is ticked in path order: what the tool builds today
//   ▶U / ▶L      the part, as the tool will build it, into that lane and its track, then Play (6a); a lane's unsent
//                change goes to the shelf first. A track holds 128 notes: a longer part is a factory clip only
// The rules (the tool's order, names, loops, which parts a list picks) are seq-midi.js's, checked against the tool.
// opts: status(msg, bad), audition(lane 0|1, clip, label): Promise
import * as M from './seq-midi.js?v=20261123200814';
import { noteCount, notesOf, MAXEV, MAXBAR } from './seq-model.js?v=20261123200814';

const ROLL_W = 132, ROLL_H = 22;           // a part's small roll (CSS px)
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const relOf = (f) => { const p = f.webkitRelativePath || f.name; const i = p.indexOf('/'); return f.webkitRelativePath && i >= 0 ? p.slice(i + 1) : p; };
const topOf = (f) => (f.webkitRelativePath ? f.webkitRelativePath.split('/')[0] : '');

export function mountPicker(el, opts = {}) {
  const status = opts.status || (() => {});
  el.innerHTML = `
    <div class="pk-top">
      <span class="seq-title">Factory clips</span>
      <label class="btn" title="your MIDI folder (tools/clips/Midi): read here, nothing is uploaded">Choose MIDI folder&hellip;<input type="file" data-k="folder" webkitdirectory multiple hidden></label>
      <label class="btn" title="start from your tools/clips/clips.txt">Open clips.txt&hellip;<input type="file" data-k="list" accept=".txt,text/plain" hidden></label>
      <button data-k="save" class="primary" title="download clips.txt: put it in tools/clips/">Save clips.txt</button>
      <input type="search" data-k="find" placeholder="filter files and parts">
      <button data-k="tickall" title="tick every part shown">Tick shown</button>
      <button data-k="untickall" title="untick every part shown">Untick shown</button>
      <span class="pk-info"></span>
      <button data-k="close">Close</button>
    </div>
    <div class="pk-cols">
      <div class="pk-col"><div class="pk-h">The folder <small class="pk-folder"></small></div><div class="pk-files"><small>Choose MIDI folder&hellip; (tools/clips/Midi)</small></div></div>
      <div class="pk-col"><div class="pk-h">Built, in this order <small>drag to move · name and loop blank: the tool's</small></div><ol class="pk-list"></ol><div class="pk-miss"></div></div>
    </div>
    <div class="pk-help"><small>Save clips.txt into <code>tools/clips/</code>, then from the M-vance folder:
      <code>python3 tools/mid2clip.py --check tools/clips/Midi</code> (lists exactly these), then
      <code>python3 tools/mid2clip.py tools/clips/Midi</code> and <code>./build.sh</code>.</small></div>`;
  const q = (s) => el.querySelector(s);

  // files: Map rel -> parts (readMid's), in the tool's order; null before a folder. list: the clips.txt opened (its
  // lines are used once a folder is there). sel: the parts to build, in F order: [{ rel, index, over, name }]. miss:
  // lines of an opened list that name nothing in the folder (kept as they are, reported)
  let files = null, folder = '', list = null, sel = [], miss = [], drag = -1;
  const partOf = (s) => (files && files.get(s.rel) || []).find((p) => p.index === s.index);
  const nOf = (rel) => (files.get(rel) || []).length;
  const key = (rel, index) => `${rel}\u0000${index}`;
  const item = (s) => { const p = partOf(s); return p && { rel: s.rel, part: p, nparts: nOf(s.rel), over: s.over, name: s.name || M.nameOf(s.rel, p.name, p.index, nOf(s.rel)) }; };
  // the part's track field: its name when that names it alone (and is not a number), else its number
  function track(rel, p) {
    const same = (files.get(rel) || []).filter((x) => x.name.toLowerCase() === p.name.toLowerCase()).length;
    return p.name && same === 1 && M.listable(p.name) && !/^\d+$/.test(p.name.trim()) && p.name === p.name.trim() ? p.name : String(p.index);
  }

  // ---- the list -> sel (once there is a folder) ----
  function fromList() {
    const lst = list || { only: false, lines: [] };
    const { items, notes } = M.selectParts(files, lst);
    sel = items.map((it) => ({ rel: it.rel, index: it.part.index, over: it.over, name: it.line >= 0 && lst.lines[it.line].name ? lst.lines[it.line].name : null }));
    miss = lst.only ? notes.filter((n) => n[0] === 'missing').map((n) => n[1]) : [];
    const twice = notes.filter((n) => n[0] === 'twice').length;
    if (list) status(`clips.txt: ${sel.length} part${sel.length === 1 ? '' : 's'}${lst.only ? '' : ' (no #!only: every part, as the tool builds today)'}`
      + (miss.length ? `; ${miss.length} line${miss.length === 1 ? '' : 's'} name nothing in this folder (shown in red)` : '') + (twice ? `; ${twice} listed twice (kept once)` : '') + '.', miss.length ? true : 'ok');
    paint();
  }
  function toList() {                      // sel as clips.txt's lines (#!only); the missing lines kept at the end
    const lines = sel.map((s) => { const p = partOf(s); return { path: s.rel, track: track(s.rel, p), over: s.over, name: s.name }; });
    const keep = list && list.only ? list.lines.filter((l) => miss.some((m) => m.startsWith(`"${l.text}"`))) : [];
    return { only: true, lines: [...lines, ...keep] };
  }
  function text() {
    return M.writeList(toList(), [
      'JT-80x0 factory clips (tools/mid2clip.py): written by the editor page\'s Factory clips dialog' + (folder ? ` from the folder "${folder}"` : '') + '.',
      'Only the parts below are built, in this order (F1 = the first line):  path | part | from bar | bars | name',
      'part: its name or number in the file; from bar and bars blank: the loop the tool finds; name blank: the tool\'s.',
    ]);
  }

  // ---- reading ----
  async function setFolder(fileList) {
    const all = Array.from(fileList || []);
    const mids = new Map(all.map((f) => [relOf(f), f]));
    const order = M.toolOrder([...mids.keys()]);
    if (!order.length) { status('No .mid files in that folder.', true); return; }
    files = new Map(); folder = all.length ? topOf(all[0]) : '';
    const bad = [];
    for (const rel of order) {
      try { files.set(rel, M.readMid(new Uint8Array(await mids.get(rel).arrayBuffer()))); } catch (e) { files.set(rel, []); bad.push(`${rel}: ${e.message}`); }
    }
    const nparts = [...files.values()].reduce((a, p) => a + p.length, 0);
    status(`${order.length} MIDI files, ${nparts} parts.` + (bad.length ? ` Skipped (as the tool does): ${bad.join('; ')}.` : '')
      + (list ? '' : ' Every part is ticked, in path order: what the tool builds with no clips.txt. Open clips.txt… to start from yours.'), bad.length ? true : 'ok');
    fromList();
  }
  async function openList(f) {
    try { list = M.parseList(typeof f === 'string' ? f : await f.text()); } catch (e) { status(`clips.txt: ${e.message}`, true); return; }
    if (files) fromList();
    else { status(`clips.txt read (${list.lines.length} lines). Choose the MIDI folder it is for.`, 'ok'); paint(); }
  }

  // ---- drawing ----
  function roll(cv, clip) {
    const dpr = (typeof devicePixelRatio === 'number' && devicePixelRatio) || 1;
    cv.width = ROLL_W * dpr; cv.height = ROLL_H * dpr;
    const g = cv.getContext && cv.getContext('2d');
    if (!g || !clip) return;
    const ns = notesOf(clip), len = clip.bars * 384;
    if (!ns.length) return;
    let lo = Math.min(...ns.map((n) => n.n)), hi = Math.max(...ns.map((n) => n.n));
    if (hi - lo < 12) { const m = (lo + hi) >> 1; lo = m - 6; hi = m + 6; }
    const css = (typeof getComputedStyle === 'function' && document.documentElement) ? getComputedStyle(document.documentElement) : null;
    g.scale(dpr, dpr);
    g.fillStyle = (css && css.getPropertyValue('--line').trim()) || '#333';
    for (let b = 1; b < clip.bars; b++) g.fillRect(Math.round(b * 384 / len * ROLL_W), 0, 1, ROLL_H);
    g.fillStyle = (css && css.getPropertyValue('--theme').trim()) || '#ff8000';
    for (const n of ns) {
      const x = n.t / len * ROLL_W, w = Math.max(1, Math.min(n.len, len - n.t) / len * ROLL_W);
      g.fillRect(x, (ROLL_H - 2) - (n.n - lo) / (hi - lo) * (ROLL_H - 3), w, 2);
    }
  }
  const find = () => q('[data-k="find"]').value.trim().toLowerCase();
  const shown = (rel, p) => { const f = find(); return !f || rel.toLowerCase().includes(f) || p.name.toLowerCase().includes(f); };
  function paint() {
    const onSel = new Map(sel.map((s, k) => [key(s.rel, s.index), k]));
    q('.pk-folder').textContent = files ? `${folder || 'chosen'} · ${files.size} files` : '';
    q('.pk-info').textContent = files ? `${sel.length} of ${[...files.values()].reduce((a, p) => a + p.length, 0)} parts → F1–F${sel.length}` : '';
    q('[data-k="save"]').disabled = !files;
    // the folder: each file and its parts
    if (files) {
      const rows = [];
      for (const [rel, parts] of files) {
        const vis = parts.filter((p) => shown(rel, p));
        if (!vis.length && !(find() && rel.toLowerCase().includes(find()))) continue;
        const okPath = M.listable(rel);
        rows.push(`<div class="pk-file"><b>${esc(rel)}</b> <small>${parts.length ? `${parts.length} part${parts.length > 1 ? 's' : ''}` : 'no notes, or not readable'}</small>`
          + (okPath ? '' : ' <span class="bad">a | or # in its name: rename the file to list it</span>') + '</div>');
        for (const p of vis) {
          const k = onSel.get(key(rel, p.index)), it = { rel, part: p, nparts: parts.length, over: null, name: '' }, c = M.itemClip(it);
          rows.push(`<div class="pk-part"><label><input type="checkbox" data-k="tick" data-r="${esc(rel)}" data-i="${p.index}"${k !== undefined ? ' checked' : ''}${okPath ? '' : ' disabled'}>
            ${esc(p.name || `part ${p.index}`)}</label> <canvas class="pk-roll" data-r="${esc(rel)}" data-i="${p.index}"></canvas>
            <small>${c.bars} bar · ${c.kept} notes${p.points.length ? ` · ${p.points.length} points` : ''}${k !== undefined ? ` · <b>F${k + 1}</b>` : ''}</small>
            <button data-k="hear" data-p="0" data-r="${esc(rel)}" data-i="${p.index}" title="into the UPPER lane and track, then Play">&#9654;U</button><button data-k="hear" data-p="1" data-r="${esc(rel)}" data-i="${p.index}" title="into the LOWER lane and track, then Play">&#9654;L</button></div>`);
        }
      }
      q('.pk-files').innerHTML = rows.join('') || '<small>nothing matches the filter</small>';
      for (const cv of el.querySelectorAll('.pk-files canvas.pk-roll')) {
        const p = (files.get(cv.dataset.r) || []).find((x) => x.index === +cv.dataset.i);
        if (p) roll(cv, M.itemPageClip({ rel: cv.dataset.r, part: p, nparts: nOf(cv.dataset.r), over: null, name: '' }));
      }
    } else if (list) q('.pk-files').innerHTML = `<small>clips.txt read: ${list.lines.length} lines. Choose MIDI folder&hellip; to see the parts.</small>`;
    // the list: F order
    q('.pk-list').innerHTML = sel.map((s, k) => {
      const it = item(s), c = it && M.itemClip(it), auto = it && M.autoLoop(it.part.notes), nn = c ? c.kept : 0;
      const tool = it ? M.nameOf(s.rel, it.part.name, it.part.index, it.nparts) : '';
      return `<li draggable="true" data-s="${k}"><span class="pk-f">F${k + 1}</span>
        <input class="pk-name" data-k="name" data-s="${k}" maxlength="${M.NAME}" value="${esc(s.name || '')}" placeholder="${esc(tool)}" title="its name on the FM-1 (16 characters); blank: ${esc(tool)}">
        <label>from bar <input type="number" data-k="start" data-s="${k}" min="1" value="${s.over ? s.over.start + 1 : ''}" placeholder="${auto ? auto.start + 1 : ''}"></label>
        <label>bars <input type="number" data-k="bars" data-s="${k}" min="1" max="${MAXBAR}" value="${s.over ? s.over.bars : ''}" placeholder="${auto ? auto.bars : ''}"></label>
        <button data-k="up" data-s="${k}" title="earlier"${k ? '' : ' disabled'}>&uarr;</button><button data-k="down" data-s="${k}" title="later"${k + 1 < sel.length ? '' : ' disabled'}>&darr;</button>
        <button data-k="hear" data-p="0" data-s="${k}" title="into the UPPER lane and track, then Play">&#9654;U</button><button data-k="hear" data-p="1" data-s="${k}" title="into the LOWER lane and track, then Play">&#9654;L</button>
        <button data-k="drop" data-s="${k}" title="not built">&times;</button>
        <small>${esc(s.rel)}${it && it.nparts > 1 ? ` [${esc(it.part.name || it.part.index)}]` : ''} · ${c ? c.bars : '?'} bar · ${nn} notes${nn > MAXEV / 2 ? ' (factory only: over a track\'s 128)' : ''}</small></li>`;
    }).join('');
    q('.pk-miss').innerHTML = miss.length ? `<div class="bad">In clips.txt, naming nothing in this folder (kept in the saved file; the tool reports them):</div>${miss.map((m) => `<div class="bad"><small>${esc(m)}</small></div>`).join('')}` : '';
  }

  // ---- edits ----
  function tick(rel, index, on) {
    const k = sel.findIndex((s) => s.rel === rel && s.index === index);
    if (on && k < 0) sel.push({ rel, index, over: null, name: null });
    if (!on && k >= 0) sel.splice(k, 1);
  }
  function move(from, to) {
    if (from < 0 || from >= sel.length || to < 0 || to >= sel.length || from === to) return;
    const [x] = sel.splice(from, 1); sel.splice(to, 0, x);
  }
  async function hear(p, s, label) {
    const it = item(s);
    if (!it || !opts.audition) return;
    const clip = M.itemPageClip(it);
    if (noteCount(clip) > MAXEV / 2 || clip.events.length > MAXEV) { status(`${label}: ${noteCount(clip)} notes: a track holds ${MAXEV / 2}, so it can't be heard here. As a factory clip it plays in full.`, true); return; }
    await opts.audition(p, clip, `factory clip ${label}`);
  }
  function save() {
    const t = text();
    if (typeof URL !== 'undefined' && URL.createObjectURL && typeof document !== 'undefined') {
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([t], { type: 'text/plain' }));
      a.download = 'clips.txt'; a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    }
    status(`clips.txt saved: ${sel.length} parts. Put it in tools/clips/ (replacing the old one), then run python3 tools/mid2clip.py --check tools/clips/Midi.`, 'ok');
    return t;
  }

  el.addEventListener('click', (e) => {
    const b = e.target.closest('button[data-k]');
    if (!b) return;
    const k = b.dataset.k, s = +b.dataset.s;
    if (k === 'close') { el.hidden = true; return; }
    if (k === 'save') { save(); return; }
    if (k === 'tickall' || k === 'untickall') {
      if (!files) return;
      for (const [rel, parts] of files) if (M.listable(rel)) for (const p of parts) if (shown(rel, p)) tick(rel, p.index, k === 'tickall');
    } else if (k === 'up') move(s, s - 1);
    else if (k === 'down') move(s, s + 1);
    else if (k === 'drop') sel.splice(s, 1);
    else if (k === 'hear') {
      const x = b.dataset.r !== undefined ? { rel: b.dataset.r, index: +b.dataset.i, over: null, name: null } : sel[s];
      const k2 = sel.findIndex((y) => y.rel === x.rel && y.index === x.index);
      const it = item(k2 >= 0 ? sel[k2] : x);
      if (it) hear(+b.dataset.p, k2 >= 0 ? sel[k2] : x, `${k2 >= 0 ? `F${k2 + 1} ` : ''}${it.name.trim()}`);
      return;
    }
    paint();
  });
  el.addEventListener('change', (e) => {
    const t = e.target, k = t.dataset.k, s = sel[+t.dataset.s];
    if (k === 'folder') { setFolder(t.files); t.value = ''; return; }
    if (k === 'list') { if (t.files && t.files[0]) openList(t.files[0]); t.value = ''; return; }
    if (k === 'tick') { tick(t.dataset.r, +t.dataset.i, t.checked); paint(); return; }
    if (!s) return;
    if (k === 'name') { const v = M.cleanName(t.value).trimEnd().slice(0, M.NAME); s.name = v.trim() ? v : null; }
    else if (k === 'start' || k === 'bars') {   // (one set: the other from the tool's loop; both blank: the tool's)
      const st = q(`input[data-k="start"][data-s="${t.dataset.s}"]`).value, bs = q(`input[data-k="bars"][data-s="${t.dataset.s}"]`).value;
      if (st === '' && bs === '') s.over = null;
      else {
        const a = M.autoLoop(partOf(s).notes);
        s.over = { start: Math.max(0, (st === '' ? a.start + 1 : +st | 0) - 1), bars: Math.max(1, Math.min(MAXBAR, bs === '' ? a.bars : +bs | 0)) };
      }
    }
    paint();
  });
  el.addEventListener('input', (e) => { if (e.target.dataset.k === 'find') paint(); });
  el.addEventListener('dragstart', (e) => { const li = e.target.closest && e.target.closest('li[data-s]'); if (li) { drag = +li.dataset.s; if (e.dataTransfer) e.dataTransfer.effectAllowed = 'move'; } });
  el.addEventListener('dragover', (e) => { if (drag >= 0 && e.target.closest && e.target.closest('li[data-s]')) e.preventDefault(); });
  el.addEventListener('drop', (e) => {
    const li = e.target.closest && e.target.closest('li[data-s]');
    if (drag >= 0 && li) { e.preventDefault(); move(drag, +li.dataset.s); drag = -1; paint(); }
  });
  el.addEventListener('dragend', () => { drag = -1; });
  paint();

  return {
    open: () => { el.hidden = false; paint(); if (el.scrollIntoView) el.scrollIntoView({ block: 'start', behavior: 'smooth' }); },   // (under the panel's top bar, brought into view)
    setFolder, openList, save, text,       // (tests: a File-like { name, webkitRelativePath, arrayBuffer() }; a list's text)
    state: () => ({ files, sel, miss }),   // (tests)
  };
}
