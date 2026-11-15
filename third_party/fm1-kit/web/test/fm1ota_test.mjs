// SPDX-License-Identifier: GPL-3.0-only
// fm1-kit: web/fm1ota.js against a pretend FM-1 (the one in the firmware repo's tests/install_test.py), and, given
// a firmware repo, against its tools/fm1_install.py on the same packages (identity, logical image, damage).
//   node web/test/fm1ota_test.mjs [FIRMWARE_REPO]       (the repo: for the Python cross-check and build/*.fwsc)
import * as O from "../fm1ota.js";
import {execFileSync} from "node:child_process";
import {existsSync, readFileSync, writeFileSync, mkdtempSync, readdirSync} from "node:fs";
import {tmpdir} from "node:os";
import {join} from "node:path";

Object.assign(O.DELAY, {open: 0, start: 0.01, reply: 0, loader: 0.01, reboot: 0.01, retry: 0.02, poll: 0.05, hs: 0.1,
  idle_check: 0.4, idle_write: 0.4, wait_loader: 2, wait_reboot: 2});
let failed = 0;
const ok = (c, what) => { console.log(what.padEnd(66) + (c ? " ok" : " FAIL")); failed += !c; };
const enc = (s) => new TextEncoder().encode(s);
const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
const sysex = (b) => Uint8Array.from([0xF0, ...O.pack7(b), 0xF7]);

// ---------------------------------------------------------- pretend FM-1 ---
class FakeFM1 {
  constructor(image, {identity = "FM-1_015", name = "FM-1", unplugAfter = null, afterWrite = "FM-1_900", stallAfter = null,
    badAddr = null, loader = ["ota-FM-1_900", "Felucca Update"]} = {}) {
    Object.assign(this, {image, unplugAfter, afterWrite, stallAfter, badAddr, loader});
    this.served = this.bad = this.upgrades = 0; this.sent = []; this.links = []; this.gen = 0;
    this.boot(identity, name);
  }
  boot(identity, name) { this.gen++; Object.assign(this, {identity, name, connected: true, waiting: null, queue: []}); }
  inputNames() { return this.connected ? [this.name] : []; }
  outputNames() { return this.inputNames(); }
  async open(inName) {
    if (inName !== this.name || !this.connected) throw new Error("no such port");
    const dev = this, gen = this.gen, q = [], waiters = [];
    const link = {name: inName, closed: false, q, gen,
      put(p) { if (waiters.length) waiters.shift()(p); else q.push(p); },
      get lost() { return gen !== dev.gen || !dev.connected; },
      send(p) { if (this.lost) return false; dev.sent.push(p); dev.rx(p); return true; },
      read(s) { return q.length ? Promise.resolve(q.shift()) : new Promise((res) => {
        const w = (p) => { clearTimeout(t); res(p); };
        const t = setTimeout(() => { waiters.splice(waiters.indexOf(w), 1); res(null); }, s * 1000);
        waiters.push(w); }); },
      drain() { q.length = 0; },
      close() { this.closed = true; }};
    this.links.push(link);
    return link;
  }
  tx(p) { for (const l of this.links) if (l.gen === this.gen && !l.closed) setTimeout(() => l.put(p), 0); }
  rx(d) {
    if (eq(d, O.HS_QUERY)) {
      const t = enc(this.identity), b = new Uint8Array(34); b.set([0, 0x59, 0x11]); b.set(t, 6);
      this.tx(sysex(b));
    } else if (eq(d, O.UPGRADE)) {
      this.upgrades++;
      const first = this.badAddr ?? 0;
      this.queue = this.identity.startsWith("ota-")
        ? [...Array(6).keys()].map((k) => [first + k * 512, 512]).concat([[O.FINISH_WRITE, 8]])
        : [[0, 64], [0x40, 160], [0x1000, 512], [O.FINISH_CHECK, 8]];
      this.next();
    } else if (this.waiting) {
      const u = O.unpack7(d.subarray(1, -1)), [addr, n] = this.waiting;
      const want = addr >= O.FINISH_CHECK ? enc("success\0") : this.image.subarray(addr, addr + n);
      if (u.length !== 15 + want.length || !eq(u.subarray(14, 14 + want.length), want)) this.bad++;
      this.waiting = null; this.served++;
      if (this.unplugAfter && this.served >= this.unplugAfter) { this.connected = false; return; }
      if (this.stallAfter && this.served >= this.stallAfter) return;
      if (addr === O.FINISH_CHECK) setTimeout(() => this.boot(...this.loader), 50);
      else if (addr === O.FINISH_WRITE) setTimeout(() => this.boot(this.afterWrite, "Felucca"), 50);
      else this.next();
    }
  }
  next() {
    if (!this.queue.length) return;
    const [addr, n] = this.waiting = this.queue.shift();
    const u = new Uint8Array(15);
    u.set([0, 0x59, 0x30]);
    for (let i = 0; i < 4; i++) u[7 + i] = Math.floor(addr / 2 ** (8 * i)) & 0xFF;
    u.set([n & 0xFF, n >> 8, 0], 11);
    let s = 0; for (let i = 6; i < 14; i++) s += u[i]; u[14] = ~s & 0xFF;
    this.tx(sysex(u));
  }
}

