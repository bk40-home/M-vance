/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM: the pages (Kris 24 Nov 2026, P3, 2a 3a 4a 5a). The FM-1's PAGES (firmware/src/mv_ui.c), four values
 * each, grouped under the VIBE's six keys right of ENTER and F2 / F6:
 *   SOUND  OSC 1 · OSC 2 1/2 · 2/2 · OSC COMMON · FILTER 1/2 · 2/2 · AMP · PLAY MODE 1/2 · 2/2
 *   ENV    PITCH ENV · FILTER ENV · AMP ENV        LFO  LFO 1/3 · 2/3 · 3/3 (SYNC)
 *   ARP    ARP 1/2 · 2/2 (K4 again: the arp on / off)   FX  TONE · MULTI-FX · DELAY 1/2 · 2/2 · REVERB 1/2 · 2/2
 *   SEQ    SEQ 1/3 · 2/3 · CLIP (P6: jt_seq.h)      F2 PERFORM 1/2 · 2/2      F6 GLOBAL
 *   PLAY   the start page (RETURN): the program, and the FM-1's HOME knobs – CUTOFF, RESO, the delay's send, REVERB
 *   SAVE   F5 (P5): TO · CURSOR · CHAR · MODE (WRITE / SWAP / FACTORY), as the FM-1's; F5 again does it (main.c)
 * The labels, the short value names and the edit rules are the FM-1's (mv_ui.c col_label, choice_name, col_value,
 * mv_ui_turn): the same words on both. Everything here edits ED (jt_edit.h), the control flow's. */
#pragma once
#include "jt_edit.h"

/* what a value edits: a patch byte (< 0x100) or one of these */
#define P_NONE 0xFFFFu
#define P_RV 0x110u                             /* + MV_RV_* */
#define P_ARP 0x120u                            /* + MV_ARP_* */
#define P_PF 0x140u                             /* + MV_PF_* */
#define P_LSYNC 0x170u                          /* LFO1 SYNC of the part edited */
#define P_CSYNC 0x171u                          /* CHORUS SYNC RATE (UPPER's) */
#define P_DSYNC 0x172u                          /* DELAY SYNC (UPPER's) */
#define P_VOICES 0x180u
#define P_TEMPO 0x181u
#define P_MIDICH 0x182u
#define P_PVELO 0x183u                          /* PATCH VELO: OFF / PATCH (the FM-1's KEYS > VELO SW) */
#define P_SV 0x190u                             /* SAVE: + 0 TO, 1 CURSOR, 2 CHAR, 3 MODE */
#define P_SQ 0x1A0u                             /* SEQ (P6): + 0 LENGTH, QUANT, MUTE L, MUTE U, TRANSPOSE, ROOT, CLIP, MODE,
                                                   CURSOR, CHAR (jt_seq.h) */
#define P_SQ_N 10u
#define IS_PATCH(p) ((p) < 0x100u)

enum { G_SOUND, G_ENV, G_LFO, G_ARP, G_FX, G_SEQ, G_PERF, G_GLOBAL, G_PLAY, G_SAVE, G_N };   /* (K1-K6 = the first six) */
static const char *const G_NAME[G_N] = {"SOUND", "ENV", "LFO", "ARP", "FX", "SEQ", "PERFORM", "GLOBAL", "PLAY", "SAVE"};
static const uint32_t G_RGB[G_N] = {0xB38CFF, 0xF5C93F, 0x6EE7A8, 0xFF6A1A, 0x3FA9F5, 0xFF5A8A, 0xFF8A1F, 0x8A93A6, 0xFF8A1F, 0xFF5A4D};

