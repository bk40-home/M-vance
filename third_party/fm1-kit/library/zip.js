/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-kit library: zip files, read and written, with no library. Read: stored and deflated entries (deflate through
 * the browser's DecompressionStream, or Node's zlib in the tests). Written: stored (patch files are small). */
(function (G) {
  "use strict";
  const FK = G.FK = G.FK || {};

  let CRC = null;
  function crc32(b) {
    if (!CRC) CRC = Array.from({length: 256}, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ c >>> 1 : c >>> 1; return c >>> 0; });
    let c = 0xFFFFFFFF;
    for (let i = 0; i < b.length; i++) c = CRC[(c ^ b[i]) & 255] ^ c >>> 8;
    return (c ^ 0xFFFFFFFF) >>> 0;
  }
  FK.crc32 = crc32;

  /* raw deflate (RFC 1951), synchronous: a patch file is a few KB, and a bank zip can hold tens of thousands */
  const LBASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
  const LEXT = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
  const DBASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
  const DEXT = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
  const ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];
  function huff(lengths, n) {
    const count = new Uint16Array(16), offs = new Uint16Array(16), sym = new Uint16Array(n);
    for (let i = 0; i < n; i++) count[lengths[i]]++;
    count[0] = 0;
    for (let i = 1; i < 16; i++) offs[i] = offs[i - 1] + count[i - 1];
    for (let i = 0; i < n; i++) if (lengths[i]) sym[offs[lengths[i]]++] = i;
    return {count, sym};
  }
  let FIXED = null;
  function inflateRaw(src, hint) {
    let out = new Uint8Array(Math.max(64, hint || src.length * 4)), op = 0, ip = 0, bb = 0, bc = 0;
    const bits = n => {
      while (bc < n) { if (ip >= src.length) throw new Error("deflate: data ends early"); bb |= src[ip++] << bc; bc += 8; }
      const v = bb & ((1 << n) - 1); bb >>>= n; bc -= n; return v;
    };
    const room = n => { if (op + n > out.length) { const o = new Uint8Array(Math.max(out.length * 2, op + n)); o.set(out); out = o; } };
    const decode = h => {
      let code = 0, first = 0, index = 0;
      for (let len = 1; len < 16; len++) {
        code |= bits(1);
        const c = h.count[len];
        if (code - c < first) return h.sym[index + (code - first)];
        index += c; first += c; first <<= 1; code <<= 1;
      }
      throw new Error("deflate: a bad code");
    };
    let last = 0;
    while (!last) {
      last = bits(1);
      const type = bits(2);
      if (type === 0) {
        bb = 0; bc = 0;
        const len = src[ip] | src[ip + 1] << 8;
        ip += 4;
        room(len); out.set(src.subarray(ip, ip + len), op); op += len; ip += len;
        continue;
      }
      let lit, dist;
      if (type === 1) {
        if (!FIXED) {
          const l = new Uint8Array(288);
          for (let i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
          FIXED = [huff(l, 288), huff(new Uint8Array(30).fill(5), 30)];
        }
        [lit, dist] = FIXED;
      } else if (type === 2) {
        const nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        const cl = new Uint8Array(19);
        for (let i = 0; i < ncode; i++) cl[ORDER[i]] = bits(3);
        const ch = huff(cl, 19), lens = new Uint8Array(nlen + ndist);
        for (let i = 0; i < nlen + ndist;) {
          const sym = decode(ch);
          if (sym < 16) lens[i++] = sym;
          else {
            let rep, v = 0;
            if (sym === 16) { if (!i) throw new Error("deflate: a bad repeat"); v = lens[i - 1]; rep = 3 + bits(2); }
            else if (sym === 17) rep = 3 + bits(3);
            else rep = 11 + bits(7);
            while (rep--) lens[i++] = v;
          }
        }
        lit = huff(lens.subarray(0, nlen), nlen);
        dist = huff(lens.subarray(nlen), ndist);
      } else throw new Error("deflate: a bad block");
      for (;;) {
        const sym = decode(lit);
        if (sym < 256) { room(1); out[op++] = sym; }
        else if (sym === 256) break;
        else {
          const k = sym - 257, len = LBASE[k] + bits(LEXT[k]);
          const dk = decode(dist), d = DBASE[dk] + bits(DEXT[dk]);
          if (d > op) throw new Error("deflate: a bad distance");
          room(len);
          for (let j = 0; j < len; j++, op++) out[op] = out[op - d];
        }
      }
    }
    return out.subarray(0, op);
  }
  FK.inflateRaw = inflateRaw;

  /* entries: [{name, data: Uint8Array}], folders left out. Reads the central directory (ZIP64 too: a bank collection
   * can hold more than 65,535 files). onProgress(done, total) now and then; the work yields to the page between */
  FK.unzip = async function (buf, onProgress) {
    const b = new Uint8Array(buf), dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    const u64 = o => dv.getUint32(o, true) + dv.getUint32(o + 4, true) * 4294967296;
    let e = b.length - 22;
    while (e >= 0 && dv.getUint32(e, true) !== 0x06054b50) e--;
    if (e < 0) throw new Error("not a zip file");
    let n = dv.getUint16(e + 10, true), p = dv.getUint32(e + 16, true);
    if ((n === 0xFFFF || p === 0xFFFFFFFF) && e >= 20 && dv.getUint32(e - 20, true) === 0x07064b50) {
      const z = u64(e - 20 + 8);
      if (dv.getUint32(z, true) !== 0x06064b50) throw new Error("a broken ZIP64 directory");
      n = u64(z + 32); p = u64(z + 48);
    }
    const out = [];
    const dec = new TextDecoder("utf-8");
    for (let k = 0; k < n; k++) {
      if (dv.getUint32(p, true) !== 0x02014b50) throw new Error("a broken zip directory");
      const method = dv.getUint16(p + 10, true);
      let csize = dv.getUint32(p + 20, true), usize = dv.getUint32(p + 24, true);
      const nlen = dv.getUint16(p + 28, true), xlen = dv.getUint16(p + 30, true), clen = dv.getUint16(p + 32, true);
      let lho = dv.getUint32(p + 42, true);
      if (usize === 0xFFFFFFFF || csize === 0xFFFFFFFF || lho === 0xFFFFFFFF) {   /* the ZIP64 extra field */
        let x = p + 46 + nlen;
        const xe = x + xlen;
        while (x + 4 <= xe) {
          const id = dv.getUint16(x, true), sz = dv.getUint16(x + 2, true);
          if (id === 1) {
            let q = x + 4;
            if (usize === 0xFFFFFFFF) { usize = u64(q); q += 8; }
            if (csize === 0xFFFFFFFF) { csize = u64(q); q += 8; }
            if (lho === 0xFFFFFFFF) lho = u64(q);
            break;
          }
          x += 4 + sz;
        }
      }
      const name = dec.decode(b.subarray(p + 46, p + 46 + nlen));
      p += 46 + nlen + xlen + clen;
      if (onProgress && k % 2000 === 1999) { onProgress(k + 1, n); await new Promise(r => setTimeout(r, 0)); }
      if (name.endsWith("/") || name.startsWith("__MACOSX/")) continue;
      const lnl = dv.getUint16(lho + 26, true), lxl = dv.getUint16(lho + 28, true);
      const raw = b.subarray(lho + 30 + lnl + lxl, lho + 30 + lnl + lxl + csize);
      if (method === 0) out.push({name, data: raw.slice()});
      else if (method === 8) out.push({name, data: G.FK_INFLATE ? G.FK_INFLATE(raw) : inflateRaw(raw, usize)});
      /* (other methods: skipped) */
    }
    return out;
  };

  /* files: [{name, data: Uint8Array | string}] -> Uint8Array of a stored zip */
  FK.zip = function (files) {
    const enc = new TextEncoder(), parts = [], dir = [];
    let off = 0;
    const u16 = v => [v & 255, v >> 8 & 255], u32 = v => [v & 255, v >> 8 & 255, v >> 16 & 255, v >>> 24 & 255];
    for (const f of files) {
      const name = enc.encode(f.name), data = typeof f.data === "string" ? enc.encode(f.data) : f.data, c = crc32(data);
      const head = [0x50, 0x4B, 3, 4, ...u16(10), ...u16(0x800), 0, 0, 0, 0, 0x21, 0, ...u32(c), ...u32(data.length), ...u32(data.length), ...u16(name.length), 0, 0];
      parts.push(new Uint8Array(head), name, data);
      dir.push(new Uint8Array([0x50, 0x4B, 1, 2, ...u16(20), ...u16(10), ...u16(0x800), 0, 0, 0, 0, 0x21, 0, ...u32(c), ...u32(data.length), ...u32(data.length), ...u16(name.length), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ...u32(off)]), name);
      off += head.length + name.length + data.length;
    }
    const dlen = dir.reduce((a, x) => a + x.length, 0);
    const end = new Uint8Array([0x50, 0x4B, 5, 6, 0, 0, 0, 0, ...u16(files.length), ...u16(files.length), ...u32(dlen), ...u32(off), 0, 0]);
    const all = [...parts, ...dir, end], total = all.reduce((a, x) => a + x.length, 0), o = new Uint8Array(total);
    let q = 0;
    for (const x of all) { o.set(x, q); q += x.length; }
    return o;
  };
})(typeof window !== "undefined" ? window : globalThis);
