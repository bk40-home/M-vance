// SPDX-License-Identifier: GPL-3.0-only
// fm1-kit library: the model's rules, in Node.   node library/test/model_test.js
"use strict";
const assert = require("node:assert/strict");
const path = require("node:path");
const zlib = require("node:zlib");
require(path.join(__dirname, "..", "model.js"));
require(path.join(__dirname, "..", "zip.js"));
const FK = globalThis.FK;
let n = 0;
const t = (name, fn) => { fn(); n++; };

const A = {n: "a"}, B = {n: "b"}, C = {n: "c"}, D = {n: "d"};
t("a drop fills from the slot on, replacing", () => {
  const v = [null, A, null, B, null];
  assert.deepEqual(FK.planDrop(v, 1, 3, false), {slots: [1, 2, 3], replaces: 2, overflow: 0});
});
t("empty slots only skips the filled ones", () => {
  const v = [null, A, null, B, null];
  assert.deepEqual(FK.planDrop(v, 1, 3, true), {slots: [2, 4], replaces: 0, overflow: 1});
});
t("past the end: what did not fit is counted", () => {
  assert.deepEqual(FK.planDrop([null, null, null], 1, 5, false), {slots: [1, 2], replaces: 0, overflow: 3});
});
t("one slot onto a filled one swaps them", () => {
  const ch = FK.planMove([A, null, B], [0], 2, false, false);
  assert.equal(ch.get(2), A); assert.equal(ch.get(0), B);
});
t("one slot onto an empty one moves it", () => {
  const ch = FK.planMove([A, null, null], [0], 2, false, false);
  assert.equal(ch.get(2), A); assert.equal(ch.get(0), null);
});
t("a block moves down, its old places emptied", () => {
  const ch = FK.planMove([A, B, null, null, C], [0, 1], 2, false, false);
  assert.equal(ch.get(2), A); assert.equal(ch.get(3), B); assert.equal(ch.get(0), null); assert.equal(ch.get(1), null);
  assert.equal(ch.has(4), false);
});
t("a block moved onto itself shifted by one keeps every patch", () => {
  const ch = FK.planMove([A, B, C, null], [0, 1, 2], 1, false, false);
  const v = [A, B, C, null]; for (const [s, p] of ch) v[s] = p;
  assert.deepEqual(v, [null, A, B, C]);
});
t("Alt copies: the originals stay", () => {
  const ch = FK.planMove([A, B, null, null], [0, 1], 2, true, false);
  assert.equal(ch.get(2), A); assert.equal(ch.get(3), B); assert.equal(ch.has(0), false);
});
t("pending: a change back to what was read is no change; undo and redo", () => {
  const P = new FK.Pending(), loaded = [A, B, null], same = (x, y) => x === y;
  P.apply(1, new Map([[2, C], [0, D]]), loaded, same);
  assert.equal(P.count(1), 2);
  P.apply(1, new Map([[0, A]]), loaded, same);
  assert.equal(P.count(1), 1);
  assert.deepEqual(P.view(1, loaded), [A, B, C]);
  P.back(); assert.deepEqual(P.view(1, loaded), [D, B, C]);
  P.back(); assert.deepEqual(P.view(1, loaded), [A, B, null]);
  P.forward(); assert.deepEqual(P.view(1, loaded), [D, B, C]);
});
t("search: words, filters, quotes", () => {
  const q = FK.parseQuery('glass cat:pad colour:blue tag:warm src:"Synth2 Factory" bank:u1 dup');
  assert.deepEqual(q.text, ["glass"]); assert.deepEqual(q.cat, ["pad"]); assert.deepEqual(q.colour, ["blue"]);
  assert.deepEqual(q.tag, ["warm"]); assert.deepEqual(q.src, ["synth2 factory"]); assert.deepEqual(q.bank, ["u1"]); assert.equal(q.dup, true);
  const e = {name: "PAD Glass Pad", cat: "PAD", colour: "blue", tags: ["warm"], src: "Synth2 Factory", bank: "U1", dup: true};
  assert.equal(FK.matches(e, q), true);
  assert.equal(FK.matches(Object.assign({}, e, {tags: []}), q), false);
  assert.equal(FK.matches(e, FK.parseQuery("")), true);
  assert.equal(FK.matches({name: "BASS Reese", cat: "BASS"}, FK.parseQuery("cat:pa")), false);
});
t("categories from names", () => {
  assert.equal(FK.guessCategory("PAD Distant Hill"), "PAD");
  assert.equal(FK.guessCategory("BS Reese Wide"), "BASS");
  assert.equal(FK.guessCategory("LEAD Sync Scream"), "LEAD");
  assert.equal(FK.guessCategory("EP Bell Mix"), "KEYS");
  assert.equal(FK.guessCategory("Warm Strings"), "STRINGS");
  assert.equal(FK.guessCategory("INIT"), "OTHER");
});
t("slots from file names", () => {
  assert.deepEqual(FK.placeByNumber(["012.sy1", "a.sy1", "001_Lead.sy1", "b.sy1"], 128), [11, 1, 0, 2]);
  assert.deepEqual(FK.placeByNumber(["001.sy1", "001.sy1"], 128), [0, 1]);
});
(async () => {
  const files = [{name: "001.sy1", data: "PAD\ncolor=blue\n"}, {name: "dir/002.sy1", data: new Uint8Array([1, 2, 3])}];
  const z = FK.zip(files), back = await FK.unzip(z);
  assert.equal(back.length, 2);
  assert.equal(new TextDecoder().decode(back[0].data), "PAD\ncolor=blue\n");
  assert.deepEqual([...back[1].data], [1, 2, 3]);
  assert.equal(back[1].name, "dir/002.sy1");
  n++;
  /* a deflated entry, made by hand */
  const raw = Buffer.from("hello hello hello hello"), def = zlib.deflateRawSync(raw), name = Buffer.from("x.txt");
  const u16 = v => [v & 255, v >> 8 & 255], u32 = v => [v & 255, v >> 8 & 255, v >> 16 & 255, v >>> 24 & 255];
  const crc = FK.crc32(raw);
  const lh = Buffer.from([0x50, 0x4B, 3, 4, ...u16(20), 0, 0, 8, 0, 0, 0, 0, 0, ...u32(crc), ...u32(def.length), ...u32(raw.length), ...u16(name.length), 0, 0]);
  const cd = Buffer.from([0x50, 0x4B, 1, 2, ...u16(20), ...u16(20), 0, 0, 8, 0, 0, 0, 0, 0, ...u32(crc), ...u32(def.length), ...u32(raw.length), ...u16(name.length), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ...u32(0)]);
  const off = lh.length + name.length + def.length;
  const end = Buffer.from([0x50, 0x4B, 5, 6, 0, 0, 0, 0, 1, 0, 1, 0, ...u32(cd.length + name.length), ...u32(off), 0, 0]);
  const zz = await FK.unzip(Buffer.concat([lh, name, def, cd, name, end]));
  assert.equal(Buffer.from(zz[0].data).toString(), "hello hello hello hello");
  n++;
  /* the inflate against zlib's deflate: text, random bytes, all three block kinds, a ZIP64 directory */
  let seed = 7;
  const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) >>> 24;
  for (const [what, buf] of [
    ["text", Buffer.from("PAD Glass Pad\ncolor=blue\nver=112\n" + Array.from({length: 99}, (_, i) => i + "," + (i * 7 % 128) + "\n").join(""))],
    ["random", Buffer.from(Array.from({length: 70000}, rnd))],
    ["runs", Buffer.alloc(100000, 65)],
  ]) for (const level of [0, 1, 9]) {
    const d = zlib.deflateRawSync(buf, {level, strategy: what === "text" && level === 9 ? zlib.constants.Z_FIXED : zlib.constants.Z_DEFAULT_STRATEGY});
    assert.deepEqual(Buffer.from(FK.inflateRaw(new Uint8Array(d))), buf, what + " level " + level);
    n++;
  }
  console.log("fm1-kit model_test: " + n + " ok");
})().catch(e => { console.error(e); process.exit(1); });
