// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance FM-1 over WebMIDI — connect, audition, send programs, read the 64 back. The transfer half of the
// bank tool (docs/bank.html, Kris 4 Nov, T1: fm1-port/mvance-bank-transfer-proposal-2026-11-04.md), moved here
// so the bank editor builds AND sends in one place (Kris 12 Nov, B1). The protocol is firmware/src/mv_xfer.h:
//   audition   the 4 messages of a performance to the temporary area 01 00 (plays at once, nothing saved)
//   send       the 4 messages of each program to its slot 03 pp (the FM-1 saves them once the transfer is over)
//   read       RQ1 03 pp 00 00, size 00 n 00 00: the FM-1 answers with n x 4 DT1s; done when quiet QUIET_MS
// Messages are built by bank-model.js (perfMsgs), the one copy of the SysEx code. ES module; WebMIDI with SysEx
// needs Chrome or Edge on https or localhost. In Node (tests) pass a MIDI access object and a sleep to connect().
import { perfMsgs } from './bank-model.js';

export const NPROG = 64;                  // programs on the FM-1
export const GAP_MS = 35;                 // ms between two messages sent: the FM-1 holds one received message at a time
export const QUIET_MS = 2500;             // ms: a dump is over when nothing came for this long
const PORT_HINT = /fm-?1|m-?vave|dinghy|m-?vance/i;   // port names picked by default

// ---- Roland RQ1 (device id 10h, model 00 06): ask for size bytes from addr ----
function rsum(bytes) { let s = 0; for (const b of bytes) s += b; return (128 - (s % 128)) % 128; }
export function rq1(addr, size) {
  const body = [...addr, ...size];
  return new Uint8Array([0xf0, 0x41, 0x10, 0x00, 0x06, 0x11, ...body, rsum(body), 0xf7]);
}

// the messages of a read (Map pp -> [common, upper, lower, ext]) in slot order, as one .syx's bytes
// (make_bank.py's own order; parseBankFile reads it back)
export function dumpBytes(msgs, first = 0, n = NPROG) {
  const parts = [];
  for (let pp = first; pp < first + n; pp++) for (const m of msgs.get(pp) || []) if (m) parts.push(m);
  let len = 0; for (const p of parts) len += p.length;
  const out = new Uint8Array(len);
  let o = 0; for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}

export class FM1 {
  // onProgress(done, total): called while sending / reading (total 0 = finished). sleep: (ms) => Promise.
  // onTemp(msg): a DT1 to what plays (01 00) from the FM-1: a dump or a linked report (link.js; Kris 13 Nov, Part B).
  // onProgram(n, ch): a Program Change from the FM-1 (PRESETS turned)
  constructor({ gapMs = GAP_MS, quietMs = QUIET_MS, onProgress = () => {}, onPorts = () => {},
                onTemp = () => {}, onProgram = () => {},
                sleep = (ms) => new Promise((r) => setTimeout(r, ms)) } = {}) {
    Object.assign(this, { gapMs, quietMs, onProgress, onPorts, onTemp, onProgram, sleep });
    this.access = null; this.out = null; this.inp = null;
    this.rx = new Map();                  // pp -> [4 messages], what a read collected
    this.rxWait = null;                   // a read in progress: { poke }
  }

  // ask the browser for MIDI with SysEx (or use access, in tests). Throws an Error whose message can be shown.
  async connect(access) {
    if (access) this.access = access;
    else {
      if (typeof navigator === 'undefined' || !navigator.requestMIDIAccess)
        throw new Error('This browser has no WebMIDI: use Chrome or Edge.');
      try { this.access = await navigator.requestMIDIAccess({ sysex: true }); }
      catch (e) { throw new Error(`MIDI with SysEx was not allowed (${e.name}). Open this page over https or ` +
                                  'localhost and allow it.'); }
    }
    this.access.onstatechange = () => this.onPorts(this.ports());
    return this.ports();
  }

