/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance patch: see mv_patch.h. Included once by mvance.c (Dinghy's unity build). */
#include "mv_patch.h"

void mv_patch_init(mv_patch_t *p)
{
    uint32_t i;
    for (i = 0; i < MV_PATCH_BYTES; i++)
        p->b[i] = MV_PATCH_INIT[i];
}

int32_t mv_param_get(const mv_patch_t *p, uint32_t id)
{
    const mv_param_t *d;
    if (id >= MV_NPARAMS) return 0;
    d = &MV_PARAMS[id];
    if (d->size == 2)                           /* 0..254 as two 7-bit bytes, the high one (0 or 1) first */
        return (int32_t)(((uint32_t)p->b[d->off] << 7) | p->b[d->off + 1u]);
    return p->b[d->off];
}

int32_t mv_param_rel(const mv_patch_t *p, uint32_t id)
{
    return id < MV_NPARAMS ? mv_param_get(p, id) - MV_PARAMS[id].centre : 0;
}

void mv_param_set(mv_patch_t *p, uint32_t id, int32_t v)
{
    const mv_param_t *d;
    if (id >= MV_NPARAMS) return;
    d = &MV_PARAMS[id];
    if (v < d->min) v = d->min;
    if (v > d->max) v = d->max;
    if (d->size == 2) {
        p->b[d->off] = (uint8_t)((uint32_t)v >> 7);
        p->b[d->off + 1u] = (uint8_t)((uint32_t)v & 0x7Fu);
    } else {
        p->b[d->off] = (uint8_t)v;
    }
}

uint32_t mv_patch_sanitize(mv_patch_t *p)
{
    uint32_t id, i, fixed = 0;
    for (i = 0; i < MV_PATCH_BYTES; i++)        /* 7-bit bytes, as on the wire */
        if (p->b[i] > 0x7Fu) { p->b[i] &= 0x7Fu; fixed++; }
    for (i = MV_NAME_OFF; i < MV_NAME_OFF + MV_NAME_LEN; i++)   /* the name: printable ASCII (JE: 32..125) */
        if (p->b[i] < 32u || p->b[i] > 125u) { p->b[i] = ' '; fixed++; }
    for (id = 0; id < MV_NPARAMS; id++) {
        int32_t v = mv_param_get(p, id);
        const mv_param_t *d = &MV_PARAMS[id];
        if (v < d->min || v > d->max) { mv_param_set(p, id, v); fixed++; }
    }
    return fixed;
}

int32_t mv_cc_apply(mv_patch_t *p, uint32_t cc, uint32_t val)
{
    uint32_t id;
    const mv_param_t *d;
    if (cc > 127u || val > 127u) return -1;
    id = MV_CC_PARAM[cc];
    if (id == MV_NONE) return -1;
    d = &MV_PARAMS[id];
    /* a CC is 0..127; a parameter with a smaller range takes the CC scaled onto it (as the JP does for its
     * switches and lists): v = min + val * (max - min + 1) / 128, a 32-bit division by a constant */
    if (d->max - d->min != 127)
        mv_param_set(p, id, (int32_t)(d->min + (val * (uint32_t)(d->max - d->min + 1u)) / 128u));
    else
        mv_param_set(p, id, (int32_t)(d->min + val));
    return (int32_t)id;
}

/* the CC value for a parameter value (the panel sends it, MIDI out): the first CC that mv_cc_apply maps to v,
 * ceil(k * 128 / n) with k = v - min, n = the number of values. Exact for every n <= 128 (all the CC parameters):
 * val * n >= k * 128, and < k * 128 + n <= (k + 1) * 128. A 128-value range is the CC itself */
uint32_t mv_cc_value(uint32_t id, int32_t v)
{
    const mv_param_t *d = &MV_PARAMS[id];
    uint32_t n = (uint32_t)(d->max - d->min) + 1u, k;
    if (v < d->min) v = d->min;
    if (v > d->max) v = d->max;
    k = (uint32_t)(v - d->min);
    if (n == 128u) return k;
    return (k * 128u + n - 1u) / n;            /* (a 32-bit division: once per panel edit, main loop) */
}

static uint8_t rol_sum(const uint8_t *a, uint32_t n)     /* Roland: 128 - (sum % 128), 7 bits */
{
    uint32_t s = 0, i;
    for (i = 0; i < n; i++) s += a[i];
    return (uint8_t)((128u - (s & 0x7Fu)) & 0x7Fu);
}

uint32_t mv_user_addr(uint32_t patch_no)
{
    patch_no &= 127u;                           /* 64 a bank: 02 bb pp 00, pp = 2 * (patch % 64) */
    return MV_ADDR_USER | ((patch_no >> 6) << 16) | (((patch_no & 63u) * 2u) << 8);
}

/* the patch byte a 4-byte address points at, or -1 if it is not inside one of our patch areas */
static int32_t addr_offset(const uint8_t *a, int32_t *patch_no)
{
    if (a[0] == 0x01u && a[1] == 0x00u && a[2] >= 0x40u && a[2] <= 0x43u) {   /* temporary upper / lower */
        *patch_no = -1;
        return (int32_t)(((uint32_t)(a[2] & 1u) << 7) | a[3]);
    }
    if (a[0] == 0x02u && a[1] <= 0x01u) {                                       /* user A11..B88 */
        *patch_no = (int32_t)(((uint32_t)a[1] << 6) | ((uint32_t)a[2] >> 1));
        return (int32_t)(((uint32_t)(a[2] & 1u) << 7) | a[3]);
    }
    return -1;
}

int mv_sysex_parse(const uint8_t *msg, uint32_t n, mv_patch_t *p, int32_t *patch_no)
{
    int32_t off, pn = -1;
    uint32_t len, i;
    if (n < 12u || msg[0] != 0xF0u || msg[1] != 0x41u || msg[3] != 0x00u || msg[4] != 0x06u || msg[5] != 0x12u)
        return MV_SX_NOT_OURS;                  /* not Roland, not the JP-8000/8080 model, not DT1 */
    off = addr_offset(msg + 6, &pn);
    if (off < 0) return MV_SX_NOT_OURS;         /* system, performance, motion: not a patch */
    if (msg[n - 1u] != 0xF7u) return MV_SX_BAD_FRAME;
    len = n - 12u;                              /* F0 41 dev 00 06 12 + 4 address .. data .. sum F7 */
    if ((uint32_t)off + len > MV_PATCH_BYTES) return MV_SX_BAD_FRAME;
    if (rol_sum(msg + 6, 4u + len) != msg[n - 2u]) return MV_SX_BAD_SUM;
    for (i = 0; i < len; i++) {
        if (msg[10u + i] > 0x7Fu) return MV_SX_BAD_FRAME;
    }
    for (i = 0; i < len; i++)
        p->b[(uint32_t)off + i] = msg[10u + i];
    mv_patch_sanitize(p);
    if (patch_no) *patch_no = pn;
    return MV_SX_OK;
}

uint32_t mv_sysex_build(const mv_patch_t *p, uint8_t dev, uint32_t addr, uint8_t *out, uint32_t cap)
{
    uint32_t i, n = 0;
    if (cap < MV_SYSEX_MAX) return 0;
    out[n++] = 0xF0u; out[n++] = 0x41u; out[n++] = (uint8_t)(dev & 0x7Fu);
    out[n++] = 0x00u; out[n++] = 0x06u; out[n++] = 0x12u;
    out[n++] = (uint8_t)(addr >> 24); out[n++] = (uint8_t)(addr >> 16);
    out[n++] = (uint8_t)(addr >> 8);  out[n++] = (uint8_t)addr;
    for (i = 0; i < MV_PATCH_BYTES; i++)
        out[n++] = (uint8_t)(p->b[i] & 0x7Fu);
    out[n] = rol_sum(out + 6, 4u + MV_PATCH_BYTES);
    n++;
    out[n++] = 0xF7u;
    return n;
}
