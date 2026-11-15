/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home).
 * M-vance's adapter for the fm1-kit library page (third_party/fm1-kit/library/adapters/README.md says what an adapter
 * gives). Kris 15 Nov 2026: fm1-port/mvance-library-editor-proposal-2026-11-15.md, 1a 2a 3a 4a 6a.
 *
 *   a patch    an M-vance program, what one slot holds (1a): {up, lo, pf}: the UPPER patch (248 bytes, the JP-8080's;
 *              its bytes 0..15 are the program's name), the LOWER patch (248) and the 16 performance bytes
 *              (mv_perf.h MV_PF_*). Held as the firmware would store it (norm: mv_patch_sanitize's clamps, the
 *              performance bytes in mv_prog_perf_ok's ranges), so what is sent reads back the same
 *   files      .syx and .mid of JP-8000 / JP-8080 DT1s: performances (03 pp ..) and user patches (02 ..), parsed by
 *              sim/bank/bank-model.js (FK.mvb: the one copy of that code, tools/make_bank.py's rules). Download
 *              writes the whole bank as ONE .syx, as tools/make_bank.py writes it (make_bank.py --from-device reads it)
 *   the wire   firmware/src/mv_xfer.h: no acknowledgement, so every message is followed by GAP_MS (the FM-1 holds
 *              one received message at a time) and the page reads each slot back (core.js) and compares
 *                identify   MIDI Universal Device Inquiry F0 7E 7F 06 01 F7; M-vance answers 7D (non-commercial),
 *                           family "MV", member "AN" (mv_xfer_ident, 2a). An M-Vsynth2 does not answer it: asked
 *                           its own way, so the page can name it
 *                write      the program's 4 DT1s to 03 pp (common, upper, lower, the performance bytes)
 *                read       RQ1 03 pp, size 00 nn 00 00: nn x 4 DT1s back
 *                audition   the 4 DT1s to the temporary area 01 00 (plays at once, nothing saved)
 *              No clear: an M-vance slot always holds a program
 *   colours    none stored (3a): the page colours a patch by its category, from its name
 *   Edit       (6a) opens the sound editor (sim/editor.html) with this patch; its Keep puts the edited sound back
 *              into the same slot as an ordinary change (BroadcastChannel "mvance-edit", same site)
 * Needs FK_DATA = {factory: [[upHex, loHex, pfHex] x 64], params: [[off, size, min, max]], pv: {...}}. */
(function (G) {
  "use strict";
  const FK = G.FK, B = FK.mvb, D = G.FK_DATA;
  const NAME = 16;                         /* a program's name: the upper patch's bytes 0..15, ASCII 32..125 */
  const NPROG = 64;                        /* programs on the FM-1 */
  const GAP_MS = 35;                       /* between two messages sent (sim/bank/fm1-midi.js GAP_MS) */
  const QUIET_MS = 3000;                   /* a read has stopped when nothing came for this long */
  const FIRST_MS = 5000;                   /* (the first answer of a read may take longer: the FM-1 may be busy) */
  /* mv_prog_perf_ok's ranges, byte by byte (mv_perf.h): MODE, SPLIT, BALANCE, VOICES, CH LO, CH UP, ARP PART,
   * LFO1 SYNC up / lo (< MV_LFO1_SYNC_N), DELAY SYNC (< MV_DSYNC_N), CHORUS SYNC (< MV_CSYNC_N), the spares 7-bit */
  const PF_MAX = [2, 127, 127, 6, 16, 16, 3, 22, 22, 10, 10, 23, 23, 127, 127, 127];

  /* ---- bytes */
  const fromHex = h => Uint8Array.from(h.match(/../g) || [], x => parseInt(x, 16));
  const toHex = b => Array.from(b, x => (x < 16 ? "0" : "") + x.toString(16)).join("");
  const sleep = ms => new Promise(r => setTimeout(r, ms));

  /* one patch as the firmware keeps it (mv_patch_sanitize): 7-bit bytes, the name printable, every parameter
   * clamped to its range (a two-byte parameter: 0..254 as high 0/1, low 7 bits) */
  function sanitize(src) {
    const p = new Uint8Array(B.PATCH);
    p.set(src.subarray ? src.subarray(0, B.PATCH) : src.slice(0, B.PATCH));
    for (let i = 0; i < B.PATCH; i++) p[i] &= 0x7F;
    for (let i = 0; i < NAME; i++) if (p[i] < 32 || p[i] > 125) p[i] = 32;
    for (const [off, size, min, max] of D.params) {
      const v = size === 2 ? (p[off] << 7) | p[off + 1] : p[off];
      if (v >= min && v <= max) continue;
      const c = v < min ? min : max;
      if (size === 2) { p[off] = c >> 7; p[off + 1] = c & 0x7F; } else p[off] = c;
    }
    return p;
  }
  /* a program as the firmware keeps it: both patches sanitised; a performance byte out of range takes the default
   * (the firmware would refuse the whole 70 message, and the slot would keep the old bytes) */
  function norm(s) {
    const pf = Uint8Array.from(s.pf.slice(0, B.NPERF));
    for (let i = 0; i < B.NPERF; i++) if (pf[i] > PF_MAX[i]) pf[i] = B.PERF_DEF[i];
    return {up: sanitize(s.up), lo: sanitize(s.lo), pf};
  }
  const nameOf = p => B.nameOf(p.up);
  const itemOf = p => B.itemFromSound(p);  /* bank-model's item: what perfMsgs and makeBankSyx take */

  /* ---- the wire (mv_xfer.h). One job at a time: a second waits for the first */
  let chain = Promise.resolve();
  const serial = fn => { const r = chain.then(fn, fn); chain = r.catch(() => {}); return r; };
  async function sendPaced(io, msgs) { for (const m of msgs) { io.send(m); await sleep(GAP_MS); } }
  const rsum = a => { let s = 0; for (const b of a) s += b; return (128 - (s % 128)) % 128; };
  /* Roland RQ1 (device 10h, model 00 06): ask for nn performances from slot pp */
  const rq1 = (pp, nn) => { const b = [0x03, pp, 0, 0, 0, nn, 0, 0]; return Uint8Array.from([0xF0, 0x41, 0x10, 0x00, 0x06, 0x11, ...b, rsum(b), 0xF7]); };
  /* a DT1 of a performance (03 pp aa ..): its slot and which of its 4 messages (0 common, 1 upper, 2 lower, 3 own) */
  function perfPart(m) {
    if (m.length < 12 || m[0] !== 0xF0 || m[1] !== 0x41 || m[3] !== 0x00 || m[4] !== 0x06 || m[5] !== 0x12 || m[6] !== 0x03) return null;
    const k = {0x00: 0, 0x40: 1, 0x42: 2, 0x70: 3}[m[8]];
    return k === undefined || m[7] >= NPROG ? null : {pp: m[7], k};
  }
  /* read n programs from slot `from`: [patch] (null where one did not come whole) */
  function readRange(io, from, n, onProgress) {
    return serial(() => new Promise((ok, no) => {
      const got = new Map();                 /* pp -> its 4 messages */
      let whole = 0, t = 0, off = null;
      const finish = err => { clearTimeout(t); off(); if (err) { no(err); return; }
        const out = new Array(n).fill(null);
        for (const [pp, ms] of got) {
          if (ms.some(x => !x)) continue;
          const len = ms.reduce((a, x) => a + x.length, 0), all = new Uint8Array(len);
          let o = 0; for (const x of ms) { all.set(x, o); o += x.length; }
          const it = B.parseBankFile(all)[0];
          if (it) out[pp - from] = norm(B.soundOf(it));
        }
        ok(out);
      };
      const arm = ms => { clearTimeout(t); t = setTimeout(() => finish(whole ? new Error("the read stopped at " + FK.slotName(from + whole)) : new Error("no answer")), ms); };
      off = io.listen(d => {
        const q = perfPart(d);
        if (!q || q.pp < from || q.pp >= from + n) return;
        const ms = got.get(q.pp) || [null, null, null, null];
        const was = ms.every(Boolean);
        ms[q.k] = Uint8Array.from(d);
        got.set(q.pp, ms);
        if (!was && ms.every(Boolean)) { whole++; if (onProgress) onProgress(whole); }
        if (whole >= n) finish(null); else arm(QUIET_MS);
      });
      arm(FIRST_MS);
      io.send(rq1(from, n >= NPROG ? NPROG : n));
    }));
  }
  /* who is there: M-vance answers the Universal Device Inquiry; an M-Vsynth2 its own IDENT (vs_xfer.h) */
  function ask(io, msg, match, ms) {
    return new Promise(ok => {
      let off = null;
      const t = setTimeout(() => { off(); ok(null); }, ms);
      off = io.listen(d => { const r = match(d); if (r) { clearTimeout(t); off(); ok(r); } });
      io.send(msg);
    });
  }
  const UNIVERSAL = Uint8Array.from([0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7]);
  function identityReply(d) {                /* F0 7E dev 06 02 mfr fam fam mem mem v v v v F7 */
    if (d.length < 15 || d[0] !== 0xF0 || d[1] !== 0x7E || d[3] !== 0x06 || d[4] !== 0x02) return null;
    if (d[5] === 0x7D && d[6] === 0x4D && d[7] === 0x56 && d[8] === 0x41 && d[9] === 0x4E)   /* 7D "MV" "AN" */
      return {product: "M-VANCE", version: d[10] + "." + d[11] + (d[12] ? "." + d[12] : "")};
    if (d[5] === 0x41) return {product: "a Roland synthesizer (a JP-8000 / 8080?)", version: ""};
    return {product: "another device (manufacturer " + d[5].toString(16).toUpperCase() + "h)", version: ""};
  }
  const VS_IDENT = Uint8Array.from([0xF0, 0x7D, 0x4D, 0x56, 0x05, 0x7B, 0xF7]);   /* M-Vsynth2: IDENT, sum */
  const vsReply = d => d.length > 6 && d[0] === 0xF0 && d[1] === 0x7D && d[2] === 0x4D && d[3] === 0x56 && d[4] === 0x15
    ? {product: "M-Vsynth2", version: ""} : null;

  /* ---- the Editor (6a): Edit sends the patch there; its Keep comes back here */
  const edits = new Map();                   /* id -> the slot it came from (FK.lib.putAt's ref) */
  let chan = null, waiting = null, editor = null;
  function channel() {
    if (chan || typeof BroadcastChannel === "undefined") return chan;
    chan = new BroadcastChannel("mvance-edit");
    chan.onmessage = e => {
      const m = e.data || {};
      if (m.type === "hello" && waiting) chan.postMessage(waiting);   /* (the editor opened after the Edit) */
      if (m.type === "took" && waiting && m.id === waiting.id) waiting = null;
      if (m.type === "keep" && edits.has(m.id)) {
        const p = norm({up: Uint8Array.from(m.sound.up), lo: Uint8Array.from(m.sound.lo), pf: Uint8Array.from(m.sound.pf)});
        const why = FK.lib.putAt(edits.get(m.id), p);
        chan.postMessage({type: "kept", id: m.id, ok: !why, why});
        if (why) FK.lib.say("Keep: " + why, "bad");
      }
    };
    return chan;
  }
  function edit(p, ref, where) {
    if (!channel()) { FK.lib.say("This browser cannot pass a patch to the editor (no BroadcastChannel)", "bad"); return; }
    const id = Date.now().toString(36) + Math.random().toString(36).slice(2, 6);
    edits.set(id, ref);
    const at = where.split(" · ")[0];        /* ("PROGRAMS 005 · program · SINGLE": the slot) */
    waiting = {type: "edit", id, where: at, name: nameOf(p), sound: {up: Array.from(p.up), lo: Array.from(p.lo), pf: Array.from(p.pf)}};
    chan.postMessage(waiting);
    if (!editor || editor.closed) editor = G.open("sim/editor.html", "mvance-editor");
    else editor.focus();
    FK.lib.say("Editing " + nameOf(p) + " in the editor: Keep there puts it back in " + at);
  }

  /* ---- what the page shows */
  const pv = (p, k) => { const [off, size] = D.pv[k]; return size === 2 ? (p.up[off] << 7) | p.up[off + 1] : p.up[off]; };
  const pvName = (p, k) => (D.pv[k + "_names"] || [])[pv(p, k)] || String(pv(p, k));
  const v127 = x => Math.max(0, Math.min(1, x / 127));
  const MODES = ["SINGLE", "DUAL", "SPLIT"];

  FK.mvance = {sanitize, norm, nameOf, itemOf, fromHex, toHex, rq1, identityReply, PF_MAX, GAP_MS};   /* (the mock, the tests) */
  FK.adapter = {
    id: "mvance",
    name: "M-vance",
    product: /^M-VANCE$/,
    deviceName: "FM-1",
    portMatch: /fm-?1|m-?vave|dinghy|m-?vance/i,
    nameLength: NAME,
    bankSize: NPROG,
    deviceBanks: [{id: 0, name: "PROGRAMS", size: NPROG}],
    fileTypes: [".syx", ".mid"],
    colours: ["default"],
    theme: "amber",                          /* (4a: M-vance's screen theme) */
    nameOf,
    colourOf: () => "default",
    withName: (p, n) => { const up = Uint8Array.from(p.up); for (let i = 0; i < NAME; i++) { const c = (n.charCodeAt(i) || 32); up[i] = c >= 32 && c <= 125 ? c : 32; } return {up, lo: Uint8Array.from(p.lo), pf: Uint8Array.from(p.pf)}; },
    withColour: p => p,
    fingerprint: p => toHex(p.up) + toHex(p.lo) + toHex(p.pf),
    soundKey: p => toHex(p.up.subarray(NAME)) + toHex(p.lo.subarray(NAME)) + toHex(p.pf),   /* (names aside) */
    parseFile(name, bytes) {
      if (!/\.(syx|mid)$/i.test(name)) return null;
      return B.parseBankFile(bytes).map(it => norm(B.soundOf(it)));
    },
    fileName: (p, slot) => FK.slotName(slot) + ".syx",
    serialise: p => { const ms = B.perfMsgs(itemOf(p), 0x03, 0x00), out = new Uint8Array(ms.reduce((a, m) => a + m.length, 0)); let o = 0; for (const m of ms) { out.set(m, o); o += m.length; } return out; },
    /* (0.2.0) Download: the whole bank as one .syx, tools/make_bank.py's own bytes; null while a slot is empty */
    bankFile: slots => slots.length === NPROG && slots.every(Boolean) ? {name: "bank.syx", bytes: B.makeBankSyx(slots.map(itemOf))} : null,
    describe: p => "program · " + (MODES[p.pf[0]] || "SINGLE"),
    preview(p) {
      const type = pv(p, "FLT_TYPE"), kind = ["hp", "bp", "lp"][type] || "lp", order = pv(p, "FLT_SLOPE") ? 4 : 2;
      return {
        filter: {label: pvName(p, "FLT_TYPE") + " " + (order === 4 ? "24" : "12") + " · " + pv(p, "CUTOFF") + " · " + pv(p, "RESO"),
                 kind, order, freq: v127(pv(p, "CUTOFF")), res: v127(pv(p, "RESO"))},
        env: {a: v127(pv(p, "AENV_A")), d: v127(pv(p, "AENV_D")), s: v127(pv(p, "AENV_S")), r: v127(pv(p, "AENV_R"))},
        facts: [["OSC 1", pvName(p, "OSC1_WAVE")], ["OSC 2", pvName(p, "OSC2_WAVE")], ["KEY MODE", MODES[p.pf[0]] || "?"],
                ["MONO", pvName(p, "MONO")], ["UNISON", pvName(p, "UNISON")], ["PORTA", pvName(p, "PORTA_SW")],
                ["MULTI FX", String(pv(p, "MFX_LEVEL"))], ["DELAY", String(pv(p, "DLY_LEVEL"))]],
      };
    },
    factory() {
      const slots = D.factory.map(([u, l, f]) => ({up: fromHex(u), lo: fromHex(l), pf: fromHex(f)}));
      return [{name: "M-vance factory", slots}];
    },
    actions: [{label: "Edit ▸", title: "Open this sound in the editor (sim/editor.html); Keep there puts it back here", run: edit}],
    device: {
      async identify(io) {
        const id = await serial(() => ask(io, UNIVERSAL, identityReply, 1500));
        if (id) return id;
        const vs = await serial(() => ask(io, VS_IDENT, vsReply, 800));
        if (vs) return vs;
        throw new Error("no answer");
      },
      read: (io, bank, size, onProgress) => readRange(io, 0, size, onProgress),
      readSlot: async (io, bank, slot) => (await readRange(io, slot, 1))[0],
      write: (io, bank, slot, p) => serial(() => sendPaced(io, B.perfMsgs(itemOf(p), 0x03, slot))),
      audition: (io, p) => serial(() => sendPaced(io, B.perfMsgs(itemOf(p), 0x01, 0x00))),
    },
    mock: () => FK.mockDevice(),
  };
})(typeof window !== "undefined" ? window : globalThis);
