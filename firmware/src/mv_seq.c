/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance sequencer: implementation (tracks, timing, recording rules in mv_seq.h). */
#include "mv_seq.h"

/* QUANT's grid in ticks (MV_SQQ_*): OFF, 1/16, 1/8, 1/16 triplet, 1/8 triplet */
static const uint8_t SQ_GRID[MV_SQQ_N] = {0, MV_SEQ_PPQN / 4u, MV_SEQ_PPQN / 2u, MV_SEQ_PPQN / 6u, MV_SEQ_PPQN / 3u};
#define SQ_CATCHUP 64u                          /* ticks one call may catch up on the MIDI clock (a long main-loop
                                                   stall, e.g. a flash erase, is followed over a few blocks) */

uint32_t mv_seq_len(const volatile uint8_t *set, uint32_t part)
{
    uint32_t c = set[MV_SQ_LEN(part)];
    if (c < 1u || c > MV_SEQ_MAXBAR) c = MV_SQ_DEF[MV_SQ_LEN(part)];   /* (never out of range from the panel) */
    return MV_SEQ_BAR * c;                      /* bars x 384 ticks (a multiply: no table) */
}

void mv_seq_rd_start(mv_seq_rd_t *r, const uint8_t *p, uint32_t bytes)
{
    r->p = p; r->bytes = (uint16_t)bytes; r->pos = 0; r->tick = 0; r->vel = 100;
}

int mv_seq_rd_next(mv_seq_rd_t *r, uint32_t *tick, uint32_t *note, uint32_t *vel)
{
    uint32_t d = 0, sh = 0, b;
    if (!r->p || r->pos >= r->bytes) return 0;
    do {                                        /* the varint: (delta << 1 | velocity follows) */
        b = r->p[r->pos++];
        d |= (b & 127u) << sh;
        sh += 7u;
    } while ((b & 128u) && r->pos < r->bytes && sh < 28u);
    if (r->pos >= r->bytes) return 0;           /* (cut short: the end) */
    r->tick = (uint16_t)(r->tick + (d >> 1));
    b = r->p[r->pos++];                         /* note | on << 7 */
    if ((d & 1u) && r->pos < r->bytes) r->vel = r->p[r->pos++];
    *tick = r->tick; *note = b & 127u; *vel = b & 128u ? (r->vel ? r->vel : 1u) : 0u;
    return 1;
}

/* a flash track's reader to the first event at or after tick (a LOAD, LENGTH moved, the loop's start) */
static void rom_seek(mv_seq_track_t *T, uint32_t tick)
{
    uint32_t t = 0, n = 0, v = 0;
    mv_seq_rd_start(&T->rd, T->rom, T->rom_bytes);
    while ((T->rhave = (uint8_t)mv_seq_rd_next(&T->rd, &t, &n, &v)) != 0 && t < tick) {}
    T->rt = (uint16_t)t; T->rn = (uint8_t)n; T->rv = (uint8_t)v;
}

/* the first event at or after tick (events are sorted by tick) */
static uint32_t at_or_after(const mv_seq_track_t *T, uint32_t tick)
{
    uint32_t lo = 0, hi = T->n;
    while (lo < hi) { uint32_t m = (lo + hi) >> 1; if (T->ev[m].tick < tick) lo = m + 1u; else hi = m; }
    return lo;
}

/* the notes the track started end (STOP, MUTE, CLEAR) */
static void flush(mv_seq_track_t *T, uint32_t part, mv_seq_emit_t emit, void *ctx)
{
    uint32_t i, k;
    for (i = 0; i < 16u; i++) {
        if (!T->sound[i]) continue;             /* (8 pitches a byte: most bytes are 0) */
        for (k = 0; k < 8u; k++) if (T->sound[i] >> k & 1u) emit(ctx, part, i * 8u + k, 0);
        T->sound[i] = 0;
    }
    for (i = 0; i < 128u; i++) T->played[i] = 0xFFu;   /* (nothing sounds now) */
}

/* a pitch the track starts / ends (the sound[] bit kept) */
static void sound_on(mv_seq_track_t *T, uint32_t part, uint32_t p, uint32_t vel, mv_seq_emit_t emit, void *ctx)
{
    T->sound[p >> 3] |= (uint8_t)(1u << (p & 7u));
    emit(ctx, part, p, vel);
}
static void sound_off(mv_seq_track_t *T, uint32_t part, uint32_t p, mv_seq_emit_t emit, void *ctx)
{
    uint8_t bit = (uint8_t)(1u << (p & 7u));
    if (!(T->sound[p >> 3] & bit)) return;      /* (ended already: MUTE came between) */
    T->sound[p >> 3] &= (uint8_t)~bit;
    emit(ctx, part, p, 0);
}

