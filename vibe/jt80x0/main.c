/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM – the JP-8080 sound of the FM-1 firmware, native (Kris 23 Nov 2026: P2, the skeleton, of
 * claude/jt80x0-vibe-port-proposal-2026-11-23.md). The engine and the factory bank are the FM-1's (jt_core.h); here the
 * SDK's four functions:
 *   keys, MIDI, the DAW's notes   play (VIBE_EV_NOTE)          pads 1-16   the bank's 16 programs
 *   B                             program - / +                A           bank 1-4 (16 programs each)
 *   HOME                          what the controls do
 * Two flows (VIBE_PARALLEL_AUDIO, a must): the engine is the audio flow's alone. The events hand it notes and program
 * changes through one queue (one writer, one reader: the data, VIBE_RELEASE, then the counter). The display reads only
 * what the events own, and one 32-bit value the audio writes (voices sounding). P3 brings the pages, P4 the DAW's
 * parameters, P5 saving. */
#include "vibe.h"
#include "vibe_gfx.h"
#include "jt_core.h"

VIBE_PARALLEL_AUDIO

/* ---- the queue: events -> audio ------------------------------------------------------------------------------ */
enum { Q_NOTE_ON = 1, Q_NOTE_OFF, Q_PROG };
#define QN 256u
static uint32_t q[QN];                          /* type << 24 | a << 8 | b */
static volatile uint32_t qw, qr;                /* qw: the events'; qr: the audio's */
static void q_push(uint32_t type, uint32_t a, uint32_t b)   /* (events) full: dropped */
{
    uint32_t w = qw;
    if (w - qr >= QN) return;
    q[w % QN] = type << 24 | (a & 0xFFFFu) << 8 | (b & 0xFFu);
    VIBE_RELEASE();                             /* the data before the counter */
    qw = w + 1u;
}

/* ---- the events' state ---------------------------------------------------------------------------------------- */
static uint32_t host_fs, rate_ok, prog, help;
static float enc_acc[VE_COUNT];
static volatile uint32_t voices;                /* the audio's: voices sounding (the display shows it) */
static const char *const HELP[] = {
    "KEYS / MIDI / DAW", "play (two parts: the program's SINGLE / DUAL / SPLIT)",
    "PADS 1-16", "the 16 programs of this bank",
    "B", "program - / +",
    "A", "bank 1-4",
    "HOME", "this overview (again or RETURN: back)",
};

static int enc_steps(int e, float v)            /* whole detents from fractional turns */
{
    int s;
    enc_acc[e] += v;
    s = (int)enc_acc[e];
    enc_acc[e] -= (float)s;
    return s;
}

static void pads_light(void)                    /* the bank's 16 programs; the one playing lit */
{
    uint32_t p;
    for (p = 0; p < VIBE_PADS; p++)
        vibe_led_pad((int)p, p == prog % 16u ? 0xFF8A1Fu : 0x3A2410u, p == prog % 16u ? 1.0f : 0.35f);
}

static void prog_select(uint32_t n)
{
    prog = n % MV_NPROG;
    q_push(Q_PROG, prog, 0);
    pads_light();
}

/* ---- the four functions ----------------------------------------------------------------------------------------- */
void vibe_on_init(float sample_rate)
{
    host_fs = (uint32_t)(sample_rate + 0.5f);
    rate_ok = (uint32_t)jt_init(host_fs, 0u);   /* (the audio has not started: the engine is ours here) */
    prog = 0;
    vibe_notes(VIBE_NOTES);                     /* keys, MIDI and the DAW's notes all come as VIBE_EV_NOTE */
    pads_light();
}

void vibe_on_event(const vibe_event *e)
{
    int s;
    switch (e->type) {
    case VIBE_EV_NOTE:
        if (e->down) {
            int v = (int)(e->value * 126.0f + 1.5f);   /* velocity 0..1 -> 1..127 */
            q_push(Q_NOTE_ON, e->id, (uint32_t)(v < 1 ? 1 : v > 127 ? 127 : v));
        } else q_push(Q_NOTE_OFF, e->id, 0);
        break;
    case VIBE_EV_PAD:
        if (e->down && e->id < VIBE_PADS) prog_select(prog / 16u * 16u + e->id);
        break;
    case VIBE_EV_ENCODER:
        s = enc_steps(e->id, e->value);
        if (!s) break;
        if (e->id == VE_B) prog_select((uint32_t)((int)prog + MV_NPROG + s));
        else if (e->id == VE_A) prog_select((prog + (uint32_t)(4 + (s > 0 ? 1 : -1)) * 16u) % MV_NPROG);
        break;
    case VIBE_EV_BUTTON:
        if (!e->down) break;
        if (e->id == VB_HOME) help = !help;
        else if (e->id == VB_RETURN) help = 0;
        break;
    default:
        break;
    }
}

