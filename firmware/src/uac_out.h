/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (felucca-bsp: FELUCCA_UAC_OUT)
 * Copyright (C) 2026 Kris (bk40-home) (M-vance: the integer port)
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not
 * distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */
/* USB audio OUTPUT: the computer plays through the FM-1 (headphones / speaker), mixed after the synth.
 * Ported from felucca-bsp bsp/chip/usb.c (commit 773b0b9, FELUCCA_UAC_OUT), as fm1-base / fm1-polly run it on the
 * FM-1. Kris 4 Nov 2026, signed off 1a 2a 3c 4a 5a: fm1-port/mvance-usb-playback-proposal-2026-11-04.md.
 *
 * The stream: UAC1, 16-bit stereo, 44.1 kHz only, EP4 OUT isochronous ADAPTIVE (0x04; EP4 IN 0x84 is the recording).
 *   adaptive = the host sends at ITS clock (44 100 Hz); the FM-1 follows it. The I2S runs at ~44 117.6 Hz (+400 ppm,
 *   17.6 frames/s faster), so uac_play resamples: linear interpolation, read phase in Q16, the step steered by how
 *   full the ring is at each render start (target UO_TGT). No feedback endpoint needed.
 *   The steering is proportional only (felucca-bsp's law): the fill settles at UO_TGT plus half the clock mismatch
 *   in Q16 step units, about 3 frames per 100 ppm of the host's clock (USB allows +-500 ppm: +-16 frames).
 *
 * Who runs what (unity build: usb.c includes uac_out.c; platform.c calls uac_play):
 *   uo_service   TIMER5, 2 kHz (from uac_service, also nested in a render): one packet a 1 ms frame -> the ring
 *   uac_play     audio ISR, after the recording's tap and the 24-bit shift: the ring -> added to the output
 *   uo_stream / uo_ep4_reset   EP0 (SET_INTERFACE / SET_CONFIGURATION / bus reset), also from TIMER5
 *
 * What changed from felucca-bsp (decision 2a, 3c):
 *   - integer arithmetic instead of float: M-vance builds without the FPU (tools/build.py: no -mfprev1), so float
 *     here would be software-emulated in the audio ISR. Same law, same constants (uo_step: the same step as the
 *     float law but for a rare +-1 in Q16 where the smoothed error sits on a boundary, ~0.2 % of renders; uo_interp:
 *     within 1 LSB of the 16-bit input of the float interpolation, at the worst full-swing step). tests/uac_out_test.c
 *     checks both against the float.
 *   - split in two (uo_step: the rate control; uo_interp: the resampler) so the test can drive each alone.
 *   - a fixed level (no gain argument): the computer's audio lands at the synth's scale with MASTER full
 *     (Q15 << 7, the platform's -6 dBFS ceiling), so synth + computer at full scale still fit 24 bits: no clamp.
 *     The computer's own volume control sets its level; the FM-1's MASTER knob does not.
 *   - the USB recording never contains it (decision 4a): platform.c taps before calling uac_play.
 *
 * Needs, before inclusion: HALF_FRAMES (platform.c), RING_PUBLISH(). RAM: the ring 8 KiB + a packet 200 B. */
#pragma once
#include <stdint.h>

/* ---- configuration ---- */
#define UO_N 2048u               /* the ring, stereo frames (16-bit L | R << 16); a power of two (decision 5a) */
#define UO_MAXF 49u              /* frames in the largest packet a host may send (44.1 kHz sends 44 or 45) */
#define UO_MAXP (UO_MAXF * 4u)   /* wMaxPacketSize, bytes (196) */
#define UO_TGT (2u * HALF_FRAMES)   /* the fill a render aims to start at: 512 frames, ~11.6 ms of latency */
#define UO_STEP_NOM 65510u       /* the read step at no error, Q16: 65536 x 44 100 / 44 117.6 */
#define UO_ADJ_MAX 400           /* the steering's limit, Q16 per frame (+-0.6 %: far more than the 400 ppm drift) */
#define UO_SMOOTH_Q16 3277       /* the error's one-pole smoothing per render: 0.05 (felucca-bsp's), Q16 */
#define UO_SHIFT 7               /* 16-bit sample -> the platform's 24-bit scale (as platform.c's Q15 << 7) */

_Static_assert((UO_N & (UO_N - 1u)) == 0, "UO_N must be a power of two (the ring index is masked)");
_Static_assert(UO_TGT + HALF_FRAMES + UO_MAXF < UO_N, "the ring holds the target, a render and a packet");

/* ---- the calls (static: this is a unity build, uac_out.c is included by usb.c) ---- */
static void uo_ep4_reset(void);                 /* EP4 OUT: isochronous mode, FIFO flushed (SIE, INDEX left at 4) */
static void uo_stream(uint32_t alt);            /* the host selects alt 0 (stop) / 1 (play) of the output stream */
static void uo_service(void);                   /* TIMER5: take a waiting EP4 OUT packet into the ring */
/* audio ISR: add the computer's audio to n stereo frames of out (L R, the platform's 24-bit scale). Does nothing
 * (not even a pass over the block) unless the host is playing and the ring is primed. */
static void uac_play(int32_t *out, uint32_t n);
/* the parts of uac_play (also for tests/uac_out_test.c) */
static uint32_t uo_step(uint32_t fill);         /* the read step, Q16, from the ring's fill at a render start */
/* add n frames read from ring frame r + pos (Q16 fraction) at step (Q16) to out; returns the new pos (the frames
 * consumed are pos >> 16) */
static uint32_t uo_interp(int32_t *out, uint32_t n, uint32_t r, uint32_t pos, uint32_t step);
