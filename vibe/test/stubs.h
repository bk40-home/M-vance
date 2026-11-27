/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* JT-80x0 on VIBE FM, the native tests: stand-ins for the SDK's functions, so main.c itself builds on this computer
 * (blocks.c, ui_test.c). Nothing here draws; FOCUS answers "not mine" (VF_NO), as the SDK's does for every event the
 * tests send (no DATA, no ENTER). Time: the frames the audio has made. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vibe.h"
#include "vibe_gfx.h"
#include "vibe_dsp.h"
static vibe_transport_info tpi;
static uint64_t stub_frames;
const vibe_transport_info *vibe_transport(void) { return &tpi; }
uint64_t vibe_frames(void) { return stub_frames; }
void vibe_notes(int f) { (void)f; }
void vibe_led_pad(int p, uint32_t c, float l) { (void)p; (void)c; (void)l; }
void vibe_led_button(int b, uint32_t c, float l) { (void)b; (void)c; (void)l; }
void vibe_label(int b, const char *t) { (void)b; (void)t; }
void vf_mode(int m) { (void)m; }
int vf_event(const vibe_event *e, int *id) { (void)e; *id = -1; return VF_NO; }
void vf_begin(void) {}
void vf_button(int id, int x, int y, int w, int h) { (void)id; (void)x; (void)y; (void)w; (void)h; }
void vf_encoder(int en, int x, int y, int w, int h) { (void)en; (void)x; (void)y; (void)w; (void)h; }
void vf_key(int b, int x, int y, int w, int h) { (void)b; (void)x; (void)y; (void)w; (void)h; }
void vf_end(uint32_t c) { (void)c; }
float vd_sin(float p) { (void)p; return 0.0f; }
void vg_controls(const char *t, const char *const *r, int n) { (void)t; (void)r; (void)n; }
void vg_clear(uint32_t c) { (void)c; }
void vg_rect(int x, int y, int w, int h, uint32_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void vg_round(int x, int y, int w, int h, int r, uint32_t c) { (void)x; (void)y; (void)w; (void)h; (void)r; (void)c; }
void vg_line(float a, float b, float c, float d, float w, uint32_t k) { (void)a; (void)b; (void)c; (void)d; (void)w; (void)k; }
void vg_circle(float x, float y, float r, uint32_t c) { (void)x; (void)y; (void)r; (void)c; }
int vg_text(const char *s, int x, int y, int f, uint32_t c, int a) { (void)s; (void)x; (void)y; (void)f; (void)c; (void)a; return 0; }
char *vg_itoa(int v, char *b) { sprintf(b, "%d", v); return b; }
/* parameters (SDK 8): kept here as the SDK keeps them; what the firmware reports is counted per parameter */
static const vibe_param *stub_params;
static int stub_np;
static float stub_pv[VIBE_MAX_PARAMS];
static uint32_t stub_pset[VIBE_MAX_PARAMS];     /* vibe_param_set calls, per parameter */
int vibe_params(const vibe_param *l, int n) { int i; stub_params = l; stub_np = n; for (i = 0; i < n; i++) stub_pv[i] = l[i].def; return n; }
void vibe_param_groups(const char *const *g) { (void)g; }
void vibe_param_set(int i, float v) { if (i >= 0 && i < stub_np) { stub_pv[i] = v; stub_pset[i]++; } }
float vibe_param_get(int i) { return i >= 0 && i < stub_np ? stub_pv[i] : 0.0f; }
void vibe_editor_control(int p, int c) { (void)p; (void)c; }
void vibe_editor_accent(uint32_t c) { (void)c; }
void vibe_editor_cards(const vibe_card *c, int n) { (void)c; (void)n; }
void vibe_editor_tab_color(const char *t, uint32_t c) { (void)t; (void)c; }
/* storage (SDK 3): files in memory, as the SDK's hosts keep them; stub_store_clear() = a fresh instance */
#define STUB_NF 96
static struct { char name[40]; uint8_t d[4096]; int n; } stub_f[STUB_NF];
static int stub_nf;
static uint32_t stub_writes;
static void stub_store_clear(void) { stub_nf = 0; stub_writes = 0; }
static int stub_find(const char *name) { int i; for (i = 0; i < stub_nf; i++) if (!strcmp(stub_f[i].name, name)) return i; return -1; }
int vibe_store_write(const char *name, const void *d, int n)
{
    int i = stub_find(name);
    if (n < 0 || n > 4096) return -1;
    if (i < 0) { if (stub_nf >= STUB_NF) return -1; i = stub_nf++; strncpy(stub_f[i].name, name, 39); }
    memcpy(stub_f[i].d, d, (size_t)n); stub_f[i].n = n; stub_writes++;
    return 0;
}
int vibe_store_read(const char *name, void *d, int max)
{
    int i = stub_find(name), n;
    if (i < 0) return -1;
    n = stub_f[i].n < max ? stub_f[i].n : max;
    memcpy(d, stub_f[i].d, (size_t)n);
    return n;
}
int vibe_store_delete(const char *name)
{
    int i = stub_find(name);
    if (i < 0) return -1;
    stub_f[i] = stub_f[--stub_nf];
    return 0;
}
