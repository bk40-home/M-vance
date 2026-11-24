/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM, P3's tests (Kris 24 Nov 2026, claude/jt80x0-vibe-p3-pages-proposal-2026-11-24.md): main.c itself,
 * natively, against stand-ins for the SDK (stubs.h).
 *   ui_test check            every page, every encoder, end to end and back, in each PART on a layered program: the
 *                            values stay in their ranges, their texts fit a column, and after each block the engine
 *                            holds exactly what the edit copy holds. Then the edits that must land in one place
 *                            (cutoff, an FX byte under LOWER, BOTH), MIDI (CC 74, bend, wheel, pressure, program
 *                            change), sustain, F3 / F4, ARP on / off.
 *   ui_test hash <rate>      a played and edited run (the list below), wasmtest's out_hash: the .wasm must print the
 *   ui_test script <rate> f  same for the script this writes (vibe/test/run.sh) */
#include "stubs.h"
#include "../jt80x0/main.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void ev(int type, int id, int down, float value, int midi)
{
    vibe_event e;
    memset(&e, 0, sizeof e);
    e.type = (uint8_t)type; e.id = (uint8_t)id; e.down = (uint8_t)down; e.value = value;
    e.midi[0] = (uint8_t)(midi & 255); e.midi[1] = (uint8_t)(midi >> 8 & 255); e.midi[2] = (uint8_t)(midi >> 16 & 255);
    if (type == VIBE_EV_KEY) {                  /* (the SDK with vibe_notes: a key is note 48 + key, from the keys) */
        e.type = VIBE_EV_NOTE; e.id = (uint8_t)(48 + id); e.midi[0] = 0; e.midi[1] = VIBE_NOTE_KEYS;
    }
    vibe_on_event(&e);
}
static void press(int b) { ev(VIBE_EV_BUTTON, b, 1, 0, 0); ev(VIBE_EV_BUTTON, b, 0, 0, 0); }
static float L[256], R[256];
static void block(void) { vibe_on_audio(0, L, R, 64); stub_frames += 64; }

static int engine_is_ed(void)                   /* the engine holds what the edit copy says */
{
    uint32_t i;
    for (i = 0; i < MV_PATCH_BYTES; i++) if (eng.patch.b[i] != ED.part[0].b[i] || eng.lower.b[i] != ED.part[1].b[i]) return 0;
    for (i = 0; i < MV_PF_N; i++) if (eng.perf[i] != ED.perf[i]) return 0;
    for (i = 0; i < MV_RV_N; i++) if (eng.rvb[i] != ED.rv[i]) return 0;
    for (i = 0; i < MV_ARP_N; i++) if (eng.arpb[i] != ED.arp[i]) return 0;
    return eng.nv == ED.nv && jt_own_bpm == ED.tempo;
}

static void walk(void)                          /* every page, every value, both ways */
{
    uint32_t p, i, k;
    for (p = 0; p < NPAGES; p++) {
        page_go(p);
        for (i = 0; i < 4u; i++) {
            uint32_t q_ = PAGES[p].p[i];
            if (q_ == P_NONE) continue;
            for (k = 0; k < 2u * 260u; k++) {
                char s[16];
                float f;
                int bip;
                ev(VIBE_EV_ENCODER, VE_1 + (int)i, 0, k < 260u ? 1.0f : -1.0f, 0);
                block();
                p_value(q_, s, &f, &bip);
                if (k % 37u == 0u || k == 259u || k == 519u) {
                    CHECK(strlen(s) >= 1u && strlen(s) <= 7u, "%s / %s: value text \"%s\"", PAGES[p].title, p_label(q_), s);
                    CHECK(f >= 0.0f && f <= 1.0f, "%s / %s: dial %f", PAGES[p].title, p_label(q_), (double)f);
                    CHECK(strlen(p_label(q_)) >= 1u && strlen(p_label(q_)) <= 7u, "%s: label \"%s\"", PAGES[p].title, p_label(q_));
                }
                if (IS_PATCH(q_)) {
                    const mv_param_t *d = &MV_PARAMS[MV_PARAM_AT[q_]];
                    int both, v = mv_param_get(&ED.part[ed_part(q_, &both)], MV_PARAM_AT[q_]);
                    if (v < d->min || v > d->max) { CHECK(0, "%s byte %u: %d outside %u..%u", PAGES[p].title, q_, v, d->min, d->max); break; }
                }
                if (!engine_is_ed()) { CHECK(0, "%s / %s: the engine differs from the edit copy (turn %u)", PAGES[p].title, p_label(q_), k); break; }
            }
        }
    }
}

