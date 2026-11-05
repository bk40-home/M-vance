/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance envelope, fixed point: the JP-8000 stage law measured on the JE-8086 (JT JpEnvLaw / EnvGen JP law,
 * Kris Bishop, MIT), one generator for amp, filter and pitch envelopes and the LFO1 fade.
 *
 *   attack   level' = 1.25 - (1.25 - level) * cA, stopped at exactly full (reaches it at ~1.61 tau)
 *   decay    level' = sus + (level - sus) * cD, parks in SUSTAIN within 1e-4 of the sustain level
 *   release  level' = level * cR, ends (IDLE, level 0) below -60 dB (MV_ENV_OFF_Q30)
 *   c = e^(-MV_CTL / fs / tau): MV_ENV_COEF[byte] (firmware/gen/mv_tables.h, from the JT's own table)
 *
 * Runs once per control block (MV_CTL frames): one multiply-add per active stage, nothing when IDLE or
 * holding SUSTAIN. The level is Q30 (full = 2^30, the 1.25 target fits below 2^31). The voice ramps the
 * block-to-block step across the frames (mv_ramp_step), so even the fastest stages stay click free.
 * Gates start from the current level (retrigger and release never jump), as on the JT. */
#pragma once
#include <stdint.h>

enum { MV_ENV_IDLE, MV_ENV_ATTACK, MV_ENV_DECAY, MV_ENV_SUSTAIN, MV_ENV_RELEASE, MV_ENV_FADE };

typedef struct {
    int32_t level;                              /* Q30, the level at the end of the last block */
    uint8_t stage;                              /* MV_ENV_* */
    /* parameters, set on a patch edit (control plane) */
    uint32_t ca, cd, cr;                        /* per-block multipliers, Q31 (MV_ENV_COEF) */
    int32_t sus;                                /* sustain level, Q30 */
} mv_env_t;

/* times from the JP knob bytes (0..127) and sustain byte (0..127, linear: the JT's mapping) */
void mv_env_set(mv_env_t *e, uint32_t a, uint32_t d, uint32_t s, uint32_t r);
void mv_env_on(mv_env_t *e);                    /* key down: attack from the current level */
void mv_env_off(mv_env_t *e);                   /* key up: release from the current level */
void mv_env_fade(mv_env_t *e);                  /* one-block fade for a voice steal (next tick: 0, IDLE) */
void mv_env_kill(mv_env_t *e);                  /* instant silence (panic) */
int32_t mv_env_tick(mv_env_t *e);               /* advance one control block, returns the level (Q30) */
static inline int mv_env_active(const mv_env_t *e) { return e->stage != MV_ENV_IDLE; }
