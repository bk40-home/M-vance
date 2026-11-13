// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance online simulator, linked to an FM-1 when wanted.
//  - Emulator: the WASM engine (mv_websim.c) plays; the panel drives it; audio from the AudioWorklet.
//  - Link FM-1 (Kris 13 Nov, Part B 4a: a switch, in place of the automatic controller mode): the sim and the FM-1
//    hold the same sound both ways (bank/link.js over the firmware's link, mvance.c link_*). The sim is muted (you
//    hear the FM-1); its knobs and buttons still drive the sim, whose edits go to the FM-1 as they happen, and the
//    FM-1's own edits show here. The keys (and a MIDI keyboard) play the FM-1, OCT moves their octave, PRESETS sends
//    the FM-1 a Program Change (its new sound then comes here).
//  (Every page at once lives on the bank editor: bank/editor.html.)
import { Link, GAP_MS } from './bank/link.js?v=20261113133234';   // ?v: build.sh's stamp

let M, pull, ctx, node, fbPtr, pullBuf;
let apiKeys, apiBtn, apiEnc, apiMidi, apiSysex, sxBuf;   // the cwrapped engine entry points (mv_websim.h)
let owed = 0, lastT = 0;                 // audio pacing: frames owed since the last pump tick, and its timestamp
const PULL_FRAMES = 2048;                // the scratch buffer size (a cap on frames moved per pump tick)
const KEY0_MIDI = 53;                    // key 0 = F3 (mvance.c KEY0); 27 keys up to G5
const BLACK = [1, 3, 6, 8, 10];          // black-key pitch classes

// ---- the FM-1 and the link ----
let midiOut = null;                      // the FM-1's MIDI output (found by name), null when none
let controller = false;                  // Link FM-1 on: the keys play the FM-1, the sim is muted (the name kept from
                                         // the controller mode it replaced)
let link = null;                         // the sim's sound and the FM-1's, kept the same (bank/link.js)
let webOct = 0, progSel = 0, noteMask = 0;
const held = new Int16Array(27).fill(-1);          // the MIDI note each key is sounding on the device (-1 = up)
const ENC_PRESETS = 6;                             // platform.c, the FM-1 layout
const indOf = {}, knobAng = {};                    // enc id -> its indicator element and cumulative angle (cosmetic)

async function boot() {
  // the Emscripten factory (build.sh: MODULARIZE, EXPORT_NAME=MVance); the .wasm fetched with the build stamp too,
  // so it always matches mvance.js
  M = await MVance({ locateFile: (path) => path + '?v=20261113133234' });
  pull    = M.cwrap('wsim_pull', 'number', ['number', 'number']);
  apiKeys = M.cwrap('wsim_keys', null, ['number']);
  apiBtn  = M.cwrap('wsim_btn',  null, ['number', 'number']);
  apiEnc  = M.cwrap('wsim_enc',  null, ['number', 'number']);
  apiMidi = M.cwrap('wsim_midi', null, ['number', 'number', 'number']);
  apiSysex = M.cwrap('wsim_sysex_in', null, ['number', 'number']);
  sxBuf   = M._malloc(1024);                    // a SysEx into the sim (the longest: a whole patch, 260 B)
  fbPtr   = M.ccall('wsim_fb', 'number', [], []);
  pullBuf = M._malloc(PULL_FRAMES * 2 * 4);     // a float32 stereo scratch buffer in the WASM heap

  M.onMidiOut = (_st, _d1, _d2) => {};
  // the sim's SysEx (its link reports): later, off the app's stack (it is mid-pass when it sends)
  M.onSysEx = (bytes) => { const m = new Uint8Array(bytes); queueMicrotask(() => link.fromSim(m)); };
  link = new Link({ simSend: sysexIn, onChange: () => setMode() });
  link.linkSim();                               // (the sim always reports to the page; the FM-1 only when linked)
  setInterval(() => link.tick(), GAP_MS);
  setInterval(pump, 8);                         // the sim's clock runs with the frames pulled: always (muted: dropped)
  wireInput();
  requestAnimationFrame(draw);
  document.getElementById('start').onclick = startAudio;   // audio needs a user gesture
  document.getElementById('link').onchange = (e) => setLink(e.target.checked);
  setMode();
}

function sysexIn(m) {                            // a whole F0..F7 message into the sim (wsim_sysex_in)
  if (m.length > 1024) return;
  M.HEAPU8.set(m, sxBuf);
  apiSysex(sxBuf, m.length);
}

function startAudio() {
  if (ctx) { ctx.resume(); return; }
  // match the engine's fixed 44.1 kHz (the context would otherwise default to 48 kHz -> wrong pitch + drift)
  ctx = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' });
  ctx.audioWorklet.addModule('audio-worklet.js').then(() => {
    node = new AudioWorkletNode(ctx, 'mvance', { outputChannelCount: [2] });
    node.connect(ctx.destination);
    document.getElementById('start').textContent = '▶ Audio on';
  });
}

