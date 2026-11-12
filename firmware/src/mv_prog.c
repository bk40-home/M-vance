/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance programs: implementation (storage and rules in mv_prog.h). */
#include "mv_prog.h"

#if MV_PARTS == 2                              /* (one part: the record, below) */
#define NBLOBS (MV_NPROG / MV_PROG_PER_BLOB)   /* 4 (and 4 more for the lower patches) */
#define BLOB_BYTES (MV_PROG_PER_BLOB * MV_PATCH_BYTES)   /* 3968 */
#define PERF_BYTES (MV_NPROG * MV_PROG_PERF)   /* 1024 */
_Static_assert(BLOB_BYTES <= SECTOR_MAX, "16 programs in a blob");
_Static_assert(MV_PROG_LBLOB0 + NBLOBS <= NBLOB && MV_PROG_BLOB0 + NBLOBS <= MV_PROG_LBLOB0, "the programs' blobs exist");
_Static_assert(PERF_BYTES <= SLOT_MAX && MV_PROG_PSLOT < NSLOT, "the performance bytes fit a slot");
#endif

static void copy(mv_patch_t *d, const uint8_t *s)
{
    uint32_t i;
    for (i = 0; i < MV_PATCH_BYTES; i++) d->b[i] = s[i];
}
static void copy_perf(uint8_t *d, const volatile uint8_t *s)
{
    uint32_t i;
    for (i = 0; i < MV_PROG_PERF; i++) d[i] = s[i];
}

#if MV_PARTS == 2
void mv_prog_load(mv_prog_t *g)
{
    uint32_t k, i, n;
    g->dirty = 0; g->from_flash = 0;
    for (k = 0; k < NBLOBS; k++) {
        /* straight into the bank (blob_load leaves it alone when it fails: then the factory set) */
        if (blob_load(MV_PROG_BLOB0 + k, &g->bank[k * MV_PROG_PER_BLOB], BLOB_BYTES) == 0) {
            g->from_flash |= (uint16_t)(1u << k);
            for (i = 0; i < MV_PROG_PER_BLOB; i++) mv_patch_sanitize(&g->bank[k * MV_PROG_PER_BLOB + i]);
        } else {
            for (i = 0; i < MV_PROG_PER_BLOB; i++) copy(&g->bank[k * MV_PROG_PER_BLOB + i], MV_FACTORY[k * MV_PROG_PER_BLOB + i]);
        }
        if (blob_load(MV_PROG_LBLOB0 + k, &g->lower[k * MV_PROG_PER_BLOB], BLOB_BYTES) == 0) {
            g->from_flash |= (uint16_t)(1u << (4u + k));
            for (i = 0; i < MV_PROG_PER_BLOB; i++) mv_patch_sanitize(&g->lower[k * MV_PROG_PER_BLOB + i]);
        } else {                                /* not saved yet: the factory's lower for a factory upper, else a copy
                                                   of the saved upper (the first boot with performances) */
            for (i = 0; i < MV_PROG_PER_BLOB; i++) {
                n = k * MV_PROG_PER_BLOB + i;
                if (g->from_flash >> k & 1u) g->lower[n] = g->bank[n];
                else copy(&g->lower[n], MV_FACTORY_LO[n]);
            }
        }
    }
    if (slot_load(MV_PROG_PSLOT, g->perf, PERF_BYTES) == 0) {
        g->from_flash |= MV_PROG_PDIRTY;
        for (n = 0; n < MV_NPROG; n++)          /* held in range (a foreign or damaged record) */
            if (!mv_prog_perf_ok(g->perf[n]))
                copy_perf(g->perf[n], MV_PERF_DEF);
    } else {                                    /* (slot_load reads straight into it: all of it again) */
        for (n = 0; n < MV_NPROG; n++)
            copy_perf(g->perf[n], (g->from_flash >> (n / MV_PROG_PER_BLOB) & 1u) ? MV_PERF_DEF : MV_FACTORY_PERF[n]);
    }
}
#endif

