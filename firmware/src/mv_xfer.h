/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance program transfer (Kris 4 Nov, F1 + F2: fm1-port/mvance-bank-transfer-proposal-2026-11-04.md): JP-8080
 * SysEx over USB, so a computer (docs/bank.html) can send programs into chosen slots, audition one without saving,
 * and read every program back.
 *
 * In (DT1, F0 41 dev 00 06 12 a3 a2 a1 a0 data.. sum F7; any device id):
 *   01 00 aa xx   the TEMPORARY performance: what plays now (not saved: the program shows edited, SAVE keeps it)
 *   03 pp aa xx   PERFORMANCE pp (0..63) = program slot pp+1: written straight into the slot (flash when silent)
 *   02 bb pp xx   USER PATCH bb x 64 + pp/2: patches 1..64 = the UPPER patch of slots 1..64 (the rest ignored)
 *     aa  00        common: name 0..15, KEY MODE 0x10, SPLIT 0x11, BALANCE 0x13 (the JP-8080's)
 *         40 / 41   the upper patch (41: its bytes 128..247), 42 / 43 the lower patch
 *         70        M-vance's own: the 16 performance bytes (MV_PF_*: VOICES, CH LO / UP, ARP .. that the JP's
 *                   common has no place for). Not a JP-8080 area: a JP ignores it
 * Request (RQ1, F0 41 dev 00 06 11 a3 a2 a1 a0 s3 s2 s1 s0 sum F7):
 *   03 pp 00 00 / size 00 nn 00 00   performances pp .. pp+nn-1 (nn 0 or a smaller size: one)
 *   01 00 00 00                      the temporary performance (what plays)
 *   answered with 4 DT1s a performance: common (20 bytes), upper (248, one message), lower (248), 70 (16), device
 *   id 10h; the same messages tools/make_bank.py writes, so a dump is a bank file as it stands.
 *
 * No flow control (DT1 has none): USB holds one received message at a time, so a sender waits ~30 ms between them
 * (docs/bank.html does). Main loop only. */
#pragma once
#include <stdint.h>
#include "mv_prog.h"
#include "mv_voice.h"

#define MV_XF_AREA_EXT 0x70u                    /* 03 pp 70 00: the 16 performance bytes (M-vance's own area) */
#define MV_XF_DEV 0x10u                         /* the device id a dump goes out with (the JP's default, 17) */
#define MV_XF_MSG_MAX (12u + 248u)              /* the longest message: a whole patch */

/* what a received message did (bits) */
enum { MV_XF_NONE = 0, MV_XF_SLOT = 1, MV_XF_TEMP = 2, MV_XF_REQ = 4, MV_XF_BAD = 8 };

typedef struct {
    uint16_t rx_n, tx_n;                        /* messages taken / sent in this transfer (the screen counts them) */
    uint8_t last_slot;                          /* the slot the last message wrote (0..63; 0xFF: none yet) */
    uint8_t dump_pp, dump_n, dump_k, dump_temp; /* a dump to send: performance dump_pp on, dump_n left, its message
                                                   dump_k (0 common, 1 upper, 2 lower, 3 own bytes); temp: what plays */
} mv_xfer_t;

/* one received SysEx (body: the bytes between F0 and F7). A slot written goes through mv_prog_write (marked, to
 * flash when the caller flushes); the temporary area writes the engine's sound. Returns MV_XF_* bits */
uint32_t mv_xfer_rx(mv_xfer_t *x, mv_prog_t *g, mv_engine_t *e, const uint8_t *body, uint32_t n);
/* the next message of a requested dump into out (F0 .. F7, at most MV_XF_MSG_MAX): its length, 0 when none is due */
uint32_t mv_xfer_next(mv_xfer_t *x, const mv_prog_t *g, const mv_engine_t *e, uint8_t *out, uint32_t cap);
