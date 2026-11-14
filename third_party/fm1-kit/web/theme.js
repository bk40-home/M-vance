// SPDX-License-Identifier: GPL-3.0-only
// fm1-kit: the theme of a page (theme.css), remembered for the site ("fm1-theme": the simulator, the editor and the
// library on one site share it). Fills and wires every <select data-theme-picker>. A plain script, run in <head>.
(function () {
  "use strict";
  var THEMES = ["sea", "ember", "ice", "orchid", "crt", "paper"];
  function get() { try { var t = localStorage.getItem("fm1-theme"); return THEMES.indexOf(t) >= 0 ? t : "sea"; } catch (e) { return "sea"; } }
  function set(t) {
    document.documentElement.dataset.theme = t;
    try { localStorage.setItem("fm1-theme", t); } catch (e) { /* (storage off) */ }
    document.querySelectorAll("select[data-theme-picker]").forEach(function (s) { s.value = t; });
    window.dispatchEvent(new CustomEvent("fm1-theme", {detail: t}));
  }
  document.documentElement.dataset.theme = get();
  function wire() {
    document.querySelectorAll("select[data-theme-picker]").forEach(function (s) {
      if (!s.options.length) THEMES.forEach(function (t) { s.add(new Option(t.toUpperCase(), t)); });
      s.value = get();
      s.addEventListener("change", function () { set(s.value); });
    });
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", wire); else wire();
  window.fm1Theme = {get: get, set: set, list: THEMES};
})();
