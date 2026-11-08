/* SPDX-License-Identifier: GPL-3.0-only */
/* mv_clip (sequencer clips, firmware/src/mv_clip.c) against a RAM stand-in for the flash sectors: an empty store,
 * SAVE (shown at once, written when flushed), LOAD from RAM before the write and from flash after it, a record's
 * neighbours in its sector kept, one SAVE waiting at a time, a failed write, skip marks and events past LENGTH
 * dropped, and a full round trip through mv_seq_load into the other track.
 *   cc -std=c99 -O2 -Wall -Ifirmware/src -Ifirmware/hal -Ifirmware/gen tests/mv_clip_test.c -o mv_clip_test */
#include <stdio.h>
#include <string.h>
#include "store.h"

/* the flash: SLOT_SECT sectors of slot MV_CLIP_SLOT (platform.c slot_sec_*: a load needs the length to match) */
static uint8_t fsec[NSLOT][SLOT_SECT][SECTOR_MAX];
static uint32_t flen[NSLOT][SLOT_SECT], writes, fail_next;
static int slot_sec_load(uint32_t i, uint32_t k, void *p, uint32_t n)
{
    if (i >= NSLOT || k >= SLOT_SECT || !flen[i][k] || flen[i][k] != n) return -1;
    memcpy(p, fsec[i][k], n);
    return 0;
}
static int slot_sec_save(uint32_t i, uint32_t k, const void *p, uint32_t n)
{
    if (fail_next) { fail_next = 0; flen[i][k] = 0; return -5; }   /* (an erase that went, a write that did not) */
    memcpy(fsec[i][k], p, n); flen[i][k] = n; writes++;
    return 0;
}

#include "mv_seq.c"
#include "mv_voice.h"
/* a factory set of the test's own (not whatever firmware/gen/mv_fclips_local.h holds): two clips */
#define MV_FCLIPS_TEST
#include "mv_fclips.h"
/* coded (mv_seq.h FLASH): C2 on at 0 velocity 90 (a change), off at 24; G2 on at 384 (90 still), off at 408 */
static const uint8_t FEV0[10] = {1, 36 | 128, 90, 48, 36, 208, 5, 43 | 128, 48, 43};
#define MV_FCLIP_N 2u
static const mv_fclip_t MV_FCLIPS[MV_FCLIP_N] = {{"TEST BASS", 2, 36, 4, 10, FEV0}, {"EMPTY", 1, 0xFF, 0, 0, FEV0}};
#include "mv_clip.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                  printf(__VA_ARGS__); printf("\n"); } } while (0)

static mv_engine_t e;                           /* (only its sequencer parts are used) */
static mv_clip_t c;
static void emit(void *ctx, uint32_t part, uint32_t note, uint32_t vel) { (void)ctx; (void)part; (void)note; (void)vel; }

/* track part: n notes, a 1/8 apart, 12 ticks long, from note base */
static void fill(uint32_t part, uint32_t n, uint32_t base)
{
    mv_seq_track_t *T = &e.seq.t[part];
    uint32_t i;
    T->n = 0;
    for (i = 0; i < n; i++) {
        T->ev[T->n].tick = (uint16_t)(i * 48u); T->ev[T->n].note = (uint8_t)(base + i); T->ev[T->n++].vel = 100;
        T->ev[T->n].tick = (uint16_t)(i * 48u + 12u); T->ev[T->n].note = (uint8_t)(base + i); T->ev[T->n++].vel = 0;
    }
    T->root = (uint8_t)base;
    e.seq.ver++;
}

