/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM: the sequencer's controls, clock, clips and files (Kris 26 Nov 2026, P6a of
 * claude/jt80x0-vibe-p6-seq-proposal-2026-11-26.md, "go with recommendations": 1a 2a 3a 4a 5a 6a 7a).
 * The sequencer itself is the engine's (mv_seq.c: two tracks, real-time REC with QUANT, overdub, LENGTH 1-32 bars,
 * TRANSPOSE, the CC automation), and runs in the audio flow. This file gives it
 *   CLOCK      its own tempo (GLOBAL TEMPO) and the VIBE's PLAY / STOP while nothing leads. While the host plays (a DAW,
 *              or a MIDI clock: SDK 27 gives both as vibe_transport) it runs from the host's position: it starts at the
 *              song's tick (mv_seq_locate: each track at song mod its LENGTH), is held to the host's quarters by the
 *              engine's own clock lock (a pulse each 1/24 quarter, set before every 16-frame piece: jt_sq_piece), is
 *              placed again when the host loops or jumps (more than JT_SQ_CHASE ticks from where it is), and stops
 *              when the host stops. A position before the song's start (a count-in) waits for beat 0
 *   KEYS       REC: a tap arms / disarms the track(s) PART says (BOTH: both; SINGLE: UPPER) and starts the transport;
 *              held 1 s: CLEAR? – a value knob picks ALL / NOTES / AUTO, REC again: yes, another button or 3 s: no.
 *              PLAY: runs / stops (stopping disarms). STOP: stops. While the host leads, both are its
 *   SEQ (K6)   SEQ 1/3 LENGTH · QUANT · MUTE L · MUTE U   SEQ 2/3 TRANSPOSE · ROOT   CLIP: CLIP · MODE · CURSOR · CHAR
 *              (F5 there: LOAD / SAVE the clip). LENGTH, ROOT and the clips act on PART's track (BOTH: UPPER)
 *   PADS       on SEQ: 1-12 the clips (tap: into PART's track at its next bar, at once when stopped; SHIFT + pad:
 *              the track saved there), 13 / 14 MUTE LOWER / UPPER, 15 / 16 REC LOWER / UPPER. Elsewhere the programs
 *   AUTOMATION the points the engine plays are applied in the audio (jt_sq_after: the FM-1's auto_play) and mirrored
 *              to the control flow (jt_edit.h), so it plays right with nothing drawing; STOP puts each moved parameter
 *              back as it was when the transport started (or at the program change since)
 *   FILES      clips/c01.jt .. c12.jt (the FM-1's clip record: a 16-byte head, 4 bytes an event, MV_SEQ_NEV events),
 *              tracks.jt (the six settings, then each track as such a record): written once things rest 1.5 s, and
 *              on VIBE_EV_SAVE; read at the start (a DAW project gives its song back; the VIBE keeps it at power-off)
 *   FLOWS      the control flow asks through the queue (Q_SQ .. Q_FORGET). A clip goes to the audio in a buffer per
 *              track (sq_ld: the control fills it while sq_ld_req = sq_ld_ack, then counts req up; the audio counts
 *              ack up once copied). The audio shows the tracks in a copy kept under a sequence count (sq_snap, taken
 *              again when the events, a ROOT or a LENGTH change), and its state in sq_pub_*.
 * No factory clips (2a): the public build carries none; clips come in from the web page (P6b). */
#pragma once
#include "jt_store.h"

#define JT_CLIP_N 12u                           /* the user clips, as the FM-1's */
#define JT_CLIP_NAME 6u
#define JT_CLIP_HEAD 16u
#define JT_CLIP_REC (JT_CLIP_HEAD + 4u * MV_SEQ_NEV)   /* 1040 B: the FM-1's record, full or not */
#define JT_TRK_BYTES (8u + 2u * JT_CLIP_REC)           /* tracks.jt: the settings (8), UPPER's record, LOWER's */
#define JT_SQ_CHASE 12                          /* ticks from the host's position that are a jump (a pulse is 4) */
_Static_assert(JT_TRK_BYTES <= JT_ST_MAX, "jt_seq: tracks.jt fits the store's buffer");
_Static_assert(MV_PART_UP == 0u && MV_PART_LO == 1u, "jt_seq: the tracks are indexed by part");
enum { SK_CLIP = 8, SK_TRK };

typedef struct { uint16_t n; uint8_t root, len; mv_seq_ev_t ev[MV_SEQ_NEV]; } jt_trk_t;   /* a track: events, ROOT, bars */
static void say(const char *s);                 /* (main.c) */

/* ---- between the flows -------------------------------------------------------------------------------------------- */
static volatile uint32_t sq_pub_run, sq_pub_arm, sq_pub_lead, sq_pub_tick[2];   /* (audio writes) running, armed, the
                                                   host leading, each track's next tick */
static jt_trk_t sq_snap[2];                     /* (audio writes) the tracks, under .. */
static volatile uint32_t sq_snap_seq, sq_snap_ver;   /* .. a count odd while it writes; +1 each copy */
static jt_trk_t sq_ld[2];                       /* (control writes) a track to load, per track */
static volatile uint32_t sq_ld_req[2], sq_ld_ack[2];

/* ---- the audio's ---------------------------------------------------------------------------------------------------- */
static int sq_lead;                             /* the host leads (plays, at or past beat 0) */
static double sq_pos, sq_qpe;                   /* while it does: the quarter the next piece starts at; quarters a frame */
static uint32_t sq_lpend[2];                    /* a load waiting: 1 at once, 2 at the track's next bar */
static uint32_t sq_snapped, au_live;            /* the engine's ver last copied; the transport as auto saw it */
static mv_patch_t au_snap[2];                   /* the sound STOP goes back to .. */
static uint8_t au_moved[2][16];                 /* .. for the CCs the automation moved (a bit a CC) */

static void au_forget(void)                     /* the sound as it is now: the one STOP goes back to */
{
    uint32_t i;
    for (i = 0; i < 16u; i++) au_moved[0][i] = au_moved[1][i] = 0;
    au_snap[0] = eng.patch; au_snap[1] = eng.lower;
}

static void jt_sq_apply(uint32_t type, uint32_t a, uint32_t b)   /* (audio, jt_drain) the control flow's asks */
{
    switch (type) {
    case Q_SQ: if (a < MV_SQ_N) eng.seqb[a] = (uint8_t)b; break;
    case Q_RUN:                                 /* PLAY / STOP (the host's own while it leads) */
        if (sq_lead) break;
        if (!b) { eng.seq_run = 0; eng.seq_arm = 0; }
        else if (!eng.seq_run) { eng.seq_ext = 0; eng.seq_run = 1; }
        break;
    case Q_ARM:                                 /* armed: running (as the FM-1's REC) */
        eng.seq_arm = (uint8_t)(b & 3u);
        if ((b & 3u) && !eng.seq_run && !sq_lead) { eng.seq_ext = 0; eng.seq_run = 1; }
        break;
    case Q_CLR: if (b >= MV_SQC_ALL && b <= MV_SQC_AUTO) eng.seq_clear[a & 1u] = (uint8_t)b; break;
    case Q_KNOB: (void)mv_engine_knob(&eng, a >> 8 & 1u, a & 127u, b & 127u); break;
    case Q_ROOT: eng.seq.t[a & 1u].root = (uint8_t)(b <= 127u ? b : MV_SEQ_NOROOT); break;
    case Q_LOAD: sq_lpend[a & 1u] = b ? 2u : 1u; break;
    case Q_FORGET: au_forget(); break;
    default: break;
    }
}

static uint32_t sq_song(void) { return (uint32_t)(sq_pos * (double)MV_SEQ_PPQN); }   /* the host's tick now */

/* (audio) once a host block, after the queue, before the pieces: who leads, a start, a stop, a jump. off: engine frames
 * already played from the last block's queued piece (the first piece rendered now starts that far in) */
static void jt_sq_block(const vibe_transport_info *tp, uint32_t off)
{
    int lead = tp->has && tp->playing && tp->beat >= 0.0;
    double bpm = tp->has && tp->bpm > 0.0f ? (double)tp->bpm : (double)jt_own_bpm;
    sq_qpe = bpm / (60.0 * (double)MV_FS);
    sq_pos = lead ? tp->beat + (double)off * sq_qpe : 0.0;
    if (lead && !sq_lead) { eng.seq_ext = 1; eng.seq_run = 1; eng.seq_at = sq_song() + 1u; }   /* from the song's tick */
    else if (!lead && sq_lead) { eng.seq_run = 0; eng.seq_arm = 0; eng.seq_ext = 0; }
    else if (lead && eng.seq_live && !eng.seq_at) {   /* a loop or a jump: placed again */
        int32_t d = (int32_t)(eng.seq.ticks - sq_song());
        if (d > JT_SQ_CHASE || d < -JT_SQ_CHASE) eng.seq_at = sq_song() + 1u;
    }
    sq_lead = lead;
}

/* (audio) before each 16-frame piece (jt_core.h's JT_PIECE_HOOK): the host's pulse; a clip waiting for its bar */
static void jt_sq_piece(void)
{
    uint32_t p, i;
    if (sq_lead) { eng.seq_pulse = (uint32_t)(sq_pos * 24.0) + 1u; sq_pos += 16.0 * sq_qpe; }
    for (p = 0; p < 2u; p++) {
        const jt_trk_t *t = &sq_ld[p];
        if (!sq_lpend[p] || eng.seq_load) continue;   /* (one load at a time: the other on the next piece) */
        if (sq_lpend[p] == 2u && eng.seq_live && eng.seq.t[p].tick % MV_SEQ_BAR) continue;   /* (its bar: not yet) */
        for (i = 0; i < t->n && i < MV_SEQ_NEV; i++) eng.seq_lbuf[i] = t->ev[i];
        eng.seq_ln = (uint16_t)i; eng.seq_lroot = t->root; eng.seq_lrom = 0; eng.seq_lbytes = 0;
        eng.seqb[MV_SQ_LEN(p)] = t->len;        /* (its LENGTH with it: ED has it already) */
        eng.seq_load = (uint8_t)(p + 1u);       /* (the engine takes it in this piece) */
        sq_lpend[p] = 0;
        VIBE_RELEASE();                         /* copied, then said */
        sq_ld_ack[p]++;
    }
}

/* (audio) after the block: the automation (6a), the state, the tracks' copy */
static void jt_sq_after(void)
{
    int32_t m;
    uint32_t p, i, did = 0, dirty;
    if (eng.seq_live != au_live) {              /* started or stopped */
        au_live = eng.seq_live;
        if (!au_live) {                         /* STOP: each moved parameter back */
            for (p = 0; p < 2u; p++) {
                for (i = 0; i < 128u; i++)
                    if (au_moved[p][i >> 3] >> (i & 7u) & 1u && MV_CC_PARAM[i] != MV_NONE) {
                        mv_param_set(p ? &eng.lower : &eng.patch, MV_CC_PARAM[i], mv_param_get(&au_snap[p], MV_CC_PARAM[i]));
                        did |= 1u << p;
                    }
                for (i = 0; i < 16u; i++) eng.au_take[p][i] = 0;
            }
            while (mv_engine_point(&eng) >= 0) {}   /* (anything left over: dropped) */
        }
        au_forget();
    }
    if (au_live) {
        for (p = 0; p < 2u; p++)                /* a knob's takeover ends when its track goes round */
            if (eng.au_take_w[p] != eng.seq.t[p].wraps)
                for (i = 0; i < 16u; i++) eng.au_take[p][i] = 0;
        while ((m = mv_engine_point(&eng)) >= 0) {
            uint32_t cc = (uint32_t)m >> 7 & 127u, v = (uint32_t)m & 127u, id = MV_CC_PARAM[cc];
            p = (uint32_t)m >> 14 & 1u;
            if (eng.au_take[p][cc >> 3] >> (cc & 7u) & 1u || id == MV_NONE) continue;
            au_moved[p][cc >> 3] |= (uint8_t)(1u << (cc & 7u));
            (void)mv_cc_apply(p ? &eng.lower : &eng.patch, cc, v);
            did |= 1u << p;
        }
    }
    if (did & 1u) mir_part(0);
    if (did & 2u) mir_part(1);
    sq_pub_run = eng.seq_live; sq_pub_arm = eng.seq_arm; sq_pub_lead = (uint32_t)sq_lead;
    sq_pub_tick[0] = eng.seq.t[0].tick; sq_pub_tick[1] = eng.seq.t[1].tick;
    dirty = eng.seq.ver != sq_snapped;
    for (p = 0; p < 2u; p++) dirty |= sq_snap[p].root != eng.seq.t[p].root || sq_snap[p].len != eng.seqb[MV_SQ_LEN(p)];
    if (dirty) {                                /* the tracks' copy (only when they changed: recording, a load, CLEAR) */
        sq_snapped = eng.seq.ver;
        sq_snap_seq++;                          /* odd: being written */
        VIBE_RELEASE();
        for (p = 0; p < 2u; p++) {
            const mv_seq_track_t *T = &eng.seq.t[p];
            jt_trk_t *s = &sq_snap[p];
            s->n = T->n; s->root = T->root; s->len = eng.seqb[MV_SQ_LEN(p)];
            for (i = 0; i < T->n; i++) {
                s->ev[i].tick = T->ev[i].tick; s->ev[i].note = (uint8_t)(T->ev[i].note & 127u); s->ev[i].vel = T->ev[i].vel;
            }
        }
        sq_snap_ver++;
        VIBE_RELEASE();
        sq_snap_seq++;                          /* even: whole */
    }
}

/* ---- the control flow's ----------------------------------------------------------------------------------------- */
typedef struct { uint8_t used, len, root, quant; uint16_t n; char name[JT_CLIP_NAME]; } jt_clip_t;
static jt_clip_t cl_dir[JT_CLIP_N];             /* what each clip file holds */
static uint32_t cl_sel, cl_mode, cl_pos;        /* the CLIP page: the clip, LOAD / SAVE, the name's cursor .. */
static char cl_name[JT_CLIP_NAME];              /* .. and the name a SAVE gives */
static jt_trk_t sq_view[2];                     /* the tracks as last copied from the audio */
static uint32_t sq_view_ver;
static uint32_t sq_rec_down, sq_rec_did, sq_ask, sq_ask_mode, sq_ask_mask;   /* REC held; the CLEAR question */
static uint64_t sq_rec_t, sq_ask_t;
static uint8_t sq_rec[JT_TRK_BYTES];            /* a record read or written */

static uint32_t sq_trk(void)                    /* the track PART points at (BOTH: UPPER) */
{
    return ED.perf[MV_PF_MODE] == MV_KEY_SINGLE || panel != PANEL_LOWER ? MV_PART_UP : MV_PART_LO;
}
static uint32_t sq_mask(void)                   /* the tracks PART says, as bits (REC, CLEAR) */
{
    if (ED.perf[MV_PF_MODE] == MV_KEY_SINGLE || panel == PANEL_UPPER) return 1u << MV_PART_UP;
    return panel == PANEL_LOWER ? 1u << MV_PART_LO : 3u;
}

static int sq_read(void)                        /* sq_view from the audio's copy: 1 = it is whole and current */
{
    uint32_t s = sq_snap_seq, v, p, i;
    VIBE_ACQUIRE();
    if (s & 1u) return 0;
    v = sq_snap_ver;
    if (v == sq_view_ver) return 1;
    for (p = 0; p < 2u; p++) {
        jt_trk_t *d = &sq_view[p];
        d->n = sq_snap[p].n < MV_SEQ_NEV ? sq_snap[p].n : (uint16_t)MV_SEQ_NEV;
        d->root = sq_snap[p].root; d->len = sq_snap[p].len;
        for (i = 0; i < d->n; i++) d->ev[i] = sq_snap[p].ev[i];
    }
    VIBE_ACQUIRE();
    if (sq_snap_seq != s) return 0;             /* (written meanwhile: the next call copies again) */
    sq_view_ver = v;
    return 1;
}

/* ---- the clip record (the FM-1's, mv_clip.h: format 2) ---------------------------------------------------------- */
static void rec_from(uint8_t *r, const jt_trk_t *t, uint32_t quant, const char *name)
{
    uint32_t i;
    for (i = 0; i < JT_CLIP_REC; i++) r[i] = 0;
    r[0] = 'C'; r[1] = 2; r[2] = t->len; r[3] = t->root; r[4] = (uint8_t)quant;
    r[6] = (uint8_t)(t->n & 255u); r[7] = (uint8_t)(t->n >> 8);
    for (i = 0; i < JT_CLIP_NAME; i++) r[8u + i] = (uint8_t)name[i];
    for (i = 0; i < t->n && i < MV_SEQ_NEV; i++) {
        uint8_t *e = r + JT_CLIP_HEAD + 4u * i;
        e[0] = (uint8_t)(t->ev[i].tick & 255u); e[1] = (uint8_t)(t->ev[i].tick >> 8); e[2] = t->ev[i].note; e[3] = t->ev[i].vel;
    }
}
static int rec_to(const uint8_t *r, jt_trk_t *t, jt_clip_t *info)   /* 1 = a clip (its events in order, in its length) */
{
    uint32_t n = r[6] | (uint32_t)r[7] << 8, len = r[2], i, last = 0;
    if (r[0] != 'C' || r[1] != 2u || n > MV_SEQ_NEV || len < 1u || len > MV_SEQ_MAXBAR) return 0;
    for (i = 0; i < n; i++) {
        const uint8_t *e = r + JT_CLIP_HEAD + 4u * i;
        uint32_t tick = e[0] | (uint32_t)e[1] << 8;
        if (tick >= len * MV_SEQ_BAR || tick < last) return 0;
        last = tick;
        if (t) { t->ev[i].tick = (uint16_t)tick; t->ev[i].note = (uint8_t)(e[2] & 127u); t->ev[i].vel = e[3]; }
    }
    if (t) { t->n = (uint16_t)n; t->len = (uint8_t)len; t->root = r[3] <= 127u ? r[3] : (uint8_t)MV_SEQ_NOROOT; }
    if (info) {
        info->used = 1; info->len = (uint8_t)len; info->root = r[3]; info->quant = r[4]; info->n = (uint16_t)n;
        for (i = 0; i < JT_CLIP_NAME; i++) info->name[i] = r[8u + i] >= 32u && r[8u + i] <= 125u ? (char)r[8u + i] : ' ';
    }
    return 1;
}
static void clip_file(uint32_t k, char *s)      /* "clips/c01.jt" */
{
    const char *a = "clips/c";
    uint32_t i = 0;
    while (*a) s[i++] = *a++;
    s[i++] = (char)('0' + (k + 1u) / 10u); s[i++] = (char)('0' + (k + 1u) % 10u);
    s[i++] = '.'; s[i++] = 'j'; s[i++] = 't'; s[i] = 0;
}
static void clip_default(uint32_t k, char *name)   /* "CLIP01" */
{
    const char *a = "CLIP";
    uint32_t i;
    for (i = 0; i < 4u; i++) name[i] = a[i];
    name[4] = (char)('0' + (k + 1u) / 10u); name[5] = (char)('0' + (k + 1u) % 10u);
}
static void clip_pick(uint32_t k)               /* the CLIP page's clip; its name to edit */
{
    uint32_t i;
    cl_sel = k % JT_CLIP_N; cl_pos = 0;
    if (cl_dir[cl_sel].used) for (i = 0; i < JT_CLIP_NAME; i++) cl_name[i] = cl_dir[cl_sel].name[i];
    else clip_default(cl_sel, cl_name);
}

static void clip_load(uint32_t k, uint32_t p)   /* clip k into track p: at its next bar while running */
{
    static char msg[24];
    char f[16];
    uint32_t i = 0;
    const char *w = "LOADED ";
    if (k >= JT_CLIP_N || !cl_dir[k].used) { say("CLIP EMPTY"); return; }
    if (sq_ld_req[p] != sq_ld_ack[p]) { say("A LOAD IS WAITING"); return; }
    clip_file(k, f);
    if (!st_read(f, SK_CLIP, sq_rec, JT_CLIP_REC) || !rec_to(sq_rec, &sq_ld[p], 0)) { say("CLIP UNREADABLE"); return; }
    ED.sq[MV_SQ_LEN(p)] = SENT.sq[MV_SQ_LEN(p)] = sq_ld[p].len;   /* (the audio sets it with the events) */
    sq_ld_req[p]++;
    (void)q_push(Q_LOAD, p, sq_pub_run ? 1u : 0u);   /* (after the buffer: q_push releases) */
    while (*w) msg[i++] = *w++;
    for (w = cl_dir[k].name; i < 7u + JT_CLIP_NAME; ) msg[i++] = *w++;
    msg[i++] = ' '; msg[i++] = '>'; msg[i++] = ' ';
    for (w = p == MV_PART_UP ? "UPPER" : "LOWER"; *w; ) msg[i++] = *w++;
    msg[i] = 0;
    say(msg);
}
static void clip_save(uint32_t k, uint32_t p, const char *name)   /* track p as it is now into clip k */
{
    char f[16];
    uint32_t i;
    if (k >= JT_CLIP_N) return;
    if (!sq_read()) { say("TRY AGAIN"); return; }
    if (!sq_view[p].n) { say("TRACK EMPTY"); return; }
    clip_file(k, f);
    rec_from(sq_rec, &sq_view[p], ED.sq[MV_SQ_QUANT], name);
    st_writes++;
    if (st_write(f, SK_CLIP, sq_rec, JT_CLIP_REC)) { say("CLIP NOT SAVED"); return; }
    (void)rec_to(sq_rec, 0, &cl_dir[k]);
    for (i = 0; i < JT_CLIP_NAME; i++) cl_name[i] = cl_dir[k].name[i];
    say(p == MV_PART_UP ? "UPPER SAVED" : "LOWER SAVED");
}

/* ---- tracks.jt ---------------------------------------------------------------------------------------------------- */
static uint8_t w_trk[JT_TRK_BYTES];             /* as last written (or read at the start) */
static uint8_t s_sq[MV_SQ_N];
static uint32_t s_trk_ver;
static uint64_t s_trk_t;
static void trk_to(uint8_t *d)                  /* the settings and both tracks (sq_view) as tracks.jt's bytes */
{
    static const char blank[JT_CLIP_NAME] = {' ', ' ', ' ', ' ', ' ', ' '};
    uint32_t i, p;
    for (i = 0; i < 8u; i++) d[i] = i < MV_SQ_N ? ED.sq[i] : 0u;
    for (p = 0; p < 2u; p++) rec_from(d + 8u + p * JT_CLIP_REC, &sq_view[p], ED.sq[MV_SQ_QUANT], blank);
}
static void sq_store_poll(int now)              /* (store_poll) once the tracks and their settings rest; now: at once */
{
    uint64_t t = vibe_frames();
    uint32_t v = sq_snap_ver;
    if (v != s_trk_ver || differ(ED.sq, s_sq, MV_SQ_N)) {
        s_trk_ver = v; cp(s_sq, ED.sq, MV_SQ_N); s_trk_t = t;
        if (!now) return;
    }
    if (!now && t - s_trk_t < (uint64_t)st_fs * 3u / 2u) return;
    if (!sq_read()) return;                     /* (the audio writing it: on the next call) */
    trk_to(sq_rec);
    if (differ(sq_rec, w_trk, JT_TRK_BYTES) && !st_write("tracks.jt", SK_TRK, sq_rec, JT_TRK_BYTES)) {
        cp(w_trk, sq_rec, JT_TRK_BYTES); st_writes++;
    }
}

/* ---- the start ---------------------------------------------------------------------------------------------------- */
static void jt_sq_init(void)                    /* (vibe_on_init, after jt_init: a fresh instance) */
{
    uint32_t p;
    sq_lead = 0; sq_pos = sq_qpe = 0.0; sq_snapped = ~0u; au_live = 0; au_forget();
    sq_snap_seq = 0; sq_snap_ver = 0; sq_view_ver = ~0u;
    for (p = 0; p < 2u; p++) {
        sq_lpend[p] = 0; sq_ld_req[p] = sq_ld_ack[p] = 0; sq_snap[p].n = 0; sq_snap[p].root = MV_SEQ_NOROOT;
        sq_snap[p].len = 0; sq_pub_tick[p] = 0;
    }
    sq_pub_run = sq_pub_arm = sq_pub_lead = 0;
    sq_rec_down = sq_rec_did = sq_ask = 0; cl_mode = 0;
}
static void sq_boot(void)                       /* (vibe_on_init, after store_boot) the clips, the song left */
{
    char f[16];
    uint32_t k, p, i;
    for (k = 0; k < JT_CLIP_N; k++) {
        clip_file(k, f);
        cl_dir[k].used = 0;
        if (st_read(f, SK_CLIP, sq_rec, JT_CLIP_REC)) (void)rec_to(sq_rec, 0, &cl_dir[k]);
    }
    clip_pick(0);
    for (p = 0; p < 2u; p++) { sq_view[p].n = 0; sq_view[p].root = MV_SEQ_NOROOT; sq_view[p].len = ED.sq[MV_SQ_LEN(p)]; }
    if (st_read("tracks.jt", SK_TRK, sq_rec, JT_TRK_BYTES)) {
        for (i = 0; i < MV_SQ_N; i++) ED.sq[i] = (uint8_t)clampi(sq_rec[i], MV_SQ_MIN[i], MV_SQ_MAX[i]);
        for (p = 0; p < 2u; p++)
            if (rec_to(sq_rec + 8u + p * JT_CLIP_REC, &sq_ld[p], 0)) {
                sq_view[p] = sq_ld[p];
                ED.sq[MV_SQ_LEN(p)] = sq_ld[p].len;
                sq_ld_req[p]++;
                (void)q_push(Q_LOAD, p, 0);     /* (the first block takes them) */
            }
    }
    trk_to(w_trk);                              /* (what is there now is what is stored) */
    cp(s_sq, ED.sq, MV_SQ_N); s_trk_ver = sq_snap_ver; s_trk_t = vibe_frames();
}

/* ---- the keys ------------------------------------------------------------------------------------------------------ */
static void sq_arm_toggle(uint32_t mask)        /* all of mask armed: off; else on */
{
    uint32_t a = sq_pub_arm & 3u;
    a = (a & mask) == mask ? a & ~mask : a | mask;
    (void)q_push(Q_ARM, 0, a);
    say(a & mask ? "REC" : "REC OFF");
}
static void sq_rec_press(void)
{
    uint32_t p;
    sq_rec_down = 1; sq_rec_did = 0; sq_rec_t = vibe_frames();
    if (!sq_ask) return;
    for (p = 0; p < 2u; p++) if (sq_ask_mask >> p & 1u) (void)q_push(Q_CLR, p, MV_SQC_ALL + sq_ask_mode);   /* yes */
    sq_ask = 0; sq_rec_did = 1;
    say("CLEARED");
}
static void sq_rec_release(void)
{
    if (sq_rec_down && !sq_rec_did) sq_arm_toggle(sq_mask());
    sq_rec_down = 0;
}
static void sq_play(void)
{
    if (sq_pub_lead) { say("THE HOST LEADS"); return; }
    (void)q_push(Q_RUN, 0, sq_pub_run ? 0u : 1u);
}
static void sq_stop(void) { if (sq_pub_lead) say("THE HOST LEADS"); else (void)q_push(Q_RUN, 0, 0); }
static void sq_poll(void)                       /* (events, draw) REC held 1 s: the question; 3 s unanswered: gone */
{
    uint64_t t = vibe_frames();
    if (sq_rec_down && !sq_rec_did && t - sq_rec_t >= (uint64_t)st_fs) {
        sq_rec_did = 1; sq_ask = 1; sq_ask_t = t; sq_ask_mode = 0; sq_ask_mask = sq_mask();
    }
    if (sq_ask && t - sq_ask_t >= 3u * (uint64_t)st_fs) sq_ask = 0;
}
static void sq_pad(uint32_t k, int shift)       /* a pad on SEQ */
{
    uint32_t p = sq_trk();
    char name[JT_CLIP_NAME];
    uint32_t i;
    if (k < JT_CLIP_N) {
        if (shift) {
            if (cl_dir[k].used) for (i = 0; i < JT_CLIP_NAME; i++) name[i] = cl_dir[k].name[i];
            else clip_default(k, name);
            clip_save(k, p, name);
        } else clip_load(k, p);
        clip_pick(k);
        return;
    }
    if (k == 12u || k == 13u) { uint32_t s = MV_SQ_MUTE(k == 12u ? MV_PART_LO : MV_PART_UP); ED.sq[s] = (uint8_t)!ED.sq[s]; return; }
    if (k == 14u || k == 15u) sq_arm_toggle(1u << (k == 14u ? MV_PART_LO : MV_PART_UP));
}
static void sq_pads_light(void)
{
    uint32_t k, a = sq_pub_arm;
    for (k = 0; k < JT_CLIP_N; k++)
        vibe_led_pad((int)k, cl_dir[k].used ? 0xFF5A8Au : 0x202630u, k == cl_sel ? 1.0f : cl_dir[k].used ? 0.45f : 0.25f);
    vibe_led_pad(12, 0xFFC830u, ED.sq[MV_SQ_MUTE(MV_PART_LO)] ? 1.0f : 0.15f);
    vibe_led_pad(13, 0xFFC830u, ED.sq[MV_SQ_MUTE(MV_PART_UP)] ? 1.0f : 0.15f);
    vibe_led_pad(14, 0xFF2030u, a >> MV_PART_LO & 1u ? 1.0f : 0.15f);
    vibe_led_pad(15, 0xFF2030u, a >> MV_PART_UP & 1u ? 1.0f : 0.15f);
}

/* ---- the SEQ values (jt_pages.h asks) ------------------------------------------------------------------------------ */
static int sq_label(uint32_t p, const char **l)
{
    static const char *const L[P_SQ_N] = {"LENGTH", "QUANT", "MUTE L", "MUTE U", "TRANSP", "ROOT", "CLIP", "MODE", "CURSOR", "CHAR"};
    if (p < P_SQ || p >= P_SQ + P_SQ_N) return 0;
    *l = L[p - P_SQ];
    return 1;
}
static int sq_value(uint32_t p, char *s, float *fill)
{
    static const char *const QN_[MV_SQQ_N] = {"OFF", "1/16", "1/8", "1/16T", "1/8T"};
    static const char *const XN[MV_SQX_N] = {"OFF", "LOWER", "UPPER", "BOTH"};
    static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    uint32_t k = p - P_SQ, t = sq_trk(), v, i = 0;
    const char *c;
    if (p < P_SQ || p >= P_SQ + P_SQ_N) return 0;
    switch (k) {
    case 0: v = ED.sq[MV_SQ_LEN(t)]; s_num(s, (int32_t)v, 0); for (c = v == 1u ? " BAR" : " BARS"; s[i]; i++) {} while (*c && i < 7u) s[i++] = *c++; s[i] = 0;
        *fill = (float)(v - 1u) / (float)(MV_SEQ_MAXBAR - 1u); break;
    case 1: v = ED.sq[MV_SQ_QUANT]; s_copy(s, QN_[v % MV_SQQ_N]); *fill = (float)v / (float)(MV_SQQ_N - 1u); break;
    case 2: case 3: v = ED.sq[MV_SQ_MUTE(k == 2u ? MV_PART_LO : MV_PART_UP)]; s_copy(s, v ? "ON" : "OFF"); *fill = (float)v; break;
    case 4: v = ED.sq[MV_SQ_XPOSE]; s_copy(s, XN[v % MV_SQX_N]); *fill = (float)v / (float)(MV_SQX_N - 1u); break;
    case 5:
        (void)sq_read();
        v = sq_view[t].root;
        if (v > 127u) { s_copy(s, "--"); break; }
        for (c = NN[v % 12u]; *c; c++) s[i++] = *c;
        if (v < 12u) { s[i++] = '-'; s[i++] = '1'; } else s[i++] = (char)('0' + v / 12u - 1u);
        s[i] = 0; *fill = (float)v / 127.0f; break;
    case 6: s[0] = 'C'; s[1] = (char)('0' + (cl_sel + 1u) / 10u); s[2] = (char)('0' + (cl_sel + 1u) % 10u);
        s[3] = cl_dir[cl_sel].used ? ' ' : 0; s[4] = '*'; s[5] = 0; *fill = (float)cl_sel / (float)(JT_CLIP_N - 1u); break;
    case 7: s_copy(s, cl_mode ? "SAVE" : "LOAD"); *fill = (float)cl_mode; break;
    case 8: s_num(s, (int32_t)cl_pos + 1, 0); *fill = (float)cl_pos / (float)(JT_CLIP_NAME - 1u); break;
    default: v = (uint8_t)cl_name[cl_pos % JT_CLIP_NAME]; if (v == ' ') s_copy(s, "SPC"); else { s[0] = (char)v; s[1] = 0; }
        *fill = (float)(v - 32u) / 93.0f; break;
    }
    return 1;
}
static int sq_turn(uint32_t p, int32_t step)
{
    uint32_t k = p - P_SQ, t = sq_trk();
    if (p < P_SQ || p >= P_SQ + P_SQ_N) return 0;
    switch (k) {
    case 0: ED.sq[MV_SQ_LEN(t)] = (uint8_t)clampi(ED.sq[MV_SQ_LEN(t)] + step, 1, (int32_t)MV_SEQ_MAXBAR); break;
    case 1: ED.sq[MV_SQ_QUANT] = (uint8_t)clampi(ED.sq[MV_SQ_QUANT] + step, 0, (int32_t)MV_SQQ_N - 1); break;
    case 2: case 3: { uint32_t s = MV_SQ_MUTE(k == 2u ? MV_PART_LO : MV_PART_UP); ED.sq[s] = (uint8_t)clampi(ED.sq[s] + step, 0, 1); break; }
    case 4: ED.sq[MV_SQ_XPOSE] = (uint8_t)clampi(ED.sq[MV_SQ_XPOSE] + step, 0, (int32_t)MV_SQX_N - 1); break;
    case 5: {                                   /* ROOT: the panel may set it (none yet: from C4) */
        int32_t r;
        (void)sq_read();
        r = sq_view[t].root > 127u ? 60 : clampi((int32_t)sq_view[t].root + step, 0, 127);
        sq_view[t].root = (uint8_t)r;           /* (shown at once; the audio's copy follows) */
        (void)q_push(Q_ROOT, t, (uint32_t)r);
        break;
    }
    case 6: clip_pick((uint32_t)clampi((int32_t)cl_sel + step, 0, (int32_t)JT_CLIP_N - 1)); break;
    case 7: cl_mode = (uint32_t)clampi((int32_t)cl_mode + step, 0, 1); break;
    case 8: cl_pos = (uint32_t)clampi((int32_t)cl_pos + step, 0, (int32_t)JT_CLIP_NAME - 1); break;
    default: { char *c = &cl_name[cl_pos % JT_CLIP_NAME]; *c = (char)clampi((int32_t)(uint8_t)*c + step, 32, 125); break; }
    }
    return 1;
}
static int32_t sq_span(uint32_t p)
{
    uint32_t k = p - P_SQ;
    return k == 0u ? (int32_t)MV_SEQ_MAXBAR - 1 : k == 5u ? 127 : k == 9u ? 93 : 4;
}
static void sq_clip_do(void)                    /* F5 on the CLIP page: MODE */
{
    if (cl_mode) clip_save(cl_sel, sq_trk(), cl_name);
    else clip_load(cl_sel, sq_trk());
}
