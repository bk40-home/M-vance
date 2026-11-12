/* SPDX-License-Identifier: GPL-3.0-only */
/* mv_prog with ONE part (MV_PARTS 1, MV_PROGS 64: the fm1-dock module's programs, Kris 12 Nov) on a RAM flash: all 64
 * factory programs as singles on a first boot; write / swap / factory in RAM; a save writes only the records that
 * changed (records of 32 programs, 32 x 252 = 8064 B: one module bank each), one a call; what was saved comes back on
 * the next boot; a record saved by the 32-program build (the first module, 12 Nov) is read as programs 1-32; only the
 * kept performance bytes survive, and a damaged record's bytes are held in range.
 *   cc -std=c99 -O2 -Wall -Wextra -Werror -Ifirmware/src -Ifirmware/gen tests/mv_prog1_test.c -o mv_prog1_test */
#define MV_PARTS 1
#define MV_XFER 0
#define MV_PROGS 64u
#include <stdio.h>
#include <string.h>
#include "store.h"                              /* NSLOT, SLOT_MAX */
static uint8_t sflash[NSLOT][SLOT_MAX];
static uint32_t slen[NSLOT], ssaves[NSLOT];
static int slot_load(uint32_t i, void *p, uint32_t n)
{
    if (i >= NSLOT || slen[i] != n) return -1;  /* (as store.h: a size change reads as nothing saved) */
    memcpy(p, sflash[i], n);
    return 0;
}
static int slot_save(uint32_t i, const void *p, uint32_t n)
{
    if (i >= NSLOT || n > SLOT_MAX) return -1;
    memcpy(sflash[i], p, n); slen[i] = n; ssaves[i]++;
    return 0;
}
#include "mv_patch.c"
#include "mv_prog.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                  printf(__VA_ARGS__); printf("\n"); } } while (0)
static mv_prog_t g, h;

/* what a single keeps of a factory program's performance bytes */
static int is_single_of(const uint8_t *pf, const uint8_t *src)
{
    uint32_t i, k;
    for (i = 0; i < MV_PROG_PERF; i++) {
        int kept = 0;
        for (k = 0; k < KEEP_N; k++) kept |= KEEP[k] == i;
        if (pf[i] != (kept ? src[i] : i < MV_PF_N ? MV_PERF_DEF[i] : 0u)) return 0;
    }
    return 1;
}
static uint32_t saves(void) { uint32_t i, n = 0; for (i = 0; i < NSLOT; i++) n += ssaves[i]; return n; }

