/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance voice engine v0: implementation (signal flow, laws and limits in mv_voice.h). */
#include "mv_voice.h"
#include "mv_fixed.h"
#include "mv_pitch.h"

_Static_assert(MV_CTL == MV_LIM_N, "mv_lim works on the engine's control block");
#include "mv_tables.h"

/* patch bytes (JP offsets) */
enum {
    B_LFO1_WAVE = 16, B_LFO1_RATE, B_LFO1_FADE, B_LFO2_RATE,
    B_RING = 21, B_XMOD, B_BALANCE, B_MOD_DEST, B_OSC_LFO1, B_PITCH_LFO2, B_PENV_DEPTH, B_PENV_A, B_PENV_D,
    B_OSC1_WAVE, B_OSC1_C1, B_OSC1_C2, B_OSC2_WAVE, B_OSC2_SYNC, B_OSC2_RANGE, B_OSC2_FINE, B_OSC2_C1, B_OSC2_C2,
    B_FLT_TYPE, B_FLT_SLOPE, B_CUTOFF, B_RESO, B_KEYFOLLOW, B_FLT_LFO1, B_FLT_LFO2, B_FENV_DEPTH,
    B_FENV_A, B_FENV_D, B_FENV_S, B_FENV_R, B_AMP_LEVEL, B_AMP_LFO1, B_AMP_LFO2,
    B_AENV_A, B_AENV_D, B_AENV_S, B_AENV_R, B_PAN_MODE,
    B_TONE_BASS = 59, B_TONE_TREBLE = 60, B_BEND_UP = 67, B_BEND_DOWN = 68, B_PORTA_SW, B_PORTA_TIME, B_MONO,
    B_LEGATO, B_OSC_SHIFT = 73, B_UNISON = 243, B_UNI_DET = 244, B_PATCH_GAIN = 245
};

#define LOG2_CTL 4u                             /* MV_CTL = 16 frames */
enum { MODE_POLY, MODE_MONO, MODE_UNISON };     /* play modes (mv_ev_drain) */
/* X-MOD: 69 Hz of OSC1 deviation per knob step (JpXmodLaw, triangle calibration) as a Q32 step at MV_FS */
#define XMOD_STEP_Q32 ((uint32_t)((69ull * 4294967296ull + MV_FS / 2u) / MV_FS))
#define XMOD_SAW_Q16   32768                    /* 0.50: saw family */
#define XMOD_P50_Q16   19399                    /* 0.296: pulse at 50 % */
#define XMOD_PSL_Q16   2147                     /* (0.296 - 0.277) / 0.58: per unit of |2 pw - 1| */
#define XMOD_SYNC_Q16  40829                    /* 0.623: under sync, non-pulse waves */
/* semitones (Q8) -> cutoff codes (Q8): x codes-per-octave / 12, in Q16 (a compile-time constant) */
#define SEMI_TO_CODE_Q16 (MV_FLT_CODE_PER_OCT_Q16 / 12)
/* key follow D2, codes (Q8) per (b - 64) x (note - 60): codes-per-octave x 256 / (12 x 32), or / (12 x 64) */
#define KEY_POS_Q16 ((int32_t)(((int64_t)MV_FLT_CODE_PER_OCT_Q16 * 256) / 384))
#define KEY_NEG_Q16 ((int32_t)(((int64_t)MV_FLT_CODE_PER_OCT_Q16 * 256) / 768))

/* OSC1 byte 30: SUPER SAW, TRIANGLE MOD, NOISE, FEEDBACK, SQR, SAW, TRIANGLE -> the waves ported so far
 * (SUPER SAW and FEEDBACK have their own renderers, mv_ssaw / mv_jfbk; their entries here are unused) */
static const uint8_t OSC1_W[7] = {MV_W_SAW, MV_W_TRIMOD, MV_W_SAW, MV_W_SAW, MV_W_SQR, MV_W_SAW, MV_W_JVTRI};
/* which waves read CTRL2 as LFO1 -> CTRL1 depth (PWM, SHAPE, OFFSET): OSC1 TRI MOD / SQR / SAW / TRI, OSC2
 * SQR / SAW / TRI. The others: SUPER SAW MIX, NOISE RESO, FEEDBACK amount */
static const uint8_t OSC1_C2LFO[7] = {0, 1, 0, 0, 1, 1, 1};
static const uint8_t OSC2_C2LFO[4] = {1, 1, 1, 0};
/* OSC2 byte 33: SQR, SAW, TRIANGLE, NOISE */
static const uint8_t OSC2_W[4] = {MV_W_SQR, MV_W_SAW, MV_W_JVTRI, MV_W_SAW};

/* a signed depth byte (64 = 0) through a JpModLaw table, with the depth knob scaled by g (Q30, the LFO1 fade:
 * the JT fades the depth CONTROL in, then applies the law), interpolated between bytes */
static int32_t depth_faded(const int16_t *t, uint32_t b, int32_t g)
{
    int32_t x = (64 << 8) + (int32_t)(((int64_t)((int32_t)b - 64) * 256 * g) >> 30);   /* byte, Q8 */
    int32_t i = x >> 8, fr = x & 255;
    if (i >= 127) return t[127];
    return t[i] + (((t[i + 1] - t[i]) * fr) >> 8);
}

/* x wheel / 127 (0..127), as x * wheel * 258 >> 15 (258 / 32768 = 1 / 127.0) */
static int32_t by_wheel(int32_t x, uint32_t w) { return (int32_t)(((int64_t)x * (int32_t)w * 258) >> 15); }

/* a part's patch bytes (UPPER: the patch, LOWER: lower) */
static const uint8_t *part_b(const mv_engine_t *e, uint32_t i) { return i ? e->lower.b : e->patch.b; }

