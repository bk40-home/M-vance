// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance bank model — parse/serialise JP-8000/8080 SysEx banks and the drag-drop editor's operations.
//
// The SysEx routines mirror tools/make_bank.py (bank_patches + the output loop) and docs/bank.html exactly, so
// a bank serialised here is byte-identical to make_bank.py's .syx. That keeps make_bank.py the canonical builder:
// the editor exports a .syx, and `python3 tools/make_bank.py --from-device FILE.syx` turns it into factory.txt +
// firmware/gen/mv_factory.h. ES module; runs in the browser (the simulator/editor page) and in Node (tests).

export const PATCH = 248;                 // a JP patch, as the synth stores it
export const NPERF = 16;                  // M-vance's performance bytes (mv_perf.h MV_PF_N)
// MV_PERF_DEF: SINGLE, split 60 (C4), balance 64, 4+4 voices, global channels, ARP AUTO, the rest spare (0)
export const PERF_DEF = [0, 60, 64, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
// a JP part's syncs (03 pp 10 / 11): [offset, index in an item's sync (= performance byte - 7; + 1 the lower's), count]:
// DELAY SYNC (MV_PF_DSYNUP 9), LFO1 SYNC (MV_PF_SYNCUP 7), CHORUS SYNC RATE (MV_PF_CSYNUP 11) (Kris 7 Nov)
const PART_SYNCS = [[4, 2, 11], [5, 0, 23], [6, 4, 24]];
const SYNC_UP = [7, 9, 11];                             // the UPPER sync bytes (LOWER's: + 1)

// ---- Roland DT1 (device id 10h, model 00 06 = JP-8000/8080) ----
function rsum(bytes) { let s = 0; for (const b of bytes) s += b; return (128 - (s % 128)) % 128; }
function dt1(addr, data) {
  const body = [...addr, ...Array.from(data, (b) => b & 0x7f)];
  return new Uint8Array([0xf0, 0x41, 0x10, 0x00, 0x06, 0x12, ...body, rsum(body), 0xf7]);
}

// ---- read a .syx (raw) or a Standard MIDI File (.mid) into its F0..F7 messages ----
function vlq(b, pos) { let v = 0, c; do { c = b[pos.i++]; v = (v << 7) | (c & 0x7f); } while (c & 0x80); return v; }
function smfSysex(b) {
  const out = [];
  let i = 14;                                          // after MThd and its 6 header bytes
  while (i + 8 <= b.length) {
    const len = (b[i + 4] << 24) | (b[i + 5] << 16) | (b[i + 6] << 8) | b[i + 7];
    const isTrk = b[i] === 0x4d && b[i + 1] === 0x54 && b[i + 2] === 0x72 && b[i + 3] === 0x6b;
    const end = i + 8 + len;
    if (isTrk) {
      const pos = { i: i + 8 };
      let rs = 0, pend = null;                          // running status; a SysEx continued past F7 escapes
      while (pos.i < end) {
        vlq(b, pos);                                     // (delta time)
        let s = b[pos.i];
        if (s & 0x80) pos.i++; else s = rs;
        if (s === 0xf0 || s === 0xf7) {
          const n = vlq(b, pos), data = b.slice(pos.i, pos.i + n);
          pos.i += n;
          let m = s === 0xf0 ? [0xf0, ...data] : (pend ? [...pend, ...data] : null);
          if (m) { if (m[m.length - 1] === 0xf7) { out.push(new Uint8Array(m)); pend = null; } else pend = m; }
        } else if (s === 0xff) { pos.i++; const n = vlq(b, pos); pos.i += n; }
        else { rs = s; pos.i += (s & 0xf0) === 0xc0 || (s & 0xf0) === 0xd0 ? 1 : 2; }
      }
    }
    i = end;
  }
  return out;
}
function rawSysex(b) {
  const out = [];
  let s = -1;
  for (let i = 0; i < b.length; i++) {
    if (b[i] === 0xf0) s = i;
    else if (b[i] === 0xf7 && s >= 0) { out.push(b.slice(s, i + 1)); s = -1; }
  }
  return out;
}
export function nameOf(b) { return String.fromCharCode(...b.slice(0, 16)).replace(/[^\x20-\x7d]/g, " ").trimEnd(); }

// ---- the patches and performances in the messages (as make_bank.py bank_patches) ----
function bankOf(msgs) {
  const pats = new Map(), coms = new Map(), exts = new Map(), syncs = new Map();
  for (const m of msgs) {
    if (m.length < 12 || m[1] !== 0x41 || m[3] !== 0x00 || m[4] !== 0x06 || m[5] !== 0x12) continue;
    const a = m.slice(6, 10), body = m.slice(10, m.length - 2);
    if (rsum(m.slice(6, m.length - 2)) !== m[m.length - 2]) continue;   // a bad checksum: left out
    let key, off;
    if (a[0] === 0x02 && a[1] <= 1) {                   // user patch: 02 bb pp xx, odd pp = bytes 128..
      key = "U" + (a[1] * 64 + (a[2] >> 1) + 1); off = (a[2] & 1) * 128 + a[3];
    } else if (a[0] === 0x03 && a[2] >= 0x40 && a[2] <= 0x43) {   // performance pp: 40/41 upper, 42/43 lower
      key = "P" + (a[1] + 1) + (a[2] < 0x42 ? "A" : "B"); off = (a[2] & 1) * 128 + a[3];
    } else if (a[0] === 0x03 && a[2] === 0x00) {        // its common: name, KEY MODE 0x10, SPLIT 0x11, BALANCE 0x13
      const c = coms.get(a[1]) || new Uint8Array(64);
      c.set(body.slice(0, 64 - a[3]), a[3]); coms.set(a[1], c); continue;
    } else if (a[0] === 0x03 && (a[2] === 0x10 || a[2] === 0x11)) {   // a part: its syncs at offsets 4, 5, 6 (JP)
      const sy = syncs.get(a[1]) || [0, 0, 0, 0, 0, 0];
      for (const [o, i, n] of PART_SYNCS)
        if (a[3] <= o && o < a[3] + body.length) { const v = body[o - a[3]]; sy[i + (a[2] & 1)] = v < n ? v : 0; }
      syncs.set(a[1], sy);
      continue;
    } else if (a[0] === 0x03 && a[2] === 0x70) {        // M-vance's own performance bytes
      const x = exts.get(a[1]) || Uint8Array.from(PERF_DEF);
      x.set(body.slice(0, NPERF - a[3]), a[3]); exts.set(a[1], x); continue;
    } else continue;
    const p = pats.get(key) || new Uint8Array(PATCH);
    p.set(body.slice(0, PATCH - off), off);
    pats.set(key, p);
  }
  const items = [];
  for (const [pp, c] of [...coms].sort((x, y) => x[0] - y[0])) {
    const up = pats.get("P" + (pp + 1) + "A");
    if (!up) continue;
    const lo = pats.get("P" + (pp + 1) + "B") || up;
    items.push({ kind: "P", no: pp + 1, com: c, up, lo, ext: exts.get(pp) || null,
                 sync: syncs.get(pp) || [0, 0, 0, 0, 0, 0], name: nameOf(c), tick: false });
  }
  for (let n = 1; n <= 128; n++) {
    const up = pats.get("U" + n);
    if (up) items.push({ kind: "U", no: n, com: null, up, lo: up, ext: null, sync: [0, 0, 0, 0, 0, 0], name: nameOf(up), tick: false });
  }
  return items;
}
function perfOf(it) {                                    // the 16 performance bytes (as make_bank: ext, else the common's)
  if (it.ext) return Array.from(it.ext);
  const p = PERF_DEF.slice();
  if (it.com) { p[0] = it.com[0x10] <= 2 ? it.com[0x10] : 0; p[1] = it.com[0x11]; p[2] = it.com[0x13]; }
  if (it.sync) for (let i = 0; i < 6; i++) p[7 + i] = it.sync[i] || 0;   // the parts' LFO1 / DELAY / CHORUS syncs
  return p;
}
// an item -> its 4 DT1 messages at base a3 a2 (03 pp = slot pp, 01 00 = the temporary area)
export function perfMsgs(it, a3, a2) {
  const name = Array.from((it.name + " ".repeat(16)).slice(0, 16), (c) => {
    const v = c.charCodeAt(0); return v >= 32 && v <= 125 ? v : 32; });
  const up = Uint8Array.from(it.up); up.set(name, 0);
  const pf = perfOf(it);
  return [dt1([a3, a2, 0x00, 0x00], [...name, pf[0], pf[1], 0, pf[2]]),   // common (0x12 PANEL SELECT: UPPER)
          dt1([a3, a2, 0x40, 0x00], up),
          dt1([a3, a2, 0x42, 0x00], it.kind === "P" ? it.lo : up),        // a single patch: its lower a copy
          dt1([a3, a2, 0x70, 0x00], pf)];                                  // M-vance's own bytes
}

// ================= public API =================

// parse a .syx or .mid file (bytes) into its items: performances (kind "P") then user patches (kind "U")
export function parseBankFile(bytes) {
  const u = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const smf = u[0] === 0x4d && u[1] === 0x54 && u[2] === 0x68 && u[3] === 0x64;   // "MThd"
  return bankOf(smf ? smfSysex(u) : rawSysex(u));
}

// 64 performance items -> a JP-8080 performance bank .syx (byte-identical to tools/make_bank.py's output)
export function makeBankSyx(slots) {
  if (slots.length !== 64) throw new Error(`a bank is 64 programs, got ${slots.length}`);
  const parts = [];
  for (let s = 0; s < 64; s++) {
    if (!slots[s]) throw new Error(`slot ${s + 1} is empty`);
    for (const m of perfMsgs(slots[s], 0x03, s)) parts.push(m);
  }
  let n = 0; for (const p of parts) n += p.length;
  const out = new Uint8Array(n);
  let o = 0; for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

// ---- editor operations (a bank is an array of 64 items; a part is one patch: UPPER = 0, LOWER = 1) ----
export function cloneItem(it) {
  return { kind: "P", no: it.no, name: it.name,
           up: Uint8Array.from(it.up), lo: Uint8Array.from(it.lo),
           ext: Uint8Array.from(it.ext || perfOf(it)), sync: it.sync ? [...it.sync] : [0, 0, 0, 0, 0, 0],
           com: it.com ? Uint8Array.from(it.com) : null };
}
// reorder: take the item at `from` and drop it at `to` (classic drag-drop list move)
export function moveSlot(bank, from, to) {
  const [it] = bank.splice(from, 1); bank.splice(to, 0, it); return bank;
}
// drop one patch (srcPart of srcItem) into a slot's UPPER or LOWER, carrying that part's syncs (LFO1, DELAY, CHORUS)
export function dropPart(bank, srcItem, srcPart, destIdx, destPart) {
  const d = bank[destIdx];
  const src = cloneItem(srcItem), sp = Array.from(src.ext);
  const pf = Uint8Array.from(d.ext || perfOf(d));
  if (destPart === 0) { d.up = srcPart === 0 ? src.up : src.lo; d.name = nameOf(d.up); }
  else d.lo = srcPart === 0 ? src.up : src.lo;
  for (const k of SYNC_UP) pf[k + destPart] = sp[k + srcPart];
  d.ext = pf; return bank;
}
// rename a slot (its name lives in the upper patch's first 16 bytes, ASCII 32..125)
export function renameSlot(it, name) {
  it.name = name.slice(0, 16);
  const nm = Array.from((it.name + " ".repeat(16)).slice(0, 16),
    (c) => { const v = c.charCodeAt(0); return v >= 32 && v <= 125 ? v : 32; });
  it.up = Uint8Array.from(it.up); it.up.set(nm, 0);
  return it;
}
