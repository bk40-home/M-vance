/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (felucca-bsp: FELUCCA_UAC_OUT)
 * Copyright (C) 2026 Kris (bk40-home) (M-vance: the integer port)
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */
/* USB audio output: see uac_out.h. Included by usb.c after sie_wr / sie_rd (it uses the SIE, `usb` and its
 * register names). */
#include "uac_out.h"

/* ---- state ---- */
static uint32_t uo_ring[UO_N];                  /* the packets' frames, in arrival order */
static volatile uint32_t uo_w, uo_r;            /* free-running frame counts: written by uo_service / uac_play */
static uint32_t ep4rx[UO_MAXF + 1u] __attribute__((aligned(32)));   /* EP4 OUT DMA buffer (32-aligned as usb.c's) */
static struct {
    volatile uint8_t alt;        /* the output stream's alt setting: 1 = the host plays */
    uint8_t go;                  /* primed: uac_play reads the ring */
    uint32_t pos;                /* the read position's fraction, Q16 (between ring frames uo_r and uo_r + 1) */
    int32_t err_q16;             /* the fill's distance from UO_TGT, smoothed, frames in Q16 */
    uint32_t pkts, frames;       /* packets / frames received */
    uint32_t underruns;          /* the ring ran dry at a render (the host stopped or lagged): re-primed */
    uint32_t overruns;           /* a packet found no room (nobody plays it): dropped */
    uint32_t starts;             /* alt 1 selections */
} uo;

/* ---- EP0 side ---- */
static void uo_ep4_reset(void)
{
    sie_wr(S_INDEX, 4);
    sie_wr(S_RXCSR1, 0x90);                     /* ClrDataTog + FlushFIFO */
    sie_wr(S_RXCSR2, 0x40);                     /* ISO */
}

static void uo_stream(uint32_t alt)
{
    uo.go = 0;                                  /* uac_play waits for UO_TGT frames again */
    RING_PUBLISH();
    uo.alt = (uint8_t)alt;
    if (alt)
        uo.starts++;
}

/* ---- TIMER5: one packet into the ring. Polled at 2 kHz, so a packet (one per 1 ms frame) is always taken before
 * the next arrives, even with the single FIFO. Called only while alt 1 (uac_service). ---- */
static void uo_service(void)
{
    uint32_t csr, n, i, w;
    sie_wr(S_INDEX, 4);
    csr = sie_rd(S_RXCSR1) | (sie_rd(S_RXCSR2) << 8);
    if (!(csr & 1u))
        return;                                 /* RxPktRdy clear: nothing waiting */
    n = (sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) << 8)) / 4u;   /* bytes -> stereo frames */
    if (n > UO_MAXF)
        n = UO_MAXF;
    fm1_usb_rx_sync();                          /* the DMA's writes before we read the buffer */
    w = uo_w;
    if (UO_N - (w - uo_r) < n) {
        uo.overruns++;                          /* nobody plays it (or the rates part): drop */
    } else {
        for (i = 0; i < n; i++)
            uo_ring[(w + i) & (UO_N - 1u)] = ep4rx[i];
        RING_PUBLISH();                         /* the frames before the index */
        uo_w = w + n;
    }
    uo.pkts++;
    uo.frames += n;
    csr = (csr & ~0x164u) | 0x10u;              /* clear RxPktRdy etc. (as ep1_rx): the FIFO takes the next packet */
    sie_wr(S_RXCSR1, csr & 0xFFu);
    sie_wr(S_RXCSR2, csr >> 8);
}

/* ---- the rate control, once a render (felucca-bsp's law, in integers) ----
 *   err += (fill - UO_TGT - err) x 0.05          (frames, kept in Q16)
 *   step = UO_STEP_NOM + floor(2 x err)          (Q16), clamped to +-UO_ADJ_MAX
 * A fill above the target reads faster, below it slower: the fill settles near UO_TGT (proportional only: offset
 * by half the clocks' mismatch in Q16 step units, uac_out.h). 2 x err in Q16 step units is
 * err_q16 >> 15 (an arithmetic shift: floor, as the float's truncation of the positive 65510 + adj). */
static uint32_t uo_step(uint32_t fill)
{
    int32_t d = ((int32_t)fill - (int32_t)UO_TGT) * (1 << 16) - uo.err_q16;   /* |d| < 2^28: fill < UO_N */
    int32_t adj;
    uo.err_q16 += (int32_t)(((int64_t)d * UO_SMOOTH_Q16) >> 16);           /* int64: d x 3277 passes 2^31 */
    adj = uo.err_q16 >> 15;
    adj = adj > UO_ADJ_MAX ? UO_ADJ_MAX : adj < -UO_ADJ_MAX ? -UO_ADJ_MAX : adj;
    return (uint32_t)((int32_t)UO_STEP_NOM + adj);
}

/* ---- the resampler: linear interpolation between ring frames a and b, a + (b - a) x frac ----
 * frac in Q15 (the Q16 fraction's top bits) so (b - a) x frac fits 32 bits (65 535 x 32 767 < 2^31); the sum
 * lands straight on the 24-bit scale: a << UO_SHIFT plus the product >> (15 - UO_SHIFT). Linear interpolation
 * never overshoots its two samples, so the result stays within the 16-bit input's range (x 2^UO_SHIFT). */
static uint32_t uo_interp(int32_t *out, uint32_t n, uint32_t r, uint32_t pos, uint32_t step)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint32_t k = pos >> 16, a = uo_ring[(r + k) & (UO_N - 1u)], b = uo_ring[(r + k + 1u) & (UO_N - 1u)];
        int32_t fr = (int32_t)((pos & 0xFFFFu) >> 1);                     /* Q15 */
        int32_t al = (int16_t)a, ar = (int16_t)(a >> 16);
        int32_t dl = (int16_t)b - al, dr = (int16_t)(b >> 16) - ar;
        out[2u * i] += al * (1 << UO_SHIFT) + ((dl * fr) >> (15 - UO_SHIFT));
        out[2u * i + 1u] += ar * (1 << UO_SHIFT) + ((dr * fr) >> (15 - UO_SHIFT));
        pos += step;
    }
    return pos;
}

/* ---- audio ISR: the ring, resampled to the I2S clock, added to the block (after the recording's tap and the
 * shift to 24 bits: platform.c). Fixed level (decision 3c): the computer's own volume sets it. ---- */
static __attribute__((noinline)) void uac_play(int32_t *out, uint32_t n)
{
    uint32_t r = uo_r, fill = uo_w - r, pos, step;
    if (!uo.alt || !usb.config) {               /* not playing: nothing to do (and nothing primed) */
        uo.go = 0;
        return;
    }
    pos = uo.pos;
    if (!uo.go) {                               /* priming: wait for UO_TGT frames, then start there */
        if (fill < UO_TGT)
            return;
        r = uo_w - UO_TGT;                      /* the excess is old: skip it */
        pos = 0;
        uo.err_q16 = 0;
        uo.go = 1;
        fill = UO_TGT;
    }
    step = uo_step(fill);
    if (fill < (uint32_t)(((uint64_t)n * step) >> 16) + 2u) {
        uo.underruns++;                         /* not enough for this block: drop what is there, prime again */
        uo.go = 0;
        RING_PUBLISH();
        uo_r = uo_w;
        return;
    }
    RING_PUBLISH();                             /* the frames after the index */
    pos = uo_interp(out, n, r, pos, step);
    RING_PUBLISH();                             /* done reading before the index moves */
    uo_r = r + (pos >> 16);
    uo.pos = pos & 0xFFFFu;
}
