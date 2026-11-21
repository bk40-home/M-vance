// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance editor page: the Sequences panel (Kris 20 Nov 2026: fm1-port/mvance-seq-editor-proposal-2026-11-20.md,
// signed off "go with recommended": 1a 2a 3a 4a 5a 6a 7a 8a).
//
//   lanes      UPPER and LOWER: each holds a clip being looked at or changed (seq-model.js) and draws it in a roll
//              that edits it (seq-roll.js: add, move, resize, copy, delete, velocity; undo / redo). A lane starts as
//              its live track on the device; Open puts a user clip, a factory clip, a shelved clip or a part of a
//              MIDI file in it (shown "not sent" until Send)
//   a file     Open .mid (or drop one on a lane): its part, cut by the import bar (seq-midi.js): the loop the tool
//              would choose (Auto loop: tools/mid2clip.py's), or a start bar and LENGTH; notes outside dropped or
//              folded in; transpose; quantize. Each change makes the lane's clip again from the file (an undo step)
//   Send       the lane into its live track (heard at once, the playhead runs on; not kept in flash)
//   Save       the lane into user clip 1..12 with a name (kept in flash once the sequencer is stopped and silent)
//   the limit  a track holds 128 notes: a longer lane says so and is not sent. Factory clip downloads it as a .mid
//              with its tools/clips/clips.txt line, so mid2clip bakes it into the next build (no limit there) (3a)
//   the shelf  clips kept in this browser (Shelve), to put back into a lane or download as .mid (7a)
//   automation (Kris 20 Nov, 8a) under each roll, one CC at a time (seq-auto.js: draw, move, delete points); the list
//              shows the CCs the lane has points for, then every CC of the sound (seq-ccs.js). A MIDI file's CCs come
//              in with its notes (seq-midi.js). Points share the track's 256 events with the notes
//   factory    Factory clips… (Kris 21 Nov: mvance-factory-clip-picker-proposal-2026-11-21.md): the dialog that picks
//              which parts of which MIDI files mid2clip builds (seq-picker.js); its ▶U / ▶L put a part in a lane, the
//              lane's unsent change shelved first, send it and Play
//   the device the sim on this page, or the FM-1 while Link FM-1 is on (opts.dev() says which); its tracks are read
//              when the panel starts and when the device changes. Linked, a change made on the device itself (a
//              recording, a CLIP LOAD on the panel) is reported (6a): a lane with no unsent change follows it, one with
//              a change asks Take / Keep mine
// The editor (editor.js) passes the device and forwards area-04 messages to feed(). No DOM outside root.
import * as S from './seq-model.js?v=__BUILD__';
import * as M from './seq-midi.js?v=__BUILD__';
import { Roll, noteName } from './seq-roll.js?v=__BUILD__';
import { AutoLane } from './seq-auto.js?v=__BUILD__';
import { SOUND_CCS } from './seq-ccs.js?v=__BUILD__';
import { mountPicker } from './seq-picker.js?v=__BUILD__';

const PROBE_MS = 600;                      // a factory clip's head not answered in this long: no more of them
const NFAC_MAX = 96;                       // factory clips the address can name (tt 20..7F)
const UNDO_MAX = 100;                      // undo steps a lane keeps
const SHELF_KEY = 'mvance-seq-shelf';      // the shelf in this browser's storage
const GRIDS = [['1/4', 96], ['1/8', 48], ['1/16', 24], ['1/32', 12], ['1/8T', 32], ['1/16T', 16], ['free', 1]];
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const PART = ['UPPER', 'LOWER'];

