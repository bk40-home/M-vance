/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance pitch: the JP-8080's pitch controls in Q8 semitones (MIDI note * 256), and Q8 -> oscillator phase step.
 *   OSC SHIFT (byte 73)         0..4 = -2..+2 octaves, both oscillators
 *   OSC2 RANGE (35), FINE (36)  -24..+24 semitones and +-50 cents; RANGE 0 / 50 = -WIDE / +WIDE: FINE/WIDE then
 *                               sets +-4 octaves in half-semitone steps (the JE's table, firmware/gen/mv_tables.h)
 *   BEND UP / DOWN (67, 68)     0..24 semitones each way
 * Control rate only (once a block or on a change); mv_pitch_inc is two table reads, a multiply and a shift. */
#pragma once
#include <stdint.h>
#include "mv_fixed.h"
#include "mv_tables.h"

#define MV_PITCH_MAX_Q8 (143 * 256 + 255)       /* top of the phase-step table: 15.8 kHz */

uint32_t mv_pitch_inc(int32_t pitch_q8);                         /* phase step per frame (Q32) at MV_FS */
int32_t mv_shift_q8(uint32_t shift_byte);                        /* OSC SHIFT -> +-24 * 256 */
int32_t mv_osc2_q8(uint32_t range_byte, uint32_t fine_byte);     /* OSC2 RANGE + FINE/WIDE -> offset from OSC1 */
int32_t mv_bend_q8(int32_t bend14, uint32_t up_semis, uint32_t down_semis);   /* bend -8192..+8191 (MIDI, centred) */
