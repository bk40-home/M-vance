// SPDX-License-Identifier: GPL-3.0-only
// M-vance simulator — AudioWorklet: plays interleaved stereo frames posted from the main thread.
// A safety cap drops the oldest audio if the queue ever runs ahead, so latency can't grow without bound
// (the real-time pump in sim.js keeps us well under the cap in normal use).
const MAX_FRAMES = 8192;   // ~0.19 s of backlog allowed before the oldest is dropped
class MvanceProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.queue = [];      // Float32Array chunks, interleaved L R
    this.frames = 0;      // stereo frames waiting in the queue (not counting the chunk in progress)
    this.cur = null;
    this.pos = 0;
    this.port.onmessage = (e) => {
      this.queue.push(e.data); this.frames += e.data.length >> 1;
      while (this.frames > MAX_FRAMES) { const d = this.queue.shift(); this.frames -= d.length >> 1; }
    };
  }
  process(_inputs, outputs) {
    const L = outputs[0][0], R = outputs[0][1];
    for (let i = 0; i < L.length; i++) {
      if (!this.cur || this.pos >= this.cur.length) {
        this.cur = this.queue.shift() || null; this.pos = 0;
        if (this.cur) this.frames -= this.cur.length >> 1;
      }
      if (this.cur) { L[i] = this.cur[this.pos++]; R[i] = this.cur[this.pos++]; }
      else { L[i] = 0; R[i] = 0; }
    }
    return true;
  }
}
registerProcessor('mvance', MvanceProcessor);