void mv_engine_init(mv_engine_t *e)
{
    uint32_t v;
    mv_patch_init(&e->patch);
    mv_patch_init(&e->lower);
    for (v = 0; v < MV_PF_N; v++) e->perf[v] = MV_PERF_DEF[v];
    mv_fx_init(&e->fx);
    mv_rv_init(&e->rv);
    mv_lim_init(&e->fxlim);
    mv_lim_init(&e->lim);
    for (v = 0; v < MV_RV_N; v++) e->rvb[v] = 0;   /* reverb off until the settings load (mvance.c) */
    mv_arp_init(&e->arp);
    for (v = 0; v < MV_ARP_N; v++) e->arpb[v] = MV_ARP_DEF[v];   /* arp off (its defaults) until the settings load */
    e->tempo = 120; e->arp_beat = 22050u; e->arp_start = 0; e->arp_live = 0;
    e->lfo1_sync_inc[0] = e->lfo1_sync_inc[1] = 0;   /* free until the main loop says (mv_engine_sync) */
    e->sync_s[0] = e->sync_s[1] = 0;            /* OFF, matching lfo1_sync_inc 0 */
    e->sync_beat[0] = e->sync_beat[1] = 0;
    for (v = 0; v < MV_NV; v++) {
        mv_voice_t *p = &e->v[v];
        p->o1.ph = 0; p->o1.inc = 0; p->o2.ph = 0; p->o2.inc = 0;
        mv_env_kill(&p->aenv); mv_env_kill(&p->fenv); mv_env_kill(&p->penv);
        mv_flt_reset(&p->flt);
        p->flt_key[0] = p->flt_key[1] = p->flt_key[2] = 0xFF;   /* forces the first mv_flt_set */
        p->gain = 0; p->age = 0; p->note = 0; p->gate = 0; p->clk = 0; p->sync_live = 0;
        p->g_cur = p->g_tgt = 0; p->g_on = 0; p->det_q8 = 0; p->part = MV_PART_UP;
        p->ss.hx = p->ss.hy = 0;
        p->ss.key_ok = 0;
        mv_ssaw_set(&p->ss, 0, 0, 0);
        p->ss.rng = 0x9E3779B9u * (v + 1u);     /* each voice its own phase sequence */
        mv_jfbk_init(&p->fb, 0x2246822Bu * (v + 1u));
        mv_flt_reset(&p->nf[0]); mv_flt_reset(&p->nf[1]);
        p->nkey[0][0] = p->nkey[1][0] = 0xFF;   /* forces the first mv_flt_lp */
        p->nrng = 0x2F6E2B1u + v * 65537u;      /* (decimal: the build reads hex 0x1xxxx as a register address) */
    }
    for (v = 0; v < 2u; v++) {                  /* the parts: UPPER keeps the seeds the single engine had (its renders
                                                   stay bit-identical); LOWER's differ, so two S&H LFOs never lock (JT) */
        mv_part_t *P = &e->part[v];
        mv_lfo_init(&P->lfo1, v ? 0x2468ACEu : 0x1234567u);
        mv_lfo_init(&P->lfo2, v ? 0x13579BDu : 0x7654321u);
        P->nstack = 0; P->uni_note = -1; P->mode = MODE_POLY; P->last_inc = 0; P->bal_g = 32768;
        P->v0 = 0; P->nvp = v ? 0u : MV_NV;
    }
    e->slice_key = MV_KEY_SINGLE | (uint32_t)MV_NV << 8;   /* SINGLE on all MV_NV: matches the parts above */
    e->bend14 = 0; e->wheel = 0; e->ev_w = e->ev_r = 0; e->ages = 0; e->tone_b = e->tone_t = 0;
    e->nv = MV_NV;                              /* 8: measured on the FM-1, 8 Super Saw voices peak at 76 % */
}

static void mv_ev_push(mv_engine_t *e, uint32_t ev)
{
    uint32_t w = e->ev_w;
    if (w - e->ev_r >= MV_EVQ) return;          /* full: drop (32 notes inside one 5.8 ms buffer) */
    e->evq[w & (MV_EVQ - 1u)] = ev;
    e->ev_w = w + 1u;                           /* published after the slot is written */
}
void mv_engine_note_on(mv_engine_t *e, uint32_t note, uint32_t vel)
{
    if (note < 128u && vel) mv_ev_push(e, MV_TO_ROUTE << 16 | 1u << 15 | note << 7 | (vel & 127u));
}
void mv_engine_note_off(mv_engine_t *e, uint32_t note) { if (note < 128u) mv_ev_push(e, MV_TO_ROUTE << 16 | note << 7); }
void mv_engine_note_to(mv_engine_t *e, uint32_t note, uint32_t vel, uint32_t to)
{
    if (note < 128u && (to & 3u)) mv_ev_push(e, (to & 3u) << 16 | (vel ? 1u << 15 | (vel & 127u) : 0u) | note << 7);
}

/* ---- play modes (JT VoiceAllocator, the JP bytes 69..72, 243, 244), per part ---- */
#define GLIDE_SNAP 9739u                        /* 0.1 Hz as a Q32 step at MV_FS: the JT's snap distance */

static uint32_t note_inc(uint32_t note) { return mv_pitch_inc((int32_t)note * 256); }   /* the note alone */

/* the glide's start for a voice moving to note (JT Voice: seeded from the part's last note, so a stolen or idle
 * voice slides from what was last played; an in-flight glide carries on from where it is) */
static void glide_to(mv_part_t *P, const uint8_t *b, mv_voice_t *p, uint32_t note)
{
    uint32_t tgt = note_inc(note);
    if (b[B_PORTA_SW] && P->last_inc) {
        if (!p->g_on) p->g_cur = P->last_inc;
        p->g_tgt = tgt;
        p->g_on = 1;
    } else {
        p->g_cur = p->g_tgt = tgt;
        p->g_on = 0;
    }
    P->last_inc = tgt;
}

/* a full strike: envelopes from their current level, random start phases, the glide seeded */
static void strike(mv_engine_t *e, mv_part_t *P, const uint8_t *b, mv_voice_t *p, uint32_t note)
{
    const int fresh = !mv_env_active(&p->aenv);   /* from silence (else a steal, a retrigger, MONO's next note) */
    if (fresh) mv_flt_reset(&p->flt);           /* a silent voice starts clean */
    glide_to(P, b, p, note);
    p->note = (uint8_t)note; p->gate = 1; p->age = ++e->ages;
    /* the envelopes start from their current level (a steal or retrigger never jumps) */
    mv_env_set(&p->aenv, b[B_AENV_A], b[B_AENV_D], b[B_AENV_S], b[B_AENV_R]);
    mv_env_set(&p->fenv, b[B_FENV_A], b[B_FENV_D], b[B_FENV_S], b[B_FENV_R]);
    mv_env_set(&p->penv, b[B_PENV_A], b[B_PENV_D], 0, b[B_PENV_D]);
    mv_env_on(&p->aenv); mv_env_on(&p->fenv); mv_env_on(&p->penv);
    mv_lfo_key(&P->lfo1);                       /* the LFO1 fade restarts on any key of the part (JT: per layer) */
    if (fresh) {
        /* random start phases (as the JT's OscSection::noteOn), FROM SILENCE ONLY (Kris 2 Nov, S2: fm1-port/mvance-
         * crackle-diagnosis-2026-11-02.md). On a voice still sounding, new phases are a waveform jump, a click at
         * its level (216 of 224 strikes in dense playing; every MONO note): there the oscillators, the Super Saw's
         * seven, the feedback osc and the sync clock run on and the new pitch takes over from where they are (the
         * lockstep the JT's random phases prevent only happens from silence). The JT takes 10 bits each; here the
         * hash's full 32 bits (finer, the same job): keeping 10 bits at the top made the compiler emit the mask
         * 0xFFC00000, which the build's ROM check rightly refuses (it is a chip-ROM address) */
        uint32_t seed = e->ages * 2654435761u, ph0 = seed, ph1 = seed * 2246822519u;
        p->o1.ph = ph0; p->o2.ph = ph1;
        p->sync_live = 0;                       /* the sync clock re-seeds from OSC1 */
        mv_ssaw_key(&p->ss);                    /* SUPER SAW: seven new random phases (Szabo 3.4) */
        mv_jfbk_key(&p->fb, ph0);               /* FEEDBACK: every note fresh, from OSC1's phase */
    }
}

