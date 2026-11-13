/* SPDX-License-Identifier: GPL-3.0-only */
/* A stand-in for Emscripten's header, so sim/mv_websim.c (and the app with it) builds natively with cc for
 * tests/websim_tiles_test.c (Kris 13 Nov): the yield calls the test's driver where the browser would run JS. */
#pragma once
#define EMSCRIPTEN_KEEPALIVE
/* the JS hooks (Module.onMidiOut, Module.onSysEx): declared here, defined by the test, which plays the page */
#define EM_JS(ret, name, params, ...) ret name params;
void wsim_test_yield(void);
#define emscripten_sleep(ms) wsim_test_yield()