static int find_layered(void)                   /* a factory program that is DUAL or SPLIT */
{
    int n;
    for (n = 0; n < (int)MV_NPROG; n++) if (MV_FACTORY_PERF[n][MV_PF_MODE] != MV_KEY_SINGLE) return n;
    return -1;
}

static void check(void)
{
    int lay, i;
    uint8_t c0;
    vibe_on_init(44100.0f);
    block();
    CHECK(engine_is_ed(), "after init");
    /* every page in SINGLE, then on a layered program in each PART */
    walk();
    lay = find_layered();
    CHECK(lay >= 0, "a DUAL / SPLIT factory program");
    if (lay >= 0) {
        uint32_t pn;
        prog_select((uint32_t)lay); block();
        CHECK(engine_is_ed(), "layered program %d loaded", lay);
        for (pn = 0; pn < 3u; pn++) { panel = pn; walk(); }
        /* an FX byte under LOWER: UPPER's (the one chain) */
        prog_select((uint32_t)lay); block(); panel = PANEL_LOWER;
        page_go(group_first(G_FX) + 2u);        /* DELAY 1/2 (TONE, MULTI-FX, DELAY 1/2 ..) */
        c0 = eng.patch.b[66];
        ev(VIBE_EV_ENCODER, VE_4, 0, -3.0f, 0); block();
        CHECK(eng.patch.b[66] == (c0 >= 3 ? c0 - 3 : 0) && eng.lower.b[66] == MV_FACTORY_LO[lay][66], "DELAY LEVEL under LOWER: UPPER's");
        /* BOTH: each part by the step, from where it is */
        prog_select((uint32_t)lay); block(); panel = PANEL_BOTH;
        page_go(group_first(G_SOUND) + 4u);     /* FILTER 1/2 */
        ev(VIBE_EV_ENCODER, VE_3, 0, -5.0f, 0); block();
        CHECK(eng.patch.b[41] == (MV_FACTORY[lay][41] >= 5 ? MV_FACTORY[lay][41] - 5 : 0) &&
              eng.lower.b[41] == (MV_FACTORY_LO[lay][41] >= 5 ? MV_FACTORY_LO[lay][41] - 5 : 0), "CUTOFF under BOTH: both by 5");
        panel = PANEL_UPPER;
    }
    /* cutoff on a single: byte 41, SHIFT x 8 */
    prog_select(0); block(); page_go(group_first(G_SOUND) + 4u);
    c0 = eng.patch.b[41];
    ev(VIBE_EV_ENCODER, VE_3, 0, -1.0f, 0); block();
    CHECK(eng.patch.b[41] == c0 - 1, "CUTOFF -1: %u -> %u", c0, eng.patch.b[41]);
    ev(VIBE_EV_BUTTON, VB_SHIFT, 1, 0, 0); ev(VIBE_EV_ENCODER, VE_3, 0, -1.0f, 0); ev(VIBE_EV_BUTTON, VB_SHIFT, 0, 0, 0); block();
    CHECK(eng.patch.b[41] == (c0 >= 9 ? c0 - 9 : 0), "SHIFT: CUTOFF -8 more: %u", eng.patch.b[41]);
    /* MIDI: CC 74 the way mv_cc_apply reads it, bend, wheel, pressure, program change */
    {
        mv_patch_t t = ED.part[0];
        (void)mv_cc_apply(&t, 74, 40);
        ev(VIBE_EV_MIDI, 0, 0, 0, 0xB0 | 74 << 8 | 40 << 16); block();
        CHECK(!memcmp(eng.patch.b, t.b, MV_PATCH_BYTES), "CC 74 = mv_cc_apply");
    }
    ev(VIBE_EV_MIDI, 0, 0, 0, 0xE0 | 0 << 8 | 96 << 16); block();
    CHECK(eng.bend14 == 96 * 128 - 8192, "bend %d", eng.bend14);
    ev(VIBE_EV_MIDI, 0, 0, 0, 0xB0 | 1 << 8 | 99 << 16); block();
    CHECK(eng.wheel == 99u, "wheel %u", eng.wheel);
    ev(VIBE_EV_MIDI, 0, 0, 0, 0xD0 | 77 << 8); block();
    CHECK(eng.ctl == 77u, "pressure -> CONTROL %u", eng.ctl);
    ev(VIBE_EV_MIDI, 0, 0, 0, 0xC0 | 9 << 8); block();
    CHECK(!memcmp(eng.patch.b, MV_FACTORY[9], MV_PATCH_BYTES) && prog == 9u, "program change 9");
    press(VB_F4); block();
    CHECK(prog == 10u && !memcmp(eng.patch.b, MV_FACTORY[10], MV_PATCH_BYTES), "F4: program 10");
    press(VB_F3); press(VB_F3); block();
    CHECK(prog == 8u, "F3 twice: program 8");
    /* sustain: a note let go while the pedal is down rings on until it comes up */
    prog_select(0); block();
    ev(VIBE_EV_KEY, 12, 1, 0.8f, 0); block();
    ev(VIBE_EV_PEDAL, VIBE_PEDAL_SWITCH, 1, 0, 0);
    ev(VIBE_EV_KEY, 12, 0, 0, 0);
    for (i = 0; i < 100; i++) block();
    CHECK(mv_engine_sounding(&eng, 60), "sustain: note 60 held by the pedal");
    ev(VIBE_EV_PEDAL, VIBE_PEDAL_SWITCH, 0, 0, 0); block();
    CHECK(!mv_engine_sounding(&eng, 60), "pedal up: note 60 let go");
    ev(VIBE_EV_MIDI, 0, 0, 0, 0xB0 | 64 << 8 | 127 << 16); ev(VIBE_EV_KEY, 14, 1, 0.8f, 0); ev(VIBE_EV_KEY, 14, 0, 0, 0);
    for (i = 0; i < 20; i++) block();
    CHECK(mv_engine_sounding(&eng, 62), "sustain by CC 64");
    ev(VIBE_EV_MIDI, 0, 0, 0, 0xB0 | 64 << 8 | 0 << 16); block();
    CHECK(!mv_engine_sounding(&eng, 62), "CC 64 off: let go");
    /* ARP: K4 opens it, K4 again turns it on, again off */
    press(VB_K4); block();
    CHECK(PAGES[pg].group == G_ARP && !eng.arpb[MV_ARP_ON], "K4: the ARP page, arp off");
    press(VB_K4); block();
    CHECK(eng.arpb[MV_ARP_ON] == 1u, "K4 again: arp on");
    press(VB_K4); block();
    CHECK(eng.arpb[MV_ARP_ON] == 0u, "K4 again: arp off");
    /* the pages: K keys, the arrows round the group, RETURN to PLAY, F2 / F6 */
    press(VB_K1); CHECK(PAGES[pg].group == G_SOUND && !strcmp(PAGES[pg].title, "FILTER 1/2"), "K1: SOUND at its last page (%s)", PAGES[pg].title);
    page_go(group_first(G_SOUND));
    press(VB_LEFT); CHECK(!strcmp(PAGES[pg].title, "PLAY MODE 2/2"), "LEFT from OSC 1: round to %s", PAGES[pg].title);
    press(VB_RIGHT); CHECK(!strcmp(PAGES[pg].title, "OSC 1"), "RIGHT: back to OSC 1");
    press(VB_RETURN); CHECK(PAGES[pg].group == G_PLAY, "RETURN: PLAY");
    press(VB_F6); CHECK(PAGES[pg].group == G_GLOBAL, "F6 GLOBAL");
    press(VB_F2); CHECK(PAGES[pg].group == G_PERF, "F2 PERFORM");
    {
        uint32_t was = pg;
        press(VB_F2); CHECK(PAGES[pg].group == G_PERF && pg != was, "F2 again: the other PERFORM page");
    }
    printf("ui_test: %d of %d checks failed\n", fails, checks);
}

