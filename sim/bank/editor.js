// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance bank editor UI — a 64-slot drag-drop grid on top of bank-model.js. No engine / emcc needed:
// load a .syx/.mid, drag a whole performance from the palette into a slot, drag a part (upper/lower) onto a
// slot's UPPER/LOWER, drag a slot onto another to reorder, rename, and export a bank .syx that
// `python3 tools/make_bank.py --from-device FILE.syx` turns into the factory bank.
//
// (Audition — hearing a slot and editing it on the panel — arrives when the sim compiles under emcc and the
//  wsim load/dump bridge is wired; the structural editing here is independent of that.)
import { parseBankFile, makeBankSyx, moveSlot, dropPart, cloneItem, renameSlot, nameOf } from './bank-model.js';

const NSLOT = 64;
let bank = new Array(NSLOT).fill(null);   // the 64 programs (performance items), null = empty
let palette = [];                          // sources to drag from: [{ item }] (performances and user patches)
let drag = null;                           // the live drag payload (the DnD API can only carry strings)

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s).replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
const status = (msg, bad) => { const s = $('status'); s.textContent = msg; s.className = bad ? 'help bad' : 'help'; };

// ---- loading a file: fill the 64 slots (intoSlots), and/or add its items to the palette ----
function load(file, intoSlots) {
  const r = new FileReader();
  r.onload = () => {
    const items = parseBankFile(new Uint8Array(r.result)).filter((i) => i.kind === 'P' || i.kind === 'U');
    if (!items.length) { status(`${file.name}: no JP-8000/8080 patches found`, true); return; }
    if (intoSlots) {
      const perfs = items.filter((i) => i.kind === 'P');
      for (let i = 0; i < NSLOT; i++) bank[i] = perfs[i] ? cloneItem(perfs[i]) : null;
    }
    for (const it of items) palette.push({ item: it });
    status(`${file.name}: ${items.length} items loaded` + (intoSlots ? ' into the slots' : ' into the palette'));
    render();
  };
  r.readAsArrayBuffer(file);
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
  d.className = 'slot' + (it ? '' : ' empty');
  d.draggable = !!it;
  d.innerHTML = `<div class="hd"><span class="no">${i + 1}</span>`
    + `<span class="nm">${it ? esc(it.name) : '—'}</span>`
    + (it ? '<button class="ren" title="rename">✎</button>' : '') + '</div>'
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
    + (it.kind === 'P' ? '<span class="chip" draggable="true" data-part="1" title="drag the lower patch">L</span>' : '');
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
}

// ---- wiring ----
$('loadBank').addEventListener('change', (e) => { if (e.target.files[0]) load(e.target.files[0], true); e.target.value = ''; });
$('addSrc').addEventListener('change', (e) => { if (e.target.files[0]) load(e.target.files[0], false); e.target.value = ''; });
$('export').addEventListener('click', exportSyx);
$('clearPal').addEventListener('click', () => { palette = []; render(); });
render();
