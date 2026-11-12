/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance build switches (Kris 12 Nov: fm1-port/mvance-dock-feasibility-proposal-2026-11-12.md, D1a). The defaults
 * are the standalone firmware, bit for bit as before. The fm1-dock module (dock/module.cfg) sets the cuts, because
 * a module has 272 KiB of RAM, no Roland SysEx in, and its host shows the render load itself.
 *   ./build.sh                                        the standalone firmware: everything on
 *   .\flash.ps1 -Trial                                 the module's cuts on the standalone firmware (flash.ps1)
 * Each switch is 0 or 1 (MV_PARTS 1 or 2, MV_PROGS a count, MV_DLY_MS milliseconds). Included first by mvance.c,
 * mv_ui.c, mv_prog.h and mv_fx.h. Constants only.
 * The fm1-dock module's set: MV_DEBUG 0, MV_XFER 0, MV_SEQ 0, MV_PARTS 1, MV_PROGS 64, MV_DLY_MS 900u. */
#pragma once

#ifndef MV_DEBUG
#define MV_DEBUG 1          /* the DEBUG page (GLO's last) and the render-time meter behind it (mv_debug.h). Off: no
                               page, and the audio ISR skips its two timer reads and the sums each buffer */
#endif

#ifndef MV_XFER
#define MV_XFER 1           /* the program transfer: JP-8080 SysEx in and out over USB (mv_xfer.h, docs/bank.html) and
                               its RECEIVING / SENDING screen. Off: Roland SysEx is ignored (APP_SYSEX not set) */
#endif

#ifndef MV_SEQ
#define MV_SEQ 1            /* the sequencer's panel: PLAY, REC, the SEQ pages, the piano roll (mv_roll.h) and the clips
                               (mv_clip.h, its flash slot and the factory clips). Off: none of these, and MIDI START /
                               STOP move only the arp. The engine's sequencer (mv_seq.h) stays, never started: it
                               costs nothing stopped (mv_voice.c seq_block) */
#endif

#ifndef MV_PARTS
#define MV_PARTS 2          /* 2: UPPER and LOWER (SINGLE / DUAL / SPLIT, PERFORM, part select). 1: UPPER only: every
                               program plays SINGLE, PERFORM and part select are gone, and the programs are records of
                               32: their patches and 4 performance bytes each (mv_prog.h: MV_PROG_PK), with no LOWER */
#endif

#ifndef MV_PROGS
#define MV_PROGS 0          /* the programs: 0 = all of the factory bank's (64); else the first MV_PROGS of it, a
                               multiple of 16 (of 32 with MV_PARTS 1: mv_prog.h) */
#endif

#ifndef MV_DLY_MS
#define MV_DLY_MS 1303u     /* the delay lines' length (ms). 1303 holds PAN L->R's R at its 1302.1 ms top. Shorter:
                               every tap (manual and synced) is held at the line's end, so a longer setting plays at
                               the line's length; RAM 2 x 2 B x (44.1 x ms + 3) */
#endif

#if (MV_DEBUG != 0 && MV_DEBUG != 1) || (MV_XFER != 0 && MV_XFER != 1) || (MV_SEQ != 0 && MV_SEQ != 1)
#error "mv_build.h: MV_DEBUG, MV_XFER and MV_SEQ are 0 or 1"
#endif
#if MV_PARTS != 1 && MV_PARTS != 2
#error "mv_build.h: MV_PARTS is 1 or 2"
#endif
#if MV_PARTS == 1 && MV_XFER
#error "mv_build.h: MV_PARTS 1 needs MV_XFER 0 (the transfer is the JP's two-part performance)"
#endif
#if MV_DLY_MS < 200u || MV_DLY_MS > 1303u
#error "mv_build.h: MV_DLY_MS is 200..1303 (the pool holds 1303 ms at most)"
#endif
