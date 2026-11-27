/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM, P3's tests (Kris 24 Nov 2026, claude/jt80x0-vibe-p3-pages-proposal-2026-11-24.md): main.c itself,
 * natively, against stand-ins for the SDK (stubs.h).
 *   ui_test check            every page, every encoder, end to end and back, in each PART on a layered program: the
 *                            values stay in their ranges, their texts fit a column, and after each block the engine
 *                            holds exactly what the edit copy holds. Then the edits that must land in one place
 *                            (cutoff, an FX byte under LOWER, BOTH), MIDI (CC 74, bend, wheel, pressure, program
 *                            change), sustain, F3 / F4, ARP on / off.
 *   ui_test daw              P4: the parameters – at most 255 (the event's id is a byte), ids unique and of a-z 0-9 _,
 *                            every id in vibe/test/param_ids.txt still there (DAW automation hangs on them); each one
 *                            set from the DAW to its ends and middle lands in the engine; a knob, a CC and a program
 *                            change are reported back; a DAW change is not echoed
 *   ui_test store            P5: nothing written until a change rests 1.5 s (then once), VIBE_EV_SAVE at once, the '*',
 *                            SAVE WRITE (with a new name) / SWAP / FACTORY, a restart that gives back the bank, the
 *                            settings and the sound left (unsaved edits too), bad files ignored, odd values held
 *   ui_test seq              P6a: the sequencer – REC (armed, running, a note on the grid), the next pass plays it, MUTE
 *                            (pad 14), LENGTH, SHIFT + pad 1 saves a clip, REC held: CLEAR (NOTES), the question gone
 *                            on another button or in 3 s, pad 1 loads at a bar, the automation recorded, played into
 *                            the engine and ED, put back by STOP; the host leading (from its position, held to it, a
 *                            jump, its stop, a count-in, PLAY its own); tracks.jt once it rests, a restart that gives
 *                            the song back and writes nothing; a bad record ignored
 *   ui_test seqhash <rate>   the sequencer recorded and then led by a host that starts mid-song and loops (wasmtest's
 *   ui_test seqscript <r> f  "daw"): the .wasm must print the same out_hash
 *   ui_test ids              the ids, one a line (for param_ids.txt, when one is added)
 *   ui_test hash <rate>      a played and edited run (the list below), wasmtest's out_hash: the .wasm must print the
 *   ui_test script <rate> f  same for the script this writes (vibe/test/run.sh) */
#include "stubs.h"
#include <math.h>
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
    for (i = 0; i < MV_SQ_N; i++) if (eng.seqb[i] != ED.sq[i]) return 0;
    return eng.nv == ED.nv && jt_own_bpm == ED.tempo && eng.pvelo == ED.pvelo;
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
                if (IS_PATCH(q_) && MV_PARAM_AT[q_] < MV_NPARAMS) {   /* (a byte with a parameter: every page's is) */
                    const mv_param_t *d = &MV_PARAMS[MV_PARAM_AT[q_]];
                    int both, v = mv_param_get(&ED.part[ed_part(q_, &both)], MV_PARAM_AT[q_]);
                    if (v < d->min || v > d->max) { CHECK(0, "%s byte %u: %d outside %u..%u", PAGES[p].title, q_, v, d->min, d->max); break; }
                }
                if (!engine_is_ed()) { CHECK(0, "%s / %s: the engine differs from the edit copy (turn %u)", PAGES[p].title, p_label(q_), k); break; }
            }
        }
    }
}

static int d_find(const char *id)              /* a parameter's index by its id, -1 none */
{
    uint32_t k;
    for (k = 0; k < d_n; k++) if (!strcmp(DP[k].id, id)) return (int)k;
    return -1;
}

