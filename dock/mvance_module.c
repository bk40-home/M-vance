/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* M-VANCE as an fm1-dock module (joakimeriksson/fm1-dock, ABI 2): the M-vance firmware's own app and engine
 * (firmware/src/mvance.c, mv_dsp.c), built with dock/module.cfg's switches (one part, 64 programs, no sequencer,
 * no DEBUG page, no SysEx transfer, delay lines of 900 ms: firmware/src/mv_build.h) on the host's services
 * (dock/mv_dock_platform.inc, by way of platform.c's DINGHY_DOCK). As OberVave's module (modules/obervave/): its
 * voices and effects render in the host's audio interrupt (one core); its panel, screen and MIDI are the host's.
 * HOME held 1.5 s alone opens fm1-dock's module list. Kris 12 Nov: fm1-port/mvance-dock-feasibility-proposal-2026-11-12.md.
 *
 * (Its calls are mvd_*: mvance.c has a ui of its own.)
 *   init      the host's table, then M-vance's start-up (the first app_main: settings, programs, the home screen)
 *   draw      one pass of M-vance's main loop (app_main); full: the screen and the LEDs again (after the picker)
 *   render    the engine into the host's block (zeroed by the host; MASTER and the EQ are the host's, after)
 *   ui, midi  the panel's events and every MIDI message (clock and transport too) into hb_fm1.h's queues
 *   release   switched away: a SAVE or a setting still waiting for silence goes to flash (the host then clears the
 *             module's RAM; it starts afresh next time) */
#include "hb_module.h"
#include "../firmware/src/mvance.c"

static void mvd_render(int32_t *out, uint32_t n)
{
    if (n == HALF_FRAMES)                       /* (the host's block: always, as Dinghy's) */
        app_audio(out, n);
}

static void mvd_ui(uint32_t ev, int32_t arg) { hbf_event(ev, arg); }
static void mvd_midi(uint32_t st, uint32_t d1, uint32_t d2) { hbf_midi(st, d1, d2); }
static void mvd_note(uint32_t ch, uint32_t key, uint32_t vel) { hbf_midi(0x90u | (ch & 15u), key, vel); }
static void mvd_cc(uint32_t ch, uint32_t num, uint32_t val) { hbf_midi(0xB0u | (ch & 15u), num, val); }

static void mvd_draw(uint32_t full)
{
    if (full) {
        dock_full = 1;                          /* (mvance.c: the screen as on a page change, at the pass's start) */
        dock_leds_resend();
    }
    app_main();
}

static int mvd_init(const hb_host_t *h)
{
    H = h;
    app_main();                                 /* the start-up and a first pass */
    dock_leds_resend();
    H->log("m-vance: M-vance " APP_VER " (fm1-dock module)");
    return 0;
}

static void mvd_release(void) { dock_release(); }

HB_DESC = {HB_ABI, "M-VANCE", mvd_init, mvd_release, mvd_note, mvd_cc, mvd_render, mvd_ui, mvd_draw, 0, 0,
           HB_F_SCREEN | HB_F_PANEL, mvd_midi, 0};   /* (no HB_F_DATA: data() 0) */