int mv_prog_perf_ok(const uint8_t *pf)
{
    uint32_t i;
    for (i = MV_PF_ARP + 1u; i < MV_PROG_PERF; i++)
        if (pf[i] > 127u) return 0;             /* the spares: 7-bit like the rest (SysEx carries them) */
    return pf[MV_PF_MODE] <= MV_KEY_SPLIT && pf[MV_PF_SPLIT] <= 127u && pf[MV_PF_BAL] <= 127u && pf[MV_PF_VSPLIT] <= 6u &&
           pf[MV_PF_CHLO] <= 16u && pf[MV_PF_CHUP] <= 16u && pf[MV_PF_ARP] <= MV_ARPP_BOTH &&
           pf[MV_PF_SYNCUP] < MV_LFO1_SYNC_N && pf[MV_PF_SYNCLO] < MV_LFO1_SYNC_N &&   /* (older programs: 0, OFF) */
           pf[MV_PF_DSYNUP] < MV_DSYNC_N && pf[MV_PF_DSYNLO] < MV_DSYNC_N &&           /* DELAY / CHORUS SYNC (Kris 7 */
           pf[MV_PF_CSYNUP] < MV_CSYNC_N && pf[MV_PF_CSYNLO] < MV_CSYNC_N;             /* Nov): likewise 0 = OFF */
}

#if MV_PARTS == 2
const mv_patch_t *mv_prog_get(const mv_prog_t *g, uint32_t n) { return &g->bank[n % MV_NPROG]; }
const mv_patch_t *mv_prog_lower(const mv_prog_t *g, uint32_t n) { return &g->lower[n % MV_NPROG]; }
const uint8_t *mv_prog_perf(const mv_prog_t *g, uint32_t n) { return g->perf[n % MV_NPROG]; }

/* a slot's three records marked: its upper blob, its lower blob, the perf slot */
static void mark(mv_prog_t *g, uint32_t n)
{
    uint32_t k = (n % MV_NPROG) / MV_PROG_PER_BLOB;
    g->dirty |= (uint16_t)(1u << k | 1u << (4u + k) | MV_PROG_PDIRTY);
}

void mv_prog_write(mv_prog_t *g, uint32_t n, const mv_patch_t *up, const mv_patch_t *lo, const volatile uint8_t *perf)
{
    n %= MV_NPROG;
    g->bank[n] = *up;
    g->lower[n] = *lo;
    copy_perf(g->perf[n], perf);
    mark(g, n);
}

void mv_prog_swap(mv_prog_t *g, uint32_t a, uint32_t b)
{
    mv_patch_t t;
    uint8_t q[MV_PROG_PERF];
    a %= MV_NPROG; b %= MV_NPROG;
    t = g->bank[a]; g->bank[a] = g->bank[b]; g->bank[b] = t;
    t = g->lower[a]; g->lower[a] = g->lower[b]; g->lower[b] = t;
    copy_perf(q, g->perf[a]); copy_perf(g->perf[a], g->perf[b]); copy_perf(g->perf[b], q);
    mark(g, a); mark(g, b);
}

void mv_prog_factory(mv_prog_t *g, uint32_t n)
{
    n %= MV_NPROG;
    copy(&g->bank[n], MV_FACTORY[n]);
    copy(&g->lower[n], MV_FACTORY_LO[n]);
    copy_perf(g->perf[n], MV_FACTORY_PERF[n]);
    mark(g, n);
}

void mv_prog_factory_all(mv_prog_t *g)
{
    uint32_t n;
    for (n = 0; n < MV_NPROG; n++) {
        copy(&g->bank[n], MV_FACTORY[n]);
        copy(&g->lower[n], MV_FACTORY_LO[n]);
        copy_perf(g->perf[n], MV_FACTORY_PERF[n]);
    }
    g->dirty = (uint16_t)(0xFFu | MV_PROG_PDIRTY);   /* the 8 blobs and the perf slot: flushed one a pass */
}

/* FNV-1a over the upper, the lower and the performance bytes of all 64: 32 KB, once at boot (~0.3 ms). Computed,
 * not generated, so it can never disagree with the bank it names */
uint32_t mv_prog_factory_id(void)
{
    uint32_t h = 2166136261u, n, i;
    for (n = 0; n < MV_NPROG; n++) {
        for (i = 0; i < MV_PATCH_BYTES; i++) h = (h ^ MV_FACTORY[n][i]) * 16777619u;
        for (i = 0; i < MV_PATCH_BYTES; i++) h = (h ^ MV_FACTORY_LO[n][i]) * 16777619u;
        for (i = 0; i < MV_PROG_PERF; i++) h = (h ^ MV_FACTORY_PERF[n][i]) * 16777619u;
    }
    return h ? h : 1u;                          /* (0: "no bank recorded" in the settings) */
}

