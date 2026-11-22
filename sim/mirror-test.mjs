// SPDX-License-Identifier: GPL-3.0-only
// sim/mirror.js (Link FM-1: what the FM-1 plays, mirrored into the sim; Kris 22 Nov), with a pretend FM-1 and sim,
// and (with the host build) against the firmware: the tracks the mirror writes into the sim are what the sim's
// firmware then holds.
//   node sim/mirror-test.mjs [HOST_BINARY]   (the host build: python3 tests/seq_xfer_test.py makes build/host/mvance-seqx)
import * as S from './seq-model.js';
import { Mirror, notesOnly, READ_GIVE_UP_MS } from './mirror.js';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, readFileSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

let checks = 0, fails = 0;
const check = (ok, what) => { checks++; if (!ok) { fails++; console.log('FAIL', what); } };
const same = (a, b) => JSON.stringify(Array.from(a)) === JSON.stringify(Array.from(b));
const flush = () => new Promise((r) => setTimeout(r, 0));

// a clip with automation (UPPER) and one without (LOWER)
const UP = { bars: 2, root: 48, quant: 1, name: 'LEAD', events: S.withPoints(S.eventsOf([{ t: 0, len: 48, n: 60, v: 100 },
  { t: 96, len: 48, n: 63, v: 90 }, { t: 384, len: 96, n: 67, v: 110 }], 2), [{ t: 0, cc: 74, v: 20 }, { t: 192, cc: 74, v: 100 }], 2) };
const LO = { bars: 4, root: 36, quant: 2, name: 'BASS', events: S.eventsOf(Array.from({ length: 16 }, (_, i) => ({ t: i * 96, len: 40, n: 36 + (i % 4), v: 100 })), 4) };
// the FM-1's answer to a whole read: the head (its seq.ver at 14..15) first, then the events
function answer(tt, c, ver = 0) {
  const ms = S.writeMsgs(tt, c), h = S.headBytes(c);
  h[14] = ver & 0x7f; h[15] = (ver >> 7) & 0x7f;
  return [S.dt1(S.addrOf(tt, 0), h), ...ms.slice(0, -1)];
}
const report = (tt, c, ver) => answer(tt, c, ver)[0];   // a linked report: the head alone

// ---- the pretend pages: own reads (the simulator page) ----
function rig(own = true) {
  const r = { sim: [], midi: [], asked: [], timers: [], states: 0 };
  r.m = new Mirror({
    simSysex: (m) => r.sim.push(m),
    simMidi: (st, a, b) => r.midi.push([st, a, b]),
    readFm1: own ? async (msgs) => { r.asked.push(...msgs); } : null,
    onState: () => r.states++,
    setTimeout: (f, ms) => { r.timers.push({ f, ms, on: true }); return r.timers.length; },
    clearTimeout: (id) => { if (id) r.timers[id - 1].on = false; },
  });
  return r;
}
const tracksIn = (msgs) => {                       // what the sim was given, read back as the firmware would hold it
  const out = new Map(), rd = new S.SeqReader({ onClip: (tt, c) => out.set(tt, c) });
  const byTt = new Map();
  for (const m of msgs) { const s = S.seqMsg(m); if (s) { if (!byTt.has(s.tt)) byTt.set(s.tt, []); byTt.get(s.tt).push(m); } }
  for (const [tt, ms] of byTt) { rd.expect(tt); [ms.at(-1), ...ms.slice(0, -1)].forEach((m) => rd.feed(m)); }
  return out;
};

