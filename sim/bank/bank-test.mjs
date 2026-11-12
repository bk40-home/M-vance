// SPDX-License-Identifier: GPL-3.0-only
// Round-trip test for sim/bank/bank-model.js, run on the host with Node (no browser, no emcc):
//   python3 tools/make_bank.py tools/bank/public.txt     # -> build/mvance-public.syx (if not already built)
//   node sim/bank/bank-test.mjs                           # from the repo root
// Proves the model parses and re-serialises a make_bank.py bank byte-for-byte, that it is idempotent, and that
// the reorder / rename operations survive a round trip. Then fm1-midi.js against a pretend FM-1 (a mock WebMIDI
// access that answers a read with a bank's messages): sending, playing and reading back, byte for byte.
import { readFileSync } from 'node:fs';
import { parseBankFile, makeBankSyx, moveSlot, renameSlot, perfMsgs } from './bank-model.js';
import { FM1, dumpBytes, rq1 } from './fm1-midi.js';

const orig = new Uint8Array(readFileSync('build/mvance-public.syx'));
let fail = 0;
const ok = (cond, msg) => { console.log((cond ? 'OK   ' : 'FAIL ') + msg); if (!cond) fail = 1; };

const perfs = () => parseBankFile(orig).filter((i) => i.kind === 'P');
const P = perfs();
ok(P.length === 64, `parsed ${P.length} performances (want 64)`);

// 1. byte-identity with make_bank.py's own .syx
const s1 = makeBankSyx(P);
let same = s1.length === orig.length, at = -1;
if (same) for (let i = 0; i < orig.length; i++) if (s1[i] !== orig[i]) { same = false; at = i; break; }
ok(same, same ? `serialised bank byte-identical to make_bank output (${s1.length} B)`
              : `serialised bank differs (len ${s1.length} vs ${orig.length}, first diff @${at})`);

// 2. idempotent: parse(serialise) -> serialise again == the same bytes
const s2 = makeBankSyx(parseBankFile(s1).filter((i) => i.kind === 'P'));
ok(s2.length === s1.length && s2.every((b, i) => b === s1[i]), 're-parse / re-serialise is idempotent');

// 3. reorder: move slot 1 to position 2, serialise, re-parse, names swapped
const b = perfs(); const n0 = b[0].name, n1 = b[1].name;
moveSlot(b, 0, 1);
const re = parseBankFile(makeBankSyx(b)).filter((i) => i.kind === 'P');
ok(re[0].name === n1 && re[1].name === n0, `reorder: slots 1&2 are now '${re[0].name}' , '${re[1].name}'`);

// 4. rename survives a round trip
renameSlot(b[0], 'MY LEAD');
const re2 = parseBankFile(makeBankSyx(b)).filter((i) => i.kind === 'P');
ok(re2[0].name === 'MY LEAD', `rename slot 1 -> '${re2[0].name}'`);

// ---- 5..8: fm1-midi.js against a pretend FM-1 ----
// a mock WebMIDI access: one output that records what is sent (and answers an RQ1 03 00 00 00 by replaying `dev`'s
// messages, `cut` of them at most, on the input) and one input
function mockAccess(dev, cut = Infinity) {
  const sent = [];
  const inp = { id: 'in1', name: 'FM-1 MIDI', onmidimessage: null };
  const out = { id: 'out1', name: 'FM-1 MIDI', send(m) {
    sent.push(Uint8Array.from(m));
    if (m[5] === 0x11 && m[6] === 0x03) {                         // RQ1 for performances: answer, as the FM-1 does
      dev.slice(0, cut).forEach((d, k) => setTimeout(() => inp.onmidimessage && inp.onmidimessage({ data: d }), k));
    } } };
  return { sent, access: { outputs: new Map([['out1', out]]), inputs: new Map([['in1', inp]]), onstatechange: null } };
}
const msgsOf = (bytes) => { const o = []; let s = -1; for (let i = 0; i < bytes.length; i++) {
  if (bytes[i] === 0xf0) s = i; else if (bytes[i] === 0xf7 && s >= 0) { o.push(bytes.slice(s, i + 1)); s = -1; } } return o; };
const eqb = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const fast = { gapMs: 0, quietMs: 30, sleep: () => Promise.resolve() };

{ // 5. send the whole bank: the messages are make_bank.py's, in order
  const m = mockAccess([]); const fm = new FM1(fast);
  const ports = await fm.connect(m.access);
  ok(fm.use(ports.outId, ports.inId), `ports picked by name: ${ports.outId}, ${ports.inId}`);
  await fm.sendSlots(P.map((it, i) => [it, i]));
  const joined = new Uint8Array(m.sent.reduce((n, x) => n + x.length, 0)); let o = 0; for (const x of m.sent) { joined.set(x, o); o += x.length; }
  ok(m.sent.length === 256 && eqb(joined, orig), `send bank: ${m.sent.length} messages, byte-identical to make_bank output`);
}
{ // 6. play: the 4 messages to the temporary area 01 00
  const m = mockAccess([]); const fm = new FM1(fast); const pr = await fm.connect(m.access); fm.use(pr.outId, pr.inId);
  await fm.play(P[0]);
  const want = perfMsgs(P[0], 0x01, 0x00);
  ok(m.sent.length === 4 && m.sent.every((x, k) => eqb(x, want[k])) && m.sent.every((x) => x[6] === 0x01 && x[7] === 0x00),
     'play: 4 messages to the temporary area (01 00 ..)');
}
{ // 7. read the 64 from the pretend FM-1: the request, then the bank rebuilt byte for byte
  const m = mockAccess(msgsOf(orig)); const fm = new FM1(fast); const pr = await fm.connect(m.access); fm.use(pr.outId, pr.inId);
  const r = await fm.read();
  const back = parseBankFile(dumpBytes(r.msgs)).filter((i) => i.kind === 'P');
  ok(eqb(m.sent[0], rq1([0x03, 0, 0, 0], [0, 64, 0, 0])), 'read: asks with RQ1 03 00 00 00, size 00 40 00 00');
  ok(r.complete && back.length === 64 && eqb(makeBankSyx(back), orig), `read: ${r.got}/${r.want} messages, the bank rebuilt byte-identical`);
}
{ // 8. a read that stops short says so
  const m = mockAccess(msgsOf(orig), 100); const fm = new FM1(fast); const pr = await fm.connect(m.access); fm.use(pr.outId, pr.inId);
  const r = await fm.read();
  ok(!r.complete && r.got === 100, `read cut short: complete=${r.complete}, ${r.got} of ${r.want}`);
}

process.exit(fail);
