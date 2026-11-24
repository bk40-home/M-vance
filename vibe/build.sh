#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# JT-80x0 for VIBE FM (Kris 23 Nov 2026, claude/jt80x0-vibe-port-proposal-2026-11-23.md, 2a): the engine copied flat from
# firmware/ into vibe/jt80x0/engine/ (git-ignored: the FM-1's source stays the one source), then the SDK's own
# tools/build.sh and vibe-check. Needs the vibe-fm-sdk checkout (VIBE_SDK, default ../vibe-fm-sdk next to this repo),
# Zig 0.13.0 and Node.
#   sh vibe/build.sh          ->  <sdk>/build/jt80x0.wasm + jt80x0.zip (drop the zip onto VIBE FM VST), and vibe-check
#   CHECK=0 sh vibe/build.sh  ->  the same without vibe-check (about a minute)
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
SDK=${VIBE_SDK:-$ROOT/../vibe-fm-sdk}
[ -f "$SDK/tools/build.sh" ] || { echo "vibe/build.sh: no VIBE FM SDK at $SDK (set VIBE_SDK)"; exit 1; }
[ "${CHECK:-1}" = 0 ] || command -v node >/dev/null 2>&1 || { echo "vibe/build.sh: Node is needed (vibe-check, wasmtest): install Node 22, e.g. nvm install 22"; exit 1; }
FW="$ROOT/vibe/jt80x0"
python3 tools/gen_params.py >/dev/null          # (the generated headers, as the FM-1 build makes them)
python3 tools/gen_tables.py >/dev/null
mkdir -p "$FW/engine"
# the engine: what mv_dsp.c includes, and the headers they need; the generated tables and the factory bank
for f in firmware/src/mv_*.c firmware/src/mv_*.h firmware/gen/mv_tables.h firmware/gen/mv_rv_tables.h \
         firmware/gen/mv_jvtri.h firmware/gen/mv_factory.h firmware/gen/mv_params.h; do
    cp "$f" "$FW/engine/"
done
cp firmware/src/audio_worker.h "$FW/engine/" 2>/dev/null || true
(cd "$SDK" && sh tools/build.sh "$FW")
if [ "${CHECK:-1}" != 0 ]; then (cd "$SDK" && node tools/vibe-check.mjs build/jt80x0.wasm); fi
