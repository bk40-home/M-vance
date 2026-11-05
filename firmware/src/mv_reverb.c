/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance reverb: implementation (flow, laws, memory in mv_reverb.h). Each step cites the JT code it ports
 * (PlateReverb.cpp / .h, ReverbPrimitives.h); tools/compare/cmp_rv.cpp checks it against that code. */
#include "mv_reverb.h"
#include "mv_fixed.h"
#include "mv_rv_tables.h"

#if defined(MV_FS) && MV_RV_FS != MV_FS
#error "mv_reverb.c: MV_RV_FS must equal MV_FS (the engine's rate) in the firmware"
#endif

/* lengths (samples, the JT's v1 values, not rescaled by the JT either) */
#define RV_D0 142u
#define RV_D1 107u
#define RV_D2 379u
#define RV_D3 277u
#define RV_A0 1800u
#define RV_A1 2656u
#define RV_T0 3720u
#define RV_T1 4217u
static const uint16_t RV_DLEN[4] = {RV_D0, RV_D1, RV_D2, RV_D3};
/* output taps: authored at 44.1 kHz, rescaled to the rate as the JT's samplesAtRate (ceiling) */
#define RV_TAP(n) ((uint32_t)(((uint64_t)(n) * MV_RV_FS + 44099u) / 44100u))
#define RV_GAIN_DIFF 911069888u                    /* the allpasses' gain, 0.42425 (Q31: 0.005 + 0.65 x 0.645, float32) */
#define RV_WET_SCALE 644245120u                    /* kWetScale 0.3 (Q31) */
#define RV_MOD_SH 11                               /* +-8 samples: lfo (Q30) >> 11 = samples (Q16) x 8 */
#define RV_QUIET 32                                /* 1 LSB of the Q15 out */
#define RV_QUIET_FRAMES (RV_T0 + RV_T1 + RV_A0 + RV_A1)        /* silent for a whole tank round: the tail has ended */

/* the lines (the platform pool: zeroed at boot) */
static int32_t rv_diff[RV_D0 + RV_D1 + RV_D2 + RV_D3] __attribute__((section(".pool")));        /* Q20 */
/* (one declaration each: a section attribute at the end of a list applies to its last name only) */
static int16_t rv_tapf0[RV_A0] __attribute__((section(".pool")));                      /* tank allpasses, Q14 */
static int16_t rv_tapf1[RV_A1] __attribute__((section(".pool")));
static int16_t rv_tdl0[RV_T0] __attribute__((section(".pool")));                       /* tank delays, Q14 */
static int16_t rv_tdl1[RV_T1] __attribute__((section(".pool")));

/* ---- helpers */

static inline int32_t rv_mul(int32_t a, uint32_t g) { return mv_mul_q31r(a, g); }   /* a x g (Q31), rounded */

/* Q20 -> a tank line's Q14: truncated toward zero (the tail ends in silence), held to +-2.0 (a write past it clamps,
 * never wraps). Q14 not Q13: the compare's error is the lines' resolution (6 dB a bit, measured), and the tank's
 * peak at the longest SIZE stays under 1.8 x the input's: -45 dB re the wet at SIZE 127, -48 .. -56 elsewhere */