typedef struct { const char *title, *chip; uint8_t group; uint16_t p[4]; } jt_page_t;   /* chip: its tab (fits 9 across) */
static const jt_page_t PAGES[] = {
    {"PLAY", "PLAY", G_PLAY, {41, 42, 66, P_RV + MV_RV_LEVEL}},      /* the start page: the FM-1's HOME knobs */
    {"OSC 1", "OSC 1", G_SOUND, {30, 31, 32, P_NONE}},                 /* WAVE CTRL1 CTRL2 */
    {"OSC 2 1/2", "OSC 2", G_SOUND, {33, 37, 38, 34}},                 /* WAVE CTRL1 CTRL2 SYNC */
    {"OSC 2 2/2", "OSC 2+", G_SOUND, {35, 36, P_NONE, P_NONE}},         /* RANGE FINE/WIDE */
    {"OSC COMMON", "COMM", G_SOUND, {21, 22, 23, 73}},                /* RING X-MOD BALANCE SHIFT */
    {"FILTER 1/2", "FLT", G_SOUND, {39, 40, 41, 42}},                /* TYPE SLOPE CUTOFF RESO */
    {"FILTER 2/2", "FLT+", G_SOUND, {43, 44, 45, 46}},                /* KEY FOLLOW, LFO1, LFO2, ENV DEPTH */
    {"AMP", "AMP", G_SOUND, {51, 52, 53, 58}},                       /* LEVEL, LFO1, LFO2, PAN MODE */
    {"PLAY MODE 1/2", "MODE", G_SOUND, {71, 72, 69, 70}},             /* MONO LEGATO PORTA, PORTA TIME */
    {"PLAY MODE 2/2", "MODE+", G_SOUND, {243, 244, 67, 68}},           /* UNISON, DETUNE, BEND UP / DOWN */
    {"PITCH ENV", "PITCH ENV", G_ENV, {27, 28, 29, P_NONE}},               /* DEPTH ATTACK DECAY */
    {"FILTER ENV", "FILTER ENV", G_ENV, {47, 48, 49, 50}},                  /* A D S R */
    {"AMP ENV", "AMP ENV", G_ENV, {54, 55, 56, 57}},
    {"LFO 1/3", "LFO 1/3", G_LFO, {16, 17, 18, 24}},                     /* LFO1 WAVE RATE FADE, DEST */
    {"LFO 2/3", "LFO 2/3", G_LFO, {19, 20, 25, 26}},                     /* LFO2 RATE, DEPTH SELECT, PITCH LFO1 / LFO2 */
    {"LFO 3/3", "LFO 3/3", G_LFO, {P_LSYNC, P_NONE, P_NONE, P_NONE}},    /* LFO1 SYNC */
    {"ARP 1/2", "ARP 1/2", G_ARP, {P_ARP + MV_ARP_MODE, P_ARP + MV_ARP_RATE, P_ARP + MV_ARP_OCT, P_ARP + MV_ARP_GATE}},
    {"ARP 2/2", "ARP 2/2", G_ARP, {P_ARP + MV_ARP_HOLD, P_ARP + MV_ARP_SWING, P_ARP + MV_ARP_CHANCE, P_ARP + MV_ARP_ORDER}},
    {"TONE", "TONE", G_FX, {59, 60, 245, P_NONE}},                    /* BASS TREBLE, PATCH GAIN */
    {"MULTI-FX", "MULTI-FX", G_FX, {61, 62, P_CSYNC, P_NONE}},            /* TYPE (12 DISTORTION), LEVEL, CHORUS SYNC */
    {"DELAY 1/2", "DELAY", G_FX, {63, 64, 65, 66}},                    /* TYPE TIME FEEDBACK LEVEL */
    {"DELAY 2/2", "DELAY+", G_FX, {P_DSYNC, P_NONE, P_NONE, P_NONE}},
    {"REVERB 1/2", "REVERB", G_FX, {P_RV + MV_RV_LEVEL, P_RV + MV_RV_SIZE, P_RV + MV_RV_HIDAMP, P_RV + MV_RV_LODAMP}},
    {"REVERB 2/2", "REVERB+", G_FX, {P_RV + MV_RV_LOWPASS, P_RV + MV_RV_HIPASS, P_RV + MV_RV_FREEZE, P_NONE}},
    {"SEQ 1/3", "SEQ 1/3", G_SEQ, {P_SQ + 0, P_SQ + 1, P_SQ + 2, P_SQ + 3}},   /* LENGTH QUANT MUTE L MUTE U */
    {"SEQ 2/3", "SEQ 2/3", G_SEQ, {P_SQ + 4, P_SQ + 5, P_NONE, P_NONE}},       /* TRANSPOSE ROOT */
    {"CLIP", "CLIP", G_SEQ, {P_SQ + 6, P_SQ + 7, P_SQ + 8, P_SQ + 9}},        /* CLIP MODE CURSOR CHAR (F5: do it) */
    {"PERFORM 1/2", "PERFORM 1/2", G_PERF, {P_PF + MV_PF_MODE, P_PF + MV_PF_SPLIT, P_PF + MV_PF_BAL, P_NONE}},
    {"PERFORM 2/2", "PERFORM 2/2", G_PERF, {P_PF + MV_PF_VSPLIT, P_PF + MV_PF_ARP, P_PF + MV_PF_CHLO, P_PF + MV_PF_CHUP}},
    {"GLOBAL", "GLOBAL", G_GLOBAL, {P_VOICES, P_TEMPO, P_MIDICH, P_PVELO}},
    {"SAVE", "SAVE", G_SAVE, {P_SV + 0, P_SV + 1, P_SV + 2, P_SV + 3}},
};
#define NPAGES (sizeof PAGES / sizeof PAGES[0])

