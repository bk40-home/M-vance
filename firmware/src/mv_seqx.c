/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance sequence transfer: implementation (the addresses and the rules in mv_seqx.h). Main loop only. */
#include "mv_seqx.h"

/* tt -> what it names. Returns 1 a live track (*k its part), 2 a user clip, 3 a factory clip (*k its index), 0 none */
static int sx_what(uint32_t tt, uint32_t *k)
{
    if (tt < MV_SX_TT_TRK + 2u) { *k = tt == MV_SX_TT_TRK ? MV_PART_UP : MV_PART_LO; return 1; }
    if (tt >= MV_SX_TT_USER && tt < MV_SX_TT_USER + MV_CLIP_N) { *k = tt - MV_SX_TT_USER; return 2; }
    if (tt >= MV_SX_TT_FAC && tt <= 0x7Fu && tt - MV_SX_TT_FAC < MV_FCLIP_N) { *k = tt - MV_SX_TT_FAC; return 3; }
    return 0;
}

/* an event (tick, note, velocity: MV_SEQ_PT a point) as its 4 wire bytes at o */
static void sx_put(uint8_t *o, uint32_t t, uint32_t note, uint32_t vel)
{
    if (vel & MV_SEQ_PT) { o[0] = (uint8_t)(0x60u | (t >> 9 & 0x1Fu)); o[1] = (uint8_t)(t >> 2 & 0x7Fu); }
    else { o[0] = (uint8_t)(t >> 7 & 0x7Fu); o[1] = (uint8_t)(t & 0x7Fu); }
    o[2] = (uint8_t)(note & 0x7Fu); o[3] = (uint8_t)(vel & 0x7Fu);
}

/* the address of offset off in tt's area */
static uint32_t sx_addr(uint32_t tt, uint32_t off)
{
    return (uint32_t)MV_SX_AREA << 24 | tt << 16 | ((off >> 7) & 0x7Fu) << 8 | (off & 0x7Fu);
}

void mv_seqx_init(mv_seqx_t *s)
{
    uint8_t *b = (uint8_t *)s;
    uint32_t i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    s->stg_tt = 0xFFu;
    s->rep_sig = 0;
}

/* ---- write ------------------------------------------------------------------------------------------------------ */

/* rec's n events, for a loop of bars: sorted by tick, inside the loop, and every pitch's on / off alternating round
 * the loop (an even number of them). 1 good */
static int sx_events_ok(const uint8_t *r, uint32_t n, uint32_t bars)
{
    uint8_t seen[16] = {0}, on[16] = {0}, odd[16] = {0};   /* a bit a pitch: has events; the last was an on; odd count */
    uint32_t i, last = 0, len = bars * MV_SEQ_BAR;
    const uint8_t *p = r + MV_CLIP_HEAD;
    for (i = 0; i < n; i++, p += 4) {
        uint32_t t = (uint32_t)p[0] | (uint32_t)p[1] << 8, nt = p[2], b = 1u << (nt & 7u), w = (nt >> 3) & 15u;
        int isOn = p[3] != 0u;
        if (nt > 127u || t >= len || t < last) return 0;
        last = t;
        if (p[3] & MV_SEQ_PT) continue;         /* an automation point: no on / off to pair */
        if ((seen[w] & b) && (((on[w] & b) != 0u) == isOn)) return 0;   /* two ons (or two offs) in a row */
        seen[w] |= (uint8_t)b;
        on[w] = (uint8_t)(isOn ? on[w] | b : on[w] & ~b);
        odd[w] ^= (uint8_t)b;
    }
    for (i = 0; i < 16u; i++) if (odd[i]) return 0;   /* (alternating and even: as many offs as ons) */
    return 1;
}

