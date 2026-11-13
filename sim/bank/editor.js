// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance bank editor UI — a 64-slot drag-drop grid on top of bank-model.js. No engine / emcc needed:
// load a .syx/.mid, drag a whole performance from the palette into a slot, drag a part (upper/lower) onto a
// slot's UPPER/LOWER, drag a slot onto another to reorder, rename, and export a bank .syx that
// `python3 tools/make_bank.py --from-device FILE.syx` turns into the factory bank.
//
// Connected to the FM-1 (fm1-midi.js, the bank tool's transfer code; Kris 12 Nov, B1) it also plays a slot or a
// palette entry on the FM-1 (the temporary area: nothing saved), sends the whole bank or the ticked slots, each to
// its own slot number, and reads the FM-1's 64 programs into the grid: one place to build and send.
//
// Part B (Kris 13 Nov: fm1-port/mvance-all-pages-editor-proposal-2026-11-13.md 4a 5a 6a, and
// fm1-port/mvance-partB-hardware-sync-proposal-2026-11-13.md 1b 2a 4a 5a 6a): the simulator runs on this page as the
// edit buffer (engine.js; its screen, every page as a tile: pages.js). Edit loads a slot into it; Store / Store as
// keep the edit in a slot; Revert and Compare go back to / listen to the stored sound. Link FM-1 keeps the FM-1's
// sound and the edit buffer the same both ways (link.js, over the firmware's link: mvance.c link_*); linked, Edit
// and ▶ play on the FM-1 too, and turning PRESETS on the FM-1 brings its program here.
import { parseBankFile, makeBankSyx, moveSlot, dropPart, cloneItem, renameSlot, nameOf, soundOf, soundCopy,
         soundEqual, itemFromSound } from './bank-model.js?v=__BUILD__';
import { FM1, NPROG, dumpBytes } from './fm1-midi.js?v=__BUILD__';
import { Link, GAP_MS } from './link.js?v=__BUILD__';
import { mountPages } from '../pages.js?v=__BUILD__';
import { startEngine } from '../engine.js?v=__BUILD__';

const NSLOT = 64;
let bank = new Array(NSLOT).fill(null);   // the 64 programs (performance items), null = empty
let palette = [];                          // sources to drag from: [{ item }] (performances and user patches)
let drag = null;                           // the live drag payload (the DnD API can only carry strings)

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s).replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
// status line: bad = true (red), 'ok' (green), else plain
const status = (msg, bad) => { const s = $('status'); s.textContent = msg; s.className = bad === 'ok' ? 'help ok' : bad ? 'help bad' : 'help'; };