/* ---- the panel's state (the control flow's) ------------------------------------------------------------------- */
enum { PANEL_UPPER, PANEL_LOWER, PANEL_BOTH };
static uint32_t pg, panel, g_last[G_N];         /* the page shown; the part(s) edited; each group's last page */
enum { SV_WRITE, SV_SWAP, SV_FACTORY };
static uint32_t sv_to, sv_pos, sv_mode, sv_back; /* SAVE: the slot, the name's cursor, what F5 does; the page to go back to */

static uint32_t group_first(uint32_t g) { uint32_t i; for (i = 0; i < NPAGES; i++) if (PAGES[i].group == g) return i; return 0; }
static uint32_t group_count(uint32_t g) { uint32_t i, n = 0; for (i = 0; i < NPAGES; i++) n += PAGES[i].group == g; return n; }
static void page_go(uint32_t p) { pg = p % NPAGES; g_last[PAGES[pg].group] = pg; }
static void group_go(uint32_t g)                /* a K key / F2 / F6: the group, at its last page */
{
    uint32_t f = group_first(g);
    page_go(g_last[g] >= f && g_last[g] < f + group_count(g) ? g_last[g] : f);
}
static void page_step(int d)                    /* ◀ ▶: within the group, round */
{
    uint32_t g = PAGES[pg].group, f = group_first(g), n = group_count(g);
    page_go(f + (pg - f + n + (uint32_t)(d > 0 ? 1 : n - 1)) % n);
}

/* the part a patch byte is edited in (as mv_ui.c ui_part): SINGLE and the FX bytes 59..66 always UPPER; *both: both */
static int fx_byte(uint32_t p) { return p >= 59u && p <= 66u; }
static uint32_t ed_part(uint32_t p, int *both)
{
    *both = 0;
    if (ED.perf[MV_PF_MODE] == MV_KEY_SINGLE || (IS_PATCH(p) && fx_byte(p))) return MV_PART_UP;
    if (panel == PANEL_BOTH) { *both = 1; return MV_PART_UP; }
    return panel == PANEL_LOWER ? MV_PART_LO : MV_PART_UP;
}

