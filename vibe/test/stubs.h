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
