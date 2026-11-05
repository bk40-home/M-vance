/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance program transfer: implementation (addresses and protocol in mv_xfer.h). */
#include "mv_xfer.h"

#define A_TEMP 0x01u                            /* address byte a3: the temporary performance */
#define A_USER 0x02u                            /* user patches */
#define A_PERF 0x03u                            /* user performances */

static uint8_t xf_sum(const uint8_t *a, uint32_t n)   /* Roland: address + data + sum = 0 (mod 128) */
{
    uint32_t s = 0, i;
    for (i = 0; i < n; i++) s += a[i];
    return (uint8_t)((128u - (s & 0x7Fu)) & 0x7Fu);
}

/* a DT1 of len bytes to addr: F0 41 10 00 06 12 a3 a2 a1 a0 data sum F7; its length */
static uint32_t xf_dt1(uint8_t *out, uint32_t addr, const uint8_t *d, uint32_t len)
{
    uint32_t i, n = 0;
    out[n++] = 0xF0u; out[n++] = 0x41u; out[n++] = MV_XF_DEV; out[n++] = 0x00u; out[n++] = 0x06u; out[n++] = 0x12u;
    out[n++] = (uint8_t)(addr >> 24); out[n++] = (uint8_t)(addr >> 16); out[n++] = (uint8_t)(addr >> 8); out[n++] = (uint8_t)addr;
    for (i = 0; i < len; i++) out[n++] = d[i] & 0x7Fu;
    out[n] = xf_sum(out + 6, n - 6u); n++;
    out[n++] = 0xF7u;
    return n;
}

/* one area's bytes into a performance (its two patches and its performance bytes), checked first: 1 taken */
static int xf_apply(mv_patch_t *up, mv_patch_t *lo, uint8_t *pf, uint32_t area, uint32_t off, const uint8_t *d,
                    uint32_t len, const uint8_t *body, uint32_t n)
{
    uint32_t i;
    if (area == 0x00u) {                        /* common: the name, KEY MODE, SPLIT, BALANCE (the rest: the JP's) */
        for (i = 0; i < len; i++) {
            uint32_t a = off + i, v = d[i];
            if (a < 16u) up->b[a] = (uint8_t)(v < 32u || v > 125u ? 32u : v);   /* (a name: ASCII 32..125) */
            else if (a == 0x10u) pf[MV_PF_MODE] = (uint8_t)(v > MV_KEY_SPLIT ? MV_KEY_SINGLE : v);
            else if (a == 0x11u) pf[MV_PF_SPLIT] = (uint8_t)v;
            else if (a == 0x13u) pf[MV_PF_BAL] = (uint8_t)v;
        }
        return 1;
    }
    if (area == MV_XF_AREA_EXT) {               /* M-vance's own: all 16, held in range as a whole */
        uint8_t t[MV_PROG_PERF];
        if (off + len > MV_PROG_PERF) return 0;
        for (i = 0; i < MV_PROG_PERF; i++) t[i] = pf[i];
        for (i = 0; i < len; i++) t[off + i] = d[i];
        if (!mv_prog_perf_ok(t)) return 0;
        for (i = 0; i < MV_PROG_PERF; i++) pf[i] = t[i];
        return 1;
    }
    if (area >= 0x40u && area <= 0x43u) {       /* a patch: the parser's own checks, as the temporary area */
        static uint8_t m[MV_SYSEX_MAX + 4u];
        mv_patch_t t = area < 0x42u ? *up : *lo;
        int32_t no;
        if (n + 2u > sizeof m) return 0;
        m[0] = 0xF0u;
        for (i = 0; i < n; i++) m[1u + i] = body[i];
        m[n + 1u] = 0xF7u;
        m[6] = 0x01u; m[7] = 0x00u; m[8] = (uint8_t)(0x40u | (area & 1u));   /* 01 00 40/41 xx */
        m[n] = xf_sum(m + 6, n - 6u);           /* (the address changed: so does the sum; the original was checked) */
        if (mv_sysex_parse(m, n + 2u, &t, &no) != MV_SX_OK) return 0;
        if (area < 0x42u) *up = t; else *lo = t;
        return 1;
    }
    return 0;                                   /* (08, 10, 11 ..: the JP's parts we do not have) */
}

