// SPDX-License-Identifier: GPL-3.0-only
// sim/seq-panel.js in jsdom against a pretend device that speaks area 04 as mv_seqx.c does: read, Open, Send, Save,
// reports (Take / follow), Play / Stop, Clear; a MIDI file (the tool's loop, transpose, LENGTH, start, undo / redo,
// Auto loop, over 128 notes); the roll's edits (add, delete, move, copy, velocity, Ctrl+Z); the shelf. Then the
// Factory clips dialog (Kris 21 Nov): a folder, ticks, order, names, loops, Save / Open clips.txt, ▶ into a lane.
//   npm i jsdom (anywhere on the module path), then: node sim/seq-panel-test.mjs
let JSDOM;
try { ({ JSDOM } = await import('jsdom')); } catch { console.log('seq-panel: jsdom is not installed: skipped'); process.exit(0); }
const dom = new JSDOM('<!doctype html><body><section id="seq"></section></body>', { pretendToBeVisual: true, url: 'http://localhost/' });
Object.assign(globalThis, { window: dom.window, document: dom.window.document, getComputedStyle: dom.window.getComputedStyle, localStorage: dom.window.localStorage,
  requestAnimationFrame: (f) => setTimeout(f, 0), addEventListener: () => {} });
dom.window.HTMLCanvasElement.prototype.getContext = () => new Proxy({}, { get: () => () => {} });
let answers = { confirm: true, prompt: [] };
globalThis.confirm = () => answers.confirm;
globalThis.prompt = () => answers.prompt.shift();
const D = process.argv[2] || new URL('.', import.meta.url).pathname.replace(/\/$/, '');
const S = await import(`${D}/seq-model.js`);
const { mountSeq } = await import(`${D}/seq-panel.js`);
const MI = await import(`${D}/seq-midi.js`);

let checks = 0, fails = 0;
const check = (ok, what) => { checks++; if (!ok) { fails++; console.log('FAIL', what); } };
const tick = (ms = 5) => new Promise((r) => setTimeout(r, ms));

