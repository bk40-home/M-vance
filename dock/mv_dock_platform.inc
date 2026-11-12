/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* M-vance as an fm1-dock module: the platform (firmware/src/platform.c's calls) on the dock host's services
 * (fm1-dock's firmware/dock/hb_module.h). platform.c includes this file in place of the hardware when DINGHY_DOCK
 * is set (dock/module.cfg), as it includes tests/host.c for the host build and sim/mv_websim.c for the simulator.
 * After fm1-dock's modules/obervave/obv_platform.inc (OberVave, the same Dinghy app shape). Kris 12 Nov:
 * fm1-port/mvance-dock-feasibility-proposal-2026-11-12.md.
 *
 *   panel      fm1-dock's modules/lib/hb_fm1.h (HB_F_PANEL: the keys, buttons and encoders as events into fm1_in,
 *              the edges, the encoders); the LEDs through a cache (a host call only for an LED that changes)
 *   time       the host's ms() and 24 MHz ticks()
 *   screen     M-vance's own gfx.c (its canvas, fonts and palette) onto the host's fill() and blit() (HB_F_SCREEN)
 *   MIDI       in: the module's midi() fills hb_fm1.h's queue (clock and transport too, over USB); out: midi_send.
 *              The clock out (midi_rt_at, from the audio ISR) is queued and sent from the main loop's next pass
 *   storage    settings: the module's settings record (240 B; M-vance's is 34). Slots: the module's banks, slot i in
 *              bank i + 1 (only the programs' records are used: slots 2 and 3, banks 3 and 4, 8064 B each,
 *              MV_PARTS 1). Blobs and
 *              slot sectors: none (the two-part programs and the clips are not in the module)
 *   not here   USB audio and the updater (the host's), Roland SysEx (MV_XFER 0), the SMK-37's own layout (the
 *              host gives an SMK-37's panel as an FM-1's) */
#include <stdint.h>
#define fm1_led_key hbf_led_key                 /* (hb_fm1.h's: a host call each time; this file's caches them) */
#include "hb_fm1.h"
#undef fm1_led_key

#if BOARD_SMK37
#error "dock: the module is built with the FM-1's layout (BOARD_SMK37 0); the host maps an SMK-37 onto it"
#endif
#if MV_XFER || MV_SEQ || MV_DEBUG || MV_PARTS != 1
#error "dock: build with dock/module.cfg's switches (MV_XFER 0, MV_SEQ 0, MV_DEBUG 0, MV_PARTS 1)"
#endif

#ifndef APP_NAME
#define APP_NAME "M-VANCE"
#endif
#ifndef APP_VER
#define APP_VER "dock"
#endif
#define FS 44100u                               /* the host's rate and block: as Dinghy's */
#define HALF_FRAMES 256u

/* the panel by name: platform.c's FM-1 enums (BTN_*, ENC_*, KEY_LED), which are the host's ids */
_Static_assert((int)BTN_OCT_DOWN == (int)HB_BTN_OCT_DOWN && (int)BTN_HOME == (int)HB_BTN_HOME &&
               (int)BTN_REC == (int)HB_BTN_REC && (int)ENC_KNOB1 == (int)HB_ENC_KNOB1 &&
               (int)ENC_PRESETS == (int)HB_ENC_PRESETS && KEY_LED(0) == HB_LED_KEY(0),
               "dock: platform.c's panel ids are the host's");

#define RING_PUBLISH() __asm__ volatile("" ::: "memory")

/* ---- the LEDs: what is lit, and the glow under it; a host call only for one that changes (M-vance sets every LED
 * every pass). dock_leds_resend: all of them again (the host's picker drew its own over them) */
#define DOCK_NLED 41u                           /* 14 buttons, 27 keys */
static uint64_t dock_lit, dock_glow, dock_sent_lit, dock_sent_glow;
static void dock_led(uint32_t id)
{
    uint64_t b = (uint64_t)1 << id;
    uint32_t v = (dock_lit & b) ? 1u : (dock_glow & b) ? 2u : 0u;
    uint32_t was = (dock_sent_lit & b) ? 1u : (dock_sent_glow & b) ? 2u : 0u;
    if (v != was) {
        H->led(id, v);
        dock_sent_lit = (dock_sent_lit & ~b) | (v == 1u ? b : 0u);
        dock_sent_glow = (dock_sent_glow & ~b) | (v == 2u ? b : 0u);
    }
}
static void fm1_led_key(uint32_t id, int on)
{
    if (id >= DOCK_NLED) return;
    dock_lit = on ? dock_lit | (uint64_t)1 << id : dock_lit & ~((uint64_t)1 << id);
    dock_led(id);
}
static void fm1_led_key_glow(uint32_t id, int on)
{
    if (id >= DOCK_NLED) return;
    dock_glow = on ? dock_glow | (uint64_t)1 << id : dock_glow & ~((uint64_t)1 << id);
    dock_led(id);
}
static void dock_leds_resend(void)
{
    uint32_t id;
    dock_sent_lit = ~dock_lit;                  /* (every one differs: each is sent) */
    dock_sent_glow = 0;
    for (id = 0; id < DOCK_NLED; id++) dock_led(id);
}

/* ---- the screen: gfx.c's canvas onto the host's (blit: the bytes as the panel takes them, as gfx.c makes them) */
static void lcd_sync(void) {}                   /* (the host's blit is done when it returns) */
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    H->fill((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, c);
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    H->blit((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, px);
}
#ifndef CV_MAX
#define CV_MAX (240u * 64u)                     /* as platform.c: the biggest text box drawn at once */
#endif
#include "gfx.c"

/* ---- MIDI clock out from the audio ISR (mvance.c mco_block): queued with nothing but a store, sent from the main
 * loop's next pass (the host's midi_send is the main loop's). The frame is not kept: the pass follows within a few
 * ms, so a DAW sees the clock with that much jitter (on Dinghy it went out at its frame) */
#define DOCK_RTQ 16u                            /* a power of two; at most ~1 F8 a buffer at 240 BPM */
static volatile uint8_t dock_rtq[DOCK_RTQ];
static volatile uint32_t dock_rt_w, dock_rt_skip;   /* the ISR's: written to; what a flush dropped (up to) */
static uint32_t dock_rt_r;                      /* the main loop's: sent up to */
static void midi_rt_at(uint32_t frame, uint8_t b)   /* audio ISR */
{
    (void)frame;
    if (dock_rt_w - dock_rt_r < DOCK_RTQ) {
        dock_rtq[dock_rt_w % DOCK_RTQ] = b;
        RING_PUBLISH();
        dock_rt_w++;
    }
}
static void midi_rt_flush(void) { dock_rt_skip = dock_rt_w; }   /* audio ISR: the F8s still waiting dropped (PLAY):
                                                   the main loop skips to here (only it moves the read) */

/* ---- storage: the settings record and the module's banks (hb_module.h) */
#include "store.h"                              /* NSLOT and the rest, as the app knows them (included once: mv_clip.h's
                                                   include of it is then nothing); the one size that differs here: */
#undef SLOT_MAX
#define SLOT_MAX HB_BANK_MAX                    /* 8176: a bank (slot i = bank i + 1) */
#define DOCK_SET_MAX 240u                       /* the module's settings record (Dinghy's: a 4 KiB sector) */
static int settings_load(void *p, uint32_t n) { return n <= DOCK_SET_MAX ? H->load(p, n) : -1; }
static int settings_save(const void *p, uint32_t n)
{
    if (n > DOCK_SET_MAX) return -1;
    H->save(p, n);                              /* (kept by the host, written when nothing plays) */
    return 0;
}
static int blob_load(uint32_t i, void *p, uint32_t n) { (void)i; (void)p; (void)n; return -1; }
static int blob_save(uint32_t i, const void *p, uint32_t n) { (void)i; (void)p; (void)n; return -1; }
static int dock_banks(void) { return HB_HOST_HAS(H, bank_free); }   /* (an ABI-2 host with its banks) */
static int slot_load(uint32_t i, void *p, uint32_t n)
{
    if (i >= NSLOT || n > SLOT_MAX || !dock_banks()) return -1;
    return H->bank_load_n(1u + i, p, n) == (int)n ? 0 : -1;   /* (another length: not ours, as store.h) */
}
static int slot_save(uint32_t i, const void *p, uint32_t n)
{
    if (i >= NSLOT || n > SLOT_MAX || !dock_banks()) return -1;
    return H->bank_save_n(1u + i, p, n) == 0 ? 0 : -1;   /* (a full bank area: -1, the save stays marked) */
}
static int slot_sec_load(uint32_t i, uint32_t k, void *p, uint32_t n) { (void)i; (void)k; (void)p; (void)n; return -1; }
static int slot_sec_save(uint32_t i, uint32_t k, const void *p, uint32_t n) { (void)i; (void)k; (void)p; (void)n; return -1; }

/* ---- the main loop's poll (mvance.c calls it every pass): the queued clock out. The host polls USB, the keys and
 * MIDI itself */
static void platform_poll(void)
{
    uint32_t skip = dock_rt_skip;
    if ((int32_t)(skip - dock_rt_r) > 0) dock_rt_r = skip;   /* (a flush since the last pass) */
    while (dock_rt_r != dock_rt_w) {
        midi_send(dock_rtq[dock_rt_r % DOCK_RTQ], 0, 0);
        dock_rt_r++;
    }
}

static void app_audio(int32_t *out, uint32_t n);   /* mvance.c: the render (dock/mvance_module.c render()) */
static void app_main(void);                        /* mvance.c: one pass a call (DINGHY_DOCK) */
