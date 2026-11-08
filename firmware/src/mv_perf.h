/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance performance bytes (Kris 3 Nov, layer / split: fm1-port/mvance-layer-split-proposal-2026-11-03.md): what a
 * program stores beside its two patches, and how the engine plays them (mv_voice.h). Shared by the engine, the
 * programs (mv_prog) and the panel (mv_ui). Constants only. */
#pragma once
#include <stdint.h>

/* the performance bytes, MV_PF_N of them (the rest are spare: zero) */
enum {
    MV_PF_MODE,                                 /* KEY MODE: MV_KEY_* */
    MV_PF_SPLIT,                                /* SPLIT POINT: notes below it LOWER, from it UPPER (0..127) */
    MV_PF_BAL,                                  /* BALANCE 0..127, 64 both at unity (the JT's law) */
    MV_PF_VSPLIT,                               /* VOICES LOWER + UPPER: 0..6 = 1+7 .. 7+1 (the JT's voice split) */
    MV_PF_CHLO, MV_PF_CHUP,                     /* a part's MIDI channel: 0 the global one, 1..16 */
    MV_PF_ARP,                                  /* ARP PART: MV_ARPP_* */
    MV_PF_SYNCUP, MV_PF_SYNCLO,                 /* a part's LFO1 SYNC, the JP's part parameter (JE page 2, index 5;
                                                   SysEx 03 pp 10 / 11 05): 0 OFF, 1..22 = MV_LFO1_SYNC_N - 1 note
                                                   values (Kris 5 Nov: fm1-port/mvance-lfo1-sync-proposal-2026-11-05.md) */
    MV_PF_DSYNUP, MV_PF_DSYNLO,                 /* a part's DELAY SYNC (JE page 2, index 4; SysEx 03 pp 10 / 11 04):
                                                   0 OFF, 1..10 = 1/16 .. 1/2, the first 10 of the LFO1 list */
    MV_PF_CSYNUP, MV_PF_CSYNLO,                 /* a part's CHORUS SYNC RATE (JE page 2, index 6; 03 pp 10 / 11 06):
                                                   0 OFF, 1..22 the LFO1 list, 23 = LFO1 (the chorus LFO at LFO1's rate).
                                                   The FX are UPPER's, so the engine plays UPPER's two; LOWER's are kept
                                                   for the JP round trip (Kris 7 Nov: fm1-port/mvance-fx-sync-proposal-
                                                   2026-11-07.md, 1a) */
    MV_PF_N = 16
};
enum { MV_KEY_SINGLE, MV_KEY_DUAL, MV_KEY_SPLIT };
enum { MV_ARPP_AUTO, MV_ARPP_UPPER, MV_ARPP_LOWER, MV_ARPP_BOTH };   /* AUTO: SINGLE UPPER, DUAL both, SPLIT LOWER */
#define MV_PART_UP 0u                           /* part index: UPPER (the engine's patch) */
#define MV_PART_LO 1u                           /* LOWER (the engine's lower) */
#define MV_TO_UP 1u                             /* a note's destination mask (bits by part) */
#define MV_TO_LO 2u
#define MV_TO_ROUTE 3u                          /* both allowed: the mode and the split decide */
#define MV_PF_SYNC(part) (MV_PF_SYNCUP + (part)) /* the LFO1 SYNC byte of part MV_PART_UP / MV_PART_LO */
#define MV_LFO1_SYNC_N 23u                      /* LFO1 SYNC values: OFF, 1/16 .. 8 MEASURES (the JP's list, mv_lfo.c) */
#define MV_PF_DSYN(part) (MV_PF_DSYNUP + (part)) /* the DELAY SYNC byte of a part */
#define MV_PF_CSYN(part) (MV_PF_CSYNUP + (part)) /* the CHORUS SYNC RATE byte of a part */
#define MV_DSYNC_N 11u                          /* DELAY SYNC values: OFF, 1/16 .. 1/2 */
#define MV_CSYNC_N 24u                          /* CHORUS SYNC RATE values: OFF, 1/16 .. 8 MEASURES, LFO1 */
#define MV_CSYNC_LFO1 23u                       /* CHORUS SYNC RATE: follow UPPER's LFO1 */
/* a single's: SINGLE, split 60 (C4), balance 64, 4+4, the global channel for both, ARP AUTO */
static const uint8_t MV_PERF_DEF[MV_PF_N] = {MV_KEY_SINGLE, 60, 64, 3, 0, 0, MV_ARPP_AUTO};