/* LEGATO: the pitch moves (gliding if PORTAMENTO is on), envelopes and phases carry on. Refused (0) when the
 * voice is not held (releasing or idle): the caller strikes instead (JT Voice::legatoTo) */
static int legato(mv_engine_t *e, mv_part_t *P, const uint8_t *b, mv_voice_t *p, uint32_t note)
{
    if (!p->gate || !mv_env_active(&p->aenv)) return 0;
    glide_to(P, b, p, note);
    p->note = (uint8_t)note;
    p->age = ++e->ages;
    return 1;
}

static void release(mv_voice_t *p)
{
    p->gate = 0;
    mv_env_off(&p->aenv); mv_env_off(&p->fenv); mv_env_off(&p->penv);
}

/* the mono note stack: last-note priority, a release returns to the newest key still held */
static void stack_remove(mv_part_t *P, uint32_t note)
{
    uint32_t i, j;
    for (i = 0; i < P->nstack; i++)
        if (P->stack[i] == note) {
            for (j = i; j + 1u < P->nstack; j++) P->stack[j] = P->stack[j + 1u];
            P->nstack--;
            return;
        }
}
static void stack_push(mv_part_t *P, uint32_t note)
{
    stack_remove(P, note);
    if (P->nstack < sizeof P->stack) P->stack[P->nstack++] = (uint8_t)note;
}

/* UNISON detune across the part's voices: -spread/2 .. +spread/2, evenly; spread = DETUNE / 50 of a semitone at
 * most (JT applyUnisonDetune, kUnisonMaxSpreadSemis 1); both oscillators. 0 outside UNISON */
static void unison_spread(mv_engine_t *e, const mv_part_t *P, const uint8_t *b, uint32_t on)
{
    uint32_t v, n = P->nvp, det = b[B_UNI_DET] > 50u ? 50u : b[B_UNI_DET];
    int32_t spread = (int32_t)(det * 256u / 50u);   /* Q8 semitones */
    for (v = 0; v < n; v++)
        e->v[P->v0 + v].det_q8 = (int16_t)(on && n > 1u ? -spread / 2 + spread * (int32_t)v / (int32_t)(n - 1u) : 0);
}

/* the cut of the voices between the parts (ISR, a block's start): SINGLE gives UPPER the first nv, DUAL / SPLIT
 * LOWER the first VOICES and UPPER the rest (the JT's voice split, 1+7 .. 7+1). Only when it moves: every voice
 * fades out in one block (no voice keeps the other part's note) and the parts' note state clears */
static void slice(mv_engine_t *e)
{
    uint32_t mode = e->perf[MV_PF_MODE] > MV_KEY_SPLIT ? MV_KEY_SINGLE : e->perf[MV_PF_MODE];
    uint32_t nv = e->nv < 1u ? 1u : e->nv > MV_NV ? MV_NV : e->nv, lo, v, i;
    uint32_t key = mode | (uint32_t)(mode ? (e->perf[MV_PF_VSPLIT] % 7u) : 0u) << 4 | (mode ? 0u : nv) << 8;
    if (key == e->slice_key) return;
    if ((e->slice_key & 15u) != mode || mode)   /* (nv alone moving in SINGLE: the voices above it finish, as before) */
        for (v = 0; v < MV_NV; v++) { e->v[v].gate = 0; mv_env_fade(&e->v[v].aenv); }
    e->slice_key = key;
    lo = mode ? e->perf[MV_PF_VSPLIT] % 7u + 1u : 0u;   /* LOWER's share: 1..7 */
    e->part[MV_PART_LO].v0 = 0; e->part[MV_PART_LO].nvp = (uint8_t)lo;
    e->part[MV_PART_UP].v0 = (uint8_t)lo; e->part[MV_PART_UP].nvp = (uint8_t)(mode ? MV_NV - lo : nv);
    for (v = 0; v < MV_NV; v++) e->v[v].part = (uint8_t)(v < lo ? MV_PART_LO : MV_PART_UP);
    for (i = 0; i < 2u; i++) { e->part[i].nstack = 0; e->part[i].uni_note = -1; }
}

