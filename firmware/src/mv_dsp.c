/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance DSP: the engine's parts in ONE translation unit of their own, compiled at -O2 (tools/build.py), while
 * the app and the platform (mvance.c: USB, flash, the screen) stay at -Os.
 *
 * Why apart: Dinghy builds its unity file at -Os because -O2 there (with usb.c) made USB-MIDI out babble and
 * broke updates (OberVave's finding). The DSP has no such ties: it touches no hardware and only C objects, so it
 * can take -O2 (unrolled inner loops, more in registers) without the platform seeing it. mvance.c includes only
 * the headers and calls mv_engine_* / mv_cc_apply / mv_patch_*. */
#include "mv_patch.c"
#include "mv_pitch.c"
#include "mv_osc.c"
#include "mv_env.c"
#include "mv_lfo.c"
#include "mv_filter.c"
#include "mv_fx.c"
#include "mv_reverb.c"
#include "mv_arp.c"
#include "mv_lim.c"
#include "mv_voice.c"