void mv_seq_init(mv_seq_t *s)
{
    uint32_t i, k;
    for (i = 0; i < 2u; i++) {
        mv_seq_track_t *T = &s->t[i];
        T->n = 0; T->next = 0; T->tick = 0; T->len = 0; T->nrec = 0; T->muted = 0;
        T->root = MV_SEQ_NOROOT; T->xpose = 0;
        T->rom = 0; T->rom_bytes = 0; T->rom_n = 0; T->rhave = 0;
        for (k = 0; k < 16u; k++) T->sound[k] = 0;
        for (k = 0; k < 128u; k++) T->played[k] = 0xFFu;
    }
    s->acc = 0; s->ticks = 0; s->beat = 22050u; s->first = 0; s->ver = 0;
}

void mv_seq_start(mv_seq_t *s)
{
    uint32_t i;
    for (i = 0; i < 2u; i++) { s->t[i].tick = 0; s->t[i].next = 0; s->t[i].len = 0; }   /* (len: set by tick 0) */
    s->acc = 0; s->ticks = 0; s->first = 1;
}

/* an event played (RAM or flash alike): the note's last start ends (at the pitch it started at), then a note-on
 * starts at the offset now (TRANSPOSE), held to 0..127 */
static void play_ev(mv_seq_track_t *T, uint32_t part, uint32_t note, uint32_t vel, mv_seq_emit_t emit, void *ctx)
{
    uint32_t p = T->played[note];
    if (p != 0xFFu) {
        T->played[note] = 0xFFu;
        sound_off(T, part, p, emit, ctx);
    }
    if (vel) {
        int32_t q = (int32_t)note + T->xpose;
        q = q < 0 ? 0 : q > 127 ? 127 : q;
        T->played[note] = (uint8_t)q;
        sound_on(T, part, (uint32_t)q, vel, emit, ctx);
    }
}

/* one tick: each track's events at its tick, then on */
static void step(mv_seq_t *s, const volatile uint8_t *set, mv_seq_emit_t emit, void *ctx)
{
    uint32_t i;
    for (i = 0; i < 2u; i++) {
        mv_seq_track_t *T = &s->t[i];
        uint32_t len = mv_seq_len(set, i), muted = set[MV_SQ_MUTE(i)] != 0u;
        if (len != T->len) {                    /* LENGTH moved (or the start): past the new end, from bar 1 */
            T->len = (uint16_t)len;
            if (T->tick >= len) T->tick = 0;
            if (T->rom) rom_seek(T, T->tick);
            else T->next = (uint16_t)at_or_after(T, T->tick);
        }
        if (muted != T->muted) {                /* MUTE on: what it started ends now */
            T->muted = (uint8_t)muted;
            if (muted) flush(T, i, emit, ctx);
        }
        if (T->rom) {                           /* a factory clip, from flash: the events at this tick */
            while (T->rhave && T->rt == T->tick) {
                uint32_t t, n, v;
                if (!muted) play_ev(T, i, T->rn, T->rv, emit, ctx);
                T->rhave = (uint8_t)mv_seq_rd_next(&T->rd, &t, &n, &v);
                T->rt = (uint16_t)t; T->rn = (uint8_t)n; T->rv = (uint8_t)v;
            }
        } else
            while (T->next < T->n && T->ev[T->next].tick == T->tick) {
                mv_seq_ev_t *ev = &T->ev[T->next++];
                uint32_t note = ev->note & 127u;
                if (ev->note & MV_SEQ_SKIP) { ev->note = (uint8_t)note; continue; }   /* written ahead this pass */
                if (!muted) play_ev(T, i, note, ev->vel, emit, ctx);
            }
        if (++T->tick >= len) {                 /* the loop's end: round again */
            T->tick = 0; T->next = 0;
            if (T->rom) rom_seek(T, 0);
        }
    }
    s->ticks++;
}

void mv_seq_tick(mv_seq_t *s, const volatile uint8_t *set, uint32_t n, uint32_t beat, uint32_t lock,
                 mv_seq_emit_t emit, void *ctx)
{
    uint32_t k = 0;
    if (!beat) beat = 22050u;                   /* (the engine always sets it; 120 BPM if not) */
    s->beat = beat;
    if (s->first) { s->first = 0; s->acc = 0; step(s, set, emit, ctx); }   /* bar 1 plays at once */
    if (lock != ~0u)                            /* the MIDI clock is ahead: catch up to its pulse */
        while (s->ticks <= lock && k++ < SQ_CATCHUP) { step(s, set, emit, ctx); s->acc = 0; }
    s->acc += n * MV_SEQ_PPQN;                  /* (16 x 96: no overflow) */
    while (s->acc >= beat) {
        if (lock != ~0u && s->ticks >= lock + 4u) { s->acc = beat - 1u; break; }   /* a pulse ahead: wait for it */
        s->acc -= beat;
        step(s, set, emit, ctx);
    }
}