// ---- the FM-1 (WebMIDI) ----
let busy = false;                          // a transfer is running: the FM-1 buttons wait
const fm1 = new FM1({
  onProgress: (v, max) => { const p = $('prog'); p.hidden = max === 0; p.max = max || 1; p.value = v; },
  onPorts: (ports) => fillPorts(ports),
  onTemp: (m) => { if (link) link.fromFm1(m); },     // the FM-1's sound: a dump or a change (linked)
  onProgram: (n) => fm1Program(n),                   // PRESETS turned on the FM-1
});
async function connect() {
  try { fillPorts(await fm1.connect()); }
  catch (e) { status(e.message, true); }
}
function fillPorts(p) {                    // fill both port lists, keep / guess the choice, then use it
  const fill = (sel, list, id) => {
    sel.innerHTML = '';
    for (const x of list) sel.add(new Option(x.name, x.id));
    if (id) sel.value = id;
    sel.disabled = !list.length;
  };
  fill($('outSel'), p.outs, p.outId);
  fill($('inSel'), p.ins, p.inId);
  usePorts();
}
function usePorts() {
  const ok = fm1.use($('outSel').value, $('inSel').value);
  status(ok ? `Connected: ${fm1.out.name}. ▶ plays, tick slots and Send, or Read 64; Link FM-1 to edit it here.` :
              'No FM-1 port: plug it in (USB) and pick it above.', ok ? 'ok' : true);
  if (!ok && link && link.fm1) setLink(false);       // the FM-1 went: unlinked
  render();
}
function buttons() {                       // what can be pressed now
  const filled = bank.some(Boolean), ticked = bank.some((it) => it && it.sel);
  $('readAll').disabled = busy || !fm1.ready;
  $('sendBank').disabled = busy || !fm1.out || !filled;
  $('sendSel').disabled = busy || !fm1.out || !ticked;
  $('link').disabled = !link || !fm1.ready;
}
async function transfer(work) {            // run one FM-1 job, buttons locked, errors on the status line
  busy = true; buttons();
  if (link) link.hold = true;                        // (the link waits: the FM-1 takes one message at a time)
  try { await work(); } catch (e) { status(e.message, true); }
  if (link) link.hold = false;
  busy = false; buttons();
}
function play(it) {
  if (link) {                                        // into the edit buffer: heard in the sim (linked: on the FM-1)
    if (!dropEdit()) return;
    editIdx = null; base = soundOf(it); comparing = null; what = 'audition';
    link.load(base); render();
    status(link.linked ? `Playing "${it.name}" on the FM-1 and here (not stored: Store as… keeps it).`
                       : `Playing "${it.name}" in the sim (not stored: Store as… keeps it).`, 'ok');
    if (!fm1.out || link.linked) return;             // (linked: the link has sent it)
  }
  if (!fm1.out) { status('Connect the FM-1 first (the button at the top).', true); return; }
  transfer(async () => { await fm1.play(it);
    status(`Playing "${it.name}" on the FM-1 (not saved: SAVE on the FM-1 keeps it).`, 'ok'); });
}
function sendSlots(which) {                // which: 'all' (every filled slot) or 'ticked'
  const pairs = bank.map((it, i) => [it, i]).filter(([it]) => it && (which === 'all' || it.sel));
  if (!pairs.length) return;
  const nums = pairs.map(([, i]) => i + 1);
  const what = pairs.length === 1 ? `slot ${nums[0]}` : `${pairs.length} slots (${nums[0]}..${nums[nums.length - 1]})`;
  if (!confirm(`Write ${what} to the FM-1? What is in those slots on the FM-1 now is replaced.`)) return;
  transfer(async () => { await fm1.sendSlots(pairs);
    status(`Sent ${what}. The FM-1 saves them to flash a moment after (when nothing is sounding).`, 'ok'); });
}
function readAll() {
  if (bank.some(Boolean) && !confirm('Replace the 64 slots here with the programs on the FM-1?')) return;
  transfer(async () => {
    status('Reading the FM-1\'s 64 programs...');
    const r = await fm1.read(0, NPROG);
    if (!r.complete) { status(`Only ${r.got} of ${r.want} messages came back: nothing changed, try again.`, true); return; }
    const perfs = parseBankFile(dumpBytes(r.msgs)).filter((i) => i.kind === 'P');
    bank = new Array(NSLOT).fill(null);
    for (const it of perfs) if (it.no >= 1 && it.no <= NSLOT) bank[it.no - 1] = cloneItem(it);
    render();
    status(`Read ${perfs.length} programs from the FM-1 into the slots. Export .syx saves them.`, 'ok');
  });
}

// ---- loading a file: fill the 64 slots (intoSlots), and/or add its items to the palette ----
function load(file, intoSlots) {
  const r = new FileReader();
  r.onload = () => loadBytes(new Uint8Array(r.result), file.name, intoSlots);
  r.readAsArrayBuffer(file);
}
// the items of a .syx / .mid's bytes into the palette (and, intoSlots, its performances into the 64 slots)
function loadBytes(bytes, name, intoSlots) {
  const items = parseBankFile(bytes).filter((i) => i.kind === 'P' || i.kind === 'U');
  if (!items.length) { status(`${name}: no JP-8000/8080 patches found`, true); return; }
  if (intoSlots) {
    const perfs = items.filter((i) => i.kind === 'P');
    for (let i = 0; i < NSLOT; i++) bank[i] = perfs[i] ? cloneItem(perfs[i]) : null;
  }
  for (const it of items) palette.push({ item: it });
  status(`${name}: ${items.length} items loaded` + (intoSlots ? ' into the slots' : ' into the palette'));
  render();
}

