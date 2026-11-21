/* SPDX-License-Identifier: GPL-3.0-only */
/* mv_seq automation (Kris 20 Nov: fm1-port/mvance-seq-automation-proposal-2026-11-20.md): knob moves recorded as
 * points (on the 4-tick grid, thinned to one a CC every MV_SEQ_PT_GAP ticks, the last value of a turn landing),
 * replayed in time each loop through emit (never sounding, never a ROOT), touch to replace (only that CC, only while
 * it turns), the 256-event budget shared with the notes (no note left without its note-off), CLEAR ALL / NOTES /
 * AUTO, a flash clip's points (the coding's flag), and disarm landing a held-back value.
 *   cc -std=c99 -O2 -Wall -Ifirmware/src tests/mv_seq_auto_test.c -o t && ./t */
#include <stdio.h>
#include <string.h>
#include "mv_seq.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                  printf(__VA_ARGS__); printf("\n"); } } while (0)
#define BEAT 22050u
static mv_seq_t s;
static uint8_t set[MV_SQ_N];
static struct { uint32_t tick, part, note, vel; } lg[8192];
static uint32_t nl;
static void emit(void *ctx, uint32_t part, uint32_t note, uint32_t vel)
{
    (void)ctx;
    if (nl < 8192u) { lg[nl].tick = s.ticks; lg[nl].part = part; lg[nl].note = note; lg[nl].vel = vel; }
    nl++;
}
static void run_ticks(uint32_t ticks)            /* exactly this many ticks (a block a tick: no rounding) */
{
    uint32_t k;
    for (k = 0; k < ticks; k++) mv_seq_tick(&s, set, BEAT / MV_SEQ_PPQN + 1u, BEAT, ~0u, emit, 0);
}
static void start(uint32_t bars)
{
    uint32_t k;
    for (k = 0; k < MV_SQ_N; k++) set[k] = MV_SQ_DEF[k];
    set[MV_SQ_LENUP] = set[MV_SQ_LENLO] = (uint8_t)bars;
    mv_seq_init(&s); nl = 0;
    mv_seq_start(&s);
    run_ticks(1);                               /* (tick 0 plays: the lengths are set) */
}
static uint32_t points(uint32_t part, uint32_t cc)   /* the track's points of a CC */
{
    uint32_t i, n = 0;
    for (i = 0; i < s.t[part].n; i++) n += (s.t[part].ev[i].vel & MV_SEQ_PT) && (s.t[part].ev[i].note & 127u) == cc;
    return n;
}
static uint32_t played(uint32_t cc, uint32_t from)   /* points of a CC emitted at or after log entry from */
{
    uint32_t i, n = 0;
    for (i = from; i < nl && i < 8192u; i++) n += (lg[i].vel & MV_SEQ_PT) && lg[i].note == cc;
    return n;
}
static int sorted_and_paired(const mv_seq_track_t *T)   /* in tick order; every pitch's on / off alternating, even */
{
    uint8_t on[128] = {0}, cnt[128] = {0};
    uint32_t i;
    for (i = 0; i < T->n; i++) {
        if (i && T->ev[i].tick < T->ev[i - 1u].tick) return 0;
        if (T->ev[i].vel & MV_SEQ_PT) continue;
        if (cnt[T->ev[i].note & 127u] && on[T->ev[i].note & 127u] == (T->ev[i].vel != 0)) return 0;
        on[T->ev[i].note & 127u] = T->ev[i].vel != 0; cnt[T->ev[i].note & 127u]++;
    }
    for (i = 0; i < 128u; i++) if (cnt[i] & 1u) return 0;
    return 1;
}

