/* SPDX-License-Identifier: GPL-3.0-only */
/* The fm1-dock module (dock/) on a computer: a stand-in for fm1-dock's host (its hb_host_t, banks in RAM) drives
 * M-vance's module through its descriptor only, as fm1-dock does (Kris 12 Nov): init, the panel's events, draw()
 * each pass, render() each block, release(), then init() again on a cleared RAM (a module switch). Checks: it plays
 * (a chord is heard, silence after), LEDs only on a change, PRESETS changes the program, a SAVE written at
 * release() comes back after the switch, MIDI clock out. Writes build/dock/dock.wav.
 *   sh tests/run_dock_host.sh                (needs an fm1-dock checkout: FM1_DOCK=~/fm1-dock, default ../fm1-dock) */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "hb_module.h"

extern const hb_module_t hb_module;            /* the module's descriptor (dock/mvance_module.c HB_DESC) */

static uint32_t now_ms, now_ticks;
static uint32_t h_ms(void) { return now_ms; }
static uint32_t h_ticks(void) { return now_ticks; }
static void h_log(const char *s) { printf("  [module log] %s\n", s); }
static void h_log_u32(const char *s, uint32_t v) { printf("  [module log] %s %08x\n", s, v); }
static uint16_t fb[240 * 240];
static uint32_t fills, blits;
static void h_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t i, j;
    fills++;
    for (j = y; j < y + h; j++) for (i = x; i < x + w; i++) if (i >= 0 && i < 240 && j >= 0 && j < 240) fb[j * 240 + i] = c;
}
static void h_blit(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *px)
{
    int32_t i, j;
    blits++;
    for (j = 0; j < h; j++) for (i = 0; i < w; i++)
        if (x + i >= 0 && x + i < 240 && y + j >= 0 && y + j < 240) {
            uint16_t c = px[j * w + i];
            fb[(y + j) * 240 + x + i] = (uint16_t)(c >> 8 | c << 8);   /* (bytes as the panel takes them) */
        }
}
static void h_text(int32_t x, int32_t y, int32_t w, uint32_t big, const char *s, uint16_t fg, uint32_t c) { (void)x; (void)y; (void)w; (void)big; (void)s; (void)fg; (void)c; }
static void h_cv_begin(int32_t w, int32_t h, uint16_t bg) { (void)w; (void)h; (void)bg; }
static void h_cv_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
static void h_cv_line(int32_t a, int32_t b, int32_t c, int32_t d, uint16_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
static void h_cv_pixel(int32_t x, int32_t y, uint16_t c) { (void)x; (void)y; (void)c; }
static void h_cv_text(int32_t x, int32_t y, uint32_t big, const char *s, uint16_t fg) { (void)x; (void)y; (void)big; (void)s; (void)fg; }
static void h_cv_blit(int32_t x, int32_t y) { (void)x; (void)y; }
static uint32_t led_calls, led_state[64];
static void h_led(uint32_t id, uint32_t on) { led_calls++; if (id < 64u) led_state[id] = on; }
static uint32_t h_rand(void) { return (uint32_t)rand(); }
static uint8_t set_rec[240]; static uint32_t set_len;
static int h_load(void *p, uint32_t n) { if (!set_len || n != set_len) return -1; memcpy(p, set_rec, n); return 0; }
static void h_save(const void *p, uint32_t n) { if (n <= 240u) { memcpy(set_rec, p, n); set_len = n; } }
static uint32_t clk_out, notes_out;
static void h_midi_send(uint32_t st, uint32_t d1, uint32_t d2) { (void)d1; (void)d2; if (st == 0xF8u) clk_out++; else if ((st & 0xF0u) == 0x90u) notes_out++; }
static uint32_t h_master(void) { return 4096u; }
static int32_t h_text_w(uint32_t big, const char *s) { return (int32_t)strlen(s) * (big ? 16 : 8); }
static uint8_t banks[256][HB_BANK_MAX]; static int32_t bank_len[256]; static uint32_t bank_saves[256];
static int h_bank_load(void *p, uint32_t n) { (void)p; (void)n; return -1; }
static int h_bank_save(const void *p, uint32_t n) { (void)p; (void)n; return -1; }
static uint32_t bank_reads_ok[256];
static int h_bank_load_n(uint32_t i, void *p, uint32_t n)
{
    if (i > 255u || bank_len[i] <= 0) return -1;
    if (n == (uint32_t)bank_len[i]) bank_reads_ok[i]++;
    memcpy(p, banks[i], n < (uint32_t)bank_len[i] ? n : (uint32_t)bank_len[i]);
    return bank_len[i];
}
static int h_bank_save_n(uint32_t i, const void *p, uint32_t n)
{
    if (i > 255u || n > HB_BANK_MAX) return -1;
    memcpy(banks[i], p, n); bank_len[i] = (int32_t)n; bank_saves[i]++;
    return 0;
}
static uint32_t h_bank_free(void) { return 8u; }
static uint32_t h_sense_keys(void) { return 0; }
static int h_key_sense(uint32_t k, hb_key_sense_t *o) { (void)k; (void)o; return -1; }
static void h_sense_set(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)a; (void)b; (void)c; (void)d; }
static int h_c1_run(void (*fn)(uint32_t), uint32_t arg) { (void)fn; (void)arg; return 0; }
static int h_c1_wait(void) { return 0; }
static void h_key_base(uint32_t n) { (void)n; }
static const hb_host_t HOST = {
    HB_ABI, sizeof(hb_host_t), h_ms, h_ticks, h_log, h_log_u32, h_fill, h_text, h_cv_begin, h_cv_rect, h_cv_line,
    h_cv_pixel, h_cv_text, h_cv_blit, h_led, h_rand, h_load, h_save, h_midi_send, h_master, h_blit, h_text_w,
    h_bank_load, h_bank_save, h_sense_keys, h_key_sense, h_sense_set, h_bank_load_n, h_bank_save_n, h_bank_free,
    h_c1_run, h_c1_wait, HB_BOARD_FM1, h_key_base,
};

