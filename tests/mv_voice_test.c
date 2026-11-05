/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of firmware/src/mv_voice.c, the v0 engine: notes sound and stop, nothing overflows at the corners
 * (every filter mode at reso 127, cutoff 0 and 127, full mod depths and wheel, four voices), voice stealing,
 * and the pitch of a held note. Host only (uses double to measure). */
#include <math.h>
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
#include "mv_lim.c"
#include "mv_voice.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

static mv_engine_t e;
static int32_t buf[2 * 256];

/* render ms milliseconds; the peak |sample| and the zero crossings (rising) of L */
static int32_t run(uint32_t ms, uint32_t *rises)
{
    uint32_t b, i, nb = ms * 44100u / 1000u / 256u;
    int32_t peak = 0, prev = 0;
    if (rises) *rises = 0;
    for (b = 0; b < nb; b++) {
        memset(buf, 0, sizeof buf);
        mv_engine_render(&e, buf, 256);
        for (i = 0; i < 512; i++) {
            int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak) peak = a;
        }
        for (i = 0; i < 256; i++) {
            if (rises && prev < 0 && buf[2 * i] >= 0) (*rises)++;
            prev = buf[2 * i];
        }
    }
    return peak;
}

int main(void)
{
    uint32_t r, m, w;
    int32_t pk;
    mv_engine_init(&e);
    CHECK(run(100, 0) == 0 && mv_engine_quiet(&e), "silent at start");
    /* a plain SAW, filter open: note A4 sounds at 440 Hz (rising zero crossings over 1 s) */
    e.patch.b[30] = 5; e.patch.b[31] = 0; e.patch.b[23] = 0;   /* OSC1 SAW, balance full OSC1 */
    e.patch.b[41] = 127; e.patch.b[42] = 0;
    mv_engine_note_on(&e, 69, 100);
    run(100, 0);
    run(1000, &r);
    CHECK(r >= 439 && r <= 441, "A4: %u rises in 1 s", r);
    CHECK(mv_engine_sounding(&e, 69) && !mv_engine_quiet(&e), "sounding");
    mv_engine_note_off(&e, 69);
    run(3000, 0);
    CHECK(mv_engine_quiet(&e), "release ends");
    /* the corners: every mode at reso 127, cutoff 0 / 127, full depths, wheel up, four voices + a steal */
    for (m = 0; m < 6; m++)
        for (w = 0; w < 2; w++) {
            e.patch.b[39] = (uint8_t)(m / 2); e.patch.b[40] = (uint8_t)(m & 1);
            e.patch.b[42] = 127; e.patch.b[41] = w ? 127 : 0;
            e.patch.b[30] = (uint8_t)(w ? (m % 2 ? 3 : 0) : m % 2 ? 4 : 1); e.patch.b[31] = 127; e.patch.b[32] = 127;   /* SSAW / JFBK / SQR / TRIMOD */
            e.patch.b[23] = 64; e.patch.b[33] = 1;
            e.patch.b[44] = 127; e.patch.b[45] = 127; e.patch.b[46] = 127; e.patch.b[43] = 127;
            e.patch.b[25] = 127; e.patch.b[26] = 127; e.patch.b[27] = 127; e.patch.b[52] = 127; e.patch.b[53] = 127;
            e.patch.b[245] = 2; e.wheel = 127; e.bend14 = 8191;
            e.nv = 4;                                            /* 4 voices: the fifth note must steal */
            mv_engine_note_on(&e, 36, 127); mv_engine_note_on(&e, 48, 127);
            mv_engine_note_on(&e, 60, 127); mv_engine_note_on(&e, 72, 127);
            mv_engine_note_on(&e, 96, 127);                      /* a fifth note: steals the oldest */
            pk = run(1500, 0);
            CHECK(pk < (1 << 23), "mode %u cut %u: peak %d (int32 headroom, no wrap)", m, w * 127, pk);
            CHECK(mv_engine_sounding(&e, 96) && !mv_engine_sounding(&e, 36), "steal the oldest (mode %u)", m);
            mv_engine_note_off(&e, 48); mv_engine_note_off(&e, 60); mv_engine_note_off(&e, 72); mv_engine_note_off(&e, 96);
            e.patch.b[57] = 0;                                   /* short release */
            run(2000, 0);
            CHECK(mv_engine_quiet(&e), "mode %u: all released", m);
        }
    {   /* SYNC: OSC2 alone (+7 semitones, sync on) must repeat at OSC1's period. Energy at the harmonics of
         * OSC1 (440 Hz) against all energy, over 0.5 s: ~1 when locked, far less when free */
        static int16_t rec[22050];
        uint32_t s1, k2, j, fr;
        double on = 0, all = 0;
        for (s1 = 0; s1 < 2; s1++) {
            mv_engine_init(&e);
            e.patch.b[30] = 5; e.patch.b[33] = 1; e.patch.b[23] = 127;   /* SAW / SAW, balance full OSC2 */
            e.patch.b[35] = 32; e.patch.b[34] = (uint8_t)s1;              /* RANGE +7, SYNC */
            e.patch.b[41] = 127; e.patch.b[57] = 0;
            mv_engine_note_on(&e, 69, 100);
            run(200, 0);
            for (j = 0; j < 22050u / 256u; j++) {
                memset(buf, 0, sizeof buf);
                mv_engine_render(&e, buf, 256);
                for (fr = 0; fr < 256; fr++) rec[j * 256 + fr] = (int16_t)(buf[2 * fr] >> 2);
            }
            on = all = 0;
            for (k2 = 1; k2 <= 2000; k2++) {     /* DFT at 2 Hz steps up to 4 kHz (0.5 s window: 2 Hz bins) */
                double re = 0, im = 0, f = k2 * 2.0, pw;
                for (j = 0; j < 22016u; j++) {
                    double ph = 2 * 3.141592653589793 * f * j / 44100.0;
                    re += rec[j] * cos(ph); im += rec[j] * sin(ph);
                }
                pw = re * re + im * im;
                all += pw;
                if (k2 % 220u == 0u) on += pw;
            }
            printf("sync %s: %.1f %% of the energy on 440 Hz harmonics\n", s1 ? "on " : "off", 100 * on / all);
            CHECK(s1 ? on / all > 0.9 : on / all < 0.5, "sync %u locks OSC2 to OSC1 (%.3f)", s1, on / all);
        }
    }
    {   /* X-MOD and RING at full, every OSC2 wave, sync on and off: no overflow, sound, release */
        uint32_t w2, sy;
        for (w2 = 0; w2 < 4; w2++)
            for (sy = 0; sy < 2; sy++) {
                mv_engine_init(&e);
                e.patch.b[30] = 1; e.patch.b[33] = (uint8_t)w2; e.patch.b[22] = 127; e.patch.b[21] = 1;
                e.patch.b[34] = (uint8_t)sy; e.patch.b[41] = 127; e.patch.b[57] = 0; e.patch.b[23] = 64;
                mv_engine_note_on(&e, 24, 127); mv_engine_note_on(&e, 108, 127);
                pk = run(500, 0);
                CHECK(pk > 100 && pk < (1 << 20), "X-MOD / ring osc2 %u sync %u: peak %d", w2, sy, pk);
                mv_engine_note_off(&e, 24); mv_engine_note_off(&e, 108);
                run(1500, 0);
                CHECK(mv_engine_quiet(&e), "X-MOD release");
            }
    }
    {   /* NOISE (provisional law): its resonant peak sits at the scene 17 cutoff, fc = 14.4 kHz x 2^((c1-127)/10.6),
         * and stays bounded at resonance 127 over the whole cutoff range */
        static const uint32_t C1[] = {80, 96, 112};
        uint32_t c, i, k2, fr;
        for (c = 0; c < 3; c++) {
            static int32_t rec[44100];
            double want = 14400.0 * pow(2.0, ((double)C1[c] - 127.0) / 10.6), best = 0, bestf = 0, avg = 0;
            mv_engine_init(&e);
            e.patch.b[30] = 2; e.patch.b[31] = (uint8_t)C1[c]; e.patch.b[32] = 64;   /* OSC1 NOISE, reso 64 */
            e.patch.b[23] = 0; e.patch.b[41] = 127; e.patch.b[42] = 0;                /* OSC1 only, main filter open */
            mv_engine_note_on(&e, 60, 100);
            for (i = 0; i < 44100u / 256u; i++) {
                memset(buf, 0, sizeof buf);
                mv_engine_render(&e, buf, 256);
                for (fr = 0; fr < 256; fr++) rec[i * 256 + fr] = buf[2 * fr];
            }
            for (k2 = 0; k2 < 120; k2++) {      /* power at 120 log-spaced frequencies, 300 Hz .. 12 kHz */
                double f = 300.0 * pow(40.0, k2 / 119.0), re = 0, im = 0, pw;
                for (i = 4410; i < 44032; i++) {
                    double ph = 2 * 3.141592653589793 * f * i / 44100.0;
                    re += rec[i] * cos(ph); im += rec[i] * sin(ph);
                }
                pw = re * re + im * im;
                avg += pw / 120;
                if (pw > best) { best = pw; bestf = f; }
            }
            printf("noise c1 %3u reso 64: peak at %5.0f Hz (law %5.0f Hz), %.0f dB over the average\n", C1[c], bestf,
                   want, 10 * log10(best / avg));
            CHECK(fabs(log2(bestf / want)) < 0.25, "noise c1 %u: peak %.0f Hz vs %.0f", C1[c], bestf, want);
        }
        for (c = 0; c < 128; c += 9) {          /* reso 127: bounded at every cutoff */
            mv_engine_init(&e);
            e.patch.b[30] = 2; e.patch.b[31] = (uint8_t)c; e.patch.b[32] = 127; e.patch.b[23] = 0;
            e.patch.b[33] = 3; e.patch.b[37] = (uint8_t)(127 - c); e.patch.b[38] = 127;   /* OSC2 NOISE too */
            e.patch.b[23] = 64; e.patch.b[41] = 127;
            mv_engine_note_on(&e, 60, 100);
            pk = run(1000, 0);
            CHECK(pk > 10 && pk < (1 << 21), "noise reso 127, c1 %u: peak %d", c, pk);
        }
    }
    {   /* TONE vs the JT's measured response (tone-ctrl/delivery-2026-10-23: its sine test at 48 kHz): a sine
         * through mv_tone, gain in dB after 0.5 s of settling */
        static const struct { uint32_t b, t; double hz, want; } T[] = {
            {127, 64, 50, 18.39}, {0, 64, 200, -7.74}, {64, 127, 10000, 13.90}, {64, 0, 2000, -6.78}, {64, 64, 1000, 0}};
        uint32_t c, i;
        for (c = 0; c < 5; c++) {
            static int32_t x[44100];
            double si = 0, so = 0, g;
            mv_engine_init(&e);
            for (i = 0; i < 44100; i++) x[i] = (int32_t)(3000 * sin(2 * 3.141592653589793 * T[c].hz * i / 44100.0));
            for (i = 0; i < 44100; i += 16) mv_tone(&e, x + i, 16, T[c].b, T[c].t);
            for (i = 22050; i < 44100; i++) {
                double v = 3000 * sin(2 * 3.141592653589793 * T[c].hz * i / 44100.0);
                si += v * v; so += (double)x[i] * x[i];
            }
            g = 10 * log10(so / si);
            printf("tone bass %3u treble %3u at %5.0f Hz: %+6.2f dB (JT %+6.2f)\n", T[c].b, T[c].t, T[c].hz, g, T[c].want);
            CHECK(fabs(g - T[c].want) < 0.5, "tone case %u: %.2f dB vs %.2f", c, g, T[c].want);
        }
    }
    {   /* FILTER ENV depth (JE scene 17 block D): LPF-12 at cutoff 0 with FENV +63 and sustain 127 sounds like the
         * open filter (+63 = +124 codes); with depth 0 it is near silent. Level of a SAW on note 60, 0.5 .. 1.0 s */
        static const uint8_t CUT[3] = {0, 0, 127}, DEP[3] = {127, 64, 64};
        double lv[3];
        uint32_t c, i, fr;
        for (c = 0; c < 3; c++) {
            double acc = 0;
            mv_engine_init(&e);
            e.patch.b[30] = 5; e.patch.b[23] = 0;                                    /* OSC1 SAW only */
            e.patch.b[39] = 2; e.patch.b[40] = 0; e.patch.b[41] = CUT[c]; e.patch.b[42] = 0;   /* LPF-12, reso 0 */
            e.patch.b[46] = DEP[c]; e.patch.b[47] = 0; e.patch.b[48] = 64; e.patch.b[49] = 127; e.patch.b[50] = 0;
            mv_engine_note_on(&e, 60, 100);
            for (i = 0; i < 44100u / 256u; i++) {
                memset(buf, 0, sizeof buf);
                mv_engine_render(&e, buf, 256);
                if (i >= 86u) for (fr = 0; fr < 256; fr++) acc += (double)buf[2 * fr] * buf[2 * fr];
            }
            lv[c] = 10.0 * log10(acc + 1.0);
        }
        printf("filter env +63 at cutoff 0: %+.1f dB re the open filter (depth 0 at cutoff 0: %+.1f dB)\n",
               lv[0] - lv[2], lv[1] - lv[2]);
        CHECK(fabs(lv[0] - lv[2]) < 1.0, "FENV +63 opens a closed filter: %+.1f dB re open", lv[0] - lv[2]);
        CHECK(lv[1] - lv[2] < -40.0, "cutoff 0, no env: closed (%+.1f dB re open)", lv[1] - lv[2]);
    }
    {   /* PLAY MODES */
        uint32_t v, n;
        int32_t lv;
        mv_engine_init(&e);
        e.patch.b[30] = 5; e.patch.b[41] = 127; e.patch.b[54] = 0; e.patch.b[55] = 0; e.patch.b[56] = 127; e.patch.b[57] = 0;
        e.patch.b[71] = 1;                                      /* MONO, legato off */
        mv_engine_note_on(&e, 60, 100); run(100, 0);
        mv_engine_note_on(&e, 64, 100); run(100, 0);
        CHECK(mv_engine_active(&e) == 1 && e.v[0].note == 64, "mono: one voice, last note (%u voices, note %u)",
              mv_engine_active(&e), e.v[0].note);
        mv_engine_note_off(&e, 64); run(100, 0);
        CHECK(e.v[0].gate && e.v[0].note == 60, "mono: release returns to the held key");
        mv_engine_note_off(&e, 60); run(500, 0);
        CHECK(mv_engine_quiet(&e), "mono: last key up ends the note");
        e.patch.b[55] = 64; e.patch.b[56] = 64;                 /* in DECAY after 300 ms: a retrigger would show */
        mv_engine_note_on(&e, 60, 100); mv_engine_note_on(&e, 64, 100); run(300, 0);
        lv = e.v[0].aenv.level;
        mv_engine_note_off(&e, 60); run(6, 0);                  /* the older, silent key up: no effect */
        CHECK(e.v[0].note == 64 && e.v[0].gate && e.v[0].aenv.level <= lv,
              "mono: releasing an older key leaves the sounding note alone (no retrigger)");
        mv_engine_note_off(&e, 64); e.patch.b[55] = 0; e.patch.b[56] = 127; run(500, 0);
        CHECK(mv_engine_quiet(&e), "mono: then the last key up ends the note");
        e.patch.b[72] = 1; e.patch.b[54] = 100;                 /* LEGATO, slow attack: a retrigger would show */
        mv_engine_note_on(&e, 60, 100); run(300, 0);
        lv = e.v[0].aenv.level;
        mv_engine_note_on(&e, 67, 100); run(6, 0);
        CHECK(e.v[0].note == 67 && e.v[0].aenv.level >= lv && e.v[0].aenv.stage == MV_ENV_ATTACK,
              "legato: pitch moves, the attack carries on (no restart)");
        mv_engine_note_off(&e, 67); mv_engine_note_off(&e, 60); e.patch.b[54] = 0; run(500, 0);
        {   /* PORTAMENTO: 48 -> 72 at time byte 64 = the JT's 2 ms x 2500^(64/127) = 103 ms: after that time the
             * remaining frequency distance is 1/e of the jump (a constant-ratio slew), within one block */
            double t_ms = 2.0 * pow(2500.0, 64.0 / 127.0), f0, f1, fr;
            uint32_t blocks = (uint32_t)(t_ms / 1000.0 * 44100.0 / 16.0 + 0.5), k;
            e.patch.b[72] = 0; e.patch.b[69] = 1; e.patch.b[70] = 64;
            mv_engine_note_on(&e, 48, 100); run(1500, 0); mv_engine_note_off(&e, 48);   /* (48 settles first) */
            mv_engine_note_on(&e, 72, 100);
            for (k = 0; k < blocks; k++) {      /* one control block at a time */
                memset(buf, 0, sizeof buf);
                mv_engine_render(&e, buf, 16);
            }
            f0 = mv_pitch_inc(48 * 256); f1 = mv_pitch_inc(72 * 256);
            fr = (f1 - e.v[0].g_cur) / (f1 - f0);
            printf("portamento byte 64 (%.0f ms): %.3f of the distance left after one time constant (want 0.368)\n", t_ms, fr);
            CHECK(fabs(fr - exp(-1.0)) < 0.02, "porta time constant: %.3f left", fr);
            run(2000, 0);
            CHECK(!e.v[0].g_on && e.v[0].g_cur == (uint32_t)f1, "porta arrives and stops");
            mv_engine_note_off(&e, 72); run(500, 0);
        }
        e.patch.b[69] = 0; e.patch.b[71] = 0;
        e.patch.b[243] = 1; e.patch.b[244] = 50;                /* UNISON, full detune */
        mv_engine_note_on(&e, 57, 100); run(100, 0);
        n = mv_engine_active(&e);
        CHECK(n == e.nv, "unison: every voice sounds (%u of %u)", n, e.nv);
        CHECK(e.v[0].det_q8 == -128 && e.v[e.nv - 1u].det_q8 == 128, "unison: spread -/+0.5 semitone at detune 50 (%d %d)",
              e.v[0].det_q8, e.v[e.nv - 1u].det_q8);
        for (v = 1; v < e.nv; v++) CHECK(e.v[v].o1.inc > e.v[v - 1u].o1.inc, "unison: voices spread in pitch (%u)", v);
        mv_engine_note_off(&e, 57); run(500, 0);
        CHECK(mv_engine_quiet(&e), "unison: key up releases all");
        mv_engine_note_on(&e, 60, 100); run(50, 0);
        e.patch.b[243] = 0;                                     /* a mode change while sounding */
        mv_engine_note_on(&e, 62, 100); run(50, 0);
        CHECK(mv_engine_active(&e) == 1 && e.v[0].det_q8 == 0, "mode change: the unison voices out, poly from clean");
    }
    {   /* EFFECTS (mv_fx.c; the DSP itself is graded against the JT by tools/compare/cmp_fx.cpp) */
        uint32_t i, diff = 0;
        mv_engine_init(&e);
        e.patch.b[30] = 5; e.patch.b[41] = 127; e.patch.b[57] = 0;   /* SAW, filter open, AMP release 0 */
        mv_engine_note_on(&e, 57, 100); run(100, 0);
        for (i = 0; i < 256; i++) diff |= (uint32_t)(buf[2 * i] != buf[2 * i + 1]);
        CHECK(!diff, "FX off (INIT levels 0): mono, L = R");
        e.patch.b[62] = 127; run(100, 0);                        /* SUPER CHORUS SLW at full level */
        for (i = 0, diff = 0; i < 256; i++) diff |= (uint32_t)(buf[2 * i] != buf[2 * i + 1]);
        CHECK(diff && e.fx.on_mfx, "Multi-FX on: stereo (R = x - wet)");
        e.patch.b[62] = 0;
        e.patch.b[63] = 4; e.patch.b[64] = 127; e.patch.b[65] = 0; e.patch.b[66] = 100;   /* MONO LONG, full time */
        run(20, 0);
        CHECK(e.fx.d_tl == 225792000 && e.fx.d_tr == 225792000, "MONO LONG 127: 1250 ms = 55125 samples (%d %d, Q12)",
              e.fx.d_tl, e.fx.d_tr);
        e.patch.b[63] = 0;                                       /* PAN L->R: 625 ms, R x 250/120 */
        run(20, 0);
        CHECK(e.fx.d_tl == 112896000 && e.fx.d_tr == 235200000, "PAN L->R 127: 625 / 1302 ms (%d %d)", e.fx.d_tl, e.fx.d_tr);
        e.patch.b[63] = 3; e.patch.b[64] = 64; e.patch.b[65] = 100;   /* MONO SHORT, ~79 ms, feedback 0.79 */
        run(300, 0);
        mv_engine_note_off(&e, 57); run(50, 0);
        CHECK(mv_engine_active(&e) == 0, "the voice has ended (release 0)");
        pk = run(200, 0);
        CHECK(pk > 100 && !mv_engine_quiet(&e), "the delay tail sounds after the last voice (peak %d)", pk);
        run(6000, 0);
        CHECK(mv_engine_quiet(&e) && run(100, 0) == 0, "the tail dies away and the engine rests again");
        /* the limiter: 8 voices, full level, +12 dB patch gain, delay feedback up: never past 0.97 */
        e.patch.b[245] = 2; e.patch.b[51] = 127; e.patch.b[65] = 127; e.patch.b[62] = 127;
        for (i = 0; i < 8u; i++) mv_engine_note_on(&e, 40 + 5 * i, 127);
        pk = run(1500, 0);
        CHECK(pk <= 31785, "limiter: peak %d <= 0.97 of full scale", pk);
        for (i = 0; i < 8u; i++) mv_engine_note_off(&e, 40 + 5 * i);
    }
    {   /* REVERB (mv_reverb.c; its DSP graded against the JT plate by tools/compare/cmp_rv.cpp) */
        uint32_t i, diff = 0;
        mv_engine_init(&e);
        e.patch.b[30] = 5; e.patch.b[41] = 127; e.patch.b[57] = 0;   /* SAW, filter open, AMP release 0 */
        CHECK(!e.rv.on && e.rvb[MV_RV_LEVEL] == 0, "reverb off at start (settings not loaded)");
        e.rvb[MV_RV_LEVEL] = 100; e.rvb[MV_RV_SIZE] = 64;
        mv_engine_note_on(&e, 57, 100); run(300, 0);
        for (i = 0; i < 256; i++) diff |= (uint32_t)(buf[2 * i] != buf[2 * i + 1]);
        CHECK(e.rv.on && diff, "reverb on: the plate's stereo taps (L != R)");
        mv_engine_note_off(&e, 57); run(50, 0);
        CHECK(mv_engine_active(&e) == 0, "the voice has ended");
        pk = run(300, 0);
        CHECK(pk > 50 && !mv_engine_quiet(&e), "the reverb tail sounds after the last voice (peak %d)", pk);
        run(15000, 0);
        CHECK(mv_engine_quiet(&e) && run(100, 0) == 0, "the tail dies away to silence and the engine rests");
        e.rvb[MV_RV_SIZE] = 100;                                 /* FREEZE holds a tail */
        mv_engine_note_on(&e, 57, 100); run(300, 0); mv_engine_note_off(&e, 57); run(100, 0);
        e.rvb[MV_RV_FREEZE] = 1; run(100, 0);
        pk = run(2000, 0);
        CHECK(pk > 50 && !mv_engine_quiet(&e), "FREEZE: the tail holds (peak %d after 2 s)", pk);
        e.rvb[MV_RV_FREEZE] = 0; e.rvb[MV_RV_LEVEL] = 0; run(50, 0);
        CHECK(!e.rv.on && mv_engine_quiet(&e) && run(100, 0) == 0, "LEVEL 0: off, the engine rests");
    }
    {   /* ARP in the engine (mv_arp.c's own logic: tests/mv_arp_test.c): keys feed it, it plays the voices */
        uint32_t k, v;
        mv_engine_init(&e);
        e.patch.b[30] = 5; e.patch.b[41] = 127; e.patch.b[57] = 0;
        e.arpb[MV_ARP_ON] = 1; e.arp_beat = 22050u;              /* 120 BPM, 1/16, UP, gate 64 */
        run(10, 0);
        CHECK(e.arp_live, "arp on");
        mv_engine_note_on(&e, 60, 100); mv_engine_note_on(&e, 64, 100); mv_engine_note_on(&e, 67, 100);
        run(140, 0);                            /* a little over one 1/16 step (125 ms at 120 BPM) */
        CHECK(mv_engine_active(&e) >= 1u, "the arp plays a voice from the first key on");
        for (v = 0, k = 0; v < MV_NV; v++) k += e.v[v].gate;
        CHECK(k <= 1u, "one arp note at a time (UP): %u gated", k);
        run(500, 0);
        CHECK(e.arp.nheld == 3u && e.arp.idx >= 3u, "keys held: the arp stepped through them (step %u)", e.arp.idx);
        e.arpb[MV_ARP_ON] = 0; run(20, 0);
        for (v = 0, k = 0; v < MV_NV; v++) k += e.v[v].gate;
        CHECK(!e.arp_live && k == 0u, "arp off: its note ends");
        mv_engine_note_off(&e, 60); mv_engine_note_off(&e, 64); mv_engine_note_off(&e, 67);
        run(2000, 0);
        CHECK(mv_engine_quiet(&e), "and the engine rests");
    }
    {   /* S2 (Kris 2 Nov): a voice still sounding keeps its oscillator phases on a new note (no waveform jump, no
         * click); from silence it starts at fresh random ones (the JT's anti-lockstep) */
        uint32_t ph1, ph2, ss0;
        mv_engine_init(&e);
        e.patch.b[30] = 0; e.patch.b[41] = 127; e.patch.b[71] = 1;   /* SUPER SAW, MONO */
        mv_engine_note_on(&e, 60, 100); run(50, 0);
        ph1 = e.v[0].o1.ph; ph2 = e.v[0].o2.ph; ss0 = e.v[0].ss.ph[3];
        mv_engine_note_on(&e, 64, 100); mv_ev_drain(&e);           /* MONO: the next note on the same voice */
        CHECK(e.v[0].note == 64u && e.v[0].o1.ph == ph1 && e.v[0].o2.ph == ph2 && e.v[0].ss.ph[3] == ss0,
              "MONO next note: the phases run on (no jump)");
        e.patch.b[71] = 0; e.nv = 1;            /* POLY, one voice: the next note steals the sounding one */
        mv_engine_note_off(&e, 64); mv_engine_note_on(&e, 67, 100); run(50, 0);
        ph1 = e.v[0].o1.ph;
        mv_engine_note_on(&e, 72, 100); mv_ev_drain(&e);
        CHECK(e.v[0].note == 72u && e.v[0].o1.ph == ph1, "a steal of a sounding voice: the phases run on");
        mv_engine_note_off(&e, 72); run(3000, 0);
        CHECK(mv_engine_quiet(&e), "silent again");
        ph1 = e.v[0].o1.ph;
        mv_engine_note_on(&e, 60, 100); mv_ev_drain(&e);
        CHECK(e.v[0].o1.ph != ph1, "from silence: fresh random phases");
    }
    {   /* PERFORMANCE (Kris 3 Nov, the JT's Performance): DUAL, SPLIT, the voice split, a mono part beside a poly
         * one, the arp on one part, BALANCE */
        uint32_t v, nlo, nup;
#define COUNT(PART) do { nlo = nup = 0; for (v = 0; v < MV_NV; v++) if (e.v[v].gate) { if (e.v[v].part) nlo++; else nup++; } } while (0)
        mv_engine_init(&e);
        e.patch.b[30] = 5; e.patch.b[41] = 127; e.lower = e.patch; e.lower.b[30] = 4;   /* UPPER SAW, LOWER SQR */
        e.perf[MV_PF_MODE] = MV_KEY_DUAL;       /* 4 + 4 */
        mv_engine_note_on(&e, 60, 100); run(20, 0); COUNT(0);
        CHECK(nup == 1u && nlo == 1u, "DUAL: a key sounds both parts (%u upper, %u lower)", nup, nlo);
        CHECK(e.part[MV_PART_LO].nvp == 4u && e.part[MV_PART_UP].nvp == 4u && e.part[MV_PART_UP].v0 == 4u,
              "DUAL: the voices cut 4 + 4");
        mv_engine_note_off(&e, 60); run(500, 0);
        e.perf[MV_PF_MODE] = MV_KEY_SPLIT; e.perf[MV_PF_SPLIT] = 60;
        mv_engine_note_on(&e, 48, 100); mv_engine_note_on(&e, 72, 100); run(20, 0); COUNT(0);
        CHECK(nup == 1u && nlo == 1u && mv_engine_sounding(&e, 48) && mv_engine_sounding(&e, 72) &&
              e.v[e.part[MV_PART_LO].v0].part == MV_PART_LO,
              "SPLIT: below 60 LOWER, from 60 UPPER (%u / %u)", nlo, nup);
        mv_engine_note_on(&e, 60, 100); run(20, 0); COUNT(0);
        CHECK(nup == 2u && nlo == 1u, "SPLIT: the split key itself is UPPER's");
        mv_engine_note_off(&e, 48); mv_engine_note_off(&e, 72); mv_engine_note_off(&e, 60); run(1500, 0);
        e.perf[MV_PF_VSPLIT] = 1;               /* 2 + 6: three low keys share LOWER's two voices */
        mv_engine_note_on(&e, 40, 100); mv_engine_note_on(&e, 43, 100); mv_engine_note_on(&e, 47, 100);
        mv_engine_note_on(&e, 64, 100); mv_engine_note_on(&e, 67, 100); mv_engine_note_on(&e, 71, 100);
        run(20, 0); COUNT(0);
        CHECK(nlo == 2u && nup == 3u, "VOICES 2+6: LOWER steals within its 2, UPPER keeps its 3 (%u / %u)", nlo, nup);
        for (v = 40; v < 72; v++) { mv_engine_note_off(&e, v); mv_ev_drain(&e); }   /* (the queue holds 32) */
        run(1500, 0);
        e.perf[MV_PF_VSPLIT] = 3; e.lower.b[71] = 1;   /* LOWER MONO, UPPER POLY */
        mv_engine_note_on(&e, 36, 100); mv_engine_note_on(&e, 40, 100);
        mv_engine_note_on(&e, 64, 100); mv_engine_note_on(&e, 67, 100); mv_engine_note_on(&e, 71, 100);
        run(20, 0); COUNT(0);
        CHECK(nlo == 1u && e.v[0].note == 40u && nup == 3u, "SPLIT: a MONO LOWER (last note 40) beside a POLY UPPER");
        mv_engine_note_off(&e, 40); run(20, 0);
        CHECK(e.v[0].gate && e.v[0].note == 36u, "MONO LOWER: back to the held 36");
        for (v = 30; v < 72; v++) { mv_engine_note_off(&e, v); mv_ev_drain(&e); }
        run(1500, 0);
        e.lower.b[71] = 0;
        e.arpb[MV_ARP_ON] = 1; e.arpb[MV_ARP_RATE] = 2;   /* the arp, AUTO: LOWER in SPLIT, 1/16 at 120 */
        run(20, 0);
        mv_engine_note_on(&e, 48, 100); mv_engine_note_on(&e, 52, 100); mv_engine_note_on(&e, 72, 100);
        {
            uint32_t lo_notes = 0, up_seen = 0, t;
            for (t = 0; t < 40; t++) {          /* 40 x 23 ms: about 7 sixteenths */
                run(23, 0);
                for (v = 0; v < 4u; v++) if (e.v[v].gate) lo_notes |= 1u << (e.v[v].note - 40u);
                for (v = 4; v < MV_NV; v++) if (e.v[v].gate && e.v[v].note == 72u) up_seen++;
            }
            CHECK(lo_notes == (1u << 8 | 1u << 12) && up_seen == 40u,
                  "the arp on LOWER: 48 and 52 arpeggiate, 72 holds on UPPER (lo %x, up %u)", lo_notes, up_seen);
        }
        mv_engine_note_off(&e, 48); mv_engine_note_off(&e, 52); mv_engine_note_off(&e, 72);
        e.arpb[MV_ARP_ON] = 0; run(1500, 0);
        CHECK(mv_engine_quiet(&e), "silent after the arp");
        {   /* BALANCE: DUAL at 127 leaves LOWER out (UPPER alone), at 0 UPPER out; 64 both */
            int32_t pk_both, pk_up, pk_lo;
            e.perf[MV_PF_MODE] = MV_KEY_DUAL; e.lower.b[51] = 127; e.patch.b[51] = 127;
            e.perf[MV_PF_BAL] = 64; mv_engine_note_on(&e, 60, 100); run(100, 0); pk_both = run(200, 0);
            e.perf[MV_PF_BAL] = 127; run(100, 0); pk_up = run(200, 0);
            e.perf[MV_PF_BAL] = 0; run(100, 0); pk_lo = run(200, 0);
            mv_engine_note_off(&e, 60); run(1500, 0);
            CHECK(pk_up > 0 && pk_lo > 0 && pk_both > pk_up && pk_both > pk_lo,
                  "BALANCE: both at 64 (%d), UPPER at 127 (%d), LOWER at 0 (%d)", pk_both, pk_up, pk_lo);
        }
        e.perf[MV_PF_MODE] = MV_KEY_SINGLE;
        mv_engine_note_on(&e, 60, 100); run(20, 0); COUNT(0);
        CHECK(nup == 1u && nlo == 0u && e.part[MV_PART_UP].nvp == MV_NV, "back to SINGLE: UPPER on all 8");
        mv_engine_note_off(&e, 60); run(1500, 0);
#undef COUNT
    }
    {   /* LFO1 SYNC (Kris 5 Nov): a part's sync byte puts the tempo's step in place of RATE's; it follows the beat,
         * each part its own (DUAL: LOWER synced, UPPER free), and OFF gives RATE back */
        uint32_t inc4 = mv_lfo_sync_inc(7, 22050);
        e.perf[MV_PF_MODE] = MV_KEY_SINGLE;
        e.patch.b[17] = 0; e.lower.b[17] = 0;   /* LFO1 RATE 0: the slowest */
        e.arp_beat = 22050;                     /* 120 BPM */
        e.perf[MV_PF_SYNCUP] = 7;               /* 1/4 */
        mv_engine_sync(&e);
        CHECK(e.lfo1_sync_inc[0] == inc4 && e.lfo1_sync_inc[1] == 0, "sync 1/4: the step made for UPPER only");
        mv_engine_sync(&e);
        CHECK(e.sync_beat[0] == 22050u && e.lfo1_sync_inc[0] == inc4, "nothing moved: the same step (no divide)");
        /* clock jitter (Kris 5 Nov, fix 2): within 1/512 of the beat the step is held, beyond it the step follows */
        e.arp_beat = 22050u + 43u; mv_engine_sync(&e);   /* +0.195 %: 22050 >> 9 = 43 */
        CHECK(e.sync_beat[0] == 22050u && e.lfo1_sync_inc[0] == inc4, "beat +43 samples (jitter): the step held");
        e.arp_beat = 22050u - 43u; mv_engine_sync(&e);
        CHECK(e.sync_beat[0] == 22050u && e.lfo1_sync_inc[0] == inc4, "beat -43 samples (jitter): the step held");
        e.arp_beat = 22050u + 44u; mv_engine_sync(&e);   /* just over: a real tempo change */
        CHECK(e.sync_beat[0] == 22094u && e.lfo1_sync_inc[0] == mv_lfo_sync_inc(7, 22094),
              "beat +44 samples: the step follows");
        e.arp_beat = 22050u; mv_engine_sync(&e);   /* back to 120 BPM for the checks below */
        CHECK(e.lfo1_sync_inc[0] == inc4, "back at 22050: the 1/4 step again");
        mv_engine_note_on(&e, 60, 100); run(50, 0);
        CHECK(e.part[0].lfo1.inc == inc4 && inc4 != MV_LFO1_INC[0], "sync on: UPPER's LFO1 runs at 1/4, not RATE's");
        e.patch.b[17] = 127; run(20, 0);
        CHECK(e.part[0].lfo1.inc == inc4, "sync on: RATE ignored");
        e.arp_beat = 11025; mv_engine_sync(&e); run(20, 0);   /* 240 BPM: twice as fast */
        CHECK(e.part[0].lfo1.inc == mv_lfo_sync_inc(7, 11025) && e.part[0].lfo1.inc > inc4 + inc4 - 2u &&
              e.part[0].lfo1.inc < inc4 + inc4 + 2u, "the step follows the beat: 240 BPM = 2 x 120");
        e.perf[MV_PF_SYNCUP] = 0; mv_engine_sync(&e); run(20, 0);
        CHECK(e.lfo1_sync_inc[0] == 0 && e.part[0].lfo1.inc == MV_LFO1_INC[127], "sync OFF: RATE 127's step again");
        mv_engine_note_off(&e, 60); run(1500, 0);
        e.perf[MV_PF_MODE] = MV_KEY_DUAL; e.perf[MV_PF_SYNCLO] = 13;   /* LOWER at 1/1, UPPER free */
        e.arp_beat = 22050; mv_engine_sync(&e);
        mv_engine_note_on(&e, 60, 100); run(50, 0);
        CHECK(e.part[1].lfo1.inc == mv_lfo_sync_inc(13, 22050) && e.part[0].lfo1.inc == MV_LFO1_INC[127],
              "DUAL: LOWER synced at 1/1, UPPER free on its RATE");
        mv_engine_note_off(&e, 60); run(1500, 0);
        e.perf[MV_PF_MODE] = MV_KEY_SINGLE; e.perf[MV_PF_SYNCLO] = 0; mv_engine_sync(&e);
    }
    {   /* the FX limiter (Kris 5 Nov, 1a: fm1-port/mvance-azs-crackle-diagnosis-2026-11-05.md): DUAL, both parts at
         * PATCH GAIN +12 dB with the chorus on, a 4-note chord (the AZS bank's case): the bus runs several times over
         * full scale. The look-ahead limiter takes it: no sample past the ceiling, no flat-topped run (a clamp), and
         * once released the tail and the limiter's held block play out to silence */
        uint32_t b2, i, flat = 0, run_n = 0, over = 0;
        int32_t prev = 0, peak = 0;
        e.perf[MV_PF_MODE] = MV_KEY_DUAL; e.perf[MV_PF_BAL] = 64;
        e.patch.b[245] = 2; e.lower.b[245] = 2;         /* +12 dB */
        e.patch.b[51] = 127; e.lower.b[51] = 127;
        e.patch.b[61] = 0; e.patch.b[62] = 100;          /* MULTI-FX SC SLW at 100 (the FX chain is UPPER's) */
        e.patch.b[41] = 127; e.lower.b[41] = 127;
        mv_engine_note_on(&e, 60, 127); mv_engine_note_on(&e, 64, 127);
        mv_engine_note_on(&e, 67, 127); mv_engine_note_on(&e, 72, 127);
        for (b2 = 0; b2 < 200u; b2++) {                 /* ~1.2 s */
            memset(buf, 0, sizeof buf);
            mv_engine_render(&e, buf, 256);
            for (i = 0; i < 512u; i++) {
                int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
                if (a > peak) peak = a;
                if (a > 31784) over++;
                if (i & 1u) continue;                   /* L: a flat run at the top = a clamp */
                if (a >= 31000 && buf[i] == prev) { if (++run_n == 3u) flat++; } else run_n = 0;
                prev = buf[i];
            }
        }
        CHECK(e.fxlim.g < (1 << 29), "the FX limiter works on a hot bus (gain %.2f)", e.fxlim.g / 1073741824.0);
        CHECK(over == 0 && peak <= 31784, "no sample past the ceiling (peak %d, %u over)", peak, over);
        CHECK(flat == 0, "no clamp: %u flat runs at the top", flat);
        mv_engine_note_off(&e, 60); mv_engine_note_off(&e, 64); mv_engine_note_off(&e, 67); mv_engine_note_off(&e, 72);
        run(4000, 0);
        CHECK(mv_engine_quiet(&e) && !mv_lim_pending(&e.fxlim), "after the tail: quiet, nothing held in the FX limiter");
        e.patch.b[245] = 0; e.lower.b[245] = 0; e.patch.b[62] = 0; e.perf[MV_PF_MODE] = MV_KEY_SINGLE;
        run(500, 0);
    }
    printf("mv_voice_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
