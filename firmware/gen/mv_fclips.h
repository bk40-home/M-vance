/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance factory clips (Kris 7 Nov 2026): read-only sequencer patterns, shown after the 12 user clips on SEQ 3/3
 * (F1, F2 ..) and LOADed into either track like them. Made by tools/mid2clip.py from MIDI files into
 * firmware/gen/mv_fclips_local.h, which git ignores (third-party transcriptions, as the factory bank): included when
 * it is there, else none (the public repo). A test may supply its own set: MV_FCLIPS_TEST, then MV_FCLIP_N and
 * MV_FCLIPS of its own before mv_clip.c. */
#pragma once
#include <stdint.h>

typedef struct {
    const char *name;                           /* 16 characters at most, ASCII 32..95 */
    uint8_t bars, root;                         /* LENGTH in bars (1..32), ROOT (the first note-on) */
    uint16_t n, bytes;                          /* events; their coded size */
    const uint8_t *ev;                          /* the events, coded as mv_seq.h FLASH says (played from here) */
} mv_fclip_t;

#ifndef MV_FCLIPS_TEST
#if defined(__has_include)
#if __has_include("mv_fclips_local.h")
#include "mv_fclips_local.h"
#endif
#endif
#ifndef MV_FCLIP_N                              /* none: an empty set (the placeholder is never shown) */
#define MV_FCLIP_N 0u
static const mv_fclip_t MV_FCLIPS[1] = {{"", 1, 0xFF, 0, 0, 0}};
#endif
#endif