// ---- the pretend device ----
const mk = (bars, root, name, notes) => ({ bars, root, quant: 1, name, events: S.eventsOf(notes, bars) });
const dev = {
  tracks: [mk(2, 48, '', [{ t: 0, len: 48, n: 48, v: 100 }]), mk(1, 36, '', [{ t: 0, len: 96, n: 36, v: 100 }])],
  user: Array.from({ length: 12 }, (_, k) => (k === 2 ? mk(4, 40, 'BASS1', [{ t: 0, len: 96, n: 40, v: 90 }]) : null)),
  fac: [mk(8, 60, 'SATURN LEAD', [{ t: 0, len: 48, n: 60, v: 100 }, { t: 96, len: 48, n: 62, v: 100 }]),
        mk(1, 50, 'SPACE', [{ t: 0, len: 12, n: 50, v: 100 }]), mk(16, 70, 'ARISEN', [{ t: 384, len: 96, n: 70, v: 100 }])],
  ver: 7, stage: null, out: [], rt: [],
  rx(m) {
    const cmd = m[5], tt = m[7], off = (m[8] << 7) | m[9], d = Array.from(m.subarray(10, m.length - 2));
    const get = () => (tt < 2 ? { ...this.tracks[tt], flags: 0, ver: this.ver } : tt < 0x20 ? this.user[tt - 0x10] && { ...this.user[tt - 0x10], flags: 0 }
      : this.fac[tt - 0x20] && { ...this.fac[tt - 0x20], flags: 3 });
    if (cmd === 0x11) {
      if (tt >= 0x20 && !this.fac[tt - 0x20]) return;
      const c = get() || { ...S.emptyClip(2), flags: 4 };
      const h = S.headBytes(c); h[5] = c.flags; h[14] = this.ver & 127;
      if (tt >= 0x20) for (let i = 0; i < 16; i++) h[16 + i] = (c.name + ' '.repeat(16)).charCodeAt(i);
      this.out.push(S.dt1(S.addrOf(tt, 0), h));
      if (m[13] !== 0x20) for (const x of S.writeMsgs(tt, c).slice(0, -1)) this.out.push(x);
      return;
    }
    if (tt >= 0x20) return;
    if (off >= 32) { const i0 = (off - 32) / 4; if (i0 === 0) this.stage = { tt, ev: [] };
      for (let i = 0; i < d.length; i += 4) this.stage.ev.push({ t: (d[i] << 7) | d[i + 1], n: d[i + 2], v: d[i + 3] }); return; }
    const h = S.parseHead(d), ev = h.n ? (this.stage && this.stage.tt === tt ? this.stage.ev : null) : [];
    if (!ev) return;
    const c = { bars: h.bars, root: h.root, quant: h.quant, name: tt < 2 ? '' : String.fromCharCode(...d.slice(8, 14)).trim(), events: ev };
    if (S.validate(c)) return;
    if (tt < 2) { this.tracks[tt] = c; this.ver++; } else this.user[tt - 0x10] = c;
    this.stage = null;
  },
  report(p) { const h = S.headBytes(this.tracks[p]); h[14] = this.ver & 127; this.out.push(S.dt1(S.addrOf(p, 0), h)); },
};
let panel;
const flush = async () => { for (let i = 0; i < 50 && dev.out.length; i++) { panel.feed(dev.out.shift()); } await tick(); };
const statusLog = [];
panel = mountSeq(document.getElementById('seq'), {
  dev: () => ({ kind: 'sim', label: 'sim', send: async (ms) => { for (const m of ms) dev.rx(m); setTimeout(flush, 0); }, rt: (st) => dev.rt.push(st) }),
  status: (m, bad) => statusLog.push([m, bad]),
});
const $ = (s) => document.querySelector(s);
const lane = (p) => $(`.lane[data-p="${p}"]`);
const click = (el) => el.dispatchEvent(new dom.window.MouseEvent('click', { bubbles: true }));
const change = (el, v) => { el.value = v; el.dispatchEvent(new dom.window.Event('change', { bubbles: true })); };
const settle = async () => { for (let i = 0; i < 12; i++) { await flush(); await tick(); } await tick(700); await flush(); await tick(); };

await panel.readAll(); await settle();
check(JSON.stringify(panel.lanes[0].clip.events) === JSON.stringify(dev.tracks[0].events), 'read: UPPER lane = the device track');
check(panel.lanes[1].clip.bars === 1, 'read: LOWER lane LENGTH 1');
const opts = [...lane(0).querySelectorAll('select[data-a="open"] option')].map((o) => o.textContent);
check(opts.some((t) => t.startsWith('3 BASS1')) && opts.some((t) => t.startsWith('F3 ARISEN')) && !opts.some((t) => t.startsWith('F4')), `open list: user 3 named, F1-F3 (${opts.length})`);
check(lane(0).querySelector('.lane-mod').textContent === '', 'not modified after read');

change(lane(0).querySelector('select[data-a="open"]'), String(0x21)); await settle();
check(panel.lanes[0].clip.bars === 1 && panel.lanes[0].clip.events.length === 2, 'Open F2 into UPPER');
check(lane(0).querySelector('.lane-mod').textContent.includes('not sent'), 'F2 shown as not sent');
click(lane(0).querySelector('[data-a="send"]')); await settle();
check(JSON.stringify(dev.tracks[0].events) === JSON.stringify(dev.fac[1].events) && dev.tracks[0].bars === 1, 'Send: the device UPPER track = F2');
check(lane(0).querySelector('.lane-mod').textContent === '', 'after Send: not modified');