/* the module's RAM (its .data + .bss): what fm1-dock copies and clears at each start. Here: the linker's symbols for
 * this program's own data, which the module is all of besides this file (cleared and re-run: a module switch) */
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                  printf(__VA_ARGS__); printf("\n"); } } while (0)

#define NB 256u
static int32_t blk[NB * 2u];
static FILE *wav; static uint32_t wav_frames;
static void wav_open(const char *p)
{
    static const uint8_t hdr[44] = {0};
    wav = fopen(p, "wb");
    if (wav) fwrite(hdr, 1, 44, wav);
}
static void wav_close(void)
{
    uint32_t data = wav_frames * 4u, v;
    uint8_t h[44];
    if (!wav) return;
    memcpy(h, "RIFF", 4); v = 36u + data; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4); h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0; v = 44100; memcpy(h + 24, &v, 4);
    v = 44100u * 4u; memcpy(h + 28, &v, 4); h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0; memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fclose(wav);
}
/* one block: the ISR renders (into a zeroed block, as the host), the main loop then draws one pass */
static double block(void)
{
    uint32_t i;
    double e = 0;
    memset(blk, 0, sizeof blk);
    hb_module.render(blk, NB);
    for (i = 0; i < NB * 2u; i++) {
        int32_t v = blk[i] > 32767 ? 32767 : blk[i] < -32767 ? -32767 : blk[i];
        int16_t s = (int16_t)v;
        e += (double)v * v;
        if (wav) fwrite(&s, 2, 1, wav);
    }
    wav_frames += NB;
    now_ticks += NB * 24000000u / 44100u;
    now_ms = (uint32_t)((uint64_t)wav_frames * 1000u / 44100u);
    hb_module.draw(0);
    return e / (NB * 2u);
}
static double run_ms(uint32_t ms)
{
    double e = 0;
    uint32_t n = 0, until = now_ms + ms;
    while (now_ms < until) { e += block(); n++; }
    return n ? e / n : 0;
}
static void ev(uint32_t e, int32_t a) { hb_module.ui(e, a); }

