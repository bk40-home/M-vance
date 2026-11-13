/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* The sim's SysEx in and its link (Kris 13 Nov, Part B 5a + 1b), tested natively as tests/websim_tiles_test.c is:
 * the real app and sim/mv_websim.c built with cc; this file plays the bank editor page.
 *
 *   tests/run_websim_tiles.sh        (builds and runs both; exit 0 = every check passed)
 *
 * Checks: RQ1 01 00 through wsim_sysex_in is answered with the 4-message dump; a knob turned on a tile is reported as
 * one DT1 of exactly that byte; a DT1 the page writes is taken (the tile shows it) and not echoed back. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int wsim_pull(float *dst, int max_frames);
void wsim_enc(int id, int detents);
void wsim_sysex_in(const uint8_t *p, int n);
int wsim_npages(void);
int wsim_page_tile(int i);
const char *wsim_page_title(int i);
int wsim_page_sub(int i);
uint32_t wsim_page_key(int i);
int wsim_goto(int i);

#define ENC_KNOB3 4                             /* platform.c, the FM-1 layout */
#define CUTOFF 41                               /* FILTER 1/2 KNOB3: patch byte 41 */
static uint8_t out[64][260];                    /* what the sim sent (Module.onSysEx), F0 .. F7 */
static int out_len[64], out_n;
static uint32_t checks, fails;
static float abuf[512];

void wsim_midi_out(int st, int d1, int d2) { (void)st; (void)d1; (void)d2; }
void wsim_sysex_out(const uint8_t *p, int n)
{
    if (out_n < 64 && n <= 260) { memcpy(out[out_n], p, (size_t)n); out_len[out_n++] = n; }
}
static void ok(int c, const char *what) { checks++; if (!c) { fails++; printf("FAIL %s\n", what); } }
static void sx(uint8_t cmd, const uint8_t a[4], const uint8_t *d, int nd)   /* a Roland message into the sim */
{
    uint8_t m[300];
    int n = 0, i, s = 0;
    m[n++] = 0xF0; m[n++] = 0x41; m[n++] = 0x10; m[n++] = 0x00; m[n++] = 0x06; m[n++] = cmd;
    for (i = 0; i < 4; i++) { m[n++] = a[i]; s += a[i]; }
    for (i = 0; i < nd; i++) { m[n++] = d[i]; s += d[i]; }
    m[n++] = (uint8_t)((128 - s % 128) % 128); m[n++] = 0xF7;
    wsim_sysex_in(m, n);
}
static int find(const char *t, int sub)
{
    int i;
    for (i = 0; i < wsim_npages(); i++)
        if (wsim_page_tile(i) && !strcmp(wsim_page_title(i), t) && (wsim_page_sub(i) & 255) == sub) return i;
    return -1;
}

void wsim_test_yield(void)
{
    static int step, wait, f;
    static uint8_t cut;
    static uint32_t key;
    static const uint8_t TEMP[4] = {1, 0, 0, 0}, ZERO[4] = {0, 0, 0, 0};
    (void)wsim_pull(abuf, 256);
    if (wait) { wait--; return; }
    switch (step++) {
    case 0: wait = 300; break;                  /* boot */
    case 1: sx(0x11, TEMP, ZERO, 4); wait = 10; break;   /* link: read what plays */
    case 2:
        ok(out_n == 4, "RQ1 01 00: 4 messages back");
        ok(out_n == 4 && out[1][8] == 0x40 && out_len[1] == 12 + 248, "the upper patch whole (01 00 40 00, 248 B)");
        cut = out_n == 4 ? out[1][10 + CUTOFF] : 0;
        f = find("FILTER", 0);
        out_n = 0;
        ok(wsim_goto(f) == 1, "goto FILTER right after the link read"); wsim_enc(ENC_KNOB3, -3);  /* a tile turned (as pages.js does) */
        wait = 5; break;
    case 3:
        ok(out_n == 1, "a tile edit: one report");
        ok(out_n >= 1 && out[0][6] == 1 && out[0][7] == 0 && out[0][8] == 0x40 && out[0][9] == CUTOFF &&
           out_len[0] == 13 && out[0][10] == (uint8_t)(cut >= 3 ? cut - 3 : 0), "it is 01 00 40 29 = CUTOFF - 3");
        out_n = 0; key = wsim_page_key(f);
        { const uint8_t A[4] = {1, 0, 0x40, CUTOFF}, v = 99; sx(0x12, A, &v, 1); }   /* the page writes CUTOFF 99 */
        wait = 5; break;
    case 4:
        ok(out_n == 0, "the page's write: not echoed");
        ok(wsim_page_key(f) != key, "the page's write: the FILTER tile shows it");
        printf("websim link: %u checks, %u failed\n", checks, fails);
        exit(fails ? 1 : 0);
    }
}