uint32_t mv_xfer_rx(mv_xfer_t *x, mv_prog_t *g, mv_engine_t *e, const uint8_t *body, uint32_t n)
{
    /* body: 41 dev 00 06 cmd a3 a2 a1 a0 [data..] sum */
    const uint8_t *a = body + 5;
    uint32_t i, len;
    if (n < 11u || body[0] != 0x41u || body[2] != 0x00u || body[3] != 0x06u) return MV_XF_NONE;   /* not a JP-8080 */
    for (i = 1; i < n; i++) if (body[i] > 0x7Fu) return MV_XF_BAD;
    if (xf_sum(body + 5, n - 6u) != body[n - 1u]) return MV_XF_BAD;   /* address + data (+ size) against the sum */
    if (body[4] == 0x11u) {                     /* RQ1: a dump to send (one at a time: a new one replaces it) */
        if (n != 14u) return MV_XF_BAD;
        if (a[0] == A_PERF && a[1] < MV_NPROG) {
            uint32_t nn = body[10] ? body[10] : 1u;   /* the size's pp digit: how many performances */
            if (a[1] + nn > MV_NPROG) nn = MV_NPROG - a[1];
            x->dump_pp = a[1]; x->dump_n = (uint8_t)nn; x->dump_temp = 0;
        } else if (a[0] == A_TEMP && a[1] == 0x00u) {
            x->dump_pp = 0; x->dump_n = 1; x->dump_temp = 1;
        } else
            return MV_XF_NONE;
        x->dump_k = 0; x->tx_n = 0;
        return MV_XF_REQ;
    }
    if (body[4] != 0x12u) return MV_XF_NONE;    /* DT1 only from here */
    len = n - 10u;                              /* 41 dev 00 06 12 + 4 address .. data .. sum */
    if (a[0] == A_TEMP && a[1] == 0x00u) {      /* what plays: the engine's own copies */
        mv_patch_t up = e->patch, lo = e->lower;
        uint8_t pf[MV_PROG_PERF];
        for (i = 0; i < MV_PROG_PERF; i++) pf[i] = e->perf[i];
        if (!xf_apply(&up, &lo, pf, a[2], a[3], body + 9, len, body, n)) return MV_XF_BAD;
        e->patch = up; e->lower = lo;
        for (i = 0; i < MV_PROG_PERF; i++) e->perf[i] = pf[i];
        x->rx_n++;
        return MV_XF_TEMP;
    }
    if ((a[0] == A_PERF && a[1] < MV_NPROG) ||
        (a[0] == A_USER && a[1] == 0u && a[2] < 2u * MV_NPROG)) {   /* a slot (user patches 1..64: its upper) */
        uint32_t slot = a[0] == A_PERF ? a[1] : a[2] >> 1;
        uint32_t area = a[0] == A_PERF ? a[2] : 0x40u | (a[2] & 1u);
        mv_patch_t up = *mv_prog_get(g, slot), lo = *mv_prog_lower(g, slot);
        uint8_t pf[MV_PROG_PERF];
        const uint8_t *q = mv_prog_perf(g, slot);
        for (i = 0; i < MV_PROG_PERF; i++) pf[i] = q[i];
        if (!xf_apply(&up, &lo, pf, area, a[3], body + 9, len, body, n)) return MV_XF_BAD;
        mv_prog_write(g, slot, &up, &lo, pf);
        x->rx_n++;
        x->last_slot = (uint8_t)slot;
        return MV_XF_SLOT;
    }
    return MV_XF_NONE;
}

uint32_t mv_xfer_next(mv_xfer_t *x, const mv_prog_t *g, const mv_engine_t *e, uint8_t *out, uint32_t cap)
{
    const mv_patch_t *up, *lo;
    uint8_t pf[MV_PROG_PERF], com[20];
    uint32_t base, i, len;
    if (!x->dump_n || cap < MV_XF_MSG_MAX) return 0;
    if (x->dump_temp) {
        up = &e->patch; lo = &e->lower; base = (uint32_t)A_TEMP << 24;
        for (i = 0; i < MV_PROG_PERF; i++) pf[i] = e->perf[i];
    } else {
        const uint8_t *q = mv_prog_perf(g, x->dump_pp);
        up = mv_prog_get(g, x->dump_pp); lo = mv_prog_lower(g, x->dump_pp);
        base = (uint32_t)A_PERF << 24 | (uint32_t)x->dump_pp << 16;
        for (i = 0; i < MV_PROG_PERF; i++) pf[i] = q[i];
    }
    switch (x->dump_k) {
    case 0:                                     /* common: the name, KEY MODE, SPLIT, PANEL SELECT (UPPER), BALANCE */
        for (i = 0; i < 16u; i++) com[i] = up->b[i];
        com[16] = pf[MV_PF_MODE]; com[17] = pf[MV_PF_SPLIT]; com[18] = 0; com[19] = pf[MV_PF_BAL];
        len = xf_dt1(out, base, com, 20u);
        break;
    case 1: len = xf_dt1(out, base | 0x4000u, up->b, MV_PATCH_BYTES); break;
    case 2: len = xf_dt1(out, base | 0x4200u, lo->b, MV_PATCH_BYTES); break;
    default: len = xf_dt1(out, base | (uint32_t)MV_XF_AREA_EXT << 8, pf, MV_PROG_PERF); break;
    }
    if (++x->dump_k == 4u) { x->dump_k = 0; x->dump_pp++; x->dump_n--; }
    x->tx_n++;
    return len;
}