// opts: dev() -> { kind: 'sim' | 'fm1', label, send(msgs): Promise, rt(status byte) }; status(msg, bad)
export function mountSeq(root, opts) {
  const status = opts.status || (() => {});
  root.innerHTML = `
    <div class="seq-top">
      <span class="seq-title">Sequences</span>
      <button data-a="play" title="MIDI START: both tracks from bar 1">&#9654; Play</button>
      <button data-a="stop" title="MIDI STOP">&#9632; Stop</button>
      <button data-a="readAll" title="read both live tracks and the clips' names again">Read from device</button>
      <button data-a="picker" title="choose which parts of your MIDI files become the firmware's factory clips (clips.txt)">Factory clips&hellip;</button>
      <label>grid <select data-a="grid">${GRIDS.map(([n, t]) => `<option value="${t}"${t === 24 ? ' selected' : ''}>${n}</option>`).join('')}</select></label>
      <span class="seq-dev"></span>
    </div>
    <div class="picker" hidden></div>
    ${[0, 1].map((p) => `
    <div class="lane" data-p="${p}">
      <div class="lane-bar">
        <b>${PART[p]}</b>
        <span class="lane-src"></span>
        <span class="lane-mod"></span>
        <span class="lane-ask" hidden>The device changed this track:
          <button data-a="take">Take</button><button data-a="mine">Keep mine</button></span>
      </div>
      <div class="lane-bar">
        <label>Open <select data-a="open"></select></label>
        <label class="btn">Open .mid&hellip;<input type="file" data-a="file" accept=".mid,.midi" hidden></label>
        <label>LENGTH <input type="number" data-a="bars" min="1" max="${S.MAXBAR}" step="1"> bars</label>
        <span class="lane-info"></span>
      </div>
      <div class="lane-bar lane-imp" hidden>
        <span class="lane-file"></span>
        <label>part <select data-a="part"></select></label>
        <label>from bar <input type="number" data-a="start" min="1" step="1"></label>
        <label><input type="checkbox" data-a="fold"> fold outside notes in</label>
        <label>transpose <input type="number" data-a="xpose" min="-24" max="24" step="1"></label>
        <label>quantize <select data-a="quant">${S.QUANTS.map((x, i) => `<option value="${i}">${x}</option>`).join('')}</select></label>
        <button data-a="auto" title="the loop tools/mid2clip.py would choose">Auto loop</button>
        <span class="lane-how"></span>
      </div>
      <div class="lane-bar">
        <button data-a="send" class="primary" title="into the live track: heard at once, not kept in flash">Send to track</button>
        <button data-a="save" title="into a user clip (1-12), kept in flash">Save to clip&hellip;</button>
        <button data-a="shelve" title="keep it in this browser's shelf">Shelve&hellip;</button>
        <button data-a="mid" title="download as a MIDI file">.mid</button>
        <button data-a="factory" title="a .mid and its clips.txt line: mid2clip bakes it into the firmware (no 128-note limit)">Factory clip&hellip;</button>
        <button data-a="clear" title="no notes (Send to empty the track)">Clear</button>
        <button data-a="revert" title="back to the track as the device has it">Revert</button>
        <button data-a="undo" title="Ctrl+Z">Undo</button><button data-a="redo" title="Ctrl+Y">Redo</button>
        <button data-a="fit" title="the rows fitted to the notes again">Fit</button>
      </div>
      <canvas class="roll" title="click: add a note · drag: move (alt: copy) · drag its end: resize · drag on nothing: select · wheel: velocity / scroll · Delete · arrows"></canvas>
      <div class="lane-bar lane-autobar">
        <label>automation <select data-a="cc"></select></label>
        <span class="lane-autoinfo"></span>
        <button data-a="ccclear" title="this CC's points off the lane">Clear CC</button>
      </div>
      <canvas class="auto" title="click: a point · drag on nothing: draw · drag a point: move · alt+click / Delete: delete"></canvas>
    </div>`).join('')}
    <div class="shelf"><span class="seq-title">Shelf</span> <span class="shelf-list"></span></div>`;
  const q = (sel, el = root) => el.querySelector(sel);
  const laneEl = (p) => q(`.lane[data-p="${p}"]`);
  let grid = 24;

  // lanes: clip (shown), dev (the track as last read: Revert, "not sent"), from (where clip came from), ver (the
  // track's seq.ver as last read), file (a MIDI file being cut: { name, parts, part, start, bars, fold, xpose, quant })
  const lanes = [0, 1].map(() => ({ clip: null, dev: null, from: '', ver: -1, ask: false, file: null, hist: [], fut: [], cc: 74 }));
  const ccName = (c) => SOUND_CCS[c] || `CC ${c}`;
  lanes.forEach((L, p) => {
    L.roll = new Roll(q('canvas.roll', laneEl(p)), {   // (notes edited: the lane's points kept as they are)
      grid: () => grid,
      onEdit: (notes, what) => { if (L.clip) edit(p, { ...S.clipCopy(L.clip), events: S.withPoints(S.eventsOf(notes, L.clip.bars), S.pointsOf(L.clip), L.clip.bars) }, what); },
      onKey: (k) => (k === 'undo' ? undo(p) : redo(p)),
    });
    L.auto = new AutoLane(q('canvas.auto', laneEl(p)), {   // (points edited: the notes kept as they are)
      grid: () => grid, cc: () => L.cc, name: ccName,
      onEdit: (pts, what) => { if (L.clip) edit(p, { ...S.clipCopy(L.clip), events: S.withPoints(L.clip.events, pts, L.clip.bars) }, what); },
      onKey: (k) => (k === 'undo' ? undo(p) : redo(p)),
    });
  });
  const dir = { user: new Array(S.NUSER).fill(null), fac: [] };   // the clips' heads (names, sizes)
  const opening = new Map();               // tt of a clip being read -> the lane it goes into
  let probe = null;                         // the factory clips' heads being asked for: { k, timer }
  let shelf = [];                           // the shelf (7a): [{ name, clip }]
  try { shelf = JSON.parse(localStorage.getItem(SHELF_KEY) || '[]'); } catch { shelf = []; }

  // ---- a lane's clip: every change an undo step ----
  function edit(p, clip, what) {
    const L = lanes[p];
    if (L.clip) { L.hist.push(L.clip); if (L.hist.length > UNDO_MAX) L.hist.shift(); }
    L.fut = []; L.clip = clip; L.lastEdit = what;
    draw();
  }
  function undo(p) { const L = lanes[p]; if (!L.hist.length) return; L.fut.push(L.clip); L.clip = L.hist.pop(); draw(); }
  function redo(p) { const L = lanes[p]; if (!L.fut.length) return; L.hist.push(L.clip); L.clip = L.fut.pop(); draw(); }
  function show(p, clip, from) { edit(p, clip, 'open'); lanes[p].from = from; }   // (a new thing in the lane: undoable)

  const reader = new S.SeqReader({
    onClip: (tt, c, h) => {
      if (tt < 2) {                         // a live track: the lane follows it (unless it holds an unsent change)
        const L = lanes[tt], keep = L.clip && L.dev && modified(tt);   // (judged against the track as it was)
        L.dev = c; L.ver = h.ver; L.ask = false;
        if (!keep && !S.clipEqual(L.clip, c)) {
          if (L.clip) edit(tt, S.clipCopy(c), 'the device'); else L.clip = S.clipCopy(c);
          L.file = null;
          L.from = `the ${PART[tt]} track` + (h.flags & S.FL.FLASH ? ' (a factory clip)' : '');
        }
      } else if (opening.has(tt)) {         // a clip opened into a lane: shown, not yet sent
        const p = opening.get(tt); opening.delete(tt);
        lanes[p].file = null;
        show(p, c, `${S.ttName(tt)}${h.name ? ' ' + h.name : ''}`);
        status(`${S.ttName(tt)} is in the ${PART[p]} lane: Send to track to hear it.`, 'ok');
      }
      draw();
    },
    onHead: (tt, h) => {
      if (tt >= S.TT.user(0) && tt < S.TT.user(S.NUSER)) { dir.user[tt - S.TT.user(0)] = h.flags & S.FL.EMPTY ? null : h; fillOpen(); }
      else if (tt >= S.TT.fac(0)) {         // a factory clip's head: kept, then the next one asked for
        dir.fac[tt - S.TT.fac(0)] = h; fillOpen();
        if (probe && tt - S.TT.fac(0) === probe.k) nextProbe();
      } else if (tt < 2) {                  // a report: the device's track changed (a recording, a CLIP LOAD, a Send)
        const L = lanes[tt];
        if (h.ver === L.ver) return;
        if (modified(tt)) { L.ask = true; draw(); }   // (an unsent change here: the user decides)
        else readTrack(tt);
      }
    },
  });

  const modified = (p) => !!lanes[p].clip && (!lanes[p].dev || !S.clipEqual(lanes[p].clip, lanes[p].dev));
  const send = (msgs) => opts.dev().send(msgs);
  function readTrack(p) { reader.expect(S.TT.track(p)); return send([S.readMsg(S.TT.track(p))]); }
  function nextProbe() {                    // the factory clips' heads, one after another until one does not answer
    clearTimeout(probe.timer);
    probe.k++;
    if (probe.k >= NFAC_MAX) { probe = null; return; }
    probe.timer = setTimeout(() => { probe = null; }, PROBE_MS);
    send([S.readMsg(S.TT.fac(probe.k), true)]);
  }
  async function readAll() {
    dir.user.fill(null); dir.fac = []; opening.clear(); fillOpen();
    for (const p of [0, 1]) { lanes[p].dev = null; lanes[p].ver = -1; }
    await readTrack(0); await readTrack(1);
    await send(Array.from({ length: S.NUSER }, (_, k) => S.readMsg(S.TT.user(k), true)));
    if (probe) clearTimeout(probe.timer);
    probe = { k: -1, timer: 0 }; nextProbe();
  }

  // ---- Open: the track, the user clips, the factory clips, the shelf ----
  function fillOpen() {
    for (const p of [0, 1]) {
      const sel = q('select[data-a="open"]', laneEl(p));
      const o = [`<option value="">— choose —</option><option value="t">the ${PART[p]} track (read again)</option>`];
      o.push('<optgroup label="user clips">' + dir.user.map((h, k) => `<option value="${S.TT.user(k)}"${h ? '' : ' disabled'}>${k + 1} ${h ? esc(h.name || '(no name)') + ` · ${h.bars} bar` : '(empty)'}</option>`).join('') + '</optgroup>');
      if (dir.fac.length) o.push('<optgroup label="factory clips">' + dir.fac.map((h, k) => h ? `<option value="${S.TT.fac(k)}">F${k + 1} ${esc(h.name)} · ${h.bars} bar</option>` : '').join('') + '</optgroup>');
      if (shelf.length) o.push('<optgroup label="shelf">' + shelf.map((s, k) => `<option value="s${k}">${esc(s.name)} · ${s.clip.bars} bar</option>`).join('') + '</optgroup>');
      sel.innerHTML = o.join(''); sel.value = '';
    }
  }
  function open(p, v) {
    if (v === '') return;
    if (modified(p) && !confirm('Drop the change in this lane (not sent)? (Undo brings it back.)')) return;
    if (v === 't') { lanes[p].dev = null; readTrack(p); return; }
    if (v[0] === 's') { const s = shelf[+v.slice(1)]; if (s) { lanes[p].file = null; show(p, S.clipCopy(s.clip), `the shelf: ${s.name}`); } return; }
    const tt = +v;
    opening.set(tt, p); reader.expect(tt);
    send([S.readMsg(tt)]);
  }

  // ---- a MIDI file into a lane (4a) ----
  async function openFile(p, f) {
    let parts;
    try { parts = M.readMid(new Uint8Array(await f.arrayBuffer())); } catch (e) { status(`${f.name}: ${e.message}`, true); return; }
    if (!parts.length) { status(`${f.name}: no notes in it.`, true); return; }
    lanes[p].file = { name: f.name, base: f.name.replace(/\.midi?$/i, ''), parts, part: 0, fold: false, xpose: 0, quant: 0 };
    autoLoop(p);
    const L = lanes[p], cc = [...new Set(L.clip.events.filter(S.isPoint).map((e) => e.n))];
    if (cc.length) L.cc = cc[0];            // (its automation shown, if it has any)
    status(`${f.name}: ${parts.length} part${parts.length > 1 ? 's' : ''}. The loop is the one mid2clip would choose; change it in the bar above the roll.`
      + (cc.length ? ` Automation: ${cc.map(ccName).join(', ')}.` : ''), 'ok');
  }
  function autoLoop(p) {                   // the tool's loop for the file's part
    const F = lanes[p].file, L = M.autoLoop(F.parts[F.part].notes);
    Object.assign(F, { start: L.start, bars: L.bars, fold: false, xpose: 0, quant: 0,
      how: L.how === 'repeat' ? `repeats every ${L.bars} of ${L.span} bars` : L.how === 'whole' ? `the whole ${L.span} bars` : `the ${L.how} of ${L.span} bars` });
    cut(p);
  }
  function cut(p) {                        // the lane's clip from the file, as the import bar says
    const L = lanes[p], F = L.file, part = F.parts[F.part];
    const name = (part.name && !/^midi out/i.test(part.name) ? part.name : F.base).toUpperCase().slice(0, 6);
    show(p, M.trim(part.notes, { start: F.start, bars: F.bars, fold: F.fold, transpose: F.xpose, quant: F.quant, name }, part.points),
      `${F.name}${F.parts.length > 1 ? ` [${part.name || part.index}]` : ''}`);
  }

  // ---- the buttons ----
  async function sendLane(p) {
    const L = lanes[p], why = L.clip && S.validate(L.clip);
    if (!L.clip) return;
    if (why) { status(`Not sent: ${why}.`, true); return; }
    await send(S.writeMsgs(S.TT.track(p), L.clip));
    L.ask = false;
    await readTrack(p);                     // (read back: the lane shows what the device took)
    status(`Sent to the ${PART[p]} track on the ${opts.dev().label}.`, 'ok');
  }
  async function saveLane(p) {
    const L = lanes[p];
    if (!L.clip) return;
    const why = S.validate(L.clip);
    if (why) { status(`Not saved: ${why}.`, true); return; }
    const a = prompt('Save into user clip (1-12):', '1');
    if (a == null) return;
    const k = parseInt(a, 10) - 1;
    if (!(k >= 0 && k < S.NUSER)) { status(`"${a}" is not a user clip (1-12).`, true); return; }
    const name = prompt('Its name (6 characters):', (L.clip.name || L.from.replace(/^.*\s/, '')).slice(0, 6));
    if (name == null) return;
    if (dir.user[k] && !confirm(`User clip ${k + 1} holds "${dir.user[k].name}". Replace it?`)) return;
    const c = { ...S.clipCopy(L.clip), name: name.toUpperCase().slice(0, 6) };
    await send(S.writeMsgs(S.TT.user(k), c));
    await send([S.readMsg(S.TT.user(k), true)]);   // (its head back: the list shows it; refused = the old one)
    setTimeout(() => {
      const h = dir.user[k];
      if (h && h.n === c.events.length && h.name === c.name.trim()) status(`Saved as user clip ${k + 1} "${c.name}". It goes to flash once the sequencer is stopped and nothing sounds.`, 'ok');
      else status(`User clip ${k + 1} was not taken: the ${opts.dev().label} is still saving the last clip. Stop the sequencer, wait a moment and Save again.`, true);
    }, 400);
  }
  function download(bytes, name) {
    if (typeof URL === 'undefined' || !URL.createObjectURL) return;   // (no browser: tests)
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([bytes], { type: 'audio/midi' }));
    a.download = name; a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }
  const fileName = (p) => ((lanes[p].clip.name || lanes[p].from.replace(/\.midi?.*$/i, '')).trim().replace(/[^\w-]+/g, '_') || `mvance-${PART[p].toLowerCase()}`);
  function factory(p) {
    const L = lanes[p], name = prompt('The factory clip\'s file name (it also names the clip, 16 characters):', fileName(p));
    if (!name) return;
    const f = `${name.replace(/[^\w -]+/g, '_')}.mid`;
    download(M.writeMid(L.clip, name), f);
    status(`Downloaded ${f}. Put it in your MIDI folder, add this line to tools/clips/clips.txt: "${f} | | 1 | ${L.clip.bars}", `
      + 'then run python3 tools/mid2clip.py on the folder and ./build.sh: it becomes a factory clip (F…), any number of notes.', 'ok');
  }
  // ---- the shelf (7a): this browser's storage ----
  const keepShelf = () => { try { localStorage.setItem(SHELF_KEY, JSON.stringify(shelf)); } catch { /* (no storage: this visit only) */ } fillOpen(); paintShelf(); };
  function shelve(p) {
    const L = lanes[p], name = prompt('A name for it on the shelf:', fileName(p));
    if (!name) return;
    shelf.push({ name, clip: S.clipCopy(L.clip) });
    keepShelf();
    status(`"${name}" is on the shelf (this browser). Open puts it back into a lane.`, 'ok');
  }
  function paintShelf() {
    q('.shelf-list').innerHTML = shelf.length ? shelf.map((s, k) =>
      `<span class="shelf-item">${esc(s.name)} <small>${s.clip.bars} bar · ${S.noteCount(s.clip)} notes</small>
       <button data-a="sh-up" data-k="${k}" title="into the UPPER lane">&rarr;U</button><button data-a="sh-lo" data-k="${k}" title="into the LOWER lane">&rarr;L</button><button data-a="sh-mid" data-k="${k}" title="download as .mid">.mid</button><button data-a="sh-del" data-k="${k}" title="off the shelf">&times;</button></span>`).join('')
      : '<small>empty: Shelve keeps a lane here</small>';
  }

  // ---- drawing (cheap: once a frame at most) ----
  let queued = false;
  function draw() { if (!queued) { queued = true; requestAnimationFrame(() => { queued = false; paint(); }); } }
  function paint() {
    q('.seq-dev').textContent = `on the ${opts.dev().label}`;
    for (const p of [0, 1]) {
      const L = lanes[p], el = laneEl(p), c = L.clip, why = c && S.validate(c), m = modified(p), F = L.file;
      q('.lane-src', el).textContent = c ? `from ${L.from || `the ${PART[p]} track`}` : 'reading…';
      q('.lane-mod', el).textContent = m ? '● not sent' : '';
      q('.lane-ask', el).hidden = !L.ask;
      const val = (sel, v) => { const x = q(sel, el); if (document.activeElement !== x) x.value = v; };
      val('input[data-a="bars"]', c ? c.bars : '');
      const over = why && S.noteCount(c) > S.MAXEV / 2;
      const np = c ? S.pointCount(c) : 0;
      q('.lane-info', el).innerHTML = c ? `ROOT ${c.root == null ? '—' : noteName(c.root)} · <span class="${why ? 'bad' : ''}">${S.noteCount(c)}/${S.MAXEV / 2} notes`
        + (np ? ` + ${np} points (${c.events.length}/${S.MAXEV} events)` : '') + '</span>'
        + (why ? ` <span class="bad">${esc(why)}${over ? ' — or make it a factory clip' : ''}</span>` : '') : '';
      fillCC(p);
      q('.lane-imp', el).hidden = !F;
      if (F) {
        q('.lane-file', el).textContent = F.name;
        const ps = q('select[data-a="part"]', el);
        if (ps.options.length !== F.parts.length) ps.innerHTML = F.parts.map((x, i) => `<option value="${i}">${esc(x.name || `part ${x.index}`)} (${x.notes.length})</option>`).join('');
        val('select[data-a="part"]', F.part); val('input[data-a="start"]', F.start + 1); val('input[data-a="xpose"]', F.xpose);
        val('select[data-a="quant"]', F.quant); q('input[data-a="fold"]', el).checked = F.fold;
        q('.lane-how', el).textContent = F.how || '';
      }
      for (const a of ['send', 'save']) q(`[data-a="${a}"]`, el).disabled = !c || !!why;
      for (const a of ['clear', 'shelve', 'mid', 'factory']) q(`[data-a="${a}"]`, el).disabled = !c;
      q('[data-a="revert"]', el).disabled = !m || !L.dev;
      q('[data-a="undo"]', el).disabled = !L.hist.length;
      q('[data-a="redo"]', el).disabled = !L.fut.length;
      L.roll.draw(c);
      L.auto.draw(c);
    }
  }
  // the automation list: the CCs the lane has points for (with how many), then the sound's others
  function fillCC(p) {
    const L = lanes[p], sel = q('select[data-a="cc"]', laneEl(p)), c = L.clip;
    const have = new Map();
    if (c) for (const e of c.events) if (S.isPoint(e)) have.set(e.n, (have.get(e.n) || 0) + 1);
    const key = `${L.cc}|${[...have].join(',')}`;
    if (sel.dataset.key === key) return;    // (unchanged: the list is not rebuilt under the mouse)
    sel.dataset.key = key;
    const opt = (cc, extra = '') => `<option value="${cc}"${cc === L.cc ? ' selected' : ''}>${esc(ccName(+cc))} (CC ${cc})${extra}</option>`;
    sel.innerHTML = (have.size ? `<optgroup label="in this lane">${[...have].sort((a, b) => a[0] - b[0]).map(([cc, n]) => opt(cc, ` · ${n}`)).join('')}</optgroup>` : '')
      + `<optgroup label="the sound">${Object.keys(SOUND_CCS).filter((cc) => !have.has(+cc)).map((cc) => opt(+cc)).join('')}</optgroup>`;
    q('.lane-autoinfo', laneEl(p)).textContent = have.get(L.cc) ? `${have.get(L.cc)} points` : '';
    q('[data-a="ccclear"]', laneEl(p)).disabled = !have.get(L.cc);
  }

  // ---- wiring ----
  root.addEventListener('click', (e) => {
    const b = e.target.closest('button[data-a]');
    if (!b) return;
    const lane = b.closest('.lane'), p = lane ? +lane.dataset.p : -1, a = b.dataset.a, L = lanes[p];
    if (a === 'play') opts.dev().rt(0xfa);
    else if (a === 'stop') opts.dev().rt(0xfc);
    else if (a === 'readAll') readAll();
    else if (a === 'picker') picker.open();
    else if (a === 'send') sendLane(p);
    else if (a === 'save') saveLane(p);
    else if (a === 'shelve') shelve(p);
    else if (a === 'mid') download(M.writeMid(L.clip, L.clip.name || ''), `${fileName(p)}.mid`);
    else if (a === 'factory') factory(p);
    else if (a === 'clear') edit(p, { ...S.emptyClip(L.clip.bars), quant: L.clip.quant }, 'clear');
    else if (a === 'revert' && L.dev) { L.file = null; show(p, S.clipCopy(L.dev), `the ${PART[p]} track`); }
    else if (a === 'undo') undo(p);
    else if (a === 'redo') redo(p);
    else if (a === 'fit') L.roll.refit();
    else if (a === 'auto' && L.file) autoLoop(p);
    else if (a === 'ccclear' && L.clip) edit(p, { ...S.clipCopy(L.clip), events: L.clip.events.filter((x) => !(S.isPoint(x) && x.n === L.cc)) }, 'clear a CC');
    else if (a === 'take') { L.dev = null; readTrack(p); }
    else if (a === 'mine') { L.ask = false; draw(); }
    else if (a.startsWith('sh-')) {
      const k = +b.dataset.k, s = shelf[k];
      if (!s) return;
      if (a === 'sh-del') { if (confirm(`Take "${s.name}" off the shelf?`)) { shelf.splice(k, 1); keepShelf(); } }
      else if (a === 'sh-mid') download(M.writeMid(s.clip, s.name), `${s.name.replace(/[^\w-]+/g, '_')}.mid`);
      else { const t = a === 'sh-up' ? 0 : 1; lanes[t].file = null; show(t, S.clipCopy(s.clip), `the shelf: ${s.name}`); }
    }
  });
  root.addEventListener('change', (e) => {
    const t = e.target, lane = t.closest('.lane'), p = lane ? +lane.dataset.p : -1, a = t.dataset.a, L = lanes[p];
    if (a === 'grid') { grid = +t.value; return; }
    if (a === 'cc') { L.cc = +t.value; draw(); return; }
    if (a === 'open') { open(p, t.value); t.value = ''; return; }
    if (a === 'file') { if (t.files && t.files[0]) openFile(p, t.files[0]); t.value = ''; return; }
    if (a === 'bars') {
      const b = Math.max(1, Math.min(S.MAXBAR, +t.value | 0));
      if (L.file) { L.file.bars = b; L.file.how = 'your cut'; cut(p); }   // (a file: cut again at the new LENGTH)
      else if (L.clip) edit(p, { ...S.clipCopy(L.clip), bars: b }, 'LENGTH');
      return;
    }
    if (!L || !L.file) return;
    const F = L.file;
    if (a === 'part') { F.part = +t.value; autoLoop(p); return; }
    if (a === 'start') F.start = Math.max(0, (+t.value | 0) - 1);
    else if (a === 'fold') F.fold = t.checked;
    else if (a === 'xpose') F.xpose = Math.max(-24, Math.min(24, +t.value | 0));
    else if (a === 'quant') F.quant = +t.value;
    F.how = 'your cut';
    cut(p);
  });
  for (const p of [0, 1]) {                // a .mid dropped on a lane
    const el = laneEl(p);
    el.addEventListener('dragover', (e) => { e.preventDefault(); });
    el.addEventListener('drop', (e) => { e.preventDefault(); const f = e.dataTransfer && e.dataTransfer.files[0]; if (f) openFile(p, f); });
  }
  if (typeof addEventListener === 'function') addEventListener('resize', draw);
  paintShelf();

  // ---- the Factory clips dialog (Kris 21 Nov): a part heard in a lane (6a) ----
  async function audition(p, clip, label) {
    const L = lanes[p];
    if (modified(p)) {                      // (the lane's unsent change: kept on the shelf, not lost)
      shelf.push({ name: `${PART[p]} before ${label}`.slice(0, 40), clip: S.clipCopy(L.clip) });
      keepShelf();
    }
    L.file = null;
    show(p, clip, label);
    await sendLane(p);
    opts.dev().rt(0xfa);
  }
  const picker = mountPicker(q('.picker'), { status, audition });

  return {
    feed: (m) => reader.feed(m),            // an area-04 message from the device: true when it was one
    readAll,                                // the device changed (linked / unlinked), or the sim is ready
    modified: () => modified(0) || modified(1),
    openFile,                               // (tests: a File-like { name, arrayBuffer() })
    lanes,                                  // (tests)
    picker,                                 // (tests: the Factory clips dialog)
    shelf: () => shelf,                     // (tests)
  };
}