/* the tick a key came on (the nearer of the two around now), unwrapped: 0 .. len, and the played-up-to mark of
 * the pass it falls in (cur): an event at an unwrapped tick >= cur has not been played this pass */
static uint32_t now_tick(const mv_seq_t *s, const mv_seq_track_t *T, uint32_t *cur)
{
    uint32_t up = 2u * s->acc >= s->beat ? 1u : 0u;   /* past half way to the next tick */
    *cur = T->tick ? T->tick : T->len;          /* (tick 0 next: the whole last pass has played) */
    return *cur - 1u + up;
}

/* an event into its place by tick (after those at the same tick: the order they were written); skip: it lies
 * ahead of the playhead this pass (the key already sounded live) */
static void insert(mv_seq_track_t *T, uint32_t tick, uint32_t note, uint32_t vel, int skip)
{
    uint32_t i = T->n, lo = 0, hi = T->n;
    while (lo < hi) { uint32_t m = (lo + hi) >> 1; if (T->ev[m].tick <= tick) lo = m + 1u; else hi = m; }
    for (; i > lo; i--) T->ev[i] = T->ev[i - 1u];   /* (1 KB at most, a key at a time) */
    T->ev[lo].tick = (uint16_t)tick;
    T->ev[lo].note = (uint8_t)(note | (skip ? MV_SEQ_SKIP : 0u));
    T->ev[lo].vel = (uint8_t)vel;
    T->n++;
    if (vel && T->root == MV_SEQ_NOROOT) T->root = (uint8_t)note;   /* the first note-on: TRANSPOSE's ROOT */
    if (tick < T->tick) T->next++;              /* before the playhead: the next event to play moved up one */
}
/* (insert's callers bump ver: s is not passed to insert) */

/* the note-off of rec entry r, now (released, disarmed or stopped): moved by the note-on's shift, a tick after
 * the note-on at least; the entry is dropped */
static void rec_off(mv_seq_t *s, mv_seq_track_t *T, uint32_t r)
{
    uint32_t cur, len = T->len, q;
    int32_t u = (int32_t)now_tick(s, T, &cur) + T->rec_shift[r];   /* unwrapped: may be < 0 or >= len */
    q = (uint32_t)(u < 0 ? u + (int32_t)len : u >= (int32_t)len ? u - (int32_t)len : u);
    if (q == T->rec_on[r]) {                    /* let go within the tick it started on: a tick long */
        u++;
        q = q + 1u >= len ? 0u : q + 1u;
    }
    insert(T, q, T->rec_note[r], 0, u >= (int32_t)cur);
    s->ver++;
    T->nrec--;                                  /* the last entry into its place */
    T->rec_note[r] = T->rec_note[T->nrec]; T->rec_shift[r] = T->rec_shift[T->nrec]; T->rec_on[r] = T->rec_on[T->nrec];
}

void mv_seq_key(mv_seq_t *s, const volatile uint8_t *set, uint32_t part, uint32_t note, uint32_t vel)
{
    mv_seq_track_t *T = &s->t[part & 1u];
    uint32_t r, cur, raw, g, qq;
    if (!T->len || note > 127u) return;         /* (not started yet: tick 0 sets the length) */
    for (r = 0; r < T->nrec && T->rec_note[r] != note; r++) {}
    if (!vel) {                                 /* a key up: its note-off, if its note-on was written */
        if (r < T->nrec) rec_off(s, T, r);
        return;
    }
    if (r < T->nrec || T->nrec >= MV_SEQ_NHELD) return;   /* (already held / too many held: plays, not written) */
    if (T->n + 2u + T->nrec > MV_SEQ_NEV) return;   /* no room for it and every note-off still to come: full */
    raw = now_tick(s, T, &cur);
    g = set[MV_SQ_QUANT] < MV_SQQ_N ? SQ_GRID[set[MV_SQ_QUANT]] : 0u;
    qq = g ? (raw + g / 2u) / g * g : raw;      /* the nearest grid line (a key-down, not per sample: a divide is
                                                   fine); unwrapped: len means bar 1 of the next pass */
    if (qq > T->len) qq = T->len;               /* (a grid that does not divide the loop: never past its end) */
    T->rec_note[T->nrec] = (uint8_t)note;
    T->rec_shift[T->nrec] = (int16_t)((int32_t)qq - (int32_t)raw);
    T->rec_on[T->nrec] = (uint16_t)(qq >= T->len ? qq - T->len : qq);
    insert(T, T->rec_on[T->nrec], note, vel & 127u ? vel & 127u : 1u, qq >= cur);
    s->ver++;
    T->nrec++;
}

