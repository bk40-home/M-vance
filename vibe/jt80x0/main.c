/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM – the JP-8080 sound of the FM-1 firmware, native (claude/jt80x0-vibe-port-proposal-2026-11-23.md).
 * The engine and the factory bank are the FM-1's (jt_core.h). P2: the four functions; P2b: 2x / 4x rates (jt_up.h);
 * P3 (Kris 24 Nov 2026, claude/jt80x0-vibe-p3-pages-proposal-2026-11-24.md): the pages and the controls –
 *   K1-K6   SOUND · ENV · LFO · ARP · FX · SEQ (again: the next page; ARP again: the arp on / off)   ◀ ▶  the pages
 *   1-4     the page's four values (SHIFT: x 8)          A  bank        B  program        PADS  the bank's 16
 *   F1 PART (UPPER / LOWER / BOTH)  F2 PERFORM  F3 / F4 PRG - / +  F5 SAVE (P5)  F6 GLOBAL     RETURN  PLAY
 *   HOME    what the controls do here       DATA + ENTER  every control on the display (FOCUS)
 *   keys, MIDI, the DAW: notes; bend, wheel, pressure (CONTROL), the JP MODE2 CCs, program change; sustain (CC64,
 *   PEDAL)
 * Two flows (VIBE_PARALLEL_AUDIO): the engine is the audio's; the events edit ED and queue what changed (jt_edit.h). */
#include "vibe.h"
#include "vibe_gfx.h"
#include "jt_core.h"
#include "jt_up.h"
#include "jt_edit.h"
#include "jt_pages.h"
#include "jt_midi.h"
#include "jt_draw.h"

VIBE_PARALLEL_AUDIO

/* ---- the events' state ---------------------------------------------------------------------------------------- */
static uint32_t host_fs, up_k, prog, help, shift;   /* up_k: host rate / engine rate (1, 2, 4); 0 = a rate we lack */
static float enc_acc[VE_COUNT];
static volatile uint32_t voices;                /* the audio's: voices sounding (the display shows it) */
static const char *toast;
static uint64_t toast_end;

static int enc_steps(int e, float v)            /* whole detents from fractional turns */
{
    int s;
    enc_acc[e] += v;
    s = (int)enc_acc[e];
    enc_acc[e] -= (float)s;
    return s;
}

static void say(const char *s) { toast = s; toast_end = vibe_frames() + (uint64_t)host_fs * 3u / 2u; }

static void pads_light(void)                    /* the bank's 16 programs; the one playing lit */
{
    uint32_t p;
    for (p = 0; p < VIBE_PADS; p++)
        vibe_led_pad((int)p, p == prog % 16u ? ACCENT : 0x3A2410u, p == prog % 16u ? 1.0f : 0.35f);
}

static void prog_select(uint32_t n)
{
    prog = n % MV_NPROG;
    jt_ed_prog(prog);                           /* (edits to the one before are dropped, as on the JP) */
    if (ED.perf[MV_PF_MODE] == MV_KEY_SINGLE) panel = PANEL_UPPER;
    pads_light();
}

/* ---- the four functions ----------------------------------------------------------------------------------------- */
void vibe_on_init(float sample_rate)
{
    uint32_t g;
    host_fs = (uint32_t)(sample_rate + 0.5f);
    for (up_k = 1u; up_k <= 4u; up_k *= 2u)     /* the engine at the host's rate, else at a half, else a quarter */
        if (host_fs % up_k == 0u && jt_init(host_fs / up_k, 0u)) break;   /* (the audio has not started: ours here) */
    if (up_k > 4u) up_k = 0;
    jt_ed_init(0u);
    prog = 0;
    for (g = 0; g < G_N; g++) g_last[g] = group_first(g);
    page_go(group_first(G_PLAY));
    vibe_notes(VIBE_NOTES);                     /* keys, MIDI and the DAW's notes all come as VIBE_EV_NOTE */
    vf_mode(VF_ALWAYS);                         /* (6a: DATA has no job of its own here – it walks the controls) */
    pads_light();
}

static void k_key(uint32_t g)                   /* K1-K6: the group; on it again: the next page (ARP: on / off) */
{
    if (PAGES[pg].group != g) { group_go(g); return; }
    if (g == G_ARP) { ED.arp[MV_ARP_ON] = (uint8_t)!ED.arp[MV_ARP_ON]; say(ED.arp[MV_ARP_ON] ? "ARP ON" : "ARP OFF"); return; }
    page_step(1);
}

