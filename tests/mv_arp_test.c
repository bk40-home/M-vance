/* SPDX-License-Identifier: GPL-3.0-only */
/* mv_arp (Skiff's arp, firmware/src/mv_arp.c): every mode's order against Skiff's own description (mv_arp.h), the
 * step length from RATE and the tempo, GATE, SWING, CHANCE, HOLD (the latch), ORDER, and CHORD.
 *   cc -std=c99 -O2 -Wall -Ifirmware/src tests/mv_arp_test.c -o mv_arp_test && ./mv_arp_test */
#include <stdio.h>
#include <string.h>
#include "mv_arp.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                  printf(__VA_ARGS__); printf("\n"); } } while (0)

#define BEAT 22050u                             /* 120 BPM at 44.1 kHz */
static mv_arp_t a;
static uint8_t set[MV_ARP_N];
static uint32_t ons[64], onat[64], non, offs, last_off_at, now;   /* now: samples since the last reset() */

/* run ms of 16-frame ticks, noting each note-on (and when) and each note-off */
static void run(uint32_t frames)
{
    uint32_t k, i, n, t = now;
    uint16_t ev[MV_ARP_EVMAX];
    for (k = 0; k < frames; k += 16u, t += 16u) {
        n = mv_arp_tick(&a, set, 16, BEAT, ev);
        CHECK(n <= MV_ARP_EVMAX, "events in a tick: %u", n);
        for (i = 0; i < n; i++) {
            if (ev[i] >> 15) { if (non < 64u) { ons[non] = ev[i] >> 7 & 127u; onat[non] = t; } non++; }
            else { offs++; last_off_at = t; }
        }
    }
    now = t;
}
static void reset(uint32_t mode)
{
    uint16_t ev[MV_ARP_EVMAX];
    uint32_t k;
    for (k = 0; k < MV_ARP_N; k++) set[k] = MV_ARP_DEF[k];
    set[MV_ARP_ON] = 1; set[MV_ARP_MODE] = (uint8_t)mode;
    mv_arp_stop(&a, ev);
    mv_arp_init(&a);
    non = 0; offs = 0; now = 0;
}
static void keys(const uint8_t *n, uint32_t c) { uint32_t i; for (i = 0; i < c; i++) mv_arp_key(&a, set, n[i], 1); }
static int seq(const uint8_t *want, uint32_t c)
{
    uint32_t i;
    if (non < c) return 0;
    for (i = 0; i < c; i++) if (ons[i] != want[i]) return 0;
    return 1;
}
static void show(const char *what) { uint32_t i; printf("  %s:", what); for (i = 0; i < non && i < 12u; i++) printf(" %u", ons[i]); printf("\n"); }

