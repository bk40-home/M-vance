/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM: the display, 480 x 320, in the house style (OberVave 3's o3_draw.h: a header, the page over the
 * full width, four dials, the soft keys below). Kris 24 Nov 2026, P3 (4a). The control flow's. */
#pragma once
#include "vibe_gfx.h"
#include "vibe_dsp.h"                           /* vd_sin (the dials) */
#include "jt_pages.h"
#include "jt_seq.h"

#define BG 0x0B0D12
#define FG 0xE6EAF2
#define DIM 0x8A93A6
#define MUTED 0x5B6577
#define CELL 0x141A26
#define CELL2 0x24314A
#define ACCENT 0xFF8A1F
#define COLW 115
static int colx(int i) { return 10 + i * COLW + COLW / 2; }

static void dial(int cx, int cy, int r, float v, int bip, uint32_t col)   /* (OberVave's: 7:30 round to 4:30) */
{
    const float span = 0.75f;
    float px = 0, py = 0, a;
    int i;
    v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    for (i = 0; i <= 48; i++) {
        float t = (float)i / 48.0f, aa = 0.625f - span * t;
        float x = (float)cx + vd_sin(aa + 0.25f) * (float)r, y = (float)cy - vd_sin(aa) * (float)r;
        int on = bip ? (v >= 0.5f ? (t >= 0.5f && t <= v) : (t <= 0.5f && t >= v)) : t <= v;
        if (i) vg_line(px, py, x, y, 4.0f, on ? col : CELL2);
        px = x; py = y;
    }
    a = 0.625f - span * v;
    vg_circle((float)cx, (float)cy, (float)r - 9.0f, CELL2);
    vg_line((float)cx, (float)cy, (float)cx + vd_sin(a + 0.25f) * (float)(r - 11), (float)cy - vd_sin(a) * (float)(r - 11), 3.0f, FG);
}

/* the four values of page p: label, dial, value (labels: PLAY names its own) */
static void knobs(uint32_t p, int y, uint32_t col)
{
    static const char *const PLAY_LABEL[4] = {"CUTOFF", "RESO", "DELAY", "REVERB"};
    int i;
    for (i = 0; i < 4; i++) {
        char s[12];
        float f;
        int bip, cx = colx(i);
        uint32_t q_ = PAGES[p].p[i];
        if (q_ == P_NONE) continue;
        p_value(q_, s, &f, &bip);
        vg_text(PAGES[p].group == G_PLAY ? PLAY_LABEL[i] : p_label(q_), cx, y, VG_MED, DIM, VG_CENTER);
        dial(cx, y + 58, 38, f, bip, col);
        vg_text(s, cx, y + 102, VG_BIG, FG, VG_CENTER);
    }
}

static void prog_name(uint32_t n, char *num, char *name)   /* "A-7", "Pluck 07" (the edit copy's: as it plays) */
{
    uint32_t i;
    num[0] = (char)('A' + n / 16u); num[1] = '-';
    vg_itoa((int)(n % 16u + 1u), num + 2);
    for (i = 0; i < 16u; i++) name[i] = (char)(ED.part[0].b[i] >= 32 && ED.part[0].b[i] < 127 ? ED.part[0].b[i] : ' ');
    for (i = 16u; i > 0u && name[i - 1u] == ' '; i--) {}
    name[i] = 0;
}

static void header(uint32_t prog, uint32_t col)
{
    char num[12], name[18];
    uint32_t i;
    prog_name(prog, num, name);
    if (jt_edited(prog)) { for (i = 0; name[i]; i++) {} name[i] = '*'; name[i + 1u] = 0; }   /* edited, not saved */
    vg_rect(0, 0, 480, 36, CELL);
    vg_rect(0, 34, 480, 2, col);
    vg_text(num, 12, 7, VG_BIG, col, VG_LEFT);
    vg_text(name, 70, 7, VG_BIG, FG, VG_LEFT);
    vg_text("JT-80x0", 468, 11, VG_BOLD, DIM, VG_RIGHT);
}

/* the chips of the group shown: its pages, the one open filled */
static void chips(uint32_t col)
{
    uint32_t g = PAGES[pg].group, f = group_first(g), n = group_count(g), i;
    int w = n ? 466 / (int)n : 466;
    for (i = 0; i < n; i++) {
        int x = 7 + (int)i * w, on = f + i == pg;
        vg_round(x, 42, w - 6, 26, 5, on ? col : CELL);
        vg_text(PAGES[f + i].chip, x + (w - 6) / 2, 47, VG_BOLD, on ? BG : DIM, VG_CENTER);
    }
}

