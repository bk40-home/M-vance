// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance online simulator + FM-1 controller.
//  - Emulator: the WASM engine (mv_websim.c) plays; the panel drives it; audio from the AudioWorklet.
//  - Controller (auto): when a real M-vance FM-1 is found on WebMIDI, the page drives the hardware instead -
//    the keyboard sends MIDI notes, SELECT/PRESETS send Program Change, the knobs send the firmware's CCs,
//    and the WASM engine is muted. (Panel buttons + a mirrored screen are a later firmware step.)
let M, pull, ctx, node, fbPtr, pullBuf;
let apiKeys, apiBtn, apiEnc, apiMidi;    // the cwrapped engine entry points (mv_websim.h)
let owed = 0, lastT = 0;                 // audio pacing: frames owed since the last pump tick, and its timestamp
const PULL_FRAMES = 2048;                // the scratch buffer size (a cap on frames moved per pump tick)
const KEY0_MIDI = 53;                    // key 0 = F3 (mvance.c KEY0); 27 keys up to G5
const BLACK = [1, 3, 6, 8, 10];          // black-key pitch classes

// ---- controller state ----
let midiOut = null, controller = false, webOct = 0, progSel = 0, noteMask = 0;
const held = new Int16Array(27).fill(-1);          // the MIDI note each key is sounding on the device (-1 = up)
const KNOB_CC = { 2: 74, 3: 71, 4: 94, 5: 91 };    // KNOB1..4 -> CUTOFF, RESO, DELAY level, REVERB (firmware CCs)
const knobVal = {};                                // enc id -> last absolute CC value (a relative knob made absolute)
const indOf = {}, knobAng = {};                    // enc id -> its indicator element and cumulative angle (cosmetic)

async function boot() {
  M = await MVance();                           // the Emscripten factory (build.sh: MODULARIZE, EXPORT_NAME=MVance)
  pull    = M.cwrap('wsim_pull', 'number', ['number', 'number']);
  apiKeys = M.cwrap('wsim_keys', null, ['number']);
  apiBtn  = M.cwrap('wsim_btn',  null, ['number', 'number']);
  apiEnc  = M.cwrap('wsim_enc',  null, ['number', 'number']);
  apiMidi = M.cwrap('wsim_midi', null, ['number', 'number', 'number']);
  fbPtr   = M.ccall('wsim_fb', 'number', [], []);
  pullBuf = M._malloc(PULL_FRAMES * 2 * 4);     // a float32 stereo scratch buffer in the WASM heap

  M.onMidiOut = (_st, _d1, _d2) => {};
  M.onSysEx = (_bytes) => {};
  wireInput();
  requestAnimationFrame(draw);
  document.getElementById('start').onclick = startAudio;   // audio needs a user gesture
  setMode();
}

function startAudio() {
  if (ctx) { ctx.resume(); return; }
  // match the engine's fixed 44.1 kHz (the context would otherwise default to 48 kHz -> wrong pitch + drift)
  ctx = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' });
  ctx.audioWorklet.addModule('audio-worklet.js').then(() => {
    node = new AudioWorkletNode(ctx, 'mvance', { outputChannelCount: [2] });
    node.connect(ctx.destination);
    owed = 0; lastT = 0;
    setInterval(pump, 8);
    document.getElementById('start').textContent = '▶ Audio on';
  });
}

// Pull only as much audio as real time has advanced (never back up). In controller mode the WASM is silent.
function pump() {
  if (!node || !ctx || controller) return;
  const now = performance.now();
  if (lastT === 0) lastT = now;
  owed += (now - lastT) * ctx.sampleRate / 1000;
  lastT = now;
  if (owed > PULL_FRAMES) owed = PULL_FRAMES;
  const want = Math.floor(owed);
  if (want <= 0) return;
  const n = pull(pullBuf, want);
  owed -= n;
  if (n > 0) node.port.postMessage(new Float32Array(M.HEAPF32.subarray(pullBuf >> 2, (pullBuf >> 2) + n * 2)));
}