/* a note event (on / off) into one part's voices, by its play mode. From the key queue, or from the arp */
static void ev_play(mv_engine_t *e, uint32_t pi, uint32_t note, uint32_t on)
{
    mv_part_t *P = &e->part[pi];
    const uint8_t *b = part_b(e, pi);
    uint32_t v, n = P->nvp, v0 = P->v0;
    uint32_t mode = b[B_UNISON] ? MODE_UNISON : b[B_MONO] ? MODE_MONO : MODE_POLY;
    if (!n) return;                             /* a part without voices (LOWER in SINGLE) */
    if (mode != P->mode) {                      /* a mode change: the part's voices out (a one-block fade, no click)
                                                   and its stack cleared, so nothing is left stuck (as the JT) */
        for (v = 0; v < n; v++) { e->v[v0 + v].gate = 0; mv_env_fade(&e->v[v0 + v].aenv); }
        P->nstack = 0; P->uni_note = -1;
        P->mode = (uint8_t)mode;
        unison_spread(e, P, b, mode == MODE_UNISON);
    }
    if (mode == MODE_MONO) {                    /* one voice (the slice's first), last-note priority */
        mv_voice_t *p = &e->v[v0];
        if (on) {
            uint32_t held = P->nstack != 0u;
            stack_push(P, note);
            if (!(b[B_LEGATO] && held && legato(e, P, b, p, note))) strike(e, P, b, p, note);
        } else {
            /* only the sounding key (the newest held) moves the voice: releasing an older, silent key just
               leaves the stack (it used to re-strike the sounding note, Kris 2 Nov) */
            const uint32_t was_top = P->nstack != 0u && P->stack[P->nstack - 1u] == note;
            stack_remove(P, note);
            if (!was_top) {
                /* nothing to do: the voice keeps playing the newest key */
            } else if (P->nstack) {             /* back to the newest key still held: legato moves the pitch,
                                                   else a retrigger (the JT's v1 decision, kept) */
                uint32_t ret = P->stack[P->nstack - 1u];
                if (!(b[B_LEGATO] && legato(e, P, b, p, ret))) strike(e, P, b, p, ret);
            } else if (p->gate) {
                release(p);
            }
        }
        return;
    }
    if (mode == MODE_UNISON) {                  /* every voice of the part on the one held note (the JT's unison) */
        if (on) {
            if (b[B_LEGATO] && P->uni_note >= 0 && P->uni_note != (int32_t)note && legato(e, P, b, &e->v[v0], note)) {
                for (v = 1; v < n; v++) legato(e, P, b, &e->v[v0 + v], note);
            } else {
                for (v = 0; v < n; v++) strike(e, P, b, &e->v[v0 + v], note);
            }
            P->uni_note = (int32_t)note;
            unison_spread(e, P, b, 1);          /* (the voice count may have changed) */
        } else if ((int32_t)note == P->uni_note) {
            for (v = 0; v < n; v++) if (e->v[v0 + v].gate) release(&e->v[v0 + v]);
            P->uni_note = -1;
        }
        return;
    }
    if (!on) {                                  /* POLY key up: every voice of the part on this note */
        for (v = 0; v < n; v++)
            if (e->v[v0 + v].gate && e->v[v0 + v].note == note) release(&e->v[v0 + v]);
        return;
    }
    {   /* POLY: the same note, else a free voice, else the quietest release, else the oldest held: within the
         * part's voices (SINGLE: the first nv, the main loop's voice limit) */
        uint32_t pick = 0, best = 0xFFFFFFFFu;
        for (v = 0; v < n; v++) {
            const mv_voice_t *p = &e->v[v0 + v];
            uint32_t score = p->gate && p->note == note ? 0u
                           : !mv_env_active(&p->aenv) ? 1u
                           : p->gate ? 0x80000000u + (p->age & 0x7FFFFFFFu) : 2u + ((uint32_t)p->aenv.level >> 2);
            if (score < best) { best = score; pick = v; }
        }
        strike(e, P, b, &e->v[v0 + pick], note);
    }
}

/* where a key goes (the JT's PerfRouter: the channel gate first, the mask, then the mode / split): MV_TO_* bits */
static uint32_t route(const mv_engine_t *e, uint32_t note, uint32_t to)
{
    uint32_t mode = e->perf[MV_PF_MODE];
    if (mode == MV_KEY_DUAL) return to;
    if (mode == MV_KEY_SPLIT) return to & (note < e->perf[MV_PF_SPLIT] ? MV_TO_LO : MV_TO_UP);
    return to & MV_TO_UP;                       /* SINGLE: UPPER alone */
}

/* the parts the arp plays (MV_TO_* bits): ARP AUTO follows the mode (SINGLE UPPER, DUAL both, SPLIT LOWER) */
static uint32_t arp_parts(const mv_engine_t *e)
{
    uint32_t mode = e->perf[MV_PF_MODE], a = e->perf[MV_PF_ARP];
    if (mode == MV_KEY_SINGLE) return MV_TO_UP;
    if (a == MV_ARPP_UPPER) return MV_TO_UP;
    if (a == MV_ARPP_LOWER) return MV_TO_LO;
    if (a == MV_ARPP_BOTH) return MV_TO_UP | MV_TO_LO;
    return mode == MV_KEY_SPLIT ? MV_TO_LO : MV_TO_UP | MV_TO_LO;
}

/* the key queue: a key for an arp part goes to the arp (it consumes the keys: Skiff's model), the rest to the
 * parts' voices. A key UP goes to every part it may have reached (a split moved meanwhile leaves nothing stuck) */
static void mv_ev_drain(mv_engine_t *e)
{
    while (e->ev_r != e->ev_w) {
        uint32_t ev = e->evq[e->ev_r & (MV_EVQ - 1u)], note = ev >> 7 & 127u, on = ev >> 15 & 1u, to = ev >> 16 & 3u;
        uint32_t t = on ? route(e, note, to) : to, ap = e->arp_live ? arp_parts(e) : 0u, i;
        e->ev_r++;
        if (t & ap) mv_arp_key(&e->arp, e->arpb, note, (int)on);
        for (i = 0; i < 2u; i++)
            if (t >> i & 1u & ~(ap >> i)) ev_play(e, i, note, on);
    }
}

/* the arp, a block at a time (before the voices render it): its switch, MIDI START, then its notes into ev_play
 * on its part(s) */
static void arp_block(mv_engine_t *e, uint32_t n)
{
    uint16_t ev[MV_ARP_EVMAX];
    uint32_t k, ne = 0, on = e->arpb[MV_ARP_ON] != 0u, v, i, ap = arp_parts(e);
    if (on != e->arp_live) {
        if (on) {                               /* ON: its parts' key voices out (they fade), the arp starts empty */
            for (v = 0; v < MV_NV; v++)
                if (e->v[v].gate && (ap >> e->v[v].part & 1u)) { e->v[v].gate = 0; mv_env_fade(&e->v[v].aenv); }
            for (i = 0; i < 2u; i++) if (ap >> i & 1u) { e->part[i].nstack = 0; e->part[i].uni_note = -1; }
            mv_arp_init(&e->arp);
        } else {
            ne = mv_arp_stop(&e->arp, ev);      /* OFF: its notes end; keys play the voices again */
        }
        e->arp_live = (uint8_t)on;
    }
    if (e->arp_start) { e->arp_start = 0; mv_arp_restart(&e->arp); }
    if (on) ne = mv_arp_tick(&e->arp, e->arpb, n, e->arp_beat ? e->arp_beat : 22050u, ev);
    for (k = 0; k < ne; k++)
        for (i = 0; i < 2u; i++)
            if (on ? ap >> i & 1u : 1u) ev_play(e, i, ev[k] >> 7 & 127u, ev[k] >> 15);   /* (OFF's note-offs: to both) */
}

/* PAN placeholder (Kris 1 Nov 2026, option 3: "just put a pan placeholder in"). Byte 58 LFO1 MOD = AUTO pans by
 * LFO1 x byte 52, MANUAL places the voice at byte 52 (spec note 4, to be confirmed on the JE); OFF is tremolo
 * (done in the amp). Until the capture gives the law, every voice sits in the centre and the out stays mono:
 * this returns the position, -32768 (left) .. 0 (centre) .. +32767 (right), and is the one place to fill in. */