int main(int argc, char **argv)
{
    double quiet, chord, tail;
    uint32_t leds, i, prog_before;
    static uint16_t fb0[240 * 240];
    if (argc > 2 && !strcmp(argv[2], "reload")) {   /* the module started again (a switch back, a power-on) */
        FILE *f = fopen("dock_flash.bin", "rb");
        CHECK(f && fread(&set_len, 4, 1, f) == 1 && fread(set_rec, 1, 240, f) == 240 && fread(bank_len, 4, 256, f) == 256 &&
              fread(banks[3], 1, HB_BANK_MAX, f) == HB_BANK_MAX, "the first run's flash");
        if (f) fclose(f);
        CHECK(hb_module.init(&HOST) == 0, "init again: ready");
        run_ms(500);
        CHECK(bank_reads_ok[3] == 1u, "the programs' record read back from bank 3 (whole: 8064 B)");
        CHECK(bank_saves[3] == 0u, "and not written again (nothing changed)");
        printf("dock_host_test (reload): %d checks, %d failed\n", checks, fails);
        return fails != 0;
    }
    wav_open(argc > 1 ? argv[1] : "dock.wav");
    CHECK(hb_module.abi == HB_ABI && !strcmp(hb_module.name, "M-VANCE") &&
          hb_module.flags == (HB_F_SCREEN | HB_F_PANEL) && hb_module.midi, "the descriptor: ABI 2, M-VANCE, screen + panel, midi()");
    CHECK(hb_module.init(&HOST) == 0, "init: ready");
    CHECK(fills > 0u && blits > 0u, "init: the home screen drawn (%u fills, %u blits)", fills, blits);
    quiet = run_ms(500);
    CHECK(quiet == 0.0, "nothing played: silence (%g)", quiet);
    leds = led_calls;
    run_ms(500);
    CHECK(led_calls - leds < 5u, "LEDs: no host call while nothing changes (%u in 0.5 s)", led_calls - leds);

    /* a chord on the keys (C E G: keys 7, 11, 14 from F3), held 1 s, then 2 s of release */
    ev(HB_EV_KEY_DOWN | 7u, 0); ev(HB_EV_KEY_DOWN | 11u, 0); ev(HB_EV_KEY_DOWN | 14u, 0);
    chord = run_ms(1000);
    CHECK(chord > 1e4, "a chord on the keys sounds (mean square %g)", chord);
    CHECK(led_state[HB_LED_KEY(7)] == 1u && led_state[HB_LED_KEY(11)] == 1u && led_state[HB_LED_KEY(1)] == 0u,
          "the held keys lit, others not");
    ev(HB_EV_KEY_UP | 7u, 0); ev(HB_EV_KEY_UP | 11u, 0); ev(HB_EV_KEY_UP | 14u, 0);
    run_ms(4000);
    tail = run_ms(500);
    CHECK(tail < chord / 1e4, "after the keys: the release and the effects die away (%g)", tail);
    CHECK(led_state[HB_LED_KEY(7)] == 0u, "the key LEDs off again");

    /* PRESETS: the next program; then a SAVE (SAVE: its page, SAVE: written into the program shown) */
    memcpy(fb0, fb, sizeof fb);
    ev(HB_EV_ENC | HB_ENC_PRESETS, 1);
    run_ms(300);
    CHECK(memcmp(fb0, fb, sizeof fb) != 0, "PRESETS: the screen changed (the program's name)");
    prog_before = bank_saves[3];
    ev(HB_EV_ENC | HB_ENC_KNOB1, 20);            /* HOME KNOB1: CUTOFF, an edit */
    run_ms(200);
    ev(HB_EV_BTN_DOWN | HB_BTN_SAVE, 0); ev(HB_EV_BTN_UP | HB_BTN_SAVE, 0);
    run_ms(200);
    ev(HB_EV_BTN_DOWN | HB_BTN_SAVE, 0); ev(HB_EV_BTN_UP | HB_BTN_SAVE, 0);
    run_ms(500);                                 /* silent: the record goes to its bank at once */
    CHECK(bank_saves[3] == prog_before + 1u && bank_len[3] == 8064,
          "SAVE: the programs' record in bank 3 (%d B, %u saves)", bank_len[3], bank_saves[3]);

    /* MIDI clock in (USB, 120 BPM: an F8 every 20.8 ms) and the arp held: the module follows; CLK OUT is off */
    for (i = 0; i < 48u; i++) { hb_module.midi(0xF8u, 0, 0); run_ms(21); }
    CHECK(clk_out == 0u, "CLK OUT off: no clock sent (%u)", clk_out);

    /* the host's picker over it, then back: draw(full) */
    memset(fb, 0, sizeof fb);
    hb_module.draw(1);
    run_ms(100);
    for (i = 0; i < 240u * 240u && !fb[i]; i++) {}
    CHECK(i < 240u * 240u, "draw(full): the screen drawn again");

    /* a module switch: release(), then the RAM cleared and init() again: the saved program is there */
    hb_module.release();
    CHECK(set_len > 0u && set_len <= 240u, "release: the settings record kept (%u B)", set_len);
    printf("dock_host_test: %d checks, %d failed\n", checks, fails);
    wav_close();
    {   /* hand the banks and the settings to the second run (a cleared module RAM: a new process) */
        FILE *f = fopen("dock_flash.bin", "wb");
        if (f) { fwrite(&set_len, 4, 1, f); fwrite(set_rec, 1, 240, f); fwrite(bank_len, 4, 256, f); fwrite(banks[3], 1, HB_BANK_MAX, f); fclose(f); }
    }
    return fails != 0;
}