void vibe_on_audio(const float *in, float *l, float *r, int frames)
{
    /* the engine renders 16 frames at a time: straight into the host's block, and a block that is no multiple of 16
     * ends through a 16-frame queue (what it holds plays first next time: at most 15 frames late) */
    static float ql[16], qr_[16];
    static uint32_t qpos = 16u;
    uint32_t i = 0, n = (uint32_t)frames, w;
    const vibe_transport_info *tp = vibe_transport();
    (void)in;
    if (!rate_ok) {                             /* no tables for this rate: silence (the display says why) */
        for (i = 0; i < n; i++) l[i] = r[i] = 0.0f;
        return;
    }
    w = qw;
    VIBE_ACQUIRE();                             /* the counter, then the data */
    for (; qr != w; qr++) {
        uint32_t c = q[qr % QN], a = c >> 8 & 0xFFFFu, b = c & 0xFFu;
        switch (c >> 24) {
        case Q_NOTE_ON:  mv_engine_note_on(&eng, a, b); break;
        case Q_NOTE_OFF: mv_engine_note_off(&eng, a); break;
        case Q_PROG:     jt_prog_load(a); break;
        default: break;
        }
    }
    jt_tempo(tp->has && tp->bpm > 0.0f ? tp->bpm : 120.0f);
    while (i < n && qpos < 16u) { l[i] = ql[qpos]; r[i] = qr_[qpos]; qpos++; i++; }
    while (n - i >= 16u) {
        uint32_t m = n - i >= JT_BLOCK ? JT_BLOCK : (n - i) & ~15u;
        jt_render(l + i, r + i, m);
        i += m;
    }
    if (i < n) {
        jt_render(ql, qr_, 16u);
        for (qpos = 0; i < n; i++, qpos++) { l[i] = ql[qpos]; r[i] = qr_[qpos]; }
    }
    voices = mv_engine_active(&eng);
}

void vibe_on_draw(uint16_t *fb)
{
    char num[12], name[17];
    uint32_t i, n = prog;
    (void)fb;
    if (help) { vg_controls("JT-80x0", HELP, (int)(sizeof HELP / sizeof HELP[0] / 2u)); return; }
    vg_clear(0x0E0F12);
    vg_rect(0, 0, 480, 40, 0xFF8A1F);
    vg_text("JT-80x0", 14, 9, VG_BIG, 0x0E0F12, VG_LEFT);
    vg_text("JP-8080 SOUND", 466, 13, VG_BOLD, 0x0E0F12, VG_RIGHT);
    if (!rate_ok) {
        vg_text(vg_itoa((int)host_fs, num), 240, 110, VG_HUGE, 0xE6E8EE, VG_CENTER);
        vg_text("Hz: this sample rate is not supported yet (44100 and 48000 are)", 240, 190, VG_MED, 0xB8BCC6, VG_CENTER);
        return;
    }
    for (i = 0; i < 16u; i++) name[i] = (char)(MV_FACTORY[n][i] >= 32 && MV_FACTORY[n][i] < 127 ? MV_FACTORY[n][i] : ' ');
    for (i = 16u; i > 0u && name[i - 1u] == ' '; i--) {}
    name[i] = 0;
    num[0] = (char)('A' + n / 16u); num[1] = '-';
    vg_itoa((int)(n % 16u + 1u), num + 2);
    vg_text(num, 240, 70, VG_HUGE, 0xFF8A1F, VG_CENTER);
    vg_text(name, 240, 150, VG_BIG, 0xE6E8EE, VG_CENTER);
    vg_text("BANK", 40, 210, VG_SMALL, 0x8A8F9C, VG_LEFT);
    for (i = 0; i < 4u; i++) vg_round(90 + (int)i * 34, 208, 26, 18, 4, i == n / 16u ? 0xFF8A1F : 0x2A2D35);
    vg_text("VOICES", 300, 210, VG_SMALL, 0x8A8F9C, VG_LEFT);
    vg_text(vg_itoa((int)voices, num), 440, 204, VG_BIG, 0xE6E8EE, VG_RIGHT);
    for (i = 0; i < 16u; i++) vg_round(48 + (int)i * 24, 250, 18, 18, 4, i == n % 16u ? 0xFF8A1F : 0x2A2D35);
    vg_text("A bank   B program   PADS the bank's 16   HOME controls", 240, 290, VG_SMALL, 0x8A8F9C, VG_CENTER);
}