static int32_t pan_position(const uint8_t *b, int32_t l1u, int32_t l1g)
{
    (void)b; (void)l1u; (void)l1g;
    return 0;
}

/* NOISE: n frames of white noise (xorshift32, +-1) through the oscillator's own low pass (o = 0 OSC1, 1 OSC2).
 * The filter law is set only when CTRL1 / CTRL2 change. */
static void noise_block(mv_voice_t *p, uint32_t o, uint32_t c1, uint32_t c2, int32_t *out, uint32_t n)
{
    uint32_t i, x = p->nrng;
    c1 &= 127u; c2 &= 127u;
    if (p->nkey[o][0] != c1 || p->nkey[o][1] != c2) {
        mv_flt_lp(&p->nf[o], MV_NOISE_G_Q26[c1], MV_NOISE_R_Q31[c2]);
        p->nkey[o][0] = (uint8_t)c1; p->nkey[o][1] = (uint8_t)c2;
    }
    for (i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        out[i] = (int32_t)x >> 16;              /* Q15 */
    }
    p->nrng = x;
    mv_flt_render(&p->nf[o], out, n);
}

/* one control block (MV_CTL frames) of one voice, added into the mono bus */
static void voice_block(mv_engine_t *e, const uint8_t *b, mv_voice_t *p, int32_t *bus, int32_t l1u, int32_t l1g,
                        int32_t l2u, uint32_t wheel, int32_t bend_q8)
{
    int32_t a[MV_CTL], c[MV_CTL];
    int32_t ae, fe, pe, mod, base, p1, p2, code, bal, g1, g2, gain, step, kb, i, rl, re, xk;
    uint32_t w1 = OSC1_W[b[B_OSC1_WAVE] < 7u ? b[B_OSC1_WAVE] : 5u];
    uint32_t w2 = OSC2_W[b[B_OSC2_WAVE] & 3u], dest = b[B_MOD_DEST], mode, ssaw, jfbk;

    (void)e;                                    /* (the profile build times through it) */
    /* envelopes, levels Q30 */
    mv_env_set(&p->aenv, b[B_AENV_A], b[B_AENV_D], b[B_AENV_S], b[B_AENV_R]);   /* edits follow at once */
    mv_env_set(&p->fenv, b[B_FENV_A], b[B_FENV_D], b[B_FENV_S], b[B_FENV_R]);
    mv_env_set(&p->penv, b[B_PENV_A], b[B_PENV_D], 0, b[B_PENV_D]);
    ae = mv_env_tick(&p->aenv);
    fe = mv_env_tick(&p->fenv);
    pe = mv_env_tick(&p->penv);

    /* pitch (Q8 semitones): common = LFO2 x wheel + bend (both oscillators, always); routed = LFO1 + PITCH ENV,
     * steered by LFO1 & ENV DEST (JT Voice.cpp, G1): OSC1+2 / OSC2 / X-MOD (adds to the X-MOD knob, below) */
    rl = (depth_faded(MV_MOD_Q8, b[B_OSC_LFO1], l1g) * l1u) >> 15;                    /* LFO1, Q8 st */
    re = (int32_t)(((int64_t)(2 * MV_MOD_Q8[b[B_PENV_DEPTH]]) * pe) >> 30);           /* env, 2 x D, Q8 st */
    mod = by_wheel((MV_MOD_Q8[b[B_PITCH_LFO2]] * l2u) >> 15, wheel);
    base = (int32_t)p->note * 256 + mv_shift_q8(b[B_OSC_SHIFT]) + bend_q8 + mod;
    p1 = base + (dest == 0u ? rl + re : 0);
    p2 = base + mv_osc2_q8(b[B_OSC2_RANGE], b[B_OSC2_FINE]) + (dest <= 1u ? rl + re : 0);
    /* X-MOD knob in steps (Q8): the byte, + the routed lanes when DEST = X-MOD (JpModLaw: LFO1 D/16.25 and env
     * D/18 knob steps, i.e. LFO1 semitones x 100 / 16.25, env semitones (2 D / 100) x 100 / 36), 0..127 */
    xk = (int32_t)b[B_XMOD] * 256;
    if (dest == 2u) xk = mv_clamp(xk + (int32_t)(((int64_t)rl * 403298 + (int64_t)re * 182044) >> 16), 0, 127 * 256);
    p1 += p->det_q8; p2 += p->det_q8;           /* UNISON spread (0 outside unison) */
    p->o1.inc = mv_pitch_inc(p1);
    p->o2.inc = mv_pitch_inc(p2);
    if (p->g_on) {                              /* PORTAMENTO: the JT's slew of the note frequency (a fixed fraction
                                                   of the remaining distance a block), applied to both oscillators
                                                   as the ratio of where the slide is to the note it heads for */
        int64_t dl = (int64_t)p->g_tgt - (int64_t)p->g_cur;
        uint32_t ratio;
        if (dl < (int64_t)GLIDE_SNAP && dl > -(int64_t)GLIDE_SNAP) {
            p->g_cur = p->g_tgt; p->g_on = 0;   /* within 0.1 Hz: there */
        } else {
            p->g_cur = (uint32_t)((int64_t)p->g_cur + ((dl * (int64_t)MV_PORTA_Q31[b[B_PORTA_TIME] & 127u]) >> 31));
        }
        /* ratio Q24: fits 32 bits for a slide of up to 8 octaves down (g_cur < 256 g_tgt; the keys span less) */
        ratio = mv_udiv64_32((uint64_t)p->g_cur << 24, p->g_tgt | 1u);
        {
            uint64_t i1 = ((uint64_t)p->o1.inc * ratio) >> 24, i2 = ((uint64_t)p->o2.inc * ratio) >> 24;
            p->o1.inc = i1 > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)i1;   /* (under fs/2) */
            p->o2.inc = i2 > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)i2;
        }
    }

    /* oscillator shapes: CTRL1, with LFO1 x CTRL2 where CTRL2 is that depth (faded like every LFO1 depth) */
    ssaw = b[B_OSC1_WAVE] == 0u;
    jfbk = b[B_OSC1_WAVE] == 3u;
    if (ssaw) mv_ssaw_set(&p->ss, p->o1.inc, b[B_OSC1_C1], b[B_OSC1_C2]);
    else if (jfbk) mv_jfbk_set(&p->fb, p->o1.inc, b[B_OSC1_C1], b[B_OSC1_C2]);
    else mv_osc_shape(&p->o1, w1, b[B_OSC1_C1], OSC1_C2LFO[b[B_OSC1_WAVE] < 7u ? b[B_OSC1_WAVE] : 5u]
                      ? (int32_t)(((int64_t)((l1u * (int32_t)b[B_OSC1_C2] * 258) >> 15) * l1g) >> 30) : 0);
    mv_osc_shape(&p->o2, w2, b[B_OSC2_C1], OSC2_C2LFO[b[B_OSC2_WAVE] & 3u]
                 ? (int32_t)(((int64_t)((l1u * (int32_t)b[B_OSC2_C2] * 258) >> 15) * l1g) >> 30) : 0);

    /* balance (JT mix.balance: linear, both full at the centre), Q15 */
    bal = (int32_t)b[B_BALANCE] - 64;
    g1 = bal > 0 ? (32767 * (63 - bal)) / 63 : 32767;
    g2 = bal < 0 ? (32767 * (64 + bal)) / 64 : 32767;

