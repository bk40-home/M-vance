// SPDX-License-Identifier: GPL-3.0-only
// fm1-kit: install a Felucca package (.fwsc) on an FM-1 (or an SMK-37 Pro) from a web page, over Web MIDI.
// A line-for-line port of the firmware repo's tools/fm1_install.py (Leo Kuroshita, Hügelton Instruments; Felucca),
// which says how and why; keep the two in step (test/fm1ota_test.mjs checks them against each other).
//
//   step 1: the running firmware reads parts of the package, checks it and starts its update loader;
//   step 2: the loader reads the whole package and writes it, then the FM-1 restarts and its identity is checked.
// In both steps the device asks (SysEx read requests on the logical image) and the page answers.
// The device's own update loader stays until the write is done: a cut-off install is finished by running it again.
//
//   import * as OTA from "./fm1ota.js";
//   const pkg = OTA.loadPackage(bytes);                         // {product, image, blocks} or throws InstallError
//   const up = new OTA.Updater(new OTA.WebMidiBackend(access));
//   const dev = await up.find();                                // {link, id: {text, model, version, loader}, name}
//   await up.install(dev, pkg.image, pkg.product, (kind, a) => ...);   // start/check/loader/write %/reboot/done
// An ES module with no imports: works from any host, offline, and in Node (the tests).

export const HS_QUERY = Uint8Array.of(0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7);
export const UPGRADE = Uint8Array.of(0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7);
export const FINISH_CHECK = 0xE0000000, FINISH_WRITE = 0xF0000000;
export const MAXDATA = 512;
export const BLOCKS = 20, BLK = 0x30, KEEP = 0x2F, MAX_BLOCKS = 64;
const LOADER_MARK = new TextEncoder().encode("FELUCCA-LOADER-1");
export const PORT_RE = /fm-1|smk|elite|mke-p37|starry|felucca|sloop|clipper|melodee|fomni|x0x|makkuro|maku|rainbow|snoop|vector|vox|ota|composite|sinco|usb-midi/i;
export const OWN_LOADER_RE = /^ota-(FM-1|SMK-37 Pro|SMK-37 Elite|MKE-P37)_9\d\d$/i;
const SMK_FAMILY = /^(SMK-37[ +].*|MKE-P37|STARRYKEY-37.*)$/i;

// seconds, as fm1_install.py; the tests shorten them
export const DELAY = {open: 0.3, start: 2.0, reply: 0.01, loader: 3.0, reboot: 3.0, retry: 1.0,
  poll: 1.0, hs: 1.0, idle_check: 8.0, idle_write: 180.0, wait_loader: 30.0, wait_reboot: 40.0};

export class InstallError extends Error {
  constructor(code, msg) { super(msg); this.code = code; }   // usage badpkg notfound model lost stopped badreq noloader noreturn mismatch foreign
}

const sleep = (s) => new Promise((r) => setTimeout(r, s * 1000));
const now = () => (typeof performance !== "undefined" ? performance.now() : Date.now()) / 1000;
const le = (b, o, n) => { let v = 0; for (let i = n - 1; i >= 0; i--) v = v * 256 + b[o + i]; return v; };
const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const sum = (b, from, to) => { let s = 0; for (let i = from; i < to; i++) s += b[i]; return s; };

// ------------------------------------------------------------ wire format ---

export function pack7(data) {
  const out = []; let acc = 0, nb = 0;
  for (const b of data) {
    acc |= b << nb; nb += 8;
    while (nb >= 7) { out.push(acc & 0x7F); acc >>>= 7; nb -= 7; }
  }
  if (nb) out.push(acc & 0x7F);
  return Uint8Array.from(out);
}

export function unpack7(s) {
  const out = []; let acc = 0, nb = 0;
  for (const b of s) {
    acc |= (b & 0x7F) << nb; nb += 7;
    while (nb >= 8) { out.push(acc & 0xFF); acc >>>= 8; nb -= 8; }
  }
  return Uint8Array.from(out);
}

