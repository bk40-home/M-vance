/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM: the display, 480 x 320, in the house style (OberVave 3's o3_draw.h: a header, the page over the
 * full width, four dials, the soft keys below). Kris 24 Nov 2026, P3 (4a). The control flow's. */
#pragma once
#include "vibe_gfx.h"
#include "vibe_dsp.h"                           /* vd_sin (the dials) */
#include "jt_pages.h"

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
    char num[12], name[17];
    prog_name(prog, num, name);
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