static inline int16_t rv_q14(int32_t v)
{
    v = v >= 0 ? v >> 6 : -((-v) >> 6);
    return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

/* DelayLine.read(d): d samples behind the write index w (d <= len) */
static inline uint32_t rv_rd(uint32_t w, uint32_t d, uint32_t len) { return w >= d ? w - d : w + len - d; }

/* OnePole_LP: y = x + a (y - x); a = 0: transparent */
static inline int32_t rv_lp(int32_t x, int32_t *s, uint32_t a, uint32_t *e)
{
    *s = x + mv_mul_q31ef(*s - x, a, e);
    return *s;
}

/* ---- set-up */

void mv_rv_init(mv_rv_t *r)
{
    uint32_t k;
    for (k = 0; k < sizeof *r; k++) ((uint8_t *)r)[k] = 0;
    for (k = 0; k < MV_RV_N; k++) r->key[k] = 0xFF;   /* build everything on the first mv_rv_set */
    r->inc = mv_udiv64_32(4ull << 32, 5u * MV_RV_FS);   /* _modRate 0.8 Hz / fs (updateModRate) */
    r->in_g = 65536;
    r->quiet = ~0u >> 1;
}

void mv_rv_set(mv_rv_t *r, const volatile uint8_t *v)
{
    uint8_t s[MV_RV_N];
    uint32_t k, same = 1;
    for (k = 0; k < MV_RV_N; k++) { s[k] = (uint8_t)(v[k] & 127u); same &= (uint32_t)(s[k] == r->key[k]); }
    if (same) return;                           /* nothing moved: no work */
    r->on = (uint8_t)(s[MV_RV_LEVEL] != 0u);    /* the JT's auto-bypass: mix <= 1e-3 */
    r->mix = (int32_t)((s[MV_RV_LEVEL] * 65536u + 63u) / 127u);   /* linear (reverb.mix) */
    if (s[MV_RV_FREEZE]) {                      /* setFreeze: infinite bright hold, input off (bleed 0) */
        r->decay = 1 << 30; r->hid = 0; r->lod = 0; r->in_g = 0;
    } else {
        r->decay = RV_DECAY[s[MV_RV_SIZE]];
        r->hid = RV_HIDAMP[s[MV_RV_HIDAMP]];
        r->lod = RV_LODAMP[s[MV_RV_LODAMP]];
        r->in_g = 65536;
    }
    r->mlp = RV_MLP[s[MV_RV_LOWPASS]];
    r->mhp = RV_MHP[s[MV_RV_HIPASS]];
    for (k = 0; k < MV_RV_N; k++) r->key[k] = s[k];
}

int mv_rv_ringing(const mv_rv_t *r) { return r->on && r->quiet < RV_QUIET_FRAMES; }

/* ---- the tank (PlateTank::processBlock) */

/* Allpass::process on a diffuser (32-bit line): y = -g x + oldest; write x + g y */
static inline int32_t rv_ap_diff(mv_rv_t *r, uint32_t k, int32_t *buf, int32_t x)
{
    uint32_t len = RV_DLEN[k], w = r->dw[k];
    int32_t y = -rv_mul(x, RV_GAIN_DIFF) + buf[rv_rd(w, len - 1u, len)];
    buf[w] = x + rv_mul(y, RV_GAIN_DIFF);
    r->dw[k] = (uint16_t)(w + 1u == len ? 0u : w + 1u);
    return y;
}

/* Allpass::processModulated on a tank allpass (16-bit line): read len - 1 + mod samples back, between samples */
static inline int32_t rv_ap_tank(uint16_t *wp, int16_t *buf, uint32_t len, int32_t x, int32_t mod_q16)
{
    int32_t d = (int32_t)((len - 1u) << 16) + mod_q16, s0, s1, y;
    uint32_t i, w = *wp;
    d = mv_clamp(d, 1 << 16, (int32_t)((len - 1u) << 16));
    i = (uint32_t)d >> 16;
    s0 = (int32_t)buf[rv_rd(w, i, len)] << 6;
    s1 = (int32_t)buf[rv_rd(w, i + 1u, len)] << 6;
    y = -rv_mul(x, RV_GAIN_DIFF) + s0 + (int32_t)(((int64_t)(s1 - s0) * (d & 0xFFFF)) >> 16);
    buf[w] = rv_q14(x + rv_mul(y, RV_GAIN_DIFF));
    *wp = (uint16_t)(w + 1u == len ? 0u : w + 1u);
    return y;
}

void mv_rv_process(mv_rv_t *r, int32_t *out, uint32_t n)
{
    uint32_t i;
    if (!r->on) return;                         /* LEVEL 0: bypassed, nothing calculated */
    for (i = 0; i < n; i++) {
        int32_t inl = mv_clamp(out[2u * i], -(1 << 25), 1 << 25) << 5;   /* Q15 -> Q20 */
        int32_t inr = mv_clamp(out[2u * i + 1u], -(1 << 25), 1 << 25) << 5;
        int32_t x = (int32_t)(((int64_t)((inl + inr) >> 1) * r->in_g) >> 16), lfo, mod, t0, t1, wl, wr, ol, or_;
        uint32_t w0, w1;
        int64_t tt;
        /* input diffusers */
        x = rv_ap_diff(r, 0, rv_diff, x);
        x = rv_ap_diff(r, 1, rv_diff + RV_D0, x);
        x = rv_ap_diff(r, 2, rv_diff + RV_D0 + RV_D1, x);
        x = rv_ap_diff(r, 3, rv_diff + RV_D0 + RV_D1 + RV_D2, x);
        /* triangleLFO: 4 |p - 0.5| - 1 */
        r->ph += r->inc;
#ifdef MV_RV_CMP_PHASE
        r->ph = MV_RV_CMP_PHASE(r->ph);         /* compare builds only: the JT's float phase */
#endif
        tt = (int64_t)r->ph - 0x80000000ll;
        lfo = (int32_t)((tt < 0 ? -tt : tt) - (1ll << 30));   /* |p - 0.5| x 2^32 = 4 |p - 0.5| (Q30); -1 */
        mod = lfo >> RV_MOD_SH;                    /* x 8 samples, Q16 */
        /* tank half 0, then half 1 (the other half's last filtered output x decay) */
        t0 = (int32_t)(((int64_t)r->fb[1] * r->decay) >> 30) + x;
        t1 = (int32_t)(((int64_t)r->fb[0] * r->decay) >> 30) + x;
        t0 = rv_ap_tank(&r->aw[0], rv_tapf0, RV_A0, t0, mod);
        w0 = r->tw[0];
        rv_tdl0[w0] = rv_q14(t0);
        r->tw[0] = (uint16_t)(w0 + 1u == RV_T0 ? 0u : w0 + 1u);
        r->fb[0] = (int32_t)rv_tdl0[rv_rd(r->tw[0], RV_T0 - 1u, RV_T0)] << 6;
        r->fb[0] = rv_lp(r->fb[0], &r->tlp[0], r->hid, &r->e[0]);
        if (r->lod) r->fb[0] -= rv_lp(r->fb[0], &r->thp[0], r->lod, &r->e[1]);   /* HP = x - LP; bypassed at 0 */
        t1 = rv_ap_tank(&r->aw[1], rv_tapf1, RV_A1, t1, -mod);
        w1 = r->tw[1];
        rv_tdl1[w1] = rv_q14(t1);
        r->tw[1] = (uint16_t)(w1 + 1u == RV_T1 ? 0u : w1 + 1u);
        r->fb[1] = (int32_t)rv_tdl1[rv_rd(r->tw[1], RV_T1 - 1u, RV_T1)] << 6;
        r->fb[1] = rv_lp(r->fb[1], &r->tlp[1], r->hid, &r->e[2]);
        if (r->lod) r->fb[1] -= rv_lp(r->fb[1], &r->thp[1], r->lod, &r->e[3]);
        /* the output taps (a subtractive cross tap widens the image), x 0.3 */
        w0 = r->tw[0]; w1 = r->tw[1];
        wl = ((int32_t)rv_tdl0[rv_rd(w0, RV_TAP(266), RV_T0)] + rv_tdl0[rv_rd(w0, RV_TAP(2974), RV_T0)] - rv_tdl1[rv_rd(w1, RV_TAP(1913), RV_T1)]) << 6;
        wr = ((int32_t)rv_tdl1[rv_rd(w1, RV_TAP(353), RV_T1)] + rv_tdl1[rv_rd(w1, RV_TAP(3627), RV_T1)] - rv_tdl0[rv_rd(w0, RV_TAP(1228), RV_T0)]) << 6;
        wl = rv_mul(wl, RV_WET_SCALE); wr = rv_mul(wr, RV_WET_SCALE);
        if (r->mlp > 2147483u) { wl = rv_lp(wl, &r->olp[0], r->mlp, &r->e[4]); wr = rv_lp(wr, &r->olp[1], r->mlp, &r->e[5]); }
        if (r->mhp > 2147483u) {                /* (> 0.001: the JT's guard) */
            wl -= rv_lp(wl, &r->ohp[0], r->mhp, &r->e[6]);
            wr -= rv_lp(wr, &r->ohp[1], r->mhp, &r->e[7]);
        }
        /* out = in + mix x wet. The JT's soft clip after it (|x| > 1: x / (1 + |x|)) is left out (Kris 2 Nov, C2a): it
         * jumps from 1.0 to 0.5 as it starts (crackles at high note density); the output limiter (mv_lim) follows */
        ol = inl + (int32_t)(((int64_t)r->mix * wl) >> 16);
        or_ = inr + (int32_t)(((int64_t)r->mix * wr) >> 16);
        out[2u * i] = (ol + 16) >> 5;           /* Q20 -> Q15 */
        out[2u * i + 1u] = (or_ + 16) >> 5;
        r->quiet = (wl > RV_QUIET || wl < -RV_QUIET || wr > RV_QUIET || wr < -RV_QUIET) ? 0u : r->quiet + 1u;
    }
}