/* ---- labels and values (mv_ui.c col_label / choice_name / col_value, on ED) ------------------------------------ */
static int sq_label(uint32_t p, const char **l);   /* (the SEQ values: jt_seq.h) */
static int sq_value(uint32_t p, char *s, float *fill);
static int sq_turn(uint32_t p, int32_t step);
static int32_t sq_span(uint32_t p);
static const char *p_label(uint32_t p)
{
    const char *sl;
    static const char *const C1A[7] = {"DETUNE", "OFFSET", "CUTOFF", "HARMON", "PW", "SHAPE", "SHAPE"};
    static const char *const C1B[7] = {"MIX", "LFO1", "RESO", "FEEDBK", "PWM", "LFO1", "LFO1"};
    static const char *const C2A[4] = {"PW", "SHAPE", "SHAPE", "CUTOFF"};
    static const char *const C2B[4] = {"PWM", "LFO1", "LFO1", "RESO"};
    int both;
    const uint8_t *b = ED.part[ed_part(p, &both)].b;
    if (sq_label(p, &sl)) return sl;
    switch (p) {
    case P_PF + MV_PF_MODE: return "MODE";
    case P_PF + MV_PF_SPLIT: return "SPLIT";
    case P_PF + MV_PF_BAL: return "BALANCE";
    case P_PF + MV_PF_VSPLIT: return "VOICES";
    case P_PF + MV_PF_ARP: return "ARP";
    case P_PF + MV_PF_CHLO: return "CH LO";
    case P_PF + MV_PF_CHUP: return "CH UP";
    case P_LSYNC: return "SYNC 1";
    case P_CSYNC: case P_DSYNC: return "SYNC";
    case P_MIDICH: return "MIDI CH";
    case P_VOICES: return "VOICES";
    case P_TEMPO: return "TEMPO";
    case P_PVELO: return "VELO SW";
    case P_RV + MV_RV_LEVEL: return "LEVEL";
    case P_RV + MV_RV_SIZE: return "SIZE";
    case P_RV + MV_RV_HIDAMP: return "HI DAMP";
    case P_RV + MV_RV_LODAMP: return "LO DAMP";
    case P_RV + MV_RV_LOWPASS: return "LOWPASS";
    case P_RV + MV_RV_HIPASS: return "HIPASS";
    case P_RV + MV_RV_FREEZE: return "FREEZE";
    case P_ARP + MV_ARP_MODE: return "MODE";
    case P_ARP + MV_ARP_RATE: return "RATE";
    case P_ARP + MV_ARP_OCT: return "OCTAVE";
    case P_ARP + MV_ARP_GATE: return "GATE";
    case P_ARP + MV_ARP_HOLD: return "HOLD";
    case P_ARP + MV_ARP_SWING: return "SWING";
    case P_ARP + MV_ARP_CHANCE: return "CHANCE";
    case P_ARP + MV_ARP_ORDER: return "ORDER";
    case P_SV + 0: return "TO";
    case P_SV + 1: return "CURSOR";
    case P_SV + 2: return "CHAR";
    case P_SV + 3: return "MODE";
    case 31: return C1A[b[30] < 7u ? b[30] : 5u];
    case 32: return C1B[b[30] < 7u ? b[30] : 5u];
    case 37: return C2A[b[33] & 3u];
    case 38: return C2B[b[33] & 3u];
    case 16: return "WAVE";
    case 19: return "RATE 2";
    case 17: return "RATE 1";
    case 20: return "DEPTH>";
    case 22: return "X-MOD";
    case 24: return "DEST";
    case 25: return "P.LFO1";
    case 26: return "P.LFO2";
    case 43: return "KEY FLW";
    case 46: return "ENV DEP";
    case 58: return "PAN";
    case 67: return "BEND +";
    case 68: return "BEND -";
    case 70: return "TIME";
    case 244: return "DETUNE";
    default: return IS_PATCH(p) && MV_PARAM_AT[p] != MV_NONE ? MV_PARAMS[MV_PARAM_AT[p]].label : "";
    }
}

static const char *const SYNC_N[MV_CSYNC_N] = {"OFF", "1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.", "1/4", "1/2T",
    "1/4.", "1/2", "1/1T", "1/2.", "1/1", "2/1T", "1/1.", "2/1", "3 MEAS", "4 MEAS", "5 MEAS", "6 MEAS", "7 MEAS",
    "8 MEAS", "LFO1"};
