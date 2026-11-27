/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM: the edit copy and the queue between the flows (Kris 24 Nov 2026, P3: claude/jt80x0-vibe-p3-pages-
 * proposal-2026-11-24.md, 1a).
 *
 * The events (vibe_on_event, vibe_on_draw: the control flow) own ED: what the pages show and edit – both parts, the
 * performance bytes, the reverb and arp settings, voices, tempo, MIDI channel. Nothing there touches the engine. After
 * every event jt_commit() compares ED with SENT (what the engine has been told) and queues each byte that differs; the
 * audio flow applies the queue before its next block (jt_apply). So every byte has one writer per flow, an edit lands
 * on a block boundary (the same events always give the same sound: the tests rely on it), and a fast turn never reads
 * back a stale value. A program change (P5) sets ED from BANK – the 64 programs as saved, else the factory's – and the
 * commit sends what differs, all before the same block.
 * Notes, bend, wheel and pressure are events of their own in the same queue (one writer: the control flow).
 * P6 (Kris 26 Nov, the sequencer: claude/jt80x0-vibe-p6-seq-proposal-2026-11-26.md, 6a): the engine's automation moves
 * its bytes in the audio flow. The audio keeps a mirror of every patch byte it writes (MIR: a queued byte, a point, STOP
 * putting one back) with a count per byte; jt_mirror() (control) takes each byte whose count moved into SENT – and into
 * ED unless an edit of that byte is still on its way – so the pages, the '*' and the DAW follow, and nothing goes back.
 * A knob turned while the sequencer runs goes to it too (Q_KNOB: recorded into an armed track, else it takes the CC
 * over until the loop goes round), as the FM-1's mv_engine_knob; a program change does not (jt_quiet). */
#pragma once
#include "jt_core.h"

/* ---- the queue (one writer: the control flow; one reader: the audio) ------------------------------------------- */
enum { Q_NOTE = 1, Q_PROG, Q_BYTE, Q_PERF, Q_RV, Q_ARP, Q_NV, Q_TEMPO, Q_BEND, Q_WHEEL, Q_CTL, Q_PVELO,
       Q_SQ, Q_RUN, Q_ARM, Q_CLR, Q_KNOB, Q_ROOT, Q_LOAD, Q_FORGET };   /* (Q_SQ ..: the sequencer, jt_seq.h) */
#define QN 2048u                               /* (a program change: up to 512 bytes at once) */
static uint32_t q[QN];                          /* type << 24 | a << 8 | b */
static volatile uint32_t qw, qr;
static int q_push(uint32_t type, uint32_t a, uint32_t b)   /* 1 = queued, 0 = full (dropped: SENT keeps the old) */
{
    uint32_t w = qw;
    if (w - qr >= QN) return 0;
    q[w % QN] = type << 24 | (a & 0xFFFFu) << 8 | (b & 0xFFu);
    VIBE_RELEASE();                             /* the data before the counter */
    qw = w + 1u;
    return 1;
}

/* ---- the edit copy (the control flow's) ------------------------------------------------------------------------ */
typedef struct {
    mv_patch_t part[2];                         /* MV_PART_UP, MV_PART_LO */
    uint8_t perf[MV_PF_N], rv[MV_RV_N], arp[MV_ARP_N];
    uint8_t nv, tempo, midich;                  /* voices 1..MV_NV, own tempo 40..240, MIDI channel 0 = OMNI */
    uint8_t sq[MV_SQ_N];                        /* the sequencer's settings (P6): LENGTH x 2, QUANT, MUTE x 2, TRANSPOSE */
    uint8_t pvelo;                              /* PATCH VELO (the FM-1's GLOBAL, Kris 22 / 25 Nov): 1 = the patches' JP
                                                   VELOCITY offsets apply; 0, the default, = the bank as it was voiced */
} jt_ed_t;
static jt_ed_t ED, SENT;
/* the audio's patch bytes, mirrored (6a: below) */
static uint8_t mir_v[2][MV_PATCH_BYTES];        /* (audio writes) each byte as the engine has it .. */
static volatile uint32_t mir_n[2][MV_PATCH_BYTES];   /* .. and +1 each time it wrote it (after the value) */
static uint32_t mir_seen[2][MV_PATCH_BYTES];    /* (control) the counts taken */

/* the 64 programs as the control flow knows them (P5): a program is UPPER 248 + LOWER 248 + performance 16 bytes (the
 * FM-1's), the factory's until one is saved (jt_store.h) */