/* ---- the played and edited run: the same events here and in the .wasm (wasmtest), the same hash ------------------- */
static const struct { int b, type, id, down; float value; int midi; } RUN[] = {
    {10, 2, 21, 1, 0.8f, 0}, {10, 2, 16, 1, 0.6f, 0},
    {40, 3, VB_K1, 1, 0, 0}, {40, 3, VB_K1, 0, 0, 0}, {45, 4, VE_1, 0, 1.0f, 0},                 /* OSC 1: SAW -> TRI */
    {60, 3, VB_RIGHT, 1, 0, 0}, {60, 3, VB_RIGHT, 0, 0, 0}, {61, 3, VB_RIGHT, 1, 0, 0}, {61, 3, VB_RIGHT, 0, 0, 0},
    {62, 3, VB_RIGHT, 1, 0, 0}, {62, 3, VB_RIGHT, 0, 0, 0}, {63, 3, VB_RIGHT, 1, 0, 0}, {63, 3, VB_RIGHT, 0, 0, 0},
    {70, 4, VE_3, 0, -20.0f, 0}, {80, 4, VE_4, 0, 30.0f, 0},                                      /* CUTOFF -20, RESO +30 */
    {90, 5, 0, 0, 0, 0xB0 | 74 << 8 | 40 << 16}, {100, 5, 0, 0, 0, 0xE0 | 96 << 16},             /* CC 74, bend */
    {120, 3, VB_K5, 1, 0, 0}, {120, 3, VB_K5, 0, 0, 0}, {121, 3, VB_RIGHT, 1, 0, 0}, {121, 3, VB_RIGHT, 0, 0, 0},
    {125, 4, VE_2, 0, 40.0f, 0},                                                                  /* MULTI-FX LEVEL */
    {150, 2, 21, 0, 0, 0}, {160, 1, 5, 1, 0.8f, 0}, {160, 1, 5, 0, 0, 0}, {170, 2, 12, 1, 1.0f, 0},
    {200, 3, VB_SHIFT, 1, 0, 0}, {200, 4, VE_3, 0, -2.0f, 0}, {200, 3, VB_SHIFT, 0, 0, 0},     /* (MULTI-FX TYPE: x1) */
    {260, 6, VIBE_PEDAL_SWITCH, 1, 0, 0}, {262, 2, 12, 0, 0, 0}, {262, 2, 16, 0, 0, 0}, {300, 6, VIBE_PEDAL_SWITCH, 0, 0, 0},
    {330, 3, VB_F4, 1, 0, 0}, {330, 3, VB_F4, 0, 0, 0}, {340, 2, 24, 1, 0.7f, 0}, {400, 2, 24, 0, 0, 0},
    {420, 3, VB_K4, 1, 0, 0}, {420, 3, VB_K4, 0, 0, 0}, {421, 3, VB_K4, 1, 0, 0}, {421, 3, VB_K4, 0, 0, 0},   /* ARP on */
    {430, 2, 7, 1, 0.9f, 0}, {430, 2, 11, 1, 0.9f, 0}, {700, 2, 7, 0, 0, 0}, {700, 2, 11, 0, 0, 0},
};
#define RUN_N (sizeof RUN / sizeof RUN[0])
#define RUN_SECONDS 4.0