static void button(uint32_t id)
{
    if (id >= VB_K1 && id <= VB_K6) { k_key(id - VB_K1); return; }
    switch (id) {
    case VB_LEFT: page_step(-1); break;
    case VB_RIGHT: page_step(1); break;
    case VB_F1:                                 /* PART: what the pages edit (a SINGLE has one) */
        if (ED.perf[MV_PF_MODE] == MV_KEY_SINGLE) say("SINGLE: one part");
        else panel = (panel + 1u) % 3u;
        break;
    case VB_F2: if (PAGES[pg].group == G_PERF) page_step(1); else group_go(G_PERF); break;
    case VB_F3: prog_select(prog + MV_NPROG - 1u); break;
    case VB_F4: prog_select(prog + 1u); break;
    case VB_F5: say("SAVE: not yet"); break;
    case VB_F6: group_go(G_GLOBAL); break;
    case VB_RETURN: if (help) help = 0; else page_go(group_first(G_PLAY)); break;
    case VB_HOME: help = !help; break;
    default: break;
    }
}

void vibe_on_event(const vibe_event *e)
{
    int id = -1, r = vf_event(e, &id), s;
    if (r == VF_PRESS && id >= 100 && id < 100 + (int)NPAGES) page_go((uint32_t)(id - 100));
    if (r != VF_NO) { jt_commit(); return; }
    switch (e->type) {
    case VIBE_EV_NOTE: note_event(e); break;
    case VIBE_EV_MIDI: midi_event(e); break;
    case VIBE_EV_PEDAL: if (e->id == VIBE_PEDAL_SWITCH) sustain(e->down); break;
    case VIBE_EV_PAD: if (e->down && e->id < VIBE_PADS) prog_select(prog / 16u * 16u + e->id); break;
    case VIBE_EV_ENCODER:
        s = enc_steps(e->id, e->value);
        if (!s) break;
        if (e->id <= VE_4) {
            uint32_t p = PAGES[pg].p[e->id - VE_1];
            p_turn(p, shift && p_span(p) >= 64 ? s * 8 : s);
        }
        else if (e->id == VE_B) prog_select((uint32_t)((int)prog + MV_NPROG * 4 + s));
        else if (e->id == VE_A) prog_select((prog + (uint32_t)(4 + (s > 0 ? 1 : -1)) * 16u) % MV_NPROG);
        break;
    case VIBE_EV_BUTTON:
        if (e->id == VB_SHIFT) { shift = e->down; break; }
        if (e->down) button(e->id);
        break;
    default: break;
    }
    jt_commit();                                /* whatever ED holds now: to the engine */
}

/* m engine frames (a multiple of 16, at most JT_BLOCK) -> m x up_k host frames */
static jt_hb up1, up2;
static float up_l[2u * JT_BLOCK], up_r[2u * JT_BLOCK], en_l[JT_BLOCK], en_r[JT_BLOCK];
static void piece(float *l, float *r, uint32_t m)
{
    if (up_k == 1u) { jt_render(l, r, m); return; }
    jt_render(en_l, en_r, m);
    if (up_k == 2u) { jt_hb_run(&up1, JT_UP1, JT_UP1_P, en_l, en_r, l, r, m); return; }
    jt_hb_run(&up1, JT_UP1, JT_UP1_P, en_l, en_r, up_l, up_r, m);
    jt_hb_run(&up2, JT_UP2, JT_UP2_P, up_l, up_r, l, r, 2u * m);
}

void vibe_on_audio(const float *in, float *l, float *r, int frames)
{
    /* the engine renders 16 frames at a time: straight into the host's block, and a block that is no multiple of
     * 16 x up_k ends through a queue of one such piece (what it holds plays first next time: at most 15 engine
     * frames late) */
    static float ql[16u * 4u], qr_[16u * 4u];
    static uint32_t qpos, qlen;
    uint32_t i = 0, n = (uint32_t)frames, piece_h = 16u * up_k;
    const vibe_transport_info *tp = vibe_transport();
    (void)in;
    if (!up_k) {                                /* no tables for this rate: silence (the display says why) */
        for (i = 0; i < n; i++) l[i] = r[i] = 0.0f;
        return;
    }
    jt_drain();                                 /* notes, edits, program changes: before this block */
    jt_tempo(tp->has && tp->bpm > 0.0f ? tp->bpm : (float)jt_own_bpm);
    while (i < n && qpos < qlen) { l[i] = ql[qpos]; r[i] = qr_[qpos]; qpos++; i++; }
    while (n - i >= piece_h) {
        uint32_t m = (n - i) / up_k;
        m = m >= JT_BLOCK ? JT_BLOCK : m & ~15u;
        piece(l + i, r + i, m);
        i += m * up_k;
    }
    if (i < n) {
        piece(ql, qr_, 16u);
        qlen = piece_h;
        for (qpos = 0; i < n; i++, qpos++) { l[i] = ql[qpos]; r[i] = qr_[qpos]; }
    }
    voices = mv_engine_active(&eng);
}

