// SPDX-License-Identifier: GPL-3.0-only
// sim/seq-model.js on its own, and (with the host build) against the firmware: what the page writes, the FM-1's
// code takes and reads back the same.
//   node sim/seq-model-test.mjs [HOST_BINARY]   (the host build: python3 tests/seq_xfer_test.py makes build/host/mvance-seqx)
import * as S from './seq-model.js';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, readFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

let checks = 0, fails = 0;
const check = (ok, what) => { checks++; if (!ok) { fails++; console.log('FAIL', what); } };
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

// clips: chords and steps (A), 64 quarters (B), a note held over the loop's end (W), a full track (BIG)
const A = { bars: 2, root: 48, quant: 1, name: '', events: S.eventsOf([{ t: 0, len: 48, n: 48, v: 100 }, { t: 0, len: 48, n: 52, v: 100 },
  { t: 96, len: 48, n: 51, v: 90 }, { t: 384, len: 96, n: 60, v: 127 }], 2) };
const B = { bars: 8, root: 36, quant: 2, name: 'BASS1', events: S.eventsOf(Array.from({ length: 32 }, (_, i) => ({ t: i * 96, len: 40, n: 36 + (i % 4) * 3, v: 64 + i })), 8) };
const W = { bars: 1, root: 50, quant: 0, name: '', events: S.eventsOf([{ t: 300, len: 134, n: 50, v: 100 }], 1) };
const BIG = { bars: 8, root: 40, quant: 1, name: 'FULL', events: S.eventsOf(Array.from({ length: 128 }, (_, i) => ({ t: i * 24, len: 12, n: 40 + (i % 24), v: 100 })), 8) };

// ---- the model on its own ----
for (const [nm, c] of [['A', A], ['B', B], ['W', W], ['BIG', BIG]]) {
  check(S.validate(c) === null, `${nm}: valid (${S.validate(c)})`);
  check(same(S.eventsOf(S.notesOf(c), c.bars), c.events), `${nm}: notes -> events -> notes round trip`);
}
check(W.events[0].t === 50 && W.events[0].v === 0 && W.events[1].t === 300, 'W: the note over the loop end has its off first (tick 50)');
check(BIG.events.length === 256, 'BIG: 256 events');
check(S.validate({ ...BIG, events: [...BIG.events, { t: 3071, n: 1, v: 9 }, { t: 3071, n: 1, v: 0 }] }) !== null, 'more than 256 events: refused');
check(S.validate({ ...A, bars: 1 }) !== null, 'a note past the loop: refused');
check(S.validate({ ...A, events: [{ t: 0, n: 1, v: 9 }] }) !== null, 'an on without its off: refused');
check(S.validate({ ...A, events: [{ t: 9, n: 1, v: 9 }, { t: 0, n: 1, v: 0 }] }) !== null, 'out of time order: refused');
// overlapping notes of a pitch are cut where the next starts
const ov = S.eventsOf([{ t: 0, len: 200, n: 60, v: 100 }, { t: 96, len: 50, n: 60, v: 90 }], 1);
check(S.validate({ bars: 1, events: ov }) === null && same(S.notesOf({ bars: 1, events: ov }).map((x) => x.len), [96, 50]), 'overlaps cut: one pitch alternates');

// the write's messages read back by SeqReader (the device answers in the same layout)
const got = [];
const rd = new S.SeqReader({ onClip: (tt, c) => got.push([tt, c]) });
for (const tt of [0, 0x13]) rd.expect(tt);
// (a read comes head first: reorder the write's messages to read order)
for (const [tt, c] of [[0, A], [0x13, B]]) { const ms = S.writeMsgs(tt, c); [ms.at(-1), ...ms.slice(0, -1)].forEach((m) => rd.feed(m)); }
check(got.length === 2 && same(got[0][1].events, A.events) && same(got[1][1].events, B.events) && got[1][1].name === 'BASS1', 'SeqReader: two clips read back');
check(S.writeMsgs(0x10, BIG).length === 9 && Math.max(...S.writeMsgs(0x10, BIG).map((m) => m.length)) === 140, 'BIG: 8 event messages + head, 140 bytes at most');
const heads = [];
new S.SeqReader({ onHead: (tt, h) => heads.push([tt, h]) }).feed(S.writeMsgs(1, W).at(-1));
check(heads.length === 1 && heads[0][0] === 1 && heads[0][1].n === 2 && heads[0][1].bars === 1, 'an unasked head (a report) goes to onHead');

// ---- against the firmware (host build): the page's messages written, then read back by the firmware ----
const exe = process.argv[2];
if (exe && existsSync(exe)) {
  const dir = mkdtempSync(join(tmpdir(), 'seqm-'));
  const plan = [[0, A], [1, W], [0x12, B], [0x13, BIG]];
  let t = 100, lines = [], k = 0;
  const put = (m) => { const f = join(dir, `m${k++}.syx`); writeFileSync(f, m); lines.push(`${t} syx ${f}`); t += 40; };
  // (a second user clip only once the first is in flash: the FM-1 keeps one clip write waiting at a time)
  for (const [tt, c] of plan) { if (tt === 0x13) t = 3000; for (const m of S.writeMsgs(tt, c)) put(m); }
  t = 6000;
  for (const [tt] of plan) { put(S.readMsg(tt)); t += 400; }
  lines.push(`${t + 500} end`);
  writeFileSync(join(dir, 'run.txt'), lines.join('\n') + '\n');
  execFileSync(exe, [join(dir, 'run'), join(dir, 'run.txt')], { stdio: 'ignore' });
  const dump = readFileSync(join(dir, 'run.syx'));
  const back = new Map();
  const r2 = new S.SeqReader({ onClip: (tt, c) => back.set(tt, c) });
  for (const [tt] of plan) r2.expect(tt);
  for (let i = 0; i < dump.length; ) { const j = dump.indexOf(0xf7, i); if (j < 0) break; r2.feed(dump.subarray(i, j + 1)); i = j + 1; }
  for (const [tt, c] of plan)
    check(back.has(tt) && same(back.get(tt).events, c.events) && back.get(tt).bars === c.bars && back.get(tt).root === c.root,
          `firmware: ${S.ttName(tt)} written by the page, read back the same`);
  check(back.get(0x12) && back.get(0x12).name === 'BASS1', 'firmware: user clip 3 keeps its name');
} else console.log('note: no host build given: the firmware round trip was not run');

console.log(`seq-model: ${checks} checks, ${fails} failed`);
process.exit(fails ? 1 : 0);
