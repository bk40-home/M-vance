/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance voice engine, v0: the ported parts played from the 248-byte JP patch (Stage 6 start).
 *
 * Signal flow per voice (JP-8080, spec draft 0.3 section 2):
 *   OSC1 + OSC2 (or RING = OSC1 x OSC2 in OSC2's place) -> BALANCE -> JpFilter -> AMP (level x A-ENV x tremolo)
 *   -> voice gain 1/8 -> patch gain -> (all voices) -> [DISTORTION] -> TONE bass / treble -> [MULTI-FX] -> [DELAY]
 *   -> [limiter] -> [REVERB] -> OUTPUT LIMITER -> out L R   (the bracketed stages: mv_fx.h / mv_reverb.h, the JT's
 *   order; mono to both sides without them. The output limiter, mv_lim.h, always: nothing past 0.97 of full scale)
 *   TONE   the JT's JP law (FxChain, 2026-10-23): first-order shelves, fixed inner corners 445 / 951 Hz, moving
 *          outer corner; TPT one-poles; once on the summed bus (it is one stage after the voices on the JT
 *          too), skipped while both are at 0
 *   SYNC   OSC1 is the master, OSC2 the slave, reset from a clock running at OSC1's UNMODULATED pitch (the JE:
 *          with sync on, OSC2 is the same at every X-MOD setting), so X-MOD only bends OSC1's audible phase
 *   X-MOD  OSC2 frequency-modulates OSC1 through zero, by a fixed 69 Hz per knob step x the JpXmodLaw scale of
 *          OSC2's wave (triangle 1, saw 0.5, pulse 0.296 .. 0.277 by width; x 0.623 under sync for non-pulse)
 *   Not on SUPER SAW / FEEDBACK OSC1: no X-MOD (JP manual p.59) and no sync (no wrapping master), as the JT
 *
 * Laws used (all from mv_tables.h, i.e. the JT's measured JP laws):
 *   pitch     note + OSC SHIFT (+ OSC2 RANGE / FINE-WIDE) + bend (BEND UP / DOWN) + LFO1 / LFO2 (JpModLaw depth) +
 *             PITCH ENV (JpModLaw envelope form = 2 x the LFO depth), routed by LFO1 & ENV DEST: OSC1+2, OSC2,
 *             or X-MOD (LFO1 adds D/16.25 and the env D/18 knob steps to the X-MOD knob, JpModLaw 2026-10-24)
 *   filter    cutoff code + key follow (D2: (b-64)/32 or /64 oct/oct about note 60) + F-ENV x JpModLaw depth (the
 *             LFO path's curve, 1 x: Kris 31 Oct, 1a; the pitch env is 2 x on the JE, so a capture may move this)
 *             + LFO1 / LFO2 (JpModLaw, in octaves), all added as cutoff codes
 *   LFO2      every LFO2 depth x mod wheel / 127 (linear, scene 19)
 *   amp       LEVEL byte / 127, A-ENV, LFO1 (byte 52 when LFO1 MOD = OFF) and LFO2 as 1 + ampIndex x wave
 *   envelopes JP law (mv_env); sustain linear (Kris 31 Oct); pitch env = attack / decay to 0
 *
 * v0 limits (flagged in the delivery, nothing invented in their place):
 *   NOISE (OSC1 wave 2, OSC2 wave 3): white noise through its own -12 low pass, CTRL1 = cutoff (scene 17 law),
 *   CTRL2 = resonance: PROVISIONAL law (Kris 1 Nov, option 1a; a capture will refine it). As a carrier or sync
 *   slave it has no phase, so X-MOD and sync do not act on it; as an X-MOD source it counts as unmeasured (1 x)
 *   PAN (byte 58 AUTO / MANUAL): a placeholder, pan_position() returns centre and the out is mono (Kris 1 Nov,
 *   option 3);
 *   unison, morph
 *   CTRL2 (LFO1 -> shape / PWM, on SQR / SAW / TRI / TRI MOD) is linear: 127 swings the CTRL1 knob by its full
 *   range (law not measured). On SUPER SAW, CTRL1 / CTRL2 are DETUNE / MIX; on FEEDBACK, HARMONICS / FEEDBACK
 * Every key: OSC1 / OSC2 start at random phases, the Super Saw draws seven, the feedback osc starts fresh at
 * OSC1's (the JT's OscSection::noteOn: kills 8-voice lockstep; the Super Saw requires it).
 *
 * Play modes (JT VoiceAllocator; JE scene 18 for legato):
 *   POLY     the same note, else a free voice, else the quietest release, else the oldest held (first nv voices)
 *   MONO     (byte 71) one voice, last-note priority; a release returns to the newest key still held
 *   LEGATO   (byte 72) in MONO / UNISON a key while another is held only moves the pitch (no envelope restart),
 *            also on the return; off, every note and every return retriggers
 *   UNISON   (byte 243) every voice (up to nv) on the one held note, detuned evenly over -/+ DETUNE (byte 244,
 *            0..50) / 100 of a semitone: the JT's unison (the JP-8080's own: to confirm, Kris 1 Nov)
 *   PORTA    (bytes 69 / 70) the JT's glide: from the last note played, a fixed fraction of the remaining
 *            frequency distance a control block, 2 ms .. 5 s over the knob (MV_PORTA_Q31; not JP-measured)
 *   A change of mode fades every voice out in one block and clears the note state (no stuck notes).
 * Performance (Kris 3 Nov, the JT-8000's Performance / PerfRouter / Layer: fm1-port/mvance-layer-split-proposal-
 * 2026-11-03.md): two PARTS, UPPER (patch, the one a single plays) and LOWER (lower), each a whole JP patch with its
 * own voices, LFO1 / LFO2, note stack, play mode, glide and bend ranges. perf[] says how they are played:
 *   MODE     SINGLE (UPPER alone, on up to nv voices), DUAL (both on every key), SPLIT (LOWER below SPLIT, UPPER
 *            from it: the JP's sides; the JT has them the other way round, a later JT fix)
 *   VOICES   the 8 voices cut between them, LOWER + UPPER = 1+7 .. 7+1 (the JT's voice split, default 4+4); a
 *            change fades every voice out in one block and re-slices (no voice keeps the other part's notes)
 *   BALANCE  the JT's law, full at the centre: LOWER at unity 0..64 and out by 127, UPPER the mirror; at 64 (and in
 *            SINGLE) no mixing stage at all
 *   CH LO / CH UP  a part's MIDI channel (0: the global one; mvance.c gates MIDI in with them, then the split)
 *   ARP      which part(s) the arp plays: AUTO (UPPER in SINGLE, BOTH in DUAL, LOWER in SPLIT), UPPER, LOWER, BOTH.
 *            Keys for the other part play it directly, so a split's left hand arpeggiates under the right
 * TONE, MULTI-FX, DELAY are one chain, set by UPPER's bytes (the RAM holds one delay); reverb and limiter global.
 * Threads: the main loop calls note_on / note_off / set_bend / set_wheel and edits the patch bytes; the audio
 * ISR calls render. Notes cross in a single-producer ring, so the ISR alone owns the voices. */
#pragma once
#include <stdint.h>
#include "mv_patch.h"
#include "mv_osc.h"
#include "mv_env.h"
#include "mv_lfo.h"
#include "mv_filter.h"
#include "mv_fx.h"
#include "mv_reverb.h"
#include "mv_arp.h"
#include "mv_lim.h"
#include "mv_perf.h"                         /* the performance bytes: MV_PF_*, MV_KEY_*, MV_TO_* */

#ifndef MV_NV
#define MV_NV 8u                                /* voices built in; how many play is mv_engine_t.nv (1..MV_NV) */
#endif
#define MV_EVQ 32u                              /* note events in flight (power of two) */

#ifndef MV_PROFILE
#define MV_PROFILE 0                            /* 1: time the stages (the app supplies mv_prof_ticks) */
#endif
#if MV_PROFILE
uint32_t mv_prof_ticks(void);                   /* a free-running counter (the app: fm1_ticks) */
#endif

typedef struct {
    mv_osc_t o1, o2;
    mv_ssaw_t ss;                               /* OSC1 SUPER SAW */
    mv_jfbk_t fb;                               /* OSC1 FEEDBACK OSC */
    mv_flt_t nf[2];                             /* NOISE: its own resonant low pass, OSC1 / OSC2 */
    uint32_t nrng;                              /* NOISE: xorshift32 */
    uint8_t nkey[2][2];                         /* the CTRL1 / CTRL2 bytes each noise filter was set for */
    mv_env_t aenv, fenv, penv;
    mv_flt_t flt;
    int32_t gain;                               /* the amp gain reached at the end of the last block, Q15 */
    uint32_t clk;                               /* sync master clock: OSC1's unmodulated phase (Q32) */
    uint32_t g_cur, g_tgt;                      /* PORTAMENTO: where the slide is / its note (note-only steps, Q32) */
    uint8_t g_on;                               /* a slide in flight */
    int16_t det_q8;                             /* UNISON detune of this voice, Q8 semitones */
    uint32_t age;
    uint8_t note, gate, sync_live, flt_key[3];             /* flt_key: the mode / reso / cutoff bytes mv_flt_set last saw */
    uint8_t part;                               /* the part it belongs to (MV_PART_UP / MV_PART_LO): its slice */
} mv_voice_t;

/* a part's own state (the ISR's): what was per engine before the performance */
typedef struct {
    mv_lfo_t lfo1, lfo2;
    uint8_t stack[16], nstack;                  /* MONO: the held keys, newest last */
    uint8_t mode;                               /* play mode its voices are in (POLY / MONO / UNISON) */
    uint8_t v0, nvp;                            /* its voices: v[v0] .. v[v0 + nvp - 1] (nvp 0: it does not play) */
    int32_t uni_note;                           /* UNISON: the held note, -1 none */
    uint32_t last_inc;                          /* the last note played (note-only step): the glide's start */
    int32_t bal_g;                              /* its bus gain (BALANCE) reached at the end of the last block, Q15 */
} mv_part_t;

typedef struct {
    mv_patch_t patch;                           /* UPPER's sound (a single's): main loop writes bytes, the ISR reads */
    mv_patch_t lower;                           /* LOWER's sound (played in DUAL / SPLIT) */
    volatile uint8_t perf[MV_PF_N];             /* how the two are played (MV_PF_*; main loop writes) */
    mv_part_t part[2];                          /* UPPER, LOWER */
    uint32_t slice_key;                         /* the cut the voices are in (mode, voice split, nv) */
    mv_voice_t v[MV_NV];
    volatile int32_t bend14;                    /* pitch bend, -8192..+8191 */
    volatile uint32_t wheel;                    /* mod wheel CC1, 0..127 */
    volatile uint32_t evq[MV_EVQ];              /* note events: to << 16 | on << 15 | note << 7 | velocity */
    volatile uint32_t ev_w, ev_r;
    uint32_t ages;
    volatile uint32_t nv;                       /* voices that take new notes, 1..MV_NV (set by the main loop: a
                                                   voice above it finishes its note and then rests) */
    int32_t tone_b, tone_t;                     /* tone one-pole states (bass LP, treble LP), Q24 */
    mv_fx_t fx;                                 /* MULTI-FX / DISTORTION / DELAY / limiter (mv_fx.c) */
    mv_rv_t rv;                                 /* the plate reverb after them (mv_reverb.c) */
    volatile uint8_t rvb[MV_RV_N];              /* its settings (global, not in the patch: the main loop sets them) */
    mv_lim_t fxlim;                             /* the FX limiter: after MULTI-FX / DELAY, before the reverb (the JT's
                                                   place; Kris 5 Nov 1a, M-vance's look-ahead law in place of the JT's) */
    mv_lim_t lim;                               /* the output limiter, last of all (mv_lim.c) */
    mv_arp_t arp;                               /* the arpeggiator (mv_arp.c, Skiff's): runs on the ISR, a tick a block */
    volatile uint8_t arpb[MV_ARP_N];            /* its settings (global; the panel sets them) */
    volatile uint8_t tempo;                     /* TEMPO, BPM (global; the main loop turns it into arp_beat) */
    volatile uint32_t arp_beat;                 /* a quarter note in samples: TEMPO, or the MIDI clock's (main loop) */
    volatile uint32_t lfo1_sync_inc[2];         /* a part's synced LFO1 step (mv_lfo_sync_inc), 0 free: main loop sets */
    uint8_t sync_s[2];                          /* main loop: the LFO1 SYNC byte each part's step was made for */
    uint32_t sync_beat[2];                      /* main loop: and the beat (samples) it was made for */
    volatile uint8_t arp_start;                 /* MIDI START seen (main loop sets, the ISR clears) */
    uint8_t arp_live;                           /* the arp the ISR is running (follows arpb[ON] at a block's start) */
#if MV_PROFILE
    volatile uint32_t prof_osc, prof_flt;       /* MV_PROFILE: ticks spent in the oscillators / the filter */
#endif
} mv_engine_t;



void mv_engine_init(mv_engine_t *e);
void mv_engine_note_on(mv_engine_t *e, uint32_t note, uint32_t vel);   /* main loop */
void mv_engine_note_off(mv_engine_t *e, uint32_t note);                 /* main loop */
/* the same with a destination: MV_TO_UP / MV_TO_LO / MV_TO_ROUTE (MIDI in, gated by the parts' channels) */
void mv_engine_note_to(mv_engine_t *e, uint32_t note, uint32_t vel, uint32_t to);   /* vel 0: off */
void mv_engine_render(mv_engine_t *e, int32_t *out, uint32_t n);        /* ISR: n stereo frames, adds Q15 */
/* main loop, after arp_beat or a program / perf byte may have moved: each part's synced LFO1 step from its LFO1 SYNC
 * byte and arp_beat. A divide only when the byte changes or the beat moves more than 1/512 (0.2 %) from the one the
 * step was made for: MIDI clock jitter leaves the step alone (Kris 5 Nov, fix 2) */
void mv_engine_sync(mv_engine_t *e);
int mv_engine_sounding(const mv_engine_t *e, uint32_t note);            /* a key held for this note */
int mv_engine_quiet(const mv_engine_t *e);                              /* nothing sounds, not even a release */
uint32_t mv_engine_active(const mv_engine_t *e);                        /* voices sounding now (for the meter) */
/* the TONE stage on a mono bus (Q15), BASS / TREBLE bytes (64 = flat). Used by render; public for the tests */
void mv_tone(mv_engine_t *e, int32_t *bus, uint32_t n, uint32_t bass, uint32_t treble);
