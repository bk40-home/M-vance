/* SPDX-License-Identifier: GPL-3.0-only */
/* mv_prog (the 64 programs, firmware/src/mv_prog.c) with a RAM flash: the factory set on a first boot, write / swap /
 * factory in RAM, a save writes only the changed 16-program blobs, and what was saved comes back on the next boot.
 * Performances (Kris 3 Nov): the lower patches (blobs 4..7) and the performance bytes (slot 0) go with them; a single
 * saved before them reads as it was, its lower a copy of its upper.
 *   cc -std=c99 -O2 -Wall -Ifirmware/src -Ifirmware/gen tests/mv_prog_test.c -o mv_prog_test && ./mv_prog_test */
#include <stdio.h>
#include <string.h>
#include "store.h"                              /* NBLOB, SECTOR_MAX */
static uint8_t flash[NBLOB][SECTOR_MAX];
static uint32_t flen[NBLOB], saves[NBLOB];
static int blob_load(uint32_t i, void *p, uint32_t n)
{
    if (i >= NBLOB || flen[i] != n) return -1;  /* (as store.h: a size change reads as nothing saved) */
    memcpy(p, flash[i], n);
    return 0;
}
static int blob_save(uint32_t i, const void *p, uint32_t n)
{
    if (i >= NBLOB || n > SECTOR_MAX) return -1;
    memcpy(flash[i], p, n); flen[i] = n; saves[i]++;
    return 0;
}
static uint8_t sflash[NSLOT][1024];
static uint32_t slen[NSLOT], ssaves[NSLOT];
static int slot_load(uint32_t i, void *p, uint32_t n)
{
    if (i >= NSLOT || slen[i] != n || n > sizeof sflash[0]) return -1;
    memcpy(p, sflash[i], n);
    return 0;
}
static int slot_save(uint32_t i, const void *p, uint32_t n)
{
    if (i >= NSLOT || n > sizeof sflash[0]) return -1;
    memcpy(sflash[i], p, n); slen[i] = n; ssaves[i]++;
    return 0;
}
#include "mv_patch.c"
#include "mv_prog.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                  printf(__VA_ARGS__); printf("\n"); } } while (0)
static mv_prog_t g, h;

