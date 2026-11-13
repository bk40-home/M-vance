/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* The simulator's tiles (sim/mv_tiles.c; Kris 13 Nov) tested natively: the real app and the real sim backend
 * (sim/mv_websim.c) built with cc against tests/websim_stub/emscripten.h. This file plays the browser: it runs at
 * each yield, pulls the audio (so the app's clock moves) and calls the wsim_ API as sim/pages.js does.
 *
 *   tests/run_websim_tiles.sh        (builds and runs it; exit 0 = every check passed)
 *
 * Checks:
 *   1 every tile is pixel-identical to the live screen once wsim_goto shows its page (rows 0..TILE_H-1)
 *   2 drawing every tile leaves the live screen and its "what is drawn" keys alone (no redraw follows)
 *   3 with nothing touched, no tile key changes (JS redraws nothing)
 *   4 a knob turned on a tile (wsim_goto + wsim_enc) edits: that tile's key changes, it still matches the screen,
 *     and the other pages' tiles that do not show the parameter keep their keys */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int wsim_pull(float *dst, int max_frames);
uint16_t *wsim_fb(void);
void wsim_enc(int id, int detents);
int wsim_npages(void);
int wsim_page_tile(int i);
const char *wsim_page_title(int i);
const char *wsim_page_btn(int i);
int wsim_page_sub(int i);
int wsim_live_page(void);
uint32_t wsim_page_key(int i);
uint16_t *wsim_tile(int i);
int wsim_goto(int i);

/* the sim's JS hooks (tests/websim_stub/emscripten.h): nothing to check here */
void wsim_midi_out(int st, int d1, int d2) { (void)st; (void)d1; (void)d2; }
void wsim_sysex_out(const uint8_t *p, int n) { (void)p; (void)n; }

#define TH 216                                  /* sim/mv_tiles.c TILE_H */
#define ENC_KNOB1 2                             /* platform.c, the FM-1 layout */
static float abuf[2 * 256];
static uint32_t ticks, fails, checks;
static int step = 0, page_i = 0;
static uint16_t snap[240 * 240];
static uint32_t keys[64];

static void ok(int c, const char *what, int i)
{
    checks++;
    if (!c) { fails++; printf("FAIL %s (page %d %s %d/%d)\n", what, i, i >= 0 ? wsim_page_title(i) : "",
                               i >= 0 ? (wsim_page_sub(i) & 255) + 1 : 0, i >= 0 ? wsim_page_sub(i) >> 8 : 0); }
}
static int same_rows(const uint16_t *a, const uint16_t *b, int rows) { return !memcmp(a, b, (size_t)rows * 240u * 2u); }
static int find(const char *title, int sub)
{
    int i;
    for (i = 0; i < wsim_npages(); i++)
        if (wsim_page_tile(i) && !strcmp(wsim_page_title(i), title) && (wsim_page_sub(i) & 255) == sub) return i;
    return -1;
}

/* the browser's turn: once per app pass. A small state machine; `wait` passes let the app draw */
void wsim_test_yield(void)
{
    static int wait;
    int i, n = wsim_npages(), changed;
    (void)wsim_pull(abuf, 256);                 /* ~5.8 ms of audio: the app's clock advances */
    ticks++;
    if (wait) { wait--; return; }
    switch (step) {
    case 0: wait = 400; step = 1; break;        /* boot: let the app settle (~2.3 s) */
    case 1:                                     /* 1: tile i, then its page shown, compared */
        while (page_i < n && !wsim_page_tile(page_i)) page_i++;
        if (page_i >= n) { step = 3; break; }
        memcpy(snap, wsim_tile(page_i), sizeof snap);
        ok(wsim_goto(page_i) >= 0, "goto accepted", page_i);
        wait = 3; step = 2; break;
    case 2:
        ok(wsim_live_page() == page_i, "goto shows the page", page_i);
        ok(same_rows(wsim_fb(), snap, TH), "tile == live screen", page_i);
        ok(same_rows(wsim_tile(page_i), snap, TH), "tile of the live page == tile before", page_i);
        page_i++; step = 1; break;
    case 3: {                                   /* 2: every tile drawn; the live screen untouched, now and after */
        int f = find("FILTER", 0);
        ok(f >= 0, "FILTER 1/2 found", f);
        wsim_goto(f); wait = 400; step = 4; break;   /* (> 3 s: the highlight gone) */
    }
    case 4:
        memcpy(snap, wsim_fb(), sizeof snap);
        for (i = 0; i < n; i++) if (wsim_page_tile(i)) (void)wsim_tile(i);
        ok(same_rows(wsim_fb(), snap, 240), "live screen untouched by the tiles", -1);
        for (i = 0; i < n && i < 64; i++) keys[i] = wsim_page_key(i);
        wait = 50; step = 5; break;
    case 5:                                     /* 3: nothing touched: no key moves */
        ok(same_rows(wsim_fb(), snap, 240), "live screen unchanged after passes", -1);
        for (changed = 0, i = 0; i < n && i < 64; i++) changed += keys[i] != wsim_page_key(i);
        ok(changed == 0, "no tile key changes while idle", -1);
        step = 6; break;
    case 6: {                                   /* 4: CUTOFF (FILTER 1/2, KNOB3) turned from its tile */
        int f = find("FILTER", 0), a = find("AMP ENV", 0);
        for (i = 0; i < n && i < 64; i++) keys[i] = wsim_page_key(i);
        ok(wsim_goto(a) == 1, "goto AMP ENV", a);   /* (from another page: the tile moves the screen first) */
        wait = 3; step = 7; (void)f; break;
    }
    case 7: {
        int f = find("FILTER", 0);
        for (i = 0; i < n && i < 64; i++) keys[i] = wsim_page_key(i);
        ok(wsim_goto(f) == 1, "goto FILTER from the tile", f);
        wsim_enc(ENC_KNOB1 + 2, -7);            /* CUTOFF down 7 (one slow turn: no x4) */
        wait = 3; step = 8; break;
    }
    case 8: {
        int f = find("FILTER", 0), a = find("AMP ENV", 0), l = find("LFO", 0);
        ok(wsim_page_key(f) != keys[f], "FILTER tile key changed by the edit", f);
        ok(same_rows(wsim_tile(f), wsim_fb(), TH), "edited tile (highlighted) == live screen", f);
        ok(wsim_page_key(a) == keys[a], "AMP ENV tile not redrawn", a);
        ok(wsim_page_key(l) == keys[l], "LFO tile not redrawn", l);
        printf("websim tiles: %u checks, %u failed (%d pages, %u passes)\n", checks, fails, n, ticks);
        exit(fails ? 1 : 0);
    }
    }
}