uint32_t mv_seqx_rx(mv_seqx_t *s, mv_clip_t *c, mv_engine_t *e, const uint8_t *body, uint32_t n)
{
    const uint8_t *a = body + 5, *d = body + 9;
    uint32_t tt, k, off, len, i, what;
    uint8_t *r;
    int cmd;
    if (n < 11u || body[5] != MV_SX_AREA) return MV_SX_NONE;   /* (not ours: the program transfer's) */
    cmd = mv_xfer_check(body, n);
    if (!cmd) return MV_SX_NONE;
    if (cmd < 0) return MV_SX_BAD;
    tt = a[1];
    what = (uint32_t)sx_what(tt, &k);
    if (!what) return MV_SX_BAD;
    if (cmd == 0x11) {                          /* RQ1: a read (a new one replaces one being sent) */
        if (n != 14u) return MV_SX_BAD;
        s->d_on = 1; s->d_tt = (uint8_t)tt; s->d_head = 1; s->d_then = 0;
        s->d_headonly = (uint8_t)(!body[9] && !body[10] && !body[11] && body[12] == MV_SX_HEAD);   /* size 32: the head */
        return MV_SX_REQ;
    }
    if (cmd != 0x12 || what == 3u) return MV_SX_BAD;   /* (DT1 only; a factory clip is read only) */
    off = (uint32_t)a[2] << 7 | a[3];
    len = n - 10u;                              /* 41 dev 00 06 12 + 4 address .. data .. sum */
    r = mv_clip_stage(c);
    if (!r || s->play_pend) { s->stg_tt = 0xFFu; return MV_SX_BAD; }   /* (busy: a SAVE waits; a LOAD waits) */
    if (off >= MV_SX_HEAD) {                    /* events: in order, 4 bytes each */
        uint32_t i0 = (off - MV_SX_HEAD) >> 2, m = len >> 2;
        uint8_t *p;
        if ((off - MV_SX_HEAD) & 3u || len & 3u || !m) return MV_SX_BAD;
        if (i0 == 0u) { s->stg_tt = (uint8_t)tt; s->stg_next = 0; s->stg_gen = c->wgen; }   /* a new write */
        else if (s->stg_tt != tt || i0 != s->stg_next || s->stg_gen != c->wgen) { s->stg_tt = 0xFFu; return MV_SX_BAD; }
        if (i0 + m > MV_SEQ_NEV) { s->stg_tt = 0xFFu; return MV_SX_BAD; }   /* (more than a track holds) */
        for (i = 0, p = r + MV_CLIP_HEAD + 4u * i0; i < m; i++, p += 4, d += 4) {
            uint32_t pt = d[0] >= 0x60u, t = pt ? ((uint32_t)(d[0] & 0x1Fu) << 9 | (uint32_t)d[1] << 2) : (uint32_t)d[0] << 7 | d[1];
            p[0] = (uint8_t)t; p[1] = (uint8_t)(t >> 8); p[2] = d[2];   /* (the record's layout) */
            p[3] = (uint8_t)(pt ? MV_SEQ_PT | d[3] : d[3]);   /* (a point: its value, marked) */
        }
        s->stg_next = (uint16_t)(i0 + m);
        return MV_SX_STAGED;
    }
    if (off != 0u || len < 8u) return MV_SX_BAD;   /* the head: from its start, at least to its count */
    {
        uint32_t bars = d[2], root = d[3], quant = d[4], nn = (uint32_t)d[6] | (uint32_t)d[7] << 7;
        if (nn && (s->stg_tt != tt || s->stg_next != nn || s->stg_gen != c->wgen)) { s->stg_tt = 0xFFu; return MV_SX_BAD; }
        s->stg_tt = 0xFFu;                      /* (taken or refused: either way this write is over) */
        if (bars < 1u || bars > MV_SEQ_MAXBAR || !sx_events_ok(r, nn, bars)) return MV_SX_BAD;
        r[0] = 'C'; r[1] = 2; r[2] = (uint8_t)bars; r[3] = (uint8_t)(root <= 126u ? root : MV_SEQ_NOROOT);
        r[4] = (uint8_t)(quant < MV_SQQ_N ? quant : MV_SQQ_OFF); r[5] = 0;
        r[6] = (uint8_t)nn; r[7] = (uint8_t)(nn >> 8);
        for (i = 0; i < MV_CLIP_NAME; i++) {    /* the name (a user clip's): ASCII 32..125, else a space */
            uint32_t ch = len > 8u + i ? d[8u + i] : 32u;
            r[8u + i] = (uint8_t)(ch >= 32u && ch <= 125u ? ch : 32u);
        }
        r[14] = 0; r[15] = 0;
        if (what == 2u) return mv_clip_put(c, k) == MV_CLIP_OK ? MV_SX_CLIP : MV_SX_BAD;
        if (mv_clip_play(c, e, k) == MV_CLIP_AGAIN) { s->play_pend = (uint8_t)(k + 1u); s->stg_gen = c->wgen; }
        else s->loaded = 1;
        return MV_SX_TRACK;
    }
}

