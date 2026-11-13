// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-vance simulator — every page at once, as tiles (Kris 13 Nov 2026;
// fm1-port/mvance-all-pages-editor-proposal-2026-11-13.md: 1a firmware-drawn, 2a editable, 3a every page).
//
// Each tile is the firmware's own drawing of that page (sim/mv_tiles.c runs mv_ui_draw on a copy of the panel), so
// it always looks exactly like the FM-1's screen on that page. Grouped under the button that reaches the page, in
// the panel's order.
//
//   edit     scroll or drag (up = clockwise) over a column = that knob: wsim_goto(page) then wsim_enc(KNOBn), the
//            real knob path (steps, fast-turn x4, CC out, the highlight); the screen shows that page meanwhile
//   show     a click (no drag) shows the page on the screen
//   redraw   about 15 times a second each tile's key is read (wsim_page_key, cheap); a tile is drawn again only when
//            its key changed, so an edit redraws only the tiles that show that parameter
//
// The bank editor mounts it (Part B, 4a; the simulator page had it in Part A and dropped it on 13 Nov: the editor is
// where sounds are edited):
//   mountPages(M, hostElement, { editable: () => bool, pageEditable: (title, button) => bool,
//                                sizeInput: <input type=range> })
// editable: all the tiles (false: greyed, read-only). pageEditable: one page (Part B 4a: while the FM-1 is linked,
// the pages of FM-1-wide settings, not the sound's, are greyed: they would only change the sim). Its own CSS is
// added to the page once (the .pgroup / .tile classes).

const CSS = `
  #pages { display:flex; flex-wrap:wrap; gap:12px; justify-content:center; }
  .pgroup { background:#262628; border-radius:12px; padding:8px 10px 10px; }
  .pgroup h3 { margin:0 0 6px; font-size:.6rem; font-weight:normal; letter-spacing:.1em; color:#ff7a1f; }
  .ptiles { display:flex; flex-wrap:wrap; gap:6px; }
  .tile canvas { display:block; width:var(--tile-w,168px); aspect-ratio:240 / 216; image-rendering:pixelated;
                 border:2px solid #000; border-radius:5px; cursor:ns-resize; touch-action:none; background:#000; }
  .tile.live canvas { border-color:#ff7a1f; }                    /* the page the screen shows */
  #pages.readonly .tile canvas, .tile.locked canvas { cursor:default; opacity:.45; }   /* not editable now */
  /* smaller than the screen: smoothed (sharp pixels at a fractional scale drop whole rows of the 8 px font and
     the labels break up; Kris 13 Nov "text is very hard to read"). 1:1 and larger: the device's sharp pixels */
  #pages.smooth .tile canvas { image-rendering:auto; }
`;
function addCss() {
  if (document.getElementById('pages-css')) return;
  const st = document.createElement('style');
  st.id = 'pages-css'; st.textContent = CSS;
  document.head.appendChild(st);
}

const TILE_W = 240;                     // the screen's width
const TILE_H = 216;                     // the header and the 4 columns (sim/mv_tiles.c TILE_H); not the edit line
const ENC_KNOB1 = 2;                    // platform.c, the FM-1 layout: KNOB1..4 = encoder ids 2..5
const REFRESH_MS = 66;                  // ~15 Hz key check
const DRAG_PX = 6;                      // as the sim's knobs: 6 px of drag = one detent