// Pull only as much audio as real time has advanced (never back up). Always: the sim's clock runs with the frames it
// renders (its screen, timers and link); before Start audio, or linked (you hear the FM-1), they are dropped.
function pump() {
  const now = performance.now();
  if (lastT === 0) lastT = now;
  owed += (now - lastT) * 44100 / 1000;        // (the engine's fixed rate: mv_websim.c FS)
  lastT = now;
  if (owed > PULL_FRAMES) owed = PULL_FRAMES;
  const want = Math.floor(owed);
  if (want <= 0) return;
  const n = pull(pullBuf, want);
  owed -= n;
  if (n > 0 && node && !controller)
    node.port.postMessage(new Float32Array(M.HEAPF32.subarray(pullBuf >> 2, (pullBuf >> 2) + n * 2)));
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

// ---- mode / the FM-1 ----
function setMode() {
  const el = document.getElementById('mode');
  if (!el) return;
  const oct = `oct ${webOct >= 0 ? '+' : ''}${webOct}`;
  el.textContent = !controller ? (midiOut ? 'Emulator  ·  FM-1 found' : 'Emulator')
    : link && link.linked ? `● Linked to FM-1  ·  prog ${progSel + 1}  ·  ${oct}` : 'Linking to FM-1…';
  el.className = controller && link && link.linked ? 'mode on' : 'mode';
  document.getElementById('link').disabled = !midiOut;
  document.getElementById('link').checked = controller;
  document.getElementById('linkBox').className = midiOut ? 'link' : 'link off';
}

// the FM-1 found (its output by name) or gone: the switch is offered; gone while linked: unlinked
function setFm1(out) {
  midiOut = out || null;
  if (!midiOut && controller) setLink(false);
  setMode();
}

// Link FM-1 on / off: on, the FM-1's sound comes into the sim (link.js adopts it), the sim is muted, the keys play the
// FM-1; off, the sim plays its own (the FM-1 keeps what it has)
function setLink(on) {
  if (on && !midiOut) on = false;
  if (on === controller) { setMode(); return; }
  for (let k = 0; k < 27; k++) noteSet(k, false);                  // no note left hanging: off where it plays now
  noteMask = 0; apiKeys(0);
  controller = on;
  if (on) link.linkFm1((m) => midiOut.send(m));
  else { link.unlinkFm1(); held.fill(-1); }
  setMode();
}

// ---- a note on a key (bit 0 = F3): linked, to the FM-1 (in webOct's octave); else to the WASM engine ----
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

// ---- an encoder detent: to the sim (linked: its edits go on to the FM-1); linked, PRESETS changes the FM-1's
// program (a Program Change: the FM-1 then reports its new sound, and the sim follows) ----
function turn(id, dir) {
  if (controller && midiOut && id === ENC_PRESETS) {
    progSel = (progSel + dir + 64) % 64;
    midiOut.send([0xC0, progSel]); setMode();
  } else {
    apiEnc(id, dir);
  }
  const el = indOf[id];                                             // cosmetic: spin the indicator either way
  if (el) { knobAng[id] = (knobAng[id] || 0) + dir * 20; el.style.transform = `rotate(${knobAng[id]}deg)`; }
}

// ---- a panel button ----
// linked: OCT moves the octave the keys play the FM-1 in; the other buttons drive the sim (its pages: what the
// knobs edit, on the FM-1 too)
function pressBtn(id, down) {
  if (controller && (id === 0 || id === 1)) {
    if (down) {
      if (id === 0) webOct = Math.max(-3, webOct - 1);
      else webOct = Math.min(3, webOct + 1);
      setMode();
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

  // WebMIDI (with SysEx, for the link): find the FM-1 by name (Link FM-1 is offered); re-scan on hot-plug.
  //   from the FM-1     its link's DT1s to what plays -> link.js; its Program Changes -> the prog shown
  //   another input    (a keyboard) plays the sim, or, linked, the FM-1
  if (navigator.requestMIDIAccess) {
    const FM1_NAME = /felucca|fm-?1|m-?vance|m-?vave|dinghy/i;
    navigator.requestMIDIAccess({ sysex: true }).then((access) => {
      const scan = () => {
        for (const inp of access.inputs.values()) {
          const fromFm1 = FM1_NAME.test(inp.name || '');
          inp.onmidimessage = (m) => {
            const d = m.data;
            if (fromFm1) {
              if (d[0] === 0xf0) link.fromFm1(new Uint8Array(d));
              else if ((d[0] & 0xf0) === 0xc0) { progSel = d[1]; setMode(); }
              return;
            }
            if (d.length !== 3) return;
            if (controller && midiOut) midiOut.send(d);              // linked: a keyboard plays the FM-1
            else apiMidi(d[0], d[1], d[2]);
          };
        }
        let out = null;
        for (const o of access.outputs.values()) if (FM1_NAME.test(o.name || '')) out = o;
        setFm1(out);
      };
      scan();
      access.onstatechange = scan;
    }).catch(() => {});
  }
}

boot();