int mv_prog_edited(const mv_prog_t *g, uint32_t n, const mv_patch_t *up, const mv_patch_t *lo, const volatile uint8_t *perf)
{
    uint32_t i;
    n %= MV_NPROG;
    for (i = 0; i < MV_PATCH_BYTES; i++) if (g->bank[n].b[i] != up->b[i]) return 1;
    if (perf[MV_PF_MODE] != MV_KEY_SINGLE)      /* the lower patch counts only where it plays */
        for (i = 0; i < MV_PATCH_BYTES; i++) if (g->lower[n].b[i] != lo->b[i]) return 1;
    for (i = 0; i < MV_PROG_PERF; i++) if (g->perf[n][i] != perf[i]) return 1;
    return 0;
}

int mv_prog_flush(mv_prog_t *g)
{
    uint32_t k;
    for (k = 0; k < 2u * NBLOBS; k++)
        if (g->dirty >> k & 1u) {
            const mv_patch_t *src = k < NBLOBS ? &g->bank[k * MV_PROG_PER_BLOB] : &g->lower[(k - NBLOBS) * MV_PROG_PER_BLOB];
            uint32_t blob = k < NBLOBS ? MV_PROG_BLOB0 + k : MV_PROG_LBLOB0 + (k - NBLOBS);
            if (blob_save(blob, src, BLOB_BYTES) != 0) return -1;
            g->dirty &= (uint16_t)~(1u << k);
            g->from_flash |= (uint16_t)(1u << k);
            return 0;
        }
    if (g->dirty & MV_PROG_PDIRTY) {
        if (slot_save(MV_PROG_PSLOT, g->perf, PERF_BYTES) != 0) return -1;
        g->dirty &= (uint16_t)~MV_PROG_PDIRTY;
        g->from_flash |= MV_PROG_PDIRTY;
        return 0;
    }
    return 1;
}
#else
/* ---- one part (MV_PARTS 1, the fm1-dock module): the programs as records of 32 (mv_prog.h). The same API: LOWER is
 * UPPER (nothing plays it: every program is SINGLE), and a save writes the records that changed */
#define KEEP_N MV_PROG_PK
static const uint8_t KEEP[KEEP_N] = {MV_PF_SYNCUP, MV_PF_DSYNUP, MV_PF_CSYNUP, MV_PF_CHUP};   /* what a single uses */
#define REC_BYTES (MV_PROG_PER_REC * (MV_PATCH_BYTES + MV_PROG_PK))   /* a record: patches, then the kept bytes */
#define P(g, n) ((g)->rec[(n) / MV_PROG_PER_REC].p[(n) % MV_PROG_PER_REC])   /* program n's patch */
_Static_assert(MV_NPROG % MV_PROG_PER_REC == 0u, "one part: MV_PROGS is a multiple of 32");
_Static_assert(REC_BYTES <= SLOT_MAX && MV_PROG_ISLOT + MV_PROG_NREC <= NSLOT, "the records fit their slots");
_Static_assert(sizeof(mv_prog_rec_t) == REC_BYTES, "a record is its bytes, no padding: stored as it is");

/* a program's performance bytes as a single plays them: the defaults (SINGLE, ARP AUTO, ...), then the kept ones */
static void single_perf(uint8_t *d, const volatile uint8_t *s)
{
    uint32_t i;
    for (i = 0; i < MV_PROG_PERF; i++) d[i] = i < MV_PF_N ? MV_PERF_DEF[i] : 0u;
    for (i = 0; i < KEEP_N; i++) d[KEEP[i]] = s[KEEP[i]];
}

static void factory_one(mv_prog_t *g, uint32_t n)
{
    copy(&P(g, n), MV_FACTORY[n]);
    single_perf(g->perf[n], MV_FACTORY_PERF[n]);
}

void mv_prog_load(mv_prog_t *g)
{
    uint32_t r, k, i, n;
    g->dirty = 0; g->from_flash = 0;
    for (r = 0; r < MV_PROG_NREC; r++) {
        mv_prog_rec_t *q = &g->rec[r];
        if (slot_load(MV_PROG_ISLOT + r, q, REC_BYTES) == 0) {   /* the record as stored */
            g->from_flash |= (uint16_t)(1u << r);
            for (k = 0; k < MV_PROG_PER_REC; k++) {
                uint8_t pf[MV_PROG_PERF];
                n = r * MV_PROG_PER_REC + k;
                mv_patch_sanitize(&q->p[k]);
                for (i = 0; i < MV_PROG_PERF; i++) pf[i] = 0;
                for (i = 0; i < KEEP_N; i++) pf[KEEP[i]] = q->pk[k][i];
                single_perf(g->perf[n], pf);
                if (!mv_prog_perf_ok(g->perf[n])) single_perf(g->perf[n], MV_PERF_DEF);   /* (held in range) */
            }
        } else {                                /* not saved yet (or another size): the factory set's */
            for (k = 0; k < MV_PROG_PER_REC; k++) factory_one(g, r * MV_PROG_PER_REC + k);
        }
    }
}

