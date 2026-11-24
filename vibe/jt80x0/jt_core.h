/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM: the sound side (Kris 23 Nov 2026, P2 of claude/jt80x0-vibe-port-proposal-2026-11-23.md).
 *
 * The engine is the FM-1's, unchanged: firmware/src + firmware/gen, copied flat into engine/ by vibe/build.sh (one
 * source: an FM-1 fix reaches the VIBE on the next build). Built here with
 *   MV_RATE_RT    the rate chosen at start (mv_rate.h): 44.1 or 48 kHz, the same sound in seconds and hertz
 *   DINGHY_VIBE   no hardware, one core (mv_dsp.c)
 * Everything in this file belongs to the AUDIO flow (vibe_on_audio) alone, after jt_init; main.c hands it notes and
 * program changes through a queue. vibe/test/ref.c builds the same file natively: the reference the .wasm must match. */
#pragma once
#define MV_RATE_RT 1
#define DINGHY_VIBE 1
#include "engine/mv_dsp.c"                      /* the engine: one translation unit with this firmware */
#include "engine/mv_factory.h"                  /* the 64 factory programs (UPPER, LOWER, performance) */

_Static_assert(MV_PF_N <= sizeof MV_FACTORY_PERF[0], "jt_core: a program's performance bytes");
_Static_assert(MV_PATCH_BYTES <= sizeof MV_FACTORY[0], "jt_core: a program's patch bytes");

static mv_engine_t eng;                         /* the engine (the audio flow's) */

/* a factory program into the engine: UPPER, LOWER and how the two play (as the FM-1's prog_to_engine) */
static void jt_prog_load(uint32_t n)
{
    uint32_t i;
    n %= MV_NPROG;
    for (i = 0; i < MV_PATCH_BYTES; i++) { eng.patch.b[i] = MV_FACTORY[n][i]; eng.lower.b[i] = MV_FACTORY_LO[n][i]; }
    for (i = 0; i < MV_PF_N; i++) eng.perf[i] = MV_FACTORY_PERF[n][i];
}

/* the engine's rate for the host's: 1 = taken (44.1 / 48 kHz), 0 = not one this engine has tables for */
static int jt_init(uint32_t fs, uint32_t prog)
{
    int ok = mv_rate_set(fs);
    mv_engine_init(&eng);                       /* (after mv_rate_set: it takes its steps at this rate) */
    jt_prog_load(prog);
    return ok;
}

/* once a block, before rendering: the arp's and the LFO / delay SYNC's quarter note at this tempo (as the FM-1's main
 * loop; mv_engine_sync divides only when it moved) */
static void jt_tempo(float bpm)
{
    if (bpm < 20.0f || bpm > 400.0f) bpm = 120.0f;
    eng.arp_beat = (uint32_t)((float)MV_FS * 60.0f / bpm + 0.5f);
    mv_engine_sync(&eng);
}

/* n frames (a multiple of 16, at most JT_BLOCK) of the engine, as floats */
#define JT_BLOCK 256u
static int32_t jt_buf[2u * JT_BLOCK];
static void jt_render(float *l, float *r, uint32_t n)
{
    uint32_t j;
    for (j = 0; j < 2u * n; j++) jt_buf[j] = 0;  /* (the engine adds into it) */
    mv_engine_render(&eng, jt_buf, n);
    for (j = 0; j < n; j++) {
        l[j] = (float)jt_buf[2u * j] * (1.0f / 32768.0f);
        r[j] = (float)jt_buf[2u * j + 1u] * (1.0f / 32768.0f);
    }
}