int mv_seqx_poll(mv_seqx_t *s, mv_clip_t *c, mv_engine_t *e)
{
    int r;
    if (s->play_pend) {
        if (s->stg_gen != c->wgen) s->play_pend = 0;   /* (a SAVE took rec meanwhile: this LOAD is lost) */
        else if (mv_clip_play(c, e, s->play_pend - 1u) != MV_CLIP_AGAIN) { s->play_pend = 0; s->loaded = 1; }
    }
    r = s->loaded;
    s->loaded = 0;
    return r;
}

/* ---- read ------------------------------------------------------------------------------------------------------- */

/* a live track's events in its loop (a flash clip: decoded and counted, those past LENGTH dropped, as SAVE does) */
static uint32_t sx_track_n(const mv_seq_track_t *T, uint32_t len)
{
    mv_seq_rd_t rd;
    uint32_t t, nt, v, m = 0, i;
    if (!T->rom) {
        const volatile mv_seq_ev_t *ev = T->ev; /* (read through volatile: the ISR owns the events) */
        for (i = 0; i < T->n && i < MV_SEQ_NEV; i++) if (ev[i].tick < len) m++;
        return m;
    }
    mv_seq_rd_start(&rd, T->rom, T->rom_bytes);
    while (mv_seq_rd_next(&rd, &t, &nt, &v)) if (t < len) m++;
    return m;
}

/* the head of what s->d_tt names into h (MV_SX_HEAD bytes); s->d_n set; the read's state from event 0 */
static void sx_head(mv_seqx_t *s, mv_clip_t *c, const mv_engine_t *e, uint8_t *h)
{
    uint32_t k = 0, i, bars = 2u, root = MV_SEQ_NOROOT, quant = MV_SQQ_OFF, flags = 0, nn = 0;
    const char *name = 0;
    uint32_t namelen = 0;
    int what = sx_what(s->d_tt, &k);
    for (i = 0; i < MV_SX_HEAD; i++) h[i] = 0;
    if (what == 1) {                            /* a live track: its LENGTH, ROOT, the QUANT now */
        const mv_seq_track_t *T = &e->seq.t[k];
        s->d_ver = e->seq.ver;
        bars = e->seqb[MV_SQ_LEN(k)];
        s->d_bars = (uint8_t)bars;
        nn = sx_track_n(T, bars * MV_SEQ_BAR);
        root = T->root; quant = e->seqb[MV_SQ_QUANT];
        if (T->rom) { flags |= MV_SXF_FLASH; mv_seq_rd_start(&s->d_rd, T->rom, T->rom_bytes); }
        h[14] = (uint8_t)(s->d_ver & 0x7Fu); h[15] = (uint8_t)(s->d_ver >> 7 & 0x7Fu);
    } else if (what == 2) {                     /* a user clip, as flash (or the SAVE waiting) has it */
        const mv_clip_info_t *dd = &c->dir[k];
        if (dd->used) {
            bars = dd->len; root = dd->root; quant = dd->quant; nn = dd->n;
            name = dd->name; namelen = MV_CLIP_NAME;
        } else flags |= MV_SXF_EMPTY;
    } else if (what == 3) {                     /* a factory clip */
        const mv_fclip_t *f = mv_clip_factory(MV_CLIP_N + k);
        bars = f->bars; root = f->root; nn = f->n; flags |= MV_SXF_FLASH | MV_SXF_RO;
        name = f->name;
        for (namelen = 0; name[namelen] && namelen < 16u; namelen++) {}
        mv_seq_rd_start(&s->d_rd, f->ev, f->bytes);
    }
    if (nn > MV_SX_MAXEV) nn = MV_SX_MAXEV;
    if (!nn && what != 2) flags |= MV_SXF_EMPTY;
    s->d_n = (uint16_t)nn; s->d_i = 0;
    h[0] = 'C'; h[1] = 2; h[2] = (uint8_t)bars; h[3] = (uint8_t)(root <= 127u ? root : 0x7Fu);
    h[4] = (uint8_t)quant; h[5] = (uint8_t)flags; h[6] = (uint8_t)(nn & 0x7Fu); h[7] = (uint8_t)(nn >> 7 & 0x7Fu);
    for (i = 0; i < 16u; i++) {
        uint32_t ch = i < namelen ? (uint8_t)name[i] : 32u;
        ch = ch >= 32u && ch <= 125u ? ch : 32u;
        h[16u + i] = (uint8_t)ch;
        if (i < MV_CLIP_NAME && what == 2) h[8u + i] = (uint8_t)ch;
    }
}

