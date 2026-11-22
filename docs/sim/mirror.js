// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance: the FM-1 mirrored into the sim while Link FM-1 is on (Kris 22 Nov 2026: mvance-link-mirror-proposal-
// 2026-11-22.md, option 1). The link keeps the sound the same (bank/link.js); this keeps what the FM-1 PLAYS the same,
// so the muted sim draws it: its scope, its lit keys, its SEQ pages and their playheads.
//   tracks     the FM-1's UPPER and LOWER tracks go into the sim's (area 04: seq-model.js writeMsgs, as Send), when the
//              link starts and whenever the FM-1 reports a track changed (a recording, a CLIP LOAD, a Send). Their
//              notes only: an automation point would move the sim's sound, the sim would report it, and the link
//              would push it on to the FM-1, which plays its own points. The FM-1 reports what its points do (the
//              link), so the sim's sound follows them anyway
//   transport  the FM-1's MIDI clock, START / CONTINUE and STOP (sent while its CLK OUT is on) go into the sim, whose
//              sequencer then runs on the FM-1's pulses (mvance.c midi_in: seq_ext), step for step
//   notes      the notes the FM-1's keys send, and those played on this page (they go to the FM-1), into the sim
// Two ways to get the tracks:
//   own reads  (the simulator page: nothing else reads the FM-1's tracks) readFm1 given: the mirror asks for them
//              (RQ1 04 tt, one read at a time: the FM-1 answers one) and follows the FM-1's reports
//   fed        (the editor: its Sequences panel reads them already, and a second reader would cut its reads short) no
//              readFm1: the panel hands each live track it reads to track()
// No DOM, no timers but the read's give-up: tests drive it with pretend devices (mirror-test.mjs).
import * as S from './seq-model.js?v=20261122212620';

export const READ_GIVE_UP_MS = 4000;      // a track not read in this long: the next one is asked for

export class Mirror {
  // simSysex(msg): a whole F0..F7 into the sim. simMidi(st, d1, d2): a MIDI message into the sim.
  // readFm1(msgs): Promise, sends to the FM-1 (paced by the caller); omitted: fed by track() instead.
  // onState(): the mirror's state changed (the page shows it). setTimeout / clearTimeout: tests pass their own
  constructor({ simSysex, simMidi, readFm1 = null, onState = () => {},
                setTimeout: st = (f, ms) => setTimeout(f, ms), clearTimeout: ct = (t) => clearTimeout(t) }) {
    Object.assign(this, { simSysex, simMidi, readFm1, onState, st, ct });
    this.on = false;
    this.ver = [-1, -1];                  // the FM-1's seq.ver of each track as last mirrored (-1: not yet)
    this.queue = [];                      // tracks still to read (own reads)
    this.reading = -1;                    // the track being read (-1: none)
    this.timer = 0;
    this.clock = false;                   // a clock has come from the FM-1 since the link (CLK OUT is on)
    this.reader = new S.SeqReader({
      onClip: (tt, c, h) => { if (tt < 2) { this.track(tt, c, h); this.#next(); } },
      onHead: (tt, h) => { if (tt < 2 && h.ver !== this.ver[tt]) this.#want(tt); },   // a report: that track changed
    });
  }

  // Link FM-1 on: the tracks are read (own reads) or will be handed in (fed)
  start() {
    this.on = true; this.ver = [-1, -1]; this.clock = false;
    if (this.readFm1) { this.#want(0); this.#want(1); }
    this.onState();
  }
  // Link FM-1 off: the sim's sequencer stopped (it was running on the FM-1's pulses, which no longer come) and no note
  // left sounding in it. It keeps the tracks it has
  stop() {
    if (!this.on) return;
    this.on = false; this.queue = []; this.reading = -1; this.ct(this.timer);
    this.simMidi(0xfc, 0, 0);
    for (let ch = 0; ch < 16; ch++) this.simMidi(0xb0 | ch, 123, 0);   // ALL NOTES OFF
    this.onState();
  }

  // a live track (tt 0 UPPER / 1 LOWER) as the FM-1 has it, into the sim's: its notes only (see the top)
  track(tt, clip, head) {
    if (!this.on || tt > 1 || !clip) return;
    const c = notesOnly(clip);
    if (S.validate(c)) return;            // (the FM-1 holds only what it accepted: a clip refused here is not sent)
    for (const m of S.writeMsgs(S.TT.track(tt), c)) this.simSysex(m);
    if (head) this.ver[tt] = head.ver;
    this.onState();
  }

  // a message from the FM-1 (any): true when the mirror took it. Area 04 goes to the own reader (fed: the caller's
  // panel reads it); the transport and the notes go into the sim
  fromFm1(m) {
    if (!this.on || !m || !m.length) return false;
    const st = m[0];
    if (st === 0xf0) return this.readFm1 ? this.reader.feed(m) : false;
    if (st === 0xf8 || st === 0xfa || st === 0xfb || st === 0xfc) {
      if (st === 0xf8 && !this.clock) { this.clock = true; this.onState(); }
      this.simMidi(st, 0, 0);
      return true;
    }
    if ((st & 0xe0) === 0x80 && m.length >= 3) { this.simMidi(st, m[1], m[2]); return true; }   // note off / on
    return false;
  }
  // a note played on this page (sent to the FM-1): the sim plays it too, muted, so its keys light
  note(st, n, v) { if (this.on) this.simMidi(st, n, v); }

  // ---- own reads: one at a time, in order; a report for a track already waiting adds nothing ----
  #want(tt) {
    if (!this.readFm1 || !this.on) return;
    if (this.reading !== tt && !this.queue.includes(tt)) this.queue.push(tt);
    if (this.reading < 0) this.#next();
  }
  #next() {
    if (!this.readFm1) return;
    this.ct(this.timer);
    this.reading = this.on && this.queue.length ? this.queue.shift() : -1;
    if (this.reading < 0) return;
    const tt = this.reading;
    this.reader.expect(S.TT.track(tt));
    this.timer = this.st(() => { this.reader.want.delete(S.TT.track(tt)); this.#next(); }, READ_GIVE_UP_MS);
    Promise.resolve(this.readFm1([S.readMsg(S.TT.track(tt))])).catch(() => {});
  }
}

// a clip without its automation points (notes only, in order)
export function notesOnly(clip) {
  return { ...S.clipCopy(clip), events: clip.events.filter((e) => !S.isPoint(e)) };
}