const sysex = (body) => { const p = pack7(body), o = new Uint8Array(p.length + 2); o[0] = 0xF0; o.set(p, 1); o[o.length - 1] = 0xF7; return o; };

export function response(addr, data, fl = 0) {
  const n = data.length, body = new Uint8Array(15 + n);
  body.set([0x00, 0x59, 0x30, (n + 8) & 0xFF, ((n + 8) >> 8) & 0xFF, 0, fl]);
  for (let i = 0; i < 4; i++) body[7 + i] = Math.floor(addr / 2 ** (8 * i)) & 0xFF;
  body.set([n & 0xFF, (n >> 8) & 0xFF, 0], 11);
  body.set(data, 14);
  body[14 + n] = ~sum(body, 6, 14 + n) & 0xFF;
  return sysex(body);
}

/** -> [flags, addr, len] or null */
export function parseRequest(pkt) {
  if (pkt.length < 2 || pkt[0] !== 0xF0 || pkt[pkt.length - 1] !== 0xF7) return null;
  const u = unpack7(pkt.subarray(1, -1));
  if (u.length !== 15 || u[0] !== 0 || u[1] !== 0x59 || u[2] !== 0x30 || (~sum(u, 6, 14) & 0xFF) !== u[14]) return null;
  return [u[6], le(u, 7, 4), le(u, 11, 3)];
}

export function parseIdentity(pkt) {
  if (pkt.length < 2 || pkt[0] !== 0xF0 || pkt[pkt.length - 1] !== 0xF7) return null;
  const d = unpack7(pkt.subarray(1, -1));
  if (d.length !== 34 || d[0] !== 0 || d[1] !== 0x59 || d[2] !== 0x11) return null;
  let txt = ""; for (let i = 6; i < 33 && d[i]; i++) txt += String.fromCharCode(d[i]);
  const m = /^([^_]+)_(\d+)$/.exec(txt);
  return m ? identity(txt, m[1], +m[2]) : null;
}
const identity = (text, model, version) => ({text, model, version, loader: model.toLowerCase().startsWith("ota-")});

// --------------------------------------------------------------- package ---

const idChar = (m, i) => { const c = (m - i - 1) & 0xFF; return c >= 0x20 && c < 0x7F; };
const marker = (raw, k) => ((k + 1) * BLK <= raw.length ? raw[k * BLK + KEEP] : null);

export function identityBlocks(raw) {
  let n = BLOCKS, m;
  while (n < MAX_BLOCKS && (m = marker(raw, n)) !== null && m !== 0x7D && idChar(m, n)) n++;
  return n > BLOCKS && marker(raw, n) === 0x7D ? n : BLOCKS;
}

export function productOf(raw) {
  if (raw.length < BLOCKS * BLK) throw new InstallError("badpkg", "not an FM-1 package (too short)");
  const ib = identityBlocks(raw);
  const n = ib === BLOCKS ? BLOCKS : Math.min(ib, deinterleave(raw)[1]);
  let s = "";
  for (let i = 0; i < n; i++) { const m = raw[i * BLK + KEEP]; if (m !== 0x7D) s += String.fromCharCode((m - i - 1) & 0xFF); }
  return s;
}

export function blockCount(raw) {
  let n = identityBlocks(raw);
  while (n < MAX_BLOCKS && (n + 1) * BLK <= raw.length && raw[n * BLK + KEEP] === 0x7D) n++;
  return n;
}

export function logicalImage(raw, blocks = BLOCKS) {
  const tail = raw.subarray(Math.min(blocks * BLK, raw.length));
  const out = new Uint8Array(blocks * KEEP + tail.length);
  let o = 0;
  for (let i = 0; i < blocks; i++) { const part = raw.subarray(i * BLK, i * BLK + KEEP); out.set(part, o); o += part.length; }
  out.set(tail, o);
  return out.subarray(0, o + tail.length);
}

