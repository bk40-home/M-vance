// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance sound editor (Kris 15 Nov 2026: fm1-port/mvance-library-editor-proposal-2026-11-15.md, 5a 6a). The sound
// half of the old bank editor (Part B, 13 Nov: sim/bank/editor.js), on its own page; the banks went to the patch
// library (docs/library.html, fm1-kit's page with M-vance's adapter).
//
//   the sound   one edit buffer (bank/link.js: link.cur), kept the same as what plays in the sim on this page and,
//               linked, on the FM-1 (both ways: the FM-1's knobs show here, the tiles turn it). base: the sound as
//               it was opened (from the library, the sim's program at start, the FM-1's when linked, a program
//               written); Revert goes back to it, Compare plays it
//   edit        every page as a tile (../pages.js, the firmware's own drawing: 5a); the names; the computer keys
//   library     its Edit sends {id, where, sound} on BroadcastChannel "mvance-edit"; this page answers "took" and
//               loads it. Keep sends {keep, id, sound}; the library puts it into that slot as a change ("kept")
//   write       Write to slot: the edit's 4 DT1s to 03 pp on the FM-1 (bank/fm1-midi.js; the FM-1 saves it)
//   sequences   the Sequences panel (seq-panel.js, Kris 20 Nov): the two tracks and the clips of the device the
//               sound is on (the sim; linked, the FM-1), over the sequencer's SysEx area 04 (mv_seqx.h)
// Nothing is sent to the FM-1 unless Connect, then Link or Write. ?load=/path.syx (same site): that file's first
// program becomes the sound (the sound-match tool's "Open in editor").
import { parseBankFile, nameOf, soundOf, soundCopy, soundEqual, itemFromSound, perfMsgs } from './bank/bank-model.js?v=__BUILD__';
import { FM1 } from './bank/fm1-midi.js?v=__BUILD__';
import { Link, GAP_MS } from './bank/link.js?v=__BUILD__';
import { mountPages } from './pages.js?v=__BUILD__';
import { startEngine } from './engine.js?v=__BUILD__';
import { mountSeq } from './seq-panel.js?v=__BUILD__';

const NPROG = 64;                          // program slots on the FM-1
const KEEP_WAIT_MS = 1500;                 // Keep: the library answers within this, or it is not open
const $ = (id) => document.getElementById(id);
const esc = (s) => String(s).replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
// the status line: bad = true (warn colour), 'ok', else plain
const status = (msg, bad) => { const s = $('status'); s.textContent = msg; s.className = bad === 'ok' ? 'help ok' : bad ? 'help bad' : 'help'; };

let engine = null, link = null, seq = null;
let base = null;                           // the sound as opened (Revert, Compare, "modified")
let comparing = null;                      // comparing: the edit, held while base plays
let what = "the sim's program";            // what is being edited (the edit line)
let lib = null;                            // opened from the library: {id, where} (Keep goes back there)

// ---- the FM-1 (WebMIDI) ----
let busy = false;                          // a write is running
const fm1 = new FM1({
  onProgress: (v, max) => { const p = $('prog'); p.hidden = max === 0; p.max = max || 1; p.value = v; },
  onPorts: (ports) => fillPorts(ports),
  onTemp: (m) => { if (link) link.fromFm1(m); },     // the FM-1's sound: a dump or a change (linked)
  onProgram: (n) => fm1Program(n),                   // PRESETS turned on the FM-1
  onSeq: (m) => { if (seq && seqDev().kind === 'fm1') seq.feed(m); },   // the FM-1's tracks and clips (linked)
});
// where the Sequences panel works: the FM-1 while linked (paced, the sound's link held meanwhile), else the sim
function seqDev() {
  if (link && link.fm1 && fm1.out) return {
    kind: 'fm1', label: 'FM-1',
    send: async (ms) => { link.hold = true; try { await fm1.send(ms); } finally { link.hold = false; } },
    rt: (st) => fm1.sendNow(new Uint8Array([st])),
  };
  return { kind: 'sim', label: 'sim', send: async (ms) => { for (const m of ms) engine.sysexIn(m); }, rt: (st) => engine.midi(st, 0, 0) };
}
async function connect() {
  try { fillPorts(await fm1.connect()); } catch (e) { status(e.message, true); }
}
function fillPorts(p) {                    // both port lists, the choice kept or guessed, then used
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
  status(ok ? `Connected: ${fm1.out.name}. Link FM-1 edits its sound here; Write to slot puts the edit into a program.`
            : 'No FM-1 port: plug it in (USB) and pick it above.', ok ? 'ok' : true);
  if (!ok && link && link.fm1) setLink(false);       // the FM-1 went: unlinked
  soundUi();
}

