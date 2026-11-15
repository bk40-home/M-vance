// SPDX-License-Identifier: GPL-3.0-only
// fm1-kit: the firmware page: find the FM-1, pick the latest release (or a .fwsc from this computer), install it
// over Web MIDI (fm1ota.js). Chrome or Edge on a computer; no iPhone / iPad (no Web MIDI there).
//   import {mount} from "./flash.js";
//   mount(document.getElementById("flash"), {name: "M-Vsynth2", manifest: "firmware/latest.json", releases: "https://..."});
// The manifest (the Pages workflow writes it from the latest GitHub release; the packages sit beside it):
//   {name, version, tag, url, published, files: [{file, product, size, sha256}]}
// Counts (count.js, when the page has it): <name>/flash/start/<identity>, .../done/<identity>, .../fail/<code>
// (the firmware's name first: several firmwares' sites can share one counter).
import * as OTA from "./fm1ota.js";

const HTML = `
<section class="panel" aria-labelledby="fx-h1"><h2 id="fx-h1">1 &middot; CONNECT</h2>
  <div class="row"><button type="button" id="fx-connect" class="primary">Find the FM-1</button>
    <span id="fx-dev-msg" class="note">Plug the FM-1 in with a USB data cable. The browser asks once for MIDI access.</span></div>
  <dl class="kv" id="fx-dev" hidden><dt>Running</dt><dd id="fx-dev-id"></dd><dt>Port</dt><dd id="fx-dev-port"></dd></dl>
</section>
<section class="panel" aria-labelledby="fx-h2"><h2 id="fx-h2">2 &middot; FIRMWARE</h2>
  <div class="src" role="radiogroup" aria-label="Which firmware">
    <label><input type="radio" name="fx-src" value="latest" checked> <span>Latest release: <b id="fx-latest">looking&hellip;</b></span></label>
    <label><input type="radio" name="fx-src" value="file"> <span>A package on this computer (.fwsc)
      <input type="file" id="fx-file" accept=".fwsc" aria-label="Package file"></span></label>
  </div>
  <dl class="kv" id="fx-pkg" hidden><dt>Package</dt><dd id="fx-pkg-id"></dd><dt>Size</dt><dd id="fx-pkg-size"></dd>
    <dt>SHA-256</dt><dd id="fx-pkg-sha"></dd></dl>
  <p id="fx-pkg-msg" class="msg" hidden></p>
</section>
<section class="panel" aria-labelledby="fx-h3"><h2 id="fx-h3">3 &middot; INSTALL</h2>
  <label class="row"><input type="checkbox" id="fx-ack"> I will keep the FM-1 plugged in and this page open until it
    says done (about a minute).</label>
  <div class="row" style="margin-top:8px"><button type="button" id="fx-go" class="primary" disabled>Install</button>
    <span id="fx-why" class="note"></span></div>
  <progress id="fx-bar" max="100" value="0" hidden></progress>
  <p id="fx-msg" class="msg" role="status" hidden></p>
  <pre id="fx-log" class="log" hidden></pre>
</section>
<p class="note">The FM-1's own update loader is kept until the new firmware is written: if an install is cut off
  (cable out, page closed), plug it back in, find it and press Install again to finish. On Windows only one program
  can use the FM-1's MIDI port: close the patch library, the simulator's Link and any DAW first.
  The same install from a command line: <code>tools/fm1_install.py</code>.</p>`;

const HINT = {
  notfound: "Is it a data cable? Close other programs using the FM-1's MIDI (on Windows only one can), then try again.",
  lost: "Plug it back in, press Find the FM-1, then Install again: the FM-1 stays in update mode until it is done.",
  stopped: "Replug the USB cable, press Find the FM-1, then Install again to finish.",
  noloader: "Replug the USB cable, press Find the FM-1, then Install again: the FM-1 stays in update mode until it is done.",
  noreturn: "Power-cycle the FM-1, then press Find the FM-1 to check what it runs.",
};

const kb = (n) => (n / 1024).toFixed(0) + " KB";
const count = (e) => { try { if (window.fm1Count) window.fm1Count(e); } catch (x) { /* counting never stops a flash */ } };