function draw() {
  const c = document.getElementById('screen').getContext('2d');
  const img = c.createImageData(240, 240);
  const px = M.HEAPU16.subarray(fbPtr >> 1, (fbPtr >> 1) + 240 * 240);
  for (let i = 0; i < 240 * 240; i++) {
    const v = px[i];                            // RGB565
    img.data[i * 4]     = ((v >> 11) & 31) * 255 / 31;
    img.data[i * 4 + 1] = ((v >> 5) & 63) * 255 / 63;
    img.data[i * 4 + 2] = (v & 31) * 255 / 31;
    img.data[i * 4 + 3] = 255;
  }
  c.putImageData(img, 0, 0);
  requestAnimationFrame(draw);
}

// ---- mode / device detection ----
function setMode() {
  const el = document.getElementById('mode');
  if (!el) return;
  el.textContent = controller
    ? `● FM-1  ·  prog ${progSel + 1}  ·  oct ${webOct >= 0 ? '+' : ''}${webOct}`
    : 'Emulator';
  el.className = controller ? 'mode on' : 'mode';
}

function setController(out) {
  const was = controller;
  midiOut = out || null;
  controller = !!out;
  if (controller && !was) { noteMask = 0; if (apiKeys) apiKeys(0); }   // entering: drop any emulator notes, mute WASM
  if (!controller && was) { lastT = 0; }                              // leaving: reset the audio pacing
  setMode();
}

// ---- a note on a key (bit 0 = F3): to the device in controller mode, else to the WASM engine ----
function noteSet(k, on) {
  if (k < 0 || k > 26) return;
  if (controller && midiOut) {
    if (on) {
      const n = KEY0_MIDI + k + 12 * webOct;
      if (n >= 0 && n < 128) { held[k] = n; midiOut.send([0x90, n, 100]); }
    } else if (held[k] >= 0) {
      midiOut.send([0x80, held[k], 0]); held[k] = -1;              // off the exact note sent (octave may have moved)
    }
    return;
  }
  noteMask = on ? (noteMask | (1 << k)) : (noteMask & ~(1 << k));
  apiKeys(noteMask);
}

// ---- an encoder detent: Program Change / CC to the device in controller mode, else to the WASM engine ----
function turn(id, dir) {
  if (controller && midiOut) {
    if (id === 0 || id === 6) {                                      // SELECT / PRESETS -> Program Change
      progSel = (progSel + dir + 64) % 64;
      midiOut.send([0xC0, progSel]); setMode();
    } else if (id in KNOB_CC) {                                      // KNOB1..4 -> a parameter CC (absolute sweep)
      let v = (knobVal[id] === undefined ? 64 : knobVal[id]) + dir * 4;
      v = v < 0 ? 0 : v > 127 ? 127 : v; knobVal[id] = v;
      midiOut.send([0xB0, KNOB_CC[id], v]);
    }
  } else {
    apiEnc(id, dir);
  }
  const el = indOf[id];                                             // cosmetic: spin the indicator either way
  if (el) { knobAng[id] = (knobAng[id] || 0) + dir * 20; el.style.transform = `rotate(${knobAng[id]}deg)`; }
}

// ---- a panel button: drives the WASM engine; in controller mode only OCT acts (shifts the sent octave) ----
function pressBtn(id, down) {
  if (controller) {
    if (down) {
      if (id === 0) webOct = Math.max(-3, webOct - 1);
      else if (id === 1) webOct = Math.min(3, webOct + 1);
      setMode();                                                    // other buttons: no device action yet (Tier 2)
    }
    return;
  }
  apiBtn(id, down);
}