int main(void)
{
    uint32_t i, k, m;
    /* 1. a sweep: one move a tick for a bar -> thinned, on the grid; the last value lands */
    start(1);
    for (i = 0; i < 100u; i++) { mv_seq_point(&s, 0, 74, i); run_ticks(1); }
    m = points(0, 74);
    CHECK(m >= 100u / MV_SEQ_PT_GAP && m <= 100u / MV_SEQ_PT_GAP + 2u, "a 100-tick sweep: %u points (one every %u ticks)", m, MV_SEQ_PT_GAP);
    for (i = k = 0; i < s.t[0].n; i++) k += s.t[0].ev[i].tick % MV_SEQ_PT_GRID != 0u;
    CHECK(k == 0, "every point on the %u-tick grid", MV_SEQ_PT_GRID);
    run_ticks(MV_SEQ_PT_GAP + 2u);
    for (i = k = 0; i < s.t[0].n; i++) if ((s.t[0].ev[i].vel & MV_SEQ_PT) && (s.t[0].ev[i].vel & 127u) == 99u) k++;
    CHECK(k == 1, "the turn's last value (99) landed");
    CHECK(s.t[0].root == MV_SEQ_NOROOT, "a point is no ROOT");
    /* 2. played back in time each loop, never as a note */
    run_ticks(MV_SEQ_PT_HOLD);                  /* (the knob let go) */
    k = nl;
    run_ticks(MV_SEQ_BAR);
    CHECK(played(74, k) == points(0, 74), "the next pass plays every point once (%u of %u)", played(74, k), points(0, 74));
    for (i = k, m = 0; i < nl; i++) m += !(lg[i].vel & MV_SEQ_PT);
    CHECK(m == 0, "no note emitted for a point");
    /* 3. touch to replace: a second pass turning only cc 71 leaves cc 74's points */
    m = points(0, 74);
    for (i = 0; i < 40u; i++) { mv_seq_point(&s, 0, 71, 10); run_ticks(1); }
    CHECK(points(0, 74) == m, "another CC's points untouched (%u)", points(0, 74));
    /* turning cc 74 again from the bar's start for 50 ticks replaces its points there, the rest stay */
    run_ticks(MV_SEQ_BAR - (s.t[0].tick ? s.t[0].tick : MV_SEQ_BAR));
    for (i = 0; i < 50u; i++) { mv_seq_point(&s, 0, 74, 127); run_ticks(1); }
    for (i = k = m = 0; i < s.t[0].n; i++) {
        const mv_seq_ev_t *e = &s.t[0].ev[i];
        if (!(e->vel & MV_SEQ_PT) || (e->note & 127u) != 74u) continue;
        if (e->tick < 48u) k += (e->vel & 127u) != 127u; else m++;
    }
    CHECK(k == 0, "touch to replace: no old cc 74 point left in the turned span");
    CHECK(m > 0, "and its points after the span kept (%u)", m);
    /* 4. the budget: notes + points within 256, every note keeps its note-off */
    start(4);
    for (i = 0; i < 300u; i++) {
        if (i % 3u == 0u) mv_seq_key(&s, set, 0, 40u + i % 20u, 100);
        mv_seq_point(&s, 0, 74, i & 127u);
        run_ticks(MV_SEQ_PT_GAP);
        if (i % 3u == 0u) mv_seq_key(&s, set, 0, 40u + i % 20u, 0);
    }
    mv_seq_disarm(&s, 0);
    CHECK(s.t[0].n <= MV_SEQ_NEV && sorted_and_paired(&s.t[0]), "full track: %u events, sorted, every note paired", s.t[0].n);
    /* 5. CLEAR NOTES / AUTO / ALL */
    start(2);
    mv_seq_key(&s, set, 0, 60, 100); run_ticks(24); mv_seq_key(&s, set, 0, 60, 0);
    for (i = 0; i < 40u; i++) { mv_seq_point(&s, 0, 74, i); run_ticks(1); }
    mv_seq_disarm(&s, 0);
    m = points(0, 74);
    CHECK(m > 0 && s.t[0].n == m + 2u, "a note and %u points", m);
    mv_seq_clear(&s, 0, MV_SQC_NOTES, emit, 0);
    CHECK(s.t[0].n == m && points(0, 74) == m && s.t[0].root == MV_SEQ_NOROOT, "CLEAR NOTES: the points stay, no ROOT");
    mv_seq_key(&s, set, 0, 62, 100); run_ticks(24); mv_seq_key(&s, set, 0, 62, 0);
    mv_seq_clear(&s, 0, MV_SQC_AUTO, emit, 0);
    CHECK(s.t[0].n == 2u && points(0, 74) == 0u && s.t[0].root == 62u, "CLEAR AUTO: the note stays, its ROOT too");
    mv_seq_clear(&s, 0, MV_SQC_ALL, emit, 0);
    CHECK(s.t[0].n == 0u, "CLEAR ALL: empty");
    /* 6. a flash clip with points: the coding's flag (velocity byte with bit 7: a point; the notes' velocity kept) */
    {
        static const uint8_t rom[] = {
            0u << 1 | 1u, 60u | 128u, 90u,      /* tick 0: note-on 60, velocity 90 */
            8u << 1 | 1u, 74u | 128u, 128u | 33u,   /* tick 8: point cc 74 = 33 */
            8u << 1 | 0u, 62u | 128u,           /* tick 16: note-on 62 at the running velocity (90, not 33) */
            40u << 1 | 0u, 60u,                 /* tick 56: offs */
            0u << 1 | 0u, 62u };
        mv_seq_rd_t r;
        uint32_t t, n, v, got[5][3];
        mv_seq_rd_start(&r, rom, sizeof rom);
        for (k = 0; k < 5u && mv_seq_rd_next(&r, &t, &n, &v); k++) { got[k][0] = t; got[k][1] = n; got[k][2] = v; }
        CHECK(k == 5u && got[1][0] == 8u && got[1][1] == 74u && got[1][2] == (128u | 33u), "a flash point read as one");
        CHECK(got[2][2] == 90u, "the next note keeps the notes' velocity (%u)", got[2][2]);
        start(1);
        mv_seq_load_rom(&s, 0, rom, sizeof rom, 5, 60, emit, 0);
        nl = 0; run_ticks(MV_SEQ_BAR);
        CHECK(played(74, 0) == 1u && s.t[0].root == 60u, "a flash clip's point played (%u)", played(74, 0));
        mv_seq_unrom(&s, 0);
        CHECK(points(0, 74) == 1u && s.t[0].n == 5u, "unrom keeps the point");
    }
    /* 7. disarm: a value the gap held back lands */
    start(1);
    mv_seq_point(&s, 0, 74, 1); run_ticks(1);
    mv_seq_point(&s, 0, 74, 77);                /* (within the gap: held back) */
    mv_seq_disarm(&s, 0);
    for (i = k = 0; i < s.t[0].n; i++) k += (s.t[0].ev[i].vel & 127u) == 77u;
    CHECK(k == 1u && s.t[0].tch[0].used == 0, "disarm: the held value written, the knob let go");
    printf("mv_seq_auto: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
