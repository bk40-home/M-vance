/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM: saving (Kris 25 Nov 2026, P5: claude/jt80x0-vibe-p5-save-proposal-2026-11-25.md, 1a 2a 3a 4a 5a).
 *   bank/p01.jt .. p64.jt   a program (UPPER, LOWER, performance: the FM-1's 512 bytes), written by SAVE; none = factory
 *   settings.jt             reverb, arp, tempo, voices, MIDI channel, the program, PART, each group's last page,
 *                           PATCH VELO (byte 33: a spare before it, so an older file reads OFF)
 *   edit.jt                 the program as it plays, unsaved edits and all: a restart gives back the sound left
 *   tracks.jt, clips/       the sequencer's (P6: jt_seq.h)
 * Each starts "JT80", a version, a kind and the length; one that does not match is ignored (the factory's plays).
 * settings.jt and edit.jt are written 1.5 s after the last change (a knob being turned writes nothing until it rests),
 * and at once on VIBE_EV_SAVE (a DAW saving its project). Only from the control flow – events and the draw – never from
 * the audio. On the VIBE: the microSD; in the plugin: inside the DAW project, per instance; in the browser: IndexedDB. */
#pragma once
#include "jt_pages.h"

#define JT_STORE_VER 1u
enum { SK_PROG = 1, SK_SET, SK_EDIT };
#define JT_SET_BYTES 40u
#define JT_ST_MAX 2200u                         /* the biggest file's bytes (tracks.jt: jt_seq.h) */
_Static_assert(JT_PROG_BYTES <= JT_ST_MAX, "jt_store: a program fits the buffer");
static uint8_t st_buf[8u + JT_ST_MAX];

static void prog_file(uint32_t n, char *name)   /* "bank/p01.jt" */
{
    const char *a = "bank/p";
    uint32_t i = 0;
    while (*a) name[i++] = *a++;
    name[i++] = (char)('0' + (n + 1u) / 10u); name[i++] = (char)('0' + (n + 1u) % 10u);
    name[i++] = '.'; name[i++] = 'j'; name[i++] = 't'; name[i] = 0;
}

static int st_write(const char *name, uint32_t kind, const uint8_t *p, uint32_t n)
{
    uint32_t i;
    st_buf[0] = 'J'; st_buf[1] = 'T'; st_buf[2] = '8'; st_buf[3] = '0';
    st_buf[4] = (uint8_t)JT_STORE_VER; st_buf[5] = (uint8_t)kind; st_buf[6] = (uint8_t)(n & 255u); st_buf[7] = (uint8_t)(n >> 8);
    for (i = 0; i < n; i++) st_buf[8u + i] = p[i];
    return vibe_store_write(name, st_buf, (int)(8u + n));
}
static int st_read(const char *name, uint32_t kind, uint8_t *p, uint32_t n)   /* 1 = read and valid */
{
    uint32_t i;
    if (vibe_store_read(name, st_buf, (int)(8u + n)) != (int)(8u + n)) return 0;
    if (st_buf[0] != 'J' || st_buf[1] != 'T' || st_buf[2] != '8' || st_buf[3] != '0' || st_buf[4] != JT_STORE_VER ||
        st_buf[5] != kind || (st_buf[6] | (uint32_t)st_buf[7] << 8) != n) return 0;
    for (i = 0; i < n; i++) p[i] = st_buf[8u + i];
    return 1;
}

/* ---- the settings as bytes --------------------------------------------------------------------------------------- */
static uint32_t prog;                           /* (the program playing: main.c and jt_daw.h read it) */
static void set_to(uint8_t *d)
{
    uint32_t i;
    for (i = 0; i < JT_SET_BYTES; i++) d[i] = 0;
    for (i = 0; i < MV_RV_N; i++) d[i] = ED.rv[i];
    for (i = 0; i < MV_ARP_N; i++) d[7u + i] = ED.arp[i];
    d[16] = ED.tempo; d[17] = ED.nv; d[18] = ED.midich; d[19] = (uint8_t)prog; d[20] = (uint8_t)panel;
    for (i = 0; i < G_N && i < 12u; i++) d[21u + i] = (uint8_t)g_last[i];
    d[33] = ED.pvelo;
}
static void set_from(const uint8_t *d)          /* each held in its range (a file from elsewhere cannot upset it) */
{
    uint32_t i;
    for (i = 0; i < MV_RV_N; i++) ED.rv[i] = (uint8_t)clampi(d[i], 0, i == MV_RV_FREEZE ? 1 : 127);
    for (i = 0; i < MV_ARP_N; i++) ED.arp[i] = (uint8_t)clampi(d[7u + i], MV_ARP_MIN[i], MV_ARP_MAX[i]);
    ED.tempo = (uint8_t)clampi(d[16], 40, 240); ED.nv = (uint8_t)clampi(d[17], 1, (int32_t)MV_NV);
    ED.midich = (uint8_t)clampi(d[18], 0, 16); prog = d[19] % MV_NPROG; panel = d[20] % 3u;
    for (i = 0; i < G_N && i < 12u; i++) if (d[21u + i] < NPAGES && PAGES[d[21u + i]].group == i) g_last[i] = d[21u + i];
    ED.pvelo = (uint8_t)(d[33] & 1u);
}

/* ---- written once things rest -------------------------------------------------------------------------------------- */
static uint8_t w_set[JT_SET_BYTES], w_edit[JT_PROG_BYTES];      /* as last written */
static uint8_t s_set[JT_SET_BYTES], s_edit[JT_PROG_BYTES];      /* as last seen */
static uint64_t s_t;                            /* when what is seen last changed (frames) */
static uint32_t st_fs = 48000u, st_writes;      /* (st_writes: files written, for the tests) */

static int differ(const uint8_t *a, const uint8_t *b, uint32_t n) { uint32_t i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 1; return 0; }
static void cp(uint8_t *a, const uint8_t *b, uint32_t n) { uint32_t i; for (i = 0; i < n; i++) a[i] = b[i]; }

static void sq_store_poll(int now);            /* (the sequencer's tracks.jt: jt_seq.h) */
static void store_poll(int now)                 /* (events, draw) now = 1: VIBE_EV_SAVE – everything at once */
{
    uint8_t cs[JT_SET_BYTES], ce[JT_PROG_BYTES];
    uint64_t t = vibe_frames();
    sq_store_poll(now);
    set_to(cs); jt_prog_to(ce);
    if (differ(cs, s_set, JT_SET_BYTES) || differ(ce, s_edit, JT_PROG_BYTES)) {
        cp(s_set, cs, JT_SET_BYTES); cp(s_edit, ce, JT_PROG_BYTES); s_t = t;
        if (!now) return;
    }
    if (!now && t - s_t < (uint64_t)st_fs * 3u / 2u) return;
    if (differ(cs, w_set, JT_SET_BYTES) && !st_write("settings.jt", SK_SET, cs, JT_SET_BYTES)) { cp(w_set, cs, JT_SET_BYTES); st_writes++; }
    if (differ(ce, w_edit, JT_PROG_BYTES) && !st_write("edit.jt", SK_EDIT, ce, JT_PROG_BYTES)) { cp(w_edit, ce, JT_PROG_BYTES); st_writes++; }
}

/* ---- SAVE (the page: jt_pages.h) ----------------------------------------------------------------------------------- */
static int store_prog(uint32_t n)               /* BANK[n] to its file */
{
    char name[16];
    prog_file(n, name);
    st_writes++;
    return st_write(name, SK_PROG, BANK[n], JT_PROG_BYTES);
}
static void store_factory(uint32_t n)           /* back to the factory's: its file gone */
{
    char name[16];
    prog_file(n, name);
    jt_bank_factory(n);
    (void)vibe_store_delete(name);
}

/* ---- the start: the bank, the settings, the sound left ------------------------------------------------------------ */
static void store_boot(uint32_t fs)             /* (vibe_on_init, after jt_ed_init) */
{
    uint8_t d[JT_PROG_BYTES];
    char name[16];
    uint32_t n;
    st_fs = fs ? fs : 48000u;
    for (n = 0; n < MV_NPROG; n++) {
        prog_file(n, name);
        if (st_read(name, SK_PROG, d, JT_PROG_BYTES)) cp(BANK[n], d, JT_PROG_BYTES);
    }
    if (st_read("settings.jt", SK_SET, d, JT_SET_BYTES)) set_from(d);
    jt_ed_prog(prog);                           /* the program; then the sound left in it, if there is one */
    if (st_read("edit.jt", SK_EDIT, d, JT_PROG_BYTES)) jt_prog_from(d);
    set_to(w_set); jt_prog_to(w_edit);          /* (what is there now is what is stored: nothing to write) */
    cp(s_set, w_set, JT_SET_BYTES); cp(s_edit, w_edit, JT_PROG_BYTES);
    s_t = vibe_frames();
}
