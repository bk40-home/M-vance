#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1-kit: build a firmware's library page as ONE self-contained HTML file (it loads nothing from anywhere, so it
works from any host and offline).

    from build_library import build
    build(out_path, data={...}, adapter_js=[paths], stamp="M-Vsynth2 0.9")

The page = library/shell.html with library/ui.css, then the scripts in this order: the data (window.FK_DATA),
model.js, zip.js, the firmware's adapter files (they set FK.adapter, and FK.mockDevice for the Sandbox), core.js.
It fails if the page would reach out (an http(s) src, href, url() or @import)."""
import json
import re
from pathlib import Path

KIT = Path(__file__).resolve().parents[1]
LIB = KIT / "library"
VERSION = (KIT / "VERSION").read_text().strip() if (KIT / "VERSION").exists() else "0"


def build(out, data, adapter_js, stamp=""):
    shell = (LIB / "shell.html").read_text()
    css = (LIB / "ui.css").read_text()
    parts = ["window.FK_DATA = " + json.dumps(data, separators=(",", ":")) + ";"]
    for f in [LIB / "model.js", LIB / "zip.js", *map(Path, adapter_js), LIB / "core.js"]:
        parts.append("/* ---- " + f.name + " */\n" + f.read_text())
    js = "\n".join(parts).replace("</script", "<\\/script")
    html = (shell.replace("__CSS__", css).replace("__JS__", js)
            .replace("__STAMP__", ("fm1-kit " + VERSION + (", " + stamp if stamp else "")).replace("--", "-")))
    bad = re.findall(r'''(?:\b(?:src|href)\s*=\s*["']?\s*https?:|url\(\s*["']?\s*https?:|@import)''', html)
    if bad:
        raise SystemExit(f"build_library: the page would load from outside: {bad[:3]}")
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    Path(out).write_text(html)
    return len(html)
