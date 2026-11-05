/* SPDX-License-Identifier: GPL-3.0-only
 * Ported from Skiff (illuminus5/skiff, firmware/src/seq.c arp_*, params.c), itself a fork of Felucca:
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (Felucca); illuminus5 (Skiff's changes) */
/* M-vance arpeggiator: Skiff's arp (Kris 2 Nov 2026, A1a; its HOLD is the LATCH of A2), as a module of its own.
 * Settings are global (Kris 1 Nov, 6a: as the JP-8080 keeps the arp outside the patch).
 *
 *   ON      the ARP button (tap): while on, the keys feed the arp, not the voices (Skiff's note consumption)
 *   MODE    UP DN UPDN RND ORD REPEAT DNUP UP+8 CONV DIVG PINKY THUMB WALK CHORD. With C E G held: UPDN C E G E;
 *           DNUP G E C E; ORD as played; REPEAT the last note held; UP+8 C E G C' (the first note again an octave over
 *           the top); CONV C G E (outside in); DIVG E G C (inside out); PINKY C G E G (the top note between the
 *           others); THUMB C E C G (the bottom one); WALK a random step up or down (bouncing off the ends); CHORD the
 *           held notes together (the first 4), an octave higher each step through OCTAVE
 *   RATE    1/4 1/8 1/16 1/32 8T 16T 1/2 1/1 2BAR 4BAR of the tempo (TEMPO, or the MIDI clock's measured tempo)
 *   OCTAVE  1..4 octaves the list runs over;  GATE 1..127 (/128 of a step);  SWING 0..100 (even steps longer, odd
 *           shorter by SWING / 250 of a step);  CHANCE 0..127 (a step plays when rand & 127 <= it: 127 always)
 *   HOLD    the LATCH: the notes stay after the keys go up; the next chord (a key with none held) replaces them
 *   ORDER   NOTE (sorted by pitch) or PLAYED (as pressed)
 * Timing: counted in samples at control rate (mv_arp_tick per 16-frame block: 0.36 ms steps), the step length from
 * the quarter note in samples the main loop measures (TEMPO, or the MIDI clock). The first key fires at once.
 * Output: note events (as the engine's queue: on << 15 | note << 7 | velocity), ends before starts; velocity 100
 * (Skiff's). No allocation, no float; the per-step work is a list of at most 64 notes (held x octaves). */
#pragma once
#include <stdint.h>

enum { MV_ARP_ON, MV_ARP_MODE, MV_ARP_RATE, MV_ARP_OCT, MV_ARP_GATE, MV_ARP_SWING, MV_ARP_CHANCE, MV_ARP_HOLD,
       MV_ARP_ORDER, MV_ARP_N };
enum { MV_AM_UP, MV_AM_DN, MV_AM_UPDN, MV_AM_RND, MV_AM_ORD, MV_AM_REPEAT, MV_AM_DNUP, MV_AM_UP8, MV_AM_CONV,
       MV_AM_DIVG, MV_AM_PINKY, MV_AM_THUMB, MV_AM_WALK, MV_AM_CHORD, MV_AM_N };   /* Skiff's AM_UP.. (OFF: ON = 0) */
#define MV_ARP_NRATE 10u                        /* RATE values (Skiff's N_DIV) */
#define MV_ARP_EVMAX 12u                        /* events one tick can give (an off, 3 chord offs, 4 chord ons, ...) */

/* the settings' ranges (0..max), for the panel and the settings load */
static const uint8_t MV_ARP_MAX[MV_ARP_N] = {1, MV_AM_N - 1u, MV_ARP_NRATE - 1u, 4, 127, 100, 127, 1, 1};
static const uint8_t MV_ARP_MIN[MV_ARP_N] = {0, 0, 0, 1, 1, 0, 0, 0, 0};
static const uint8_t MV_ARP_DEF[MV_ARP_N] = {0, MV_AM_UP, 2, 1, 64, 0, 127, 0, 0};   /* off, UP, 1/16, 1 oct, .. */

typedef struct {
    uint8_t held[16], nheld;                    /* the notes the arp plays from (as pressed) */
    uint8_t phys;                               /* keys physically down (HOLD: a key with none down starts anew) */
    uint8_t note, ch[3];                        /* sounding: the note (0 none), CHORD's other three */
    uint8_t walk, hold;                         /* WALK's place; HOLD as last seen (its release lets go) */
    uint32_t pos, off, idx;                     /* samples into the step; samples to the gate's end; steps played */
    uint32_t rng;                               /* xorshift (Skiff's libc rng) */
} mv_arp_t;

void mv_arp_init(mv_arp_t *a);
/* a key on / off (set = the MV_ARP_* settings) */
void mv_arp_key(mv_arp_t *a, const volatile uint8_t *set, uint32_t note, int on);
/* n samples on at beat samples a quarter note: the events into ev (at most MV_ARP_EVMAX), their count returned */
uint32_t mv_arp_tick(mv_arp_t *a, const volatile uint8_t *set, uint32_t n, uint32_t beat, uint16_t *ev);
/* the arp switched off (or the transport reset): its sounding notes end (events), the held notes are forgotten */
uint32_t mv_arp_stop(mv_arp_t *a, uint16_t *ev);
/* MIDI START: the next block plays step 1 */
void mv_arp_restart(mv_arp_t *a);
