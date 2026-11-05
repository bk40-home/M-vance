/* SPDX-License-Identifier: GPL-3.0-only
 * Ported from Skiff (illuminus5/skiff, firmware/src/seq.c), a fork of Felucca:
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (Felucca); illuminus5 (Skiff's changes) */
/* M-vance arpeggiator: implementation (modes, timing and output in mv_arp.h). Each function names the Skiff one it
 * ports; what Skiff's track model adds (TAKES, recording, scale keyboards, SOLO keys) is left out. */
#include "mv_arp.h"

#define FIRE 0xFFFFFFFu                         /* arp_pos marker: fire on this block (Skiff's 0xFFFFFFF) */
#define VEL 100u                                /* the arp's velocity (Skiff's) */
#define EV_ON(n) ((uint16_t)(1u << 15 | (uint32_t)(n) << 7 | VEL))
#define EV_OFF(n) ((uint16_t)((uint32_t)(n) << 7))

static uint32_t arp_rng(mv_arp_t *a)           /* libc.c rng: xorshift 13 / 17 / 5 */
{
    a->rng ^= a->rng << 13;
    a->rng ^= a->rng >> 17;
    a->rng ^= a->rng << 5;
    return a->rng;
}

static uint32_t clamp127(int32_t v) { return (uint32_t)(v < 0 ? 0 : v > 127 ? 127 : v); }

void mv_arp_init(mv_arp_t *a)
{
    uint32_t k;
    for (k = 0; k < sizeof *a; k++) ((uint8_t *)a)[k] = 0;
    a->rng = 0x1234567u;
}

void mv_arp_restart(mv_arp_t *a)
{
    if (a->nheld) { a->pos = FIRE; a->idx = 0xFFFFFFFFu; }
}

/* arp_add / arp_remove */
void mv_arp_key(mv_arp_t *a, const volatile uint8_t *set, uint32_t note, int on)
{
    uint32_t i, k = 0;
    if (on) {
        if (set[MV_ARP_HOLD] && a->phys == 0u) a->nheld = 0;   /* a new chord replaces the latched one */
        if (a->phys < 255u) a->phys++;          /* every key-down: the key-up counts it off */
        for (i = 0; i < a->nheld; i++)
            if (a->held[i] == note) return;     /* repeated note-on: not a new note */
        if (a->nheld < 16u) a->held[a->nheld++] = (uint8_t)note;
        if (a->nheld == 1u) { a->pos = FIRE; a->idx = 0xFFFFFFFFu; }   /* fire on this block */
        return;
    }
    if (a->phys) a->phys--;
    if (set[MV_ARP_HOLD]) return;               /* latched: the note stays */
    for (i = 0; i < a->nheld; i++)
        if (a->held[i] != note) a->held[k++] = a->held[i];
    a->nheld = (uint8_t)k;
}

/* arp_list: the held notes (sorted, or as played) over OCTAVE octaves; *cnt the distinct held notes */
static uint32_t arp_list(const mv_arp_t *a, const volatile uint8_t *set, uint32_t *list, uint32_t *cnt)
{
    uint32_t i, j, n = 0, x, last = 0, len = 0, o, oct = set[MV_ARP_OCT] ? set[MV_ARP_OCT] : 1u;
    for (i = 0; i < a->nheld; i++) {
        x = a->held[i];
        last = x;
        for (j = 0; j < n && list[j] != x; j++) {}
        if (j == n) list[n++] = x;
    }
    *cnt = n;
    if (!set[MV_ARP_ORDER])                     /* NOTE: sorted by pitch (insertion sort, at most 16) */
        for (i = 1; i < n; i++)
            for (j = i; j > 0 && list[j - 1u] > list[j]; j--) { x = list[j]; list[j] = list[j - 1u]; list[j - 1u] = x; }
    for (o = 0; o < oct; o++)
        for (i = 0; i < n && len < 64u; i++)
            list[len++] = clamp127((int32_t)list[i] + 12 * (int32_t)o);
    if (n && set[MV_ARP_MODE] == MV_AM_REPEAT) { list[0] = last; len = 1; }   /* REPEAT: the last note, no octaves */
    return len;
}