uint32_t mv_seqx_next(mv_seqx_t *s, mv_clip_t *c, const mv_engine_t *e, uint8_t *out, uint32_t cap)
{
    uint8_t buf[MV_SX_CHUNK * 4u > MV_SX_HEAD ? MV_SX_CHUNK * 4u : MV_SX_HEAD];
    uint32_t k = 0, i, m;
    int what;
    if (!s->d_on || cap < MV_XF_MSG_MAX) return 0;
    what = sx_what(s->d_tt, &k);
    if (what == 1 && !s->d_head && (e->seq.ver != s->d_ver || e->seqb[MV_SQ_LEN(k)] != s->d_bars))
        s->d_head = 1;                          /* the track changed under the read: again from its head */
    if (s->d_head) {
        uint32_t tt = s->d_tt;                  /* (a report moves d_tt on to LOWER below: this head is tt's) */
        sx_head(s, c, e, buf);
        s->d_head = 0;
        if (s->d_headonly || !s->d_n) {         /* (nothing more to send) */
            s->d_on = 0;
            if (s->d_then) { s->d_on = 1; s->d_tt = s->d_then; s->d_then = 0; s->d_head = 1; s->d_headonly = 1; }
        }
        return mv_xfer_dt1(out, sx_addr(tt, 0u), buf, MV_SX_HEAD);
    }
    m = s->d_n - s->d_i;
    if (m > MV_SX_CHUNK) m = MV_SX_CHUNK;
    if (what == 1 && !e->seq.t[k].rom) {        /* a RAM track: copied, then checked unchanged */
        const volatile mv_seq_ev_t *ev = e->seq.t[k].ev;
        uint32_t len = (uint32_t)s->d_bars * MV_SEQ_BAR, j = 0, got = 0;
        for (i = 0; got < s->d_i + m && i < MV_SEQ_NEV && i < e->seq.t[k].n; i++) {
            uint32_t t = ev[i].tick;
            if (t >= len) continue;             /* (past a shortened LENGTH: not in the loop) */
            if (got >= s->d_i) { sx_put(buf + j, t, ev[i].note, ev[i].vel); j += 4u; }
            got++;
        }
        if (e->seq.ver != s->d_ver || j != 4u * m) { s->d_head = 1; return mv_seqx_next(s, c, e, out, cap); }
    } else if (what == 2) {                     /* a user clip's record */
        const uint8_t *r = mv_clip_rec(c, k) + MV_CLIP_HEAD + 4u * s->d_i;
        for (i = 0; i < m; i++, r += 4) {
            sx_put(buf + 4u * i, (uint32_t)r[0] | (uint32_t)r[1] << 8, r[2], r[3]);
        }
    } else {                                    /* a flash clip (a factory one, or a live track playing one) */
        uint32_t len = what == 1 ? (uint32_t)s->d_bars * MV_SEQ_BAR : 0xFFFFFFFFu, t, nt, v;
        for (i = 0; i < m; ) {
            if (!mv_seq_rd_next(&s->d_rd, &t, &nt, &v)) break;
            if (t >= len) continue;
            sx_put(buf + 4u * i, t, nt, v);
            i++;
        }
        if (i < m) { s->d_on = 0; return 0; }  /* (ran out early: the coded clip is short of its count) */
    }
    i = s->d_i;
    s->d_i = (uint16_t)(s->d_i + m);
    if (s->d_i >= s->d_n) s->d_on = 0;
    return mv_xfer_dt1(out, sx_addr(s->d_tt, MV_SX_HEAD + 4u * i), buf, 4u * m);
}

/* what a report watches: the tracks' events (seq.ver), their LENGTHs and the QUANT, in one word */
static uint32_t sx_sig(const mv_engine_t *e)
{
    return e->seq.ver * 2654435761u ^ (uint32_t)e->seqb[MV_SQ_LENUP] << 8 ^ (uint32_t)e->seqb[MV_SQ_LENLO] << 16
           ^ (uint32_t)e->seqb[MV_SQ_QUANT] << 24;
}

void mv_seqx_link(mv_seqx_t *s, const mv_engine_t *e) { s->rep_sig = sx_sig(e); }

void mv_seqx_report(mv_seqx_t *s, const mv_engine_t *e, uint32_t now_ms)
{
    uint32_t sig = sx_sig(e);
    if (s->d_on || sig == s->rep_sig || now_ms - s->rep_t < MV_SX_REPORT_MS) return;
    s->rep_sig = sig; s->rep_t = now_ms;
    s->d_on = 1; s->d_tt = MV_SX_TT_TRK; s->d_head = 1; s->d_headonly = 1;   /* UPPER's head, then LOWER's */
    s->d_then = MV_SX_TT_TRK + 1u;
}
