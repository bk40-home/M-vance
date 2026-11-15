/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home).
 * A pretend FM-1 running M-vance, for the library page's Sandbox and for the tests (tests/library_test.js). It answers
 * as firmware/src/mv_xfer.c does:
 *   F0 7E .. 06 01 F7        the identity reply (mv_xfer_ident): 7D "MV" "AN", version 0.0 here
 *   RQ1 03 pp, size 00 nn    nn programs from slot pp, 4 DT1s each (common 20 bytes, upper, lower, the 16 own bytes)
 *   RQ1 01 00                what plays (the temporary area)
 *   DT1 03 pp aa / 01 00 aa  into slot pp / what plays: xf_apply's rules (the name 32..125; KEY MODE > SPLIT = SINGLE;
 *                            a patch through the parser's clamps; the own bytes refused whole when out of range)
 * A bad checksum or another maker's message: ignored. Holds the factory bank to start; nothing is kept after the page
 * closes. */
(function (G) {
  "use strict";
  const FK = G.FK;
  FK.mockDevice = function (opts) {
    const o = opts || {}, B = FK.mvb, M = FK.mvance, listeners = new Set();
    const copy = s => ({up: Uint8Array.from(s.up), lo: Uint8Array.from(s.lo), pf: Uint8Array.from(s.pf)});
    const slots = FK.adapter.factory()[0].slots.map(copy);
    if (o.seed) o.seed(slots);
    const later = (fn, ms) => setTimeout(fn, ms === undefined ? 1 : ms);
    const emit = (bytes, ms) => later(() => listeners.forEach(f => f(Uint8Array.from(bytes))), ms);
    const rsum = a => { let s = 0; for (const b of a) s += b; return (128 - (s % 128)) % 128; };
    const dt1 = (addr, d) => [0xF0, 0x41, 0x10, 0x00, 0x06, 0x12, ...addr, ...d, rsum([...addr, ...d]), 0xF7];
    /* a program's 4 messages, as mv_xfer_next sends them */
    const dump = (s, a3, a2) => [
      dt1([a3, a2, 0x00, 0x00], [...s.up.slice(0, 16), s.pf[0], s.pf[1], 0, s.pf[2]]),
      dt1([a3, a2, 0x40, 0x00], s.up), dt1([a3, a2, 0x42, 0x00], s.lo), dt1([a3, a2, 0x70, 0x00], s.pf)];
    /* one area's bytes into program s (xf_apply): true when taken */
    function apply(s, area, off, d) {
      if (area === 0x00) {
        d.forEach((v, i) => { const a = off + i;
          if (a < 16) s.up[a] = v < 32 || v > 125 ? 32 : v; else if (a === 0x10) s.pf[0] = v > 2 ? 0 : v;
          else if (a === 0x11) s.pf[1] = v; else if (a === 0x13) s.pf[2] = v; });
        return true;
      }
      if (area === 0x70) {
        if (off + d.length > B.NPERF) return false;
        const t = Uint8Array.from(s.pf); t.set(d, off);
        if (t.some((v, i) => v > M.PF_MAX[i])) return false;
        s.pf = t;
        return true;
      }
      if (area >= 0x40 && area <= 0x43) {
        const t = Uint8Array.from(area < 0x42 ? s.up : s.lo), at = (area & 1) * 128 + off;
        t.set(d.slice(0, Math.max(0, B.PATCH - at)), at);
        if (area < 0x42) s.up = M.sanitize(t); else s.lo = M.sanitize(t);
        return true;
      }
      return false;
    }
    const dev = {
      slots, temp: copy(slots[0]), played: [], log: [],
      version: [0, 0, 0, 0],                    /* (the identity reply's version bytes) */
      send(bytes) {
        const m = Array.from(bytes);
        if (m[0] === 0xF0 && m[1] === 0x7E && m[3] === 0x06 && m[4] === 0x01 && m.length === 6) {   /* who are you */
          dev.log.push("ident");
          emit([0xF0, 0x7E, 0x10, 0x06, 0x02, 0x7D, 0x4D, 0x56, 0x41, 0x4E, ...dev.version, 0xF7]);
          return;
        }
        if (m.length < 12 || m[0] !== 0xF0 || m[1] !== 0x41 || m[3] !== 0x00 || m[4] !== 0x06) return;
        if (rsum(m.slice(6, m.length - 2)) !== m[m.length - 2]) return;   /* a bad sum: refused */
        const [a3, a2, a1, a0] = m.slice(6, 10);
        if (m[5] === 0x11 && m.length === 16) {                          /* RQ1 */
          if (a3 === 0x03 && a2 < 64) {
            const nn = Math.min(m[11] || 1, 64 - a2);
            dev.log.push("read " + a2 + " " + nn);
            let k = 0;
            for (let pp = a2; pp < a2 + nn; pp++) for (const x of dump(slots[pp], 0x03, pp)) emit(x, 1 + k++);
          } else if (a3 === 0x01 && a2 === 0x00) {
            let k = 0;
            for (const x of dump(dev.temp, 0x01, 0x00)) emit(x, 1 + k++);
          }
          return;
        }
        if (m[5] !== 0x12) return;
        const d = m.slice(10, m.length - 2);
        if (a3 === 0x01 && a2 === 0x00) {                                /* what plays */
          const t = copy(dev.temp);
          if (apply(t, a1, a0, d)) { dev.temp = t; if (a1 === 0x70) dev.played.push(copy(t)); }
        } else if (a3 === 0x03 && a2 < 64) {                             /* a slot */
          const t = copy(slots[a2]);
          if (apply(t, a1, a0, d)) { slots[a2] = t; dev.log.push("write " + a2 + " " + a1.toString(16)); }
        }
      },
      listen(f) { listeners.add(f); return () => listeners.delete(f); },
      close() { listeners.clear(); },
    };
    return dev;
  };
})(typeof window !== "undefined" ? window : globalThis);