static void daw(const char *golden)
{
    uint32_t k, j;
    char line[64];
    FILE *f;
    int i, n_gold = 0;
    vibe_on_init(44100.0f);
    block();
    printf("daw: %u parameters\n", d_n);
    CHECK(d_n >= 100u && d_n <= 255u, "%u parameters (the event's id is a byte: 255 at most)", d_n);
    for (k = 0; k < d_n; k++) {
        const char *c;
        for (c = DP[k].id; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) break;
        CHECK(*DP[k].id && !*c, "id \"%s\": a-z 0-9 _ only", DP[k].id);
        for (j = 0; j < k; j++) CHECK(strcmp(DP[j].id, DP[k].id), "id \"%s\" twice", DP[k].id);
        CHECK(DP[k].steps == (int)(DP[k].max - DP[k].min) + 1, "%s: steps", DP[k].id);
        CHECK(d_get(k) >= DP[k].min && d_get(k) <= DP[k].max, "%s: %g within %g..%g", DP[k].id, (double)d_get(k), (double)DP[k].min, (double)DP[k].max);
        CHECK(stub_pv[k] == d_get(k), "%s: the host was told it at start", DP[k].id);
    }
    f = fopen(golden, "r");
    CHECK(f != 0, "%s", golden);
    while (f && fgets(line, sizeof line, f)) {  /* every published id still there */
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        n_gold++;
        CHECK(d_find(line) >= 0, "the published id \"%s\" is gone (DAW automation hangs on it)", line);
    }
    if (f) fclose(f);
    printf("daw: %d published ids all present\n", n_gold);
    /* each one from the DAW: its ends and its middle, into the engine, and not echoed */
    for (k = 0; k < d_n; k++) {
        float v[3];
        v[0] = DP[k].min; v[1] = DP[k].max; v[2] = (float)(int)((DP[k].min + DP[k].max) / 2.0f);
        for (i = 0; i < 3; i++) {
            uint32_t echo = stub_pset[k];
            ev(VIBE_EV_PARAM, (int)k, 0, v[i], 0);
            block();
            CHECK(d_get(k) == v[i], "%s set to %g: reads %g", DP[k].id, (double)v[i], (double)d_get(k));
            CHECK(engine_is_ed(), "%s set to %g: the engine differs", DP[k].id, (double)v[i]);
            CHECK(stub_pset[k] == echo, "%s: a DAW change echoed back", DP[k].id);
        }
    }
    /* changes made here go back: a knob, a CC, a program change */
    prog_select(0); vibe_on_event(&(vibe_event){0}); block();
    {
        int cu = d_find("u_cutoff"), pr = d_find("program"), rl = d_find("rv_level");
        uint32_t c0;
        CHECK(cu >= 0 && pr >= 0 && rl >= 0, "u_cutoff, program, rv_level");
        page_go(group_first(G_SOUND) + 4u); panel = PANEL_UPPER;
        c0 = stub_pset[cu];
        ev(VIBE_EV_ENCODER, VE_3, 0, -4.0f, 0);
        CHECK(stub_pset[cu] == c0 + 1u && stub_pv[cu] == d_get((uint32_t)cu), "knob: u_cutoff reported (%g)", (double)stub_pv[cu]);
        ev(VIBE_EV_MIDI, 0, 0, 0, 0xB0 | 74 << 8 | 100 << 16);
        CHECK(stub_pv[cu] == d_get((uint32_t)cu), "CC 74: u_cutoff reported (%g)", (double)stub_pv[cu]);
        page_go(group_first(G_PLAY));
        {
            float r0 = d_get((uint32_t)rl);
            ev(VIBE_EV_ENCODER, VE_4, 0, 9.0f, 0);
            CHECK(stub_pv[rl] == r0 + 9.0f && d_get((uint32_t)rl) == r0 + 9.0f, "PLAY knob 4: rv_level %g -> reported %g", (double)r0, (double)stub_pv[rl]);
        }
        press(VB_F4);
        CHECK(stub_pv[pr] == 1.0f && stub_pv[cu] == (float)MV_FACTORY[1][41], "F4: program and its cutoff reported");
        ev(VIBE_EV_PARAM, pr, 0, 12.0f, 0); block();
        CHECK(prog == 12u && !memcmp(eng.patch.b, MV_FACTORY[12], MV_PATCH_BYTES), "program from the DAW: 12");
        CHECK(stub_pv[cu] == (float)MV_FACTORY[12][41], "... and its cutoff reported");
    }
    printf("ui_test daw: %d of %d checks failed\n", fails, checks);
}

static void rest(uint32_t ms)                   /* time passes, nothing touched (the draw runs, as the host's does) */
{
    uint32_t k;
    for (k = 0; k < ms * 44100u / 1000u / 64u; k++) block();
    vibe_on_draw(0);
}
static int file_is(const char *name, const uint8_t *d, uint32_t n)   /* a stored file holds header + these bytes */
{
    int i = stub_find(name);
    return i >= 0 && stub_f[i].n == (int)(8u + n) && !memcmp(stub_f[i].d, "JT80", 4) && !memcmp(stub_f[i].d + 8, d, n);
}

