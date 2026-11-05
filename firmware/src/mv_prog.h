/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance programs: 64 JP-8080 patches (Kris 1 Nov, 9: PRESETS selects, SAVE writes; 2 Nov: a test bank he can
 * rename, reorder and edit). The main loop's: never the ISR.
 *
 *   factory  firmware/gen/mv_factory.h (tools/mid2bank.c from a .mid / .syx in docs/presets/, parsed by the firmware's
 *            own mv_sysex_parse): in flash, read-only; what a slot holds until it is first saved, and FACTORY's
 *   the bank in RAM (64 x 248 B = 15.9 KB), loaded at boot: flash blobs 0..3 hold 16 programs each (3968 of a
 *            blob's 4084 B; one sector each, so a save rewrites 16 programs, not 64). A blob not saved yet (or
 *            unreadable: another firmware's, a size change) loads its 16 from the factory set
 *   changes  write (the edited patch into a slot), swap (two slots: the reorder), factory (a slot back to the
 *            file's): in RAM at once, the blob marked; mv_prog_flush writes the marked blobs, and the caller calls it
 *            only when nothing sounds (an erase holds the interrupts off ~50 ms a sector: docs/API.md)
 * Performances (Kris 3 Nov: layer / split): a program is a performance, its UPPER patch (bank: the one a single
 * plays, and the program's name), its LOWER patch (lower) and its performance bytes (perf: MV_PF_* of mv_voice.h).
 *   upper    blobs 0..3, as before (a single saved before the performance reads as it was)
 *   lower    blobs 4..7, the same layout; a blob not saved yet takes each program's upper patch (or the factory
 *            lower, for a program still the factory's): a single switched to DUAL doubles its own sound
 *   perf     slot 0, all 64 x 16 B; not saved yet: the factory's (a single: MV_PERF_DEF)
 *   RAM      3 x 15.9 KB + 1 KB (.bss had 58 KB free)
 * It is #included into mvance.c (the unity build): it saves with platform.c's blob_load / blob_save, slot_*. */
#pragma once
#include <stdint.h>
#include "mv_patch.h"
#include "mv_perf.h"                         /* MV_PF_*, MV_PERF_DEF */
/* The factory bank (Kris 5 Nov, B): the local one when it is there, else the INIT-only one committed to GitHub.
 * mv_factory.h is git-ignored (its programs come from third-party banks, LICENSING.md); both headers define the
 * same names (MV_NPROG, MV_FACTORY, MV_FACTORY_LO, MV_FACTORY_PERF), and both are made by tools/make_bank.py. */
#if __has_include("mv_factory.h")
#include "mv_factory.h"                        /* local: tools/make_bank.py [tools/bank/factory.txt] */
#define MV_FACTORY_PUBLIC 0                    /* (tests that check particular programs run) */
#else
#include "mv_factory_public.h"                 /* public: tools/make_bank.py tools/bank/public.txt */
#define MV_FACTORY_PUBLIC 1                    /* (64 INIT programs: those tests are skipped) */
#endif

#define MV_PROG_PER_BLOB 16u                   /* programs a flash blob holds */
#define MV_PROG_BLOB0 0u                       /* blobs 0..3 */
#define MV_PROG_NAME 16u                       /* a name: patch bytes 0..15 (ASCII 32..125, as the JP) */
#define MV_PROG_LBLOB0 4u                      /* the lower patches: blobs 4..7 */
#define MV_PROG_PSLOT 0u                       /* the performance bytes: slot 0 */
#define MV_PROG_PERF 16u                       /* performance bytes a program (MV_PF_N) */

typedef struct {
    mv_patch_t bank[MV_NPROG];                 /* the programs' UPPER patches (RAM copy of blobs 0..3) */
    mv_patch_t lower[MV_NPROG];                /* their LOWER patches (blobs 4..7) */
    uint8_t perf[MV_NPROG][MV_PROG_PERF];      /* their performance bytes (slot 0) */
    uint16_t dirty;                            /* bit k: blob k changed (0..3 upper, 4..7 lower), bit 8: the perf */
    uint16_t from_flash;                       /* the same bits: read from flash (else the factory set) */
} mv_prog_t;
#define MV_PROG_PDIRTY 0x100u

void mv_prog_load(mv_prog_t *g);               /* boot: the 4 blobs, the factory set where one is missing */
/* performance bytes in range (MODE, SPLIT, BALANCE, VOICES, CH LO / UP, ARP; the spares 7-bit): the load and a
 * received program check with it */
int mv_prog_perf_ok(const uint8_t *pf);
const mv_patch_t *mv_prog_get(const mv_prog_t *g, uint32_t n);          /* the UPPER patch (a single's) */
const mv_patch_t *mv_prog_lower(const mv_prog_t *g, uint32_t n);        /* the LOWER patch */
const uint8_t *mv_prog_perf(const mv_prog_t *g, uint32_t n);            /* the performance bytes (MV_PF_N) */
/* slot n = upper, lower, perf (the whole performance) */
void mv_prog_write(mv_prog_t *g, uint32_t n, const mv_patch_t *up, const mv_patch_t *lo, const volatile uint8_t *perf);
void mv_prog_swap(mv_prog_t *g, uint32_t a, uint32_t b);             /* slots a and b trade places */
void mv_prog_factory(mv_prog_t *g, uint32_t n);                      /* slot n = the factory set's */
/* the factory bank (A1, Kris 3 Nov): a firmware with a new bank finds the programs it saved from the old one */
void mv_prog_factory_all(mv_prog_t *g);         /* all 64 = the factory set's (to flash, record by record, when silent) */
uint32_t mv_prog_factory_id(void);              /* the bank built into this firmware: FNV-1a of its 64 programs */
static inline int mv_prog_saved(const mv_prog_t *g) { return g->from_flash != 0u; }   /* any record from flash */
/* the performance playing differs from slot n (either patch or the performance bytes) */
int mv_prog_edited(const mv_prog_t *g, uint32_t n, const mv_patch_t *up, const mv_patch_t *lo, const volatile uint8_t *perf);
static inline int mv_prog_dirty(const mv_prog_t *g) { return g->dirty != 0u; }
/* write ONE marked blob (the caller: when nothing sounds; call again for the next). 0 written, 1 none was marked,
 * -1 the flash refused (it stays marked) */
int mv_prog_flush(mv_prog_t *g);
