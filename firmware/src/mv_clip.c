/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance clips: implementation (what a clip is, where it lives, SAVE and LOAD in mv_clip.h). Main loop only. */
#include "mv_clip.h"

static uint8_t clip_sec[MV_CLIP_SEC];           /* one sector's image: boot, LOAD, the write (main loop only) */

/* a record's head into info (0: not a clip) */
static int clip_head(const uint8_t *r, mv_clip_info_t *d)
{
    uint32_t i, n = (uint32_t)r[6] | (uint32_t)r[7] << 8;
    d->used = 0;
    if (r[0] != 'C' || r[1] != 1u || r[2] < 1u || r[2] > 4u || n > MV_SEQ_NEV) return 0;
    d->used = 1; d->len = r[2]; d->root = r[3]; d->quant = r[4]; d->n = (uint16_t)n;
    for (i = 0; i < MV_CLIP_NAME; i++) d->name[i] = (char)(r[8u + i] >= 32u && r[8u + i] <= 125u ? r[8u + i] : ' ');
    return 1;
}

/* sector s of the clips into clip_sec (a sector never written, or unreadable: all three empty) */
static void sec_read(uint32_t s)
{
    uint32_t i;
    if (slot_sec_load(MV_CLIP_SLOT, s, clip_sec, MV_CLIP_SEC) != 0)
        for (i = 0; i < MV_CLIP_SEC; i++) clip_sec[i] = 0;
}

void mv_clip_init(mv_clip_t *c)
{
    uint32_t s, j;
    c->pend = MV_CLIP_NONE; c->err = 0;
    for (s = 0; s < SLOT_SECT; s++) {
        sec_read(s);
        for (j = 0; j < MV_CLIP_PER; j++) (void)clip_head(clip_sec + j * MV_CLIP_REC, &c->dir[s * MV_CLIP_PER + j]);
    }
}

int mv_clip_save(mv_clip_t *c, const mv_engine_t *e, uint32_t part, uint32_t k, const char *name)
{
    const mv_seq_track_t *T = &e->seq.t[part & 1u];
    const volatile mv_seq_ev_t *src = T->ev;    /* (read through volatile: the ISR owns the events) */
    uint32_t v = e->seq.ver, n = T->n, len = mv_seq_len(e->seqb, part & 1u), i, m = 0;
    uint8_t *r = c->rec, *p;
    if (k >= MV_CLIP_N) return MV_CLIP_EMPTY;
    if (c->pend != MV_CLIP_NONE && c->pend != k) return MV_CLIP_BUSY;   /* (the same clip again: replaced) */
    if (n > MV_SEQ_NEV) n = MV_SEQ_NEV;
    for (i = 0, p = r + MV_CLIP_HEAD; i < n; i++) {   /* the events in the loop (none past a shortened LENGTH) */
        uint32_t t = src[i].tick;
        if (t >= len) continue;
        p[0] = (uint8_t)t; p[1] = (uint8_t)(t >> 8); p[2] = (uint8_t)(src[i].note & 127u); p[3] = src[i].vel;
        p += 4; m++;
    }
    if (e->seq.ver != v) return MV_CLIP_AGAIN;  /* written meanwhile (a key recorded): the next pass tries again */
    for (i = MV_CLIP_HEAD + 4u * m; i < MV_CLIP_REC; i++) r[i] = 0;
    r[0] = 'C'; r[1] = 1; r[2] = e->seqb[MV_SQ_LEN(part & 1u)]; r[3] = T->root; r[4] = e->seqb[MV_SQ_QUANT]; r[5] = 0;
    r[6] = (uint8_t)m; r[7] = (uint8_t)(m >> 8);
    for (i = 0; i < MV_CLIP_NAME; i++) r[8u + i] = (uint8_t)name[i];
    r[14] = 0; r[15] = 0;
    (void)clip_head(r, &c->dir[k]);             /* shown at once; written when silent (mv_clip_flush) */
    c->pend = (uint8_t)k;
    return MV_CLIP_OK;
}

int mv_clip_load(mv_clip_t *c, mv_engine_t *e, uint32_t k, uint32_t part)
{
    const uint8_t *r, *p;
    mv_clip_info_t d;
    uint32_t i;
    if (k >= MV_CLIP_N || !c->dir[k].used) return MV_CLIP_EMPTY;
    if (e->seq_load) return MV_CLIP_AGAIN;      /* the ISR has not taken the last one yet */
    if (c->pend == k) r = c->rec;               /* (a SAVE not yet written: from RAM) */
    else { sec_read(k / MV_CLIP_PER); r = clip_sec + (k % MV_CLIP_PER) * MV_CLIP_REC; }
    if (!clip_head(r, &d)) return MV_CLIP_EMPTY;
    for (i = 0, p = r + MV_CLIP_HEAD; i < d.n; i++, p += 4) {
        e->seq_lbuf[i].tick = (uint16_t)(p[0] | p[1] << 8);
        e->seq_lbuf[i].note = (uint8_t)(p[2] & 127u);
        e->seq_lbuf[i].vel = p[3];
    }
    e->seq_ln = d.n;
    e->seq_lroot = d.root;
    e->seqb[MV_SQ_LEN(part & 1u)] = d.len;      /* its LENGTH (a setting: the caller keeps it) */
    e->seq_load = (uint8_t)((part & 1u) + 1u);  /* last: the ISR takes it at its next block */
    return MV_CLIP_OK;
}

int mv_clip_flush(mv_clip_t *c)
{
    uint32_t k = c->pend, s, i;
    uint8_t *dst;
    int rc;
    if (k == MV_CLIP_NONE) return 0;
    s = k / MV_CLIP_PER;
    sec_read(s);                                /* its two neighbours as they are */
    dst = clip_sec + (k % MV_CLIP_PER) * MV_CLIP_REC;
    for (i = 0; i < MV_CLIP_REC; i++) dst[i] = c->rec[i];
    rc = slot_sec_save(MV_CLIP_SLOT, s, clip_sec, MV_CLIP_SEC);   /* one erase (~50 ms): only when silent */
    c->pend = MV_CLIP_NONE;
    if (rc) mv_clip_init(c);                    /* (the directory as flash really has it) */
    c->err = rc != 0;                           /* (after the reread: mv_clip_init clears it) */
    return rc;
}
