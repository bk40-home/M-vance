/* SPDX-License-Identifier: GPL-3.0-only */
/* The second core's build options. The dual-core files (fm1_multicore.h, fm1_core1_*.h, fm1_power.h,
 * src/audio_worker.h) come from Melodee (keremimo/melodee 165d177, Kerem Kilic, GPL-3.0-only; its CPU1 hold from
 * Felucca 1.6.1) and keep its option names; M-Vsynth2 sets them here. Override one with DINGHY_DEFS, e.g. a one-core
 * build: DINGHY_DEFS=-DMELODEE_DUAL_CORE=0 ./build.sh */
#pragma once
#ifndef MELODEE_DUAL_CORE
#define MELODEE_DUAL_CORE 1           /* CPU1 renders half the voices (vs_voice.c); 0: one core */
#endif
#define MELODEE_CORE1_FALLBACK 1      /* a CPU1 that fails, faults, misreads or stalls is retired: CPU0 renders alone */
#define MELODEE_AUDIO_PROFILE 0
#define MELODEE_CACHE_RAM 0
#define MELODEE_CORE1_TEST 0
