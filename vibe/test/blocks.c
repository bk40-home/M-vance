/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM, P2's block test (Kris 23 Nov 2026): main.c itself, built natively against stand-ins for the
 * SDK, plays a chord in host blocks of random sizes (1..256, as a DAW splits them at its events) and in blocks of 128.
 * The two streams must be the same, sample for sample: the 16-frame queue in vibe_on_audio loses nothing, repeats
 * nothing and adds nothing. Each way runs in a process of its own (the engine's lines are statics, zeroed once at
 * start as on the device: a second run in the same process would hear the first one's echoes).
 *   blocks fixed|random <file>   writes the first half (before the note-offs) as raw floats; blocks rate: 32 kHz is silent */
#include <stdio.h>
#include <string.h>
#include "vibe.h"
static vibe_transport_info tpi;
const vibe_transport_info *vibe_transport(void) { return &tpi; }
void vibe_notes(int f) { (void)f; }
void vibe_led_pad(int p, uint32_t c, float l) { (void)p; (void)c; (void)l; }
void vg_controls(const char *t, const char *const *r, int n) { (void)t; (void)r; (void)n; }
void vg_clear(uint32_t c) { (void)c; }
void vg_rect(int x, int y, int w, int h, uint32_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void vg_round(int x, int y, int w, int h, int r, uint32_t c) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)c; }
int vg_text(const char *s, int x, int y, int f, uint32_t c, int a) { (void)s; (void)x; (void)y; (void)f; (void)c; (void)a; return 0; }
char *vg_itoa(int v, char *b) { sprintf(b, "%d", v); return b; }
#include "../jt80x0/main.c"

#define N (44100u * 3u)
static float A[2][N];

static void note(int down, int id, float v)
{
    vibe_event e; memset(&e, 0, sizeof e);
    e.type = VIBE_EV_NOTE; e.id = (uint8_t)id; e.down = (uint8_t)down; e.value = v;
    vibe_on_event(&e);
}

static void play(float (*o)[N], int random_sizes)
{
    uint32_t done = 0, seed = 12345u;
    vibe_on_init(44100.0f);
    note(1, 57, 0.9f); note(1, 64, 0.7f); note(1, 69, 0.5f);
    while (done < N) {
        uint32_t n = 128u;
        if (random_sizes) { seed = seed * 1664525u + 1013904223u; n = 1u + (seed >> 8) % 256u; }
        if (n > N - done) n = N - done;
        if (done >= N / 2u && done - n < N / 2u) { note(0, 57, 0); note(0, 64, 0); note(0, 69, 0); }
        vibe_on_audio(0, o[0] + done, o[1] + done, (int)n);
        done += n;
    }
}

int main(int argc, char **argv)
{
    uint32_t i, loud = 0;
    float s[64], t[64];
    FILE *f;
    if (argc == 2 && !strcmp(argv[1], "rate")) {   /* a rate without tables: silence, no crash */
        vibe_on_init(32000.0f);
        note(1, 60, 1.0f);
        for (i = 0; i < 64u; i++) s[i] = t[i] = 1.0f;
        vibe_on_audio(0, s, t, 64);
        for (i = 0; i < 64u && s[i] == 0.0f && t[i] == 0.0f; i++) {}
        printf("32 kHz: %s\n", i == 64u ? "silent, as it should be" : "NOT silent");
        return i != 64u;
    }
    if (argc != 3) return 2;
    play(A, !strcmp(argv[1], "random"));
    for (i = 0; i < N / 2u - 256u; i++) if (A[0][i] > 0.01f || A[0][i] < -0.01f) loud++;
    printf("%s blocks: %u loud frames\n", argv[1], loud);
    f = fopen(argv[2], "wb");
    /* (the note-offs land on a different frame with random blocks: up to the first block that holds N / 2) */
    for (i = 0; i < N / 2u - 256u; i++) { fwrite(&A[0][i], 4, 1, f); fwrite(&A[1][i], 4, 1, f); }
    fclose(f);
    return loud < 1000u;
}
