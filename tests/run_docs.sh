#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The web page's generated parts, from the firmware itself:
#   docs/cheatsheet.html   the cheat sheet (tools/gen_cheatsheet.py) from the panel's page table (mv_ui_dump)
#   docs/img/*.png         the screens (tests/scripts/web.txt on the host build)
#   APP_VER=0.1 sh tests/run_docs.sh       (the version printed on the sheet; default: dev)
# Fails when a page's knob has no line in the cheat sheet (so a new page cannot ship undocumented).
set -e
cd "$(dirname "$0")/.."
APP_VER=${APP_VER:-dev} sh tests/run_host.sh tests/scripts/web.txt > /dev/null
MV_DUMP_PAGES=build/pages.json build/host/mvance build/host/mvance
python3 tools/gen_cheatsheet.py build/pages.json docs/cheatsheet.html "${APP_VER:-dev}"
mkdir -p docs/img
# the shots of web.txt, by what they show
set -- 3000 home 3200 osc1 3400 filter 3700 filter-env 4000 multi-fx 4200 delay 4400 reverb 5200 arp 5400 save
while [ $# -gt 1 ]; do cp "build/host/mvance-$1.png" "docs/img/$2.png"; shift 2; done
sh tests/run_host.sh tests/scripts/debug.txt > /dev/null   # the DEBUG page (GLO x7), the MIDI page on the way
cp build/host/mvance-1700.png docs/img/debug.png
cp build/host/mvance-1290.png docs/img/midi.png
cp build/host/mvance-1260.png docs/img/themes.png   # GLOBAL 2/2: THEME, COLOUR (the screen kit)
sh flash/build.sh                                  # the firmware page (docs/flash: fm1-kit's flash.js, fm1ota.js)
echo "run_docs: docs/cheatsheet.html, docs/img/ (11 screens), docs/flash/"