change(lane(1).querySelector('select[data-a="open"]'), String(0x22)); await settle();
change(lane(1).querySelector('input[data-a="bars"]'), '1'); await tick(20);
check(lane(1).querySelector('[data-a="send"]').disabled && /past the loop/.test(lane(1).querySelector('.lane-info').textContent), 'LENGTH 1 under a 16-bar clip: refused, said why');
change(lane(1).querySelector('input[data-a="bars"]'), '16'); await tick(20);
check(!lane(1).querySelector('[data-a="send"]').disabled, 'LENGTH 16 again: sendable');
answers.prompt = ['5', 'lead'];
click(lane(1).querySelector('[data-a="save"]')); await settle();
check(dev.user[4] && dev.user[4].name === 'LEAD' && dev.user[4].events.length === 2, 'Save to clip 5 "LEAD"');
check(statusLog.at(-1)[1] === 'ok' && /user clip 5/.test(statusLog.at(-1)[0]), 'Save: reported ok');

// a report: the device changed LOWER itself. The lane holds F1 (unsent) -> asks; Take -> follows
dev.tracks[1] = mk(1, 36, '', [{ t: 0, len: 24, n: 38, v: 99 }]); dev.ver++; dev.report(1); await settle();
check(!lane(1).querySelector('.lane-ask').hidden, 'report over an unsent change: Take / Keep mine');
click(lane(1).querySelector('[data-a="take"]')); await settle();
check(panel.lanes[1].clip.events[0].n === 38 && lane(1).querySelector('.lane-ask').hidden, 'Take: the lane = the device track');
dev.tracks[1] = mk(1, 36, '', [{ t: 0, len: 24, n: 41, v: 99 }]); dev.ver++; dev.report(1); await settle();
check(panel.lanes[1].clip.events[0].n === 41, 'report with no unsent change: the lane follows');
click($('[data-a="play"]')); click($('[data-a="stop"]'));
check(JSON.stringify(dev.rt) === '[250,252]', 'Play / Stop: MIDI START / STOP');
click(lane(0).querySelector('[data-a="clear"]')); await tick(20); click(lane(0).querySelector('[data-a="send"]')); await settle();
check(dev.tracks[0].events.length === 0, 'Clear + Send: the track emptied');

// ---- P3: a MIDI file: the tool's loop, then a cut of its own ----
const pat = [];
for (let bar = 0; bar < 8; bar++) for (let i = 0; i < 4; i++) pat.push({ t: bar * 384 + i * 96, len: 48, n: 48 + ((bar % 2) * 4 + i) % 7, v: 100 });
const song = MI.writeMid({ bars: 8, events: S.eventsOf(pat, 8) }, 'Bassline');
const asFile = (name, b) => ({ name, arrayBuffer: async () => b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength) });
await panel.openFile(0, asFile('song.mid', song)); await settle();
const L0 = panel.lanes[0];
check(L0.clip.bars === 2 && S.noteCount(L0.clip) === 8 && /repeats every 2 of 8/.test(lane(0).querySelector('.lane-how').textContent), 'open .mid: the tool loop (2 of 8 bars)');
check(!lane(0).querySelector('.lane-imp').hidden && L0.clip.name === 'BASSLI', 'the import bar shows; the clip named from the part');
change(lane(0).querySelector('input[data-a="xpose"]'), '12'); await settle();
check(L0.clip.root === 60, 'transpose +12: ROOT C4');
change(lane(0).querySelector('input[data-a="bars"]'), '1'); await settle();
check(L0.clip.bars === 1 && S.noteCount(L0.clip) === 4, 'LENGTH 1: one bar of the file');
change(lane(0).querySelector('input[data-a="start"]'), '2'); await settle();
check(L0.clip.events.find((e) => e.v).n === 52 + 12, 'from bar 2: its notes');
click(lane(0).querySelector('[data-a="undo"]')); await settle();
check(L0.clip.events.find((e) => e.v).n === 48 + 12, 'Undo: back to bar 1');
click(lane(0).querySelector('[data-a="redo"]')); await settle();
check(L0.clip.events.find((e) => e.v).n === 52 + 12, 'Redo');
click(lane(0).querySelector('[data-a="auto"]')); await settle();
check(L0.clip.bars === 2 && L0.clip.root === 48, 'Auto loop again: the tool loop, transpose reset');
// a long part: over 128 notes, not sendable, factory clip offered
const long = Array.from({ length: 200 }, (_, i) => ({ t: i * 48, len: 24, n: 40 + (i * 7) % 30, v: 100 }));
await panel.openFile(1, asFile('long.mid', MI.writeMid({ bars: 25, events: S.eventsOf(long, 25) }, 'Long'))); await settle();
check(lane(1).querySelector('[data-a="send"]').disabled && /factory clip/.test(lane(1).querySelector('.lane-info').textContent), 'over 128 notes: not sendable, factory clip offered');
check(!lane(1).querySelector('[data-a="factory"]').disabled, 'Factory clip… enabled');

