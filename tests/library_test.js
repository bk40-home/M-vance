// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home).
// M-vance's library adapter, in Node (Kris 15 Nov 2026, fm1-port/mvance-library-editor-proposal-2026-11-15.md):
//   1. the factory bank in the page is the firmware's (mv_factory.h), already as the firmware keeps it (the adapter's
//      clamps change nothing), and its 64 names match tools/bank/factory.txt
//   2. files: every program through a .syx and back; the bank as one .syx is tools/make_bank.py's byte format
//      (bank-model.js makeBankSyx) and reads back whole; a .syx of the device's own dump (tools/bank/device.syx)
//   3. the clamps: a patch out of range is held as mv_patch_sanitize holds it; performance bytes out of range
//   4. the wire, against the pretend FM-1 (library/mock-mvance.js, mv_xfer.c's rules): identify, read 64, write and
//      read back, audition, a renamed program, no clear
//   node tests/library_test.js build/unit/library_data.json     (tests/run_unit.sh makes the json)
"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const ROOT = path.join(__dirname, "..");
const data = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
globalThis.FK_DATA = data;
require(path.join(ROOT, "build/library/bank-model.classic.js"));
require(path.join(ROOT, "third_party/fm1-kit/library/model.js"));
require(path.join(ROOT, "third_party/fm1-kit/library/zip.js"));
require(path.join(ROOT, "library/adapter-mvance.js"));
require(path.join(ROOT, "library/mock-mvance.js"));
const FK = globalThis.FK, A = FK.adapter, M = FK.mvance, B = FK.mvb;
const fp = A.fingerprint;
let n = 0;

/* 1. the factory bank */
const fac = A.factory()[0].slots;
assert.equal(fac.length, 64);
fac.forEach((p, s) => assert.equal(fp(M.norm(p)), fp(p), "factory " + (s + 1) + ": the clamps changed it (mv_params.h vs the adapter)"));
const listed = [...fs.readFileSync(path.join(ROOT, "tools/bank/factory.txt"), "utf8").matchAll(/^\s*(\d+)\s+\S+\s+"([^"]*)"/gm)];
assert.equal(listed.length, 64, "factory.txt: 64 programs");
for (const [, slot, name] of listed) assert.equal(A.nameOf(fac[slot - 1]), name.trimEnd(), "factory " + slot + ": the name");
n++;

/* 2. files */
fac.forEach((p, s) => {
  const back = A.parseFile(A.fileName(p, s), A.serialise(p));
  assert.equal(back.length, 1); assert.equal(fp(back[0]), fp(p), "program " + (s + 1) + " through a .syx");
});
const one = A.bankFile(fac);
assert.deepEqual(Array.from(one.bytes), Array.from(B.makeBankSyx(fac.map(M.itemOf))));
const again = A.parseFile(one.name, one.bytes);
assert.equal(again.length, 64);
again.forEach((p, s) => assert.equal(fp(p), fp(fac[s]), "the bank .syx, slot " + (s + 1)));
/* (0.3.0, 2b' i) a bank with empty slots: only its programs, each at its own slot, and back to the same slots */
{
  const part = new Array(64).fill(null);
  part[0] = fac[3]; part[4] = fac[9]; part[63] = fac[20];
  const f = A.bankFile(part);
  assert.equal(f.name, "3-programs.syx"); assert.match(f.note, /3 of 64/);
  assert.deepEqual(Array.from(f.bytes), [0, 4, 63].flatMap(s => B.perfMsgs(M.itemOf(part[s]), 0x03, s)).flatMap(m => Array.from(m)));
  const back = A.parseFile(f.name, f.bytes);
  assert.equal(back.length, 64); assert.equal(back.filter(Boolean).length, 3);
  for (const s of [0, 4, 63]) assert.equal(fp(back[s]), fp(part[s]), "the part bank: slot " + (s + 1));
  assert.equal(A.bankFile(new Array(64).fill(null)), null);
  assert.equal(A.parseFile("x.syx", A.serialise(fac[5])).length, 1, "one program's file: slot 1 (core.js places it)");
}
assert.equal(A.parseFile("x.sy1", new Uint8Array(4)), null);
const dump = A.parseFile("device.syx", fs.readFileSync(path.join(ROOT, "tools/bank/device.syx")));
assert.equal(dump.length, 64, "tools/bank/device.syx: 64 programs");
n++;
/* the firmware's own dump (tests/xfer_test.py: the host build answering RQ1 03 00, all 64): what the page reads */
const hostDump = path.join(ROOT, "build/unit/host_factory_dump.syx");
if (fs.existsSync(hostDump)) {
  const got = A.parseFile("dump.syx", fs.readFileSync(hostDump));
  assert.equal(got.length, 64);
  got.forEach((p, s) => assert.equal(fp(p), fp(fac[s]), "the host build's dump, slot " + (s + 1) + ": as the page's factory"));
  n++;
} else console.log("library_test: no build/unit/host_factory_dump.syx (run tests/xfer_test.py first): that check skipped");

