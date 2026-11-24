/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM, P2's reference (Kris 23 Nov 2026): the sound side (vibe/jt80x0/jt_core.h) built natively, fed the
 * events of events.h (vibe/test/make_script.py) at the same blocks as wasmtest feeds the .wasm, in 128-frame blocks as
 * main.c's vibe_on_audio renders them. Prints wasmtest's out_hash (FNV-1a over the float bits, L then R, sample by
 * sample): the .wasm must print the same. And the program each change chose, for the eye. */
#include <stdio.h>
#include "../jt80x0/jt_core.h"
#include "events.h"

int main(void)
{
    static float l[128], r[128];
    uint32_t b, k = 0, i, h = 2166136261u, last = 0;
    if (!jt_init(REF_RATE, 0u)) { printf("rate %u not taken\n", REF_RATE); return 1; }
    for (b = 0; b < REF_BLOCKS; b++) {
        for (; k < sizeof REF_EV / sizeof REF_EV[0] && REF_EV[k][0] == b; k++) {
            if (REF_EV[k][1] == 1u) mv_engine_note_on(&eng, REF_EV[k][2], REF_EV[k][3]);
            else if (REF_EV[k][1] == 2u) mv_engine_note_off(&eng, REF_EV[k][2]);
            else { jt_prog_load(REF_EV[k][2]); last = REF_EV[k][2]; }
        }
        jt_tempo(120.0f);
        jt_render(l, r, 128u);
        for (i = 0; i < 128u; i++) {
            union { float f; uint32_t u; } a, c;
            a.f = l[i]; c.f = r[i];
            h = (h ^ a.u) * 16777619u;
            h = (h ^ c.u) * 16777619u;
        }
    }
    printf("ref %u Hz: out_hash %08x (last program %u)\n", REF_RATE, h, last);
    return 0;
}