#if MV_PROFILE
    uint32_t pt0 = mv_prof_ticks(), pt1;
#endif
    /* OSC1 / OSC2 coupling (JP-8000 manual p.59; JT OscSection, JpXmod): render only what is heard or used */
    {
        uint32_t own = b[B_OSC1_WAVE] == 0u || b[B_OSC1_WAVE] == 3u;   /* SUPER SAW / FEEDBACK: no sync, X-MOD */
        uint32_t ring = b[B_RING] != 0u, sync = b[B_OSC2_SYNC] != 0u && !own;
        uint32_t xmod = xk > 0 && !own;
        uint32_t need2 = g2 > 0 || ring || xmod, need1 = g1 > 0 || ring;

        if (need2 && b[B_OSC2_WAVE] == 3u) {
            noise_block(p, 1u, b[B_OSC2_C1], b[B_OSC2_C2], c, MV_CTL);   /* (no phase: sync does not act) */
        } else if (need2) {
            if (sync) {                         /* the clock: OSC1's unmodulated phase, wraps reset OSC2 */
                uint32_t rst[MV_CTL], clk, inc1 = p->o1.inc, r16;
                if (!p->sync_live) p->clk = p->o1.ph;   /* entering sync: start the clock at OSC1 */
                /* OSC2 / OSC1 step ratio (Q16, < 2^16 for any two playable pitches): after a wrap the clock
                 * is clk into OSC1's cycle, which is clk / inc1 frames, i.e. OSC2's phase clk x inc2 / inc1 */
                r16 = inc1 ? mv_udiv64_32((uint64_t)p->o2.inc << 16, inc1 | 1u) : 0u;
                clk = p->clk;
                for (i = 0; i < (int32_t)MV_CTL; i++) {
                    uint32_t c0 = clk;
                    clk += inc1;
                    rst[i] = clk < c0 ? (uint32_t)(((uint64_t)clk * r16) >> 16) : MV_NO_RESET;
                }
                p->clk = clk;
                mv_osc_render_sync(&p->o2, w2, c, MV_CTL, rst);
            } else {
                mv_osc_render(&p->o2, w2, c, MV_CTL);
            }
        }
        p->sync_live = (uint8_t)sync;
        if (need1 && b[B_OSC1_WAVE] == 2u) {
            noise_block(p, 0u, b[B_OSC1_C1], b[B_OSC1_C2], a, MV_CTL);   /* (no phase: X-MOD does not act) */
        } else if (need1) {
            if (xmod) {                         /* through-zero linear FM of OSC1 by OSC2, Hz-fixed depth */
                int32_t d[MV_CTL], sc;
                uint32_t wb = b[B_OSC2_WAVE];
                if (wb == 0u) {                 /* pulse: by width, not by sync. |2 pw - 1| = |pw - 2^31| / 2^31 */
                    uint32_t pw = p->o2.pw, dv = pw >= 0x80000000u ? pw - 0x80000000u : 0x80000000u - pw;
                    sc = XMOD_P50_Q16 - (int32_t)(((uint64_t)XMOD_PSL_Q16 * (dv >> 15)) >> 16);
                } else {
                    sc = wb == 1u ? XMOD_SAW_Q16 : 65536;   /* saw 0.5; triangle and noise: 1 (unmeasured) */
                    if (sync) sc = (sc * XMOD_SYNC_Q16) >> 16;
                }
                {   /* the deviation at OSC2 = +-1, Q32: 69 Hz x knob steps x scale (<= 8.5e8) */
                    int32_t full = (int32_t)(((((uint64_t)XMOD_STEP_Q32 * (uint32_t)xk) >> 8) * (uint32_t)sc) >> 16);
                    for (i = 0; i < (int32_t)MV_CTL; i++) d[i] = (int32_t)(((int64_t)full * c[i]) >> 15);
                }
                mv_osc_render_fm(&p->o1, w1, a, MV_CTL, d);
            } else if (ssaw) {
                mv_ssaw_render(&p->ss, a, MV_CTL);
            } else if (jfbk) {
                mv_jfbk_render(&p->fb, a, MV_CTL);
            } else {
                mv_osc_render(&p->o1, w1, a, MV_CTL);
            }
        }
        /* the mix: OSC1 x g1 + (RING ? OSC1 x OSC2 : OSC2) x g2 (on the JP the ring product replaces OSC2) */
        if (!need1) for (i = 0; i < (int32_t)MV_CTL; i++) a[i] = 0;
        if (!need2) for (i = 0; i < (int32_t)MV_CTL; i++) c[i] = 0;
        if (ring) for (i = 0; i < (int32_t)MV_CTL; i++) c[i] = (int32_t)(((int64_t)a[i] * c[i]) >> 15);
        for (i = 0; i < (int32_t)MV_CTL; i++) a[i] = (int32_t)(((int64_t)a[i] * g1 + (int64_t)c[i] * g2) >> 15);
    }

#if MV_PROFILE
    pt1 = mv_prof_ticks();
    e->prof_osc += pt1 - pt0;