/* 3. the clamps (mv_patch_sanitize, mv_prog_perf_ok) */
{
  const p = fac[0], up = Uint8Array.from(p.up), pf = Uint8Array.from(p.pf);
  const off = 39;                                               /* FILTER TYPE (0..2): one parameter at that byte */
  const max = Math.min(...data.params.filter(r => r[0] === off).map(r => r[3]));
  up[off] = 127; up[3] = 7; up[200] = 0x85; pf[0] = 9; pf[3] = 7;
  const q = M.norm({up, lo: p.lo, pf});
  assert.equal(q.up[off], max); assert.equal(q.up[3], 32); assert.ok(q.up.every(v => v < 128)); assert.equal(fp(M.norm(q)), fp(q));   /* 7-bit; held once is held */
  assert.equal(q.pf[0], 0); assert.equal(q.pf[3], B.PERF_DEF[3]);
  assert.equal(A.nameOf(A.withName(p, "Too long a name for M-vance")), "Too long a name");
  n++;
}

/* 3b. layers (0.3.0, 3a) */
{
  const L = A.layers, a = fac[2], b = fac[30];
  const pfa = Uint8Array.from(a.pf), pfb = Uint8Array.from(b.pf);
  pfb[7] = 5; pfb[8] = 6; pfb[9] = 3; pfb[10] = 4; pfb[11] = 7; pfb[12] = 8;   /* b's part bytes, told apart */
  const bb = M.norm({up: b.up, lo: b.lo, pf: pfb}), aa = M.norm({up: a.up, lo: a.lo, pf: pfa});
  /* LOWER of aa from bb's UPPER: aa's UPPER, name and mode unchanged; bb's upper sync bytes on aa's lower part */
  let q = L.take(aa, 1, bb, 0);
  assert.deepEqual(Array.from(q.lo), Array.from(bb.up)); assert.deepEqual(Array.from(q.up), Array.from(aa.up));
  assert.deepEqual([q.pf[8], q.pf[10], q.pf[12]], [5, 3, 7]); assert.equal(q.pf[0], aa.pf[0]);
  /* UPPER from bb's LOWER (Alt): the sound is bb's lower, the name stays aa's */
  q = L.take(aa, 0, bb, 1);
  assert.deepEqual(Array.from(q.up.subarray(16)), Array.from(bb.lo.subarray(16))); assert.equal(A.nameOf(q), A.nameOf(aa));
  assert.deepEqual([q.pf[7], q.pf[9], q.pf[11]], [6, 4, 8]);
  /* swap twice is the program; copy UPPER -> LOWER */
  const d = M.norm({up: aa.up, lo: bb.up, pf: Uint8Array.from([1, 60, 64, 3, 0, 0, 0, 1, 2, 3, 4, 5, 6, 0, 0, 0])});
  const sw = L.swap(d);
  assert.deepEqual(Array.from(sw.up.subarray(16)), Array.from(d.lo.subarray(16))); assert.equal(A.nameOf(sw), A.nameOf(d));
  assert.deepEqual([sw.pf[7], sw.pf[8], sw.pf[9], sw.pf[10], sw.pf[11], sw.pf[12]], [2, 1, 4, 3, 6, 5]);
  assert.deepEqual(Array.from(L.swap(sw).up.subarray(16)), Array.from(d.up.subarray(16)));
  const cp = L.copy(d, 0, 1);
  assert.deepEqual(Array.from(cp.lo.subarray(16)), Array.from(d.up.subarray(16))); assert.equal(cp.pf[8], d.pf[7]);
  /* mode, split, heard, solo */
  assert.equal(L.withMode(d, 2).pf[0], 2); assert.equal(L.params[0].set(d, 48).pf[1], 48); assert.equal(L.params[0].fmt(60), "C4");
  assert.ok(!L.params[0].show(d)); assert.ok(L.params[0].show(L.withMode(d, 2)));
  assert.ok(L.heard(d, 1)); assert.ok(!L.heard(L.withMode(d, 0), 1)); assert.ok(L.heard(L.withMode(d, 0), 0));
  const so = L.solo(d, 1);
  assert.equal(so.pf[0], 0); assert.deepEqual(Array.from(so.up.subarray(16)), Array.from(d.lo.subarray(16))); assert.equal(so.pf[7], d.pf[8]);
  assert.equal(L.nameOf(d, 1), B.nameOf(bb.up));
  /* every result is as the firmware keeps it, and goes through a .syx unchanged */
  for (const r of [L.take(aa, 1, bb, 0), sw, cp, so]) { assert.equal(fp(M.norm(r)), fp(r)); assert.equal(fp(A.parseFile("x.syx", A.serialise(r))[0]), fp(r)); }
  n++;
}