static void store(void)
{
    uint8_t want[JT_PROG_BYTES], a[JT_PROG_BYTES], b[JT_PROG_BYTES];
    uint32_t w0, k, to, rv;
    stub_store_clear();
    vibe_on_init(44100.0f); block();
    rest(2000);
    CHECK(stub_writes == 0u, "a fresh start writes nothing (%u)", stub_writes);
    CHECK(!jt_edited(prog), "factory program, not edited");
    /* an edit: nothing until it rests, then once; the '*' */
    press(VB_K1); page_go(group_first(G_SOUND) + 4u);
    ev(VIBE_EV_ENCODER, VE_3, 0, -10.0f, 0); block();
    CHECK(jt_edited(prog), "edited: the '*'");
    for (k = 0; k < 10u; k++) { rest(500); ev(VIBE_EV_ENCODER, VE_4, 0, 1.0f, 0); }   /* turned every 0.5 s */
    CHECK(stub_writes == 0u, "a knob being turned writes nothing (%u)", stub_writes);
    w0 = stub_writes; rest(2000);
    jt_prog_to(want);
    CHECK(stub_writes > w0 && file_is("edit.jt", want, JT_PROG_BYTES), "rested 1.5 s: edit.jt written");
    w0 = stub_writes; rest(3000);
    CHECK(stub_writes == w0, "and not again (%u)", stub_writes - w0);
    /* VIBE_EV_SAVE: at once */
    ev(VIBE_EV_ENCODER, VE_4, 0, 2.0f, 0);
    w0 = stub_writes; ev(VIBE_EV_SAVE, 0, 0, 0, 0);
    jt_prog_to(want);
    CHECK(stub_writes > w0 && file_is("edit.jt", want, JT_PROG_BYTES), "VIBE_EV_SAVE: written at once");
    /* the reverb (a setting), then SAVE: WRITE to A-6 with a new first letter */
    page_go(group_first(G_PLAY)); ev(VIBE_EV_ENCODER, VE_4, 0, 33.0f, 0); rv = ED.rv[MV_RV_LEVEL];
    page_go(group_first(G_SOUND) + 4u);
    press(VB_F5);
    CHECK(PAGES[pg].group == G_SAVE && sv_to == prog && sv_mode == SV_WRITE, "F5: SAVE, TO = the program, WRITE");
    ev(VIBE_EV_ENCODER, VE_1, 0, 5.0f, 0);      /* TO: A-6 */
    ev(VIBE_EV_ENCODER, VE_3, 0, 1.0f, 0);      /* the first letter + 1 */
    to = sv_to;
    CHECK(to == 5u, "TO A-6 (%u)", to);
    press(VB_F5); block();
    jt_prog_to(want);
    CHECK(prog == to && !jt_edited(prog) && !memcmp(BANK[to], want, JT_PROG_BYTES), "WRITE: A-6 holds the sound and plays");
    CHECK(want[0] == (uint8_t)(MV_FACTORY[0][0] + 1u), "the new name's first letter (%c)", want[0]);
    CHECK(file_is("bank/p06.jt", want, JT_PROG_BYTES), "bank/p06.jt written");
    CHECK(!strcmp(PAGES[pg].title, "FILTER 1/2"), "back to the page SAVE was opened from (%s)", PAGES[pg].title);
    /* an edit left unsaved, kept by edit.jt; PATCH VELO on (GLOBAL, encoder 4); then a restart */
    ev(VIBE_EV_ENCODER, VE_4, 0, -7.0f, 0);
    { uint32_t was = pg; press(VB_F6); ev(VIBE_EV_ENCODER, VE_4, 0, 1.0f, 0); CHECK(ED.pvelo == 1u, "GLOBAL 4: PATCH VELO on"); page_go(was); }
    rest(2000);
    jt_prog_to(want);
    vibe_on_init(44100.0f); block();
    jt_prog_to(a);
    CHECK(prog == to, "restart: the program (A-6) again (%u)", prog);
    CHECK(!memcmp(a, want, JT_PROG_BYTES) && jt_edited(prog), "restart: the sound left, unsaved edit and all, '*'");
    CHECK(ED.rv[MV_RV_LEVEL] == rv, "restart: the reverb (%u)", ED.rv[MV_RV_LEVEL]);
    CHECK(ED.pvelo == 1u && eng.pvelo == 1u, "restart: PATCH VELO on");
    CHECK(engine_is_ed(), "restart: the engine holds it");
    CHECK(BANK[to][0] == (uint8_t)(MV_FACTORY[0][0] + 1u), "restart: A-6 as saved");
    /* SWAP: A-6 (playing, edited) with B-1 */
    jt_prog_to(a);
    for (k = 0; k < JT_PROG_BYTES; k++) b[k] = BANK[16][k];
    press(VB_F5); ev(VIBE_EV_ENCODER, VE_1, 0, 11.0f, 0); ev(VIBE_EV_ENCODER, VE_4, 0, 1.0f, 0);
    CHECK(sv_to == 16u && sv_mode == SV_SWAP, "TO B-1, SWAP");
    press(VB_F5); block();
    CHECK(!memcmp(BANK[16], a, JT_PROG_BYTES) && !memcmp(BANK[5], b, JT_PROG_BYTES) && prog == 16u, "SWAP: B-1 has the sound, A-6 B-1's");
    CHECK(file_is("bank/p17.jt", a, JT_PROG_BYTES) && file_is("bank/p06.jt", b, JT_PROG_BYTES), "SWAP: both files");
    /* FACTORY: B-1 (playing) back to the factory's */
    press(VB_F5); ev(VIBE_EV_ENCODER, VE_4, 0, 2.0f, 0);
    CHECK(sv_mode == SV_FACTORY && sv_to == 16u, "FACTORY B-1");
    press(VB_F5); block();
    CHECK(stub_find("bank/p17.jt") < 0, "FACTORY: its file gone");
    CHECK(!memcmp(eng.patch.b, MV_FACTORY[16], MV_PATCH_BYTES) && !jt_edited(16u), "FACTORY: playing it, the factory sound");
    /* bad files: ignored */
    {
        static const uint8_t junk[20] = "not a JT80 file....";
        uint8_t odd[8u + JT_SET_BYTES];
        (void)vibe_store_write("bank/p03.jt", junk, 20);
        memset(odd, 0, sizeof odd); memcpy(odd, "JT80", 4); odd[4] = JT_STORE_VER; odd[5] = SK_SET; odd[6] = JT_SET_BYTES;
        odd[8 + 16] = 255; odd[8 + 17] = 0; odd[8 + 18] = 99; odd[8 + 19] = 70; odd[8 + 7] = 200;   /* tempo, voices, channel, program, arp on */
        (void)vibe_store_write("settings.jt", odd, (int)sizeof odd);
        (void)vibe_store_write("edit.jt", junk, 20);
        vibe_on_init(44100.0f); block();
        CHECK(stub_find("bank/p03.jt") >= 0, "bad bank file: still there, no crash");
        {
            uint8_t f[JT_PROG_BYTES];
            for (k = 0; k < MV_PATCH_BYTES; k++) { f[k] = MV_FACTORY[2][k]; f[MV_PATCH_BYTES + k] = MV_FACTORY_LO[2][k]; }
            for (k = 0; k < MV_PF_N; k++) f[2u * MV_PATCH_BYTES + k] = MV_FACTORY_PERF[2][k];
            CHECK(!memcmp(BANK[2], f, JT_PROG_BYTES), "bad bank file: A-3 the factory's");
        }
        CHECK(ED.tempo == 240u && ED.nv == 1u && ED.midich == 16u && prog == 70u % MV_NPROG && ED.arp[MV_ARP_ON] == 1u,
              "odd settings held in range (tempo %u voices %u ch %u prog %u)", ED.tempo, ED.nv, ED.midich, prog);
        CHECK(!jt_edited(prog) && engine_is_ed(), "a bad edit.jt: the program as stored");
    }
    printf("ui_test store: %d of %d checks failed\n", fails, checks);
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
static const struct { int b, type, id, down; float value; int midi; const char *pid; } RUN[] = {
    {10, 2, 21, 1, 0.8f, 0, 0}, {10, 2, 16, 1, 0.6f, 0, 0},
    {40, 3, VB_K1, 1, 0, 0, 0}, {40, 3, VB_K1, 0, 0, 0, 0}, {45, 4, VE_1, 0, 1.0f, 0, 0},                 /* OSC 1: SAW -> TRI */
    {60, 3, VB_RIGHT, 1, 0, 0, 0}, {60, 3, VB_RIGHT, 0, 0, 0, 0}, {61, 3, VB_RIGHT, 1, 0, 0, 0}, {61, 3, VB_RIGHT, 0, 0, 0, 0},
    {62, 3, VB_RIGHT, 1, 0, 0, 0}, {62, 3, VB_RIGHT, 0, 0, 0, 0}, {63, 3, VB_RIGHT, 1, 0, 0, 0}, {63, 3, VB_RIGHT, 0, 0, 0, 0},
    {70, 4, VE_3, 0, -20.0f, 0, 0}, {80, 4, VE_4, 0, 30.0f, 0, 0},                                      /* CUTOFF -20, RESO +30 */
    {90, 5, 0, 0, 0, 0xB0 | 74 << 8 | 40 << 16, 0}, {100, 5, 0, 0, 0, 0xE0 | 96 << 16, 0},             /* CC 74, bend */
    {120, 3, VB_K5, 1, 0, 0, 0}, {120, 3, VB_K5, 0, 0, 0, 0}, {121, 3, VB_RIGHT, 1, 0, 0, 0}, {121, 3, VB_RIGHT, 0, 0, 0, 0},
    {125, 4, VE_2, 0, 40.0f, 0, 0},                                                                  /* MULTI-FX LEVEL */
    {150, 2, 21, 0, 0, 0, 0}, {160, 1, 5, 1, 0.8f, 0, 0}, {160, 1, 5, 0, 0, 0, 0}, {170, 2, 12, 1, 1.0f, 0, 0},
    {200, 3, VB_SHIFT, 1, 0, 0, 0}, {200, 4, VE_3, 0, -2.0f, 0, 0}, {200, 3, VB_SHIFT, 0, 0, 0, 0},     /* (MULTI-FX TYPE: x1) */
    {260, 6, VIBE_PEDAL_SWITCH, 1, 0, 0, 0}, {262, 2, 12, 0, 0, 0, 0}, {262, 2, 16, 0, 0, 0, 0}, {300, 6, VIBE_PEDAL_SWITCH, 0, 0, 0, 0},
    {330, 3, VB_F4, 1, 0, 0, 0}, {330, 3, VB_F4, 0, 0, 0, 0}, {340, 2, 24, 1, 0.7f, 0, 0}, {400, 2, 24, 0, 0, 0, 0},
    {420, 3, VB_K4, 1, 0, 0, 0}, {420, 3, VB_K4, 0, 0, 0, 0}, {421, 3, VB_K4, 1, 0, 0, 0}, {421, 3, VB_K4, 0, 0, 0, 0},   /* ARP on */
    {430, 2, 7, 1, 0.9f, 0, 0}, {430, 2, 11, 1, 0.9f, 0, 0},
    {500, 10, 0, 0, 30.0f, 0, "u_cutoff"}, {560, 10, 0, 0, 90.0f, 0, "rv_level"}, {600, 10, 0, 0, 5.0f, 0, "arp_rate"},   /* P4 */
    {700, 2, 7, 0, 0, 0, 0}, {700, 2, 11, 0, 0, 0, 0},
    {720, 10, 0, 0, 20.0f, 0, "program"}, {730, 2, 14, 1, 0.8f, 0, 0}, {800, 2, 14, 0, 0, 0, 0},
};   /* (in block order: run_hash walks it once) */
#define RUN_N (sizeof RUN / sizeof RUN[0])
#define RUN_SECONDS 4.0

static void run_hash(uint32_t rate)
{
    {   /* RUN must be in block order (wasmtest sorts by time; this loop walks it once) */
        uint32_t j;
        for (j = 1; j < RUN_N; j++) if (RUN[j].b < RUN[j - 1u].b) { printf("ui_test: RUN out of order at %u\n", j); exit(2); }
    }
    uint32_t b, k = 0, i, h = 2166136261u, nb = (uint32_t)(RUN_SECONDS * rate / 128.0 + 0.999999);
    static float l[128], r[128];
    vibe_on_init((float)rate);
    for (b = 0; b < nb; b++) {
        for (; k < RUN_N && (uint32_t)RUN[k].b == b; k++)
            ev(RUN[k].type, RUN[k].pid ? d_find(RUN[k].pid) : RUN[k].id, RUN[k].down, RUN[k].value, RUN[k].midi);
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
    vibe_on_init((float)rate);                  /* (the parameters' indices: by id) */
    fprintf(f, "{\"seconds\": %.1f, \"steps\": [", RUN_SECONDS);
    for (k = 0; k < RUN_N; k++)
        fprintf(f, "%s{\"t\": %.17g, \"ev\": [%d, %d, %d, %.9g, %d]}", k ? ", " : "", (double)RUN[k].b * 128.0 / rate,
                RUN[k].type, RUN[k].pid ? d_find(RUN[k].pid) : RUN[k].id, RUN[k].down, (double)RUN[k].value, RUN[k].midi);
    fprintf(f, "]}\n");
    fclose(f);
}

/* ---- P6a: the sequencer (ui_test seq) ------------------------------------------------------------------------------ */
static void blocks_ms(uint32_t ms) { uint32_t k; for (k = 0; k < ms * 44100u / 1000u / 64u; k++) block(); }
static int has_point(uint32_t p)                /* track p holds an automation point */
{
    uint32_t i;
    for (i = 0; i < eng.seq.t[p].n; i++) if (eng.seq.t[p].ev[i].vel & MV_SEQ_PT) return 1;
    return 0;
}
static void daw_at(double beat, int playing)    /* the host's position for the next block (as wasmtest writes it) */
{
    tpi.has = 1; tpi.playing = playing; tpi.bpm = 120.0f; tpi.sig_num = 4; tpi.sig_den = 4;
    tpi.beat = beat; tpi.bar_start = (double)(int)(beat / 4.0) * 4.0;
}
static void daw_blocks(double *beat, uint32_t n)   /* n blocks of 64 frames, the host playing on from *beat */
{
    uint32_t k;
    for (k = 0; k < n; k++) { daw_at(*beat, 1); block(); *beat += 64.0 * 120.0 / 60.0 / 44100.0; }
}
static int near(uint32_t a, uint32_t b, uint32_t d) { return a + d >= b && b + d >= a; }
static void rest_drawn(uint32_t ms) { uint32_t k; for (k = 0; k < ms / 50u; k++) rest(50); }   /* (drawn as a host does) */

static void seq(void)
{
    uint32_t n0, i, c0, c1, moved;
    mv_seq_ev_t keep[MV_SEQ_NEV];
    uint8_t keep_sq[MV_SQ_N];
    stub_store_clear();
    memset(&tpi, 0, sizeof tpi);
    vibe_on_init(44100.0f); block();
    panel = PANEL_UPPER;
    press(VB_K6);
    CHECK(PAGES[pg].group == G_SEQ && PAGES[pg].p[0] == P_SQ, "K6: SEQ 1/3");
    CHECK(engine_is_ed() && eng.seqb[MV_SQ_QUANT] == MV_SQQ_16, "the defaults: 1/16, the engine as ED");
    /* REC: armed and running; a note recorded on the grid; REC again: disarmed */
    press(VB_REC); block();
    CHECK(eng.seq_live && (eng.seq_armed & 1u) && sq_pub_arm == 1u && sq_pub_run, "REC: UPPER armed, the transport runs");
    blocks_ms(210);
    ev(VIBE_EV_KEY, 12, 1, 0.8f, 0); blocks_ms(300); ev(VIBE_EV_KEY, 12, 0, 0, 0); block();
    CHECK(eng.seq.t[0].n == 2u && eng.seq.t[0].ev[0].note == 60u && eng.seq.t[0].ev[0].vel, "a note recorded (%u events)", eng.seq.t[0].n);
    CHECK(eng.seq.t[0].ev[0].tick % 24u == 0u, "QUANT 1/16: on the grid (tick %u)", eng.seq.t[0].ev[0].tick);
    press(VB_REC); block();
    CHECK(!(eng.seq_armed & 1u) && eng.seq_live, "REC again: disarmed, still running");
    rest(10);
    CHECK(sq_view[0].n == 2u, "the display's copy: 2 events (%u)", sq_view[0].n);
    /* the next pass plays it, nothing held */
    {
        uint32_t heard = 0, k;
        while (eng.seq.t[0].tick > 0u && eng.seq.t[0].tick < 2u * MV_SEQ_BAR - 1u) block();   /* to the loop's end */
        for (k = 0; k < 600u * 44100u / 1000u / 64u; k++) { block(); heard |= mv_engine_active(&eng) > 0u; }
        CHECK(heard, "the next pass plays the note");
    }
    /* MUTE (pad 14: UPPER), and back */
    ev(VIBE_EV_PAD, 13, 1, 1.0f, 0); ev(VIBE_EV_PAD, 13, 0, 0, 0); block();
    CHECK(ED.sq[MV_SQ_MUTEUP] == 1u && eng.seqb[MV_SQ_MUTEUP] == 1u, "pad 14: UPPER muted");
    ev(VIBE_EV_PAD, 13, 1, 1.0f, 0); ev(VIBE_EV_PAD, 13, 0, 0, 0); block();
    CHECK(eng.seqb[MV_SQ_MUTEUP] == 0u && engine_is_ed(), "pad 14 again: not muted");
    /* LENGTH on the page; the engine follows */
    ev(VIBE_EV_ENCODER, VE_1, 0, 2.0f, 0); block();
    CHECK(ED.sq[MV_SQ_LENUP] == 4u && eng.seqb[MV_SQ_LENUP] == 4u, "LENGTH +2: 4 bars (%u)", eng.seqb[MV_SQ_LENUP]);
    ev(VIBE_EV_ENCODER, VE_1, 0, -2.0f, 0); block();
    /* SHIFT + pad 1: saved as clip 1 */
    ev(VIBE_EV_BUTTON, VB_SHIFT, 1, 0, 0); ev(VIBE_EV_PAD, 0, 1, 1.0f, 0); ev(VIBE_EV_PAD, 0, 0, 0, 0); ev(VIBE_EV_BUTTON, VB_SHIFT, 0, 0, 0);
    CHECK(cl_dir[0].used && cl_dir[0].n == 2u && cl_dir[0].len == 2u && stub_find("clips/c01.jt") >= 0, "SHIFT + pad 1: clip 1 saved");
    CHECK(!memcmp(cl_dir[0].name, "CLIP01", 6), "... named CLIP01");
    /* REC held 1 s: CLEAR? a knob: NOTES; REC: yes */
    ev(VIBE_EV_BUTTON, VB_REC, 1, 0, 0); rest(1100); ev(VIBE_EV_BUTTON, VB_REC, 0, 0, 0);
    CHECK(sq_ask && sq_ask_mask == 1u && sq_ask_mode == 0u, "REC held 1 s: CLEAR UPPER? ALL");
    ev(VIBE_EV_ENCODER, VE_2, 0, 1.0f, 0);
    CHECK(sq_ask_mode == 1u, "a knob: NOTES");
    press(VB_REC); block();
    CHECK(!sq_ask && eng.seq.t[0].n == 0u && !(eng.seq_armed & 1u), "REC: cleared, not armed");
    ev(VIBE_EV_BUTTON, VB_REC, 1, 0, 0); rest(1100); ev(VIBE_EV_BUTTON, VB_REC, 0, 0, 0);
    press(VB_F4); prog_select(0); block();
    CHECK(!sq_ask && eng.seq.t[0].n == 0u, "another button: the question gone");
    ev(VIBE_EV_BUTTON, VB_REC, 1, 0, 0); rest(1100); ev(VIBE_EV_BUTTON, VB_REC, 0, 0, 0);
    CHECK(sq_ask, "REC held again: asked");
    rest(3100);
    CHECK(!sq_ask && !(eng.seq_armed & 1u), "... unanswered it goes in 3 s (nothing armed)");
    /* pad 1: clip 1 back into UPPER, at its next bar (running) */
    sq_pad(0, 0); block();
    {
        uint32_t k, at = ~0u;
        for (k = 0; k < 2200u * 44100u / 1000u / 64u && at == ~0u; k++) { block(); if (eng.seq.t[0].n) at = eng.seq.t[0].tick; }
        CHECK(eng.seq.t[0].n == 2u && eng.seq.t[0].ev[0].note == 60u, "pad 1: clip 1 loaded (%u events)", eng.seq.t[0].n);
        CHECK(at < 4u || (at >= MV_SEQ_BAR && at < MV_SEQ_BAR + 4u), "... at a bar (tick %u)", at);
        CHECK(sq_ld_req[0] == sq_ld_ack[0], "... and the buffer handed back");
    }
    /* automation: STOP; REC (from bar 1); CUTOFF turned while armed; the next pass plays it into the engine and ED;
     * STOP puts it back */
    press(VB_STOP); block();
    CHECK(!eng.seq_live && !sq_pub_run && !eng.seq_arm, "STOP: stopped, disarmed");
    page_go(group_first(G_SOUND) + 4u);         /* FILTER 1/2: CUTOFF on 3 */
    c0 = ED.part[0].b[41];
    press(VB_REC); block();
    for (i = 0; i < 12u; i++) { ev(VIBE_EV_ENCODER, VE_3, 0, -3.0f, 0); blocks_ms(60); }
    c1 = ED.part[0].b[41];
    press(VB_REC); block();
    CHECK(has_point(0) && c1 != c0, "CUTOFF turned while armed: points recorded (%u -> %u)", c0, c1);
    for (i = 0, moved = 0; i < 4500u * 44100u / 1000u / 64u; i++) {
        block();
        if (i % 7u == 0u) { vibe_on_draw(0); if (!engine_is_ed()) { CHECK(0, "playing the points: the engine differs from ED"); break; } }
        moved |= eng.patch.b[41] != c1;
    }
    CHECK(moved, "the next pass plays the points (CUTOFF moved from %u)", c1);
    press(VB_STOP); block(); vibe_on_draw(0);
    CHECK(eng.patch.b[41] == c0 && ED.part[0].b[41] == c0 && engine_is_ed(), "STOP: CUTOFF back to %u (%u), ED too", c0, eng.patch.b[41]);
    /* the host leads: from its position, held to it, a jump, its stop; PLAY is its */
    {
        double beat = 10.0;
        daw_blocks(&beat, 1);
        CHECK(eng.seq_live && eng.seq_ext && near(eng.seq.ticks, 960u, 4u), "the host plays from beat 10: tick %u (960)", eng.seq.ticks);
        CHECK(eng.seq.t[0].tick == eng.seq.ticks % (2u * MV_SEQ_BAR), "... UPPER at the song mod its LENGTH (%u)", eng.seq.t[0].tick);
        daw_blocks(&beat, 689);                 /* ~1 s: 2 quarters */
        CHECK(near(eng.seq.ticks, (uint32_t)(beat * 96.0), 4u), "1 s on, held to the host: %u (%u)", eng.seq.ticks, (uint32_t)(beat * 96.0));
        press(VB_PLAY); daw_blocks(&beat, 1);
        CHECK(eng.seq_live, "PLAY while the host leads: nothing");
        beat = 2.0; daw_blocks(&beat, 1);
        CHECK(near(eng.seq.ticks, 192u, 4u), "a jump to beat 2: tick %u (192)", eng.seq.ticks);
        daw_at(beat, 0); block(); block();
        CHECK(!eng.seq_live && !sq_pub_lead, "the host stops: stopped");
        daw_at(-1.0, 1); block();
        CHECK(!eng.seq_live, "a count-in (beat -1): waits");
        memset(&tpi, 0, sizeof tpi);
    }
    /* the song kept: tracks.jt once it rests, given back by a restart */
    press(VB_REC); blocks_ms(100); ev(VIBE_EV_KEY, 7, 1, 0.7f, 0); blocks_ms(150); ev(VIBE_EV_KEY, 7, 0, 0, 0); blocks_ms(50);
    press(VB_STOP); block();
    ED.sq[MV_SQ_QUANT] = MV_SQQ_8; ED.sq[MV_SQ_LENLO] = 3; vibe_on_event(&(vibe_event){0}); block();
    i = stub_writes; rest_drawn(1400);
    CHECK(stub_writes == i, "tracks.jt: not before it rests 1.5 s");
    rest_drawn(400);
    CHECK(stub_writes > i && stub_find("tracks.jt") >= 0, "tracks.jt written once things rest");
    n0 = eng.seq.t[0].n;
    for (i = 0; i < n0; i++) keep[i] = eng.seq.t[0].ev[i];
    for (i = 0; i < MV_SQ_N; i++) keep_sq[i] = eng.seqb[i];
    i = stub_writes; rest_drawn(3000);
    CHECK(stub_writes == i, "and not again (%u)", stub_writes - i);
    vibe_on_init(44100.0f); block(); block();
    CHECK(eng.seq.t[0].n == n0 && n0 >= 2u, "restarted: UPPER's %u events back (%u)", n0, eng.seq.t[0].n);
    for (i = 0; i < n0 && i < eng.seq.t[0].n; i++)
        if (memcmp(&keep[i], &eng.seq.t[0].ev[i], sizeof keep[i])) { CHECK(0, "restarted: event %u differs", i); break; }
    CHECK(!memcmp(keep_sq, (const void *)eng.seqb, MV_SQ_N) && engine_is_ed(), "restarted: the settings back (QUANT %u, LENGTH L %u)", eng.seqb[MV_SQ_QUANT], eng.seqb[MV_SQ_LENLO]);
    CHECK(cl_dir[0].used && cl_dir[0].n == 2u, "restarted: clip 1 there");
    i = stub_writes; rest_drawn(3000);
    CHECK(stub_writes == i, "a restart writes nothing (%u)", stub_writes - i);
    /* odd files: ignored */
    {
        int k = stub_find("tracks.jt");
        stub_f[k].d[8 + 8 + 2] = 99;            /* UPPER's LENGTH: 99 bars */
        vibe_on_init(44100.0f); block();
        CHECK(eng.seq.t[0].n == 0u, "a bad record: an empty track");
    }
    printf("ui_test seq: %d of %d checks failed\n", fails, checks);
}

/* the sequencer played, recorded and led by a host (as wasmtest's "daw"): the same here and in the .wasm */
static const struct { int b, type, id, down; float value; int midi; } SRUN[] = {
    {10, 3, VB_K6, 1, 0, 0}, {10, 3, VB_K6, 0, 0, 0}, {20, 3, VB_REC, 1, 0, 0}, {20, 3, VB_REC, 0, 0, 0},
    {40, 2, 12, 1, 0.8f, 0}, {70, 2, 12, 0, 0, 0}, {90, 2, 16, 1, 0.6f, 0}, {100, 2, 19, 1, 0.7f, 0}, {130, 2, 16, 0, 0, 0},
    {140, 2, 19, 0, 0, 0}, {150, 3, VB_K1, 1, 0, 0}, {150, 3, VB_K1, 0, 0, 0}, {151, 3, VB_RIGHT, 1, 0, 0}, {151, 3, VB_RIGHT, 0, 0, 0},
    {152, 3, VB_RIGHT, 1, 0, 0}, {152, 3, VB_RIGHT, 0, 0, 0}, {153, 3, VB_RIGHT, 1, 0, 0}, {153, 3, VB_RIGHT, 0, 0, 0},
    {154, 3, VB_RIGHT, 1, 0, 0}, {154, 3, VB_RIGHT, 0, 0, 0}, {160, 4, VE_3, 0, -6.0f, 0}, {180, 4, VE_3, 0, -6.0f, 0},
    {200, 4, VE_3, 0, -6.0f, 0}, {220, 4, VE_3, 0, 9.0f, 0}, {300, 3, VB_REC, 1, 0, 0}, {300, 3, VB_REC, 0, 0, 0},
    {900, 2, 7, 1, 0.9f, 0}, {960, 2, 7, 0, 0, 0},
};
#define SRUN_N (sizeof SRUN / sizeof SRUN[0])
#define SRUN_SECONDS 9.0
#define SRUN_AT 3.0                             /* the host plays from 3 s, at beat 6, looping 4..12 (wasmtest "daw") */
#define SRUN_START 6.0
static void srun_daw(uint32_t b, uint32_t rate)  /* wasmtest's dawWrite, to the bit */
{
    double t = (double)b * 128.0 / rate - SRUN_AT, beat = SRUN_START + (t > 0.0 ? t : 0.0) * 120.0 / 60.0;
    if (beat >= 12.0 && SRUN_START < 12.0) beat = 4.0 + fmod(beat - 12.0, 12.0 - 4.0);
    tpi.has = 1; tpi.playing = t >= 0.0; tpi.bpm = 120.0f; tpi.sig_num = 4; tpi.sig_den = 4;
    tpi.beat = beat; tpi.bar_start = floor(beat / 4.0 + 1e-9) * 4.0;
    tpi.loop = 1; tpi.loop_start = 4.0; tpi.loop_end = 12.0;
}
static void seq_hash(uint32_t rate)
{
    uint32_t b, k = 0, i, h = 2166136261u, nb = (uint32_t)(SRUN_SECONDS * rate / 128.0 + 0.999999), at = 0, wraps = 0, played = 0;
    uint32_t last = 0;
    static float l[128], r[128];
    int sent_play = 0;
    for (b = 1; b < SRUN_N; b++) if (SRUN[b].b < SRUN[b - 1u].b) { printf("ui_test: SRUN out of order at %u\n", b); exit(2); }
    stub_store_clear();
    memset(&tpi, 0, sizeof tpi);
    vibe_on_init((float)rate);
    ev(8, 1, 0, 120.0f, 0);                     /* (wasmtest's TRANSPORT tempo at 0: ignored, sent alike) */
    for (b = 0; b < nb; b++) {
        if (!sent_play && (double)b * 128.0 / rate >= SRUN_AT) { ev(8, 0, 1, 0, 0); sent_play = 1; }
        for (; k < SRUN_N && (uint32_t)SRUN[k].b == b; k++) ev(SRUN[k].type, SRUN[k].id, SRUN[k].down, SRUN[k].value, SRUN[k].midi);
        srun_daw(b, rate);
        vibe_on_audio(0, l, r, 128); stub_frames += 128;
        if (eng.seq_ext && !at) at = eng.seq.ticks;
        if (eng.seq_ext && eng.seq.ticks + 100u < last) wraps++;
        if (eng.seq_ext) last = eng.seq.ticks;
        played |= mv_engine_active(&eng) > 0u && b > 1100u;
        for (i = 0; i < 128u; i++) {
            union { float f; uint32_t u; } a, c;
            a.f = l[i]; c.f = r[i];
            h = (h ^ a.u) * 16777619u; h = (h ^ c.u) * 16777619u;
        }
    }
    printf("ui_test seq %u Hz: out_hash %08x (host from tick %u, %u loops, %u events, played %u)\n", rate, h, at, wraps,
           eng.seq.t[0].n, played);
}
static void seq_script(uint32_t rate, const char *file)
{
    FILE *f = fopen(file, "w");
    uint32_t k;
    fprintf(f, "{\"seconds\": %.1f, \"daw\": {\"bpm\": 120, \"at\": %.1f, \"start\": %.1f, \"loop\": [4, 12], \"sig\": [4, 4]}, \"steps\": [",
            SRUN_SECONDS, SRUN_AT, SRUN_START);
    for (k = 0; k < SRUN_N; k++)
        fprintf(f, "%s{\"t\": %.17g, \"ev\": [%d, %d, %d, %.9g, %d]}", k ? ", " : "", (double)SRUN[k].b * 128.0 / rate,
                SRUN[k].type, SRUN[k].id, SRUN[k].down, (double)SRUN[k].value, SRUN[k].midi);
    fprintf(f, "]}\n");
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "seq")) { seq(); return fails != 0; }
    if (argc >= 3 && !strcmp(argv[1], "seqhash")) { seq_hash((uint32_t)atoi(argv[2])); return 0; }
    if (argc >= 4 && !strcmp(argv[1], "seqscript")) { seq_script((uint32_t)atoi(argv[2]), argv[3]); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "check")) { check(); return fails != 0; }
    if (argc >= 3 && !strcmp(argv[1], "daw")) { daw(argv[2]); return fails != 0; }
    if (argc >= 2 && !strcmp(argv[1], "store")) { store(); return fails != 0; }
    if (argc >= 2 && !strcmp(argv[1], "ids")) { uint32_t k; vibe_on_init(44100.0f); for (k = 0; k < d_n; k++) printf("%s\n", DP[k].id); return 0; }
    if (argc >= 3 && !strcmp(argv[1], "hash")) { run_hash((uint32_t)atoi(argv[2])); return 0; }
    if (argc >= 4 && !strcmp(argv[1], "script")) { run_script((uint32_t)atoi(argv[2]), argv[3]); return 0; }
    printf("ui_test check | hash <rate> | script <rate> <file>\n");
    return 2;
}
