/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* M-vance online simulator — every page at once, as tiles (Kris 13 Nov 2026,
 * fm1-port/mvance-all-pages-editor-proposal-2026-11-13.md: 1a firmware-drawn, 2a editable, 3a every mv_ui_draw page).
 *
 * #included at the end of firmware/src/mvance.c, and only with -DDINGHY_WEBSIM, so it sees the app's panel (ui), its
 * engine (eng) and settings (set). The device and host builds never see it.
 *
 *   draw    wsim_tile(i): mv_ui_draw on a COPY of the panel, set to page i, into a scratch 240 x 240 (mv_websim.c's
 *           fb_dst). The firmware's own drawing, so a tile is exactly what the FM-1 shows on that page; the live
 *           screen and the live panel's "what is drawn" keys are untouched
 *   when    wsim_page_key(i): a key of what the tile shows (mv_ui.c mv_ui_page_key). JS draws a tile only when its
 *           key changed: an edit redraws the one or two tiles it shows on, not all of them
 *   edit    wsim_goto(i), then wsim_enc(KNOBn, detents): the page shown, then the real knob path (mv_ui_input:
 *           steps, the fast-turn x4, the CC out, the highlight), so a tile edits exactly as the hardware knob does
 *
 * Called from JS only between the app's passes (the one yield is platform_poll's emscripten_sleep): never mid-draw. */

#define TILE_H 216u                             /* what JS shows of a tile: the header and the columns (mv_ui.c COL_Y
                                                   + COL_H); the edit line below is the live screen's only */
static uint16_t tile_fb[240u * 240u];           /* the scratch a tile draws into (240 rows: the edit line fits too) */

EMSCRIPTEN_KEEPALIVE int wsim_npages(void) { return (int)mv_ui_npages(); }
EMSCRIPTEN_KEEPALIVE int wsim_page_tile(int i) { return i >= 0 && mv_ui_page_tile((uint32_t)i); }
EMSCRIPTEN_KEEPALIVE const char *wsim_page_title(int i) { return mv_ui_page_title((uint32_t)i); }
EMSCRIPTEN_KEEPALIVE const char *wsim_page_btn(int i) { return mv_ui_page_btn((uint32_t)i); }
EMSCRIPTEN_KEEPALIVE int wsim_page_sub(int i) { return (int)mv_ui_page_sub((uint32_t)i); }
EMSCRIPTEN_KEEPALIVE int wsim_live_page(void) { return (int)ui.page; }   /* the page the screen shows (0 = HOME) */
EMSCRIPTEN_KEEPALIVE uint32_t wsim_page_key(int i)
{
    return i >= 0 ? mv_ui_page_key(&ui, &eng, (uint32_t)i, set.ch) : 0u;
}

/* page i drawn into the scratch: returns it (240 x 240 RGB565; JS shows the top TILE_H rows), or 0 if not a tile */
EMSCRIPTEN_KEEPALIVE uint16_t *wsim_tile(int i)
{
    mv_ui_t t;
    if (i < 0 || !mv_ui_page_tile((uint32_t)i)) return 0;
    t = ui;                                     /* a copy: the live panel's drawn[] keys stay as they are */
    t.page = (uint8_t)i;
    t.redraw = 1;                               /* the header and all 4 columns */
    if ((uint32_t)i != ui.page) t.last = 0xFF;  /* the highlight only on the page showing (mv_ui_page_key) */
    fb_dst = tile_fb;
    lcd_fill(0, 0, 240, 240, C_BLACK);          /* as the screen is cleared when a page opens (mvance.c) */
    mv_ui_draw(&t, &eng, set.ch);
    fb_dst = fb;
    return tile_fb;
}

/* page i shown on the screen (a tile clicked or turned): 1 moved, 0 already shown, -1 refused (not a tile, or the
 * factory-bank question or the transfer screen holds the screen). The bank editor's link reads and writes what plays
 * all the time without the transfer screen (mvance.c, Part B 3a): those never block a tile */
EMSCRIPTEN_KEEPALIVE int wsim_goto(int i)
{
    int r;
#if MV_XFER
    if (xfer_slots && xfer_busy()) return -1;   /* (a slot transfer: its screen is up) */
#endif
    if (i < 0 || bank_ask) return -1;
    r = mv_ui_goto(&ui, (uint32_t)i);
    if (r > 0) wsim_page_moved = 1;             /* mvance.c's loop: the screen cleared and the page drawn */
    return r;
}
