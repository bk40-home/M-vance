/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Kris (bk40-home); ref_step / ref_interp after felucca-bsp bsp/chip/usb.c (773b0b9) uac_play,
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */
/* The USB audio output (firmware/src/uac_out.c) on this computer, with a fake SIE for EP4 OUT:
 *   1. uo_step against felucca-bsp's float law: the same step, but for rare +-1 (Q16)
 *   2. uo_interp against the float interpolation: within 1 LSB of the 16-bit input
 *   3. full scale: synth + computer at their peaks stay inside 24 bits
 *   4. not playing: the block untouched
 *   5. uo_service: a packet into the ring, an oversize packet clipped, RXCSR2 keeps ISO, overruns when nobody plays
 *   6. the clocks: a host at 44 100 Hz x (1 + ppm) against the I2S at 44 117.6 Hz, 60 s each: after priming no
 *      underrun / overrun / missed packet, the fill settles where the proportional law puts it (UO_TGT + half the
 *      clocks' mismatch in Q16 step units), the sine comes out without a glitch
 *   7. the host stops and starts again: an underrun, then playing again
 *   cc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -Wdouble-promotion -o uac_out_test tests/uac_out_test.c -lm */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- what usb.c / platform.c give uac_out.c ---- */
#define HALF_FRAMES 256u
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
enum { S_INDEX = 14, S_RXCSR1 = 20, S_RXCSR2 = 21, S_RXCOUNT1 = 22, S_RXCOUNT2 = 23 };
static struct { uint8_t config; } usb = {1};
/* the fake SIE: one EP4 OUT packet slot (as the FIFO) */
static uint32_t sie_index, sie_pending, sie_bytes, sie_rxcsr2_w = 0xFFFFu, sie_lost;
static void sie_wr(uint32_t r, uint32_t v)
{
    if (r == S_INDEX)
        sie_index = v;
    else if (sie_index == 4u && r == S_RXCSR1)
        sie_pending = 0;                        /* flush (0x90) or the packet taken: the slot is free */
    else if (sie_index == 4u && r == S_RXCSR2)
        sie_rxcsr2_w = v;
}
static uint32_t sie_rd(uint32_t r)
{
    if (sie_index != 4u)
        return 0;
    switch (r) {
    case S_RXCSR1: return sie_pending ? 1u : 0u;
    case S_RXCSR2: return 0x40u;                /* ISO */
    case S_RXCOUNT1: return sie_bytes & 0xFFu;
    case S_RXCOUNT2: return sie_bytes >> 8;
    default: return 0;
    }
}
static void fm1_usb_rx_sync(void) {}
#include "../firmware/src/uac_out.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ---- felucca-bsp's float law and interpolation (uac_play, at this port's fixed level: x 128) ---- */
static float ref_err;
static uint32_t ref_step(uint32_t fill)
{
    float adj;
    ref_err += ((float)fill - (float)UO_TGT - ref_err) * 0.05f;
    adj = ref_err * 2.0f;
    adj = adj > 400.0f ? 400.0f : adj < -400.0f ? -400.0f : adj;
    return (uint32_t)(65510.0f + adj);
}
static uint32_t ref_interp(int32_t *out, uint32_t n, uint32_t r, uint32_t pos, uint32_t step)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint32_t k = pos >> 16, a = uo_ring[(r + k) & (UO_N - 1u)], b = uo_ring[(r + k + 1u) & (UO_N - 1u)];
        float fr = (float)(pos & 0xFFFFu) * (1.0f / 65536.0f);
        float l = (float)(int16_t)a + ((float)(int16_t)b - (float)(int16_t)a) * fr;
        float rr = (float)(int16_t)(a >> 16) + ((float)(int16_t)(b >> 16) - (float)(int16_t)(a >> 16)) * fr;
        out[2u * i] += (int32_t)(l * 128.0f);
        out[2u * i + 1u] += (int32_t)(rr * 128.0f);
        pos += step;
    }
    return pos;
}

static uint32_t rnd_s = 12345u;
static uint32_t rnd(void) { rnd_s = rnd_s * 1664525u + 1013904223u; return rnd_s >> 8; }
static uint32_t frame(int32_t l, int32_t r) { return (uint32_t)(uint16_t)(int16_t)l | (uint32_t)(uint16_t)(int16_t)r << 16; }