int main(void)
{
    uint32_t k, i, same;
    mv_seq_init(&e.seq);
    for (k = 0; k < MV_SQ_N; k++) e.seqb[k] = MV_SQ_DEF[k];
    e.seq_load = 0;

    /* 1. an empty store */
    mv_clip_init(&c);
    for (k = 0, same = 0; k < MV_CLIP_N; k++) same += c.dir[k].used;
    CHECK(MV_CLIP_N == 12u && same == 0u && !mv_clip_dirty(&c), "12 clips, all empty");
    CHECK(mv_clip_load(&c, &e, 0, 0) == MV_CLIP_EMPTY, "LOAD of an empty clip: nothing");

    /* 2. SAVE: shown at once, waits; LOAD before the write comes from RAM */
    fill(0, 8, 48);
    e.seq.t[0].ev[2].note |= MV_SEQ_SKIP;       /* (a skip mark: not saved) */
    e.seqb[MV_SQ_LENUP] = 1;                    /* 1 bar */
    CHECK(mv_clip_save(&c, &e, 0, 4, "BASS 1") == MV_CLIP_OK, "SAVE clip 5");
    CHECK(c.dir[4].used && c.dir[4].n == 16u && c.dir[4].len == 1u && c.dir[4].root == 48u && !memcmp(c.dir[4].name, "BASS 1", 6),
          "clip 5 shown: 16 events, 1 bar, ROOT C3, its name");
    CHECK(mv_clip_dirty(&c) && writes == 0u, "waits for silence (nothing written yet)");
    CHECK(mv_clip_save(&c, &e, 0, 5, "OTHER ") == MV_CLIP_BUSY, "another SAVE while one waits: busy");
    e.seqb[MV_SQ_LENLO] = 3;
    CHECK(mv_clip_load(&c, &e, 4, 1) == MV_CLIP_OK && e.seq_load == 2u && e.seq_ln == 16u && e.seq_lroot == 48u,
          "LOAD before the write: from RAM, into LOWER");
    CHECK(e.seqb[MV_SQ_LENLO] == 1u, "LOAD sets that track's LENGTH (%u)", e.seqb[MV_SQ_LENLO]);
    CHECK(e.seq_lbuf[2].note == 49u && e.seq_lbuf[2].tick == 48u, "the skip mark is not saved");
    CHECK(mv_clip_load(&c, &e, 4, 1) == MV_CLIP_AGAIN, "a second LOAD before the ISR took the first: again");
    mv_seq_load(&e.seq, 1, e.seq_lbuf, e.seq_ln, e.seq_lroot, emit, 0); e.seq_load = 0;   /* (as the ISR does) */
    for (i = 0, same = e.seq.t[1].n == 16u; i < 16u && same; i++)
        same = e.seq.t[1].ev[i].tick == e.seq.t[0].ev[i].tick && e.seq.t[1].ev[i].vel == e.seq.t[0].ev[i].vel &&
               e.seq.t[1].ev[i].note == (e.seq.t[0].ev[i].note & 127u);
    CHECK(same && e.seq.t[1].root == 48u && e.seq.t[1].xpose == 0, "round trip: LOWER now holds UPPER's notes");

    /* 3. the write: one sector; its neighbours kept */
    CHECK(mv_clip_flush(&c) == 0 && writes == 1u && !mv_clip_dirty(&c), "flushed: one sector written");
    fill(0, 3, 60);
    CHECK(mv_clip_save(&c, &e, 0, 3, "LEAD  ") == MV_CLIP_OK && mv_clip_flush(&c) == 0, "SAVE clip 4 (same sector)");
    mv_clip_init(&c);                           /* (as after a power-off) */
    CHECK(c.dir[3].used && c.dir[3].n == 6u && c.dir[4].used && c.dir[4].n == 16u && !c.dir[5].used,
          "after reading flash again: clips 4 and 5 both there, 6 empty");
    CHECK(mv_clip_load(&c, &e, 4, 0) == MV_CLIP_OK && e.seq_ln == 16u && e.seq_lbuf[15].tick == 7u * 48u + 12u,
          "LOAD from flash");
    e.seq_load = 0;

    /* 4. events past LENGTH are not saved (a loop shortened after recording) */
    fill(0, 8, 40);                             /* ticks up to 348 */
    e.seqb[MV_SQ_LENUP] = 1;                    /* (all inside one bar: 384) */
    e.seq.t[0].ev[15].tick = 500;               /* one past the loop */
    CHECK(mv_clip_save(&c, &e, 0, 0, "CUT   ") == MV_CLIP_OK && c.dir[0].n == 15u, "the one past LENGTH dropped (%u)", c.dir[0].n);

    /* 5. a failed write: the SAVE goes, err set, the directory as flash has it (sector 0: nothing) */
    fail_next = 1;
    CHECK(mv_clip_flush(&c) != 0 && c.err && !mv_clip_dirty(&c) && !c.dir[0].used, "a failed write: err, clip 1 empty");
    CHECK(c.dir[4].used, "other sectors untouched");

    /* 6. the same clip saved again while it waits: replaced, not busy */
    fill(0, 2, 70);
    CHECK(mv_clip_save(&c, &e, 0, 7, "A     ") == MV_CLIP_OK && mv_clip_save(&c, &e, 0, 7, "B     ") == MV_CLIP_OK &&
          c.dir[7].name[0] == 'B', "the waiting clip saved again: replaced");
    CHECK(mv_clip_flush(&c) == 0, "and written");

    /* 7. factory clips (Kris 7 Nov): after the user ones, LOAD only */
    CHECK(MV_CLIP_ALL == 14u && mv_clip_factory(MV_CLIP_N) == &MV_FCLIPS[0] && !mv_clip_factory(3), "F1 is clip 13");
    CHECK(mv_clip_save(&c, &e, 0, MV_CLIP_N, "NOPE  ") == MV_CLIP_RO, "SAVE onto a factory clip: read-only");
    e.seqb[MV_SQ_LENUP] = 4;
    CHECK(mv_clip_load(&c, &e, MV_CLIP_N, 0) == MV_CLIP_OK && e.seq_load == 1u && e.seq_ln == 4u && e.seq_lroot == 36u &&
          e.seq_lrom == FEV0 && e.seq_lbytes == 10u && e.seqb[MV_SQ_LENUP] == 2u,
          "LOAD F1 into UPPER: from flash (no copy), 4 events, ROOT C2, 2 bars");
    mv_seq_load_rom(&e.seq, 0, e.seq_lrom, e.seq_lbytes, e.seq_ln, e.seq_lroot, emit, 0); e.seq_load = 0;   /* (the ISR) */
    {   /* its events as coded: C2 90 at 0, off 24, G2 90 at 384, off 408 */
        mv_seq_rd_t rd; uint32_t t, n, v, ok = 1, k2 = 0;
        static const uint32_t W[4][3] = {{0, 36, 90}, {24, 36, 0}, {384, 43, 90}, {408, 43, 0}};
        mv_seq_rd_start(&rd, FEV0, 10);
        while (mv_seq_rd_next(&rd, &t, &n, &v)) { ok &= k2 < 4u && t == W[k2][0] && n == W[k2][1] && v == W[k2][2]; k2++; }
        CHECK(ok && k2 == 4u, "its events as coded (%u)", k2);
    }
    CHECK(mv_clip_save(&c, &e, 0, 9, "COPY  ") == MV_CLIP_OK && c.dir[9].n == 4u && c.dir[9].len == 2u,
          "SAVE of a track playing F1: decoded into clip 10 (4 events, 2 bars)");
    CHECK(mv_clip_flush(&c) == 0, "and written");
    CHECK(mv_clip_load(&c, &e, MV_CLIP_N + 1u, 1) == MV_CLIP_EMPTY && !e.seq_load, "an empty factory clip: nothing");
    CHECK(mv_clip_load(&c, &e, MV_CLIP_ALL, 1) == MV_CLIP_EMPTY, "past the last one: nothing");

    /* 8. a record from before 7 Nov (format 1: LENGTH a 1..4 code) reads as its bars: code 4 = 8 bars */
    {
        uint8_t old[MV_CLIP_SEC];
        mv_clip_info_t d;
        memset(old, 0, sizeof old);
        old[0] = 'C'; old[1] = 1; old[2] = 4; old[3] = 48; old[6] = 2;
        CHECK(clip_head(old, &d) && d.len == 8u && d.n == 2u, "format 1, code 4: 8 bars (%u)", d.len);
        old[1] = 2; old[2] = 11;
        CHECK(clip_head(old, &d) && d.len == 11u, "format 2: 11 bars");
        old[2] = 33;
        CHECK(!clip_head(old, &d), "format 2, 33 bars: not a clip");
    }
    /* 9. SAVE of a flash clip longer than a RAM track holds: refused */
    {
        static uint8_t big[2000];
        uint32_t k2, nb = 0;
        for (k2 = 0; k2 < 300u; k2++) { big[nb++] = 2; big[nb++] = (uint8_t)(60u | 128u); big[nb++] = 2; big[nb++] = 60; }   /* 600 events */
        e.seqb[MV_SQ_LENUP] = 8;
        mv_seq_load_rom(&e.seq, 0, big, nb, 600, 60, emit, 0);
        CHECK(mv_clip_save(&c, &e, 0, 10, "BIG   ") == MV_CLIP_LONG && !mv_clip_dirty(&c), "600 events: too long to save");
    }

    printf("mv_clip_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