// ---- P4: the roll edits ----
const cv = lane(0).querySelector('canvas');
const pe = (type, x, y, o = {}) => cv.dispatchEvent(new dom.window.MouseEvent(type, { clientX: x, clientY: y, button: 0, bubbles: true, ...o }));
const n0 = S.noteCount(L0.clip);
const roll = L0.roll, gx = (t) => 34 + t * (600 - 34) / (L0.clip.bars * 384) + 2, gy = (n) => (roll.hi - 1 - n) * (180 / (roll.hi - roll.lo)) + 3;
pe('pointerdown', gx(600), gy(70)); pe('pointerup', gx(600), gy(70)); await settle();
check(S.noteCount(L0.clip) === n0 + 1 && L0.clip.events.some((e) => e.v && e.n === 70 && e.t === 600), 'click on nothing: a note (snapped to 1/16)');
cv.dispatchEvent(new dom.window.KeyboardEvent('keydown', { key: 'Delete', bubbles: true })); await settle();
check(S.noteCount(L0.clip) === n0, 'Delete: the new note (selected) gone');
const first = S.notesOf(L0.clip)[0];
pe('pointerdown', gx(first.t + 5), gy(first.n)); pe('pointermove', gx(first.t + 5 + 96), gy(first.n + 2)); pe('pointerup', gx(first.t + 5 + 96), gy(first.n + 2)); await settle();
check(L0.clip.events.some((e) => e.v && e.t === first.t + 96 && e.n === first.n + 2) && S.noteCount(L0.clip) === n0, 'drag: the note moved a beat on, 2 up');
pe('pointerdown', gx(first.t + 96 + 5), gy(first.n + 2), { altKey: true }); pe('pointermove', gx(first.t + 96 + 5 + 192), gy(first.n + 2), { altKey: true }); pe('pointerup', gx(first.t + 96 + 5 + 192), gy(first.n + 2)); await settle();
check(S.noteCount(L0.clip) === n0 + 1, 'alt+drag: a copy');
cv.dispatchEvent(new dom.window.WheelEvent('wheel', { clientX: gx(first.t + 96 + 5), clientY: gy(first.n + 2), deltaY: -1, bubbles: true, cancelable: true })); await settle();
check(L0.clip.events.some((e) => e.t === first.t + 96 && e.n === first.n + 2 && e.v === first.v + 8), 'wheel up over a note: velocity +8');
cv.dispatchEvent(new dom.window.KeyboardEvent('keydown', { key: 'z', ctrlKey: true, bubbles: true })); await settle();
check(L0.clip.events.some((e) => e.t === first.t + 96 && e.n === first.n + 2 && e.v === first.v), 'Ctrl+Z: velocity back');
check(S.validate(L0.clip) === null, 'after the edits: a valid clip');
click(lane(0).querySelector('[data-a="send"]')); await settle();
check(JSON.stringify(dev.tracks[0].events) === JSON.stringify(L0.clip.events), 'the edited lane sent');

// ---- the shelf ----
answers.prompt = ['my bass'];
click(lane(0).querySelector('[data-a="shelve"]')); await settle();
check(JSON.parse(localStorage.getItem('mvance-seq-shelf')).length === 1 && /my bass/.test(document.querySelector('.shelf-list').textContent), 'Shelve: kept in this browser');
click(document.querySelector('[data-a="sh-lo"]')); await settle();
check(JSON.stringify(panel.lanes[1].clip.events) === JSON.stringify(L0.clip.events), 'the shelf -> LOWER');