static void reset_all(void)
{
    memset(uo_ring, 0, sizeof uo_ring);
    memset(&uo, 0, sizeof uo);
    uo_w = uo_r = 0;
    sie_pending = 0;
    sie_lost = 0;
    usb.config = 1;
}

/* 1 */
static void test_step(void)
{
    uint32_t i, fill = UO_TGT, diff = 0, maxd = 0;
    uo.err_q16 = 0;
    ref_err = 0.0f;
    for (i = 0; i < 200000u; i++) {
        uint32_t a, b, d;
        if (i % 20000u < 200u)                  /* now and then far out: the clamp */
            fill = (i / 20000u) & 1u ? UO_TGT + 900u : 40u;
        else {
            fill += rnd() % 33u;                /* a random walk around the target */
            fill -= 16u;
            if (fill > 1500u || fill < 100u)
                fill = UO_TGT;
        }
        a = uo_step(fill);
        b = ref_step(fill);
        d = a > b ? a - b : b - a;
        if (d)
            diff++;
        if (d > maxd)
            maxd = d;
    }
    CHECK(maxd <= 1u, "uo_step differs from the float law by %u (Q16)", maxd);
    CHECK(diff <= 200000u / 50u, "uo_step differs from the float law in %u of 200000 renders", diff);
    printf("1. uo_step vs float: %u of 200000 renders differ, by at most %u / 65536\n", diff, maxd);
}

/* 2 */
static void test_interp(void)
{
    static int32_t a[2u * HALF_FRAMES], b[2u * HALF_FRAMES];
    uint32_t t, i, maxd = 0;
    for (i = 0; i < UO_N; i++)
        uo_ring[i] = frame((int32_t)(rnd() & 0xFFFFu) - 32768, (int32_t)(rnd() & 0xFFFFu) - 32768);
    uo_ring[7] = frame(32767, -32768);          /* the largest steps: +-65535 */
    uo_ring[8] = frame(-32768, 32767);
    for (t = 0; t < 2000u; t++) {
        uint32_t r = t < 10u ? 7u : rnd() % UO_N, pos = rnd() & 0xFFFFu, step = 65110u + rnd() % 801u;
        uint32_t pa, pb;
        memset(a, 0, sizeof a);
        memset(b, 0, sizeof b);
        pa = uo_interp(a, HALF_FRAMES, r, pos, step);
        pb = ref_interp(b, HALF_FRAMES, r, pos, step);
        CHECK(pa == pb, "uo_interp's position differs");
        for (i = 0; i < 2u * HALF_FRAMES; i++) {
            uint32_t d = (uint32_t)abs(a[i] - b[i]);
            if (d > maxd)
                maxd = d;
        }
    }
    CHECK(maxd <= 130u, "uo_interp differs from the float by %u (24-bit scale)", maxd);   /* bound: |b-a|/512 + 1 */
    printf("2. uo_interp vs float: at most %u on the 24-bit scale (%.2f LSB of the 16-bit input)\n", maxd,
           (double)maxd / 128.0);
}

/* 3, 4 */
static void test_scale_idle(void)
{
    int32_t o[2u * HALF_FRAMES];
    uint32_t i;
    for (i = 0; i < UO_N; i++)
        uo_ring[i] = frame(32767, -32768);
    for (i = 0; i < HALF_FRAMES; i++) {
        o[2u * i] = 32767 * 128;                /* the synth at its peak after platform.c's clamp and shift */
        o[2u * i + 1u] = -32767 * 128;
    }
    uo_interp(o, HALF_FRAMES, 0, 0x8000u, UO_STEP_NOM);
    for (i = 0; i < HALF_FRAMES; i++) {
        CHECK(o[2u * i] == 8388352 && o[2u * i] <= 8388607, "full scale L: %d", o[2u * i]);
        CHECK(o[2u * i + 1u] == -8388480 && o[2u * i + 1u] >= -8388608, "full scale R: %d", o[2u * i + 1u]);
    }
    printf("3. full scale: L %d, R %d (24-bit limits %d / %d)\n", o[0], o[1], 8388607, -8388608);

    reset_all();
    uo_w = UO_N - 1u;                           /* a full ring, but alt 0 */
    for (i = 0; i < 2u * HALF_FRAMES; i++)
        o[i] = (int32_t)i;
    uac_play(o, HALF_FRAMES);
    for (i = 0; i < 2u * HALF_FRAMES; i++)
        CHECK(o[i] == (int32_t)i, "alt 0: the block changed");
    CHECK(!uo.go && uo_r == 0, "alt 0: the ring was read");
    uo_stream(1);
    usb.config = 0;
    uac_play(o, HALF_FRAMES);
    CHECK(o[5] == 5 && !uo.go, "not configured: the block changed");
    printf("4. not playing (alt 0 / not configured): the block untouched\n");
}