#endif
    /* filter: mode and knob bytes (control plane, only on a change), then the modulated cutoff code */
    mode = (b[B_FLT_TYPE] == 2u ? MV_FLT_LP24 : b[B_FLT_TYPE] == 0u ? MV_FLT_HP24 : MV_FLT_BP24) + (b[B_FLT_SLOPE] ? 0u : 1u);
    if (p->flt_key[0] != mode || p->flt_key[1] != b[B_RESO] || p->flt_key[2] != b[B_CUTOFF]) {
        if (p->flt_key[0] != mode) mv_flt_reset(&p->flt);
        mv_flt_set(&p->flt, mode, b[B_RESO], b[B_CUTOFF]);
        p->flt_key[0] = (uint8_t)mode; p->flt_key[1] = b[B_RESO]; p->flt_key[2] = b[B_CUTOFF];
    }
    kb = (int32_t)b[B_KEYFOLLOW] - 64;
    /* LFOs: Q8 semitones on the LFO curve (scene 19: +32 = +-0.66 oct), converted to codes below */
    mod = ((depth_faded(MV_MOD_Q8, b[B_FLT_LFO1], l1g) * l1u) >> 15)
        + by_wheel((MV_MOD_Q8[b[B_FLT_LFO2]] * l2u) >> 15, wheel);
    /* FILTER ENV: already in cutoff codes Q8 (+63 = 124 codes at full env, JE scene 17; Kris 2 Nov, option 1) */
    code = (int32_t)b[B_CUTOFF] * 256 + (int32_t)(((int64_t)MV_FENV_Q8[b[B_FENV_DEPTH]] * fe) >> 30)
         + (int32_t)(((int64_t)mod * SEMI_TO_CODE_Q16) >> 16)
         + (int32_t)(((int64_t)kb * ((int32_t)p->note - 60) * (kb >= 0 ? KEY_POS_Q16 : KEY_NEG_Q16)) >> 16);
    mv_flt_cutoff(&p->flt, code);
    mv_flt_render(&p->flt, a, MV_CTL);
#if MV_PROFILE
    e->prof_flt += mv_prof_ticks() - pt1;
#endif

    /* amp, Q15: LEVEL x A-ENV x (1 + LFO1 tremolo) x (1 + LFO2 x wheel) */
    gain = (int32_t)(((int64_t)b[B_AMP_LEVEL] * 258 * (ae >> 15)) >> 15);           /* level / 127 x env */
    if (b[B_PAN_MODE] == 0u && b[B_AMP_LFO1] != 64u)
        gain = (int32_t)(((int64_t)gain * mv_clamp(32768 + ((depth_faded(MV_AMP_IDX_Q15, b[B_AMP_LFO1], l1g) * l1u) >> 15),
                                                   0, 65536)) >> 15);
    if (b[B_AMP_LFO2] != 64u && wheel)
        gain = (int32_t)(((int64_t)gain * mv_clamp(32768 + by_wheel((MV_AMP_IDX_Q15[b[B_AMP_LFO2]] * l2u) >> 15, wheel),
                                                   0, 65536)) >> 15);
    gain = mv_clamp(gain, 0, 65535);
    step = mv_ramp_step(p->gain, gain, LOG2_CTL);   /* across the block: no zipper */
    {
        int32_t g = p->gain;
        uint32_t sh = b[B_PATCH_GAIN] > 2u ? 0u : b[B_PATCH_GAIN];   /* 0 / +6 / +12 dB */
        for (i = 0; i < (int32_t)MV_CTL; i++) {
            int32_t y;
            g += step;
            /* x gain (Q15), x the JT voice gain 1/8 (>> 3), x the patch gain */
            y = (int32_t)(((int64_t)a[i] * g) >> (18 - sh));
            bus[i] += y;
        }
    }
    p->gain = gain;
    (void)pan_position(b, l1u, l1g);            /* PAN placeholder: centre (see pan_position) */
}

void mv_tone(mv_engine_t *e, int32_t *bus, uint32_t n, uint32_t tb, uint32_t tt)
{
    const uint32_t ab = MV_TONE_BASS_A_Q31[tb & 127u], at = MV_TONE_TREB_A_Q31[tt & 127u];
    const int32_t db = MV_TONE_BASS_D_Q16[tb & 127u], dt = MV_TONE_TREB_D_Q16[tt & 127u];
    int32_t sb = e->tone_b, st = e->tone_t;
    uint32_t i;
    if (tb == 64u && tt == 64u) return;         /* flat: not calculated (the states hold; both are LPs of the
                                                   signal, so they catch up within a few ms when used again) */
    for (i = 0; i < n; i++) {
        int32_t x = bus[i] * 512, vb, vt, lb, lt;   /* Q15 -> Q24 */
        int64_t y;
        vb = (int32_t)(((int64_t)(x - sb) * ab) >> 31); lb = sb + vb; sb = lb + vb;   /* TPT one-pole LPs */
        vt = (int32_t)(((int64_t)(x - st) * at) >> 31); lt = st + vt; st = lt + vt;
        y = (int64_t)x + (((int64_t)db * lb + (int64_t)dt * (x - lt)) >> 16);        /* + (G-1) LP, + (G-1) HP */
        y >>= 9;                                /* Q15; bass +63 is +27 dB: bounded to the int32 bus */
        bus[i] = y > 0x3FFFFFFF ? 0x3FFFFFFF : y < -0x3FFFFFFF ? -0x3FFFFFFF : (int32_t)y;
    }
    e->tone_b = sb; e->tone_t = st;
}

/* BALANCE (the JT's law, full at the centre): LOWER at unity 0..64, out by 127; UPPER at unity 64..127, out by 0 */
static int32_t bal_gain(uint32_t bal, uint32_t pi)
{
    bal &= 127u;
    if (pi == MV_PART_LO) return bal <= 64u ? 32768 : (int32_t)((127u - bal) * 32768u / 63u);
    return bal >= 64u ? 32768 : (int32_t)(bal * 32768u / 64u);
}

void mv_engine_sync(mv_engine_t *e)
{
    uint32_t i, beat = e->arp_beat;
    for (i = 0; i < 2u; i++) {
        uint32_t s = e->perf[MV_PF_SYNC(i)], was = e->sync_beat[i];
        uint32_t d = beat > was ? beat - was : was - beat;   /* how far the beat has moved since the last step */
        /* Nothing to do when the byte is the same and either it is OFF or the beat is within 1/512 of the one the
         * step was made for. 1/512 (0.2 %, 0.24 BPM at 120) is above the MIDI clock's ms jitter, even averaged over
         * 4 quarters (about 11 samples at 130 BPM, 0.05 %), and far below a hearable LFO rate change: the LFO is
         * free-running (no phase to keep), so holding the step a hair off the tempo costs nothing. */
        if (s == e->sync_s[i] && (!s || d <= (was >> 9))) continue;
        e->sync_s[i] = (uint8_t)s;
        e->sync_beat[i] = beat;
        e->lfo1_sync_inc[i] = s ? mv_lfo_sync_inc(s, beat) : 0u;   /* (one 32-bit store: the ISR reads it whole) */
    }
}

