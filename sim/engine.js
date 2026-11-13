// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance simulator engine for a page (Kris 13 Nov 2026, Part B): the WebAssembly app (mvance.js / .wasm, built by
// sim/build.sh) loaded, run in real time, its screen drawn, its SysEx in and out. The bank editor uses it
// (sim/bank/editor.js); the simulator page keeps its own copy of the same loop (sim.js) for now.
//
//   clock    the app's clock runs with the audio it renders (mv_websim.c), so pump() pulls real time's worth of
//            frames every PUMP_MS whether or not they are heard: muted (the FM-1 linked) or before Start audio, the
//            frames are dropped, and the sim's screen, timers and link still run at the right speed
//   sysex    sysexIn(msg): a whole F0..F7 message into the app (wsim_sysex_in). What the app sends comes to
//            onSysEx(msg) a moment later (queueMicrotask): never while the app is mid-pass, so a handler may call
//            back into the sim
//
//   const eng = await startEngine({ base: '../', stamp: '?v=...', screen: canvas });
//   eng.onSysEx = (m) => ...; eng.sysexIn(m); eng.keys(mask); eng.startAudio(); eng.muted = true;

const PULL_FRAMES = 2048;                 // the pull buffer (frames): at most this much a pump
const PUMP_MS = 8;                        // how often the app is fed real time
const FS = 44100;                         // the engine's fixed rate (mv_websim.c FS)

export async function startEngine({ base = './', stamp = '', screen = null, worklet = 'audio-worklet.js' } = {}) {
  if (typeof MVance !== 'function') throw new Error('the simulator (mvance.js) is not built: run sh sim/build.sh');
  const M = await MVance({ locateFile: (p) => base + p + stamp });
  const api = {
    pull: M.cwrap('wsim_pull', 'number', ['number', 'number']),
    keys: M.cwrap('wsim_keys', null, ['number']),
    btn: M.cwrap('wsim_btn', null, ['number', 'number']),
    enc: M.cwrap('wsim_enc', null, ['number', 'number']),
    midi: M.cwrap('wsim_midi', null, ['number', 'number', 'number']),
    sysex: M.cwrap('wsim_sysex_in', null, ['number', 'number']),
  };
  const fbPtr = M.ccall('wsim_fb', 'number', [], []);
  const pullBuf = M._malloc(PULL_FRAMES * 2 * 4);   // float32 stereo
  const sxBuf = M._malloc(1024);                     // a SysEx in (the longest: a whole patch, 260 B)
  let ctx = null, node = null, owed = 0, lastT = 0;

  const eng = {
    M, muted: false,
    onSysEx: () => {},
    sysexIn(m) {
      if (m.length > 1024) return;
      M.HEAPU8.set(m, sxBuf);
      api.sysex(sxBuf, m.length);
    },
    keys: (mask) => api.keys(mask),
    btn: (id, down) => api.btn(id, down),
    enc: (id, d) => api.enc(id, d),
    midi: (st, d1, d2) => api.midi(st, d1, d2),
    startAudio() {                         // (needs a user gesture)
      if (ctx) { ctx.resume(); return Promise.resolve(); }
      ctx = new AudioContext({ sampleRate: FS, latencyHint: 'interactive' });
      return ctx.audioWorklet.addModule(base + worklet + stamp).then(() => {
        node = new AudioWorkletNode(ctx, 'mvance', { outputChannelCount: [2] });
        node.connect(ctx.destination);
      });
    },
    get audioOn() { return !!node; },
  };

  // the app's messages out: later, off the app's stack (it is mid-pass when it sends)
  M.onSysEx = (bytes) => { const m = new Uint8Array(bytes); queueMicrotask(() => eng.onSysEx(m)); };
  M.onMidiOut = () => {};

  setInterval(() => {                     // real time's worth of frames: heard, or dropped (the clock runs on)
    const now = performance.now();
    if (lastT === 0) lastT = now;
    owed = Math.min(PULL_FRAMES, owed + (now - lastT) * FS / 1000);
    lastT = now;
    const want = Math.floor(owed);
    if (want <= 0) return;
    const n = api.pull(pullBuf, want);
    owed -= n;
    if (n > 0 && node && !eng.muted)
      node.port.postMessage(new Float32Array(M.HEAPF32.subarray(pullBuf >> 2, (pullBuf >> 2) + n * 2)));
  }, PUMP_MS);

  if (screen) {                           // the screen, as sim.js draws it (RGB565 -> RGBA)
    const c = screen.getContext('2d'), img = c.createImageData(240, 240);
    const draw = () => {
      const px = M.HEAPU16.subarray(fbPtr >> 1, (fbPtr >> 1) + 240 * 240);
      const d = img.data;
      for (let i = 0, o = 0; i < px.length; i++, o += 4) {
        const v = px[i];
        d[o] = ((v >> 11) & 31) * 255 / 31; d[o + 1] = ((v >> 5) & 63) * 255 / 63; d[o + 2] = (v & 31) * 255 / 31; d[o + 3] = 255;
      }
      c.putImageData(img, 0, 0);
      requestAnimationFrame(draw);
    };
    requestAnimationFrame(draw);
  }
  return eng;
}