/* the host's side: one packet of n frames (a sine, or a count) into the fake FIFO */
static uint32_t host_frames;                    /* frames the host has sent */
static void host_packet(uint32_t n, int sine)
{
    uint32_t i;
    if (sie_pending)
        sie_lost++;                             /* the last one was never taken: lost */
    for (i = 0; i < n && i <= UO_MAXF; i++) {
        uint32_t k = host_frames + i;
        int32_t v = sine ? (int32_t)lrint(20000.0 * sin(2.0 * M_PI * 200.0 * (double)k / 44100.0)) : (int32_t)(k & 0x7FFFu);
        ep4rx[i] = frame(v, -v);
    }
    host_frames += n;
    sie_bytes = n * 4u;
    sie_pending = 1;
}

/* 5 */
static void test_service(void)
{
    uint32_t i, n0;
    reset_all();
    uo_ep4_reset();
    CHECK(sie_rxcsr2_w == 0x40u, "uo_ep4_reset: RXCSR2 %#x, not ISO", sie_rxcsr2_w);
    uo_stream(1);
    host_frames = 0;
    host_packet(45, 0);
    uo_service();
    CHECK(uo_w == 45u && !sie_pending, "one packet: %u frames in the ring", uo_w);
    CHECK(uo_ring[44] == frame(44, -44), "one packet: the last frame");
    CHECK(sie_rxcsr2_w == 0x40u, "uo_service: RXCSR2 written %#x (ISO lost)", sie_rxcsr2_w);
    uo_service();
    CHECK(uo_w == 45u, "no packet waiting: the ring moved");
    host_packet(60, 0);                         /* oversize: only UO_MAXF frames are kept */
    uo_service();
    CHECK(uo_w == 45u + UO_MAXF, "oversize packet: %u frames kept, want %u", uo_w - 45u, UO_MAXF);
    n0 = uo.overruns;
    for (i = 0; i < 100u; i++) {                /* nobody plays: the ring fills, then packets drop */
        host_packet(45, 0);
        uo_service();
    }
    CHECK(uo_w - uo_r <= UO_N, "the ring overfilled: %u", uo_w - uo_r);
    CHECK(uo.overruns > n0, "no overrun counted with nobody playing");
    printf("5. uo_service: packets into the ring, oversize clipped to %u, ISO kept, %u overruns with nobody playing\n",
           UO_MAXF, uo.overruns - n0);
}

/* 6, 7: the clocks. Events in microseconds x 1000 (ns): host packets every 1 ms, TIMER5 polls every 0.5 ms, renders
 * every 256 / 44 117.647 s. ppm: the host's clock against its nominal 44 100 Hz. */