// a package with a valid header and file list (one file: 0x400 to the end), as install_test.py's package()
function pkg({product = "FM-1_900", marker = true, size = 0x2000, damage = false, blocks = 20, seed = 7} = {}) {
  const img = new Uint8Array(size).map((_, i) => (i * seed) & 0xFF);
  img.set(marker ? enc("FELUCCA-LOADER-1") : new Uint8Array(16), 0x1800);
  const data = img.subarray(0x400), e = new Uint8Array(0x50), w = (b, o, v, n) => { for (let i = 0; i < n; i++) b[o + i] = (v >>> (8 * i)) & 0xFF; };
  w(e, 4, O.crc16(data), 2); w(e, 8, 0x400, 4); w(e, 12, data.length, 4); e.set(enc("flash.bin"), 0x40);
  const lst = O.dec(e), hdr = new Uint8Array(0x40);
  w(hdr, 4, size, 4); w(hdr, 8, 1, 2); w(hdr, 2, O.crc16(lst), 2); w(hdr, 0, O.crc16(hdr.subarray(2)), 2);
  img.set(O.dec(hdr), 0); img.set(lst, 0x40);
  if (damage) img[0x1000] ^= 1;
  const out = [];
  for (let i = 0; i < blocks; i++) {
    out.push(...img.subarray(i * O.KEEP, (i + 1) * O.KEEP), i < product.length ? (product.charCodeAt(i) + i + 1) & 0xFF : 0x7D);
  }
  out.push(...img.subarray(blocks * O.KEEP));
  return Uint8Array.from(out);
}

async function run(dev, raw, opts = {}) {
  const steps = [];
  try {
    const p = O.loadPackage(raw, opts.force);
    const up = new O.Updater(dev, opts.port);
    const d = await up.find();
    if (!d) return {code: "notfound", steps};
    return {done: await up.install(d, p.image, p.product, (k, a) => steps.push(k + (a !== undefined ? " " + a : ""))), steps};
  } catch (e) { if (!(e instanceof O.InstallError)) throw e; return {code: e.code, msg: e.message, steps}; }
}

