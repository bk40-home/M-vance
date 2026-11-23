#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The firmware page: flash/index.html and fm1-kit's flash.js, fm1ota.js, count.js, flash.css, theme.css, theme.js into
# docs/flash/. No compiler needed. The packages come from the latest GitHub release (the Pages workflow); to try the
# page here, put a package and its manifest beside it:
#   sh flash/build.sh && python3 tools/flash_manifest.py --version dev build/jt80x0.fwsc
#   python3 -m http.server --directory docs 8000        # then http://localhost:8000/flash/ in Chrome or Edge
set -e
cd "$(dirname "$0")/.."
OUT=docs/flash
KIT=third_party/fm1-kit/web
mkdir -p "$OUT"
cp flash/index.html "$KIT/flash.js" "$KIT/fm1ota.js" "$KIT/count.js" "$KIT/flash.css" "$KIT/theme.css" "$KIT/theme.js" "$OUT"/
BUILD=$(git rev-parse --short HEAD 2>/dev/null || date +%Y%m%d%H%M%S)     # ?v=<stamp>: no stale cached script
sed -i "s/__BUILD__/$BUILD/g" "$OUT/index.html"
echo "built -> $OUT/"