static void run_clocks(double ppm, double secs, uint32_t gap_from_ms, uint32_t gap_ms)
{
    static int32_t o[2u * HALF_FRAMES];
    const double fs_host = 44100.0 * (1.0 + ppm * 1e-6), fs_i2s = 24e6 / 544.0;
    const double t_render = (double)HALF_FRAMES / fs_i2s;
    double t_next_render = t_render, sent_exact = 0.0;
    uint32_t ms, sub, renders = 0, glitches = 0, primed_at = 0, u0 = 0, o0 = 0, lost0 = 0, have = 0;
    double fill_sum = 0.0;
    uint32_t fill_n = 0, fill_lo = 0xFFFFFFFFu, fill_hi = 0;
    int32_t p1 = 0, p2 = 0;
    reset_all();
    uo_stream(1);
    host_frames = 0;
    for (ms = 0; (double)ms < secs * 1000.0; ms++) {
        int gap = gap_ms && ms >= gap_from_ms && ms < gap_from_ms + gap_ms;
        uint32_t n;
        sent_exact += fs_host / 1000.0;
        n = (uint32_t)sent_exact - host_frames;  /* the host's own rate: 44 / 45 a ms (more or fewer with ppm) */
        if (gap)
            host_frames += n;                   /* the host plays nothing (paused): the frames are not sent */
        else
            host_packet(n, 1);
        for (sub = 0; sub < 2u; sub++) {
            double t = (double)ms * 1e-3 + (double)sub * 0.5e-3;
            uo_service();
            while (t_next_render <= t + 0.5e-3) {   /* the renders due before the next poll */
                uint32_t i, fill = uo_w - uo_r;
                memset(o, 0, sizeof o);
                uac_play(o, HALF_FRAMES);
                renders++;
                if (uo.go && !primed_at) {
                    primed_at = renders;
                    u0 = uo.underruns;
                    o0 = uo.overruns;
                    lost0 = sie_lost;
                    have = 0;
                }
                if (primed_at && renders > primed_at + 2000u && !gap_ms) {   /* settled: ~11.6 s on */
                    fill_sum += fill;
                    fill_n++;
                    fill_lo = fill < fill_lo ? fill : fill_lo;
                    fill_hi = fill > fill_hi ? fill : fill_hi;
                }
                if (uo.go) {                    /* a glitch: the sine's second difference jumps */
                    for (i = 0; i < HALF_FRAMES; i++) {
                        int32_t x = o[2u * i];
                        if (have >= 2u && abs(x - 2 * p1 + p2) > 10000)
                            glitches++;
                        CHECK(o[2u * i + 1u] == -x || o[2u * i + 1u] == -x - 1 || o[2u * i + 1u] == -x + 1,
                              "R is not -L");
                        p2 = p1;
                        p1 = x;
                        if (have < 2u)
                            have++;
                    }
                } else
                    have = 0;
                t_next_render += t_render;
            }
        }
    }
    if (!gap_ms) {
        double mean = fill_n ? fill_sum / fill_n : 0.0;
        double want = (double)UO_TGT + (65536.0 * fs_host / fs_i2s - (double)UO_STEP_NOM) / 2.0;   /* the P law */
        CHECK(primed_at > 0 && primed_at < 10u, "%+.0f ppm: primed only at render %u", ppm, primed_at);
        CHECK(uo.underruns == u0 && uo.overruns == o0 && sie_lost == lost0,
              "%+.0f ppm: %u underruns, %u overruns, %u packets lost after priming", ppm, uo.underruns - u0,
              uo.overruns - o0, sie_lost - lost0);
        CHECK(glitches == 0, "%+.0f ppm: %u glitches in the sine", ppm, glitches);
        CHECK(fabs(mean - want) < 4.0, "%+.0f ppm: the fill settles at %.1f, the law says %.1f", ppm, mean, want);
        CHECK(fill_lo > (uint32_t)(((uint64_t)HALF_FRAMES * 65910u) >> 16) + 2u && fill_hi + UO_MAXF < UO_N,
              "%+.0f ppm: the fill %u..%u leaves no margin", ppm, fill_lo, fill_hi);
        printf("6. host %+5.0f ppm, %.0f s: primed at render %u; 0 underruns / overruns / lost; fill %.1f (law %.1f, "
               "%u..%u), %u glitches\n", ppm, secs, primed_at, mean, want, fill_lo, fill_hi, glitches);
    } else {
        CHECK(uo.underruns > u0, "a %u ms pause: no underrun", gap_ms);
        CHECK(uo.go, "a %u ms pause: not playing again at the end", gap_ms);
        printf("7. a %u ms pause in the host's stream: %u underrun(s), playing again after it: %s\n", gap_ms,
               uo.underruns - u0, uo.go ? "yes" : "NO");
    }
}

int main(void)
{
    test_step();
    test_interp();
    test_scale_idle();
    test_service();
    run_clocks(0.0, 60.0, 0, 0);
    run_clocks(-1000.0, 60.0, 0, 0);
    run_clocks(+1000.0, 60.0, 0, 0);
    run_clocks(+3000.0, 60.0, 0, 0);
    run_clocks(0.0, 10.0, 4000, 50);
    printf(fails ? "uac_out_test: %d FAILED\n" : "uac_out_test: ok\n", fails);
    return fails != 0;
}
