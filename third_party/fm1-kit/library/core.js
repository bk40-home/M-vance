/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-kit library: the page. Sources (what you have opened) above, the device's banks below; drag one patch or many
 * from one to the other; search, categories, tags; send only what changed, acknowledged and read back.
 * Everything firmware-specific comes from FK.adapter (adapters/README.md). The look: ui.css, the device's six themes.
 * Nothing leaves the page but MIDI; the library stays in this browser (IndexedDB) and goes out as zip files.
 * 0.3.0 (Kris 15 Nov 2026, fm1-port/library-save-blank-layers-proposal-2026-11-15.md, all as recommended):
 *   Save… on both panes (a bank as one file, the adapter's bankFile, else a zip); Export library takes the banks read
 *   from the device too; + New bank (an empty bank of your own: drop patches on its slots, move them about, Delete,
 *   double-click its name to rename); Undo covers source edits as well; Hold keeps a patch in the details panel
 *   while you click others; Layers (A.layers: M-vance's UPPER / LOWER) in the details panel. */
(function (G) {
  "use strict";
  const FK = G.FK;
  const A = FK.adapter;
  const $ = (s, r) => (r || document).querySelector(s);
  const SIZE = A.bankSize;

  /* ---- state */
  const S = {
    sources: [],                     /* {id, name, kind, slots: [patch|null]} */
    srcTab: 0,                       /* index into sources, or "all" */
    io: null, ident: null, sandbox: false,
    banks: new Map(),                /* device bank id -> slots as read (null: not read) */
    devTab: A.deviceBanks.find(b => !b.readOnly).id,
    pend: new FK.Pending(),
    hist: [],                        /* (0.3.0) the undo order: "dev" (S.pend has it) or a source's state before a change */
    pin: null,                       /* (0.3.0) Hold: the details panel stays on this slot, {dev, s} or {src, s} */
    backup: new Map(),               /* bank -> slots as they were before this session's first send */
    meta: {},                        /* fingerprint -> {cat, tags} */
    sel: {pane: null, set: new Set(), anchor: -1},
    focus: {src: 0, dev: 0}, lastPane: "src",
    query: FK.parseQuery(""), catChip: "",
    emptyOnly: false, details: false, busy: false,
    drag: null, hoverTimer: 0,
    ver: 0,                          /* bumped on every change to sources or the device's banks */
    menu: {open: false, filter: "", active: 0, confirm: ""},
  };
  const fpCache = new WeakMap(), skCache = new WeakMap();
  const fp = p => { let h = fpCache.get(p); if (!h) { h = FK.hash(A.fingerprint(p)); fpCache.set(p, h); } return h; };
  const sk = p => { let h = skCache.get(p); if (!h) { h = FK.hash(A.soundKey(p)); skCache.set(p, h); } return h; };
  const same = (a, b) => (!a && !b) || (!!a && !!b && fp(a) === fp(b));
  const catOf = p => (S.meta[fp(p)] && S.meta[fp(p)].cat) || FK.guessCategory(A.nameOf(p));
  const tagsOf = p => (S.meta[fp(p)] && S.meta[fp(p)].tags) || [];
  const bankDef = id => A.deviceBanks.find(b => b.id === id);
  /* (0.3.0) a source you can change: a bank you made or opened; the built-in one is read only */
  const editable = s => !!s && s.kind !== "factory";

  /* ---- persistence (IndexedDB: this browser only) */
  let db = null, saveT = 0;
  function idb() {
    return new Promise(ok => {
      try {
        const r = indexedDB.open("fk-" + A.id, 1);
        r.onupgradeneeded = () => r.result.createObjectStore("kv");
        r.onsuccess = () => ok(r.result);
        r.onerror = () => ok(null);
      } catch (e) { ok(null); }
    });
  }
  function kvGet(k) {
    return new Promise(ok => {
      if (!db) return ok(undefined);
      try { const r = db.transaction("kv").objectStore("kv").get(k); r.onsuccess = () => ok(r.result); r.onerror = () => ok(undefined); }
      catch (e) { ok(undefined); }
    });
  }
  function kvPut(k, v) { try { if (db) db.transaction("kv", "readwrite").objectStore("kv").put(v, k); } catch (e) { /* (storage full or off) */ } }
  /* the library in IndexedDB: an index of source ids, and a key a source, written only when it changed */
  const dirty = new Set();
  let written = new Set();
  const touch = s => { if (s && s.kind !== "factory") dirty.add(s.id); };
  function kvDel(k) { try { if (db) db.transaction("kv", "readwrite").objectStore("kv").delete(k); } catch (e) { /* */ } }
  function save(now) {
    if (!now) S.ver++;
    clearTimeout(saveT);
    saveT = 0;
    const put = () => {
      const keep = S.sources.filter(s => s.kind !== "factory"), ids = new Set(keep.map(s => s.id));
      for (const s of keep) if (dirty.has(s.id) || !written.has(s.id)) kvPut("src:" + s.id, s);
      for (const id of written) if (!ids.has(id)) kvDel("src:" + id);
      dirty.clear();
      written = ids;
      kvPut("srcIndex", keep.map(s => s.id));
      kvPut("meta", S.meta);
      const dev = {};
      for (const [b, sl] of S.banks) if (sl) dev[b] = sl;
      kvPut("device", dev);
      saveT = 0;
    };
    if (now) put(); else saveT = setTimeout(put, 300);
  }
  const flush = () => { if (saveT) save(true); };
  function pref(k, v) {
    try { if (v === undefined) return localStorage.getItem("fk-" + A.id + "-" + k); localStorage.setItem("fk-" + A.id + "-" + k, v); }
    catch (e) { return null; }
    return null;
  }

  /* ---- messages */
  function say(text, kind) { const s = $("#status"); s.textContent = text; s.dataset.kind = kind || ""; }
  function progress(done, total, text) {
    const p = $("#progress");
    p.hidden = !total;
    $("#progress-bar").style.width = total ? Math.round(100 * done / total) + "%" : "0";
    $("#progress-text").textContent = text || "";
  }

  /* ---- sources */
  let srcN = 0;
  const srcLabel = s => s.group && s.group !== s.name ? s.group + " › " + s.name : s.name;
  function addSource(name, kind, patches, places, group) {
    /* patches: [patch], places: slot each (or null: in order). Past the bank's size: more banks, "name (2)" */
    const banks = [];
    patches.forEach((p, i) => {
      const s = places ? places[i] : i, b = Math.floor(s / SIZE);
      while (banks.length <= b) banks.push(new Array(SIZE).fill(null));
      banks[b][s % SIZE] = p;
    });
    if (!banks.length) return -1;
    const first = S.sources.length;
    banks.forEach((slots, k) => S.sources.push({id: "s" + (++srcN) + "-" + Date.now(), name: k ? name + " (" + (k + 1) + ")" : name, kind, slots, group: group || name}));
    save();
    return first;
  }
  async function readEntries(files) {
    /* files: [{path, name, data: Uint8Array}] -> sources by folder; zips opened (a zip with library.json: a library) */
    const groups = new Map();
    let opened = 0, skipped = 0, first = -1;
    for (const f of files) {
      if (/\.zip$/i.test(f.name)) {
        let ents;
        try { ents = await FK.unzip(f.data, (d, t) => progress(d, t, "Unpacking " + f.name + ": " + d + " of " + t)); }
        catch (e) { say(f.name + ": " + e.message, "bad"); progress(0, 0); continue; }
        const lib = ents.find(e => /(^|\/)library\.json$/.test(e.name));
        if (lib) { const k = await importLibrary(ents, lib, f.name); if (first < 0) first = k; opened += 1; continue; }
        const zb = f.name.replace(/\.zip$/i, "");
        for (const e of ents) {
          const dir = e.name.includes("/") ? e.name.slice(0, e.name.lastIndexOf("/")) : "";
          const bank = tidyPath(dir, zb) || zb, key = zb + "\u0000" + bank;
          if (!groups.has(key)) groups.set(key, {group: zb, name: bank, list: []});
          groups.get(key).list.push({name: e.name.split("/").pop(), data: e.data, kind: "zip"});
        }
        continue;
      }
      const dir = f.path && f.path.includes("/") ? f.path.slice(0, f.path.lastIndexOf("/")) : "";
      const top = dir ? dir.split("/")[0] : "Opened files", bank = dir ? tidyPath(dir, top) || top : "Opened files";
      const key = top + "\u0000" + bank;
      if (!groups.has(key)) groups.set(key, {group: top, name: bank, list: []});
      groups.get(key).list.push({name: f.name, data: f.data, kind: f.path ? "folder" : "files"});
    }
    let gi = 0;
    const gn = groups.size, total = [...groups.values()].reduce((a, g) => a + g.list.length, 0);
    let done = 0;
    const nat = (x, y) => x.localeCompare(y, undefined, {numeric: true, sensitivity: "base"});
    for (const {group, name, list} of [...groups.values()].sort((x, y) => nat(x.group, y.group) || nat(x.name, y.name))) {
      if (++gi % 8 === 0) { progress(done, total, "Reading patches: " + done + " of " + total); await new Promise(r => setTimeout(r, 0)); }
      done += list.length;
      list.sort((a, b) => a.name.localeCompare(b.name, undefined, {numeric: true}));
      const pats = [], names = [];
      for (const x of list) {
        const ps = A.parseFile(x.name, x.data);
        if (!ps || !ps.length) { skipped++; continue; }
        ps.forEach((p, i) => { pats.push(p); names.push(ps.length > 1 ? FK.slotName(i) : x.name); });
      }
      if (!pats.length) continue;
      const k = addSource(name, list[0].kind, pats, FK.placeByNumber(names, SIZE), group);
      if (first < 0) first = k;
      opened += pats.length;
    }
    progress(0, 0);
    void gn;
    if (first >= 0) { S.srcTab = first; S.lastBank = first; }
    say(opened ? "Opened " + opened + (skipped ? " (" + skipped + " files were not patches)" : "") : "No patches in what was opened", opened ? "" : "bad");
    render();
  }
  /* a bank's name from its folder path: the collection's own name and repeats left out ("Kel/Kel" is "Kel") */
  function tidyPath(dir, top) {
    const parts = String(dir).split("/").map(x => x.trim()).filter(Boolean);
    const out = [];
    for (const p of parts) if (p !== top && p !== out[out.length - 1]) out.push(p);
    return out.join(" / ");
  }
  function tidySource(s) {         /* (sources saved before groups: "A/A" -> group A, bank A) */
    if (s.group) return s;
    const parts = s.name.split("/").map(x => x.trim()).filter(Boolean);
    s.group = parts[0] || s.name;
    s.name = tidyPath(parts.slice(1).join("/"), s.group) || s.group;
    return s;
  }
  async function importLibrary(ents, libEnt, zipName) {
    let lib;
    try { lib = JSON.parse(new TextDecoder().decode(libEnt.data)); } catch (e) { say(zipName + ": library.json does not read", "bad"); return -1; }
    if (lib.meta) Object.assign(S.meta, lib.meta);
    let first = -1;
    for (const src of lib.sources || []) {
      const pats = [], places = [];
      for (const f of src.files || []) {
        const e = ents.find(x => x.name === f.file);
        const ps = e && A.parseFile(f.file, e.data);
        if (ps && ps[0]) { pats.push(ps[0]); places.push(f.slot); }
      }
      const k = addSource(src.name, src.kind || "zip", pats, places, src.group || zipName.replace(/\.zip$/i, ""));
      if (first < 0) first = k;
    }
    say("Library imported from " + zipName);
    return first;
  }
  async function filesFromInput(list) {
    const out = [];
    for (const f of list) out.push({name: f.name, path: f.webkitRelativePath || "", data: new Uint8Array(await f.arrayBuffer())});
    return out;
  }
  async function filesFromDrop(dt) {
    const out = [], walk = async (entry, path) => {
      if (entry.isFile) {
        const f = await new Promise((ok, no) => entry.file(ok, no));
        out.push({name: f.name, path: path ? path + "/" + f.name : "", data: new Uint8Array(await f.arrayBuffer())});
      } else if (entry.isDirectory) {
        const r = entry.createReader();
        let batch;
        do {
          batch = await new Promise((ok, no) => r.readEntries(ok, no));
          for (const e of batch) await walk(e, path ? path + "/" + entry.name : entry.name);
        } while (batch.length);
      }
    };
    const entries = [...dt.items].map(i => i.webkitGetAsEntry && i.webkitGetAsEntry()).filter(Boolean);
    if (entries.length) for (const e of entries) await walk(e, "");
    else for (const f of dt.files) out.push({name: f.name, path: "", data: new Uint8Array(await f.arrayBuffer())});
    return out;
  }

  /* ---- what each pane shows */
  const ALL_MAX = 512;              /* "All sources" shows this many matches at most: refine the search for more */
  let allCache = null;
  function srcSlots() {
    if (S.srcTab === "all") {
      const key = S.ver + "|" + JSON.stringify(S.query);
      if (allCache && allCache.key === key) return allCache.v;
      const all = [];
      let total = 0;
      for (const s of S.sources) s.slots.forEach((p, i) => {
        if (!p || !FK.matches(entryOf(p, srcLabel(s), ""), S.query)) return;
        total++;
        if (all.length < ALL_MAX) all.push({p, from: srcLabel(s) + " " + FK.slotName(i)});
      });
      allCache = {key, v: {slots: all.map(x => x.p), from: all.map(x => x.from), all: true, total}};
      return allCache.v;
    }
    const s = S.sources[S.srcTab];
    return {slots: s ? s.slots : new Array(SIZE).fill(null), from: null, all: false};
  }
  const devLoaded = b => S.banks.get(b) || new Array(bankDef(b).size).fill(null);
  const devView = b => S.pend.view(b, devLoaded(b));
  let dupMap = new Map(), dupVer = -1;
  function rebuildDups() {          /* only when asked for (the dup filter, the details panel), and once a change */
    if (dupVer === S.ver) return;
    dupVer = S.ver;
    dupMap = new Map();
    const add = (p, where) => { const k = sk(p); if (!dupMap.has(k)) dupMap.set(k, []); dupMap.get(k).push(where); };
    S.sources.forEach(s => s.slots.forEach((p, i) => p && add(p, s.name + " " + FK.slotName(i))));
    for (const b of A.deviceBanks) devView(b.id).forEach((p, i) => p && add(p, b.name + " " + FK.slotName(i)));
  }
  function entryOf(p, src, bank) {
    const d = S.query.dup ? dupMap.get(sk(p)) : null;
    return {name: A.nameOf(p), cat: catOf(p), colour: A.colourOf(p), tags: tagsOf(p), src, bank, dup: !!(d && d.length > 1)};
  }

  /* ---- the grids: columns sized to the longest name, filled down the columns */
  const measure = document.createElement("canvas").getContext("2d");
  function layout(grid, slots) {
    const cs = getComputedStyle(grid);
    measure.font = cs.getPropertyValue("--cell-font").trim() || "500 13px system-ui";
    let w = measure.measureText("n".repeat(A.nameLength)).width;   /* (a typical full name: both banks line up) */
    for (const p of slots) if (p) w = Math.max(w, measure.measureText(A.nameOf(p)).width);
    measure.font = "11px " + (cs.fontFamily || "system-ui");
    const numW = measure.measureText("000").width;
    const colW = Math.ceil(w + numW) + 20;        /* + padding 8, two gaps 8, the stripe 3 */
    const gap = 2, width = (grid.clientWidth || 1200) - 4;
    const cols = Math.max(1, Math.min(Math.ceil(slots.length / 4) || 1, Math.floor((width + gap) / (colW + gap))));
    const rows = Math.max(1, Math.ceil(slots.length / cols));
    grid.style.gridTemplateColumns = "repeat(" + cols + ", minmax(" + colW + "px, 1fr))";
    grid.style.gridTemplateRows = "repeat(" + rows + ", var(--row-h))";
    grid.dataset.rows = rows;
    grid.dataset.cols = cols;
  }
  function fillGrid(grid, slots, o) {
    layout(grid, slots);
    const frag = document.createDocumentFragment();
    slots.forEach((p, i) => {
      const c = document.createElement("div");
      c.className = "cell";
      c.dataset.i = i;
      c.setAttribute("role", "gridcell");
      const num = o.from ? String(i + 1).padStart(3, "0") : FK.slotName(i);
      if (!p) {
        c.classList.add("empty");
        c.innerHTML = '<span class="num"></span><span class="nm">—</span>';
        c.firstChild.textContent = num;
        c.setAttribute("aria-label", num + " empty");
      } else {
        const name = A.nameOf(p), cat = catOf(p);
        c.draggable = true;
        c.innerHTML = '<span class="num"></span><span class="stripe"></span><span class="nm"></span>';
        c.children[0].textContent = num;
        c.children[1].style.background = "var(--cat-" + cat + ")";
        c.children[2].textContent = name;
        const col = A.colourOf(p);
        if (col && col !== "default") c.children[0].style.color = "var(--pc-" + col + ")";   /* the patch's colour: its number */
        const where = o.from ? o.from[i] + " · " : "";
        c.title = name + " · " + where + cat + (tagsOf(p).length ? " · " + tagsOf(p).join(", ") : "");
        c.setAttribute("aria-label", num + " " + name + ", " + cat);
        if (o.dim && !FK.matches(entryOf(p, o.srcName || "", o.bankName || ""), S.query)) c.classList.add("dim");
      }
      if (o.changed && o.changed.has(i)) { c.classList.add("changed"); c.setAttribute("aria-label", c.getAttribute("aria-label") + ", changed, not sent"); }
      if (S.sel.pane === o.pane && S.sel.set.has(i)) c.classList.add("sel");
      if ((o.pane === "src" ? S.focus.src : S.focus.dev) === i) c.classList.add("focus");
      frag.appendChild(c);
    });
    grid.replaceChildren(frag);
  }

  function render() {
    rebuildDups();
    /* the source picker: the bank shown, back / forward, and the list (renderMenu) */
    const cur = S.sources[S.srcTab];
    $("#src-pick .g").textContent = S.srcTab === "all" ? "" : cur && cur.group !== cur.name ? cur.group + " › " : "";
    $("#src-pick .t").textContent = S.srcTab === "all" ? "All sources" : cur ? cur.name : "Nothing open";
    $("#src-pick .n").textContent = S.srcTab === "all" ? "" : cur ? cur.slots.filter(Boolean).length : "";
    $("#src-pick").title = S.srcTab === "all" ? "All sources" : cur ? srcLabel(cur) : "";
    $("#src-prev").disabled = S.srcTab === "all" || S.srcTab <= 0;
    $("#src-next").disabled = S.srcTab === "all" || S.srcTab >= S.sources.length - 1;
    $("#src-save").disabled = S.srcTab === "all" || !cur;
    $("#src-pick").classList.toggle("own", S.srcTab !== "all" && editable(cur));
    $("#src-all").setAttribute("aria-pressed", S.srcTab === "all" ? "true" : "false");
    $("#src-all").textContent = S.srcTab === "all" ? "Back to " + (S.lastBank != null && S.sources[S.lastBank] ? S.sources[S.lastBank].name : "the bank") : "All sources";
    const nGroups = new Set(S.sources.map(x => x.group)).size;
    $("#src-pick-total").textContent = S.sources.length > 1 ? S.sources.length + " banks in " + nGroups + " collection" + (nGroups > 1 ? "s" : "") : "";
    if (S.menu.open) renderMenu();
    /* source grid */
    const ss = srcSlots(), src = S.sources[S.srcTab];
    fillGrid($("#src-grid"), ss.slots, {pane: "src", dim: !ss.all, from: ss.from, srcName: src ? srcLabel(src) : ""});
    const matchN = ss.all ? ss.total : ss.slots.filter(p => p && FK.matches(entryOf(p, src ? srcLabel(src) : "", ""), S.query)).length;
    $("#src-count").textContent = ss.all ? (ss.total > ss.slots.length ? "first " + ss.slots.length + " of " + ss.total + ": search to narrow" : ss.total + (S.query.empty ? " patches" : " match")) :
      S.query.empty ? ss.slots.filter(Boolean).length + " patches" : matchN + " match here · All sources for every bank";
    $("#src-sel").textContent = S.sel.pane === "src" && S.sel.set.size ? S.sel.set.size + " selected · drag down to a slot, or S" : "";
    /* device tabs */
    const dt = $("#dev-tabs");
    dt.replaceChildren();
    for (const b of A.deviceBanks) {
      const t = document.createElement("button");
      t.type = "button";
      t.className = "tab" + (S.devTab === b.id ? " on" : "");
      const v = devView(b.id), used = S.banks.get(b.id) ? v.filter(Boolean).length : "", ch = S.pend.count(b.id);
      t.innerHTML = '<span class="t"></span> <span class="n"></span>' + (ch ? '<span class="chg" aria-label="changed"></span>' : "");
      t.querySelector(".t").textContent = b.name;
      t.querySelector(".n").textContent = used;
      t.onclick = () => { S.devTab = b.id; S.sel = {pane: null, set: new Set(), anchor: -1}; render(); if (S.io && !S.banks.get(b.id)) readBank(b.id); };
      t.ondragenter = () => { clearTimeout(S.hoverTimer); S.hoverTimer = setTimeout(() => { if (S.devTab !== b.id) { S.devTab = b.id; render(); } }, 500); };
      t.ondragleave = () => clearTimeout(S.hoverTimer);
      dt.appendChild(t);
    }
    /* device grid */
    const bd = bankDef(S.devTab), view = devView(S.devTab), pm = S.pend.of(S.devTab);
    fillGrid($("#dev-grid"), view, {pane: "dev", dim: false, changed: new Set(pm.keys()), bankName: bd.name});
    const loaded = !!S.banks.get(S.devTab);
    $("#dev-info").textContent = !S.io ? "not connected" : (S.sandbox ? "SANDBOX · " : S.ident.product + " " + S.ident.version + " · ") +
      (!loaded ? bd.name + " not read yet" : bd.name + " " + view.filter(Boolean).length + " of " + bd.size + (bd.readOnly ? " · read only" : ""));
    const n = S.pend.total();
    $("#send").textContent = n ? "Send " + n + " change" + (n > 1 ? "s" : "") : "Send";
    $("#send").disabled = !n || S.busy;
    $("#read").disabled = !S.io || S.busy;
    $("#restore").hidden = !S.backup.has(S.devTab);
    $("#undo").disabled = !S.pend.undo.length && !S.hist.some(h => h !== "dev");
    $("#conn").textContent = S.ident ? "Disconnect" : "Connect";
    $("#conn").classList.toggle("on", !!S.ident);
    $("#dev-dot").classList.toggle("on", !!S.ident);
    $("#empty-only").checked = S.emptyOnly;
    document.querySelectorAll("#cats button").forEach(b => b.classList.toggle("on", b.dataset.cat === S.catChip));
    if (S.details) rebuildDups();
    renderDetails();
  }

  /* ---- the source list: grouped by collection, filtered, its rows made only while it is open */
  const MENU_MAX = 400;
  function menuRows() {
    const f = S.menu.filter.toLowerCase().trim(), rows = [];
    let last = null, shown = 0, hidden = 0;
    S.sources.forEach((s, k) => {
      if (f && !srcLabel(s).toLowerCase().includes(f)) return;
      if (shown >= MENU_MAX) { hidden++; return; }
      if (s.group !== last) { rows.push({grp: s.group}); last = s.group; }
      rows.push({k, s}); shown++;
    });
    return {rows, hidden};
  }
  function renderMenu() {
    const box = $("#src-list"), {rows, hidden} = menuRows(), opts = rows.filter(r => r.s);
    S.menu.active = Math.max(0, Math.min(opts.length - 1, S.menu.active));
    const frag = document.createDocumentFragment();
    let oi = 0;
    for (const r of rows) {
      const d = document.createElement("div");
      if (r.grp !== undefined) {
        const n = S.sources.filter(x => x.group === r.grp).length, builtin = S.sources.some(x => x.group === r.grp && x.kind === "factory");
        d.className = "grp";
        d.innerHTML = '<span class="gt"></span><span class="n"></span>' + (builtin ? "" : '<button type="button" class="x"></button>');
        d.querySelector(".gt").textContent = r.grp;
        d.querySelector(".n").textContent = n + (n > 1 ? " banks" : " bank");
        const x = d.querySelector(".x");
        if (x) {
          const armed = S.menu.confirm === "g:" + r.grp;
          x.textContent = armed ? "Close " + n + "?" : "Close";
          x.classList.toggle("armed", armed);
          x.onclick = e => { e.stopPropagation(); if (!armed) { S.menu.confirm = "g:" + r.grp; renderMenu(); return; } closeSources(s => s.group === r.grp); };
        }
      } else {
        d.className = "it" + (r.k === S.srcTab ? " on" : "") + (oi === S.menu.active ? " act" : "");
        d.setAttribute("role", "option");
        d.setAttribute("aria-selected", r.k === S.srcTab ? "true" : "false");
        d.id = "src-opt-" + oi;
        d.innerHTML = '<span class="t"></span><span class="n"></span>' + (r.s.kind === "factory" ? "" : '<button type="button" class="x" aria-label="Close this bank">×</button>');
        d.querySelector(".t").textContent = r.s.name;
        d.querySelector(".n").textContent = r.s.slots.filter(Boolean).length;
        d.onclick = e => {
          if (e.target.classList.contains("x")) { e.stopPropagation(); closeSources(s => s === r.s); return; }
          pickSource(r.k);
        };
        oi++;
      }
      frag.appendChild(d);
    }
    if (hidden) { const d = document.createElement("div"); d.className = "more"; d.textContent = hidden + " more: type to narrow the list"; frag.appendChild(d); }
    if (!rows.length) { const d = document.createElement("div"); d.className = "more"; d.textContent = "No bank by that name"; frag.appendChild(d); }
    box.replaceChildren(frag);
    $("#src-filter").setAttribute("aria-activedescendant", opts.length ? "src-opt-" + S.menu.active : "");
    const act = $("#src-list .it.act");
    if (act) act.scrollIntoView({block: "nearest"});
    const opened = S.sources.filter(x => x.kind !== "factory").length, armedAll = S.menu.confirm === "all";
    $("#src-closeall").hidden = !opened;
    $("#src-closeall").textContent = armedAll ? "Close all " + opened + "? Click again" : "Close all opened";
    $("#src-closeall").classList.toggle("armed", armedAll);
  }
  function openMenu(open) {
    S.menu.open = open;
    S.menu.confirm = "";
    $("#src-menu").hidden = !open;
    $("#src-pick").setAttribute("aria-expanded", open ? "true" : "false");
    if (open) {
      $("#src-filter").value = S.menu.filter;
      const opts = menuRows().rows.filter(r => r.s);
      S.menu.active = Math.max(0, opts.findIndex(r => r.k === S.srcTab));
      renderMenu();
      $("#src-filter").focus();
    }
  }
  function pickSource(k) {
    S.srcTab = k; S.lastBank = k;
    S.sel = {pane: null, set: new Set(), anchor: -1};
    openMenu(false);
    render();
    $("#src-grid").focus({preventScroll: true});
  }
  function closeSources(fn) {
    const keep = S.sources.filter(s => s.kind === "factory" || !fn(s)), n = S.sources.length - keep.length;
    const cur = S.sources[S.srcTab];
    S.sources = keep;
    S.srcTab = Math.max(0, keep.indexOf(cur));
    S.menu.confirm = "";
    save();
    say("Closed " + n + (n > 1 ? " banks" : " bank") + " (the library keeps no copy: open them again to get them back)");
    render();
    if (S.menu.open) renderMenu();
  }

  /* ---- selection, focus, keys */
  function paneSlots(pane) { return pane === "src" ? srcSlots().slots : devView(S.devTab); }
  function selectClick(pane, i, e) {
    if (S.sel.pane !== pane) S.sel = {pane, set: new Set(), anchor: -1};
    const slots = paneSlots(pane);
    if (e.shiftKey && S.sel.anchor >= 0) {
      const [a, b] = [Math.min(S.sel.anchor, i), Math.max(S.sel.anchor, i)];
      if (!(e.ctrlKey || e.metaKey)) S.sel.set.clear();
      for (let k = a; k <= b; k++) if (slots[k]) S.sel.set.add(k);
    } else if (e.ctrlKey || e.metaKey) {
      if (S.sel.set.has(i)) S.sel.set.delete(i); else if (slots[i]) S.sel.set.add(i);
      S.sel.anchor = i;
    } else {
      S.sel.set = new Set(slots[i] ? [i] : []);
      S.sel.anchor = i;
    }
    if (pane === "src") S.focus.src = i; else S.focus.dev = i;
    S.lastPane = pane;
    render();
  }
  function selected(pane) {
    const slots = paneSlots(pane);
    if (S.sel.pane === pane && S.sel.set.size) return [...S.sel.set].sort((a, b) => a - b).filter(i => slots[i]);
    const f = pane === "src" ? S.focus.src : S.focus.dev;
    return slots[f] ? [f] : [];
  }
  function onKey(pane, e) {
    const grid = pane === "src" ? $("#src-grid") : $("#dev-grid");
    const rows = +grid.dataset.rows || 16, n = paneSlots(pane).length, key = pane === "src" ? "src" : "dev";
    let f = S.focus[key];
    const mv = d => { e.preventDefault(); f = Math.max(0, Math.min(n - 1, f + d)); S.focus[key] = f; S.lastPane = pane; if (e.shiftKey) { if (S.sel.pane !== pane) S.sel = {pane, set: new Set(), anchor: f}; if (paneSlots(pane)[f]) S.sel.set.add(f); } render(); };
    const ctrl = e.ctrlKey || e.metaKey;
    if (e.key === "ArrowDown") mv(1);
    else if (e.key === "ArrowUp") mv(-1);
    else if (e.key === "ArrowRight") mv(rows);
    else if (e.key === "ArrowLeft") mv(-rows);
    else if (e.key === " ") { e.preventDefault(); selectClick(pane, f, {ctrlKey: true}); }
    else if (e.key === "Enter") { e.preventDefault(); const p = paneSlots(pane)[f]; if (p) audition(p); }
    else if (e.key === "i" || e.key === "I") { S.details = !S.details; render(); }
    else if ((e.key === "s" || e.key === "S") && !ctrl && pane === "src") { e.preventDefault(); dropInto(S.focus.dev, "src", srcSlots().slots, selected("src"), false); }
    else if ((e.key === "Delete" || e.key === "Backspace") && pane === "dev") { e.preventDefault(); clearSlots(selected("dev")); }
    else if ((e.key === "Delete" || e.key === "Backspace") && pane === "src") { e.preventDefault(); clearSource(selected("src")); }
    else if (ctrl && (e.key === "z" || e.key === "Z")) { e.preventDefault(); if (e.shiftKey) redo(); else undo(); }
    else if (ctrl && (e.key === "y" || e.key === "Y")) { e.preventDefault(); redo(); }
    else if (ctrl && (e.key === "a" || e.key === "A")) { e.preventDefault(); const sl = paneSlots(pane); S.sel = {pane, set: new Set(sl.map((p, i) => p ? i : -1).filter(i => i >= 0)), anchor: 0}; render(); }
  }

  /* ---- changes to the device's banks */
  function changeDev(bank, changes, what) {
    const bd = bankDef(bank);
    if (bd.readOnly) { say(bd.name + " is read only", "bad"); return false; }
    S.pend.apply(bank, changes, devLoaded(bank), same);
    S.hist.push("dev");
    say(what + " · Ctrl+Z undoes · nothing reaches the " + A.deviceName + " until Send");
    save();
    render();
    return true;
  }
  function dropInto(start, fromPane, fromSlots, idx, copy) {
    const bank = S.devTab, view = devView(bank);
    if (!idx.length) { say("Select patches first", "bad"); return; }
    if (fromPane === "dev" && fromSlots === null) {
      const ch = FK.planMove(view, idx, start, copy, S.emptyOnly);
      if (ch.size) changeDev(bank, ch, (copy ? "Copied " : "Moved ") + idx.length + " to " + bankDef(bank).name + " " + FK.slotName(start));
      return;
    }
    const items = idx.map(i => fromSlots[i]).filter(Boolean);
    const plan = FK.planDrop(view, start, items.length, S.emptyOnly);
    if (!plan.slots.length) { say("No room from " + FK.slotName(start), "bad"); return; }
    const ch = new Map();
    plan.slots.forEach((s, k) => ch.set(s, items[k]));
    changeDev(bank, ch, "Put " + plan.slots.length + " into " + bankDef(bank).name + " " + FK.slotName(plan.slots[0]) +
      (plan.slots.length > 1 ? "–" + FK.slotName(plan.slots[plan.slots.length - 1]) : "") +
      (plan.replaces ? " · replaced " + plan.replaces : "") + (plan.overflow ? " · " + plan.overflow + " did not fit" : ""));
  }
  function clearSlots(idx) {
    if (!idx.length) return;
    if (!A.device.clear) { say("A slot on the " + A.deviceName + " always holds a patch: drop another one on it instead", "bad"); return; }
    const ch = new Map(idx.map(i => [i, null]));
    changeDev(S.devTab, ch, "Cleared " + idx.length);
  }
  /* ---- (0.3.0) changes to a source bank of your own. Undo takes back the latest change, a source's or the device's
   * (S.hist keeps the order); a source change has no redo */
  function changeSrc(s, changes, what) {
    if (!editable(s)) { say((s ? s.name : "That bank") + " is built in (read only): drag the patches into a bank of your own (+ New bank)", "bad"); return false; }
    if (!changes.size) return false;
    S.hist.push({src: s.id, name: s.name, slots: s.slots.slice()});
    for (const [i, p] of changes) s.slots[i] = p;
    touch(s); save();
    say(what + " · Ctrl+Z undoes");
    render();
    return true;
  }
  function dropIntoSource(s, start, d, copy) {    /* d: S.drag. Within the same bank a move (Alt: a copy) */
    if (d.pane === "src" && d.srcId === s.id) {
      const ch = FK.planMove(s.slots, d.idx, start, copy, S.emptyOnly);
      changeSrc(s, ch, (copy ? "Copied " : "Moved ") + d.idx.length + " to " + s.name + " " + FK.slotName(start));
      return;
    }
    const items = d.idx.map(i => d.slots[i]).filter(Boolean);
    const plan = FK.planDrop(s.slots, start, items.length, S.emptyOnly);
    if (!plan.slots.length) { say("No room from " + FK.slotName(start), "bad"); return; }
    const ch = new Map();
    plan.slots.forEach((k, j) => ch.set(k, items[j]));
    changeSrc(s, ch, "Put " + plan.slots.length + " into " + s.name + " " + FK.slotName(plan.slots[0]) +
      (plan.slots.length > 1 ? "–" + FK.slotName(plan.slots[plan.slots.length - 1]) : "") +
      (plan.replaces ? " · replaced " + plan.replaces : "") + (plan.overflow ? " · " + plan.overflow + " did not fit" : ""));
  }
  function clearSource(idx) {
    const s = S.sources[S.srcTab];
    if (S.srcTab === "all" || !idx.length) return;
    changeSrc(s, new Map(idx.map(i => [i, null])), "Cleared " + idx.length + " in " + (s ? s.name : ""));
  }
  function undo() {
    let h;
    do h = S.hist.pop(); while (h === "dev" && !S.pend.undo.length);   /* (a send ends the device's undo) */
    if (h && h !== "dev") {
      const s = S.sources.find(x => x.id === h.src);
      if (!s) { say("That bank was closed: nothing to undo there", "bad"); render(); return; }
      s.slots = h.slots; s.name = h.name; touch(s); save(); say("Undone in " + s.name); render();
      return;
    }
    if (S.pend.back() !== null) { say("Undone"); save(); render(); }
  }
  function redo() { if (S.pend.forward() !== null) { S.hist.push("dev"); say("Redone"); save(); render(); } }

  /* ---- drag and drop */
  function markTargets(slots, text, grid) {
    document.querySelectorAll(".grid .target").forEach(c => c.classList.remove("target"));
    for (const s of slots) { const c = $((grid || "#dev-grid") + ' .cell[data-i="' + s + '"]'); if (c) c.classList.add("target"); }
    if (text) say(text); 
  }
  function bindGrid(grid, pane) {
    grid.addEventListener("click", e => { const c = e.target.closest(".cell"); if (c) selectClick(pane, +c.dataset.i, e); grid.focus({preventScroll: true}); });
    grid.addEventListener("dblclick", e => { const c = e.target.closest(".cell"); const p = c && paneSlots(pane)[+c.dataset.i]; if (p) audition(p); });
    grid.addEventListener("keydown", e => onKey(pane, e));
    grid.addEventListener("dragstart", e => {
      const c = e.target.closest(".cell");
      if (!c) return;
      const i = +c.dataset.i;
      const idx = S.sel.pane === pane && S.sel.set.has(i) ? [...S.sel.set].sort((a, b) => a - b) : [i];
      const slots = paneSlots(pane);
      S.drag = {pane, bank: pane === "dev" ? S.devTab : null, idx, slots: slots.slice(),
        srcId: pane === "src" && S.srcTab !== "all" && S.sources[S.srcTab] ? S.sources[S.srcTab].id : null};
      e.dataTransfer.effectAllowed = "copyMove";
      e.dataTransfer.setData("text/plain", idx.map(k => slots[k] ? A.nameOf(slots[k]) : "").join("\n"));
    });
    grid.addEventListener("dragend", () => { S.drag = null; markTargets([]); });
  }
  function bindDeviceDrop(grid) {
    const plan = e => {
      const c = e.target.closest(".cell");
      if (!c) return null;
      const start = +c.dataset.i, view = devView(S.devTab), d = S.drag;
      if (d) {
        if (d.pane === "dev" && d.bank === S.devTab) {
          const ch = FK.planMove(view, d.idx, start, e.altKey, S.emptyOnly);
          return {start, slots: [...ch.keys()].filter(s => ch.get(s)), text: (e.altKey ? "Copy " : "Move ") + d.idx.length + " to " + FK.slotName(start)};
        }
        const p = FK.planDrop(view, start, d.idx.length, S.emptyOnly);
        return {start, slots: p.slots, text: "Dropping " + p.slots.length + " into " + bankDef(S.devTab).name + " " +
          FK.slotName(p.slots[0] || start) + (p.slots.length > 1 ? "–" + FK.slotName(p.slots[p.slots.length - 1]) : "") +
          (p.replaces ? " · replaces " + p.replaces : "") + (p.overflow ? " · " + p.overflow + " will not fit" : "")};
      }
      return {start, slots: [start], text: "Drop files to open them into " + bankDef(S.devTab).name + " from " + FK.slotName(start)};
    };
    grid.addEventListener("dragover", e => {
      if (bankDef(S.devTab).readOnly) { e.dataTransfer.dropEffect = "none"; markTargets([], bankDef(S.devTab).name + " is read only"); return; }
      const p = plan(e);
      if (!p) return;
      e.preventDefault();
      e.dataTransfer.dropEffect = S.drag && S.drag.pane === "dev" && S.drag.bank === S.devTab && !e.altKey ? "move" : "copy";
      markTargets(p.slots, p.text);
    });
    grid.addEventListener("dragleave", e => { if (!grid.contains(e.relatedTarget)) markTargets([]); });
    grid.addEventListener("drop", async e => {
      e.preventDefault();
      const p = plan(e), d = S.drag;
      markTargets([]);
      if (!p) return;
      if (d) {
        if (d.pane === "dev" && d.bank === S.devTab) dropInto(p.start, "dev", null, d.idx, e.altKey);
        else dropInto(p.start, d.pane, d.slots, d.idx, true);
      } else if (e.dataTransfer.files.length) {
        const files = await filesFromDrop(e.dataTransfer), before = S.sources.length;
        await readEntries(files);
        const added = S.sources.slice(before);
        const pats = added.flatMap(s => s.slots.filter(Boolean));
        if (pats.length) dropInto(p.start, "src", pats, pats.map((_, i) => i), true);
      }
      S.drag = null;
    });
  }
  function bindSourceDrop(pane) {
    /* (0.3.0) a slot of the bank shown, when it is your own and the drag is a patch: the drop goes there */
    const slotAt = e => {
      const c = e.target.closest("#src-grid .cell"), s = S.srcTab === "all" ? null : S.sources[S.srcTab];
      return c && S.drag && editable(s) ? {s, start: +c.dataset.i} : null;
    };
    pane.addEventListener("dragover", e => {
      const t = slotAt(e);
      if (t) {
        e.preventDefault();
        const same = S.drag.pane === "src" && S.drag.srcId === t.s.id;
        e.dataTransfer.dropEffect = same && !e.altKey ? "move" : "copy";
        const p = same ? null : FK.planDrop(t.s.slots, t.start, S.drag.idx.length, S.emptyOnly);
        markTargets(p ? p.slots : [t.start], (same ? (e.altKey ? "Copy " : "Move ") : "Dropping ") + S.drag.idx.length + " into " + t.s.name + " " + FK.slotName(t.start), "#src-grid");
        return;
      }
      if (S.drag && S.drag.pane === "src") return;
      e.preventDefault();
      e.dataTransfer.dropEffect = "copy";
      pane.classList.add("drop-here");
    });
    pane.addEventListener("dragleave", e => { if (!pane.contains(e.relatedTarget)) pane.classList.remove("drop-here"); });
    pane.addEventListener("drop", async e => {
      e.preventDefault();
      pane.classList.remove("drop-here");
      markTargets([]);
      const d = S.drag, t = slotAt(e);
      if (t) { dropIntoSource(t.s, t.start, d, e.altKey); S.drag = null; return; }
      if (d && d.pane === "dev") {      /* taken from the device into the library */
        const pats = d.idx.map(i => d.slots[i]).filter(Boolean);
        const bd = bankDef(d.bank);
        S.srcTab = addSource("From " + bd.name, "device", pats, d.idx.filter(i => d.slots[i]));
        say("Kept " + pats.length + " from " + bd.name + " in the library");
        render();
      } else if (e.dataTransfer.files.length) await readEntries(await filesFromDrop(e.dataTransfer));
      S.drag = null;
    });
  }

  /* ---- the device */
  async function connect() {
    if (S.io) { disconnect(); return; }
    if (!navigator.requestMIDIAccess) { say("This browser has no Web MIDI: use Chrome or Edge, or the Sandbox", "bad"); return; }
    let m;
    try { m = await navigator.requestMIDIAccess({sysex: true}); }
    catch (e) { say("MIDI was not allowed: " + e.message, "bad"); return; }
    const outs = [...m.outputs.values()], ins = [...m.inputs.values()];
    const pick = list => list.find(p => A.portMatch.test(p.name)) || list[0];
    const out = pick(outs), inp = pick(ins);
    if (!out || !inp) { say("No MIDI device: is the " + A.deviceName + " plugged in?", "bad"); return; }
    const listeners = new Set();
    inp.onmidimessage = ev => listeners.forEach(f => f(ev.data));
    await attach({send: b => out.send(b), listen: f => { listeners.add(f); return () => listeners.delete(f); }, close: () => { inp.onmidimessage = null; }}, false, out.name);
  }
  function sandbox() {
    if (S.io) disconnect();
    attach(A.mock(), true, "the sandbox");
  }
  async function attach(io, sb, label) {
    S.io = io; S.sandbox = sb;
    say("Asking " + label + " who it is…");
    let id;
    try { id = await A.device.identify(io); }
    catch (e) { S.io = null; say(label + " does not answer: is " + A.name + " installed on it?", "bad"); render(); return; }
    if (!A.product.test(id.product)) {
      S.io = null;
      say("This page is for " + A.name + "; the device is running " + id.product + ". Use that firmware's own library page.", "bad");
      render();
      return;
    }
    S.ident = id;
    S.banks = new Map();
    say("Connected: " + id.product + " " + id.version + (sb ? " (sandbox: nothing real is written)" : ""));
    render();
    await readBank(S.devTab);
  }
  function disconnect() {
    try { S.io && S.io.close && S.io.close(); } catch (e) { /* */ }
    S.io = null; S.ident = null; S.sandbox = false;
    say("Disconnected");
    render();
  }
  async function readBank(b) {
    if (!S.io || S.busy) return;
    S.busy = true; render();
    const bd = bankDef(b);
    try {
      const slots = await A.device.read(S.io, b, bd.size, n => progress(n, bd.size, "Reading " + bd.name + ": " + n + " of " + bd.size));
      S.banks.set(b, slots);
      save();
      say("Read " + bd.name + ": " + slots.filter(Boolean).length + " patches");
    } catch (e) { say("Reading " + bd.name + " stopped: " + e.message, "bad"); }
    S.busy = false; progress(0, 0); render();
  }
  async function send() {
    if (!S.io) { say("Connect first (or use the Sandbox)", "bad"); return; }
    if (S.busy) return;
    const jobs = [];
    for (const [b, m] of S.pend.by) for (const [s, p] of m) jobs.push({b, s, p});
    if (!jobs.length) return;
    for (const b of new Set(jobs.map(j => j.b))) if (!S.banks.get(b)) await readBank(b);
    S.busy = true; render();
    jobs.sort((x, y) => x.b - y.b || x.s - y.s);
    let done = 0, bad = 0;
    for (const b of new Set(jobs.map(j => j.b))) if (!S.backup.has(b)) S.backup.set(b, devLoaded(b).slice());
    for (const j of jobs) {
      const bd = bankDef(j.b), where = bd.name + " " + FK.slotName(j.s);
      progress(done, jobs.length * 2, "Sending " + where + " (" + (done + 1) + " of " + jobs.length + ")");
      try {
        if (j.p) await A.device.write(S.io, j.b, j.s, j.p); else await A.device.clear(S.io, j.b, j.s);
      } catch (e) { bad++; say(where + ": " + e.message, "bad"); break; }
      done++;
    }
    /* read back what was sent, and compare */
    let ok = 0;
    for (const j of jobs.slice(0, done)) {
      progress(done + ok, jobs.length * 2, "Checking " + bankDef(j.b).name + " " + FK.slotName(j.s));
      try {
        const back = await A.device.readSlot(S.io, j.b, j.s);
        const loaded = devLoaded(j.b).slice();
        loaded[j.s] = back;
        S.banks.set(j.b, loaded);
        if (same(back, j.p)) { S.pend.of(j.b).delete(j.s); ok++; }
        else { bad++; say(bankDef(j.b).name + " " + FK.slotName(j.s) + " reads back different: still marked to send", "bad"); }
      } catch (e) { bad++; say("Checking stopped: " + e.message, "bad"); break; }
    }
    S.pend.undo = []; S.pend.redo = [];
    S.hist = S.hist.filter(h => h !== "dev");
    S.busy = false; progress(0, 0); save();
    if (!bad) say("Sent " + ok + ", every one acknowledged and read back the same" + (S.sandbox ? " (sandbox)" : ""));
    render();
  }
  function restore() {
    const b = S.devTab, old = S.backup.get(b);
    if (!old) return;
    const cur = devView(b), ch = new Map();
    old.forEach((p, s) => { if (!same(p, cur[s])) ch.set(s, p); });
    if (!ch.size) { say("Already as it was"); return; }
    changeDev(b, ch, "Put back " + bankDef(b).name + " as it was before this session's first send (" + ch.size + " slots): press Send to write it");
  }
  async function audition(p) {
    if (!S.io) { say("Connect to audition", "bad"); return; }
    if (S.busy) return;
    try { await A.device.audition(S.io, p); say("Playing " + A.nameOf(p) + " on the " + A.deviceName + " (not saved)"); }
    catch (e) { say("Audition: " + e.message, "bad"); }
  }

  /* ---- files out */
  function download(name, bytes) {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([bytes], {type: "application/zip"}));
    a.download = name;
    document.body.appendChild(a);
    a.click();
    setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
  }
  function metaFor(pats) { const m = {}; for (const p of pats) if (S.meta[fp(p)]) m[fp(p)] = S.meta[fp(p)]; return m; }
  /* a bank as a file (0.3.0: Save… on both panes): the adapter's own bank file when it gives one (0.2.0 bankFile:
   * slots with nulls in, {name, bytes, note?} or null), else a zip of one file a patch */
  function saveSlots(label, slots, note) {
    const files = [];
    slots.forEach((p, s) => { if (p) files.push({name: A.fileName(p, s), data: A.serialise(p)}); });
    if (!files.length) { say(label + " is empty", "bad"); return; }
    const one = A.bankFile && A.bankFile(slots), base = (A.id + "-" + label).replace(/[\\/:*?"<>|]/g, "_");
    if (one) download(base + "-" + one.name, one.bytes);
    else download(base + ".zip", FK.zip(files));
    say("Saved " + label + " (" + files.length + (files.length === 1 ? " patch" : " patches") + (note || "") + ")" + (one && one.note ? " · " + one.note : ""));
  }
  function downloadBank() {
    const bd = bankDef(S.devTab);
    if (!S.banks.get(S.devTab) && !S.pend.count(S.devTab)) { say(bd.name + " has not been read: Connect, then Read", "bad"); return; }
    saveSlots(bd.name, devView(S.devTab), S.pend.count(S.devTab) ? ", with the changes not yet sent" : "");
  }
  function saveSource() {
    if (S.srcTab === "all") { say("Pick one bank to save: All sources is a list, not a bank", "bad"); return; }
    const s = S.sources[S.srcTab];
    if (!s) { say("Nothing open to save", "bad"); return; }
    saveSlots(s.name, s.slots, "");
  }
  /* (0.3.0) + New bank: an empty bank of your own, the device's bank size, kept in this browser like any other */
  function newBank() {
    let name = "New bank", n = 1;
    while (S.sources.some(s => s.name === name)) name = "New bank " + (++n);
    S.sources.push({id: "s" + (++srcN) + "-" + Date.now(), name, kind: "new", slots: new Array(SIZE).fill(null), group: "My banks"});
    const k = S.sources.length - 1;
    touch(S.sources[k]); save();
    pickSource(k);
    say("Made " + name + ": drag patches onto its slots (from a bank above or from the " + A.deviceName + "); double-click its name to rename it");
  }
  function renameSource() {
    const s = S.srcTab === "all" ? null : S.sources[S.srcTab];
    if (!editable(s)) { if (s) say(s.name + " is built in: its name stays", "bad"); return; }
    const n = (prompt("Name this bank", s.name) || "").trim();
    if (!n || n === s.name) return;
    S.hist.push({src: s.id, name: s.name, slots: s.slots.slice()});
    if (s.group === s.name) s.group = n;
    s.name = n; touch(s); save(); render();
    say("Renamed to " + n + " · Ctrl+Z undoes");
  }
  function exportLibrary() {
    const files = [], lib = {app: A.id, made: new Date().toISOString(), sources: [], meta: {}};
    const used = new Set();
    /* (0.3.0) the device's banks as read (with the changes not yet sent) go in too, as "From <bank>" */
    const fromDev = A.deviceBanks.filter(b => S.banks.get(b.id)).map(b => ({name: "From " + b.name, group: A.deviceName, kind: "device", slots: devView(b.id)}));
    [...S.sources.filter(s => s.kind !== "factory"), ...fromDev].forEach(s => {
      let dir = ((s.group && s.group !== s.name ? s.group + " - " : "") + s.name).replace(/[\\/:*?"<>|]/g, "_");
      while (used.has(dir)) dir += "_";
      used.add(dir);
      const ent = {name: s.name, group: s.group, kind: s.kind, files: []};
      s.slots.forEach((p, i) => { if (!p) return; const f = dir + "/" + A.fileName(p, i); files.push({name: f, data: A.serialise(p)}); ent.files.push({slot: i, file: f}); });
      lib.sources.push(ent);
      Object.assign(lib.meta, metaFor(s.slots.filter(Boolean)));
    });
    if (!lib.sources.length) { say("Nothing to export yet: open patches or a bank of your own, or Read the " + A.deviceName, "bad"); return; }
    files.push({name: "library.json", data: JSON.stringify(lib, null, 1)});
    download(A.id + "-library.zip", FK.zip(files));
    say("Exported " + lib.sources.length + " sources; open the zip here again to bring them back");
  }

  /* ---- details: the patch in focus */
  function focused() {
    const pane = S.lastPane, slots = paneSlots(pane), i = pane === "src" ? S.focus.src : S.focus.dev;
    return {pane, i, p: slots[i] || null};
  }
  /* (0.3.0) the patch the details panel shows: the held one (Hold), else the one in focus. ref: putAt's slot */
  function resolve(ref) {
    if (!ref) return null;
    if (ref.dev != null) return devView(ref.dev)[ref.s] || null;
    const s = S.sources.find(x => x.id === ref.src);
    return s ? s.slots[ref.s] || null : null;
  }
  function shown() {
    if (S.pin) {
      const p = resolve(S.pin);
      if (p) return {ref: S.pin, p, where: refWhere(S.pin)};
      S.pin = null;                              /* (its bank was closed or the slot emptied) */
    }
    const {pane, i, p} = focused();
    return {ref: refOf(pane, i), p, where: pane === "src" && S.srcTab === "all" ? srcSlots().from[i] : refWhere(refOf(pane, i))};
  }
  function refWhere(ref) {
    if (!ref) return "";
    if (ref.dev != null) return bankDef(ref.dev).name + " " + FK.slotName(ref.s);
    const s = S.sources.find(x => x.id === ref.src);
    return (s ? s.name : "?") + " " + FK.slotName(ref.s);
  }
  function replaceShown(np, what) { const why = putAt(shown().ref, np, what); if (why) say(why, "bad"); }
  /* a slot, by what it is rather than what is shown now (0.2.0: an adapter's action puts a patch back later, after
   * the tabs may have changed): {dev: bank, s} or {src: source id, s}; null for the "All sources" list */
  function refOf(pane, i) {
    if (pane === "dev") return {dev: S.devTab, s: i};
    return S.srcTab === "all" || !S.sources[S.srcTab] ? null : {src: S.sources[S.srcTab].id, s: i};
  }
  /* put np into that slot as an ordinary change (a device slot: pending until Send, undoable). The reason when not */
  function putAt(ref, np, what) {
    if (!ref) return "that patch was picked in All sources: pick it from its own bank";
    if (ref.dev != null) {
      if (bankDef(ref.dev).readOnly) return bankDef(ref.dev).name + " is read only";
      changeDev(ref.dev, new Map([[ref.s, np]]), what || "Put " + A.nameOf(np) + " into " + bankDef(ref.dev).name + " " + FK.slotName(ref.s));
      return "";
    }
    const src = S.sources.find(x => x.id === ref.src);
    if (!src) return "that bank was closed";
    if (!editable(src)) return src.name + " is built in (read only): drag the patch into a bank of your own first";
    changeSrc(src, new Map([[ref.s, np]]), what || "Put " + A.nameOf(np) + " into " + src.name + " " + FK.slotName(ref.s));
    return "";
  }
  FK.lib = {putAt, say: (t, k) => say(t, k)};
  /* (for the tests, 0.3.0) */
  FK.lib._t = {newBank, saveSlots, dropIntoSource, changeSrc, clearSource, undo, putAt, editable, renameSource, exportLibrary, saveSource, shown};
  function setMeta(p, fn) {
    const k = fp(p), m = S.meta[k] || {cat: catOf(p), tags: tagsOf(p).slice()};
    fn(m);
    S.meta[k] = m;
    save(); render();
  }
  function renderDetails() {
    const d = $("#details");
    d.hidden = !S.details;
    $("#details-btn").setAttribute("aria-pressed", S.details ? "true" : "false");
    if (!S.details) return;
    const {ref, p, where} = shown();
    $("#d-empty").hidden = !!p;
    $("#d-body").hidden = !p;
    $("#d-hold").hidden = !p;
    $("#d-hold").setAttribute("aria-pressed", S.pin ? "true" : "false");
    if (!p) return;
    const nm = $("#d-name");
    if (document.activeElement !== nm) nm.value = A.nameOf(p);
    nm.maxLength = A.nameLength;
    $("#d-where").textContent = where + (A.describe ? " · " + A.describe(p) : "") + (S.pin ? " · held" : "");
    const cs = $("#d-colours");
    cs.replaceChildren();
    cs.hidden = cs.previousElementSibling.hidden = A.colours.length < 2;   /* (one colour: nothing to pick) */
    for (const c of A.colours) {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "sw" + (A.colourOf(p) === c ? " on" : "");
      b.style.background = "var(--pc-" + c + ")";
      b.setAttribute("aria-label", "Colour " + c);
      b.title = c;
      b.onclick = () => replaceShown(A.withColour(p, c));
      cs.appendChild(b);
    }
    const cat = $("#d-cat");
    if (cat.options.length !== FK.CATS.length) { cat.replaceChildren(...FK.CATS.map(c => new Option(c, c))); }
    cat.value = catOf(p);
    const tg = $("#d-tags");
    tg.replaceChildren();
    for (const t of tagsOf(p)) {
      const b = document.createElement("button");
      b.type = "button"; b.className = "chip"; b.textContent = t + " ×"; b.setAttribute("aria-label", "Remove tag " + t);
      b.onclick = () => setMeta(p, m => { m.tags = m.tags.filter(x => x !== t); });
      tg.appendChild(b);
    }
    const dups = (dupMap.get(sk(p)) || []).filter(w => w !== where);
    $("#d-dups").textContent = dups.length ? "Same sound in " + dups.slice(0, 4).join(", ") + (dups.length > 4 ? " and " + (dups.length - 4) + " more" : "") : "";
    const pv = A.preview(p);
    $("#d-facts").replaceChildren(...pv.facts.map(([k, v]) => { const r = document.createElement("div"); r.innerHTML = "<span></span><b></b>"; r.firstChild.textContent = k; r.lastChild.textContent = v; return r; }));
    $("#d-filter-cap").textContent = "FILTER · " + pv.filter.label;
    drawFilter($("#d-filter"), pv.filter);
    drawEnv($("#d-env"), pv.env);
    renderLayers(ref, p);
  }
  /* (0.3.0) Layers, when the adapter has them (A.layers, adapters/README.md; M-vance: UPPER / LOWER): the key mode
   * (and what goes with it), each layer's name, ▶ to hear it alone, a drop target (a patch dropped there gives its
   * first layer, with Alt its second), Swap and Copy. Every change is an ordinary one through putAt: pending on the
   * device until Send, undoable, refused on a built-in bank */
  function renderLayers(ref, p) {
    const box = $("#d-layers"), L = A.layers;
    box.hidden = !L;
    if (!L) return;
    box.replaceChildren();
    const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
    const put = (np, what) => { const why = putAt(ref, np, what); if (why) say(why, "bad"); };
    box.appendChild(el("span", "lab", "Layers"));
    const head = el("div", "lay-head");
    const ms = el("select");
    ms.setAttribute("aria-label", L.modeLabel);
    L.modes.forEach((m, k) => ms.add(new Option(m, k)));
    ms.value = String(L.mode(p));
    ms.onchange = () => put(L.withMode(p, +ms.value), L.modeLabel + " " + L.modes[+ms.value] + " in " + refWhere(ref));
    head.append(el("span", "k", L.modeLabel), ms);
    for (const q of L.params || []) {
      if (q.show && !q.show(p)) continue;
      const inp = el("input");
      inp.type = "number"; inp.min = q.min; inp.max = q.max; inp.value = q.get(p);
      inp.setAttribute("aria-label", q.label);
      const shownV = el("span", "v", q.fmt ? q.fmt(q.get(p)) : "");
      inp.onchange = () => { const v = Math.max(q.min, Math.min(q.max, Math.round(+inp.value || 0))); put(q.set(p, v), q.label + " " + (q.fmt ? q.fmt(v) : v) + " in " + refWhere(ref)); };
      head.append(el("span", "k", q.label), inp, shownV);
    }
    box.appendChild(head);
    L.names.forEach((nm, k) => {
      const row = el("div", "lay" + (L.heard && !L.heard(p, k) ? " silent" : ""));
      row.append(el("span", "k", nm), el("span", "nm", L.nameOf(p, k)));
      if (L.heard && !L.heard(p, k)) row.title = nm + " is not heard in " + L.modes[L.mode(p)];
      const play = el("button", null, "▶");
      play.type = "button"; play.title = "Hear " + nm + " alone (nothing saved)"; play.setAttribute("aria-label", play.title);
      play.onclick = () => audition(L.solo(p, k));
      row.appendChild(play);
      row.ondragover = e => { if (!S.drag) return; e.preventDefault(); e.dataTransfer.dropEffect = "copy"; row.classList.add("over");
        say("Drop: " + nm + " takes the patch's " + L.names[e.altKey ? 1 : 0] + (e.altKey ? "" : " (hold Alt for its " + L.names[1] + ")")); };
      row.ondragleave = () => row.classList.remove("over");
      row.ondrop = e => {
        e.preventDefault(); e.stopPropagation(); row.classList.remove("over");
        const d = S.drag, from = d && d.slots[d.idx[0]];
        S.drag = null;
        if (!from) return;
        const fk = e.altKey ? 1 : 0;
        put(L.take(p, k, from, fk), nm + " of " + refWhere(ref) + " from " + A.nameOf(from) + (fk ? " (its " + L.names[1] + ")" : "") +
          (L.heard && !L.heard(p, k) ? " · " + nm + " is heard in " + L.modes.filter((_, m) => m).join(" or ") : ""));
      };
      box.appendChild(row);
    });
    const bar = el("div", "bar");
    const b1 = el("button", null, "Swap ⇅"), b2 = el("button", null, "Copy " + L.names[0] + " → " + L.names[1]);
    b1.type = b2.type = "button";
    b1.onclick = () => put(L.swap(p), "Swapped " + L.names.join(" and ") + " in " + refWhere(ref));
    b2.onclick = () => put(L.copy(p, 0, 1), "Copied " + L.names[0] + " to " + L.names[1] + " in " + refWhere(ref));
    bar.append(b1, b2);
    box.appendChild(bar);
  }
  function tok(n) { return getComputedStyle(document.documentElement).getPropertyValue("--" + n).trim(); }
  function prepCanvas(c) {
    const r = window.devicePixelRatio || 1, w = c.clientWidth || 220, h = c.clientHeight || 80;
    c.width = w * r; c.height = h * r;
    const g = c.getContext("2d");
    g.setTransform(r, 0, 0, r, 0, 0);
    g.fillStyle = tok("surf"); g.fillRect(0, 0, w, h);
    g.strokeStyle = tok("grid"); g.lineWidth = 1;
    for (let k = 1; k < 4; k++) { g.beginPath(); g.moveTo(0, Math.round(h * k / 4) + 0.5); g.lineTo(w, Math.round(h * k / 4) + 0.5); g.stroke(); }
    return {g, w, h};
  }
  function strokeFill(g, pts, h) {
    g.beginPath(); pts.forEach(([x, y], k) => k ? g.lineTo(x, y) : g.moveTo(x, y));
    g.lineTo(pts[pts.length - 1][0], h); g.lineTo(pts[0][0], h); g.closePath();
    g.fillStyle = tok("fill"); g.fill();
    g.beginPath(); pts.forEach(([x, y], k) => k ? g.lineTo(x, y) : g.moveTo(x, y));
    g.strokeStyle = tok("theme"); g.lineWidth = 2; g.stroke();
  }
  function drawFilter(c, f) {
    const {g, w, h} = prepCanvas(c), pts = [];
    const q = 0.707 * Math.pow(2, f.res * 4.2), k = 3.7 * f.res;
    for (let x = 0; x <= w; x += 2) {
      const wr = Math.pow(2, (x - f.freq * w) / (w / 10)), s = {re: 0, im: wr};
      const den2 = (re, im) => [1 - wr * wr + re, wr / q + im];
      let mag;
      const [dr, di] = den2(0, 0), d = Math.hypot(dr, di);
      if (f.kind === "lp" && f.order === 4) { const g1 = 1 / Math.hypot(1, wr), ph = 4 * Math.atan(wr), gr = Math.pow(g1, 4); mag = gr * (1 + k) / Math.hypot(1 + k * gr * Math.cos(ph), -k * gr * Math.sin(ph)); }
      else if (f.kind === "hp") mag = wr * wr / d;
      else if (f.kind === "bp") mag = (wr / q) / d;
      else mag = 1 / d;
      void s;
      const db = Math.max(-60, Math.min(24, 20 * Math.log10(mag)));
      pts.push([x, h * 0.3 - db * h / 90]);
    }
    strokeFill(g, pts.map(([x, y]) => [x, Math.max(1, Math.min(h - 1, y))]), h);
    g.strokeStyle = tok("dim"); g.setLineDash([2, 3]); g.beginPath(); g.moveTo(f.freq * w + 0.5, 0); g.lineTo(f.freq * w + 0.5, h); g.stroke(); g.setLineDash([]);
  }
  function drawEnv(c, e) {
    const {g, w, h} = prepCanvas(c), top = 6, bot = h - 4;
    const A_ = 4 + e.a * w * 0.22, D = 6 + e.d * w * 0.24, Sh = w * 0.2, R = 6 + e.r * w * 0.24, sy = bot - (bot - top) * e.s;
    const pts = [[0, bot], [A_, top]];
    for (let k = 1; k <= 16; k++) pts.push([A_ + D * k / 16, sy + (top - sy) * Math.exp(-4.2 * k / 16)]);
    pts.push([A_ + D + Sh, sy]);
    for (let k = 1; k <= 16; k++) pts.push([A_ + D + Sh + R * k / 16, bot - (bot - sy) * Math.exp(-4.2 * k / 16)]);
    pts.push([w, bot]);
    strokeFill(g, pts, h);
  }

  /* ---- wiring */
  function init() {
    document.title = A.name + " library";
    $("#app-name").textContent = A.name;
    $("#dev-label").textContent = A.deviceName;
    const themes = ["sea", "ember", "ice", "orchid", "crt", "paper", "amber"];   /* (amber: M-vance's screen, 0.2.0) */
    const th = $("#theme");
    th.replaceChildren(...themes.map(t => new Option(t.toUpperCase(), t)));
    th.value = themes.includes(pref("theme")) ? pref("theme") : (themes.includes(A.theme) ? A.theme : "sea");
    document.documentElement.dataset.theme = th.value;
    th.onchange = () => { document.documentElement.dataset.theme = th.value; pref("theme", th.value); render(); };
    const cats = $("#cats");
    for (const c of ["", ...FK.CATS]) {
      const b = document.createElement("button");
      b.type = "button"; b.dataset.cat = c; b.textContent = c || "ALL";
      b.onclick = () => { S.catChip = c; const q = $("#search").value.replace(/\bcat:\S+/gi, "").trim(); $("#search").value = (q + (c ? " cat:" + c.toLowerCase() : "")).trim(); S.query = FK.parseQuery($("#search").value); render(); };
      cats.appendChild(b);
    }
    let qt = 0;
    $("#search").addEventListener("input", e => {
      clearTimeout(qt);
      qt = setTimeout(() => { S.query = FK.parseQuery(e.target.value); const m = e.target.value.match(/\bcat:(\S+)/i); S.catChip = m ? (FK.CATS.find(c => c.toLowerCase().startsWith(m[1].toLowerCase())) || "") : ""; render(); }, 120);
    });
    $("#open-files").onclick = () => $("#file-in").click();
    $("#open-folder").onclick = () => $("#folder-in").click();
    $("#file-in").accept = [...A.fileTypes, ".zip"].join(",");
    $("#file-in").onchange = async e => { await readEntries(await filesFromInput(e.target.files)); e.target.value = ""; };
    $("#folder-in").onchange = async e => { await readEntries(await filesFromInput(e.target.files)); e.target.value = ""; };
    $("#export").onclick = exportLibrary;
    $("#src-save").onclick = saveSource;
    $("#src-new").onclick = newBank;
    $("#src-pick").ondblclick = e => { e.preventDefault(); openMenu(false); renameSource(); };
    $("#d-hold").onclick = () => { S.pin = S.pin ? null : shown().ref; render(); };   /* (0.3.0) Hold: keep this patch here */
    $("#src-pick").onclick = () => openMenu(!S.menu.open);
    $("#src-prev").onclick = () => { if (S.srcTab > 0) pickSource(S.srcTab - 1); };
    $("#src-next").onclick = () => { if (S.srcTab < S.sources.length - 1) pickSource(S.srcTab + 1); };
    $("#src-all").onclick = () => {
      if (S.srcTab === "all") pickSource(S.lastBank != null && S.sources[S.lastBank] ? S.lastBank : 0);
      else { S.lastBank = S.srcTab; S.srcTab = "all"; S.sel = {pane: null, set: new Set(), anchor: -1}; openMenu(false); render(); }
    };
    $("#src-filter").addEventListener("input", e => { S.menu.filter = e.target.value; S.menu.active = 0; S.menu.confirm = ""; renderMenu(); });
    $("#src-filter").addEventListener("keydown", e => {
      const opts = menuRows().rows.filter(r => r.s);
      if (e.key === "ArrowDown") { e.preventDefault(); S.menu.active = Math.min(opts.length - 1, S.menu.active + 1); renderMenu(); }
      else if (e.key === "ArrowUp") { e.preventDefault(); S.menu.active = Math.max(0, S.menu.active - 1); renderMenu(); }
      else if (e.key === "PageDown") { e.preventDefault(); S.menu.active = Math.min(opts.length - 1, S.menu.active + 12); renderMenu(); }
      else if (e.key === "PageUp") { e.preventDefault(); S.menu.active = Math.max(0, S.menu.active - 12); renderMenu(); }
      else if (e.key === "Enter") { e.preventDefault(); if (opts[S.menu.active]) pickSource(opts[S.menu.active].k); }
      else if (e.key === "Escape") { e.preventDefault(); openMenu(false); $("#src-pick").focus(); }
    });
    $("#src-closeall").onclick = e => {
      e.stopPropagation();
      if (S.menu.confirm !== "all") { S.menu.confirm = "all"; renderMenu(); return; }
      closeSources(() => true);
    };
    document.addEventListener("pointerdown", e => {
      if (S.menu.open && !e.target.closest("#src-menu, #src-pick")) openMenu(false);
    });
    $("#conn").onclick = connect;
    $("#sandbox").onclick = sandbox;
    $("#read").onclick = () => readBank(S.devTab);
    $("#send").onclick = send;
    $("#download").onclick = downloadBank;
    $("#restore").onclick = restore;
    $("#undo").onclick = undo;
    $("#empty-only").onchange = e => { S.emptyOnly = e.target.checked; pref("emptyOnly", S.emptyOnly ? "1" : ""); };
    $("#details-btn").onclick = () => { S.details = !S.details; render(); };
    $("#d-close").onclick = () => { S.details = false; render(); };
    $("#d-name").addEventListener("change", e => {
      const {p} = shown();
      if (!p) return;
      const name = e.target.value.replace(/[^\x20-\x7e]/g, "?").slice(0, A.nameLength);
      const np = A.withName(p, name), old = S.meta[fp(p)];
      if (old) S.meta[fp(np)] = old;
      replaceShown(np, "Renamed to " + name.trim());
    });
    $("#d-cat").onchange = e => { const {p} = shown(); if (p) setMeta(p, m => { m.cat = e.target.value; }); };
    $("#d-tag-in").addEventListener("keydown", e => {
      if (e.key !== "Enter") return;
      const t = e.target.value.trim().toLowerCase().replace(/\s+/g, "-");
      const {p} = shown();
      if (t && p) setMeta(p, m => { if (!m.tags.includes(t)) m.tags.push(t); });
      e.target.value = "";
    });
    $("#d-play").onclick = () => { const {p} = shown(); if (p) audition(p); };
    for (const act of A.actions || []) {         /* the adapter's buttons (0.2.0): run(patch, ref, where) */
      const b = document.createElement("button");
      b.type = "button"; b.textContent = act.label; if (act.title) b.title = act.title;
      b.onclick = () => { const {ref, p} = shown(); if (p) act.run(p, ref, $("#d-where").textContent); };
      $("#d-play").parentNode.appendChild(b);
    }
    bindGrid($("#src-grid"), "src");
    bindGrid($("#dev-grid"), "dev");
    bindDeviceDrop($("#dev-grid"));
    bindSourceDrop($("#src-pane"));
    /* the splitter */
    const sp = $("#split"), top = $("#src-pane");
    const h0 = +pref("split");
    if (h0 > 80) top.style.flexBasis = h0 + "px";
    sp.addEventListener("pointerdown", e => {
      sp.setPointerCapture(e.pointerId);
      const y0 = e.clientY, b0 = top.getBoundingClientRect().height;
      const mv = ev => { top.style.flexBasis = Math.max(80, b0 + ev.clientY - y0) + "px"; };
      sp.addEventListener("pointermove", mv);
      sp.addEventListener("pointerup", () => { sp.removeEventListener("pointermove", mv); pref("split", Math.round(top.getBoundingClientRect().height)); render(); }, {once: true});
    });
    sp.addEventListener("keydown", e => {
      if (e.key !== "ArrowUp" && e.key !== "ArrowDown") return;
      e.preventDefault();
      top.style.flexBasis = Math.max(80, top.getBoundingClientRect().height + (e.key === "ArrowUp" ? -24 : 24)) + "px";
      render();
    });
    let rt = 0;
    new ResizeObserver(() => { clearTimeout(rt); rt = setTimeout(render, 60); }).observe(document.body);
    document.addEventListener("keydown", e => {
      if (e.target.closest("input, select, textarea")) return;
      if ((e.ctrlKey || e.metaKey) && (e.key === "z" || e.key === "Z") && !e.target.closest(".grid")) { e.preventDefault(); if (e.shiftKey) redo(); else undo(); }
      if (e.key === "/") { e.preventDefault(); $("#search").focus(); }
      if (e.key === "Escape" && S.menu.open) openMenu(false);
    });
    window.addEventListener("beforeunload", e => { flush(); if (S.pend.total()) { e.preventDefault(); e.returnValue = ""; } });
    window.addEventListener("pagehide", flush);
    document.addEventListener("visibilitychange", () => { if (document.visibilityState === "hidden") flush(); });
    S.emptyOnly = pref("emptyOnly") === "1";
  }

  async function start() {
    init();
    db = await idb();
    for (const f of A.factory()) addSource(f.name, "factory", f.slots, f.slots.map((_, i) => i), "Built in");
    const idx = await kvGet("srcIndex");
    if (Array.isArray(idx)) {
      const got = await Promise.all(idx.map(id => kvGet("src:" + id)));
      for (const s of got) if (s && s.slots) { S.sources.push(tidySource(s)); written.add(s.id); srcN++; }
    } else {                                     /* (a library saved before the key a source) */
      const old = await kvGet("sources");
      if (Array.isArray(old)) { for (const s of old) { S.sources.push(tidySource(s)); srcN++; } kvDel("sources"); save(); }
    }
    const meta = await kvGet("meta");
    if (meta && typeof meta === "object") S.meta = meta;
    const dev = await kvGet("device");
    if (dev && typeof dev === "object") for (const k of Object.keys(dev)) S.banks.set(+k, dev[k]);
    say("Open patches above, connect the " + A.deviceName + " (or try the Sandbox), then drag patches down into a bank");
    render();
  }
  FK.start = start;
  FK._state = S;                                 /* (for the tests) */
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start); else start();
})(window);
