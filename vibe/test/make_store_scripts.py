#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""JT-80x0 on VIBE FM, P5's .wasm check (Kris 25 Nov 2026): what is saved comes back after a restart, in the real firmware.
  make_store_scripts.py <out dir> <store A> <store B>   writes s1.json s2.json s3.json
  s1  store A, fresh: edits, SAVE (F5 F5), a setting (REVERB), an edit left unsaved, then 3 s of rest (the files)
  s2  store A again (a restart): one note
  s3  store B, fresh: s1's events all before the first block, then the same note
s2 and s3 must give the same out_hash: the restart gave back exactly the sound s1 left."""
import json, sys
out, sa, sb = sys.argv[1], sys.argv[2], sys.argv[3]
K1, F5, RIGHT, RETURN = 10, 4, 20, 16
def press(b): return [[3, b, 1, 0], [3, b, 0, 0]]
EDITS = (press(K1) + press(RIGHT) * 4 + [[4, 2, 0, -20], [4, 3, 0, 30]] + press(F5) + press(F5)   # FILTER: CUTOFF -20, RESO +30; WRITE A-1
         + press(RETURN) + [[4, 3, 0, 40]] + press(K1) + [[4, 2, 0, -5]])                          # PLAY: REVERB +40; FILTER: CUTOFF -5 (unsaved)
NOTE = [{"t": 0.5, "ev": [2, 21, 1, 0.8]}, {"t": 1.5, "ev": [2, 21, 0, 0]}]
json.dump({"seconds": 3.0, "store": sa, "steps": [{"t": 0.1 + 0.01 * i, "ev": e} for i, e in enumerate(EDITS)]}, open(f"{out}/s1.json", "w"))
json.dump({"seconds": 2.5, "float": True, "store": sa, "steps": NOTE}, open(f"{out}/s2.json", "w"))
json.dump({"seconds": 2.5, "float": True, "store": sb, "steps": [{"t": 0.0, "ev": e} for e in EDITS] + NOTE}, open(f"{out}/s3.json", "w"))