const mv_patch_t *mv_prog_get(const mv_prog_t *g, uint32_t n) { n %= MV_NPROG; return &P(g, n); }
const mv_patch_t *mv_prog_lower(const mv_prog_t *g, uint32_t n) { n %= MV_NPROG; return &P(g, n); }   /* (= UPPER) */
const uint8_t *mv_prog_perf(const mv_prog_t *g, uint32_t n) { return g->perf[n % MV_NPROG]; }

static void mark(mv_prog_t *g, uint32_t n) { g->dirty |= (uint16_t)(1u << (n / MV_PROG_PER_REC)); }

void mv_prog_write(mv_prog_t *g, uint32_t n, const mv_patch_t *up, const mv_patch_t *lo, const volatile uint8_t *perf)
{
    (void)lo;                                   /* (no LOWER) */
    n %= MV_NPROG;
    P(g, n) = *up;
    single_perf(g->perf[n], perf);
    mark(g, n);
}

void mv_prog_swap(mv_prog_t *g, uint32_t a, uint32_t b)
{
    mv_patch_t t;
    uint8_t q[MV_PROG_PERF];
    a %= MV_NPROG; b %= MV_NPROG;
    t = P(g, a); P(g, a) = P(g, b); P(g, b) = t;
    copy_perf(q, g->perf[a]); copy_perf(g->perf[a], g->perf[b]); copy_perf(g->perf[b], q);
    mark(g, a); mark(g, b);
}

void mv_prog_factory(mv_prog_t *g, uint32_t n)
{
    n %= MV_NPROG;
    factory_one(g, n);
    mark(g, n);
}

void mv_prog_factory_all(mv_prog_t *g)
{
    uint32_t n;
    for (n = 0; n < MV_NPROG; n++) { factory_one(g, n); mark(g, n); }
}

/* FNV-1a over what this build keeps of the factory set: its patches and the kept bytes (a two-part build's id
 * differs: its programs are other records) */
uint32_t mv_prog_factory_id(void)
{
    uint32_t h = 2166136261u, n, i;
    for (n = 0; n < MV_NPROG; n++) {
        for (i = 0; i < MV_PATCH_BYTES; i++) h = (h ^ MV_FACTORY[n][i]) * 16777619u;
        for (i = 0; i < KEEP_N; i++) h = (h ^ MV_FACTORY_PERF[n][KEEP[i]]) * 16777619u;
    }
    return h ? h : 1u;                          /* (0: "no bank recorded" in the settings) */
}

int mv_prog_edited(const mv_prog_t *g, uint32_t n, const mv_patch_t *up, const mv_patch_t *lo, const volatile uint8_t *perf)
{
    uint32_t i;
    (void)lo;
    n %= MV_NPROG;
    for (i = 0; i < MV_PATCH_BYTES; i++) if (P(g, n).b[i] != up->b[i]) return 1;
    for (i = 0; i < MV_PROG_PERF; i++) if (g->perf[n][i] != perf[i]) return 1;
    return 0;
}

/* one record a call (as the two-part blobs: the caller calls again while dirty), so one erase at a time */
int mv_prog_flush(mv_prog_t *g)
{
    uint32_t r, k, i;
    for (r = 0; r < MV_PROG_NREC; r++) {
        mv_prog_rec_t *q = &g->rec[r];
        if (!(g->dirty >> r & 1u)) continue;
        for (k = 0; k < MV_PROG_PER_REC; k++)   /* the kept bytes into the record's second part */
            for (i = 0; i < KEEP_N; i++) q->pk[k][i] = g->perf[r * MV_PROG_PER_REC + k][KEEP[i]];
        if (slot_save(MV_PROG_ISLOT + r, q, REC_BYTES) != 0) return -1;
        g->dirty &= (uint16_t)~(1u << r);
        g->from_flash |= (uint16_t)(1u << r);
        return 0;
    }
    return 1;
}
#endif