static void run_hash(uint32_t rate)
{
    uint32_t b, k = 0, i, h = 2166136261u, nb = (uint32_t)(RUN_SECONDS * rate / 128.0 + 0.999999);
    static float l[128], r[128];
    vibe_on_init((float)rate);
    for (b = 0; b < nb; b++) {
        for (; k < RUN_N && (uint32_t)RUN[k].b == b; k++) ev(RUN[k].type, RUN[k].id, RUN[k].down, RUN[k].value, RUN[k].midi);
        vibe_on_audio(0, l, r, 128); stub_frames += 128;
        for (i = 0; i < 128u; i++) {
            union { float f; uint32_t u; } a, c;
            a.f = l[i]; c.f = r[i];
            h = (h ^ a.u) * 16777619u; h = (h ^ c.u) * 16777619u;
        }
    }
    printf("ui_test %u Hz: out_hash %08x\n", rate, h);
}

static void run_script(uint32_t rate, const char *file)   /* the same run for wasmtest (t = block x 128 / rate) */
{
    FILE *f = fopen(file, "w");
    uint32_t k;
    fprintf(f, "{\"seconds\": %.1f, \"steps\": [", RUN_SECONDS);
    for (k = 0; k < RUN_N; k++)
        fprintf(f, "%s{\"t\": %.17g, \"ev\": [%d, %d, %d, %.9g, %d]}", k ? ", " : "", (double)RUN[k].b * 128.0 / rate,
                RUN[k].type, RUN[k].id, RUN[k].down, (double)RUN[k].value, RUN[k].midi);
    fprintf(f, "]}\n");
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "check")) { check(); return fails != 0; }
    if (argc >= 3 && !strcmp(argv[1], "hash")) { run_hash((uint32_t)atoi(argv[2])); return 0; }
    if (argc >= 4 && !strcmp(argv[1], "script")) { run_script((uint32_t)atoi(argv[2]), argv[3]); return 0; }
    printf("ui_test check | hash <rate> | script <rate> <file>\n");
    return 2;
}