#define JT_PROG_BYTES (2u * MV_PATCH_BYTES + MV_PF_N)
static uint8_t BANK[MV_NPROG][JT_PROG_BYTES];
static void jt_bank_factory(uint32_t n)
{
    uint32_t i;
    for (i = 0; i < MV_PATCH_BYTES; i++) { BANK[n][i] = MV_FACTORY[n][i]; BANK[n][MV_PATCH_BYTES + i] = MV_FACTORY_LO[n][i]; }
    for (i = 0; i < MV_PF_N; i++) BANK[n][2u * MV_PATCH_BYTES + i] = MV_FACTORY_PERF[n][i];
}
static void jt_prog_to(uint8_t *d)              /* ED's program into d (JT_PROG_BYTES) */
{
    uint32_t i;
    for (i = 0; i < MV_PATCH_BYTES; i++) { d[i] = ED.part[0].b[i]; d[MV_PATCH_BYTES + i] = ED.part[1].b[i]; }
    for (i = 0; i < MV_PF_N; i++) d[2u * MV_PATCH_BYTES + i] = ED.perf[i];
}
static void jt_prog_from(const uint8_t *s)      /* ED's program from s */
{
    uint32_t i;
    for (i = 0; i < MV_PATCH_BYTES; i++) { ED.part[0].b[i] = s[i]; ED.part[1].b[i] = s[MV_PATCH_BYTES + i]; }
    for (i = 0; i < MV_PF_N; i++) ED.perf[i] = s[2u * MV_PATCH_BYTES + i];
}
static int jt_edited(uint32_t n)                /* ED differs from program n as stored (the '*') */
{
    uint8_t t[JT_PROG_BYTES];
    uint32_t i;
    jt_prog_to(t);
    for (i = 0; i < JT_PROG_BYTES; i++) if (t[i] != BANK[n % MV_NPROG][i]) return 1;
    return 0;
}
static void jt_ed_prog(uint32_t n) { jt_prog_from(BANK[n % MV_NPROG]); }   /* (control) jt_commit tells the engine */

static void jt_ed_init(uint32_t prog)           /* (vibe_on_init, after jt_init: the engine's own state to start from) */
{
    uint32_t i;
    for (i = 0; i < MV_NPROG; i++) jt_bank_factory(i);
    for (i = 0; i < MV_RV_N; i++) ED.rv[i] = eng.rvb[i];
    for (i = 0; i < MV_ARP_N; i++) ED.arp[i] = eng.arpb[i];
    ED.nv = (uint8_t)eng.nv; ED.tempo = 120; ED.midich = 0; ED.pvelo = eng.pvelo;
    for (i = 0; i < MV_SQ_N; i++) ED.sq[i] = eng.seqb[i];
    for (i = 0; i < MV_PATCH_BYTES; i++) { ED.part[0].b[i] = eng.patch.b[i]; ED.part[1].b[i] = eng.lower.b[i]; }
    for (i = 0; i < MV_PF_N; i++) ED.perf[i] = eng.perf[i];
    SENT = ED;
    for (i = 0; i < MV_PATCH_BYTES; i++) {
        mir_v[0][i] = eng.patch.b[i]; mir_v[1][i] = eng.lower.b[i];
        mir_n[0][i] = mir_n[1][i] = 0; mir_seen[0][i] = mir_seen[1][i] = 0;
    }
    (void)prog;
}

/* ---- the audio's patch bytes, mirrored (6a) ---------------------------------------------------------------------- */
static void mir_set(uint32_t p, uint32_t i, uint32_t v)   /* (audio) the engine's byte i of part p is now v */
{
    mir_v[p][i] = (uint8_t)v;
    VIBE_RELEASE();
    mir_n[p][i]++;
}
static void mir_part(uint32_t p)                /* (audio) after the engine moved part p's bytes itself */
{
    const uint8_t *b = p ? eng.lower.b : eng.patch.b;
    uint32_t i;
    for (i = 0; i < MV_PATCH_BYTES; i++) if (mir_v[p][i] != b[i]) mir_set(p, i, b[i]);
}
static void jt_mirror(void)                     /* (control) what the audio moved: into SENT, and ED where no edit waits */
{
    uint32_t p, i, n;
    for (p = 0; p < 2u; p++)
        for (i = 0; i < MV_PATCH_BYTES; i++)
            if ((n = mir_n[p][i]) != mir_seen[p][i]) {
                uint8_t v;
                VIBE_ACQUIRE();
                v = mir_v[p][i];
                mir_seen[p][i] = n;
                if (ED.part[p].b[i] == SENT.part[p].b[i]) ED.part[p].b[i] = v;
                SENT.part[p].b[i] = v;          /* (an edit on its way: ED differs, and is sent again) */
            }
}