/* SAVE: what F5 will do, and the name with its cursor */
static void save_view(uint32_t col)
{
    static const char *const WHAT[3] = {"WRITE", "SWAP", "FACTORY"};
    char num[12], name[18], to[8];
    uint32_t i;
    int x;
    prog_name(prog, num, name);
    to[0] = (char)('A' + sv_to / 16u); to[1] = '-'; vg_itoa((int)(sv_to % 16u + 1u), to + 2);
    vg_text(WHAT[sv_mode % 3u], 12, 236, VG_BOLD, col, VG_LEFT);
    if (sv_mode == 2u) { vg_text(to, 90, 236, VG_BOLD, FG, VG_LEFT); vg_text("back to the factory's", 130, 236, VG_BOLD, DIM, VG_LEFT); }
    else { vg_text(num, 90, 236, VG_BOLD, FG, VG_LEFT); vg_text(sv_mode == 1u ? "<->" : "->", 130, 236, VG_BOLD, DIM, VG_LEFT); vg_text(to, 160, 236, VG_BOLD, FG, VG_LEFT); }
    vg_text("F5 does it, RETURN leaves", 468, 236, VG_SMALL, DIM, VG_RIGHT);
    for (i = 0, x = 12; i < 16u; i++, x += 16) {   /* the name, the cursor's character boxed */
        char c[2];
        c[0] = (char)ED.part[0].b[i]; c[1] = 0;
        if (c[0] < 32 || c[0] > 126) c[0] = ' ';
        if (i == sv_pos) vg_round(x - 1, 256, 15, 22, 3, col);
        vg_text(c, x + 6, 259, VG_MED, i == sv_pos ? BG : FG, VG_CENTER);
    }
}

/* SEQ (P6): each track – where it plays (bar.beat), its events, armed, muted; the transport; on CLIP the clip's name */
static void seq_view(uint32_t col)
{
    static const char *const PN[2] = {"UPPER", "LOWER"};
    char t[40], d[12];
    uint32_t p, i, k, run = sq_pub_run;
    (void)sq_read();
    for (p = 0; p < 2u; p++) {
        uint32_t tk = sq_pub_tick[p], arm = sq_pub_arm >> p & 1u, x = 12u + p * 236u;
        const char *c;
        i = 0;
        for (c = PN[p]; *c; c++) t[i++] = *c;
        t[i++] = ' ';
        if (run) {                              /* bar.beat */
            vg_itoa((int)(tk / MV_SEQ_BAR + 1u), d); for (k = 0; d[k]; k++) t[i++] = d[k];
            t[i++] = '.'; t[i++] = (char)('1' + tk % MV_SEQ_BAR / MV_SEQ_PPQN); t[i++] = ' ';
        }
        vg_itoa((int)sq_view[p].n, d); for (k = 0; d[k]; k++) t[i++] = d[k];
        for (c = " ev"; *c; c++) t[i++] = *c;
        if (ED.sq[MV_SQ_MUTE(p)]) for (c = " MUTE"; *c; c++) t[i++] = *c;
        t[i] = 0;
        if (arm) vg_round((int)x - 4, 206, 8, 20, 3, 0xFF2030u);
        vg_text(t, (int)x + 8, 208, VG_BOLD, p == sq_trk() ? FG : DIM, VG_LEFT);
    }
    vg_text(sq_pub_lead ? "HOST" : run ? "PLAY" : "STOP", 468, 252, VG_BOLD, run ? 0x30E060u : DIM, VG_RIGHT);
    if (PAGES[pg].p[0] == P_SQ + 6u) {          /* CLIP: the name, the cursor's character boxed */
        int x;
        for (i = 0, x = 12; i < JT_CLIP_NAME; i++, x += 16) {
            char c[2];
            c[0] = cl_name[i]; c[1] = 0;
            if (i == cl_pos) vg_round(x - 1, 228, 15, 22, 3, col);
            vg_text(c, x + 6, 231, VG_MED, i == cl_pos ? BG : FG, VG_CENTER);
        }
        vg_text(cl_mode ? "F5 saves PART's track here" : "F5 loads it into PART's track", 468, 231, VG_SMALL, DIM, VG_RIGHT);
    }
}

/* the CLEAR question (REC held 1 s) */
static void ask_view(void)
{
    static const char *const WHAT[3] = {"ALL", "NOTES", "AUTOMATION"};
    const char *who = sq_ask_mask == 3u ? "CLEAR BOTH TRACKS?" : sq_ask_mask & 1u ? "CLEAR UPPER?" : "CLEAR LOWER?";
    vg_round(70, 110, 340, 96, 8, CELL2);
    vg_text(who, 240, 120, VG_BIG, FG, VG_CENTER);
    vg_text(WHAT[sq_ask_mode % 3u], 240, 154, VG_BOLD, 0xFF5A8Au, VG_CENTER);
    vg_text("a knob: what · REC: yes · other: no", 240, 180, VG_SMALL, DIM, VG_CENTER);
}

static void softkeys(const char *const *cap, uint32_t lit, uint32_t dim)   /* F1-F6: bit i lit / dimmed */
{
    int i;
    for (i = 0; i < 6; i++) {
        int x = 3 + i * 80;
        uint32_t on = lit >> i & 1u;
        vg_round(x, 286, 74, 30, 5, on ? ACCENT : CELL);
        vg_text(cap[i], x + 37, 293, VG_BOLD, on ? BG : (dim >> i & 1u) ? MUTED : FG, VG_CENTER);
    }
}
