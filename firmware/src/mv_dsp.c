/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance DSP: the engine's parts in ONE translation unit of their own, compiled at -O2 (tools/build.py), while
 * the app and the platform (mvance.c: USB, flash, the screen) stay at -Os.
 *
 * Why apart: Dinghy builds its unity file at -Os because -O2 there (with usb.c) made USB-MIDI out babble and
 * broke updates (OberVave's finding). The DSP has no such ties: it touches no hardware and only C objects, so it
 * can take -O2 (unrolled inner loops, more in registers) without the platform seeing it. mvance.c includes only
 * the headers and calls mv_engine_* / mv_cc_apply / mv_patch_*; platform.c starts the second core
 * through mv_core1_start. */
/* The second core (Melodee's, hal/fm1_multicore.h + src/audio_worker.h; as M-Vsynth2's vs_dsp.c): CPU1 polls a mailbox
 * from RAM. Stage D0: it starts and idles; the engine does not hand it voices yet (MV_PAR_* are unused until D1).
 * The only hardware this file touches. Not on a computer, in a browser, or as an fm1-dock module (the dock's host
 * firmware owns CPU1): those are one core */
#if !defined(DINGHY_HOST) && !defined(DINGHY_WEBSIM) && !defined(DINGHY_DOCK)
#include "fm1_time.h"
#include "fm1_dual.h"
#if MELODEE_DUAL_CORE
#include "fm1_multicore.h"
#define MV_PAR_SUBMIT(fn, ctx) audio_worker_submit((fn), (ctx))   /* 1: CPU1 took the job */
#define MV_PAR_JOIN() audio_worker_join()      /* 1: its results are in; 0: CPU1 retired (one core from now) */
#define MV_PAR_CHECK() audio_worker_check()    /* between blocks: retire a CPU1 that faulted or misread */
int mv_core1_start(uint32_t power_ok, uint32_t early)   /* platform.c, at boot: 1 online */
{
    return fm1_core1_boot_allowed(power_ok, early) ? fm1_multicore_start() : 0;
}
int mv_core1_online(void);                     /* (declared in mv_voice.h from D1; here so D0 stands alone) */
int mv_core1_online(void) { return audio_worker_online; }
#endif
#endif
#ifndef MV_PAR_SUBMIT
int mv_core1_online(void);
int mv_core1_online(void) { return 0; }
#endif
#include "mv_patch.c"
#include "mv_pitch.c"
#include "mv_osc.c"
#include "mv_env.c"
#include "mv_lfo.c"
#include "mv_filter.c"
#include "mv_fx.c"
#include "mv_reverb.c"
#include "mv_arp.c"
#include "mv_seq.c"
#include "mv_lim.c"
#include "mv_voice.c"
