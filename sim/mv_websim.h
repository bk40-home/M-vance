/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* M-vance online simulator — the browser (WebAssembly) platform backend's JS-facing interface.
 *
 * platform.c includes mv_websim.c when DINGHY_WEBSIM is set, exactly as it includes tests/host.c for
 * DINGHY_HOST. mv_websim.c supplies the same platform symbols the app needs (screen, input, storage, MIDI,
 * the audio-ISR driver) but, instead of host.c's one-shot script + WAV file, it runs the real app loop under
 * Emscripten Asyncify and exchanges audio, video and input with JavaScript in real time.
 *
 * The app's control flow is unchanged: main() -> app_main() (mvance.c) loops forever calling platform_poll();
 * platform_poll() here fills an audio ring (the real app_audio path, Q15 -> float), advances the ms clock with
 * the audio, takes the input JS has posted, then yields with emscripten_sleep(). The AudioWorklet pulls audio
 * (wsim_pull); a canvas draws the framebuffer (wsim_fb). These are the only symbols JavaScript touches: */
#pragma once
#include <stdint.h>

/* audio: copy up to max_frames stereo frames (L R interleaved, float -1..1) out of the ring; returns frames copied */
int       wsim_pull(float *dst, int max_frames);
/* video: a pointer to the 240 x 240 RGB565 framebuffer (JS reads it via HEAPU16, length 240*240) */
uint16_t *wsim_fb(void);
/* input — call from JS between frames: */
void wsim_keys(uint32_t mask);          /* the 27 on-screen keys, bit 0 = F3 (drives fm1_in.notes) */
void wsim_btn(int id, int down);        /* a panel button held (1) or let go (0); ids are platform.c's BTN_* enum */
void wsim_enc(int id, int detents);     /* an encoder turned (+ = clockwise); ids are the ENC_* enum */
void wsim_midi(int st, int d1, int d2); /* one MIDI message in, over USB */
void wsim_sysex_in(const uint8_t *p, int n);   /* one SysEx in, F0 .. F7 (the program transfer, mv_xfer.h) */
