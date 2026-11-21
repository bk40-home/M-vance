/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance sequence transfer: the editor page reads and writes the sequencer's tracks and clips over SysEx (Kris 20 Nov
 * 2026: fm1-port/mvance-seq-editor-proposal-2026-11-20.md, signed off "go with recommended": 1a .. 8a).
 *
 * The same JP-8080 framing as the program transfer (mv_xfer.h), in an area of M-vance's own (a JP ignores it):
 *   address 04 tt hh ll    tt  00 / 01        the live track UPPER / LOWER (what plays now: not kept in flash)
 *                              10 + k         user clip k + 1 (k 0..11: MV_CLIP_N)
 *                              20 + k         factory clip F(k + 1) (read only)
 *                          hh ll  the offset, 14 bits (hh x 128 + ll): the head at 0, event i at 32 + 4 i
 *   head (32 bytes)   0 'C'   1 format 2   2 LENGTH (bars, 1..32)   3 ROOT (7F: none)   4 QUANT (MV_SQQ_*)
 *                     5 flags (MV_SXF_*)   6 / 7 events (n, low 7 bits / high 7 bits)   8..13 a user clip's name
 *                     14 / 15 a live track's seq.ver (low 7 / next 7 bits: it moved, the track changed)
 *                     16..31 the name in full (a factory clip's 16 characters; a user clip's 6, then spaces)
 *   event (4 bytes)   tick >> 7, tick & 7F, note, velocity (0: note-off). Ticks of MV_SEQ_PPQN (96) a quarter, in
 *                     the loop (0 .. LENGTH x 384 - 1): 14 bits hold 32 bars, so nothing needs packing into 7 bits
 *   point (4 bytes)   an automation point (mv_seq.h, Kris 20 Nov): 60 | tick >> 9, (tick >> 2) & 7F, CC, value. A
 *                     note's first byte is at most 5F (tick 12287 >> 7), so 60..7F marks a point; its tick is on the
 *                     4-tick grid (MV_SEQ_PT_GRID) and needs 12 bits
 *
 * Write (DT1) a track or a user clip: its events first, in order from event 0 (32 a message: 128 bytes), then its head,
 * which commits them: n in the head must be the events written. Put together in mv_clip's rec (no RAM of its own), then
 *   a live track   played as a LOAD: its notes end, the playhead runs on, its LENGTH set (a setting, kept)
 *   a user clip    kept as a SAVE: written to flash once nothing sounds
 * Refused (nothing changes): a factory clip; a SAVE still waiting for silence (busy); events out of order; a head
 * that does not match them; ticks not sorted or past the loop; a pitch whose on / off do not alternate (round the
 * loop: a note held over the loop's end has its note-off first, as a recording makes it). DT1 has no answer: the page
 * reads the head back to see it was taken.
 * Read (RQ1 04 tt 00 00, size 00 00 00 20: the head only, as a directory; any other size: the lot): the head, then the
 * events, 32 a message, one message a main-loop pass. A live
 * track that changes meanwhile (seq.ver, its LENGTH) starts again from the head, so what arrives is one state.
 * Linked (the page reads what plays: mvance.c link_*): when a track changes (seq.ver, LENGTH, QUANT), at most every
 * MV_SX_REPORT_MS, the two live tracks' heads go out unasked; the page reads the track when it wants it.
 *
 * Main loop only (the ISR's track is read through seq.ver, as the piano roll does). #included into mvance.c after
 * mv_clip.c, under MV_XFER and MV_SEQ. */
#pragma once
#include <stdint.h>
#include "mv_seq.h"
#include "mv_clip.h"
#include "mv_xfer.h"

#define MV_SX_AREA 0x04u                        /* a3: the sequencer's area */
#define MV_SX_TT_TRK 0x00u                      /* tt: the live tracks (+0 UPPER, +1 LOWER) */
#define MV_SX_TT_USER 0x10u                     /* tt: user clips (+k) */
#define MV_SX_TT_FAC 0x20u                      /* tt: factory clips (+k), up to tt 7F */
#define MV_SX_HEAD 32u                          /* the head's bytes (events from here) */
#define MV_SX_CHUNK 32u                         /* events a message (128 bytes of data) */
#define MV_SX_MAXEV 4000u                       /* the most events a read sends (offset 14 bits: 32 + 4 x 4000) */
#define MV_SX_REPORT_MS 500u                    /* linked: a track's change reported at most this often */
enum { MV_SXF_FLASH = 1, MV_SXF_RO = 2, MV_SXF_EMPTY = 4 };   /* head flags: plays from flash, read only, nothing there */

/* what mv_seqx_rx did */
enum { MV_SX_NONE = 0,                          /* not area 04: for mv_xfer_rx */
       MV_SX_STAGED,                            /* events taken (waiting for their head) */
       MV_SX_TRACK,                             /* a live track written (or it will be, at mv_seqx_poll) */
       MV_SX_CLIP,                              /* a user clip written (to flash when silent) */
       MV_SX_REQ,                               /* a read asked for: mv_seqx_next sends it */
       MV_SX_BAD };                             /* refused: nothing changed */

typedef struct {
    /* a write being put together in rec */
    uint8_t stg_tt;                             /* its tt (0xFF: none) */
    uint16_t stg_next;                          /* events taken so far (the next must be this one) */
    uint32_t stg_gen;                           /* clips.wgen when it began (a SAVE since: void) */
    uint8_t play_pend;                          /* a track to LOAD rec into: its part + 1 (0 none; the ISR was busy) */
    uint8_t loaded;                             /* a track was loaded: the caller keeps its LENGTH (mv_seqx_poll) */
    /* a read being sent */
    uint8_t d_on, d_tt, d_head, d_headonly;     /* sending; what; the head is next; only the head (a report) */
    uint8_t d_bars;                             /* a live track's LENGTH when it began */
    uint8_t d_then;                             /* a report: the LOWER head after the UPPER's */
    uint16_t d_i, d_n;                          /* the next event; how many */
    uint32_t d_ver;                             /* a live track: seq.ver when it began */
    mv_seq_rd_t d_rd;                           /* a flash clip's reader at event d_i */
    /* the link's reports */
    uint32_t rep_sig, rep_t;                    /* the tracks as last reported; when */
} mv_seqx_t;

void mv_seqx_init(mv_seqx_t *s);
/* one received SysEx (body: between F0 and F7): MV_SX_* */
uint32_t mv_seqx_rx(mv_seqx_t *s, mv_clip_t *c, mv_engine_t *e, const uint8_t *body, uint32_t n);
/* the next message of a read into out (at least MV_XF_MSG_MAX bytes): its length, 0 when none is due */
uint32_t mv_seqx_next(mv_seqx_t *s, mv_clip_t *c, const mv_engine_t *e, uint8_t *out, uint32_t cap);
static inline int mv_seqx_sending(const mv_seqx_t *s) { return s->d_on; }
/* each main-loop pass: a track LOAD the ISR was too busy for. 1: a track was loaded since the last call (the caller
 * keeps the sequencer's settings: its LENGTH changed) */
int mv_seqx_poll(mv_seqx_t *s, mv_clip_t *c, mv_engine_t *e);
/* the page linked (it read what plays, and reads the tracks itself): reports from the tracks as they are now */
void mv_seqx_link(mv_seqx_t *s, const mv_engine_t *e);
/* linked, each pass with nothing else to send: a change to the tracks starts a report (their heads) */
void mv_seqx_report(mv_seqx_t *s, const mv_engine_t *e, uint32_t now_ms);
