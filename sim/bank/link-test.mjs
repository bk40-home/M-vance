// SPDX-License-Identifier: GPL-3.0-only
// The bank editor's link (sim/bank/link.js; Kris 13 Nov, Part B) against two pretend devices, run with Node:
//   node sim/bank/link-test.mjs            # from the repo root (needs build/mvance-factory.syx: tools/make_bank.py)
// A pretend device does what the firmware's link does (firmware/src/mvance.c link_*, pinned by tests/link_test.py):
// a DT1 to 01 00 writes what plays (and the page then has those bytes: no echo); RQ1 01 00 links it and is answered
// with a 4-message dump (common first); while linked, each pass reports the first region that differs as one DT1.
import { readFileSync } from 'node:fs';
import { parseBankFile, perfMsgs, soundOf, soundCopy, soundEqual, applyTemp, diffMsg, tempArea, itemFromSound }
  from './bank-model.js';
import { Link, GAP_MS, RENEW_MS } from './link.js';

let fail = 0;
const ok = (c, msg) => { console.log((c ? 'OK   ' : 'FAIL ') + msg); if (!c) fail = 1; };
const P = parseBankFile(new Uint8Array(readFileSync('build/mvance-factory.syx'))).filter((i) => i.kind === 'P');
const CUTOFF = 41;

class Device {                            // what plays, the page's copy of it, its messages out (to the link)
  constructor(sound, name) { Object.assign(this, { eng: soundCopy(sound), have: null, out: [], rx: [], name }); }
  receive(m) {                            // a message from the page
    this.rx.push(m);
    if (m[5] === 0x11) {                  // RQ1 01 00: link, dump
      this.have = soundCopy(this.eng);
      for (const x of perfMsgs(itemFromSound(this.eng), 0x01, 0x00)) this.out.push(x);
    } else if (tempArea(m) >= 0) {        // DT1 01 00: written; the page has those bytes
      applyTemp(this.eng, m);
      if (this.have) applyTemp(this.have, m);
    }
  }
  pass(to) {                              // one firmware pass: a queued dump message, else one report
    if (this.out.length) { to(this.out.shift()); return; }
    if (!this.have) return;
    const m = diffMsg(this.have, this.eng);
    if (m) { applyTemp(this.have, m); to(m); }
  }
  run(to, n = 10) { for (let i = 0; i < n; i++) this.pass(to); }
}

let t = 0;
const sim = new Device(soundOf(P[0]), 'sim');
const fm1 = new Device(soundOf(P[5]), 'fm1');
const changes = [];
const link = new Link({ simSend: (m) => sim.receive(m), onChange: (w, d) => changes.push(w + ':' + d), now: () => t });
const toPage = { sim: (m) => link.fromSim(m), fm1: (m) => link.fromFm1(m) };
const settle = (ticks = 10) => {          // devices run, the page ticks GAP_MS apart
  for (let i = 0; i < ticks; i++) { sim.run(toPage.sim); fm1.run(toPage.fm1); t += GAP_MS; link.tick(); }
  sim.run(toPage.sim); fm1.run(toPage.fm1);
};

// 1 the sim links at start; its program becomes the edit buffer
link.linkSim(); settle();
ok(soundEqual(link.cur, soundOf(P[0])) && changes.includes('adopt:sim'), 'sim linked: its program is the edit buffer');

// 2 a tile edit in the sim (a knob on the sim): into the edit buffer, not pushed back to the sim
sim.rx = []; sim.eng.up[CUTOFF] -= 3; settle();
ok(link.cur.up[CUTOFF] === sim.eng.up[CUTOFF], 'a sim edit reaches the edit buffer');
ok(sim.rx.length === 0, 'and is not echoed back to the sim');

// 3 Link FM-1: the FM-1's sound becomes the edit buffer (the hardware leads) and the sim follows
link.linkFm1((m) => fm1.receive(m)); settle();
ok(link.linked && soundEqual(link.cur, fm1.eng), 'Link FM-1: the FM-1\'s sound is the edit buffer');
ok(soundEqual(sim.eng, fm1.eng), 'the sim now plays the FM-1\'s sound');