/* (control) everything in ED that the engine has not been told yet: queued, byte by byte */
static int jt_quiet;                            /* 1: patch changes are no knob moves (a program change, a restore) */
static void jt_commit(void)
{
    uint32_t p, i, d = 0;
    jt_mirror();
    for (p = 0; p < 2u; p++)
        for (i = 0; i < MV_PATCH_BYTES; i++) d |= ED.part[p].b[i] != SENT.part[p].b[i];
    if (d && !jt_quiet)                         /* the sequencer: each moved parameter with a JP CC, as its CC value */
        for (p = 0; p < 2u; p++)
            for (i = 0; i < MV_NPARAMS; i++) {
                int32_t v;
                if (MV_PARAMS[i].cc == MV_NONE || (v = mv_param_get(&ED.part[p], i)) == mv_param_get(&SENT.part[p], i)) continue;
                (void)q_push(Q_KNOB, p << 8 | MV_PARAMS[i].cc, mv_cc_value(i, v));
            }
    for (p = 0; p < 2u; p++)
        for (i = 0; i < MV_PATCH_BYTES; i++)
            if (ED.part[p].b[i] != SENT.part[p].b[i] && q_push(Q_BYTE, p << 8 | i, ED.part[p].b[i])) SENT.part[p].b[i] = ED.part[p].b[i];
    for (i = 0; i < MV_PF_N; i++) if (ED.perf[i] != SENT.perf[i] && q_push(Q_PERF, i, ED.perf[i])) SENT.perf[i] = ED.perf[i];
    for (i = 0; i < MV_RV_N; i++) if (ED.rv[i] != SENT.rv[i] && q_push(Q_RV, i, ED.rv[i])) SENT.rv[i] = ED.rv[i];
    for (i = 0; i < MV_ARP_N; i++) if (ED.arp[i] != SENT.arp[i] && q_push(Q_ARP, i, ED.arp[i])) SENT.arp[i] = ED.arp[i];
    if (ED.nv != SENT.nv && q_push(Q_NV, 0, ED.nv)) SENT.nv = ED.nv;
    if (ED.tempo != SENT.tempo && q_push(Q_TEMPO, 0, ED.tempo)) SENT.tempo = ED.tempo;
    if (ED.pvelo != SENT.pvelo && q_push(Q_PVELO, 0, ED.pvelo)) SENT.pvelo = ED.pvelo;
    for (i = 0; i < MV_SQ_N; i++) if (ED.sq[i] != SENT.sq[i] && q_push(Q_SQ, i, ED.sq[i])) SENT.sq[i] = ED.sq[i];
    SENT.midich = ED.midich;                    /* (the events' own: the channel gate is theirs) */
}

/* ---- the audio side ------------------------------------------------------------------------------------------- */
static uint32_t jt_own_bpm = 120u;              /* the audio's copy of ED.tempo */
static void jt_sq_apply(uint32_t type, uint32_t a, uint32_t b);   /* (jt_seq.h: Q_SQ ..) */
static void jt_apply(uint32_t c)
{
    uint32_t a = c >> 8 & 0xFFFFu, b = c & 0xFFu;
    switch (c >> 24) {
    case Q_NOTE:  mv_engine_note_to(&eng, a & 0xFFu, b, a >> 8); break;   /* (vel 0: off; to: MV_TO_* mask) */
    case Q_PROG:  jt_prog_load(a); mir_part(0); mir_part(1); break;
    case Q_BYTE:
        if ((a & 0xFFu) < MV_PATCH_BYTES) {
            (a >> 8 ? &eng.lower : &eng.patch)->b[a & 0xFFu] = (uint8_t)b;
            mir_set(a >> 8 & 1u, a & 0xFFu, b);
        }
        break;
    case Q_PERF:  if (a < MV_PF_N) eng.perf[a] = (uint8_t)b; break;
    case Q_RV:    if (a < MV_RV_N) eng.rvb[a] = (uint8_t)b; break;
    case Q_ARP:   if (a < MV_ARP_N) eng.arpb[a] = (uint8_t)b; break;
    case Q_NV:    eng.nv = b; break;
    case Q_TEMPO: jt_own_bpm = b; break;
    case Q_BEND:  eng.bend14 = (int32_t)a - 8192; break;
    case Q_WHEEL: eng.wheel = b; break;
    case Q_CTL:   eng.ctl = (uint8_t)b; break;
    case Q_PVELO: eng.pvelo = (uint8_t)(b & 1u); break;
    default: jt_sq_apply(c >> 24, a, b); break;
    }
}
static void jt_drain(void)                      /* (audio) everything queued so far, before the block */
{
    uint32_t w = qw;
    VIBE_ACQUIRE();                             /* the counter, then the data */
    for (; qr != w; qr++) jt_apply(q[qr % QN]);
}