/* arp_pick: the place in the list (len notes) step idx plays; UP+8 adds its note */
static uint32_t arp_pick(mv_arp_t *a, const volatile uint8_t *set, uint32_t *list, uint32_t len)
{
    uint32_t i = a->idx, m = set[MV_ARP_MODE], cyc, k, w;
    if (m == MV_AM_UP8 && len < 64u)            /* UP+8: the first note again over the top */
        list[len++] = clamp127((int32_t)list[0] + 12 * (int32_t)set[MV_ARP_OCT]);
    cyc = len > 1u ? 2u * len - 2u : 1u;
    k = i % cyc;
    switch (m) {
    case MV_AM_DN: return len - 1u - i % len;
    case MV_AM_UPDN: return k < len ? k : cyc - k;
    case MV_AM_DNUP: return k < len ? len - 1u - k : k - (len - 1u);
    case MV_AM_RND: return arp_rng(a) % len;
    case MV_AM_CONV:
    case MV_AM_DIVG:
        k = i % len;
        if (m == MV_AM_DIVG) k = len - 1u - k;
        return (k & 1u) ? len - 1u - k / 2u : k / 2u;
    case MV_AM_PINKY: return len < 2u ? 0u : (k & 1u) ? len - 1u : k / 2u;
    case MV_AM_THUMB: return len < 2u ? 0u : (k & 1u) ? 1u + k / 2u : 0u;
    case MV_AM_WALK:
        w = a->walk;
        if (!i) w = 0;                          /* (from the bottom) */
        else if (w >= len) w = len - 1u;        /* (notes let go) */
        else if (len > 1u) w = !w ? 1u : w == len - 1u ? w - 1u : (arp_rng(a) >> 7) & 1u ? w + 1u : w - 1u;
        a->walk = (uint8_t)w;
        return w;
    default: return i % len;                    /* UP, ORD (the list as played), UP+8 */
    }
}

/* arp_off: the sounding note(s) end */
static uint32_t arp_off(mv_arp_t *a, uint16_t *ev, uint32_t ne)
{
    uint32_t i;
    for (i = 0; i < 3u; i++)
        if (a->ch[i]) { ev[ne++] = EV_OFF(a->ch[i]); a->ch[i] = 0; }
    if (a->note) { ev[ne++] = EV_OFF(a->note); a->note = 0; }
    return ne;
}

/* fx.c div_samples: RATE -> samples a step (DIV_DEN 1 2 4 8 3 6; 1/2 .. 4BAR double the quarter each) */
static uint32_t div_samples(uint32_t rate, uint32_t beat)
{
    static const uint8_t DEN[6] = {1, 2, 4, 8, 3, 6};
    return rate < 6u ? beat / DEN[rate] : beat << (rate - 5u);
}

/* arp_fire: a step starts (period: its length): the next note (or CHORD) of the list */
static uint32_t arp_fire(mv_arp_t *a, const volatile uint8_t *set, uint32_t period, uint16_t *ev, uint32_t ne)
{
    uint32_t cnt, list[65], len, j, i, o, nt, top = 0, oct = set[MV_ARP_OCT] ? set[MV_ARP_OCT] : 1u;
    if (!(len = arp_list(a, set, list, &cnt))) return arp_off(a, ev, ne);
    a->idx++;
    j = set[MV_ARP_MODE] == MV_AM_CHORD ? 0u : arp_pick(a, set, list, len);
    ne = arp_off(a, ev, ne);
    if ((arp_rng(a) & 127u) > set[MV_ARP_CHANCE]) return ne;   /* CHANCE: this step rests */
    a->off = (uint32_t)(((uint64_t)period * set[MV_ARP_GATE]) >> 7);
    if (set[MV_ARP_MODE] != MV_AM_CHORD) {
        a->note = (uint8_t)list[j];
        ev[ne++] = EV_ON(a->note);
        return ne;
    }
    o = a->idx % oct;                           /* arp_chord: the first 4 held notes, o octaves up */
    for (i = 0; i < cnt && i < 4u; i++) {
        nt = clamp127((int32_t)list[i] + 12 * (int32_t)o);
        if (nt == 127u && top++) continue;      /* (notes clamped onto the top one: once) */
        if (!a->note) a->note = (uint8_t)nt;
        else a->ch[i - 1u] = (uint8_t)nt;
        ev[ne++] = EV_ON(nt);
    }
    return ne;
}

/* arp_step / arp_tick (n samples, the gate counted in the same) */
uint32_t mv_arp_tick(mv_arp_t *a, const volatile uint8_t *set, uint32_t n, uint32_t beat, uint16_t *ev)
{
    uint32_t ne = 0, period;
    int32_t sw;
    if (a->hold && !set[MV_ARP_HOLD] && !a->phys) a->nheld = 0;   /* HOLD let go with no key down: the latch ends */
    a->hold = set[MV_ARP_HOLD];
    if (a->note) {
        if (a->off <= n) ne = arp_off(a, ev, ne);
        else a->off -= n;
    }
    if (!a->nheld) {
        if (a->note) ne = arp_off(a, ev, ne);
        return ne;
    }
    period = div_samples(set[MV_ARP_RATE] < MV_ARP_NRATE ? set[MV_ARP_RATE] : 2u, beat);
    if (!period) period = 1;
    sw = (int32_t)set[MV_ARP_SWING] * (int32_t)period / 250;
    a->pos += n;
    if (a->pos < (uint32_t)((int32_t)period + ((a->idx & 1u) ? sw : -sw)) && a->pos != FIRE + n) return ne;
    a->pos = 0;
    return arp_fire(a, set, period, ev, ne);
}

uint32_t mv_arp_stop(mv_arp_t *a, uint16_t *ev)
{
    a->nheld = 0; a->phys = 0;
    return arp_off(a, ev, 0);
}