// ---- the Factory clips dialog (Kris 21 Nov) ----
{
  const vq = (v) => { const o = [v & 127]; while ((v = Math.floor(v / 128))) o.unshift((v & 127) | 128); return o; };
  const midFile = (tracks) => {             // format 1 at 96 a quarter; tracks: [[name, [[tick, note, len]]]]
    const trs = tracks.map(([name, ns]) => {
      const ev = []; for (const [t, n, d] of ns) ev.push([t, 1, [0x90, n, 100]], [t + d, 0, [0x80, n, 0]]);
      ev.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
      const tr = name ? [0, 0xff, 3, ...vq(name.length), ...Array.from(name, (c) => c.charCodeAt(0))] : [];
      let last = 0; for (const [t, , by] of ev) { tr.push(...vq(t - last), ...by); last = t; }
      tr.push(0, 0xff, 0x2f, 0);
      return [0x4d, 0x54, 0x72, 0x6b, (tr.length >>> 24) & 255, (tr.length >>> 16) & 255, (tr.length >>> 8) & 255, tr.length & 255, ...tr];
    });
    return new Uint8Array([0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 1, 0, trs.length, 0, 96, ...trs.flat()]);
  };
  const qn = (n, bars, every = 96) => Array.from({ length: bars * 384 / every }, (_, i) => [i * every, n, every / 2]);
  const fileLike = (path, bytes) => ({ name: path.split('/').pop(), webkitRelativePath: path, arrayBuffer: async () => bytes.buffer });
  const folderFiles = [
    fileLike('Midi/song - one.mid', midFile([[null, []], ['Lead', qn(60, 4)], ['Bass', qn(36, 4)], ['Pad', qn(48, 4)]])),
    fileLike('Midi/two.mid', midFile([['MIDI Out', qn(62, 2)], ['MIDI Out', qn(64, 2)]])),
    fileLike('Midi/sub/arp.mid', midFile([['Arp', qn(70, 2, 24)]])),
    fileLike('Midi/sub/long.mid', midFile([['Long', Array.from({ length: 256 }, (_, i) => [i * 12, 40 + (i * 7) % 41, 6])]])),   // (256 notes, no repeat: factory only)
    fileLike('Midi/readme.txt', new Uint8Array([1, 2, 3])),
  ];
  const pk = panel.picker, P = () => pk.state(), el = document.querySelector('.picker');
  const srcs = () => P().sel.map((x) => `${x.rel}#${x.index}`);
  click(document.querySelector('[data-a="picker"]')); await settle();
  check(!el.hidden, 'Factory clips… opens the dialog');
  await pk.setFolder(folderFiles); await settle();
  check(P().files.size === 4 && JSON.stringify([...P().files.keys()]) === JSON.stringify(['song - one.mid', 'two.mid', 'sub/arp.mid', 'sub/long.mid']), `the folder: 4 .mid in the tool's order (${[...P().files.keys()]})`);
  check(JSON.stringify(srcs()) === JSON.stringify(['song - one.mid#1', 'song - one.mid#2', 'song - one.mid#3', 'two.mid#1', 'two.mid#2', 'sub/arp.mid#1', 'sub/long.mid#1']), `no clips.txt: every part ticked, path order (${srcs()})`);
  check(el.querySelectorAll('.pk-list li').length === 7 && /7 of 7 parts/.test(el.querySelector('.pk-info').textContent), 'the list shows F1-F7');
  const box = (rel, i) => el.querySelector(`input[data-k="tick"][data-r="${rel}"][data-i="${i}"]`);
  const tickBox = (rel, i, on) => { const b = box(rel, i); b.checked = on; b.dispatchEvent(new dom.window.Event('change', { bubbles: true })); };
  tickBox('song - one.mid', 2, false); tickBox('sub/long.mid', 1, false); await settle();
  check(JSON.stringify(srcs()) === JSON.stringify(['song - one.mid#1', 'song - one.mid#3', 'two.mid#1', 'two.mid#2', 'sub/arp.mid#1']), 'untick Bass and Long');
  tickBox('song - one.mid', 2, true); await settle();
  check(srcs().at(-1) === 'song - one.mid#2', 'tick Bass again: last (F6)');
  click(el.querySelector('button[data-k="up"][data-s="5"]')); await settle();
  check(srcs()[4] === 'song - one.mid#2', '↑: Bass is F5');
  const inp = (k, i, v) => { const x = el.querySelector(`input[data-k="${k}"][data-s="${i}"]`); x.value = v; x.dispatchEvent(new dom.window.Event('change', { bubbles: true })); };
  inp('name', 4, 'big bass'); inp('bars', 0, '1'); await settle();
  check(P().sel[4].name === 'BIG BASS' && P().sel[0].over && P().sel[0].over.bars === 1 && P().sel[0].over.start === 0, 'a name (upper case) and a loop (bars set: from bar the tool\'s)');
  click(el.querySelector('button[data-k="drop"][data-s="1"]')); await settle();
  check(P().sel.length === 5 && !srcs().includes('song - one.mid#3'), '✕: Pad not built');
  const txt = pk.save();
  const MI2 = MI.parseList(txt);
  check(MI2.only && JSON.stringify(MI2.lines.map((l) => [l.path, l.track, l.over && l.over.bars, l.name])) === JSON.stringify([
    ['song - one.mid', 'lead', 1, null], ['two.mid', '1', null, null], ['two.mid', '2', null, null], ['song - one.mid', 'bass', null, 'BIG BASS'], ['sub/arp.mid', 'arp', null, null]]),
    `Save: #!only, the parts in order; the same-named parts by number (${txt})`);
  const F = new Map([...P().files]);
  const back = MI.selectParts(F, MI2).items.map((it) => `${it.rel}#${it.part.index}`);
  check(JSON.stringify(back) === JSON.stringify(srcs()), 'the saved list picks exactly the dialog\'s parts, in its order (the tool\'s rules)');
  // Open clips.txt: its parts, order, names, loops; a line naming nothing kept and shown
  await pk.openList('#!only\nsub/arp.mid |  |  |  | MY ARP\nsong - one.mid | pad | 2 | 2\ngone.mid |  |  |\n'); await settle();
  check(JSON.stringify(srcs()) === JSON.stringify(['sub/arp.mid#1', 'song - one.mid#3']) && P().sel[0].name === 'MY ARP' && P().sel[1].over.start === 1, 'Open clips.txt: its parts, order, name and loop');
  check(P().miss.length === 1 && /gone\.mid/.test(el.querySelector('.pk-miss').textContent), 'a line naming nothing: shown');
  check(/gone\.mid/.test(pk.text()), '... and kept in the saved file');
  // ▶U: the part as the tool builds it, into UPPER and its track, then Play; the lane's unsent change shelved
  const before = panel.shelf().length;
  panel.lanes[0].clip = { ...S.clipCopy(panel.lanes[0].clip), events: [] };   // (an unsent change in the lane)
  dev.rt.length = 0;
  click(el.querySelector('button[data-k="hear"][data-p="0"][data-s="0"]')); await settle();
  const arp = MI.itemPageClip({ rel: 'sub/arp.mid', part: P().files.get('sub/arp.mid')[0], nparts: 1, over: null, name: 'MY ARP' });
  check(JSON.stringify(dev.tracks[0].events) === JSON.stringify(arp.events), '▶U: the tool\'s clip of the part in the UPPER track');
  check(dev.rt.includes(0xfa), '▶U: Play');
  check(panel.shelf().length === before + 1, '▶U: the lane\'s unsent change kept on the shelf');
  statusLog.length = 0;
  click(el.querySelector('button[data-k="hear"][data-p="1"][data-r="sub/long.mid"]')); await settle();
  check(statusLog.some(([m, bad]) => bad === true && /256 notes/.test(m)), '▶L on a 256-note part: refused (factory only), by name');
  click(el.querySelector('button[data-k="close"]')); await settle();
  check(el.hidden, 'Close');
}
console.log(`seq-panel: ${checks} checks, ${fails} failed`);
process.exit(fails ? 1 : 0);
