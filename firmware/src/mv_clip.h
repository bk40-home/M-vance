/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance clips: a sequencer track saved to flash and loaded back (Kris 6 Nov 2026, sequencer phase 3: §2d of
 * fm1-port/mvance-sequencer-proposal-2026-11-06.md, signed off "go with all your recommendations").
 *
 *   a clip     one track's events (as recorded, at most MV_SEQ_NEV), its LENGTH, ROOT and the QUANT it was recorded
 *              at, and a 6-character name. Not tied to a program: any clip loads into either track, under any program
 *   where      flash slot MV_CLIP_SLOT (1), MV_CLIP_PER clips to each of its SLOT_SECT sectors (platform.c
 *              slot_sec_*): MV_CLIP_N = 12. (The proposal said 15 in the slot: 15 x 1040 B needs the whole 16 KB slot
 *              rewritten from a 16 KB RAM image on every save. A sector at a time keeps a save to one erase, ~50 ms,
 *              and the RAM to one 3 KB sector image.)
 *   SAVE       the selected track as it is now (a copy, checked against seq.ver) into clip k: shown at once (the
 *              directory), written once nothing sounds (an erase stalls the audio, as SAVE of a program): one SAVE
 *              waits at a time
 *   LOAD       clip k into the selected track: read from flash (a read does not stall), handed to the ISR
 *              (mv_seq_load at the next block: what the track played ends first, the playhead runs on); its LENGTH
 *              is set for that track (a setting: kept in flash), its ROOT comes with it, the offset goes to 0
 *   factory    (Kris 7 Nov) read-only clips in the firmware (firmware/gen/mv_fclips.h, made by tools/mid2clip.py from
 *              MIDI files), numbered after the user ones: k = MV_CLIP_N .. MV_CLIP_ALL - 1, shown F1, F2 ..; LOAD
 *              only (SAVE answers MV_CLIP_RO). They play straight from flash (mv_seq_load_rom): any length to 32
 *              bars, any number of notes. SAVE of a track playing one: decoded into the record if it fits (else
 *              MV_CLIP_LONG)
 *   record     16-byte head: 'C', format 2, LENGTH in bars (format 1, before 7 Nov: a 1..4 code for 1 / 2 / 4 / 8
 *              bars, read as such), ROOT, QUANT, 0, events (16-bit LE), name (6), 0, 0; then the
 *              events, 4 bytes each: tick (16-bit LE), note, velocity (0 = note-off). A sector: MV_CLIP_PER records,
 *              under platform.c's own header (magic, length, CRC): a sector never written, or cut short, reads as 3
 *              empty clips
 *
 * Main loop only. #included into mvance.c after the platform (it uses slot_sec_load / slot_sec_save). */
#pragma once
#include <stdint.h>
#include "mv_voice.h"
#include "store.h"
#include "mv_fclips.h"

#define MV_CLIP_SLOT 1u                         /* the slot (0: the programs' performance bytes, mv_prog.h) */
#define MV_CLIP_PER 3u                          /* clips a sector */
#define MV_CLIP_N (MV_CLIP_PER * SLOT_SECT)     /* 12 */
#define MV_CLIP_NAME 6u
#define MV_CLIP_HEAD 16u
#define MV_CLIP_REC (MV_CLIP_HEAD + 4u * MV_SEQ_NEV)   /* 1040 B: a clip, full or not (fixed places in its sector) */
#define MV_CLIP_SEC (MV_CLIP_PER * MV_CLIP_REC)        /* 3120 B: a sector's image */
_Static_assert(MV_CLIP_SEC <= SECTOR_MAX, "3 clips fit a sector");
_Static_assert(MV_CLIP_SLOT < NSLOT, "the clips' slot exists");
#define MV_CLIP_NONE 0xFFu
#define MV_CLIP_ALL (MV_CLIP_N + MV_FCLIP_N)    /* user clips, then the factory ones */

typedef struct {
    uint8_t used;                               /* 1: it holds a track (its events may still be 0) */
    uint8_t len, root, quant;                   /* LENGTH in bars (1..32), ROOT (MV_SEQ_NOROOT none), QUANT recorded at */
    uint16_t n;                                 /* events */
    char name[MV_CLIP_NAME];                    /* ASCII 32..125, not 0-terminated */
} mv_clip_info_t;

typedef struct {
    mv_clip_info_t dir[MV_CLIP_N];              /* what each clip holds: read at boot, kept up to date by SAVE */
    uint8_t pend;                               /* a SAVE waiting for silence: its clip (MV_CLIP_NONE none) */
    uint8_t rec[MV_CLIP_REC];                   /* that SAVE's record */
    uint8_t err;                                /* the last write to flash failed */
} mv_clip_t;

/* what mv_clip_save / mv_clip_load report */
enum { MV_CLIP_OK = 0, MV_CLIP_AGAIN = -1, MV_CLIP_EMPTY = -2, MV_CLIP_BUSY = -3, MV_CLIP_RO = -4, MV_CLIP_LONG = -5 };

void mv_clip_init(mv_clip_t *c);                /* the directory, from flash */
/* SAVE: part's track into clip k, named name (MV_CLIP_NAME characters). MV_CLIP_AGAIN: the track changed while it
 * was copied (try on the next pass); MV_CLIP_BUSY: another SAVE still waits for silence */
int mv_clip_save(mv_clip_t *c, const mv_engine_t *e, uint32_t part, uint32_t k, const char *name);
/* LOAD: clip k into part's track (and its LENGTH into e->seqb: the caller keeps the settings). MV_CLIP_EMPTY: nothing
 * there (or unreadable); MV_CLIP_AGAIN: the last LOAD is not taken yet (try on the next pass) */
int mv_clip_load(mv_clip_t *c, mv_engine_t *e, uint32_t k, uint32_t part);
static inline int mv_clip_dirty(const mv_clip_t *c) { return c->pend != MV_CLIP_NONE; }
/* a factory clip (k >= MV_CLIP_N): its entry, else 0 */
static inline const mv_fclip_t *mv_clip_factory(uint32_t k)
{
    return k >= MV_CLIP_N && k < MV_CLIP_ALL ? &MV_FCLIPS[k - MV_CLIP_N] : 0;
}
/* the waiting SAVE to flash (call only when nothing sounds): its sector read, its record put in, the sector written.
 * 0 done (or nothing to do), else the write failed (err set; the SAVE is dropped, the directory reread) */
int mv_clip_flush(mv_clip_t *c);