int main(void)
{
    mv_patch_t p, lo;
    uint8_t pf[MV_PF_N];
    uint32_t i;
    mv_prog_load(&g);
    CHECK(g.from_flash == 0u && !mv_prog_dirty(&g), "first boot: nothing in flash, nothing to save");
    for (i = 0; i < MV_NPROG; i++)
        CHECK(!memcmp(g.bank[i].b, MV_FACTORY[i], MV_PATCH_BYTES) && !memcmp(g.lower[i].b, MV_FACTORY_LO[i], MV_PATCH_BYTES) &&
              !memcmp(g.perf[i], MV_FACTORY_PERF[i], MV_PF_N), "program %u = factory (upper, lower, perf)", i + 1u);
    CHECK(!memcmp(g.bank[0].b, MV_FACTORY[0], 16) && !memcmp(g.bank[63].b, MV_FACTORY[63], 16),
          "the bank's first and last names");    /* (by the factory table: the bank's contents may change) */
    /* WRITE: an edited performance into slot 20 (blob 1, lower blob 5, the perf slot) */
    p = *mv_prog_get(&g, 2); memcpy(p.b, "MY TEST PATCH   ", 16); p.b[41] = 10;
    lo = *mv_prog_lower(&g, 2); lo.b[41] = 99;
    memcpy(pf, mv_prog_perf(&g, 2), MV_PF_N);
    CHECK(mv_prog_edited(&g, 2, &p, &lo, pf) && !mv_prog_edited(&g, 2, mv_prog_get(&g, 2), mv_prog_lower(&g, 2), mv_prog_perf(&g, 2)),
          "edited: differs from its slot");
    {   /* (a SINGLE program of the factory set: whichever bank was built, tools/make_bank.py) */
        uint32_t s1 = 0;
        mv_patch_t lo1;
        while (s1 < MV_NPROG && MV_FACTORY_PERF[s1][MV_PF_MODE] != MV_KEY_SINGLE) s1++;
        lo1 = *mv_prog_lower(&g, s1); lo1.b[41] ^= 1u;
        CHECK(s1 < MV_NPROG && !mv_prog_edited(&g, s1, mv_prog_get(&g, s1), &lo1, mv_prog_perf(&g, s1)),
              "a SINGLE: the lower patch does not count");
    }
    pf[MV_PF_MODE] = MV_KEY_SPLIT; pf[MV_PF_SPLIT] = 55;
    CHECK(mv_prog_edited(&g, 2, mv_prog_get(&g, 2), mv_prog_lower(&g, 2), pf), "the performance bytes count");
    mv_prog_write(&g, 19, &p, &lo, pf);
    CHECK(g.dirty == (2u | 0x20u | MV_PROG_PDIRTY), "WRITE to 20: blobs 1 and 5, the perf slot marked (%x)", g.dirty);
    /* SWAP 1 and 64 (blobs 0 and 3, lower 4 and 7) */
    mv_prog_swap(&g, 0, 63);
    CHECK(!memcmp(g.bank[0].b, MV_FACTORY[63], 16) && !memcmp(g.bank[63].b, MV_FACTORY[0], 16), "SWAP 1 <-> 64");
    CHECK(g.dirty == (0xBu | 0xB0u | MV_PROG_PDIRTY), "blobs 0 1 3, 4 5 7, perf marked (%x)", g.dirty);
    /* save: one record a call, only the marked ones */
    while (mv_prog_flush(&g) == 0) {}
    CHECK(!mv_prog_dirty(&g) && saves[0] == 1u && saves[1] == 1u && saves[2] == 0u && saves[3] == 1u &&
          saves[4] == 1u && saves[5] == 1u && saves[6] == 0u && saves[7] == 1u && ssaves[0] == 1u,
          "saved: blobs 0 1 3 4 5 7 and the perf slot once each, 2 and 6 untouched");
    /* the next boot */
    mv_prog_load(&h);
    CHECK(h.from_flash == (0xBu | 0xB0u | MV_PROG_PDIRTY), "reload: 6 blobs and the perf from flash (%x)", h.from_flash);
    CHECK(!memcmp(h.bank[19].b, p.b, MV_PATCH_BYTES) && !memcmp(h.lower[19].b, lo.b, MV_PATCH_BYTES) &&
          h.perf[19][MV_PF_MODE] == MV_KEY_SPLIT && h.perf[19][MV_PF_SPLIT] == 55u, "reload: slot 20 as written (both patches, SPLIT 55)");
    CHECK(!memcmp(h.bank[0].b, MV_FACTORY[63], 16) && !memcmp(h.bank[63].b, MV_FACTORY[0], 16), "reload: the swap");
    /* FACTORY: slot 20 back */
    mv_prog_factory(&h, 19);
    CHECK(!memcmp(h.bank[19].b, MV_FACTORY[19], MV_PATCH_BYTES) && !memcmp(h.perf[19], MV_FACTORY_PERF[19], MV_PF_N) &&
          h.dirty == (2u | 0x20u | MV_PROG_PDIRTY), "FACTORY: slot 20 restored, its records marked");
    /* a flash with singles only (saved before the performance): upper as saved, lower = upper, SINGLE */
    memset(flen, 0, sizeof flen); memset(slen, 0, sizeof slen);
    p = *mv_prog_get(&g, 5); memcpy(p.b, "OLD SINGLE      ", 16);
    {
        static mv_patch_t blob[16];
        for (i = 0; i < 16u; i++) blob[i] = *mv_prog_get(&g, i);
        blob[5] = p;
        blob_save(0, blob, sizeof blob);
    }
    mv_prog_load(&h);
    CHECK(!memcmp(h.bank[5].b, p.b, MV_PATCH_BYTES) && !memcmp(h.lower[5].b, p.b, MV_PATCH_BYTES) &&
          !memcmp(h.perf[5], MV_PERF_DEF, MV_PF_N) && !memcmp(h.lower[20].b, MV_FACTORY_LO[20], MV_PATCH_BYTES),
          "an old flash: the saved single as it was, its lower a copy, SINGLE; the factory's elsewhere");
    /* the factory bank (A1): its id is stable and not 0; FACTORY ALL puts all 64 back and marks every record */
    CHECK(mv_prog_saved(&h) && mv_prog_factory_id() == mv_prog_factory_id() && mv_prog_factory_id() != 0u,
          "a saved flash; the bank's id the same each call, never 0");
    h.dirty = 0;
    mv_prog_factory_all(&h);
    for (i = 0; i < MV_NPROG; i++)
        if (memcmp(h.bank[i].b, MV_FACTORY[i], MV_PATCH_BYTES) || memcmp(h.lower[i].b, MV_FACTORY_LO[i], MV_PATCH_BYTES) ||
            memcmp(h.perf[i], MV_FACTORY_PERF[i], MV_PROG_PERF)) break;
    CHECK(i == MV_NPROG && h.dirty == (0xFFu | MV_PROG_PDIRTY), "FACTORY ALL: all 64 the factory's, 8 blobs + perf marked (%x)", h.dirty);
    {   /* LFO1 SYNC bytes (Kris 5 Nov): 0..22 kept, 23 refused; the factory's slot 37 (BS Downward) at 1/4D */
        uint8_t t[MV_PF_N];
        memcpy(t, MV_PERF_DEF, MV_PF_N);
        CHECK(mv_prog_perf_ok(t) && t[MV_PF_SYNCUP] == 0 && t[MV_PF_SYNCLO] == 0, "the default: sync OFF");
        t[MV_PF_SYNCUP] = 22; t[MV_PF_SYNCLO] = 22;
        CHECK(mv_prog_perf_ok(t), "sync 8 MEAS kept");
        t[MV_PF_SYNCLO] = 23;
        CHECK(!mv_prog_perf_ok(t), "sync 23 refused");
#if !MV_FACTORY_PUBLIC                          /* the local bank: every slot's LFO1 SYNC must be a valid value
                                                   (0..22). Bank-agnostic, so re-curating the factory can't break it. */
        for (i = 0; i < MV_NPROG; i++)
            if (MV_FACTORY_PERF[i][MV_PF_SYNCUP] >= MV_LFO1_SYNC_N ||
                MV_FACTORY_PERF[i][MV_PF_SYNCLO] >= MV_LFO1_SYNC_N) break;
        CHECK(i == MV_NPROG, "the factory: every slot's LFO1 SYNC is 0..22");
#endif
    }
    {   /* DELAY SYNC 0..10 and CHORUS SYNC RATE 0..23 (Kris 7 Nov): kept; one past the top refused, each byte */
        static const uint8_t K[4] = {MV_PF_DSYNUP, MV_PF_DSYNLO, MV_PF_CSYNUP, MV_PF_CSYNLO};
        uint8_t t[MV_PF_N];
        uint32_t k;
        memcpy(t, MV_PERF_DEF, MV_PF_N);
        t[MV_PF_DSYNUP] = t[MV_PF_DSYNLO] = MV_DSYNC_N - 1u;
        t[MV_PF_CSYNUP] = t[MV_PF_CSYNLO] = MV_CSYNC_N - 1u;
        CHECK(mv_prog_perf_ok(t), "DELAY SYNC 1/2, CHORUS SYNC LFO1 kept");
        for (k = 0; k < 4u; k++) {
            uint8_t keep = t[K[k]];
            t[K[k]] = (uint8_t)(k < 2u ? MV_DSYNC_N : MV_CSYNC_N);
            CHECK(!mv_prog_perf_ok(t), "perf byte %u one past its top: refused", (unsigned)K[k]);
            t[K[k]] = keep;
        }
        for (i = 0; i < MV_NPROG; i++)          /* the factory (either bank): all valid */
            if (!mv_prog_perf_ok(MV_FACTORY_PERF[i])) break;
        CHECK(i == MV_NPROG, "the factory: every slot's performance bytes valid");
    }
    printf("mv_prog_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