  // the ports, and the ones to pick: the open ones if still there, else one whose name looks like the FM-1
  ports() {
    const list = (m) => [...m.values()].map((p) => ({ id: p.id, name: p.name }));
    const pick = (m, cur) => (cur && m.has(cur.id) ? cur.id : ([...m.values()].find((p) => PORT_HINT.test(p.name)) || {}).id || null);
    if (!this.access) return { outs: [], ins: [], outId: null, inId: null };
    return { outs: list(this.access.outputs), ins: list(this.access.inputs),
             outId: pick(this.access.outputs, this.out), inId: pick(this.access.inputs, this.inp) };
  }

  // use these ports (ids); returns true when both are there
  use(outId, inId) {
    if (this.inp) this.inp.onmidimessage = null;
    this.out = (outId && this.access.outputs.get(outId)) || null;
    this.inp = (inId && this.access.inputs.get(inId)) || null;
    if (this.inp) this.inp.onmidimessage = (ev) => this.onMidi(ev.data);
    return this.ready;
  }
  get ready() { return !!(this.out && this.inp); }

  // send messages one at a time, GAP_MS apart
  async send(msgs) {
    if (!this.out) throw new Error('Connect to the FM-1 first.');
    this.onProgress(0, msgs.length);
    for (let i = 0; i < msgs.length; i++) {
      this.out.send(msgs[i]);
      this.onProgress(i + 1, msgs.length);
      await this.sleep(this.gapMs);
    }
    this.onProgress(0, 0);
  }

  // audition an item (a performance or a single patch) in the temporary area: plays now, nothing is saved
  play(item) { return this.send(perfMsgs(item, 0x01, 0x00)); }

  // write programs into slots: pairs of [item, slot 0..63]
  sendSlots(pairs) {
    const msgs = [];
    for (const [it, pp] of pairs) msgs.push(...perfMsgs(it, 0x03, pp));
    return this.send(msgs);
  }

  // one message, now (link.js paces its own: one a tick)
  sendNow(m) { if (this.out) this.out.send(m); }

  // from the FM-1: a Program Change (onProgram), a DT1 to what plays (onTemp), or the performance messages
  // (03 pp 00 / 40 / 42 / 70) of a read (kept here)
  onMidi(m) {
    if ((m[0] & 0xf0) === 0xc0 && m.length >= 2) { this.onProgram(m[1], m[0] & 15); return; }
    if (m[0] === 0xf0 && m[1] === 0x41 && m[4] === 0x06 && m[5] === 0x12 && m[6] === 0x01) {
      this.onTemp(new Uint8Array(m)); return;
    }
    if (m[0] !== 0xf0 || m[1] !== 0x41 || m[4] !== 0x06 || m[5] !== 0x12 || m[6] !== 0x03) return;
    const pp = m[7], area = m[8];
    const k = area === 0x00 ? 0 : area === 0x40 ? 1 : area === 0x42 ? 2 : area === 0x70 ? 3 : -1;
    if (k < 0 || pp >= NPROG) return;
    const set = this.rx.get(pp) || [null, null, null, null];
    set[k] = new Uint8Array(m);
    this.rx.set(pp, set);
    if (this.rxWait) this.rxWait.poke();
  }

  // read n programs from slot first: resolves { msgs (Map), complete (all n x 4 came), got, want }
  read(first = 0, n = NPROG) {
    if (!this.ready) return Promise.reject(new Error('Connect to the FM-1 (both ports) first.'));
    for (let k = first; k < first + n; k++) this.rx.delete(k);
    const count = () => { let c = 0; for (let k = first; k < first + n; k++) for (const m of this.rx.get(k) || []) if (m) c++; return c; };
    return new Promise((resolve) => {
      let timer = null;
      const done = () => {
        this.rxWait = null; this.onProgress(0, 0);
        const got = count();
        resolve({ msgs: this.rx, complete: got === n * 4, got, want: n * 4 });
      };
      this.rxWait = { poke: () => { clearTimeout(timer); timer = setTimeout(done, this.quietMs); this.onProgress(count(), n * 4); } };
      timer = setTimeout(done, this.quietMs * 2);       // (the first answer may take a moment)
      this.out.send(rq1([0x03, first, 0x00, 0x00], [0x00, n, 0x00, 0x00]));
    });
  }
}