// ---- the edit line and its buttons (cheap: on every change, at most once a frame) ----
const modified = () => !!(link && base && !soundEqual(link.cur, base));
// an unkept edit is dropped only if the user says so (true: go on)
const dropEdit = () => !modified() || comparing || confirm('Drop the changes not kept or written?');
let uiQueued = false;
function scheduleUi() { if (!uiQueued) { uiQueued = true; requestAnimationFrame(() => { uiQueued = false; soundUi(); }); } }
function soundUi() {
  $('link').disabled = !link || !fm1.ready;
  $('write').disabled = !link || !fm1.out || busy || !!comparing;
  if (!link) return;
  const m = modified();
  $('editing').innerHTML = `Editing ${esc(what)}: "${esc(nameOf(link.cur.up))}"`
    + (comparing ? ' — <span class="mod">comparing: as opened</span>' : m ? ' <span class="mod">● modified</span>' : '');
  $('from').textContent = lib ? `From the library: ${lib.where}. Keep puts the edit back there.` : '';
  $('keep').disabled = !lib || !!comparing;
  $('revert').disabled = !m || !!comparing;
  $('compare').disabled = !base || (!m && !comparing);
  $('compare').textContent = comparing ? 'Back to the edit' : 'Compare';
  $('save').disabled = !!comparing;
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

// ---- the sound ----
// a new sound to edit: it becomes base and plays (sim; linked: the FM-1 too)
function open(sound, label, from) {
  comparing = null; base = soundCopy(sound); what = label; lib = from || null;
  link.load(base);
  soundUi();
}
// the names: a patch's first 16 bytes, ASCII 32..125 (as the FM-1's SAVE page); each keystroke is an edit
const nameBytes = (str) => Array.from((str + ' '.repeat(16)).slice(0, 16), (c) => { const v = c.charCodeAt(0); return v >= 32 && v <= 125 ? v : 32; });
function setName(lower, str) {
  if (!link || comparing) return;
  const s = soundCopy(link.cur);
  (lower ? s.lo : s.up).set(nameBytes(str), 0);
  link.load(s);
}
function revert() { if (link && base) { comparing = null; link.load(base); } }
function compare() {                       // as opened, then back to the edit
  if (!link || !base) return;
  if (comparing) { const e = comparing; comparing = null; link.load(e); }
  else { comparing = soundCopy(link.cur); link.load(base); }
  soundUi();
}
function setLink(on) {
  if (!link) return;
  if (on) {
    if (!dropEdit()) { $('link').checked = false; return; }
    comparing = null; lib = null;          // (the FM-1's sound comes in: the library's is no longer the one edited)
    link.linkFm1((m) => fm1.sendNow(m));
    engine.muted = true;                   // (you hear the FM-1)
    status('Linking: reading the FM-1\'s sound...');
  } else {
    link.unlinkFm1(); engine.muted = false;
    if (seq) seq.readAll();                // (the panel shows the sim's tracks again)
    status('Unlinked: the sim plays the edit again; the FM-1 keeps what it has.', 'ok');
  }
  soundUi();
}
function fm1Program(n) {                   // PRESETS turned on the FM-1 (linked): its reports bring the sound here
  if (!link || !link.linked) return;
  const lost = modified() && !comparing;
  comparing = null; lib = null; what = `FM-1 program ${n + 1}`; base = null;
  setTimeout(() => { if (link) { base = soundCopy(link.cur); soundUi(); } }, 600);   // (its reports take a moment)
  soundUi();
  status(`The FM-1 changed to program ${n + 1}` + (lost ? ': the edit here went too, as on the FM-1.' : '.'), lost ? true : 'ok');
}
// Write to slot: the edit into program slot n on the FM-1 (it saves it to flash when nothing sounds)
async function write() {
  if (!link || !fm1.out || busy) return;
  const a = prompt('Write the edit into FM-1 program slot (1-64):', '1');
  if (a == null) return;
  const n = parseInt(a, 10) - 1;
  if (!(n >= 0 && n < NPROG)) { status(`"${a}" is not a slot (1-64).`, true); return; }
  if (!confirm(`Write "${nameOf(link.cur.up)}" into program ${n + 1} on the FM-1? What is there now is replaced.`)) return;
  busy = true; link.hold = true; soundUi();
  try {
    await fm1.sendSlots([[itemFromSound(link.cur), n]]);
    base = soundCopy(link.cur); what = `FM-1 program ${n + 1}`;
    status(`Written to program ${n + 1}. The FM-1 saves it a moment after (when nothing sounds); the library's Read shows it.`, 'ok');
  } catch (e) { status(e.message, true); }
  link.hold = false; busy = false; soundUi();
}
function saveSyx() {                       // the edit as one program (03 00: slot 1), a file the library opens
  if (!link) return;
  const ms = perfMsgs(itemFromSound(link.cur), 0x03, 0x00), out = new Uint8Array(ms.reduce((t, m) => t + m.length, 0));
  let o = 0; for (const m of ms) { out.set(m, o); o += m.length; }
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([out], { type: 'application/octet-stream' }));
  a.download = (nameOf(link.cur.up).trim().replace(/[^\w.-]+/g, '_') || 'mvance') + '.syx';
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

// ---- the library (6a): Edit comes in, Keep goes back ----
const chan = typeof BroadcastChannel === 'function' ? new BroadcastChannel('mvance-edit') : null;
let incoming = null;                       // an Edit that came before the sim was ready
let keepT = 0;
function fromLibrary(m) {
  const sound = { up: Uint8Array.from(m.sound.up), lo: Uint8Array.from(m.sound.lo), pf: Uint8Array.from(m.sound.pf) };
  if (!dropEdit()) { status(`Not opened: "${m.name}" from the library (the edit here was kept).`, true); return; }
  open(sound, m.where, { id: m.id, where: m.where });   // (linked: it goes to the FM-1 too, as Part B's Edit)
  status(`Opened ${m.name} from the library. Keep in library puts the edit back into ${m.where}.`, 'ok');
  window.focus();
}
if (chan) {
  chan.onmessage = (e) => {
    const m = e.data || {};
    if (m.type === 'edit') {
      chan.postMessage({ type: 'took', id: m.id });
      if (link && link.sim.has) fromLibrary(m); else incoming = m;   // (before the sim's first dump: after it)
    } else if (m.type === 'kept' && lib && m.id === lib.id) {
      clearTimeout(keepT);
      if (m.ok) { base = soundCopy(link.cur); status(`Kept in the library: ${lib.where}. Send there writes it to the FM-1.`, 'ok'); }
      else status(`The library did not keep it: ${m.why}`, true);
      soundUi();
    }
  };
}
function keep() {
  if (!lib || !chan || !link) return;
  chan.postMessage({ type: 'keep', id: lib.id, sound: { up: Array.from(link.cur.up), lo: Array.from(link.cur.lo), pf: Array.from(link.cur.pf) } });
  clearTimeout(keepT);
  keepT = setTimeout(() => status('The library page is closed (or was reloaded): open the program there with Edit again, '
                                  + 'or Download .syx and open that in the library.', true), KEEP_WAIT_MS);
}

// ---- the computer keys (Z S X ... as the simulator page): the sim, or, linked, the FM-1 over MIDI ----
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

// ?load=/path.syx (same site only: a leading /, so a link cannot make the page fetch from elsewhere)
async function loadUrl() {
  const u = new URLSearchParams(location.search).get('load');
  if (!u || !u.startsWith('/') || u.startsWith('//')) return;
  const name = decodeURIComponent(u.split('/').pop());
  try {
    const r = await fetch(u);
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const it = parseBankFile(new Uint8Array(await r.arrayBuffer()))[0];
    if (!it) throw new Error('no JP-8000/8080 patch in it');
    open(soundOf(it), name);
    status(`${name} is playing here. Link FM-1 to hear it there; Write to slot keeps it on the FM-1.`, 'ok');
  } catch (e) { status(`could not load ${name}: ${e.message}`, true); }
}

async function start() {
  try { engine = await startEngine({ base: './', stamp: '?v=__BUILD__', screen: $('screen') }); }
  catch (e) {                              // no simulator built: nothing to edit
    $('sound').hidden = true; document.querySelector('details.pages').hidden = true; $('nosim').style.display = 'block';
    console.warn('editor: no simulator:', e.message);
    return;
  }
  link = new Link({
    simSend: (m) => engine.sysexIn(m),
    onChange: (why, dev) => {
      if (why === 'adopt') {               // a device's sound became the edit buffer
        comparing = null; base = soundCopy(link.cur);
        if (dev === 'fm1') { what = "the FM-1's sound"; status('Linked: the FM-1\'s sound is here. Its knobs show on the tiles; the tiles turn it.', 'ok'); seq.readAll(); }
        else if (incoming) { const m = incoming; incoming = null; fromLibrary(m); }   // (an Edit waited for the sim)
        else loadUrl();
        soundUi();
      } else scheduleUi();
    },
  });
  seq = mountSeq($('seq'), { dev: seqDev, status });
  engine.onSysEx = (m) => {                // area 04: the panel's (only while it works on the sim); the rest: the link's
    if (m[5] === 0x12 && m[6] === 0x04) { if (seqDev().kind === 'sim') seq.feed(m); return; }
    link.fromSim(m);
  };
  link.linkSim();
  seq.readAll();
  setInterval(() => link.tick(), GAP_MS);
  mountPages(engine.M, $('pages'), { sizeInput: $('tileSize'),
    // FM-1-wide settings, not the sound's: greyed while linked (they would change the sim only)
    pageEditable: (title, btn) => !(link.fm1 && (['REVERB', 'GLOBAL', 'MIDI'].includes(title) || ['ARP', 'SEQ'].includes(btn))) });
  $('audio').addEventListener('click', () => engine.startAudio().then(() => { $('audio').textContent = '▶ Audio on'; }));
  if (chan) chan.postMessage({ type: 'hello' });   // (an Edit sent while this page was opening comes again)
  soundUi();
}

// ---- wiring ----
$('connect').addEventListener('click', connect);
$('outSel').addEventListener('change', usePorts);
$('inSel').addEventListener('change', usePorts);
$('link').addEventListener('change', () => setLink($('link').checked));
$('keep').addEventListener('click', keep);
$('write').addEventListener('click', write);
$('revert').addEventListener('click', revert);
$('compare').addEventListener('click', compare);
$('save').addEventListener('click', saveSyx);
$('nameUp').addEventListener('input', (e) => setName(false, e.target.value));
$('nameLo').addEventListener('input', (e) => setName(true, e.target.value));
for (const id of ['nameUp', 'nameLo']) $(id).addEventListener('blur', soundUi);   // (shows it as kept: trimmed)
addEventListener('beforeunload', (e) => { if (modified() && !comparing) { e.preventDefault(); e.returnValue = ''; } });
start();
