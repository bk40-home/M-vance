/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the rate chosen at start (firmware/src/mv_rate.h, MV_RATE_RT: the VIBE FM build; Kris 23 Nov 2026, P1 of
 * claude/jt80x0-vibe-port-proposal-2026-11-23.md). The FM-1 build is checked elsewhere: tests/run_rate_compare.sh renders
 * every host script both ways at 44.1 kHz and wants the same WAV, bit for bit. Here, what is new: the same sound in
 * SECONDS and HERTZ at 48 kHz as at 44.1 (pitch, the amp envelope, the delay's echo, the LFO), and the sets themselves.
 * Built with -DMV_RATE_RT; host only (uses double to measure). */
#include <stdio.h>
#include <string.h>
#include "mv_patch.c"
#include "mv_pitch.c"
#include "mv_osc.c"
#include "mv_env.c"
#include "mv_lfo.c"
#include "mv_filter.c"
#include "mv_fx.c"
#include "mv_reverb.c"
#include "mv_arp.c"
#include "mv_seq.c"
#include "mv_lim.c"
#include "mv_voice.c"
#include "mv_rate.c"

#ifndef MV_RATE_RT
#error "mv_rate_test: build with -DMV_RATE_RT"
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

static mv_engine_t e;
static int32_t buf[2 * 256];

/* render n frames in blocks of 256; the rising zero crossings of L and the frame the sound last went past lvl */
static void run(uint32_t n, uint32_t *rises, uint32_t *last_loud, int32_t lvl)
{
    uint32_t done = 0, i;
    static int32_t prev;
    if (rises) *rises = 0;
    if (last_loud) *last_loud = 0;
    while (done < n) {
        uint32_t k = n - done < 256u ? n - done : 256u;
        k &= ~(MV_CTL - 1u);                    /* the engine's blocks */
        if (!k) break;
        memset(buf, 0, sizeof buf);
        mv_engine_render(&e, buf, k);
        for (i = 0; i < k; i++) {
            if (rises && prev < 0 && buf[2 * i] >= 0) (*rises)++;
            if (last_loud && (buf[2 * i] > lvl || buf[2 * i] < -lvl)) *last_loud = done + i;
            prev = buf[2 * i];
        }
        done += k;
    }
}

/* a plain SAW A4, filter open (mv_voice_test's patch), amp release byte rel */
static void saw_patch(uint32_t rel)
{
    mv_engine_init(&e);
    e.patch.b[30] = 5; e.patch.b[31] = 0; e.patch.b[23] = 0;   /* OSC1 SAW, balance full OSC1 */
    e.patch.b[41] = 127; e.patch.b[42] = 0;                    /* cutoff open, no resonance */
    e.patch.b[57] = (uint8_t)rel;
}

/* the delay alone (mv_fx): an impulse in, the frame its first echo comes out (MONO LONG, TIME 64: 630 ms) */
static uint32_t echo_frame(void)
{
    static mv_fx_t fx;
    static uint8_t b[256];
    static int32_t bus[16], out[32];
    uint32_t f, i;
    memset(b, 0, sizeof b);
    b[63] = 4; b[64] = 64; b[65] = 0; b[66] = 127;           /* MONO LONG, TIME 64, no feedback, full level */
    mv_fx_init(&fx);
    mv_fx_set(&fx, b, 0u, 0u);
    memset(bus, 0, sizeof bus);
    for (f = 0; f < 3u * MV_FS; f += 16u) {     /* silence while the tap glides from its 10 ms start to TIME (the JT's slew) */
        memset(out, 0, sizeof out);
        mv_fx_out(&fx, bus, out, 16u);
    }
    for (f = 0; f < 2u * MV_FS; f += 16u) {
        for (i = 0; i < 16u; i++) bus[i] = (f == 0u && i == 0u) ? 16384 : 0;
        memset(out, 0, sizeof out);             /* (mv_fx_out adds into out, as the engine's render does) */
        mv_fx_out(&fx, bus, out, 16u);
        for (i = 0; i < 16u; i++)
            if (f + i > MV_FS / 100u && (out[2 * i] > 2000 || out[2 * i] < -2000)) return f + i;
    }
    return 0;
}

int main(void)
{
    static const uint32_t RATES[2] = {44100u, 48000u};
    double rel_s[2] = {0, 0}, echo_s[2] = {0, 0}, lfo_hz[2] = {0, 0};
    uint32_t k, r, j;

    /* the sets: the two rates are taken, another leaves the one in use */
    CHECK(mv_rate->fs == 44100u, "44.1 kHz until a set is chosen (%u)", mv_rate->fs);
    CHECK(mv_rate_set(48000u) == 1 && MV_FS == 48000u, "48 kHz taken");
    CHECK(mv_rate_set(96000u) == 0 && MV_FS == 48000u, "96 kHz refused, 48 kept");
    CHECK(mv_rate_set(44100u) == 1 && MV_FS == 44100u, "44.1 kHz taken");
    /* the pitch tables: the 48 kHz step is the 44.1 one x 44100 / 48000, to the rounding */
    for (j = 0; j < 12u; j++) {
        const double want = MV_INC_TOP_44K[j] * 44100.0 / 48000.0;
        CHECK(MV_INC_TOP_48K[j] > want - 1.0 && MV_INC_TOP_48K[j] < want + 1.0, "INC_TOP[%u] at 48 kHz", j);
    }
    CHECK(MV_FLT_G_N_48K > MV_FLT_G_N_44K, "the filter table reaches 0.49 fs at 48 kHz (%u > %u)", MV_FLT_G_N_48K, MV_FLT_G_N_44K);
    CHECK(MV_RATES[1].fx_dlen <= MV_FX_DLEN_MAX && MV_RATES[1].fx_mlen <= MV_FX_MLEN_MAX, "the lines hold 48 kHz");

    for (k = 0; k < 2u; k++) {
        uint32_t fs = RATES[k], rises, last;
        CHECK(mv_rate_set(fs) == 1, "set %u", fs);
        /* pitch: A4 = 440 rises in one second, whatever the rate */
        saw_patch(0);
        mv_engine_note_on(&e, 69, 100);
        run(fs / 10u, 0, 0, 0);
        run(fs, &rises, 0, 0);
        CHECK(rises >= 439u && rises <= 441u, "%u Hz: A4 %u rises in 1 s", fs, rises);
        /* the amp release (byte 57): the time it takes to fall under -40 dB, in seconds */
        saw_patch(60);
        mv_engine_note_on(&e, 69, 100);
        run(fs / 5u, 0, 0, 0);
        mv_engine_note_off(&e, 69);
        run(4u * fs, 0, &last, 32768 / 100);
        rel_s[k] = (double)last / fs;
        /* LFO1 to pitch (rate 64, depth up): its rate from the pitch's wobble, the rises counted per 1/8 s */
        saw_patch(0);
        e.patch.b[16] = 0; e.patch.b[17] = 64; e.patch.b[25] = 127;   /* LFO1 WAVE 0, RATE 64; PITCH LFO1 depth full */
        mv_engine_note_on(&e, 69, 100);
        run(fs / 5u, 0, 0, 0);
        {
            uint32_t seg[32], s, ups = 0;
            for (s = 0; s < 32u; s++) run(fs / 8u, &seg[s], 0, 0);
            for (s = 1; s + 1u < 32u; s++) if (seg[s] > seg[s - 1u] && seg[s] >= seg[s + 1u]) ups++;
            lfo_hz[k] = ups / 4.0;                 /* the wobble's peaks over 4 s */
        }
        /* the delay: its first echo, in seconds */
        echo_s[k] = (double)echo_frame() / fs;
        /* the corners at this rate (mv_voice_test's): every filter mode at reso 127, cutoff 0 / 127, full depths, the
         * reverb at full, four voices; nothing wraps, everything ends */
        for (r = 0; r < 6u; r++) {
            int32_t pk = 0;
            uint32_t i;
            mv_engine_init(&e);
            e.patch.b[39] = (uint8_t)(r / 2u); e.patch.b[40] = (uint8_t)(r & 1u); e.patch.b[42] = 127;
            e.patch.b[41] = (uint8_t)(r & 1u ? 127 : 0); e.patch.b[30] = 0; e.patch.b[31] = 127;
            e.patch.b[44] = 127; e.patch.b[45] = 127; e.patch.b[43] = 127; e.patch.b[52] = 127; e.patch.b[53] = 127;
            e.patch.b[61] = 5; e.patch.b[62] = 127; e.patch.b[63] = 4; e.patch.b[64] = 127; e.patch.b[65] = 127; e.patch.b[66] = 127;
            e.patch.b[57] = 0;
            for (i = 0; i < MV_RV_N; i++) e.rvb[i] = 127;
            e.rvb[MV_RV_FREEZE] = 0;
            mv_engine_note_on(&e, 36, 127); mv_engine_note_on(&e, 60, 127); mv_engine_note_on(&e, 84, 127);
            for (i = 0; i < fs / 256u; i++) {
                uint32_t x;
                memset(buf, 0, sizeof buf);
                mv_engine_render(&e, buf, 256);
                for (x = 0; x < 512u; x++) { int32_t a = buf[x] < 0 ? -buf[x] : buf[x]; if (a > pk) pk = a; }
            }
            CHECK(pk <= 32767, "%u Hz mode %u: peak %d (the limiter holds the output)", fs, r, pk);
            mv_engine_note_off(&e, 36); mv_engine_note_off(&e, 60); mv_engine_note_off(&e, 84);
            e.rvb[MV_RV_LEVEL] = 0; e.patch.b[66] = 0; e.patch.b[62] = 0;
            run(3u * fs, 0, 0, 0);
            CHECK(mv_engine_quiet(&e), "%u Hz mode %u: all released", fs, r);
        }
    }
    printf("44.1 kHz / 48 kHz: release %.3f / %.3f s, LFO1 %.2f / %.2f Hz, echo %.4f / %.4f s\n",
           rel_s[0], rel_s[1], lfo_hz[0], lfo_hz[1], echo_s[0], echo_s[1]);
    CHECK(rel_s[0] > 0.05 && rel_s[1] > rel_s[0] * 0.98 && rel_s[1] < rel_s[0] * 1.02, "the release lasts as long (2 %%)");
    CHECK(lfo_hz[0] > 0.5 && lfo_hz[1] >= lfo_hz[0] - 0.25 && lfo_hz[1] <= lfo_hz[0] + 0.25, "LFO1 runs as fast");
    CHECK(echo_s[0] > 0.6 && echo_s[0] < 0.66 && echo_s[1] > echo_s[0] - 0.001 && echo_s[1] < echo_s[0] + 0.001,
          "the echo comes as late (630 ms, 1 ms)");
    printf("mv_rate_test: %d of %d checks failed\n", fails, checks);
    return fails != 0;
}