// 4 a tile edit (sim) goes to the FM-1 as one 1-byte DT1; 5 a hardware edit comes to the sim, not back to the FM-1
fm1.rx = []; sim.eng.up[CUTOFF] = 10; settle();
ok(fm1.eng.up[CUTOFF] === 10, 'a tile edit reaches the FM-1');
ok(fm1.rx.length === 1 && fm1.rx[0].length === 13 && fm1.rx[0][8] === 0x40 && fm1.rx[0][9] === CUTOFF,
   `as one DT1 01 00 40 29 of one byte (${fm1.rx.length} message(s))`);
fm1.rx = []; sim.rx = []; fm1.eng.pf[0] = 1; fm1.eng.up[42] = 77; settle();   // MODE -> DUAL, RESO 77 on the hardware
ok(sim.eng.pf[0] === 1 && sim.eng.up[42] === 77 && link.cur.pf[0] === 1, 'hardware edits (MODE, RESO) reach the sim');
ok(fm1.rx.length === 0, 'and are not echoed back to the FM-1');

// 6 a knob sweep in the sim: the FM-1 gets the newest value, at most one message a tick
fm1.rx = []; let ticks = 0;
for (let v = 20; v < 60; v++) { sim.eng.up[CUTOFF] = v; sim.run(toPage.sim, 1); if (v % 4 === 0) { t += GAP_MS; link.tick(); ticks++; fm1.run(toPage.fm1); } }
settle(3); ticks += 3;
ok(fm1.eng.up[CUTOFF] === 59, 'sweep: the FM-1 ends on the last value');
ok(fm1.rx.length <= ticks + 1, `sweep: paced (${fm1.rx.length} messages for 40 steps over ${ticks} ticks)`);

// 7 Edit a slot (load): the sim at once, the FM-1 paced; both end on the slot's sound
sim.rx = []; fm1.rx = [];
link.load(soundOf(P[9]));
ok(soundEqual(sim.eng, soundOf(P[9])) && sim.rx.length <= 3, `load: the sim at once (${sim.rx.length} message(s))`);
ok(fm1.rx.length <= 1, 'load: the FM-1 one message a tick');
settle(5);
ok(soundEqual(fm1.eng, soundOf(P[9])) && fm1.rx.length <= 3, `load: the FM-1 has it after a few ticks (${fm1.rx.length})`);

// 8 a bank transfer holds the FM-1 pushes; they resume after
link.hold = true; fm1.rx = []; link.load(soundOf(P[3])); settle(3);
ok(fm1.rx.length === 0, 'hold: nothing to the FM-1 during a bank transfer');
link.hold = false; settle(5);
ok(soundEqual(fm1.eng, soundOf(P[3])), 'hold released: the FM-1 catches up');

// 9 renew: after RENEW_MS both are read again; a byte the FM-1 lost is mended toward the edit buffer
fm1.eng.up[50] ^= 1; fm1.have = soundCopy(fm1.eng);   // (a change the FM-1 never reported: the dump shows it)
t += RENEW_MS; fm1.rx = []; settle(5);
ok(fm1.rx.some((m) => m[5] === 0x11), 'renew: RQ1 01 00 again');
ok(soundEqual(fm1.eng, link.cur) && soundEqual(link.cur, soundOf(P[3])), 'renew: the FM-1 mended to the edit buffer');

// 10 PRESETS on the FM-1: its reports bring the new program into the edit buffer and the sim
fm1.eng = soundOf(P[20]); settle(6);
ok(soundEqual(link.cur, soundOf(P[20])) && soundEqual(sim.eng, soundOf(P[20])), 'program change on the FM-1: page and sim follow');

// 11 unlink: edits stay in the sim
link.unlinkFm1(); fm1.rx = []; sim.eng.up[CUTOFF] = 5; settle();
ok(fm1.rx.length === 0 && link.cur.up[CUTOFF] === 5, 'unlinked: nothing to the FM-1, the sim still edits');

process.exit(fail);