// ------------------------------------------------------------------- tests ---
{
  const data = Uint8Array.from({length: 768}, (_, i) => i & 0xFF);
  ok(eq(O.unpack7(O.pack7(data)).subarray(0, data.length), data), "pack7 / unpack7 round trip");
  const u = O.unpack7(O.response(0x12345, Uint8Array.of(1, 2, 3), 5).subarray(1, -1));
  let s = 0; for (let i = 6; i < 17; i++) s += u[i];
  ok(u[1] === 0x59 && u[2] === 0x30 && u[7] === 0x45 && u[8] === 0x23 && u[9] === 1 && eq(u.subarray(14, 17), [1, 2, 3]) && (~s & 0xFF) === u[17],
    "response: header, address, data, checksum");
  const fin = O.unpack7(O.response(O.FINISH_WRITE, enc("success\0")).subarray(1, -1));
  ok(fin[10] === 0xF0 && fin[7] === 0, "response: the 0xF0000000 address (no sign trouble)");
  const raw = pkg({product: "FM-1_906"});
  ok(O.productOf(raw) === "FM-1_906" && O.logicalImage(raw).length === raw.length - 20, "productOf / logicalImage (synthetic)");
  ok(O.PORT_RE.test("SMK-37 Pro Midi") && O.PORT_RE.test("FM-1") && !O.PORT_RE.test("IAC Driver Bus 1")
    && O.OWN_LOADER_RE.test("ota-FM-1_925") && !O.OWN_LOADER_RE.test("ota-FM-1_015") && O.sameKeyboard("SMK-37 Elite_900", "SMK-37 Pro_016")
    && !O.sameKeyboard("FM-1_015", "SMK-37 Pro_900"), "ports, own loader, same keyboard");
  ok(O.pairOutput("FM-1 3", ["FM-1 4", "Other"]) === "FM-1 4", "pairOutput: Windows' numbered ports");
}
{
  const raw = pkg(), image = O.logicalImage(raw);
  let dev = new FakeFM1(image), r = await run(dev, raw);
  ok(r.done === "FM-1_900" && dev.bad === 0 && dev.upgrades === 2 && dev.served === 11 && r.steps.includes("write 100"),
    `install: running -> loader -> new firmware (${dev.served} reads)`);
  dev = new FakeFM1(image, {identity: "ota-FM-1_900", name: "Felucca Update"}); r = await run(dev, raw);
  ok(r.done === "FM-1_900" && dev.upgrades === 1, "install: already in update mode -> finishes the write");
  r = await run(new FakeFM1(image, {name: "IAC Driver Bus 1"}), raw);
  ok(r.code === "notfound", "no FM-1 port -> notfound");
  dev = new FakeFM1(image, {unplugAfter: 2}); r = await run(dev, raw);
  ok(r.code === "lost" && /nothing was written/.test(r.msg), "unplugged in step 1 -> lost, nothing written");
  r = await run(new FakeFM1(image, {identity: "ota-FM-1_900", name: "Felucca Update", unplugAfter: 3}), raw);
  ok(r.code === "lost" && /install again/.test(r.msg), "unplugged during the write -> lost, run it again");
  r = await run(new FakeFM1(image, {identity: "ota-FM-1_900", name: "Felucca Update", stallAfter: 3}), raw);
  ok(r.code === "stopped", "loader stops answering -> stopped");
  r = await run(new FakeFM1(image, {identity: "ota-FM-1_900", name: "Felucca Update", badAddr: image.length}), raw);
  ok(r.code === "badreq", "read past the package -> badreq");
  r = await run(new FakeFM1(image, {afterWrite: "FM-1_015"}), raw);
  ok(r.code === "mismatch", "another identity after the restart -> mismatch");
  dev = new FakeFM1(image, {identity: "XY-9_001", name: "usb-midi"}); r = await run(dev, raw);
  ok(r.code === "model" && dev.upgrades === 0, "another model -> model, nothing sent");
  dev = new FakeFM1(image, {identity: "ota-FM-1_015", name: "FM-1 Update"}); r = await run(dev, raw);
  ok(r.code === "foreign" && dev.upgrades === 0, "another firmware's update loader -> foreign, nothing sent");
  class Stuck extends FakeFM1 { boot(i, n) { super.boot(i, n); if (i.startsWith("ota-")) this.connected = false; } }
  r = await run(new Stuck(image), raw);
  ok(r.code === "noloader", "no loader after step 1 -> noloader");
  dev = new FakeFM1(image); r = await run(dev, pkg({marker: false}));
  ok(r.code === "badpkg" && dev.sent.length === 0, "package without the loader marker -> badpkg, no MIDI");
  dev = new FakeFM1(image); r = await run(dev, pkg({damage: true}));
  ok(r.code === "badpkg" && /damaged/.test(r.msg) && dev.sent.length === 0, "damaged package -> badpkg, no MIDI");
  r = await run(new FakeFM1(image), new Uint8Array(100));
  ok(r.code === "badpkg", "not a package -> badpkg");
  const smk = pkg({product: "SMK-37 Pro_900"});
  dev = new FakeFM1(O.logicalImage(smk), {identity: "SMK-37 Pro_016", name: "SMK-37 Pro Midi", afterWrite: "SMK-37 Pro_900",
    loader: ["ota-SMK-37 Pro_900", "Felucca Update"]}); r = await run(dev, smk);
  ok(r.done === "SMK-37 Pro_900" && dev.bad === 0, "install on an SMK-37 Pro");
  dev = new FakeFM1(image); r = await run(dev, smk);
  ok(r.code === "model" && dev.upgrades === 0, "SMK package on an FM-1 -> model, nothing sent");
  const raw36 = pkg({product: "SMK-37 Pro_900", blocks: 36, size: 0x3000}), [img36, n36] = O.deinterleave(raw36);
  ok(n36 === 36 && !O.packageDamage(img36) && O.productOf(raw36) === "SMK-37 Pro_900", "36 blocks (synthetic): detected");
}

// -------------------------------------------- the same answers as fm1_install.py ---
const repo = process.argv[2];
if (!repo || !existsSync(join(repo, "tools/fm1_install.py"))) {
  console.log("fm1_install.py cross-check: skipped (give the firmware repo)");
} else {
  const tmp = mkdtempSync(join(tmpdir(), "fm1ota-")), files = [];
  [["ok", pkg()], ["smk36", pkg({product: "SMK-37 Pro_900", blocks: 36, size: 0x3000})], ["damaged", pkg({damage: true})],
    ["seed", pkg({seed: 3})]].forEach(([n, b]) => { const p = join(tmp, n + ".fwsc"); writeFileSync(p, b); files.push(p); });
  const bdir = join(repo, "build");
  if (existsSync(bdir)) readdirSync(bdir).filter((f) => f.endsWith(".fwsc")).forEach((f) => files.push(join(bdir, f)));
  const py = `import sys, hashlib, json; sys.path.insert(0, sys.argv[1] + "/tools"); import fm1_install as I
out = []
for f in sys.argv[2:]:
    raw = open(f, "rb").read(); img, n = I.deinterleave(raw)
    out.append([I.product_of(raw), n, hashlib.sha256(img).hexdigest(), I.package_damage(img)])
print(json.dumps(out))`;
  const want = JSON.parse(execFileSync("python3", ["-c", py, repo, ...files]).toString());
  const {createHash} = await import("node:crypto");
  files.forEach((f, i) => {
    const raw = new Uint8Array(readFileSync(f)), [img, n] = O.deinterleave(raw);
    const got = [O.productOf(raw), n, createHash("sha256").update(img).digest("hex"), O.packageDamage(img)];
    ok(JSON.stringify(got) === JSON.stringify(want[i]), `as fm1_install.py: ${f.split(/[\\/]/).pop()} (${got[0]}, ${n} blocks)`);
  });
}
console.log(failed ? `fm1ota: ${failed} FAILED` : "fm1ota: all passed");
process.exit(failed ? 1 : 0);
