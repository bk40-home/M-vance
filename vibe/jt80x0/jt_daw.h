/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM: the DAW's parameters (Kris 23 Nov 2026, port proposal 7a; P4, 24 Nov). What the pages edit, as
 * parameters a DAW automates and the plugin's Editor shows (tabs UPPER · LOWER · FX · ARP · PERFORM, a card per page):
 *   program                         the factory program (64 names)
 *   u_<id> / l_<id>                 each part's patch bytes on the pages (jt_pid.h: the ids, frozen)
 *   <id>                            the FX bytes 59..66 (UPPER's: the one chain), chorus_sync, delay_sync
 *   u_lfo1_sync / l_lfo1_sync       each part's LFO1 SYNC
 *   rv_* · arp_* · perf_*           the reverb, the arp, the program's performance bytes
 * Values are as the pages show them (signed ones around 0: CUTOFF 0..127, BALANCE -64..+63), whole steps, names for
 * choices. A change from the DAW (VIBE_EV_PARAM) goes into ED like a turn – its part is the parameter's, not PART's –
 * and jt_commit tells the engine. Every change made here (a knob, a CC, a program change) goes back with
 * vibe_param_set, once an event (daw_report), so the DAW records it. The control flow's. */
#pragma once
#include "jt_store.h"                         /* (prog) */
#include "jt_pid.h"

enum { DK_PROG, DK_BYTE, DK_PERF, DK_RV, DK_ARP, DK_PVELO };   /* what a parameter is */
#define JT_NP 160u
static vibe_param DP[JT_NP];
static struct { uint8_t kind, part, idx; int8_t off; } DM[JT_NP];   /* (off: shown = stored - off) */
static const char *DG[JT_NP];                   /* its Editor group, "UPPER · FILTER" */
static char d_id[JT_NP][28], d_name[JT_NP][24], d_grp[JT_NP][28];
static float d_rep[JT_NP];                      /* the value last told to the host */
static uint32_t d_n;
static char prog_names[MV_NPROG][17];
static const char *prog_list[MV_NPROG];

static void s_cat(char *d, const char *a, uint32_t max) { uint32_t i = 0; while (d[i]) i++; while (*a && i + 1u < max) d[i++] = *a++; d[i] = 0; }

/* a parameter: id (prefix + stem), shown name, group, range */
static void d_add(uint32_t kind, uint32_t part, uint32_t idx, const char *pre, const char *stem, const char *name,
                  const char *tab, const char *card, float mn, float mx, float def, int off, const char *const *names)
{
    uint32_t k = d_n;
    if (k >= JT_NP) return;
    d_id[k][0] = 0; s_cat(d_id[k], pre, 28); s_cat(d_id[k], stem, 28);
    d_name[k][0] = 0; s_cat(d_name[k], name, 24);
    d_grp[k][0] = 0; s_cat(d_grp[k], tab, 28); s_cat(d_grp[k], " \xC2\xB7 ", 28); s_cat(d_grp[k], card, 28);
    DP[k].id = d_id[k]; DP[k].name = d_name[k]; DP[k].unit = "";
    DP[k].min = mn; DP[k].max = mx; DP[k].def = def;
    DP[k].steps = (int)(mx - mn) + 1; DP[k].choices = names;
    DG[k] = d_grp[k];
    DM[k].kind = (uint8_t)kind; DM[k].part = (uint8_t)part; DM[k].idx = (uint8_t)idx; DM[k].off = (int8_t)off;
    d_n++;
}

static void card_of(const char *title, char *c)  /* "FILTER 1/2" -> "FILTER" */
{
    uint32_t i = 0;
    while (title[i] && i < 15u) { c[i] = title[i]; i++; }
    c[i] = 0;
    if (i > 4u && c[i - 2u] == '/' && c[i - 4u] == ' ') c[i - 4u] = 0;
}

static void d_patch(uint32_t part, uint32_t b, const char *tab, const char *card)
{
    const mv_param_t *d = &MV_PARAMS[MV_PARAM_AT[b]];
    int off = (d->disp == MV_D_SIGNED || d->disp == MV_D_CENT || d->disp == MV_D_RANGE) ? d->centre : 0;
    char nm[24];
    nm[0] = 0;
    if (!fx_byte(b)) s_cat(nm, part ? "L " : "U ", 24);
    s_cat(nm, d->label, 24);
    d_add(DK_BYTE, part, b, fx_byte(b) ? "" : part ? "l_" : "u_", JT_PID[b], nm, tab, card,
          (float)(d->min - off), (float)(d->max - off), (float)(d->def - off), off, d->disp == MV_D_ENUM ? d->names : 0);
}

static void daw_init(void)                      /* (vibe_on_init) the list, in a fixed order */
{
    static const char *const OFFON[2] = {"OFF", "ON"};
    static const char *const KMODE[3] = {"SINGLE", "DUAL", "SPLIT"};
    static const char *const VSPL[7] = {"1+7", "2+6", "3+5", "4+4", "5+3", "6+2", "7+1"};
    static const char *const ARPP[4] = {"AUTO", "UPPER", "LOWER", "BOTH"};
    static const char *const RV_ID[MV_RV_N] = {"rv_level", "rv_size", "rv_hidamp", "rv_lodamp", "rv_lowpass", "rv_hipass", "rv_freeze"};
    static const char *const RV_NM[MV_RV_N] = {"REVERB", "RV SIZE", "RV HI DAMP", "RV LO DAMP", "RV LOWPASS", "RV HIPASS", "RV FREEZE"};
    static const char *const ARP_ID[MV_ARP_N] = {"arp_on", "arp_mode", "arp_rate", "arp_oct", "arp_gate", "arp_swing", "arp_chance", "arp_hold", "arp_order"};
    static const char *const ARP_NM[MV_ARP_N] = {"ARP", "ARP MODE", "ARP RATE", "ARP OCTAVE", "ARP GATE", "ARP SWING", "ARP CHANCE", "ARP HOLD", "ARP ORDER"};
    static const char *const PF_ID[5] = {"perf_mode", "perf_split", "perf_balance", "perf_voices", "perf_arp"};
    static const char *const PF_NM[5] = {"KEY MODE", "SPLIT", "BALANCE", "VOICE SPLIT", "ARP PART"};
    static const uint8_t PF_K[5] = {MV_PF_MODE, MV_PF_SPLIT, MV_PF_BAL, MV_PF_VSPLIT, MV_PF_ARP};
    static const char *const *PF_C[5] = {KMODE, 0, 0, VSPL, ARPP};
    static const uint8_t PF_MAX[5] = {2, 127, 127, 6, 3};
    static const char *const AMODE[MV_AM_N] = {"UP", "DOWN", "UP&DOWN", "RANDOM", "ORDER", "REPEAT", "DOWN&UP", "UP+8",
        "CONVERGE", "DIVERGE", "PINKY", "THUMB", "WALK", "CHORD"};
    static const char *const ARATE[MV_ARP_NRATE] = {"1/4", "1/8", "1/16", "1/32", "1/8T", "1/16T", "1/2", "1/1", "2 BAR", "4 BAR"};
    static const char *const AORDER[2] = {"NOTE", "PLAYED"};
    static const char *const PVELO_N[2] = {"OFF", "PATCH"};
    uint32_t part, p, i, n;
    char card[16];
    d_n = 0;
    for (n = 0; n < MV_NPROG; n++) {            /* "A-1 Pluck 01" */
        prog_names[n][0] = (char)('A' + n / 16u); prog_names[n][1] = '-';
        i = 2; if (n % 16u + 1u >= 10u) prog_names[n][i++] = '1';
        prog_names[n][i++] = (char)('0' + (n % 16u + 1u) % 10u); prog_names[n][i++] = ' ';
        prog_names[n][i] = 0;
        for (p = 0; p < 16u && i < 16u; p++) prog_names[n][i++] = (char)(MV_FACTORY[n][p] >= 32 && MV_FACTORY[n][p] < 127 ? MV_FACTORY[n][p] : ' ');
        while (i > 0u && prog_names[n][i - 1u] == ' ') i--;
        prog_names[n][i] = 0;
        prog_list[n] = prog_names[n];
    }
    d_add(DK_PROG, 0, 0, "", "program", "PROGRAM", "PERFORM", "PROGRAM", 0.0f, (float)(MV_NPROG - 1u), 0.0f, 0, prog_list);
    for (part = 0; part < 2u; part++)           /* each part: the SOUND, ENV and LFO pages' bytes, then its LFO1 SYNC */
        for (p = 0; p < NPAGES; p++) {
            if (PAGES[p].group != G_SOUND && PAGES[p].group != G_ENV && PAGES[p].group != G_LFO) continue;
            card_of(PAGES[p].title, card);
            for (i = 0; i < 4u; i++) {
                uint32_t b = PAGES[p].p[i];
                if (IS_PATCH(b)) d_patch(part, b, part ? "LOWER" : "UPPER", card);
                else if (b == P_LSYNC)
                    d_add(DK_PERF, part, MV_PF_SYNC(part), part ? "l_" : "u_", "lfo1_sync", part ? "L LFO1 SYNC" : "U LFO1 SYNC",
                          part ? "LOWER" : "UPPER", card, 0.0f, (float)(MV_LFO1_SYNC_N - 1u), 0.0f, 0, SYNC_N);
            }
            if (PAGES[p].group == G_SOUND && PAGES[p].p[0] == 51u)   /* (the AMP page) */
                d_patch(part, 245u, part ? "LOWER" : "UPPER", card);   /* PATCH GAIN (on TONE, but a part's own) */
        }
    for (p = 0; p < NPAGES; p++) {              /* FX: the chain's bytes (UPPER's), its two syncs, the reverb */
        if (PAGES[p].group != G_FX) continue;
        card_of(PAGES[p].title, card);
        for (i = 0; i < 4u; i++) {
            uint32_t b = PAGES[p].p[i];
            if (IS_PATCH(b) && fx_byte(b)) d_patch(0, b, "FX", card);
            else if (b == P_CSYNC) d_add(DK_PERF, 0, MV_PF_CSYNUP, "", "chorus_sync", "CHORUS SYNC", "FX", card, 0.0f, (float)(MV_CSYNC_N - 1u), 0.0f, 0, SYNC_N);
            else if (b == P_DSYNC) d_add(DK_PERF, 0, MV_PF_DSYNUP, "", "delay_sync", "DELAY SYNC", "FX", card, 0.0f, (float)(MV_DSYNC_N - 1u), 0.0f, 0, SYNC_N);
            else if (b >= P_RV && b < P_RV + MV_RV_N) {
                uint32_t k = b - P_RV;
                d_add(DK_RV, 0, k, "", RV_ID[k], RV_NM[k], "FX", card, 0.0f, k == MV_RV_FREEZE ? 1.0f : 127.0f, 0.0f, 0, k == MV_RV_FREEZE ? OFFON : 0);
            }
        }
    }
    for (i = 0; i < MV_ARP_N; i++)              /* the arp */
        d_add(DK_ARP, 0, i, "", ARP_ID[i], ARP_NM[i], "ARP", i < MV_ARP_GATE ? "ARP" : "ARP +", (float)MV_ARP_MIN[i], (float)MV_ARP_MAX[i],
              (float)MV_ARP_DEF[i], 0, i == MV_ARP_ON || i == MV_ARP_HOLD ? OFFON : i == MV_ARP_MODE ? AMODE : i == MV_ARP_RATE ? ARATE : i == MV_ARP_ORDER ? AORDER : 0);
    for (i = 0; i < 5u; i++)                    /* the program's performance */
        d_add(DK_PERF, 0, PF_K[i], "", PF_ID[i], PF_NM[i], "PERFORM", "PERFORM", PF_K[i] == MV_PF_BAL ? -64.0f : 0.0f,
              PF_K[i] == MV_PF_BAL ? 63.0f : (float)PF_MAX[i], PF_K[i] == MV_PF_SPLIT ? 60.0f : 0.0f, PF_K[i] == MV_PF_BAL ? 64 : 0, PF_C[i]);
    d_add(DK_PVELO, 0, 0, "", "patch_velo", "PATCH VELO", "PERFORM", "PERFORM", 0.0f, 1.0f, 0.0f, 0, PVELO_N);   /* (25 Nov: added last) */
    vibe_params(DP, (int)d_n);
    vibe_param_groups(DG);
    {   /* the Editor's cards in the pages' order (else it sorts them by name): each card name once, as listed */
        static vibe_card cards[48];
        static char cname[48][16];
        uint32_t nc = 0, c;
        for (i = 0; i < d_n; i++) {
            const char *nm = d_grp[i];
            while (*nm && *nm != (char)0xB7) nm++;   /* (after "TAB \xC2\xB7 ") */
            if (*nm) nm += 2;
            for (c = 0; c < nc; c++) {          /* (seen already?) */
                uint32_t q = 0;
                while (cname[c][q] && cname[c][q] == nm[q]) q++;
                if (!cname[c][q] && !nm[q]) break;
            }
            if (c == nc && nc < 48u) { cname[nc][0] = 0; s_cat(cname[nc], nm, 16); cards[nc].card = cname[nc]; cards[nc].width = 0; cards[nc].note = 0; nc++; }
        }
        vibe_editor_cards(cards, (int)nc);
    }
    for (i = 0; i < d_n; i++) {                 /* the Editor: choices as menus / switches, signed ones from the middle */
        if (DP[i].choices) vibe_editor_control((int)i, DP[i].steps == 2 ? VIBE_CTL_TOGGLE : VIBE_CTL_MENU);
        else if (DP[i].min < 0.0f) vibe_editor_control((int)i, VIBE_CTL_BIPOLAR);
        d_rep[i] = -1000.0f;                    /* (all told on the first report) */
    }
    vibe_editor_accent(0xFF8A1F);
    vibe_editor_tab_color("UPPER", 0xB38CFF); vibe_editor_tab_color("LOWER", 0x6EE7A8); vibe_editor_tab_color("FX", 0x3FA9F5);
    vibe_editor_tab_color("ARP", 0xFF6A1A); vibe_editor_tab_color("PERFORM", 0xF5C93F);
}

static float d_get(uint32_t k)                  /* parameter k as ED holds it, shown */
{
    switch (DM[k].kind) {
    case DK_PROG: return (float)prog;
    case DK_BYTE: return (float)(mv_param_get(&ED.part[DM[k].part], MV_PARAM_AT[DM[k].idx]) - DM[k].off);
    case DK_PERF: return (float)(ED.perf[DM[k].idx] - DM[k].off);
    case DK_RV:   return (float)ED.rv[DM[k].idx];
    case DK_ARP:  return (float)ED.arp[DM[k].idx];
    case DK_PVELO: return (float)ED.pvelo;
    default: return 0.0f;
    }
}

static void prog_select(uint32_t n);
static void d_set(uint32_t k, float v)          /* parameter k from the DAW: into ED (its own part), clamped */
{
    int32_t x = (int32_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
    x = clampi(x, (int32_t)DP[k].min, (int32_t)DP[k].max) + DM[k].off;
    switch (DM[k].kind) {
    case DK_PROG: if ((uint32_t)x != prog) prog_select((uint32_t)x); break;
    case DK_BYTE: mv_param_set(&ED.part[DM[k].part], MV_PARAM_AT[DM[k].idx], x); break;
    case DK_PERF: ED.perf[DM[k].idx] = (uint8_t)x; break;
    case DK_RV:   ED.rv[DM[k].idx] = (uint8_t)x; break;
    case DK_ARP:  ED.arp[DM[k].idx] = (uint8_t)x; break;
    case DK_PVELO: ED.pvelo = (uint8_t)x; break;
    default: break;
    }
}

static void daw_report(void)                    /* (after every event) what changed here: to the host */
{
    uint32_t k;
    for (k = 0; k < d_n; k++) {
        float v = d_get(k);
        if (v != d_rep[k]) { d_rep[k] = v; vibe_param_set((int)k, v); }
    }
}

static void daw_event(const vibe_event *e)      /* VIBE_EV_PARAM: the DAW moved one (its value is stored already) */
{
    if (e->id >= d_n) return;
    d_set(e->id, e->value);
    d_rep[e->id] = d_get(e->id);                /* (the host has it: not told back) */
}
