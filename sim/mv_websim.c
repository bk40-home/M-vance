/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* M-vance online simulator — the WebAssembly platform backend (see mv_websim.h).
 *
 * Built only for -DDINGHY_WEBSIM; the device and host builds never see this file. It provides the same
 * platform symbols as tests/host.c (the FM-1, BOARD_SMK37=0, layout) so the app (firmware/src/mvance.c) runs
 * unchanged, but drives audio/video/input from JavaScript in real time instead of a script + WAV. */
#include <stdint.h>
#include <string.h>
#include <emscripten.h>
#include "mv_websim.h"

#define FS 44100u
#define HALF_FRAMES 256u

/* ---- the free-running 24 MHz counter the app times renders with (as host.c); good enough for the CPU meter ---- */
#define FM1_TICKS_PER_US 24u
static uint32_t sim_ticks_us;
static uint32_t fm1_ticks(void) { return sim_ticks_us * FM1_TICKS_PER_US; }

/* ---- the app's millisecond clock (UI timers, LFO fades, the arp read it) ---- */
static volatile uint32_t fm1_ms;

/* ---- input: 27 keys, the panel buttons, the 7 encoders (FM-1 layout, as host.c) ---- */
static struct { uint32_t notes, buttons; } fm1_in;
static uint32_t host_edges, host_rel, host_rel_next;
static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t b = host_edges;
    host_edges = 0;
    *released = host_rel;
    host_rel = host_rel_next;
    host_rel_next = 0;
    return b;
}
static uint32_t fm1_input_note_edges(void) { return 0; }   /* the keys arrive whole in fm1_in.notes */
#define HOST_NENC 7
static int32_t host_enc[HOST_NENC];
static int32_t fm1_enc_take(uint32_t e) { int32_t v = host_enc[e % HOST_NENC]; host_enc[e % HOST_NENC] = 0; return v; }

/* LEDs: not surfaced yet. TODO: expose host_leds to JS to light the panel buttons/keys. */
static uint64_t host_leds, host_glow;
static void fm1_led_key(uint32_t id, int on) { if (on) host_leds |= (uint64_t)1 << id; else host_leds &= ~((uint64_t)1 << id); }
static void fm1_led_key_glow(uint32_t id, int on) { if (on) host_glow |= (uint64_t)1 << id; else host_glow &= ~((uint64_t)1 << id); }
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}

/* ---- the screen: 240 x 240 RGB565 (gfx.c draws into fb; JS reads it) ---- */
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u) {
                uint16_t c = px[j * w + i];
                fb[(y + j) * 240u + x + i] = (uint16_t)(c >> 8 | c << 8);   /* gfx.c sends bytes swapped (as the ST7789 gets them) */
            }
}
#ifndef CV_MAX
#define CV_MAX (240u * 64u)
#endif
#include "gfx.c"

/* ---- storage in RAM (lost on reload). TODO: back with localStorage (EM_JS) so patches and settings persist ---- */
#include "store.h"
static uint8_t s_set[SECTOR_MAX + 4], s_blob[NBLOB][SECTOR_MAX + 4], s_slot[NSLOT][SLOT_MAX + 4];
static int s_load(const uint8_t *a, uint32_t max, void *p, uint32_t n)
{
    if (!a[0] || n > max || (uint32_t)(a[1] | a[2] << 8 | a[3] << 16) != n) return -1;
    memcpy(p, a + 4, n);
    return 0;
}
static int s_save(uint8_t *a, uint32_t max, const void *p, uint32_t n)
{
    if (n > max) return -1;
    a[0] = 1; a[1] = (uint8_t)n; a[2] = (uint8_t)(n >> 8); a[3] = (uint8_t)(n >> 16);
    memcpy(a + 4, p, n);
    return 0;
}
static int settings_load(void *p, uint32_t n) { return s_load(s_set, SECTOR_MAX, p, n); }
static int settings_save(const void *p, uint32_t n) { return s_save(s_set, SECTOR_MAX, p, n); }
static int blob_load(uint32_t i, void *p, uint32_t n) { return i < NBLOB ? s_load(s_blob[i], SECTOR_MAX, p, n) : -1; }
static int blob_save(uint32_t i, const void *p, uint32_t n) { return i < NBLOB ? s_save(s_blob[i], SECTOR_MAX, p, n) : -1; }
static int slot_load(uint32_t i, void *p, uint32_t n) { return i < NSLOT ? s_load(s_slot[i], SLOT_MAX, p, n) : -1; }
static int slot_save(uint32_t i, const void *p, uint32_t n) { return i < NSLOT ? s_save(s_slot[i], SLOT_MAX, p, n) : -1; }
/* a slot's sector on its own (platform.c slot_sec_*: M-vance's clips) */
static uint8_t s_ssec[NSLOT][SLOT_SECT][SECTOR_MAX + 4];
static int slot_sec_load(uint32_t i, uint32_t k, void *p, uint32_t n) { return i < NSLOT && k < SLOT_SECT ? s_load(s_ssec[i][k], SECTOR_MAX, p, n) : -1; }
static int slot_sec_save(uint32_t i, uint32_t k, const void *p, uint32_t n) { return i < NSLOT && k < SLOT_SECT ? s_save(s_ssec[i][k], SECTOR_MAX, p, n) : -1; }