static const char *choice_name(uint32_t p, uint32_t v)
{
    static const char *const OFFON[2] = {"OFF", "ON"};
    static const char *const LW[4] = {"TRI", "SAW", "SQR", "RANDOM"};
    static const char *const DEST[3] = {"OSC1+2", "OSC2", "X-MOD"};
    static const char *const DSEL[3] = {"PITCH", "FILTER", "AMP"};
    static const char *const W1[7] = {"SSAW", "TRIMOD", "NOISE", "FBK", "SQR", "SAW", "TRI"};
    static const char *const W2[4] = {"SQR", "SAW", "TRI", "NOISE"};
    static const char *const FT[3] = {"HPF", "BPF", "LPF"};
    static const char *const SL[2] = {"-12", "-24"};
    static const char *const PAN[3] = {"OFF", "AUTO", "MANUAL"};
    static const char *const GAIN[3] = {"0dB", "+6dB", "+12dB"};
    static const char *const MFX[13] = {"SC SLW", "SC MID", "SC FST", "SC CLR", "FL SLOW", "FL DEEP", "FL FAST",
                                        "DEEP PH", "JET PH", "TWIST", "FREEZE1", "FREEZE2", "DIST"};
    static const char *const DLY[5] = {"PAN L>R", "PAN R>L", "PAN SHT", "MONO S", "MONO L"};
    static const char *const AMODE[MV_AM_N] = {"UP", "DN", "UPDN", "RND", "ORD", "REPEAT", "DNUP", "UP+8", "CONV",
                                               "DIVG", "PINKY", "THUMB", "WALK", "CHORD"};
    static const char *const ARATE[MV_ARP_NRATE] = {"1/4", "1/8", "1/16", "1/32", "8T", "16T", "1/2", "1/1", "2BAR", "4BAR"};
    static const char *const AORDER[2] = {"NOTE", "PLAYED"};
    static const char *const KMODE[3] = {"SINGLE", "DUAL", "SPLIT"};
    static const char *const VSPL[7] = {"1+7", "2+6", "3+5", "4+4", "5+3", "6+2", "7+1"};
    static const char *const ARPP[4] = {"AUTO", "UPPER", "LOWER", "BOTH"};
    switch (p) {
    case 16: return LW[v & 3u];
    case 20: return DSEL[v < 3u ? v : 0u];
    case 24: return DEST[v < 3u ? v : 0u];
    case 30: return W1[v < 7u ? v : 0u];
    case 33: return W2[v & 3u];
    case 39: return FT[v < 3u ? v : 2u];
    case 40: return SL[v & 1u];
    case 58: return PAN[v < 3u ? v : 0u];
    case 245: return GAIN[v < 3u ? v : 0u];
    case 61: return MFX[v < 13u ? v : 0u];
    case 63: return DLY[v < 5u ? v : 0u];
    case 21: case 34: case 69: case 71: case 72: case 243: case P_RV + MV_RV_FREEZE: case P_ARP + MV_ARP_HOLD:
        return OFFON[v & 1u];
    case P_ARP + MV_ARP_MODE: return AMODE[v < MV_AM_N ? v : 0u];
    case P_ARP + MV_ARP_RATE: return ARATE[v < MV_ARP_NRATE ? v : 2u];
    case P_ARP + MV_ARP_ORDER: return AORDER[v & 1u];
    case P_PF + MV_PF_MODE: return KMODE[v < 3u ? v : 0u];
    case P_PF + MV_PF_VSPLIT: return VSPL[v < 7u ? v : 3u];
    case P_PF + MV_PF_ARP: return ARPP[v & 3u];
    case P_LSYNC: return SYNC_N[v < MV_LFO1_SYNC_N ? v : 0u];
    case P_DSYNC: return SYNC_N[v < MV_DSYNC_N ? v : 0u];
    case P_CSYNC: return SYNC_N[v < MV_CSYNC_N ? v : 0u];
    default: return 0;
    }
}

