/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance patch: the 248 bytes of a JP-8080 patch, held exactly as the JP stores them, and the ways in and out:
 * parameter get / set (with the 2-byte morph values), the JP MODE2 CC map, and Roland DT1 SysEx.
 *
 * Why the raw bytes: a JP-8000 / JP-8080 .syx loads byte for byte, saves byte for byte, and the engine reads each
 * byte through a law table built on the PC (Stage 5). Nothing here runs in the audio ISR.
 * Integers only (the chip has no FPU), no 64-bit division. */
#pragma once
#include <stdint.h>
#include "mv_params.h"

typedef struct {
    uint8_t b[MV_PATCH_BYTES];                  /* JP patch bytes, 7-bit each */
} mv_patch_t;

/* SysEx address areas (Roland 4-byte, 7-bit) */
#define MV_ADDR_TEMP_UPPER 0x01004000u          /* the temporary patch of the upper part (the one we play) */
#define MV_ADDR_TEMP_LOWER 0x01004200u          /* the lower part: taken as ours too (we have one part) */
#define MV_ADDR_USER       (0x02u << 24)        /* user patches: 02 bb pp 00, 128 of them (written as a shift:
                                                   the build's register check reads 0x020..... as the XIP window) */
#define MV_SYSEX_MAX       (11u + MV_PATCH_BYTES + 2u)   /* one DT1 message carrying a whole patch */

/* parse results */
#define MV_SX_NOT_OURS   0                      /* not a JP-8080 DT1 to a patch area: ignore it */
#define MV_SX_OK         1                      /* bytes written into the patch */
#define MV_SX_BAD_SUM   -1                      /* checksum wrong: nothing written */
#define MV_SX_BAD_FRAME -2                      /* too short, no F7, data past the patch: nothing written */

void mv_patch_init(mv_patch_t *p);                                     /* the init patch (MV_PATCH_INIT) */
int32_t mv_param_get(const mv_patch_t *p, uint32_t id);                /* the stored value, min..max */
int32_t mv_param_rel(const mv_patch_t *p, uint32_t id);                /* value - centre (signed params) */
void mv_param_set(mv_patch_t *p, uint32_t id, int32_t v);              /* clamped to min..max */
uint32_t mv_patch_sanitize(mv_patch_t *p);                             /* clamp every parameter; returns how many
                                                                          were out of range (a foreign patch) */
int32_t mv_cc_apply(mv_patch_t *p, uint32_t cc, uint32_t val);         /* JP MODE2 CC -> parameter id, or -1 */
uint32_t mv_cc_value(uint32_t id, int32_t v);  /* the inverse: the CC value that mv_cc_apply turns back into v */

/* One DT1 message: F0 41 dev 00 06 12 a3 a2 a1 a0 data.. sum F7. Writes the bytes it carries into *p (from
 * whatever offset its address gives), then clamps them. *patch_no (if not 0) = 0..127 for a user patch,
 * -1 for a temporary patch. A JP-8000 dump (239 bytes, plus an extension message) fills in the same way. */
int mv_sysex_parse(const uint8_t *msg, uint32_t n, mv_patch_t *p, int32_t *patch_no);
/* The whole patch as one DT1 message to addr (MV_ADDR_TEMP_UPPER, or mv_user_addr(n)); returns its length. */
uint32_t mv_sysex_build(const mv_patch_t *p, uint8_t dev, uint32_t addr, uint8_t *out, uint32_t cap);
uint32_t mv_user_addr(uint32_t patch_no);                              /* 0..127 -> 02 bb pp 00 */