/* ---- MIDI ---- */
static uint8_t mq[64][4];                               /* status, d1, d2, source (1 = USB) */
static uint32_t mq_w, mq_r;
static int midi_get(uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    int src;
    if (mq_r == mq_w) return 0;
    *st = mq[mq_r % 64u][0]; *d1 = mq[mq_r % 64u][1]; *d2 = mq[mq_r % 64u][2];
    src = mq[mq_r % 64u][3] ? mq[mq_r % 64u][3] : 1;
    mq_r++;
    return src;
}
EM_JS(void, wsim_midi_out, (int st, int d1, int d2), { if (Module.onMidiOut) Module.onMidiOut(st, d1, d2); });
static void midi_send(uint32_t st, uint32_t d1, uint32_t d2) { wsim_midi_out((int)st, (int)d1, (int)d2); }
static void midi_rt_at(uint32_t frame, uint8_t b) { (void)frame; wsim_midi_out(b, 0, 0); }   /* (the sim: at the render) */
static void midi_rt_flush(void) { }             /* (sent at once: nothing waits) */
// SysEx out (program/bank dumps etc.): hand the whole F0..F7 message to JS (the bank editor reads these back);
// return 0 = the host kept reading, so the dump never aborts (a browser never applies back-pressure here)
EM_JS(void, wsim_sysex_out, (const uint8_t *p, int n), { if (Module.onSysEx) Module.onSysEx(HEAPU8.slice(p, p + n)); });
static int sysex_send(const uint8_t *p, uint32_t n) { wsim_sysex_out(p, (int)n); return 0; }

/* ---- the audio ring: platform_poll fills it (Q15 -> float), the AudioWorklet drains it via wsim_pull ---- */
#define RING_FRAMES 2048u                               /* a power of two; ~46 ms. The app can't render further ahead
                                                           than the ring is deep, so this also bounds play latency.
                                                           Smaller = snappier but more risk of a crackle on underrun. */
static float ring[RING_FRAMES * 2u];
static volatile uint32_t ring_w, ring_r;                 /* frame indices */
static uint32_t ring_used(void) { return (ring_w - ring_r) & (RING_FRAMES - 1u); }
static uint32_t ring_free(void) { return RING_FRAMES - 1u - ring_used(); }

static void app_audio(int32_t *out, uint32_t n);
static void app_main(void);

/* one render block: the real ISR path, Q15 int -> float, into the ring */
static void pump_block(void)
{
    int32_t blk[2u * HALF_FRAMES];
    uint32_t i;
    memset(blk, 0, sizeof blk);                         /* app_audio expects a zeroed buffer */
    app_audio(blk, HALF_FRAMES);
    for (i = 0; i < HALF_FRAMES; i++) {
        uint32_t w = ring_w & (RING_FRAMES - 1u);
        ring[2u * w]       = (float)blk[2u * i]      * (1.0f / 32768.0f);
        ring[2u * w + 1u]  = (float)blk[2u * i + 1u] * (1.0f / 32768.0f);
        ring_w++;
    }
    sim_ticks_us += HALF_FRAMES * 1000000u / FS;        /* advance the fake clock by the block's duration */
}

/* mvance.c's app_main loop calls this often. Non-blocking: fill the ring while it has room, carry the ms clock
 * along with the audio, then yield to the browser (Asyncify unwinds the app's stack here and rewinds after;
 * the pull, input and canvas all run during the yield). */
static uint32_t ms_accum;                               /* sample fraction toward the next millisecond */
static void platform_poll(void)
{
    while (ring_free() >= HALF_FRAMES) {
        pump_block();
        ms_accum += HALF_FRAMES * 1000u;
        while (ms_accum >= FS) { fm1_ms++; ms_accum -= FS; }
    }
    emscripten_sleep(1);
}

/* ================= the JavaScript-facing API (mv_websim.h) ================= */
EMSCRIPTEN_KEEPALIVE int wsim_pull(float *dst, int max_frames)
{
    int i = 0;
    while (i < max_frames && ring_r != ring_w) {
        uint32_t r = ring_r & (RING_FRAMES - 1u);
        dst[2 * i]     = ring[2u * r];
        dst[2 * i + 1] = ring[2u * r + 1u];
        ring_r++;
        i++;
    }
    return i;
}
EMSCRIPTEN_KEEPALIVE uint16_t *wsim_fb(void) { return fb; }
EMSCRIPTEN_KEEPALIVE void wsim_keys(uint32_t mask) { fm1_in.notes = mask & 0x07FFFFFFu; }   /* 27 keys */
EMSCRIPTEN_KEEPALIVE void wsim_btn(int id, int down)
{
    uint32_t bit = 1u << ((uint32_t)id & 31u);
    if (down) { host_edges |= bit; fm1_in.buttons |= bit; }
    else { host_rel |= bit; fm1_in.buttons &= ~bit; }
}
EMSCRIPTEN_KEEPALIVE void wsim_enc(int id, int detents) { host_enc[(uint32_t)id % HOST_NENC] += detents; }
EMSCRIPTEN_KEEPALIVE void wsim_midi(int st, int d1, int d2)
{
    mq[mq_w % 64u][0] = (uint8_t)st; mq[mq_w % 64u][1] = (uint8_t)d1;
    mq[mq_w % 64u][2] = (uint8_t)d2; mq[mq_w % 64u][3] = 1;
    mq_w++;
}

int main(void) { app_main(); return 0; }   /* app_main never returns: it runs the simulator under Asyncify */