/** -> [logical image, block count]: the detected count when the files' CRCs agree, else 20 */
export function deinterleave(raw) {
  const n = blockCount(raw), image = logicalImage(raw, n);
  if (n !== BLOCKS && packageDamage(image)) {
    const alt = logicalImage(raw, BLOCKS);
    if (!packageDamage(alt)) return [alt, BLOCKS];
  }
  return [image, n];
}

export function modelOf(text) { const m = text.split("_")[0]; return m.startsWith("ota-") ? m.slice(4) : m; }

export function sameKeyboard(a, b) {
  const ma = modelOf(a), mb = modelOf(b);
  return ma === mb || (SMK_FAMILY.test(ma) && SMK_FAMILY.test(mb));
}

export function crc16(data, crc = 0) {          // CRC-16/XMODEM, as the package and the device use
  for (const b of data) {
    crc ^= b << 8;
    for (let k = 0; k < 8; k++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
  }
  return crc;
}

export function dec(b) {                         // JieLi ENC stream cipher (its own inverse)
  const out = Uint8Array.from(b); let key = 0xFFFF;
  for (let i = 0; i < out.length; i++) { out[i] ^= key & 0xFF; key = ((key << 1) ^ (key & 0x8000 ? 0x1021 : 0)) & 0xFFFF; }
  return out;
}

/** what is wrong with the update image (header, file list, each installed file's CRC), or null */
export function packageDamage(image) {
  if (image.length < 0x40) return "too short";
  const hdr = dec(image.subarray(0, 0x40));
  if (crc16(hdr.subarray(2, 0x40)) !== le(hdr, 0, 2)) return "header CRC";
  const count = le(hdr, 8, 2);
  if (!(count >= 1 && count <= 8) || image.length < 0x40 + 0x50 * count) return "file list";
  const lst = image.subarray(0x40, 0x40 + 0x50 * count);
  if (crc16(lst) !== le(hdr, 2, 2)) return "file list CRC";
  for (let i = 0; i < count; i++) {
    const e = dec(lst.subarray(i * 0x50, (i + 1) * 0x50));
    const dcrc = le(e, 4, 2), off = le(e, 8, 4), size = le(e, 12, 4), typ = le(e, 0, 2);
    let name = ""; for (let k = 0x40; k < 0x50 && e[k]; k++) name += String.fromCharCode(e[k]);
    if (off + size > image.length) return name + ": cut short";
    if ((typ === 0 || typ === 100) && crc16(image.subarray(off, off + size)) !== dcrc) return name + ": CRC";
  }
  return null;
}

function contains(hay, needle) {
  outer: for (let i = 0; i + needle.length <= hay.length; i++) {
    for (let k = 0; k < needle.length; k++) if (hay[i + k] !== needle[k]) continue outer;
    return true;
  }
  return false;
}

/** a .fwsc's bytes -> {product, image, blocks}; throws InstallError("badpkg") as fm1_install.load_package */
export function loadPackage(raw, force = false) {
  raw = raw instanceof Uint8Array ? raw : new Uint8Array(raw);
  const product = productOf(raw);
  if (!/^[^_]+_\d+$/.test(product))
    throw new InstallError("badpkg", /^[\x20-\x7E]*$/.test(product) ? `not an FM-1 package (identity "${product}")` : "not an FM-1 package (.fwsc)");
  if (!force && !contains(raw, LOADER_MARK))
    throw new InstallError("badpkg", "no Felucca update loader in this package; only Felucca's own packages are installed");
  const [image, blocks] = deinterleave(raw);
  const bad = packageDamage(image);
  if (bad) throw new InstallError("badpkg", `damaged package (${bad}); nothing was written. Download it again.`);
  return {product, image, blocks};
}

export async function sha256hex(bytes) {
  if (!globalThis.crypto || !crypto.subtle) throw new Error("this page needs https (or localhost)");
  const h = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return Array.from(h, (b) => b.toString(16).padStart(2, "0")).join("");
}

// ------------------------------------------------------------------ MIDI ---
// A backend: inputNames(), outputNames(), open(inName, outName) -> link.
// A link: name, send(pkt) -> bool, read(seconds) -> Promise<pkt|null>, drain(), lost (bool), close().

class Inbox {
  constructor() { this.q = []; this.waiter = null; }
  put(p) { if (this.waiter) { const w = this.waiter; this.waiter = null; w(p); } else this.q.push(p); }
  read(secs) {
    if (this.q.length) return Promise.resolve(this.q.shift());
    return new Promise((res) => {
      const t = setTimeout(() => { this.waiter = null; res(null); }, Math.max(secs, 0) * 1000);
      this.waiter = (p) => { clearTimeout(t); res(p); };
    });
  }
  drain() { this.q.length = 0; }
}

export class WebMidiBackend {
  constructor(access) { this.access = access; }
  ports(kind) { return Array.from(this.access[kind].values()).filter((p) => p.state !== "disconnected"); }
  inputNames() { return [...new Set(this.ports("inputs").map((p) => p.name))]; }
  outputNames() { return [...new Set(this.ports("outputs").map((p) => p.name))]; }
  async open(inName, outName) {
    const inp = this.ports("inputs").find((p) => p.name === inName), out = this.ports("outputs").find((p) => p.name === outName);
    if (!inp || !out) throw new Error("no such port");
    await inp.open(); await out.open();             // (Windows: fails while another app holds the port)
    const box = new Inbox(), backend = this;
    inp.onmidimessage = (e) => { if (e.data[0] === 0xF0) box.put(Uint8Array.from(e.data)); };
    await sleep(DELAY.open);
    return {
      name: inName,
      send(pkt) { try { out.send(pkt); return true; } catch (e) { return false; } },
      read: (secs) => box.read(secs),
      drain: () => box.drain(),
      get lost() { return inp.state === "disconnected" || !backend.inputNames().includes(inName); },
      close() { inp.onmidimessage = null; try { inp.close(); out.close(); } catch (e) { /* gone */ } },
    };
  }
}

export function pairOutput(name, outs) {
  if (outs.includes(name)) return name;
  const base = (s) => s.replace(/\s+\d+$/, "");     // Windows numbers each port
  const same = outs.filter((o) => base(o) === base(name));
  if (same.length === 1) return same[0];
  return outs.length === 1 ? outs[0] : null;
}

// --------------------------------------------------------------- updater ---

export async function handshake(link, tries = 3) {
  link.drain();
  for (let t = 0; t < tries; t++) {
    if (!link.send(HS_QUERY)) return null;
    const end = now() + DELAY.hs;
    let left;
    while ((left = end - now()) > 0) {
      const p = await link.read(left);
      if (p === null) break;
      const id = parseIdentity(p);
      if (id) return id;
    }
  }
  return null;
}

export class Updater {
  constructor(backend, port = null) { this.backend = backend; this.port = port; }

  candidates(onlyPort) {
    const names = this.backend.inputNames();
    if (this.port) {
      const mine = names.filter((n) => n.toLowerCase().includes(this.port.toLowerCase()));
      return onlyPort ? mine : mine.concat(names.filter((n) => !mine.includes(n) && PORT_RE.test(n)));
    }
    return names.filter((n) => PORT_RE.test(n));
  }

  /** the first matching port that answers the handshake (and want(identity)); -> {link, id, name} or null */
  async find(want = null, onlyPort = true) {
    const outs = this.backend.outputNames();
    for (const name of this.candidates(onlyPort)) {
      const out = pairOutput(name, outs);
      if (!out) continue;
      let link;
      try { link = await this.backend.open(name, out); } catch (e) { continue; }
      const id = await handshake(link, 2);
      if (id && (!want || want(id))) return {link, id, name};
      link.close();
    }
    return null;
  }

  async waitFor(want, secs) {
    const end = now() + secs;
    while (now() < end) {
      const dev = await this.find(want, false);
      if (dev) return dev;
      await sleep(DELAY.retry);
    }
    return null;
  }

  /** answer read requests until the device asks for `finish`; -> [served, finished, lost] */
  async serve(link, image, finish, idle, progress) {
    let served = 0, last = now();
    for (;;) {
      const pkt = await link.read(DELAY.poll);
      if (pkt === null) {
        if (link.lost) return [served, false, true];
        if (now() - last > idle) return [served, false, false];
        continue;
      }
      const r = parseRequest(pkt);
      if (!r) continue;
      const [fl, addr, n] = r;
      last = now();
      if (addr === FINISH_CHECK || addr === FINISH_WRITE) {
        link.send(response(addr, new TextEncoder().encode("success\0")));
        if (addr === finish) return [served, true, false];
        continue;
      }
      if (n > MAXDATA || addr + n > image.length)
        throw new InstallError("badreq", `the device asked for 0x${addr.toString(16)}+${n}, outside the package`);
      await sleep(DELAY.reply);
      if (!link.send(response(addr, image.subarray(addr, addr + n), fl))) return [served, false, true];
      served++;
      progress(served, addr + n);
    }
  }

  async check(dev, image, step) {                 // step 1
    step("start", dev.id.text);
    dev.link.send(UPGRADE);
    await sleep(DELAY.start);
    const [served, finished, lost] = await this.serve(dev.link, image, FINISH_CHECK, DELAY.idle_check, (k) => step("check", k));
    dev.link.close();
    if (!finished)
      throw new InstallError(lost ? "lost" : "stopped",
        `the FM-1 ${lost ? "was disconnected" : "stopped answering"} after ${served} requests; nothing was written`);
    step("loader");
    await sleep(DELAY.loader);
    const ota = await this.waitFor((i) => i.loader, DELAY.wait_loader);
    if (!ota)
      throw new InstallError("noloader", "the update loader did not appear. Replug the USB cable and run the install " +
        "again: the FM-1 stays in update mode until it is done");
    return ota;
  }

  async write(ota, image, step) {                 // step 2
    step("write", 0);
    ota.link.send(UPGRADE);
    await sleep(DELAY.start);
    const [served, finished, lost] = await this.serve(ota.link, image, FINISH_WRITE, DELAY.idle_write,
      (k, end) => step("write", Math.min(99, Math.floor(end * 100 / image.length))));
    ota.link.close();
    if (!finished)
      throw new InstallError(lost ? "lost" : "stopped",
        `the loader ${lost ? "was disconnected" : "stopped answering"} after ${served} requests. Replug the USB cable ` +
        "and run the install again to finish");
    step("write", 100);
  }

  async verify(product, step) {
    step("reboot");
    await sleep(DELAY.reboot);
    const back = await this.waitFor((i) => !i.loader, DELAY.wait_reboot);
    if (!back) throw new InstallError("noreturn", "the FM-1 did not come back: power-cycle it");
    back.link.close();
    if (back.id.text !== product) throw new InstallError("mismatch", `written, but the FM-1 reports ${back.id.text} (expected ${product})`);
    step("done", back.id.text);
    return back.id.text;
  }

  /** running firmware -> loader -> new firmware; dev from find(). A device already in update mode only needs the write. */
  async install(dev, image, product, step = () => {}) {
    if (!sameKeyboard(dev.id.text, product)) {
      dev.link.close();
      throw new InstallError("model", `the device is ${dev.id.text}, the package is for ${product}`);
    }
    if (dev.id.loader && !OWN_LOADER_RE.test(dev.id.text)) {
      dev.link.close();
      throw new InstallError("foreign", `the FM-1 is in the update mode of another firmware (${dev.id.text}): finish ` +
        "that update with its own updater, then install again");
    }
    const ota = dev.id.loader ? dev : await this.check(dev, image, step);
    await this.write(ota, image, step);
    return this.verify(product, step);
  }
}
