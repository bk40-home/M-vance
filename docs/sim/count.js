// SPDX-License-Identifier: GPL-3.0-only
// fm1-kit: count page views and a few events with GoatCounter (goatcounter.com: open source, no cookies, nothing
// personal kept). No outside script is loaded: one 1x1 image request to the site's own counter per view or event.
//   <meta name="fm1-count" content="https://CODE.goatcounter.com/count">   (the firmware's Pages workflow fills it)
//   <script src="count.js"></script>                                        (a plain script, anywhere)
//   fm1Count("flash/done/FM-1_968091")                                     (an event: a name, no leading slash)
// Counts nothing when the meta is missing or not a URL (a local copy), on localhost or a file, in an automated
// browser, or when the browser sends Do Not Track or Global Privacy Control.
(function () {
  "use strict";
  function endpoint() {
    var m = document.querySelector('meta[name="fm1-count"]'), u = m && m.content;
    return u && /^https:\/\/[^/]+\/count$/.test(u) ? u : null;
  }
  function off() {
    var h = location.hostname, n = navigator;
    return location.protocol === "file:" || h === "localhost" || /^127\.|^\[?::1\]?$/.test(h) || !h ||
      n.webdriver || n.doNotTrack === "1" || window.doNotTrack === "1" || n.globalPrivacyControl === true;
  }
  function send(params) {
    var url = endpoint();
    if (!url || off()) return false;
    params.rnd = Math.random().toString(36).slice(2);
    var q = Object.keys(params).map(function (k) { return k + "=" + encodeURIComponent(params[k]); }).join("&");
    var img = new Image(); img.src = url + "?" + q;            // GoatCounter's /count answers a 1x1 GIF
    return true;
  }
  function view() {
    var r = document.referrer;
    send({p: location.pathname, t: document.title, r: r && r.indexOf(location.origin) !== 0 ? r : "",
      s: [screen.width, screen.height, window.devicePixelRatio || 1].join(","), q: location.search});
  }
  window.fm1Count = function (name) { return send({p: String(name).replace(/^\/+/, ""), t: document.title, e: "true"}); };
  if (document.readyState === "complete") view(); else window.addEventListener("load", view);
})();
