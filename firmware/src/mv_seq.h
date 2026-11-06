/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance sequencer: two real-time recording tracks, LOWER and UPPER (Kris 6 Nov 2026, phase 1 of
 * fm1-port/mvance-sequencer-proposal-2026-11-06.md; signed off "go with all your recommendations": 1a 2a 3a 4a 5a
 * 6a 7a, 256 events a track, MIDI START / STOP drive it).
 *
 *   TRACKS   one per part (MV_PART_UP / MV_PART_LO): a track plays only into its own part (the engine's ev_play), so
 *            it uses that part's voices, play mode and glide. In SINGLE only UPPER has voices (4a): LOWER's events go
 *            nowhere
 *   RECORD   real time (3a): the keys a part gets while its track is armed and the transport runs, timed in ticks
 *            of MV_SEQ_PPQN a quarter (5.2 ms at 120 BPM). QUANT moves each note-on to the nearest grid line as it
 *            is written; its note-off moves by the same amount, so the note keeps its length (at least a tick).
 *            Every pass overdubs. A note held across the loop's end simply ends where it was let go, on the next
 *            pass round (the loop is a circle)
 *   PLAY     each tick, the events at that tick into the part; a note the track started is remembered (sound[]),
 *            so STOP, MUTE and CLEAR end exactly what it started
 *   TRANSPOSE (phase 2, Kris 6 Nov: fm1-port/mvance-sequencer-phase2-proposal-2026-11-06.md) a track with TRANSPOSE
 *            on, running and not empty, takes its part's keys: a key sets the track's offset (key - ROOT, -24..+24),
 *            which stays after the key goes up (latched). It applies from the next note-on: a note sounding keeps its
 *            pitch, and its note-off ends the pitch it started at (played[]). ROOT: the first note-on written to an
 *            empty track; the panel may set it; CLEAR forgets it (and the offset)
 *   CLIPS    (phase 3, mv_clip.h) a track's events saved to flash and loaded back: mv_seq_load replaces a track's
 *            events (the ISR, at a block's start: what it started ends first), its ROOT with them, the offset 0
 *   VIEW     ver counts every change to a track's events (a write, a CLEAR): the main loop (the piano roll) copies
 *            the events and keeps the copy only if ver did not move meanwhile (the ISR is never interrupted by it)
 *   LENGTH   per track, 1 / 2 / 4 / 8 bars of 4/4 (codes 1..4), so a 1-bar bass can run under a 4-bar line
 *   CLOCK    the tick comes from the quarter note in samples (TEMPO, or the MIDI clock's average: the arp's beat).
 *            After a MIDI START it is also held to the clock's pulses (4 ticks a pulse): never more than a pulse
 *            ahead, and caught up when behind (64 ticks a block at most), so it cannot drift from the DAW
 *   EVENTS   4 bytes: tick, note, velocity (0 = note-off). Kept sorted by tick (at the same tick, in the order they
 *            were written). MV_SEQ_NEV a track (256: 1 KB); a note-on is only written when there is room for it and
 *            for the note-offs still to come, so the cap never leaves a note hanging
 *
 * Runs on the ISR (the engine calls it once per 16-frame block, and from the key queue). Integers only, no
 * division in the per-tick path; no allocation. The notes go out through a callback (the engine: ev_play). */
#pragma once
#include <stdint.h>

#define MV_SEQ_PPQN 96u                         /* ticks a quarter note (5.2 ms at 120 BPM) */
#define MV_SEQ_BAR (4u * MV_SEQ_PPQN)           /* ticks a bar (4/4) */
#define MV_SEQ_NEV 256u                         /* events a track (Kris 6 Nov, decision 8: 256 = 128 notes, 1 KB) */
#define MV_SEQ_NHELD 16u                        /* keys a track can be recording at once (as the arp's held list) */
#define MV_SEQ_SKIP 0x80u                       /* ev.note: written ahead of the playhead this pass (the key already
                                                   sounded live): skipped once, then played every pass after */

/* the settings (global, kept in flash with the arp's: Kris 6 Nov, 6a), the engine's seqb[] */
enum { MV_SQ_LENUP, MV_SQ_LENLO, MV_SQ_QUANT, MV_SQ_MUTEUP, MV_SQ_MUTELO, MV_SQ_XPOSE, MV_SQ_N };
#define MV_SQ_LEN(part) (MV_SQ_LENUP + (part))  /* the LENGTH byte of part MV_PART_UP / MV_PART_LO */
#define MV_SQ_MUTE(part) (MV_SQ_MUTEUP + (part))
enum { MV_SQQ_OFF, MV_SQQ_16, MV_SQQ_8, MV_SQQ_16T, MV_SQQ_8T, MV_SQQ_N };   /* QUANT */
enum { MV_SQX_OFF, MV_SQX_LO, MV_SQX_UP, MV_SQX_BOTH, MV_SQX_N };              /* TRANSPOSE: the tracks it is on for */
#define MV_SEQ_XMAX 24                          /* the offset's range: -24 .. +24 semitones */
#define MV_SEQ_NOROOT 0xFFu                     /* root: none yet (an empty track) */
/* ranges and defaults (the panel and the settings load). LENGTH 1..4 = 1, 2, 4, 8 bars: 0 is never valid, so a
 * settings record from before the sequencer (its spare bytes 0) is seen and given the defaults */
static const uint8_t MV_SQ_MIN[MV_SQ_N] = {1, 1, 0, 0, 0, 0};
static const uint8_t MV_SQ_MAX[MV_SQ_N] = {4, 4, MV_SQQ_N - 1u, 1, 1, MV_SQX_N - 1u};
static const uint8_t MV_SQ_DEF[MV_SQ_N] = {2, 2, MV_SQQ_16, 0, 0, MV_SQX_OFF};   /* 2 bars each, 1/16, none muted,
                                                   TRANSPOSE off (0: a record from phase 1 reads as off) */

typedef struct {
    uint16_t tick;                              /* when, in the track's loop (0 .. its length - 1) */
    uint8_t note;                               /* the note (bit 7: MV_SEQ_SKIP) */
    uint8_t vel;                                /* 1..127 a note-on, 0 its note-off */
} mv_seq_ev_t;

typedef struct {
    mv_seq_ev_t ev[MV_SEQ_NEV];
    uint16_t n;                                 /* events written */
    uint16_t next;                              /* playback: the first event at or after tick */
    uint16_t tick;                              /* the next tick to play (the ones before it have, this pass) */
    uint16_t len;                               /* the loop in ticks the track last played with (0: not yet) */
    uint8_t sound[16];                          /* pitches the track has started and not yet ended (bit per pitch) */
    uint8_t played[128];                        /* the pitch each recorded note is sounding at (offset applied), or
                                                   0xFF: its note-off ends that pitch, whatever the offset is now */
    uint8_t root;                               /* TRANSPOSE's reference note (MV_SEQ_NOROOT: none). The ISR sets it
                                                   (the first note-on, CLEAR); the panel may set it (a byte store) */
    int8_t xpose;                               /* the offset the keys set (-24 .. +24) */
    uint8_t rec_note[MV_SEQ_NHELD];             /* keys being recorded: the note .. */
    int16_t rec_shift[MV_SEQ_NHELD];            /* .. how far QUANT moved its note-on (ticks, + later) .. */
    uint16_t rec_on[MV_SEQ_NHELD];              /* .. and the tick it was written at */
    uint8_t nrec;
    uint8_t muted;                              /* MUTE as last seen (a change ends its notes) */
} mv_seq_track_t;

typedef struct {
    mv_seq_track_t t[2];                        /* MV_PART_UP, MV_PART_LO */
    uint32_t acc;                               /* samples x PPQN into the tick (a tick = beat samples of it) */
    uint32_t ticks;                             /* ticks since the start (the MIDI clock lock: 4 a pulse; the LEDs) */
    uint32_t beat;                              /* the quarter note (samples) the last tick ran at */
    uint8_t first;                              /* just started: tick 0 plays on the next mv_seq_tick */
    volatile uint32_t ver;                      /* +1 at every change to the events (the piano roll's check) */
} mv_seq_t;

/* where a note goes: part, note, velocity (0 = off). ctx: the caller's (the engine) */
typedef void (*mv_seq_emit_t)(void *ctx, uint32_t part, uint32_t note, uint32_t vel);

void mv_seq_init(mv_seq_t *s);                  /* empty tracks, stopped */
void mv_seq_start(mv_seq_t *s);                 /* from bar 1: tick 0 plays on the next mv_seq_tick */
/* stopped: the keys still being recorded get their note-offs (now), every note the tracks started ends */
void mv_seq_stop(mv_seq_t *s, const volatile uint8_t *set, mv_seq_emit_t emit, void *ctx);
/* n samples on, at beat samples a quarter note. lock: after a MIDI START, the tick the latest clock pulse is (the
 * engine: (pulses - 1) x 4, the first pulse being beat 1); the ticks run at most 3 past it, and catch up to it
 * when behind. ~0u: free (TEMPO, or a clock with no START) */
void mv_seq_tick(mv_seq_t *s, const volatile uint8_t *set, uint32_t n, uint32_t beat, uint32_t lock,
                 mv_seq_emit_t emit, void *ctx);
/* a key on / off that part got while its track is armed and running: written (vel 0 = off) */
void mv_seq_key(mv_seq_t *s, const volatile uint8_t *set, uint32_t part, uint32_t note, uint32_t vel);
/* the track no longer armed: the keys it was recording get their note-offs now (nothing is left hanging) */
void mv_seq_disarm(mv_seq_t *s, uint32_t part);
/* the track emptied; the notes it started end */
void mv_seq_clear(mv_seq_t *s, uint32_t part, mv_seq_emit_t emit, void *ctx);
/* TRANSPOSE on for part's track (the setting), and the track has a ROOT (it is not empty) */
int mv_seq_xpose_on(const mv_seq_t *s, const volatile uint8_t *set, uint32_t part);
/* a key for a transposing track: its offset (key - ROOT, held to +-MV_SEQ_XMAX), from the next note-on */
void mv_seq_transpose(mv_seq_t *s, uint32_t part, uint32_t key);
/* a clip into part's track (phase 3): its notes end, then n events (sorted by tick) and the clip's ROOT replace
 * the track's; the playhead runs on (the next event from where it is); nothing being recorded is kept */
void mv_seq_load(mv_seq_t *s, uint32_t part, const mv_seq_ev_t *ev, uint32_t n, uint32_t root, mv_seq_emit_t emit,
                 void *ctx);
/* the track's loop in ticks, from its LENGTH code (1..4: 1, 2, 4, 8 bars) */
uint32_t mv_seq_len(const volatile uint8_t *set, uint32_t part);