function exportSyx() {
  const empty = bank.findIndex((s) => !s);
  if (empty >= 0) { status(`slot ${empty + 1} is empty — every one of the 64 must be filled before export`, true); return; }
  const blob = new Blob([makeBankSyx(bank)], { type: 'application/octet-stream' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob); a.download = 'mvance-bank.syx'; a.click();
  URL.revokeObjectURL(a.href);
  status('exported mvance-bank.syx — build it with:  python3 tools/make_bank.py --from-device mvance-bank.syx');
}

// ---- a slot card: draggable (reorder), with UPPER / LOWER drop zones for parts ----
function slotCard(it, i) {
  const d = document.createElement('div');
  d.className = 'slot' + (it ? '' : ' empty') + (it && it.sel ? ' ticked' : '') + (i === editIdx ? ' editing' : '')
    + (i === fm1Prog && link && link.linked ? ' onfm1' : '');
  d.draggable = !!it;
  d.innerHTML = `<div class="hd"><span class="no">${i + 1}</span>`
    + `<span class="nm">${it ? esc(it.name) : '—'}</span>`
    + (it ? `<input type="checkbox" class="sel" title="tick to send this slot"${it.sel ? ' checked' : ''}>`
          + '<button class="edit" title="edit this slot here (linked: on the FM-1 too)">Edit</button>'
          + '<button class="play" title="play it (not stored)">▶</button>'
          + '<button class="ren" title="rename">✎</button>' : '') + '</div>'
    + (it ? `<div class="part" data-part="0">U&nbsp;${esc(nameOf(it.up))}</div>`
          + `<div class="part" data-part="1">L&nbsp;${esc(nameOf(it.lo))}</div>` : '');

  if (it) d.addEventListener('dragstart', (e) => { drag = { kind: 'slot', from: i }; e.dataTransfer.setData('text/plain', 'slot'); });
  d.addEventListener('dragover', (e) => { if (drag) { e.preventDefault(); d.classList.add('over'); } });
  d.addEventListener('dragleave', () => d.classList.remove('over'));
  d.addEventListener('drop', (e) => {
    e.preventDefault(); d.classList.remove('over');
    if (!drag) return;
    if (drag.kind === 'slot') moveSlot(bank, drag.from, i);        // reorder
    else if (drag.kind === 'perf') bank[i] = cloneItem(drag.item);  // a whole performance into this slot
    drag = null; render();
  });

  if (it) d.querySelectorAll('.part').forEach((row) => {           // drop a single patch onto UPPER / LOWER
    row.addEventListener('dragover', (e) => { if (drag && drag.kind === 'part') { e.preventDefault(); e.stopPropagation(); row.classList.add('over'); } });
    row.addEventListener('dragleave', () => row.classList.remove('over'));
    row.addEventListener('drop', (e) => {
      if (!drag || drag.kind !== 'part') return;
      e.preventDefault(); e.stopPropagation(); row.classList.remove('over');
      dropPart(bank, drag.item, drag.part, i, +row.dataset.part);
      drag = null; render();
    });
  });
  if (it) {
    const cb = d.querySelector('.sel');
    cb.addEventListener('click', (e) => e.stopPropagation());
    cb.addEventListener('change', () => { it.sel = cb.checked; d.classList.toggle('ticked', cb.checked); buttons(); });
    d.querySelector('.play').addEventListener('click', () => play(it));
    d.querySelector('.edit').addEventListener('click', () => editSlot(i));
  }
  if (it) d.querySelector('.ren').addEventListener('click', () => {
    const n = prompt('Program name (up to 16 characters):', it.name);
    if (n != null) { renameSlot(it, n); render(); }
  });
  return d;
}

// ---- a palette row: the whole performance, plus U / L chips for its parts ----
function palRow(p) {
  const it = p.item;
  const d = document.createElement('div');
  d.className = 'pal';
  d.innerHTML = `<span class="pnm" draggable="true" title="drag the whole performance into a slot">${esc(it.name)}</span>`
    + '<span class="chip" draggable="true" data-part="0" title="drag the upper patch">U</span>'
    + (it.kind === 'P' ? '<span class="chip" draggable="true" data-part="1" title="drag the lower patch">L</span>' : '')
    + '<button class="play" title="play on the FM-1 (not saved)">▶</button>';
  d.querySelector('.play').addEventListener('click', () => play(it));
  d.querySelector('.pnm').addEventListener('dragstart', (e) => { drag = { kind: 'perf', item: it }; e.dataTransfer.setData('text/plain', 'perf'); });
  d.querySelectorAll('.chip').forEach((c) => c.addEventListener('dragstart', (e) => {
    drag = { kind: 'part', item: it, part: +c.dataset.part }; e.dataTransfer.setData('text/plain', 'part');
  }));
  return d;
}

function render() {
  const g = $('grid'); g.innerHTML = '';
  bank.forEach((it, i) => g.appendChild(slotCard(it, i)));
  const pal = $('palette'); pal.innerHTML = '';
  palette.forEach((x) => pal.appendChild(palRow(x)));
  $('count').textContent = `${bank.filter(Boolean).length} / 64 slots filled`;
  buttons();
  soundUi();
}

// ================= the sound (Part B): the sim as the edit buffer, the FM-1 linked to it =================
let engine = null;                         // the simulator (engine.js); null: not built, or not loaded yet
let link = null;                           // the edit buffer and the devices linked to it (link.js)
let editIdx = null;                        // the slot being edited (null: an audition, the sim's or the FM-1's sound)
let base = null;                           // the sound as stored / as loaded: Revert, Compare and "● modified"
let comparing = null;                      // Compare on: the edit, put aside while the stored sound plays
let fm1Prog = null;                        // the program the FM-1 plays (its last Program Change, while linked)
let what = "the sim's program";            // what the edit buffer holds when it is not a slot
// FM-1-wide settings, not the sound's (Part B 4a): their tiles are greyed while linked (they would change the sim only)
const SHARED_TITLES = ['REVERB', 'GLOBAL', 'MIDI'], SHARED_BUTTONS = ['ARP', 'SEQ'];

const modified = () => !!(link && base && !soundEqual(link.cur, base));
// an unstored edit is dropped only if the user says so (true: go on)
const dropEdit = () => !modified() || comparing || confirm('Drop the unstored changes in the edit buffer?');

let uiQueued = false;
function scheduleUi() { if (!uiQueued) { uiQueued = true; requestAnimationFrame(() => { uiQueued = false; soundUi(); }); } }
function soundUi() {                       // the edit line and its buttons (cheap: called on every change)
  if (!link) return;
  const m = modified(), nm = esc(nameOf(link.cur.up));
  $('editing').innerHTML = (editIdx != null ? `Editing slot ${editIdx + 1} "${nm}"` : `Editing ${esc(what)}: "${nm}"`)
    + (comparing ? ' — <span class="mod">comparing: the stored sound</span>' : m ? ' <span class="mod">● modified</span>' : '');
  $('store').disabled = editIdx == null || !m || !!comparing;
  $('storeAs').disabled = !!comparing;
  $('revert').disabled = !m || !!comparing;
  $('compare').disabled = !base || (!m && !comparing);
  $('compare').textContent = comparing ? 'Back to the edit' : 'Compare';
  const ls = $('linkState');
  ls.textContent = link.linked ? 'linked: you hear the FM-1' : link.fm1 ? 'linking…' : 'sim only';
  ls.className = link.linked ? 'on' : '';
  $('link').checked = !!link.fm1;
  for (const [id, b] of [['nameUp', link.cur.up], ['nameLo', link.cur.lo]]) {   // (not while being typed in)
    const el = $(id);
    if (document.activeElement !== el) el.value = nameOf(b);
    el.disabled = !!comparing;
  }
}

// the names (Kris 13 Nov): the edit buffer's UPPER (= the program's) and LOWER patch names, typed in the sound panel.
// A name is the patch's first 16 bytes, ASCII 32..125 (as renameSlot, and the FM-1's SAVE page): anything else is a
// space. Each keystroke is an edit like a tile's: into the edit buffer (● modified), to the sim, and linked to the FM-1
// (16 bytes at 01 00 40 / 42 00); Store keeps it, and the slot's name follows
function nameBytes(str) {
  return Array.from((str + ' '.repeat(16)).slice(0, 16), (c) => { const v = c.charCodeAt(0); return v >= 32 && v <= 125 ? v : 32; });
}
function setName(lower, str) {
  if (!link || comparing) return;
  const s = soundCopy(link.cur);
  (lower ? s.lo : s.up).set(nameBytes(str), 0);
  link.load(s);
}

function editSlot(i) {
  if (!link || !bank[i] || !dropEdit()) return;
  editIdx = i; base = soundOf(bank[i]); comparing = null;
  link.load(base); render();
  status(`Editing slot ${i + 1}${link.linked ? ' here and on the FM-1' : ''}. Store keeps the edit in the slot.`, 'ok');
}
function store() {
  if (editIdx == null || !link) return;
  bank[editIdx] = itemFromSound(link.cur, bank[editIdx]);
  bank[editIdx].sel = true;                // ticked: Send ticked puts it on the FM-1
  base = soundCopy(link.cur); render();
  status(`Stored in slot ${editIdx + 1} (ticked: Send ticked puts it on the FM-1; Export .syx saves the bank).`, 'ok');
}
function storeAs() {
  if (!link) return;
  const first = bank.findIndex((x) => !x);
  const a = prompt('Store the edit in slot (1-64):', String(editIdx != null ? editIdx + 1 : first >= 0 ? first + 1 : 1));
  if (a == null) return;
  const n = parseInt(a, 10) - 1;
  if (!(n >= 0 && n < NSLOT)) { status(`"${a}" is not a slot (1-64).`, true); return; }
  if (bank[n] && n !== editIdx && !confirm(`Replace slot ${n + 1} "${bank[n].name}" with the edit?`)) return;
  editIdx = n;
  store();
}
function revert() { if (link && base) { comparing = null; link.load(base); } }
function compare() {                       // the stored sound, then back to the edit (edits meanwhile: dropped)
  if (!link || !base) return;
  if (comparing) { const e = comparing; comparing = null; link.load(e); }
  else { comparing = soundCopy(link.cur); link.load(base); }
  soundUi();
}
function setLink(on) {
  if (!link) return;
  if (on) {
    if (!dropEdit()) { $('link').checked = false; return; }
    comparing = null;
    link.linkFm1((m) => fm1.sendNow(m));
    engine.muted = true;                   // (you hear the FM-1)
    status('Linking: reading the FM-1\'s sound...');
  } else {
    link.unlinkFm1(); engine.muted = false; fm1Prog = null;
    status('Unlinked: the sim plays the edit again; the FM-1 keeps what it has.', 'ok');
  }
  render();
}
function fm1Program(n) {                   // PRESETS turned on the FM-1 (linked): its reports bring the sound here
  if (!link || !link.linked) return;
  const lost = modified() && !comparing;
  fm1Prog = n; comparing = null; what = `FM-1 program ${n + 1}`;
  editIdx = n < NSLOT && bank[n] ? n : null; base = null;
  setTimeout(() => { if (link) { base = soundCopy(link.cur); soundUi(); } }, 600);   // (its reports take a moment)
  render();
  status(`The FM-1 changed to program ${n + 1}` + (lost ? ': the unstored edit here went too, as on the FM-1.' : '.'),
         lost ? true : 'ok');
}

// the computer keys play (Z S X ... as the simulator page): the sim, or, linked, the FM-1 over MIDI
const ROW = 'zsxdcvgbhnjm,l.;/', KEY0_MIDI = 53;   // key 0 = F3 (mvance.c KEY0)
let noteMask = 0;
function noteKey(i, on) {
  if (link && link.linked && fm1.out) { fm1.sendNow(new Uint8Array([on ? 0x90 : 0x80, KEY0_MIDI + i, on ? 100 : 0])); return; }
  if (!engine) return;
  noteMask = on ? noteMask | (1 << i) : noteMask & ~(1 << i);
  engine.keys(noteMask);
}
const typing = (e) => e.target && e.target.closest && e.target.closest('input[type=text],textarea,select');
addEventListener('keydown', (e) => { const i = ROW.indexOf(e.key); if (i >= 0 && !e.repeat && !typing(e)) noteKey(i, true); });
addEventListener('keyup', (e) => { const i = ROW.indexOf(e.key); if (i >= 0 && !typing(e)) noteKey(i, false); });

async function startSound() {
  try { engine = await startEngine({ base: '../', stamp: '?v=__BUILD__', screen: $('screen') }); }
  catch (e) {                              // no simulator here: the bank tools work as before
    $('sound').hidden = true; document.querySelector('details.pages').hidden = true;
    console.warn('bank editor: no simulator:', e.message);
    return;
  }
  link = new Link({
    simSend: (m) => engine.sysexIn(m),
    onChange: (why, dev) => {
      if (why === 'adopt') {               // a device's sound became the edit buffer
        editIdx = null; comparing = null; base = soundCopy(link.cur);
        what = dev === 'fm1' ? "the FM-1's sound" : "the sim's program";
        if (dev === 'fm1') status('Linked: the FM-1\'s sound is here. Its knobs show on the tiles; the tiles turn it.', 'ok');
        render();
      } else scheduleUi();
    },
  });
  engine.onSysEx = (m) => link.fromSim(m);
  link.linkSim();
  setInterval(() => link.tick(), GAP_MS);
  mountPages(engine.M, $('pages'), {
    pageEditable: (title, btn) => !(link.fm1 && (SHARED_TITLES.includes(title) || SHARED_BUTTONS.includes(btn))),
    sizeInput: $('tileSize'),
  });
  $('audio').addEventListener('click', () => engine.startAudio().then(() => { $('audio').textContent = '▶ Audio on'; }));
  $('link').addEventListener('change', () => setLink($('link').checked));
  $('store').addEventListener('click', store);
  $('storeAs').addEventListener('click', storeAs);
  $('revert').addEventListener('click', revert);
  $('compare').addEventListener('click', compare);
  $('nameUp').addEventListener('input', (e) => setName(false, e.target.value));
  $('nameLo').addEventListener('input', (e) => setName(true, e.target.value));
  for (const id of ['nameUp', 'nameLo']) $(id).addEventListener('blur', soundUi);   // (shows it as stored: trimmed)
  render();
}

// ---- wiring ----
$('loadBank').addEventListener('change', (e) => { if (e.target.files[0]) load(e.target.files[0], true); e.target.value = ''; });
$('addSrc').addEventListener('change', (e) => { if (e.target.files[0]) load(e.target.files[0], false); e.target.value = ''; });
$('export').addEventListener('click', exportSyx);
$('clearPal').addEventListener('click', () => { palette = []; render(); });
$('connect').addEventListener('click', connect);
$('outSel').addEventListener('change', usePorts);
$('inSel').addEventListener('change', usePorts);
$('readAll').addEventListener('click', readAll);
$('sendBank').addEventListener('click', () => sendSlots('all'));
$('sendSel').addEventListener('click', () => sendSlots('ticked'));
render();
startSound();

// opened with ?load=<path> (the sound-match page's "Open in bank editor", Kris 12 Nov): that file goes into the
// palette. Same-site paths only (a leading /), so a link can't make the page fetch from elsewhere.
const loadUrl = new URLSearchParams(location.search).get('load');
if (loadUrl && loadUrl.startsWith('/') && !loadUrl.startsWith('//')) {
  const name = decodeURIComponent(loadUrl.split('/').pop());
  fetch(loadUrl)
    .then((r) => { if (!r.ok) throw new Error(`HTTP ${r.status}`); return r.arrayBuffer(); })
    .then((b) => {
      loadBytes(new Uint8Array(b), name, false);
      if (palette.length) status(`${name} is in the palette (bottom right): Connect FM-1, then ▶ plays it; drag it into `
                                 + 'a slot, tick the slot and Send ticked to keep it.', 'ok');
    })
    .catch((e) => status(`could not load ${name}: ${e.message}`, true));
}