/* ---- the display ------------------------------------------------------------------------------------------------ */
static void help_view(void)                     /* HOME: this page's controls, the encoders by name */
{
    static char enc[4][24];
    static const char *rows[2 * 14];
    static const char *const KEYS[] = {"K1-K6", "groups; again: next", "K4 on ARP", "arp on / off",
        "LEFT / RIGHT", "the group's pages", "A / B", "bank / program", "PADS 1-16", "the bank's programs",
        "F1", "PART U / L / BOTH", "F2 / F6", "PERFORM / GLOBAL", "F3 / F4", "program - / +",
        "SHIFT + 1-4", "8 steps a detent", "RETURN", "the PLAY page", "DATA + ENTER", "every control"};
    uint32_t i, n = 0, k;
    for (i = 0; i < 4u; i++) {
        uint32_t p = PAGES[pg].p[i];
        const char *lb = p == P_NONE ? "-" : PAGES[pg].group == G_PLAY ? (const char *const[]){"CUTOFF", "RESO", "DELAY", "REVERB"}[i] : p_label(p);
        enc[i][0] = 'E'; enc[i][1] = 'N'; enc[i][2] = 'C'; enc[i][3] = ' '; enc[i][4] = (char)('1' + i); enc[i][5] = 0;
        rows[n++] = enc[i];
        rows[n++] = lb;
    }
    for (k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++) rows[n++] = KEYS[k];
    vg_controls(PAGES[pg].title, rows, (int)(n / 2u));
}

void vibe_on_draw(uint16_t *fb)
{
    static const char *cap[6];
    static const char *const PANEL_N[3] = {"UPPER", "LOWER", "BOTH"};
    uint32_t g = PAGES[pg].group, col = G_RGB[g], i, single = ED.perf[MV_PF_MODE] == MV_KEY_SINGLE;
    char num[12], name[17];
    (void)fb;
    cap[0] = single ? "SINGLE" : PANEL_N[panel % 3u]; cap[1] = "PERFORM"; cap[2] = "PRG -"; cap[3] = "PRG +";
    cap[4] = "SAVE"; cap[5] = "GLOBAL";
    for (i = 0; i < 6u; i++) { vibe_label(VB_F1 + (int)i, cap[i]); vibe_label(VB_K1 + (int)i, G_NAME[i]); }
    for (i = 0; i < 6u; i++) vibe_led_button(VB_K1 + (int)i, G_RGB[i], g == i ? 1.0f : 0.15f);
    vibe_led_button(VB_K4, G_RGB[G_ARP], g == G_ARP ? 1.0f : ED.arp[MV_ARP_ON] ? 0.6f : 0.15f);
    if (help) { help_view(); return; }
    vg_clear(BG);
    if (!up_k) {
        vg_text(vg_itoa((int)host_fs, num), 240, 110, VG_HUGE, FG, VG_CENTER);
        vg_text("Hz: not a rate JT-80x0 plays at", 240, 190, VG_MED, DIM, VG_CENTER);
        vg_text("44.1 / 48 kHz, x2 or x4 (88.2, 96, 176.4, 192)", 240, 216, VG_MED, DIM, VG_CENTER);
        return;
    }
    header(prog, col);
    if (g == G_PLAY) {                          /* the start page: the program big, the four home knobs */
        prog_name(prog, num, name);
        vg_text(num, 120, 44, VG_HUGE, ACCENT, VG_CENTER);
        vg_text(single ? "SINGLE" : PANEL_N[panel % 3u], 300, 58, VG_BOLD, DIM, VG_LEFT);
        vg_text(vg_itoa((int)voices, num), 440, 54, VG_BIG, FG, VG_RIGHT);
        vg_text("VOICES", 400, 82, VG_SMALL, DIM, VG_RIGHT);
        knobs(pg, 116, ACCENT);
    } else {
        chips(col);
        if (g == G_SEQ) vg_text("the sequencer comes in a later version", 240, 150, VG_MED, DIM, VG_CENTER);
        else knobs(pg, 80, col);
        vg_text(G_NAME[g], 12, 252, VG_BOLD, col, VG_LEFT);
        if (g <= G_FX && g != G_FX && !single) vg_text(PANEL_N[panel % 3u], 468, 252, VG_BOLD, DIM, VG_RIGHT);
    }
    if (toast && vibe_frames() < toast_end) vg_text(toast, 240, 252, VG_BOLD, FG, VG_CENTER);
    softkeys(cap, (g == G_PERF ? 2u : 0u) | (g == G_GLOBAL ? 32u : 0u), (single ? 1u : 0u) | 16u);
    /* FOCUS: the chips, the four values, the soft keys */
    vf_begin();
    if (g != G_PLAY) {
        uint32_t f = group_first(g), n = group_count(g);
        int w = n ? 466 / (int)n : 466;
        for (i = 0; i < n; i++) vf_button(100 + (int)(f + i), 7 + (int)i * w, 42, w - 6, 26);
    }
    for (i = 0; i < 4u; i++)
        if (PAGES[pg].p[i] != P_NONE) vf_encoder(VE_1 + (int)i, colx((int)i) - COLW / 2 + 4, g == G_PLAY ? 114 : 78, COLW - 8, 128);
    for (i = 0; i < 6u; i++) vf_key(VB_F1 + (int)i, 3 + (int)i * 80, 286, 74, 30);
    vf_end(col);
}