static void s_copy(char *s, const char *a) { uint32_t i = 0; while (a[i] && i < 7u) { s[i] = a[i]; i++; } s[i] = 0; }
static void s_num(char *s, int32_t v, int sign)
{
    char t[8];
    uint32_t n = 0, a = (uint32_t)(v < 0 ? -v : v), i = 0;
    do { t[n++] = (char)('0' + a % 10u); a /= 10u; } while (a && n < 6u);
    if (v < 0) s[i++] = '-'; else if (sign && v > 0) s[i++] = '+';
    while (n) s[i++] = t[--n];
    s[i] = 0;
}

/* p's value as text (s: 8 chars) and where its dial stands: *fill 0..1, *bip 1 = from the middle */
static void p_value(uint32_t p, char *s, float *fill, int *bip)
{
    const char *c;
    int both;
    *fill = 0.0f; *bip = 0; s[0] = 0;
    if (p == P_NONE || sq_value(p, s, fill)) return;
    if (p >= P_SV && p < P_SV + 4u) {           /* SAVE */
        static const char *const SVM[3] = {"WRITE", "SWAP", "FACTORY"};
        uint32_t k = p - P_SV, c;
        if (k == 0u) { s[0] = (char)('A' + sv_to / 16u); s[1] = '-'; s_num(s + 2, (int32_t)(sv_to % 16u + 1u), 0); *fill = (float)sv_to / 63.0f; }
        else if (k == 1u) { s_num(s, (int32_t)sv_pos + 1, 0); *fill = (float)sv_pos / 15.0f; }
        else if (k == 2u) { c = ED.part[0].b[sv_pos % MV_NAME_LEN]; if (c == ' ') s_copy(s, "SPC"); else { s[0] = (char)c; s[1] = 0; } *fill = (float)(c - 32u) / 93.0f; }
        else { s_copy(s, SVM[sv_mode % 3u]); *fill = (float)sv_mode / 2.0f; }
        return;
    }
    if (p == P_VOICES) { s_num(s, ED.nv, 0); *fill = (float)ED.nv / (float)MV_NV; return; }
    if (p == P_TEMPO) { s_num(s, ED.tempo, 0); *fill = (float)(ED.tempo - 40) / 200.0f; return; }
    if (p == P_PVELO) { s_copy(s, ED.pvelo ? "PATCH" : "OFF"); *fill = (float)ED.pvelo; return; }   /* (PATCH: each
                                                   patch's own VELO SW decides, as on the FM-1) */
    if (p == P_MIDICH) { if (ED.midich) s_num(s, ED.midich, 0); else s_copy(s, "OMNI"); *fill = (float)ED.midich / 16.0f; return; }
    if (p == P_LSYNC || p == 17u) {             /* RATE 1 shows the SYNC while its part is synced */
        uint32_t sy = ED.perf[MV_PF_SYNC(ed_part(p, &both))];
        if (p == P_LSYNC || sy) { s_copy(s, choice_name(P_LSYNC, sy)); *fill = (float)sy / (float)(MV_LFO1_SYNC_N - 1u); return; }
    }
    if (p == P_CSYNC) { s_copy(s, choice_name(p, ED.perf[MV_PF_CSYNUP])); *fill = (float)ED.perf[MV_PF_CSYNUP] / (float)(MV_CSYNC_N - 1u); return; }
    if (p == P_DSYNC || p == 64u) {             /* TIME shows the SYNC while synced */
        uint32_t sy = ED.perf[MV_PF_DSYNUP];
        if (p == P_DSYNC || sy) { s_copy(s, choice_name(P_DSYNC, sy)); *fill = (float)sy / (float)(MV_DSYNC_N - 1u); return; }
    }
    if (p >= P_PF && p < P_PF + MV_PF_N) {
        static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        uint32_t k = p - P_PF, v = ED.perf[k], i = 0;
        if ((c = choice_name(p, v)) != 0) { s_copy(s, c); *fill = k == MV_PF_MODE ? (float)v / 2.0f : (float)v / 6.0f; return; }
        if (k == MV_PF_SPLIT) {
            for (c = NN[v % 12u]; *c; c++) s[i++] = *c;
            if (v < 12u) { s[i++] = '-'; s[i++] = '1'; } else s[i++] = (char)('0' + v / 12u - 1u);
            s[i] = 0; *fill = (float)v / 127.0f; return;
        }
        if ((k == MV_PF_CHLO || k == MV_PF_CHUP) && !v) { s_copy(s, "GLOBAL"); return; }
        s_num(s, (int32_t)v, 0);
        if (k == MV_PF_BAL) { *bip = 1; *fill = (float)v / 127.0f; } else *fill = (float)v / 16.0f;
        return;
    }
    if (p >= P_ARP && p < P_ARP + MV_ARP_N) {
        uint32_t k = p - P_ARP, v = ED.arp[k];
        *fill = MV_ARP_MAX[k] > MV_ARP_MIN[k] ? (float)(v - MV_ARP_MIN[k]) / (float)(MV_ARP_MAX[k] - MV_ARP_MIN[k]) : 0.0f;
        if ((c = choice_name(p, v)) != 0) { s_copy(s, c); return; }
        s_num(s, (int32_t)v, 0); return;
    }
    if (p >= P_RV && p < P_RV + MV_RV_N) {
        uint32_t v = ED.rv[p - P_RV];
        *fill = p == P_RV + MV_RV_FREEZE ? (float)v : (float)v / 127.0f;
        if ((c = choice_name(p, v)) != 0) { s_copy(s, c); return; }
        s_num(s, (int32_t)v, 0); return;
    }
    if (IS_PATCH(p) && MV_PARAM_AT[p] != MV_NONE) {
        const mv_param_t *d = &MV_PARAMS[MV_PARAM_AT[p]];
        int32_t v = mv_param_get(&ED.part[ed_part(p, &both)], MV_PARAM_AT[p]);
        *fill = d->max > d->min ? (float)(v - d->min) / (float)(d->max - d->min) : 0.0f;
        if ((c = choice_name(p, (uint32_t)v)) != 0) { s_copy(s, c); return; }
        if (d->disp == MV_D_SIGNED || d->disp == MV_D_CENT || d->disp == MV_D_RANGE) {
            *bip = 1;
            if (d->disp == MV_D_RANGE && (v == d->min || v == d->max)) s_copy(s, v == d->min ? "-WIDE" : "+WIDE");
            else s_num(s, v - d->centre, 1);
            return;
        }
        s_num(s, v, 0);
    }
}