/* 4. the wire, against the pretend FM-1 */
(async () => {
  const dev = A.mock();
  const id = await A.device.identify(dev);
  assert.equal(id.product, "JT-80X0"); assert.ok(A.product.test(id.product)); n++;
  const all = await A.device.read(dev, 0, 64, () => {});
  all.forEach((p, s) => assert.equal(fp(p), fp(fac[s]), "read: slot " + (s + 1))); n++;
  for (const [s, from] of [[4, 40], [5, 41], [63, 0]]) {      /* three slots rewritten, then read back */
    await A.device.write(dev, 0, s, fac[from]);
    assert.equal(fp(await A.device.readSlot(dev, 0, s)), fp(fac[from]), "slot " + (s + 1) + " reads back");
  }
  assert.equal(fp(await A.device.readSlot(dev, 0, 6)), fp(fac[6]), "a slot not written: unchanged"); n++;
  const q = A.withName(fac[10], "Renamed 16 chars");
  await A.device.write(dev, 0, 20, q);
  const q2 = await A.device.readSlot(dev, 0, 20);
  assert.equal(A.nameOf(q2), "Renamed 16 chars"); assert.equal(fp(q2), fp(q)); n++;
  const dual = {up: fac[12].up, lo: fac[30].up, pf: Uint8Array.from([1, 60, 64, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])};
  await A.device.write(dev, 0, 33, dual);
  assert.equal(fp(await A.device.readSlot(dev, 0, 33)), fp(M.norm(dual)), "a DUAL performance"); n++;
  const built = A.layers.withMode(A.layers.take(fac[1], 1, fac[50], 0), 1);   /* (0.3.0) a DUAL made from two singles */
  await A.device.write(dev, 0, 34, built);
  assert.equal(fp(await A.device.readSlot(dev, 0, 34)), fp(built), "a DUAL built with the layers"); n++;
  await A.device.audition(dev, fac[7]);
  assert.equal(fp(dev.played[0]), fp(fac[7])); assert.equal(fp(dev.slots[7]), fp(fac[7])); n++;
  assert.equal(A.device.clear, undefined, "no clear: an M-vance slot always holds a program"); n++;
  /* a silent device: identify says so */
  const mute = {send() {}, listen() { return () => {}; }, close() {}};
  await assert.rejects(A.device.identify(mute), /no answer/); n++;
  console.log("library_test: " + n + " ok (64 factory programs, the bank .syx, the mock FM-1)");
})().catch(e => { console.error(e); process.exit(1); });