int main(void)
{
    mv_patch_t p;
    uint8_t pf[MV_PROG_PERF];
    uint32_t i, layered = 0;
    CHECK(MV_NPROG == 64u && MV_PROG_NREC == 2u, "64 programs, 2 records (%u, %u)", (unsigned)MV_NPROG, (unsigned)MV_PROG_NREC);
    CHECK(REC_BYTES == 8064u && REC_BYTES <= 8176u, "a record: %u B, one module bank (8176)", (unsigned)REC_BYTES);
    mv_prog_load(&g);
    CHECK(g.from_flash == 0u && !mv_prog_dirty(&g), "first boot: nothing in flash, nothing to save");
    for (i = 0; i < MV_NPROG; i++) {
        CHECK(!memcmp(mv_prog_get(&g, i)->b, MV_FACTORY[i], MV_PATCH_BYTES), "program %u: the factory's upper patch", i + 1u);
        CHECK(is_single_of(g.perf[i], MV_FACTORY_PERF[i]) && g.perf[i][MV_PF_MODE] == MV_KEY_SINGLE,
              "program %u: a single, its kept bytes the factory's", i + 1u);
        CHECK(mv_prog_lower(&g, i) == mv_prog_get(&g, i), "program %u: LOWER is UPPER", i + 1u);
        if (MV_FACTORY_PERF[i][MV_PF_MODE] != MV_KEY_SINGLE) layered++;
    }
    printf("factory programs that are DUAL / SPLIT in the two-part firmware (they play their UPPER only): %u\n",
           (unsigned)layered);
    for (i = 0; i < MV_NPROG; i++)
        if (MV_FACTORY_PERF[i][MV_PF_MODE] != MV_KEY_SINGLE)
            printf("  %2u %.16s (%s)\n", (unsigned)(i + 1u), (const char *)MV_FACTORY[i],
                   MV_FACTORY_PERF[i][MV_PF_MODE] == MV_KEY_DUAL ? "DUAL" : "SPLIT");

    /* WRITE into program 20 (record 0) with a DUAL / LOWER-only performance asked for: kept as a single */
    p = *mv_prog_get(&g, 2); memcpy(p.b, "MY TEST PATCH   ", 16); p.b[41] = 10;
    for (i = 0; i < MV_PROG_PERF; i++) pf[i] = 0;
    pf[MV_PF_MODE] = MV_KEY_DUAL; pf[MV_PF_SPLIT] = 40; pf[MV_PF_SYNCUP] = 5; pf[MV_PF_SYNCLO] = 9; pf[MV_PF_DSYNUP] = 3;
    pf[MV_PF_CSYNUP] = 23; pf[MV_PF_CHUP] = 4; pf[MV_PF_CHLO] = 7;
    CHECK(mv_prog_edited(&g, 19, &p, &p, g.perf[19]), "an edit shows as edited");
    mv_prog_write(&g, 19, &p, &p, pf);
    CHECK(g.dirty == 1u && !memcmp(mv_prog_get(&g, 19)->b, p.b, MV_PATCH_BYTES), "written into 20 in RAM: record 0 to save");
    CHECK(g.perf[19][MV_PF_MODE] == MV_KEY_SINGLE && g.perf[19][MV_PF_SPLIT] == MV_PERF_DEF[MV_PF_SPLIT] &&
          g.perf[19][MV_PF_SYNCLO] == 0u && g.perf[19][MV_PF_CHLO] == 0u, "the two-part bytes: a single's");
    CHECK(g.perf[19][MV_PF_SYNCUP] == 5u && g.perf[19][MV_PF_DSYNUP] == 3u && g.perf[19][MV_PF_CSYNUP] == 23u &&
          g.perf[19][MV_PF_CHUP] == 4u, "the kept bytes: as written");
    CHECK(!mv_prog_edited(&g, 19, &p, &p, g.perf[19]), "what was written is not edited");
    CHECK(mv_prog_flush(&g) == 0 && ssaves[MV_PROG_ISLOT] == 1u && saves() == 1u && slen[MV_PROG_ISLOT] == 8064u,
          "the save: record 0 only, once (%u B)", (unsigned)slen[MV_PROG_ISLOT]);
    CHECK(mv_prog_flush(&g) == 1 && !mv_prog_dirty(&g), "nothing more to save");

    /* a write into record 1 (program 50): only record 1 saved */
    mv_prog_write(&g, 49, &p, &p, pf);
    CHECK(g.dirty == 2u, "written into 50: record 1 to save");
    CHECK(mv_prog_flush(&g) == 0 && ssaves[MV_PROG_ISLOT + 1u] == 1u && saves() == 2u, "the save: record 1 only");
    /* SWAP across the records (1 and 64): both saved, one a call */
    mv_prog_swap(&g, 0, 63);
    CHECK(!memcmp(mv_prog_get(&g, 0)->b, MV_FACTORY[63], MV_PATCH_BYTES) &&
          !memcmp(mv_prog_get(&g, 63)->b, MV_FACTORY[0], MV_PATCH_BYTES) && g.dirty == 3u, "swap 1 and 64: both records");
    CHECK(mv_prog_flush(&g) == 0 && mv_prog_flush(&g) == 0 && mv_prog_flush(&g) == 1 && saves() == 4u,
          "the saves: one record a call, two in all");
    mv_prog_factory(&g, 63);
    CHECK(!memcmp(mv_prog_get(&g, 63)->b, MV_FACTORY[63], MV_PATCH_BYTES) && is_single_of(g.perf[63], MV_FACTORY_PERF[63]) &&
          g.dirty == 2u, "64 back to the factory's: record 1");
    (void)mv_prog_flush(&g);

    /* the next boot: every program back as saved */
    mv_prog_load(&h);
    CHECK(h.from_flash == 3u && !mv_prog_dirty(&h), "next boot: both records from flash");
    CHECK(!memcmp(h.rec, g.rec, sizeof g.rec) && !memcmp(h.perf, g.perf, sizeof g.perf),
          "every program back as saved (patches and performance bytes)");

    /* the first module's bank (32 programs, 12 Nov): only record 0 saved: programs 1-32 from it, 33-64 the factory's */
    memset(ssaves, 0, sizeof ssaves);
    slen[MV_PROG_ISLOT + 1u] = 0;
    mv_prog_load(&h);
    CHECK(h.from_flash == 1u && !memcmp(h.rec[0].p, g.rec[0].p, sizeof g.rec[0].p) &&
          !memcmp(mv_prog_get(&h, 49)->b, MV_FACTORY[49], MV_PATCH_BYTES), "a 32-program bank: 1-32 kept, 33-64 the factory's");
    CHECK(!mv_prog_dirty(&h) && saves() == 0u, "and nothing written by the load");

    /* a damaged record: a kept byte out of range is held to a single's default */
    sflash[MV_PROG_ISLOT][MV_PROG_PER_REC * MV_PATCH_BYTES + 5u * MV_PROG_PK + 0u] = 200;   /* program 6's LFO1 SYNC */
    mv_prog_load(&h);
    CHECK(h.perf[5][MV_PF_SYNCUP] == 0u && mv_prog_perf_ok(h.perf[5]), "a damaged byte: held in range");
    /* a record of another size: the factory set's for those programs */
    slen[MV_PROG_ISLOT] = 1024u;
    mv_prog_load(&h);
    CHECK(h.from_flash == 0u && !memcmp(mv_prog_get(&h, 19)->b, MV_FACTORY[19], MV_PATCH_BYTES), "another size: the factory set");
    /* FACTORY ALL: every program the factory's, both records saved */
    mv_prog_factory_all(&g);
    CHECK(g.dirty == 3u && mv_prog_flush(&g) == 0 && mv_prog_flush(&g) == 0 && !mv_prog_dirty(&g), "factory all: two saves");
    CHECK(mv_prog_factory_id() != 0u, "the bank's id");

    printf("mv_prog1_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