int main(void)
{
    static const uint8_t CEG[3] = {60, 64, 67}, GCE[3] = {67, 60, 64};
    const uint32_t step = BEAT / 4u;            /* 1/16 at 120 BPM: 5512 samples */
    /* the modes, C E G held (pressed G C E, to see NOTE vs PLAYED), 1 octave, 9 steps */
    static const struct { uint32_t mode; uint8_t want[9]; const char *name; } M[] = {
        {MV_AM_UP, {60, 64, 67, 60, 64, 67, 60, 64, 67}, "UP"},
        {MV_AM_DN, {67, 64, 60, 67, 64, 60, 67, 64, 60}, "DN"},
        {MV_AM_UPDN, {60, 64, 67, 64, 60, 64, 67, 64, 60}, "UPDN"},
        {MV_AM_DNUP, {67, 64, 60, 64, 67, 64, 60, 64, 67}, "DNUP"},
        {MV_AM_ORD, {60, 64, 67, 60, 64, 67, 60, 64, 67}, "ORD (sorted with ORDER NOTE)"},
        {MV_AM_REPEAT, {64, 64, 64, 64, 64, 64, 64, 64, 64}, "REPEAT (the last pressed)"},
        {MV_AM_UP8, {60, 64, 67, 72, 60, 64, 67, 72, 60}, "UP+8"},
        {MV_AM_CONV, {60, 67, 64, 60, 67, 64, 60, 67, 64}, "CONV"},
        {MV_AM_DIVG, {64, 67, 60, 64, 67, 60, 64, 67, 60}, "DIVG"},
        {MV_AM_PINKY, {60, 67, 64, 67, 60, 67, 64, 67, 60}, "PINKY"},
        {MV_AM_THUMB, {60, 64, 60, 67, 60, 64, 60, 67, 60}, "THUMB"},
    };
    uint32_t m, i;
    for (m = 0; m < sizeof M / sizeof M[0]; m++) {
        reset(M[m].mode);
        keys(GCE, 3);
        run(step * 9u - 100u);                  /* 9 steps (the first at once) */
        CHECK(non == 9u && seq(M[m].want, 9), "%s", M[m].name);
        if (non != 9u || !seq(M[m].want, 9)) show(M[m].name);
    }
    /* ORDER PLAYED: UP plays them as pressed */
    reset(MV_AM_UP); set[MV_ARP_ORDER] = 1; keys(GCE, 3); run(step * 3u - 100u);
    CHECK(seq(GCE, 3), "ORDER PLAYED: as pressed");
    /* OCTAVE 2: C E G C' E' G' */
    {
        static const uint8_t W[6] = {60, 64, 67, 72, 76, 79};
        reset(MV_AM_UP); set[MV_ARP_OCT] = 2; keys(CEG, 3); run(step * 6u - 100u);
        CHECK(non == 6u && seq(W, 6), "OCTAVE 2: up two octaves");
    }
    /* timing: 1/16 at 120 BPM = 5512 samples, to the 16-frame tick; GATE 64 = half a step */
    reset(MV_AM_UP); keys(CEG, 3); run(step * 4u);
    CHECK(onat[0] == 0u, "the first key fires at once");
    for (i = 1; i < 4u; i++)
        CHECK(onat[i] - onat[i - 1u] >= step && onat[i] - onat[i - 1u] < step + 16u, "1/16 step %u: %u samples",
              i, onat[i] - onat[i - 1u]);
    /* RATE 8T: a third of a beat */
    reset(MV_AM_UP); set[MV_ARP_RATE] = 4; keys(CEG, 3); run(BEAT);
    CHECK(onat[1] - onat[0] >= BEAT / 3u && onat[1] - onat[0] < BEAT / 3u + 16u, "8T: %u", onat[1] - onat[0]);
    /* GATE: the note ends GATE / 128 into its step */
    reset(MV_AM_UP); set[MV_ARP_GATE] = 32; keys(CEG, 1); run(step / 2u);
    CHECK(offs == 1u && last_off_at >= step / 4u && last_off_at < step / 4u + 32u, "GATE 32: off at %u", last_off_at);
    /* SWING 100: a pair of steps keeps its length; AS SKIFF'S ENGINE DOES IT, the first step of a pair is the SHORT
     * one (it compares with the step count after its increment: the off-beats come early), though Skiff's own arp
     * graph draws them late (normal swing). Kept as Skiff's, flagged (fm1-port delivery): the fix is one sign */
    reset(MV_AM_UP); set[MV_ARP_SWING] = 100; keys(CEG, 3); run(step * 5u);
    {
        uint32_t s1 = onat[1] - onat[0], s2 = onat[2] - onat[1];
        CHECK(s1 < step && s2 > step && s1 + s2 >= 2u * step && s1 + s2 < 2u * step + 32u, "SWING: %u %u", s1, s2);
    }
    /* CHANCE 0: nothing plays (rand & 127 <= 0 one time in 128: allow it) */
    reset(MV_AM_UP); set[MV_ARP_CHANCE] = 0; keys(CEG, 3); run(step * 64u);
    CHECK(non <= 3u, "CHANCE 0: %u notes in 64 steps", non);
    /* keys up: the arp stops (no HOLD), its note ends */
    reset(MV_AM_UP); keys(CEG, 3); run(step * 2u);
    for (i = 0; i < 3u; i++) mv_arp_key(&a, set, CEG[i], 0);
    non = 0; run(step * 4u);
    CHECK(non == 0u && a.note == 0u, "keys up: no more notes");
    /* HOLD: the notes stay after the keys go up; a new chord replaces them */
    reset(MV_AM_UP); set[MV_ARP_HOLD] = 1; keys(CEG, 3); run(step);
    for (i = 0; i < 3u; i++) mv_arp_key(&a, set, CEG[i], 0);
    non = 0; run(step * 3u);
    CHECK(non == 3u, "HOLD: still playing after the keys go up (%u notes)", non);
    mv_arp_key(&a, set, 50, 1);
    CHECK(a.nheld == 1u && a.held[0] == 50u, "HOLD: a new key with none down replaces the chord");
    mv_arp_key(&a, set, 53, 1);
    CHECK(a.nheld == 2u, "HOLD: keys pressed together add up");
    mv_arp_key(&a, set, 50, 0); mv_arp_key(&a, set, 53, 0);
    set[MV_ARP_HOLD] = 0; run(16);
    CHECK(a.nheld == 0u, "HOLD off with no key down: the latch lets go");
    /* CHORD: the held notes together, an octave up each step through OCTAVE 2 */
    reset(MV_AM_CHORD); set[MV_ARP_OCT] = 2; keys(CEG, 3); run(step + 100u);   /* just past the 2nd chord: sounding */
    {
        static const uint8_t W[6] = {60, 64, 67, 72, 76, 79};
        CHECK(non == 6u && seq(W, 6), "CHORD: C E G, then an octave up");
    }
    /* stop: the sounding notes end and nothing is held */
    {
        uint16_t ev[MV_ARP_EVMAX];
        uint32_t n = mv_arp_stop(&a, ev);
        CHECK(n == 3u && a.nheld == 0u && a.note == 0u, "stop: 3 chord notes off (%u), nothing held", n);
    }
    /* WALK: steps of one, inside the list */
    reset(MV_AM_WALK); keys(CEG, 3); run(step * 20u);
    for (i = 1; i < non && i < 20u; i++) {
        int32_t d = (int32_t)ons[i] - (int32_t)ons[i - 1u];
        CHECK(d == 4 || d == -4 || d == 3 || d == -3, "WALK: one place at a time (%d)", d);
    }
    printf("mv_arp_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