export function mountPages(M, host, { editable = () => true, pageEditable = () => true, sizeInput = null } = {}) {
  addCss();
  const api = {
    npages: M.cwrap('wsim_npages', 'number', []),
    isTile: M.cwrap('wsim_page_tile', 'number', ['number']),
    title:  M.cwrap('wsim_page_title', 'string', ['number']),
    btn:    M.cwrap('wsim_page_btn', 'string', ['number']),
    sub:    M.cwrap('wsim_page_sub', 'number', ['number']),
    live:   M.cwrap('wsim_live_page', 'number', []),
    key:    M.cwrap('wsim_page_key', 'number', ['number']),
    tile:   M.cwrap('wsim_tile', 'number', ['number']),
    go:     M.cwrap('wsim_goto', 'number', ['number']),
    enc:    M.cwrap('wsim_enc', null, ['number', 'number']),
  };

  // ---- build: one group per button, its pages in table order (= the order the button steps through them) ----
  const tiles = [];
  const groups = new Map();
  host.innerHTML = '';
  const n = api.npages();
  for (let i = 0; i < n; i++) {
    if (!api.isTile(i)) continue;
    const b = api.btn(i);
    if (!groups.has(b)) {
      const g = document.createElement('div');
      g.className = 'pgroup';
      g.innerHTML = `<h3>${b}</h3><div class="ptiles"></div>`;
      host.appendChild(g);
      groups.set(b, g.querySelector('.ptiles'));
    }
    const s = api.sub(i);
    const el = document.createElement('div');
    el.className = 'tile';
    const cv = document.createElement('canvas');
    cv.width = TILE_W; cv.height = TILE_H;
    cv.title = `${api.title(i)}${(s >> 8) > 1 ? ` ${(s & 255) + 1}/${s >> 8}` : ''} (${b})`;
    el.appendChild(cv);
    groups.get(b).appendChild(el);
    const ctx = cv.getContext('2d');
    const t = { i, el, cv, ctx, img: ctx.createImageData(TILE_W, TILE_H), key: null, title: api.title(i), btn: b };
    tiles.push(t);
    wire(t);
  }

  // ---- input: which column (knob) the pointer is over ----
  function colAt(t, clientX) {
    const r = t.cv.getBoundingClientRect();
    const c = Math.floor((clientX - r.left) / r.width * 4);
    return c < 0 ? 0 : c > 3 ? 3 : c;
  }
  const canEdit = (t) => editable() && pageEditable(t.title, t.btn);
  function turn(t, col, dir) {
    if (!canEdit(t)) return;
    if (api.go(t.i) < 0) return;                 // refused (the bank question or a transfer holds the screen)
    api.enc(ENC_KNOB1 + col, dir);
  }
  function wire(t) {
    t.cv.addEventListener('wheel', (e) => {
      e.preventDefault();
      turn(t, colAt(t, e.clientX), e.deltaY < 0 ? 1 : -1);
    }, { passive: false });
    let down = false, moved = false, acc = 0, col = 0;
    t.cv.addEventListener('pointerdown', (e) => {
      down = true; moved = false; acc = 0; col = colAt(t, e.clientX);
      t.cv.setPointerCapture?.(e.pointerId);
    });
    t.cv.addEventListener('pointermove', (e) => {
      if (!down) return;
      acc += e.movementY;
      while (acc <= -DRAG_PX) { turn(t, col, 1); acc += DRAG_PX; moved = true; }
      while (acc >= DRAG_PX)  { turn(t, col, -1); acc -= DRAG_PX; moved = true; }
    });
    const end = () => {
      if (down && !moved && canEdit(t)) api.go(t.i);   // a click: the page on the screen
      down = false;
    };
    t.cv.addEventListener('pointerup', end);
    t.cv.addEventListener('pointercancel', () => { down = false; });
  }

  // ---- drawing: RGB565 -> RGBA, only the tiles whose key changed ----
  function paint(t) {
    const ptr = api.tile(t.i);
    if (!ptr) return;
    const px = M.HEAPU16.subarray(ptr >> 1, (ptr >> 1) + TILE_W * TILE_H);   // (read fresh: memory can grow)
    const d = t.img.data;
    for (let k = 0, o = 0; k < px.length; k++, o += 4) {
      const v = px[k];
      d[o]     = ((v >> 11) & 31) * 255 / 31;
      d[o + 1] = ((v >> 5) & 63) * 255 / 63;
      d[o + 2] = (v & 31) * 255 / 31;
      d[o + 3] = 255;
    }
    t.ctx.putImageData(t.img, 0, 0);
  }
  function tick() {
    if (document.hidden) return;                 // nothing to see: nothing computed
    const live = api.live();
    host.classList.toggle('readonly', !editable());
    for (const t of tiles) {
      const k = api.key(t.i);
      if (k !== t.key) { t.key = k; paint(t); }
      t.el.classList.toggle('live', t.i === live);
      t.el.classList.toggle('locked', !pageEditable(t.title, t.btn));
    }
  }

  if (sizeInput) {
    const size = () => {
      host.style.setProperty('--tile-w', sizeInput.value + 'px');
      host.classList.toggle('smooth', +sizeInput.value < TILE_W);
    };
    sizeInput.addEventListener('input', size);
    size();
  }
  tick();
  const timer = setInterval(tick, REFRESH_MS);
  return { tick, stop: () => clearInterval(timer), count: tiles.length };
}