/* ---- a turn: p by step (as mv_ui.c mv_ui_turn), into ED; jt_commit tells the engine ------------------------------ */
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static int32_t p_span(uint32_t p)               /* how far the value goes (SHIFT x 8 on the long ones) */
{
    if (IS_PATCH(p) && MV_PARAM_AT[p] != MV_NONE) return MV_PARAMS[MV_PARAM_AT[p]].max - MV_PARAMS[MV_PARAM_AT[p]].min;
    if (p >= P_RV && p < P_RV + MV_RV_N) return p == P_RV + MV_RV_FREEZE ? 1 : 127;
    if (p >= P_ARP && p < P_ARP + MV_ARP_N) return MV_ARP_MAX[p - P_ARP] - MV_ARP_MIN[p - P_ARP];
    if (p == P_PF + MV_PF_SPLIT || p == P_PF + MV_PF_BAL) return 127;
    if (p == P_TEMPO) return 200;
    if (p >= P_SV && p < P_SV + 4u) return p == P_SV + 0u ? 63 : 0;   /* (SHIFT steps TO by 8: a bank half) */
    if (p >= P_SQ && p < P_SQ + P_SQ_N) return sq_span(p);
    return 16;
}
static void p_turn(uint32_t p, int32_t step)
{
    int both;
    if (p == P_NONE || !step || sq_turn(p, step)) return;
    if (p >= P_SV && p < P_SV + 4u) {           /* SAVE: the slot, the cursor, a character of the name (ASCII 32..125,
                                                   as the JP's: the name is UPPER's bytes 0..15), the mode */
        uint32_t k = p - P_SV;
        if (k == 0u) sv_to = (uint32_t)clampi((int32_t)sv_to + step, 0, (int32_t)MV_NPROG - 1);
        else if (k == 1u) sv_pos = (uint32_t)clampi((int32_t)sv_pos + step, 0, (int32_t)MV_NAME_LEN - 1);
        else if (k == 2u) { uint8_t *c = &ED.part[0].b[sv_pos % MV_NAME_LEN]; *c = (uint8_t)clampi((int32_t)*c + step, 32, 125); }
        else sv_mode = (uint32_t)clampi((int32_t)sv_mode + step, 0, 2);
        return;
    }
    if (p == P_VOICES) { ED.nv = (uint8_t)clampi(ED.nv + step, 1, (int32_t)MV_NV); return; }
    if (p == P_TEMPO) { ED.tempo = (uint8_t)clampi(ED.tempo + step, 40, 240); return; }
    if (p == P_MIDICH) { ED.midich = (uint8_t)clampi(ED.midich + step, 0, 16); return; }
    if (p == P_PVELO) { ED.pvelo = (uint8_t)clampi(ED.pvelo + step, 0, 1); return; }
    if (p == P_LSYNC) {
        uint32_t k = MV_PF_SYNC(ed_part(p, &both));
        ED.perf[k] = (uint8_t)clampi(ED.perf[k] + step, 0, (int32_t)MV_LFO1_SYNC_N - 1);
        if (both) ED.perf[MV_PF_SYNCLO] = (uint8_t)clampi(ED.perf[MV_PF_SYNCLO] + step, 0, (int32_t)MV_LFO1_SYNC_N - 1);
        return;
    }
    if (p == P_CSYNC || p == P_DSYNC) {
        uint32_t k = p == P_CSYNC ? MV_PF_CSYNUP : MV_PF_DSYNUP;
        ED.perf[k] = (uint8_t)clampi(ED.perf[k] + step, 0, (int32_t)(p == P_CSYNC ? MV_CSYNC_N : MV_DSYNC_N) - 1);
        return;
    }
    if (p >= P_PF && p < P_PF + MV_PF_N) {
        static const uint8_t TOP[MV_PF_N] = {2, 127, 127, 6, 16, 16, 3, MV_LFO1_SYNC_N - 1u, MV_LFO1_SYNC_N - 1u,
                                             MV_DSYNC_N - 1u, MV_DSYNC_N - 1u, MV_CSYNC_N - 1u, MV_CSYNC_N - 1u};
        uint32_t k = p - P_PF;
        ED.perf[k] = (uint8_t)clampi(ED.perf[k] + step, 0, TOP[k]);
        return;
    }
    if (p >= P_ARP && p < P_ARP + MV_ARP_N) {
        uint32_t k = p - P_ARP;
        ED.arp[k] = (uint8_t)clampi(ED.arp[k] + step, MV_ARP_MIN[k], MV_ARP_MAX[k]);
        return;
    }
    if (p >= P_RV && p < P_RV + MV_RV_N) {
        ED.rv[p - P_RV] = (uint8_t)clampi(ED.rv[p - P_RV] + step, 0, p == P_RV + MV_RV_FREEZE ? 1 : 127);
        return;
    }
    if (IS_PATCH(p) && MV_PARAM_AT[p] != MV_NONE) { /* the part PANEL says; BOTH: each part by the step, from where it is */
        uint32_t id = MV_PARAM_AT[p];
        const mv_param_t *d = &MV_PARAMS[id];
        mv_patch_t *t = &ED.part[ed_part(p, &both)];
        mv_param_set(t, id, clampi(mv_param_get(t, id) + step, d->min, d->max));
        if (both) mv_param_set(&ED.part[MV_PART_LO], id, clampi(mv_param_get(&ED.part[MV_PART_LO], id) + step, d->min, d->max));
    }
}