// ---- the on-screen 27-note keyboard (F3..G5); white keys flow, black keys straddle the gaps ----
function buildKeyboard() {
  const host = document.getElementById('keys');
  host.innerHTML = '';
  const WW = 36, BW = 22, blacks = [];
  let x = 0;
  for (let k = 0; k < 27; k++) {
    const isBlack = BLACK.includes((KEY0_MIDI + k) % 12);
    const el = document.createElement('div');
    el.className = 'key ' + (isBlack ? 'black' : 'white');
    el.style.left = (isBlack ? x - BW / 2 : x) + 'px';
    if (!isBlack) x += WW; else blacks.push(el);
    const dn = (e) => { e.preventDefault(); el.setPointerCapture?.(e.pointerId); noteSet(k, true); el.classList.add('on'); };
    const up = () => { noteSet(k, false); el.classList.remove('on'); };
    el.addEventListener('pointerdown', dn);
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
    el.addEventListener('pointerleave', up);
    host.appendChild(el);
  }
  blacks.forEach((b) => host.appendChild(b));
  host.style.width = x + 'px';
}

// ---- a rotary encoder: drag (up = clockwise), scroll; routes through turn() ----
function wireKnob(el) {
  const id = +el.dataset.enc;
  indOf[id] = el.querySelector('.ind');
  let dragging = false, acc = 0;
  el.addEventListener('wheel', (e) => { turn(id, e.deltaY < 0 ? 1 : -1); e.preventDefault(); }, { passive: false });
  el.addEventListener('pointerdown', (e) => { dragging = true; acc = 0; el.setPointerCapture(e.pointerId); });
  el.addEventListener('pointermove', (e) => {
    if (!dragging) return;
    acc += e.movementY;
    while (acc <= -6) { turn(id, 1); acc += 6; }
    while (acc >= 6)  { turn(id, -1); acc -= 6; }
  });
  const end = () => { dragging = false; };
  el.addEventListener('pointerup', end);
  el.addEventListener('pointercancel', end);
}

function wireInput() {
  // computer keyboard notes
  const ROW = 'zsxdcvgbhnjm,l.;/';
  addEventListener('keydown', (e) => { const i = ROW.indexOf(e.key); if (i >= 0 && !e.repeat) noteSet(i, true); });
  addEventListener('keyup',   (e) => { const i = ROW.indexOf(e.key); if (i >= 0) noteSet(i, false); });

  // knob + octave shortcuts
  addEventListener('keydown', (e) => {
    switch (e.key) {
      case 'ArrowUp':    turn(0, 1);  e.preventDefault(); break;    // SELECT +
      case 'ArrowDown':  turn(0, -1); e.preventDefault(); break;
      case 'ArrowRight': turn(1, 1);  e.preventDefault(); break;    // ALGORITHM +
      case 'ArrowLeft':  turn(1, -1); e.preventDefault(); break;
      case '[': pressBtn(0, true); pressBtn(0, false); break;        // OCT -
      case ']': pressBtn(1, true); pressBtn(1, false); break;        // OCT +
    }
  });

  buildKeyboard();
  document.querySelectorAll('.knob[data-enc]').forEach((el) => wireKnob(el));
  document.querySelectorAll('button[data-btn]').forEach((el) => {
    const id = +el.dataset.btn;
    el.addEventListener('pointerdown', () => { pressBtn(id, 1); el.classList.add('on'); });
    el.addEventListener('pointerup',   () => { pressBtn(id, 0); el.classList.remove('on'); });
    el.addEventListener('pointerleave', () => { pressBtn(id, 0); el.classList.remove('on'); });
  });

  // WebMIDI: find a real FM-1 output and auto-switch to controller; re-scan on hot-plug
  if (navigator.requestMIDIAccess) {
    navigator.requestMIDIAccess().then((access) => {
      const scan = () => {
        for (const inp of access.inputs.values())
          inp.onmidimessage = (m) => { if (m.data.length === 3 && !controller) apiMidi(m.data[0], m.data[1], m.data[2]); };
        let out = null;
        for (const o of access.outputs.values()) if (/felucca|fm-?1|m-?vance/i.test(o.name || '')) out = o;
        setController(out);
      };
      scan();
      access.onstatechange = scan;
    }).catch(() => {});
  }
}

boot();