export function mount(root, cfg) {
  root.classList.add("flash");
  root.innerHTML = HTML;
  const $ = (id) => root.querySelector("#" + id);
  const st = {access: null, up: null, dev: null, manifest: null, pkg: null, busy: false};

  function show(el, text, kind) { el.hidden = !text; el.textContent = text || ""; el.className = "msg" + (kind ? " " + kind : ""); }
  function log(line) { const l = $("fx-log"); l.hidden = false; l.textContent += line + "\n"; l.scrollTop = l.scrollHeight; }
  const src = () => root.querySelector('input[name="fx-src"]:checked').value;

  function refresh() {
    const d = st.dev, p = st.pkg;
    let why = "";
    if (!d) why = "Find the FM-1 first.";
    else if (!p) why = "Pick a firmware.";
    else if (!OTA.sameKeyboard(d.id.text, p.product)) why = `This package is for ${OTA.modelOf(p.product)}, not ${OTA.modelOf(d.id.text)}.`;
    else if (!$("fx-ack").checked) why = "Tick the box above.";
    else if (d.id.text === p.product) why = "It already runs this; Install writes it again.";
    else if (d.id.loader) why = "It is in update mode: Install finishes the write.";
    $("fx-why").textContent = st.busy ? "Installing: do not unplug." : why;
    $("fx-go").disabled = st.busy || !d || !p || !$("fx-ack").checked || !OTA.sameKeyboard(d.id.text, p.product);
    for (const id of ["fx-connect", "fx-file", "fx-ack"]) $(id).disabled = st.busy;
    root.querySelectorAll('input[name="fx-src"]').forEach((r) => { r.disabled = st.busy || (r.value === "latest" && !st.manifest); });
  }

  function setPkg(p, label) {
    st.pkg = p;
    $("fx-pkg").hidden = !p;
    if (p) {
      $("fx-pkg-id").textContent = p.product + (label ? "  (" + label + ")" : "");
      $("fx-pkg-size").textContent = kb(p.size);
      $("fx-pkg-sha").textContent = p.sha.slice(0, 16) + "…";
      $("fx-pkg-sha").title = p.sha;
    }
    refresh();
  }

  async function pickLatest() {
    show($("fx-pkg-msg"), "");
    const m = st.manifest;
    if (!m) { setPkg(null); return; }
    const want = st.dev ? st.dev.id.text : "FM-1_0";
    const f = m.files.find((x) => OTA.sameKeyboard(x.product, want));
    if (!f) { setPkg(null); show($("fx-pkg-msg"), `The latest release has no package for ${OTA.modelOf(want)}.`, "bad"); return; }
    try {
      const r = await fetch(new URL(f.file, new URL(cfg.manifest, location.href)), {cache: "no-cache"});
      if (!r.ok) throw new Error(r.status + " " + r.statusText);
      const bytes = new Uint8Array(await r.arrayBuffer()), sha = await OTA.sha256hex(bytes);
      if (sha !== f.sha256) throw new Error("its SHA-256 does not match the release's: reload the page");
      const p = OTA.loadPackage(bytes);
      setPkg({...p, size: bytes.length, sha}, (m.name || cfg.name) + " " + m.version);
    } catch (e) { setPkg(null); show($("fx-pkg-msg"), "Could not load " + f.file + ": " + e.message, "bad"); }
  }

  async function pickFile() {
    show($("fx-pkg-msg"), "");
    const f = $("fx-file").files[0];
    if (!f) { setPkg(null); return; }
    try {
      const bytes = new Uint8Array(await f.arrayBuffer());
      const p = OTA.loadPackage(bytes);
      setPkg({...p, size: bytes.length, sha: await OTA.sha256hex(bytes)}, f.name);
    } catch (e) { setPkg(null); show($("fx-pkg-msg"), f.name + ": " + e.message, "bad"); }
  }

  async function connect() {
    show($("fx-msg"), "");
    if (!navigator.requestMIDIAccess) { $("fx-dev-msg").textContent = "This browser has no Web MIDI: use Chrome or Edge on a computer."; return; }
    try {
      if (!st.access) { st.access = await navigator.requestMIDIAccess({sysex: true}); st.up = new OTA.Updater(new OTA.WebMidiBackend(st.access)); }
    } catch (e) { $("fx-dev-msg").textContent = "MIDI access was refused (the site settings can allow it)."; return; }
    $("fx-connect").disabled = true;
    $("fx-dev-msg").textContent = "Looking…";
    const dev = await st.up.find();
    $("fx-connect").disabled = false;
    st.dev = dev ? {id: dev.id, name: dev.name} : null;
    if (dev) dev.link.close();
    $("fx-dev").hidden = !dev;
    if (dev) {
      $("fx-dev-id").textContent = dev.id.text + (dev.id.loader ? "  (update mode: an install was not finished)" : "");
      $("fx-dev-port").textContent = dev.name;
      $("fx-dev-msg").textContent = "Found.";
    } else {
      const ins = st.up.backend.inputNames();
      $("fx-dev-msg").textContent = "No FM-1 answered. " + HINT.notfound + (ins.length ? "  MIDI inputs: " + ins.join(", ") : "");
    }
    if (src() === "latest") await pickLatest(); else refresh();
  }

  function guard(e) { e.preventDefault(); e.returnValue = ""; }

  async function install() {
    if (st.busy || !st.pkg) return;
    st.busy = true; refresh();
    window.addEventListener("beforeunload", guard);
    const p = st.pkg, bar = $("fx-bar");
    $("fx-log").textContent = ""; bar.hidden = false; bar.value = 0;
    show($("fx-msg"), "Installing " + p.product + "… do not unplug the FM-1.");
    const ev = cfg.name + "/flash/";
    count(ev + "start/" + p.product);
    try {
      const dev = await st.up.find();
      if (!dev) throw new OTA.InstallError("notfound", "the FM-1 did not answer");
      log(`device:  ${dev.id.text}  (${dev.name})`);
      log(`package: ${p.product}  ${kb(p.size)}`);
      let lastStep = "";
      const done = await st.up.install(dev, p.image, p.product, (k, a) => {
        if (k === "check") { bar.removeAttribute("value"); return; }
        if (k === "write") { bar.value = a; if (lastStep !== "write") log("writing…"); lastStep = k; return; }
        lastStep = k;
        log({start: `starting the update on ${a}`, loader: "switching to update mode", reboot: "restarting",
          done: `done: the FM-1 runs ${a}`}[k]);
      });
      bar.value = 100;
      show($("fx-msg"), "Done: the FM-1 runs " + done + ".", "good");
      st.dev = {id: {text: done, model: OTA.modelOf(done), version: +done.split("_")[1], loader: false}, name: $("fx-dev-port").textContent};
      $("fx-dev-id").textContent = done;
      count(ev + "done/" + p.product);
    } catch (e) {
      const code = e instanceof OTA.InstallError ? e.code : "error";
      log("error: " + e.message);
      show($("fx-msg"), "Not installed: " + e.message + (HINT[code] ? ". " + HINT[code] : ""), "bad");
      count(ev + "fail/" + code);
      st.dev = null; $("fx-dev").hidden = true;
    } finally {
      st.busy = false;
      window.removeEventListener("beforeunload", guard);
      refresh();
    }
  }

  $("fx-connect").addEventListener("click", connect);
  $("fx-go").addEventListener("click", install);
  $("fx-ack").addEventListener("change", refresh);
  $("fx-file").addEventListener("change", () => { root.querySelector('input[value="file"]').checked = true; pickFile(); });
  root.querySelectorAll('input[name="fx-src"]').forEach((r) => r.addEventListener("change", () => (src() === "latest" ? pickLatest() : pickFile())));

  fetch(cfg.manifest, {cache: "no-cache"}).then((r) => (r.ok ? r.json() : null)).catch(() => null).then((m) => {
    st.manifest = m && Array.isArray(m.files) && m.files.length ? m : null;
    const el = $("fx-latest");
    if (st.manifest) {
      el.textContent = `${st.manifest.name || cfg.name} ${st.manifest.version}`;
      if (st.manifest.url) { const a = document.createElement("a"); a.href = st.manifest.url; a.textContent = " release notes"; el.after(a); }
      pickLatest();
    } else {
      el.textContent = "none published here yet";
      root.querySelector('input[value="file"]').checked = true;
      refresh();
    }
  });
  refresh();
  return st;
}
