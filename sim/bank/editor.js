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
// (Hearing a slot in the simulator itself still waits for the sim's load/dump bridge.)
import { parseBankFile, makeBankSyx, moveSlot, dropPart, cloneItem, renameSlot, nameOf } from './bank-model.js';
import { FM1, NPROG, dumpBytes } from './fm1-midi.js';

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
  status(ok ? `Connected: ${fm1.out.name}. ▶ plays, tick slots and Send, or Read 64.` :
              'No FM-1 port: plug it in (USB) and pick it above.', ok ? 'ok' : true);
  render();
}
function buttons() {                       // what can be pressed now
  const filled = bank.some(Boolean), ticked = bank.some((it) => it && it.sel);
  $('readAll').disabled = busy || !fm1.ready;
  $('sendBank').disabled = busy || !fm1.out || !filled;
  $('sendSel').disabled = busy || !fm1.out || !ticked;
}
async function transfer(work) {            // run one FM-1 job, buttons locked, errors on the status line
  busy = true; buttons();
  try { await work(); } catch (e) { status(e.message, true); }
  busy = false; buttons();
}
function play(it) {
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
  d.className = 'slot' + (it ? '' : ' empty') + (it && it.sel ? ' ticked' : '');
  d.draggable = !!it;
  d.innerHTML = `<div class="hd"><span class="no">${i + 1}</span>`
    + `<span class="nm">${it ? esc(it.name) : '—'}</span>`
    + (it ? `<input type="checkbox" class="sel" title="tick to send this slot"${it.sel ? ' checked' : ''}>`
          + '<button class="play" title="play on the FM-1 (not saved)">▶</button>'
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