void mv_seq_disarm(mv_seq_t *s, uint32_t part)
{
    mv_seq_track_t *T = &s->t[part & 1u];
    if (!T->len) { T->nrec = 0; return; }       /* (never started: nothing was written) */
    while (T->nrec) rec_off(s, T, 0);           /* the keys held: their note-offs now */
}

void mv_seq_stop(mv_seq_t *s, const volatile uint8_t *set, mv_seq_emit_t emit, void *ctx)
{
    uint32_t i;
    (void)set;
    for (i = 0; i < 2u; i++) {
        mv_seq_disarm(s, i);
        flush(&s->t[i], i, emit, ctx);
    }
    s->first = 0;
}

void mv_seq_clear(mv_seq_t *s, uint32_t part, mv_seq_emit_t emit, void *ctx)
{
    mv_seq_track_t *T = &s->t[part & 1u];
    flush(T, part & 1u, emit, ctx);
    T->n = 0; T->next = 0; T->nrec = 0;         /* (the playhead runs on: an armed track records afresh) */
    T->rom = 0;                                 /* (a factory clip: left) */
    T->root = MV_SEQ_NOROOT; T->xpose = 0;      /* (TRANSPOSE: back to none) */
    s->ver++;
}

void mv_seq_load(mv_seq_t *s, uint32_t part, const mv_seq_ev_t *ev, uint32_t n, uint32_t root, mv_seq_emit_t emit,
                 void *ctx)
{
    mv_seq_track_t *T = &s->t[part & 1u];
    uint32_t i;
    if (n > MV_SEQ_NEV) n = MV_SEQ_NEV;
    flush(T, part & 1u, emit, ctx);             /* what it was playing ends */
    for (i = 0; i < n; i++) {                   /* (1 KB at most, once a LOAD: a few us on the ISR) */
        T->ev[i].tick = ev[i].tick;
        T->ev[i].note = (uint8_t)(ev[i].note & 127u);   /* (no skip marks: nothing was played live) */
        T->ev[i].vel = ev[i].vel;
    }
    T->n = (uint16_t)n; T->nrec = 0; T->rom = 0;
    T->next = (uint16_t)at_or_after(T, T->tick);   /* (the playhead runs on, from where it is) */
    T->root = (uint8_t)(root <= 127u ? root : MV_SEQ_NOROOT); T->xpose = 0;
    s->ver++;
}

void mv_seq_load_rom(mv_seq_t *s, uint32_t part, const uint8_t *rom, uint32_t bytes, uint32_t n, uint32_t root,
                     mv_seq_emit_t emit, void *ctx)
{
    mv_seq_track_t *T = &s->t[part & 1u];
    flush(T, part & 1u, emit, ctx);             /* what it was playing ends */
    T->n = 0; T->next = 0; T->nrec = 0;         /* (its RAM events: none) */
    T->rom = rom; T->rom_bytes = (uint16_t)bytes; T->rom_n = (uint16_t)n;
    rom_seek(T, T->tick);                       /* (the playhead runs on, from where it is) */
    T->root = (uint8_t)(root <= 127u ? root : MV_SEQ_NOROOT); T->xpose = 0;
    s->ver++;
}

void mv_seq_unrom(mv_seq_t *s, uint32_t part)
{
    mv_seq_track_t *T = &s->t[part & 1u];
    mv_seq_rd_t r;
    uint32_t t, n, v, k = 0;
    if (!T->rom) return;
    mv_seq_rd_start(&r, T->rom, T->rom_bytes);
    while (k < MV_SEQ_NEV && mv_seq_rd_next(&r, &t, &n, &v)) {   /* (sorted already: as coded) */
        T->ev[k].tick = (uint16_t)t; T->ev[k].note = (uint8_t)n; T->ev[k].vel = (uint8_t)v;
        k++;
    }
    T->n = (uint16_t)k; T->rom = 0;
    T->next = (uint16_t)at_or_after(T, T->tick);   /* (what sounds keeps sounding: played[] is the same notes') */
    s->ver++;
}

int mv_seq_xpose_on(const mv_seq_t *s, const volatile uint8_t *set, uint32_t part)
{
    uint32_t x = set[MV_SQ_XPOSE];
    int on = (part & 1u) == 1u ? x == MV_SQX_LO || x == MV_SQX_BOTH : x == MV_SQX_UP || x == MV_SQX_BOTH;
    return on && s->t[part & 1u].root != MV_SEQ_NOROOT;
}

void mv_seq_transpose(mv_seq_t *s, uint32_t part, uint32_t key)
{
    mv_seq_track_t *T = &s->t[part & 1u];
    int32_t d;
    if (T->root == MV_SEQ_NOROOT) return;
    d = (int32_t)key - (int32_t)T->root;
    T->xpose = (int8_t)(d < -MV_SEQ_XMAX ? -MV_SEQ_XMAX : d > MV_SEQ_XMAX ? MV_SEQ_XMAX : d);
}