void mv_engine_render(mv_engine_t *e, int32_t *out, uint32_t n)
{
    const uint8_t *b = e->patch.b;              /* UPPER: the shared FX chain's bytes */
    uint32_t k, v, i, wheel = e->wheel > 127u ? 127u : e->wheel;
    slice(e);                                   /* the parts' voices (only when the cut moved) */
    mv_ev_drain(e);
    for (k = 0; k + MV_CTL <= n; k += MV_CTL) {
        int32_t l1u[2] = {0, 0}, l1g[2] = {0, 0}, l2u[2] = {0, 0}, bend_q8[2] = {0, 0};
        int any = 0;
        for (v = 0; v < MV_NV; v++) any |= mv_env_active(&e->v[v].aenv);
        mv_fx_set(&e->fx, b);                   /* the FX bytes (a compare when nothing moved) */
        mv_rv_set(&e->rv, e->rvb);              /* the reverb settings (likewise) */
        arp_block(e, MV_CTL);                   /* the arp's notes for this block (it may start a voice) */
        if (!any && e->arp_live)                /* (a note it just started) */
            for (v = 0; v < MV_NV; v++) any |= mv_env_active(&e->v[v].aenv);
        if (!any && !mv_fx_ringing(&e->fx) && !mv_rv_ringing(&e->rv) && !mv_lim_pending(&e->fxlim)) {   /* silent and no tail: nothing calculated
                                                                             (LFO phases pause) */
            if (mv_lim_pending(&e->lim)) mv_lim_block(&e->lim, out + 2u * k);   /* but the limiter's held block out */
            continue;
        }
        for (i = 0; i < 2u; i++) {              /* each playing part's LFOs and bend (LOWER only when it has voices) */
            const uint8_t *pb = part_b(e, i);
            mv_part_t *P = &e->part[i];
            if (!P->nvp && i) continue;
            mv_lfo1_set(&P->lfo1, pb[B_LFO1_WAVE], pb[B_LFO1_RATE], pb[B_LFO1_FADE]);
            if (e->lfo1_sync_inc[i]) P->lfo1.inc = e->lfo1_sync_inc[i];   /* LFO1 SYNC: the tempo's, RATE ignored */
            mv_lfo2_set(&P->lfo2, MV_LFO_TRI, pb[B_LFO2_RATE]);   /* LFO2: triangle (scene 18) */
            l1u[i] = mv_lfo_tick(&P->lfo1);
            l1g[i] = P->lfo1.gain;
            l2u[i] = mv_lfo_tick(&P->lfo2);
            bend_q8[i] = mv_bend_q8(e->bend14, pb[B_BEND_UP], pb[B_BEND_DOWN]);
        }
        {
            int32_t bus[MV_CTL], lbus[MV_CTL], *pbus[2];
            uint32_t tb = b[B_TONE_BASS] & 127u, tt = b[B_TONE_TREBLE] & 127u;   /* (bytes 0..127) */
            int32_t g[2];
            int mix;                            /* BALANCE off the centre in DUAL / SPLIT: LOWER on its own bus */
            g[0] = e->perf[MV_PF_MODE] == MV_KEY_SINGLE ? 32768 : bal_gain(e->perf[MV_PF_BAL], MV_PART_UP);
            g[1] = e->perf[MV_PF_MODE] == MV_KEY_SINGLE ? 32768 : bal_gain(e->perf[MV_PF_BAL], MV_PART_LO);
            mix = g[0] != 32768 || g[1] != 32768 || e->part[0].bal_g != 32768 || e->part[1].bal_g != 32768;
            for (i = 0; i < MV_CTL; i++) bus[i] = 0;
            pbus[0] = bus; pbus[1] = bus;       /* at the centre both parts add straight into the bus */
            if (mix) { pbus[1] = lbus; for (i = 0; i < MV_CTL; i++) lbus[i] = 0; }
            for (v = 0; v < MV_NV; v++)
                if (mv_env_active(&e->v[v].aenv)) {
                    uint32_t pi = e->v[v].part;
                    voice_block(e, part_b(e, pi), &e->v[v], pbus[pi], l1u[pi], l1g[pi], l2u[pi], wheel, bend_q8[pi]);
                }
            if (mix) {                          /* UPPER x its gain in place, + LOWER x its: each ramped over the
                                                   block from where it was (no zipper on a BALANCE sweep) */
                int32_t gu = e->part[0].bal_g, gl = e->part[1].bal_g;
                int32_t su = (g[0] - gu) / (int32_t)MV_CTL, sl = (g[1] - gl) / (int32_t)MV_CTL;
                for (i = 0; i < MV_CTL; i++) {
                    gu += su; gl += sl;
                    bus[i] = (int32_t)(((int64_t)bus[i] * gu + (int64_t)lbus[i] * gl) >> 15);
                }
            }
            e->part[0].bal_g = g[0]; e->part[1].bal_g = g[1];
            mv_fx_drive(&e->fx, bus, MV_CTL);   /* DISTORTION (before TONE, as the JT) */
            mv_tone(e, bus, MV_CTL, tb, tt);
            mv_fx_out(&e->fx, bus, out + 2u * k, MV_CTL);   /* MULTI-FX, DELAY -> L R (or mono to both) */
            mv_lim_block(&e->fxlim, out + 2u * k);  /* FX LIMITER: 0.97 FS, look-ahead and ramped, never a clamp
                                                       (16 frames late; on every path, so the latency never jumps) */
            mv_rv_process(&e->rv, out + 2u * k, MV_CTL);    /* REVERB on L R, after the limiter (the JT's order) */
            mv_lim_block(&e->lim, out + 2u * k);    /* OUTPUT LIMITER: 0.97 FS at most (16 frames late) */
        }
    }
}

int mv_engine_sounding(const mv_engine_t *e, uint32_t note)
{
    uint32_t v;
    for (v = 0; v < MV_NV; v++)
        if (e->v[v].gate && e->v[v].note == note) return 1;
    return 0;
}

uint32_t mv_engine_active(const mv_engine_t *e)
{
    uint32_t v, n = 0;
    for (v = 0; v < MV_NV; v++) n += (uint32_t)mv_env_active(&e->v[v].aenv);
    return n;
}

int mv_engine_quiet(const mv_engine_t *e)
{
    uint32_t v;
    for (v = 0; v < MV_NV; v++)
        if (mv_env_active(&e->v[v].aenv) || e->v[v].gain) return 0;
    return !mv_fx_ringing(&e->fx) && !mv_rv_ringing(&e->rv) && !mv_lim_pending(&e->fxlim);   /* a tail still sounds */
}