{
  const r = rig();
  check(!r.m.fromFm1(new Uint8Array([0xf8])) && r.midi.length === 0, 'off: the clock is not taken');
  r.m.start(); await flush();
  check(r.asked.length === 1 && same(r.asked[0], S.readMsg(0)), 'start: UPPER asked for first (one read at a time)');
  answer(0, UP, 5).forEach((m) => check(r.m.fromFm1(m), 'its answer taken'));
  await flush();
  const got = tracksIn(r.sim);
  check(got.has(0) && same(JSON.stringify(got.get(0).events), JSON.stringify(notesOnly(UP).events)), 'UPPER into the sim: its notes');
  check(got.get(0).events.every((e) => !S.isPoint(e)) && S.pointCount(UP) === 2, 'its automation points left out');
  check(r.asked.length === 2 && same(r.asked[1], S.readMsg(1)), 'then LOWER asked for');
  answer(1, LO, 9).forEach((m) => r.m.fromFm1(m));
  await flush();
  check(tracksIn(r.sim).has(1) && S.clipEqual(tracksIn(r.sim).get(1), { ...LO, name: 'BASS' }), 'LOWER into the sim, the same');
  const n0 = r.asked.length;
  r.m.fromFm1(report(1, LO, 9)); await flush();
  check(r.asked.length === n0, 'a report of the version mirrored: nothing read');
  r.m.fromFm1(report(1, LO, 10)); r.m.fromFm1(report(1, LO, 11)); await flush();
  check(r.asked.length === n0 + 1 && same(r.asked.at(-1), S.readMsg(1)), 'a new version reported: that track read again (once)');
  // a read never answered: given up, the next one asked
  r.m.fromFm1(report(0, UP, 6)); await flush();
  const t = r.timers.filter((x) => x.on).at(-1);
  check(t && t.ms === READ_GIVE_UP_MS, 'a read waits at most READ_GIVE_UP_MS');
  t.f(); await flush();
  check(same(r.asked.at(-1), S.readMsg(0)), 'given up: the waiting UPPER read asked for');
  // the transport and the notes
  r.midi = [];
  for (const b of [0xfa, 0xf8, 0xf8, 0xfc]) check(r.m.fromFm1(new Uint8Array([b])), `0x${b.toString(16)} taken`);
  check(r.m.clock === true, 'a clock seen: CLK OUT is on');
  check(r.m.fromFm1(new Uint8Array([0x91, 60, 100])) && r.m.fromFm1(new Uint8Array([0x81, 60, 0])), 'the FM-1 keys\' notes taken');
  check(!r.m.fromFm1(new Uint8Array([0xc0, 3])) && !r.m.fromFm1(new Uint8Array([0xb0, 74, 3])), 'a program change, a CC: not the mirror\'s');
  r.m.note(0x90, 53, 100);
  check(same(r.midi.map((x) => x[0]), [0xfa, 0xf8, 0xf8, 0xfc, 0x91, 0x81, 0x90]), 'into the sim, in order');
  check(r.m.fromFm1(new Uint8Array([0xf0, 0x41, 0x10, 0x00, 0x06, 0x12, 0x01, 0x00, 0x40, 0x00, 0x10, 0x2f, 0xf7])) === false, 'the sound\'s link DT1: not the mirror\'s');
  // stop
  r.midi = [];
  r.m.stop();
  check(r.midi[0][0] === 0xfc && r.midi.filter((x) => x[1] === 123).length === 16, 'stop: STOP, then ALL NOTES OFF on every channel');
  r.midi = [];
  r.m.fromFm1(new Uint8Array([0xf8])); r.m.note(0x90, 60, 1);
  check(r.midi.length === 0, 'stopped: nothing more into the sim');
}
{
  const r = rig(false);                            // fed (the editor's panel reads)
  r.m.start(); await flush();
  check(r.asked.length === 0, 'fed: no reads of its own');
  check(r.m.fromFm1(answer(0, UP)[0]) === false, 'fed: area 04 left to the panel');
  r.m.track(1, LO, { ver: 3 });
  check(tracksIn(r.sim).has(1) && r.m.ver[1] === 3, 'fed: a track handed in goes into the sim');
  r.sim = [];
  r.m.track(0, { ...UP, bars: 1 });                 // (a clip the FM-1 would not hold: a note past the loop)
  check(r.sim.length === 0, 'a clip that does not validate: not written');
  r.m.stop(); r.sim = [];
  r.m.track(0, UP);
  check(r.sim.length === 0, 'off: no track written');
}

// ---- against the firmware (host build): what the mirror writes, the sim's firmware holds ----
const exe = process.argv[2];
if (exe && existsSync(exe)) {
  const r = rig(false);
  r.m.start();
  r.m.track(0, UP); r.m.track(1, LO);
  const dir = mkdtempSync(join(tmpdir(), 'mirror-'));
  let t = 100, k = 0;
  const lines = [];
  const put = (m) => { const f = join(dir, `m${k++}.syx`); writeFileSync(f, m); lines.push(`${t} syx ${f}`); t += 40; };
  r.sim.forEach(put);
  t = 3000; put(S.readMsg(0)); t += 400; put(S.readMsg(1));
  lines.push(`${t + 500} end`);
  writeFileSync(join(dir, 'run.txt'), lines.join('\n') + '\n');
  execFileSync(exe, [join(dir, 'run'), join(dir, 'run.txt')], { stdio: 'ignore' });
  const dump = readFileSync(join(dir, 'run.syx')), back = new Map();
  const rd = new S.SeqReader({ onClip: (tt, c) => back.set(tt, c) });
  rd.expect(0); rd.expect(1);
  for (let i = 0; i < dump.length; ) { const j = dump.indexOf(0xf7, i); if (j < 0) break; rd.feed(dump.subarray(i, j + 1)); i = j + 1; }
  // (as seq-model-test: the events, LENGTH and ROOT; a live track's QUANT is the FM-1's setting, not the track's)
  const asSent = (b, c) => !!b && JSON.stringify(b.events) === JSON.stringify(c.events) && b.bars === c.bars && b.root === c.root;
  check(asSent(back.get(0), notesOnly(UP)), 'firmware: UPPER as mirrored (notes only)');
  check(asSent(back.get(1), LO), 'firmware: LOWER as mirrored');
} else console.log('note: no host build given: the firmware round trip was not run');

console.log(`mirror: ${checks} checks, ${fails} failed`);
process.exit(fails ? 1 : 0);
