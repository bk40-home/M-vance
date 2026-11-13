// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance bank editor — the edit buffer, and the sim and the FM-1 linked to it (Kris 13 Nov 2026, Part B:
// fm1-port/mvance-partB-hardware-sync-proposal-2026-11-13.md, 1b 2a 4a 5a 6a).
//
// Three copies of a sound (bank-model.js soundOf): the bank's slot (the truth, kept by Store), the edit buffer `cur`
// (here), and what plays on each linked device. Both devices speak the same firmware protocol (mv_xfer.h, mvance.c
// link_*), so the sim and the FM-1 are handled alike:
//   link      RQ1 01 00: the device dumps what plays (4 DT1s, the first to the common, 01 00 00) and from then on
//             reports every change as a DT1 of the changed bytes, while it is read again every RENEW_MS
//   adopt     the first dump after linking becomes the edit buffer (the device leads: the sim's program at start,
//             the FM-1's sound when Link FM-1 is switched on)
//   report    a device's change goes into `cur` and into what we know it has (`has`), so it is not sent back
//   push      tick(): where `cur` differs from what a device has, the changed bytes go to it as DT1s to 01 00
//             (bank-model.js diffMsg: first to last changed byte of a region). The sim takes them all at once; the
//             FM-1 one a tick (the FM-1 holds one received message at a time: tick every GAP_MS, newest value only)
//   renew     every RENEW_MS: RQ1 01 00 again. That dump goes into `has` only, so a lost message is mended toward
//             the edit buffer (the page's) at the next tick
// No DOM here: the editor drives it (editor.js), and tests drive it with pretend devices (link-test.mjs).
import { soundCopy, soundNew, applyTemp, diffMsg, tempArea } from './bank-model.js';

export const GAP_MS = 35;                 // tick: at most one message to the FM-1 this often (fm1-midi.js GAP_MS)
export const RENEW_MS = 20000;            // the firmware's link lapses 30 s after the last read (mvance.c LINK_HOLD)

// RQ1 01 00 00 00, size 0: read what plays (and link)
function rqTemp() {
  return new Uint8Array([0xf0, 0x41, 0x10, 0x00, 0x06, 0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xf7]);
}

// one linked device: send (a whole F0..F7 message to it), what it has (null: not known yet), its dump in progress
class Dev {
  constructor(send, pace) {
    Object.assign(this, { send, pace, has: null, dump: 0, adopt: false, pending: null, renewAt: 0 });
  }
}

export class Link {
  // simSend(msg): a message into the sim (engine.js sysexIn). onChange(why, dev): the edit buffer changed
  // ('adopt' | 'edit'), or a device was linked / unlinked ('link'). now(): ms (tests pass their own clock)
  constructor({ simSend, onChange = () => {}, now = () => Date.now() }) {
    this.cur = soundNew();                // the edit buffer
    this.onChange = onChange; this.now = now;
    this.sim = new Dev(simSend, false);
    this.fm1 = null;                      // linked: a Dev sending through the FM-1 output, paced
    this.hold = false;                    // a bank transfer to the FM-1 is running: no pushes to it meanwhile
  }

  // link a device: read what plays; its dump becomes the edit buffer (adopt)
  #open(d) {
    d.has = null; d.dump = 0; d.adopt = true; d.pending = null;
    d.renewAt = this.now() + RENEW_MS;
    d.send(rqTemp());
  }
  linkSim() { this.#open(this.sim); }
  linkFm1(send) { this.fm1 = new Dev(send, true); this.#open(this.fm1); this.onChange('link', 'fm1'); }
  unlinkFm1() { this.fm1 = null; this.onChange('link', 'fm1'); }   // (the FM-1's link lapses by itself in 30 s)
  get linked() { return !!(this.fm1 && this.fm1.has); }

  // the edit buffer becomes this sound (Edit a slot, Revert, Compare, ▶): pushed at once to the sim, paced to the FM-1
  load(sound) { this.cur = soundCopy(sound); this.tick(); this.onChange('edit', 'page'); }

  fromSim(m) { this.#take(this.sim, 'sim', m); }
  fromFm1(m) { if (this.fm1) this.#take(this.fm1, 'fm1', m); }

  // a DT1 to 01 00 from a device: part of a dump (after its common) or a report of a change
  #take(d, name, m) {
    const area = tempArea(m);
    if (area < 0) return;
    if (area === 0x00) {                  // a dump begins (reports never carry the common): 3 messages follow
      d.dump = 3;
      if (d.adopt) d.pending = soundNew();
      return;
    }
    if (d.dump > 0) {                     // a dump: the device's whole sound
      d.dump--;
      if (d.adopt) {
        applyTemp(d.pending, m);
        if (d.dump === 0) {               // adopted: the device's sound is the edit buffer
          d.has = d.pending; d.pending = null; d.adopt = false;
          this.cur = soundCopy(d.has);
          this.onChange('adopt', name);
        }
      } else if (d.has) applyTemp(d.has, m);   // a renewal: what it has (the edit buffer wins, mended at tick)
      return;
    }
    if (!d.has) return;                   // (before its first dump: nothing to apply to)
    if (applyTemp(d.has, m) && applyTemp(this.cur, m)) this.onChange('edit', name);
  }

  // every GAP_MS: push the differences, renew the links
  tick() {
    const t = this.now();
    for (const d of [this.sim, this.fm1]) {
      if (!d) continue;
      if (t >= d.renewAt && !d.adopt) { d.renewAt = t + RENEW_MS; d.send(rqTemp()); }
      if (!d.has || d.dump > 0 || (d.pace && this.hold)) continue;   // (not known yet, mid-dump, or a transfer)
      for (let k = 0; k < 3; k++) {       // the sim: every region now; the FM-1: one message a tick
        const m = diffMsg(d.has, this.cur);
        if (!m) break;
        d.send(m);
        applyTemp(d.has, m);
        if (d.pace) break;
      }
    }
  }
}
