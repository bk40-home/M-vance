/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM: notes and MIDI (Kris 24 Nov 2026, P3, 7a), as the FM-1's mvance.c midi_in:
 *   notes          keys, pads-as-notes and the DAW's: where the mode and the split say; MIDI notes through the channel
 *                  gate (a part listens on its CH LO / CH UP, else on MIDI CH; 0 = OMNI; SINGLE: UPPER only)
 *   pitch bend     the engine's bend14            channel pressure   CONTROL (the JP's morph)
 *   CC1            the mod wheel (LFO2 in)        CC64, PEDAL        sustain (new on the VIBE: note-offs held)
 *                  (fm1-kit's fk_sustain, Kris 25 Nov: the same module as the FM-1's – one sustain for all engines)
 *   other CCs      the JP MODE2 CCs edit the part(s) PANEL edits (ED: the page follows)
 *   program change the program
 * The control flow's; everything reaches the audio through the queue (jt_edit.h). */
#pragma once
#include "jt_pages.h"
#include "../../third_party/fm1-kit/firmware/fk_sustain/fk_sustain.h"

static fk_sus_t sus;                            /* (zeroed: pedal up, nothing held) */
static void sus_release(void *ctx, uint32_t note, uint32_t mask) { (void)ctx; (void)q_push(Q_NOTE, (mask & 3u) << 8 | note, 0); }

static void note_send(uint32_t note, uint32_t vel, uint32_t to)
{
    if (note >= 128u || !(to & 3u)) return;
    if (vel) fk_sus_note_on(&sus, note);
    else if (!fk_sus_note_off(&sus, note, to)) return;   /* (held: the pedal sends it) */
    (void)q_push(Q_NOTE, (to & 3u) << 8 | note, vel);
}

static void sustain(uint32_t on) { (void)fk_sus_pedal(&sus, on ? 127u : 0u, sus_release, 0); }

/* the channel gate (the JT's PerfRouter, mvance.c midi_in): which parts hear channel ch (0..15) */
static uint32_t gate(uint32_t ch)
{
    uint32_t to = 0, cu = ED.perf[MV_PF_CHUP] ? ED.perf[MV_PF_CHUP] : ED.midich;
    uint32_t cl = ED.perf[MV_PF_CHLO] ? ED.perf[MV_PF_CHLO] : ED.midich;
    if (!cu || cu - 1u == ch) to |= MV_TO_UP;
    if (ED.perf[MV_PF_MODE] != MV_KEY_SINGLE && (!cl || cl - 1u == ch)) to |= MV_TO_LO;
    return to;
}

static void note_event(const vibe_event *e)     /* VIBE_EV_NOTE */
{
    uint32_t to = e->midi[1] == VIBE_NOTE_MIDI ? gate(e->midi[0] & 15u) : MV_TO_ROUTE;
    if (e->down) {
        int v = (int)(e->value * 126.0f + 1.5f);   /* velocity 0..1 -> 1..127 */
        note_send(e->id, (uint32_t)(v < 1 ? 1 : v > 127 ? 127 : v), to);
    } else note_send(e->id, 0, to);
}

static void prog_select(uint32_t n);            /* (main.c) */
static void midi_event(const vibe_event *e)     /* VIBE_EV_MIDI: what is not a note */
{
    uint32_t st = e->midi[0], ty = st & 0xF0u, d1 = e->midi[1] & 127u, d2 = e->midi[2] & 127u;
    if (ty == 0xE0u) (void)q_push(Q_BEND, d2 << 7 | d1, 0);
    else if (ty == 0xD0u) (void)q_push(Q_CTL, 0, d1);
    else if (ty == 0xC0u) prog_select(d1);
    else if (ty == 0xB0u) {
        if (d1 == 1u) (void)q_push(Q_WHEEL, 0, d2);
        else if (d1 == 64u) sustain(d2 >= 64u);
        else {                                  /* the JP MODE2 CCs, into the part(s) PANEL edits */
            if (ED.perf[MV_PF_MODE] == MV_KEY_SINGLE || panel != PANEL_LOWER) (void)mv_cc_apply(&ED.part[MV_PART_UP], d1, d2);
            if (ED.perf[MV_PF_MODE] != MV_KEY_SINGLE && panel != PANEL_UPPER) (void)mv_cc_apply(&ED.part[MV_PART_LO], d1, d2);
        }
    }
}
