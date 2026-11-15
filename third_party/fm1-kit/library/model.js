/* SPDX-License-Identifier: GPL-3.0-only
 * fm1-kit library: the model. Pure logic, no DOM: what a drop fills, moves and copies between slots, undo, search,
 * categories. The page (core.js) and the tests (test/model_test.js, in Node) both run it.
 *
 * A bank is an array of slots, each a patch or null. A firmware's patches are the adapter's own objects; the model
 * only asks whether a slot holds one. */
(function (G) {
  "use strict";
  const FK = G.FK = G.FK || {};

  /* ---- categories: one each, guessed from the name, always editable (kept in the library, not on the device) */
  const CATS = ["BASS", "LEAD", "PAD", "KEYS", "PLUCK", "BRASS", "STRINGS", "FX", "SEQ", "DRUM", "OTHER"];
  const GUESS = [
    [/^(bass|bs|bas)\b|\b(bass|sub ?bass|808|reese)\b/i, "BASS"],
    [/^(lead|ld)\b|\blead\b/i, "LEAD"],
    [/^(pad|pd)\b|\bpad\b/i, "PAD"],
    [/^(keys?|ky|ep|org|pno|clav)\b|\b(piano|organ|e\.?piano|clav|rhodes|keys)\b/i, "KEYS"],
    [/^(pluck|plk|pl)\b|\b(pluck|harp|koto)\b/i, "PLUCK"],
    [/^(brass|brs|br)\b|\bbrass\b/i, "BRASS"],
    [/^(str|strings?|st)\b|\b(strings?|solina|ensemble)\b/i, "STRINGS"],
    [/^(fx|sfx)\b|\b(fx|sweep|riser|laser|noise)\b/i, "FX"],
    [/^(seq|arp|sq)\b|\b(arp|seq|sequence)\b/i, "SEQ"],
    [/^(drum|drm|dr|perc|kick|bd|sd|hh)\b|\b(kick|snare|hat|tom|perc|drum)\b/i, "DRUM"],
  ];
  FK.CATS = CATS;
  FK.guessCategory = function (name) {
    const n = String(name || "").trim();
    for (const [re, c] of GUESS) if (re.test(n)) return c;
    return "OTHER";
  };

  /* FNV-1a, 32 bits, as hex: a patch's fingerprint (the adapter says what goes in it) */
  FK.hash = function (s) {
    let h = 0x811c9dc5;
    for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 0x01000193) >>> 0; }
    return h.toString(16).padStart(8, "0");
  };

  /* ---- drops. view: the bank as it shows (loaded + pending). Returns the slots the items will take, how many of
   * them hold a patch now, and how many items do not fit. emptyOnly: skip filled slots instead of replacing them */
  FK.planDrop = function (view, start, count, emptyOnly) {
    const slots = [];
    let i = start, replaces = 0;
    for (let k = 0; k < count; k++) {
      if (emptyOnly) while (i < view.length && view[i]) i++;
      if (i >= view.length) break;
      if (view[i]) replaces++;
      slots.push(i);
      i++;
    }
    return {slots, replaces, overflow: count - slots.length};
  };

  /* a move inside one bank: from (sorted slots) to start. One slot onto a filled slot swaps them; a block is laid
   * from start on, its old places emptied unless copy. Returns the changes, slot -> patch | null */
  FK.planMove = function (view, from, start, copy, emptyOnly) {
    const ch = new Map();
    if (from.length === 1 && !copy && !emptyOnly) {
      const a = from[0];
      if (a === start) return ch;
      ch.set(start, view[a] || null);
      ch.set(a, view[start] || null);
      return ch;
    }
    const items = from.map(i => view[i] || null);
    const work = view.slice();
    if (!copy) for (const i of from) work[i] = null;      /* (their places are free for the block itself) */
    const plan = FK.planDrop(work, start, items.length, emptyOnly);
    if (!copy) for (const i of from) ch.set(i, null);
    plan.slots.forEach((s, k) => ch.set(s, items[k]));
    for (const [s, p] of [...ch]) if ((view[s] || null) === p) ch.delete(s);   /* nothing moved there */
    return ch;
  };

  /* ---- pending changes on the device's banks, with undo */
  FK.Pending = class {
    constructor() { this.by = new Map(); this.undo = []; this.redo = []; }
    of(bank) { if (!this.by.has(bank)) this.by.set(bank, new Map()); return this.by.get(bank); }
    view(bank, loaded) {
      const v = loaded.slice(), p = this.by.get(bank);
      if (p) for (const [s, x] of p) v[s] = x;
      return v;
    }
    /* changes: slot -> patch | null. loaded: the bank as read (a change back to it is no change) */
    apply(bank, changes, loaded, same) {
      const m = this.of(bank);
      this.undo.push({bank, before: new Map(m)});
      this.redo = [];
      for (const [s, x] of changes) {
        const orig = loaded[s] || null;
        if (same(orig, x)) m.delete(s); else m.set(s, x);
      }
    }
    back() {
      const u = this.undo.pop();
      if (!u) return null;
      this.redo.push({bank: u.bank, before: new Map(this.of(u.bank))});
      this.by.set(u.bank, u.before);
      return u.bank;
    }
    forward() {
      const r = this.redo.pop();
      if (!r) return null;
      this.undo.push({bank: r.bank, before: new Map(this.of(r.bank))});
      this.by.set(r.bank, r.before);
      return r.bank;
    }
    count(bank) { const m = this.by.get(bank); return m ? m.size : 0; }
    total() { let n = 0; for (const m of this.by.values()) n += m.size; return n; }
    clear(bank) { this.by.delete(bank); }
  };

  /* ---- search. "pad warm cat:lead colour:red src:factory tag:dark bank:U1 dup" ; quotes keep spaces */
  FK.parseQuery = function (q) {
    const f = {text: [], cat: [], colour: [], src: [], tag: [], bank: [], dup: false};
    const toks = String(q || "").match(/(\w+:)?"[^"]*"|\S+/g) || [];
    for (let t of toks) {
      const m = t.match(/^(\w+):(.*)$/);
      let key = m ? m[1].toLowerCase() : "", val = (m ? m[2] : t).replace(/^"|"$/g, "").toLowerCase();
      if (key === "color") key = "colour";
      if (key === "category") key = "cat";
      if (!m && val === "dup") { f.dup = true; continue; }
      if (key && f[key] && Array.isArray(f[key])) { if (val) f[key].push(val); }
      else if (val) f.text.push(m ? t.toLowerCase() : val);
    }
    f.empty = !f.text.length && !f.cat.length && !f.colour.length && !f.src.length && !f.tag.length && !f.bank.length && !f.dup;
    return f;
  };
  /* e: {name, cat, colour, tags[], src, bank, dup} */
  FK.matches = function (e, f) {
    if (f.empty) return true;
    const name = e.name.toLowerCase(), tags = (e.tags || []).map(t => t.toLowerCase());
    const cat = (e.cat || "").toLowerCase(), src = (e.src || "").toLowerCase(), bank = (e.bank || "").toLowerCase();
    const any = (arr, fn) => arr.some(fn);
    if (f.cat.length && !any(f.cat, c => cat.startsWith(c))) return false;
    if (f.colour.length && !any(f.colour, c => (e.colour || "").toLowerCase() === c)) return false;
    if (f.src.length && !any(f.src, s => src.includes(s))) return false;
    if (f.tag.length && !f.tag.every(t => tags.includes(t))) return false;
    if (f.bank.length && !any(f.bank, b => bank === b)) return false;
    if (f.dup && !e.dup) return false;
    return f.text.every(w => name.includes(w) || tags.some(t => t.includes(w)) || cat === w || src.includes(w));
  };

  /* ---- slots in a bank from file names: "012.sy1", "012_Lead.sy1" go to slot 12; the rest fill the gaps in order */
  FK.placeByNumber = function (names, size) {
    const at = new Array(names.length).fill(-1), used = new Set();
    names.forEach((n, i) => {
      const m = String(n).match(/^(\d{1,3})(?!\d)/);
      if (m) { const s = +m[1] - 1; if (s >= 0 && s < size && !used.has(s)) { at[i] = s; used.add(s); } }
    });
    let free = 0;
    names.forEach((n, i) => {
      if (at[i] >= 0) return;
      while (used.has(free)) free++;
      at[i] = free; used.add(free); free++;
    });
    return at;   /* may run past size: the caller splits into more banks */
  };

  /* slot labels: 1-based, three digits */
  FK.slotName = s => String(s + 1).padStart(3, "0");
})(typeof window !== "undefined" ? window : globalThis);
