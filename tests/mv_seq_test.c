/* SPDX-License-Identifier: GPL-3.0-only */
/* mv_seq (the two-track sequencer, firmware/src/mv_seq.c): tick timing against the tempo, recording with and
 * without QUANT (a note keeps its length), overdub, a note held across the loop's end, no double note for a key
 * written ahead of the playhead, the 256-event cap (never a note-on without its note-off), STOP / MUTE / CLEAR /
 * disarm ending what was started, per-track LENGTH, and the MIDI clock lock.
 *   cc -std=c99 -O2 -Wall -Ifirmware/src tests/mv_seq_test.c -o mv_seq_test && ./mv_seq_test */
#include <stdio.h>
#include <string.h>
#include "mv_seq.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                                  printf(__VA_ARGS__); printf("\n"); } } while (0)

#define BEAT 22050u                             /* 120 BPM at 44.1 kHz: a tick is 229.7 samples */
static mv_seq_t s;
static uint8_t set[MV_SQ_N];
static uint32_t now;                            /* samples since start() */
static struct { uint32_t t, part, note, vel; } log_[4096];
static uint32_t nlog;

static void emit(void *ctx, uint32_t part, uint32_t note, uint32_t vel)
{
    (void)ctx;
    if (nlog < 4096u) { log_[nlog].t = now; log_[nlog].part = part; log_[nlog].note = note; log_[nlog].vel = vel; }
    nlog++;
}
static void run_lock(uint32_t frames, uint32_t lock)
{
    uint32_t k;
    for (k = 0; k < frames; k += 16u) { mv_seq_tick(&s, set, 16, BEAT, lock, emit, 0); now += 16u; }
}
static void run(uint32_t frames) { run_lock(frames, ~0u); }
static uint32_t T2S(uint32_t ticks) { return ticks * BEAT / MV_SEQ_PPQN; }   /* ticks -> samples */
static void run_ticks(uint32_t ticks) { run(T2S(ticks)); }   /* (to within a block) */
static void start(void)
{
    uint32_t k;
    for (k = 0; k < MV_SQ_N; k++) set[k] = MV_SQ_DEF[k];
    mv_seq_init(&s);
    nlog = 0; now = 0;
    mv_seq_start(&s);
}
/* the tick (from the start) that sample time t is */
static uint32_t tick_at(uint32_t t) { return (uint32_t)((uint64_t)t * MV_SEQ_PPQN / BEAT); }
static uint32_t count(uint32_t part, int on, uint32_t from)   /* events logged at or after sample from */
{
    uint32_t i, n = 0;
    for (i = 0; i < nlog && i < 4096u; i++)
        if (log_[i].part == part && (log_[i].vel != 0) == on && log_[i].t >= from) n++;
    return n;
}
/* a note written: key down at the playhead, held for ticks */
static void note(uint32_t part, uint32_t n, uint32_t ticks)
{
    mv_seq_key(&s, set, part, n, 100);
    run_ticks(ticks);
    mv_seq_key(&s, set, part, n, 0);
}

int main(void)
{
    uint32_t i, j, bar = MV_SEQ_BAR, len2 = 2u * MV_SEQ_BAR;
    uint32_t pass = T2S(len2);                  /* 2 bars in samples (176400) */

    /* 1. the length codes, and empty tracks */
    for (i = 1; i <= MV_SEQ_MAXBAR; i++) { set[MV_SQ_LENUP] = (uint8_t)i; CHECK(mv_seq_len(set, 0) == bar * i, "LENGTH %u bars", i); }
    start(); run(pass * 2u);
    CHECK(nlog == 0, "empty tracks: nothing out (%u)", nlog);
    CHECK(s.ticks >= 2u * len2 && s.ticks <= 2u * len2 + 1u, "ticks after 4 bars: %u", s.ticks);

    /* 2. QUANT OFF: a key at tick 100 held 50 ticks plays back at 100 and 150 in the next pass */
    start(); set[MV_SQ_QUANT] = MV_SQQ_OFF;
    run_ticks(100); note(0, 60, 50);
    CHECK(s.t[0].n == 2u, "2 events written (%u)", s.t[0].n);
    nlog = 0; run(pass + 2000u);
    CHECK(count(0, 1, 0) == 1u && count(0, 0, 0) == 1u, "pass 2: one on, one off (%u)", nlog);
    if (nlog >= 2u) {
        uint32_t t_on = tick_at(log_[0].t) % len2, d = tick_at(log_[1].t) - tick_at(log_[0].t);
        CHECK(t_on >= 99u && t_on <= 101u, "on at tick %u (want 100)", t_on);
        CHECK(d >= 49u && d <= 51u, "held %u ticks (want 50)", d);
        CHECK(log_[0].note == 60u && log_[0].vel == 100u, "note 60 vel 100 (%u %u)", log_[0].note, log_[0].vel);
    }

    /* 3. QUANT 1/16 (24 ticks): on at 30 -> 24, off at 70 -> 64 (length kept); on at 13 -> 24 (forward) */
    start(); set[MV_SQ_QUANT] = MV_SQQ_16;
    run_ticks(30); note(0, 62, 40);
    CHECK(s.t[0].n == 2u && s.t[0].ev[0].tick == 24u && s.t[0].ev[0].vel, "1/16: on at %u (want 24)", s.t[0].ev[0].tick);
    CHECK(s.t[0].ev[1].tick >= 63u && s.t[0].ev[1].tick <= 65u && !s.t[0].ev[1].vel, "1/16: off at %u (want 64)", s.t[0].ev[1].tick);
    start(); set[MV_SQ_QUANT] = MV_SQQ_16;
    run_ticks(13); mv_seq_key(&s, set, 1, 64, 100);
    CHECK(s.t[1].ev[0].tick == 24u && (s.t[1].ev[0].note & MV_SEQ_SKIP), "1/16 forward: at 24, skipped this pass");
    nlog = 0; run_ticks(60);
    CHECK(count(1, 1, 0) == 0u, "written ahead: not played again this pass (the key sounded live)");
    mv_seq_key(&s, set, 1, 64, 0);
    nlog = 0; run(pass + T2S(100));             /* (past its off, at 84, in the next pass) */
    CHECK(count(1, 1, 0) == 1u && count(1, 0, 0) == 1u, "next pass: played once (%u)", nlog);
    CHECK(!(s.t[1].ev[0].note & MV_SEQ_SKIP), "the skip mark is cleared once passed");

    /* 4. triplet grids: 1/8T = 32 ticks, 1/16T = 16 */
    start(); set[MV_SQ_QUANT] = MV_SQQ_8T; run_ticks(50); mv_seq_key(&s, set, 0, 60, 100);
    CHECK(s.t[0].ev[0].tick == 64u, "1/8T: 50 -> %u (want 64)", s.t[0].ev[0].tick);
    start(); set[MV_SQ_QUANT] = MV_SQQ_16T; run_ticks(50); mv_seq_key(&s, set, 0, 60, 100);
    CHECK(s.t[0].ev[0].tick == 48u, "1/16T: 50 -> %u (want 48)", s.t[0].ev[0].tick);

    /* 5. near the loop's end, 1/16 rounds to bar 1 of the next pass: tick 0, skipped once, then every pass */
    start(); set[MV_SQ_QUANT] = MV_SQQ_16;
    run_ticks(len2 - 5u); mv_seq_key(&s, set, 0, 67, 100);
    CHECK(s.t[0].ev[0].tick == 0u && (s.t[0].ev[0].note & MV_SEQ_SKIP), "end -> tick 0 (%u), skipped", s.t[0].ev[0].tick);
    nlog = 0; run_ticks(40); mv_seq_key(&s, set, 0, 67, 0);
    CHECK(count(0, 1, 0) == 0u, "no double at the wrap");
    nlog = 0; run(pass + T2S(100));
    CHECK(count(0, 1, 0) == 1u && count(0, 0, 0) == 1u, "played the pass after (%u)", nlog);

    /* 6. a note held across the loop's end: on at len-48, off 96 ticks later (tick 48 of the next pass) */
    start(); set[MV_SQ_QUANT] = MV_SQQ_OFF;
    run_ticks(len2 - 48u); note(0, 55, 96);
    nlog = 0; run(pass + 4000u);
    CHECK(count(0, 1, 0) == 1u && count(0, 0, 0) == 1u, "across the end: on and off (%u)", nlog);
    if (nlog >= 2u) {
        uint32_t a = tick_at(log_[0].t) % len2, b = tick_at(log_[1].t) % len2;
        CHECK(log_[0].vel && !log_[1].vel, "the on, then its off in the next pass");
        CHECK(a >= len2 - 49u && a <= len2 - 47u && b >= 47u && b <= 49u, "on at %u (want %u), off at %u (want 48)", a, len2 - 48u, b);
    }

    /* 7. overdub: pass 1 writes one note, pass 2 another; pass 3 plays both, sorted */
    start(); set[MV_SQ_QUANT] = MV_SQQ_16;
    run_ticks(96); note(0, 60, 24);             /* (now at tick ~120) */
    run(pass - T2S(120) + T2S(48));             /* to tick 48 of pass 2 */
    note(0, 72, 24);
    CHECK(s.t[0].n == 4u, "overdub: 4 events (%u)", s.t[0].n);
    for (i = 1, j = 1; i < s.t[0].n; i++) j &= s.t[0].ev[i - 1u].tick <= s.t[0].ev[i].tick;
    CHECK(j, "sorted by tick");
    nlog = 0; run(pass);
    CHECK(count(0, 1, 0) == 2u && count(0, 0, 0) == 2u, "pass 3: both notes (%u)", nlog);

    /* 8. the cap: 200 short notes offered, 128 written, ons = offs, never more than MV_SEQ_NEV */
    start(); set[MV_SQ_QUANT] = MV_SQQ_OFF; set[MV_SQ_LENUP] = 8;   /* 8 bars: 3072 ticks */
    for (i = 0; i < 200u; i++) { note(0, 40u + i % 40u, 3); run_ticks(3); }
    {
        uint32_t on = 0, off = 0;
        for (i = 0; i < s.t[0].n; i++) { if (s.t[0].ev[i].vel) on++; else off++; }
        CHECK(s.t[0].n <= MV_SEQ_NEV && on == off && on == MV_SEQ_NEV / 2u, "cap: %u events, %u on, %u off", s.t[0].n, on, off);
    }
    mv_seq_key(&s, set, 0, 90, 100);            /* full: no room for a note-off, so the note-on is not written */
    CHECK(s.t[0].nrec == 0u, "full: a new key is not written");

    /* 9. STOP ends what the tracks started; a key held while recording gets its note-off */
    start(); set[MV_SQ_QUANT] = MV_SQQ_OFF;
    run_ticks(10); note(0, 60, 200);            /* (now at tick ~210) */
    run(pass - T2S(110));                       /* tick ~100 of pass 2: the note sounds */
    mv_seq_key(&s, set, 1, 48, 100);            /* LOWER: a key held while recording */
    nlog = 0;
    mv_seq_stop(&s, set, emit, 0);
    CHECK(count(0, 0, 0) == 1u && log_[0].note == 60u, "STOP: the sounding note ends (%u)", nlog);
    CHECK(s.t[1].nrec == 0u && s.t[1].n == 2u && !s.t[1].ev[1].vel, "STOP: the held key's note-off written");

    /* 10. MUTE: what sounds ends, nothing plays while muted, it plays again unmuted */
    start(); set[MV_SQ_QUANT] = MV_SQQ_OFF;
    run_ticks(10); note(0, 60, 300);            /* (now at tick ~310) */
    run(pass - T2S(210));                       /* tick ~100 of pass 2: sounding */
    CHECK(s.t[0].sound[60 >> 3] & (1u << (60 & 7)), "sounding before MUTE (tick %u)", s.t[0].tick);
    nlog = 0; set[MV_SQ_MUTEUP] = 1; run_ticks(2);
    CHECK(count(0, 0, 0) == 1u, "MUTE: the sounding note ends (%u)", nlog);
    nlog = 0; run(pass);
    CHECK(nlog == 0u, "muted: nothing (%u)", nlog);
    set[MV_SQ_MUTEUP] = 0; nlog = 0; run(pass + T2S(250));   /* (its off at 310 of the next pass) */
    CHECK(count(0, 1, 0) == 1u && count(0, 0, 0) == 1u, "unmuted: plays (%u)", nlog);

    /* 11. CLEAR: ends its notes and empties the track; disarm closes a held recording */
    start(); set[MV_SQ_QUANT] = MV_SQQ_OFF;
    run_ticks(10); note(0, 60, 300);
    run(pass - T2S(210));
    nlog = 0; mv_seq_clear(&s, 0, emit, 0);
    CHECK(s.t[0].n == 0u && count(0, 0, 0) == 1u, "CLEAR: empty, its note ended (%u)", nlog);
    mv_seq_key(&s, set, 0, 65, 100); run_ticks(20); mv_seq_disarm(&s, 0);
    CHECK(s.t[0].n == 2u && s.t[0].nrec == 0u && !s.t[0].ev[1].vel, "disarm: the held key's note-off written");

    /* 12. LENGTH per track: UPPER 1 bar, LOWER 2 bars; a note at tick 0 of each loops at its own period */
    start(); set[MV_SQ_QUANT] = MV_SQQ_OFF; set[MV_SQ_LENUP] = 1; set[MV_SQ_LENLO] = 2;
    run(64); mv_seq_key(&s, set, 0, 60, 100); mv_seq_key(&s, set, 1, 36, 100);
    run_ticks(10); mv_seq_key(&s, set, 0, 60, 0); mv_seq_key(&s, set, 1, 36, 0);
    nlog = 0; i = now; run(4u * pass);          /* 8 bars */
    CHECK(count(0, 1, i) == 8u, "UPPER 1 bar: 8 notes in 8 bars (%u)", count(0, 1, i));
    CHECK(count(1, 1, i) == 4u, "LOWER 2 bars: 4 notes in 8 bars (%u)", count(1, 1, i));
    run(pass / 2u + T2S(100));                  /* LOWER past bar 1 .. */
    set[MV_SQ_LENLO] = 1; run_ticks(2);         /* .. then shortened to 1 bar: back inside it */
    CHECK(s.t[1].len == bar && s.t[1].tick < bar, "LENGTH shortened: len %u tick %u", s.t[1].len, s.t[1].tick);

    /* 13. the MIDI clock lock: never more than a pulse (4 ticks) ahead; caught up when behind */
    start();
    run_lock(BEAT * 2u, 8u);                    /* 2 beats of time, but the clock says pulse 2 (tick 8) */
    CHECK(s.ticks <= 8u + 4u, "held at the clock: %u ticks (<= 12)", s.ticks);
    run_lock(16, 100u);                         /* the clock is at pulse 25: 64 ticks a call at most .. */
    CHECK(s.ticks >= 8u + 64u && s.ticks <= 12u + 64u + 1u, "catching up: %u ticks", s.ticks);
    run_lock(16, 100u);                         /* .. then there */
    CHECK(s.ticks >= 101u && s.ticks <= 105u, "caught up: %u ticks (>= 101)", s.ticks);

    /* 14. the timing: a note on beat 2 plays on beat 2 of the next pass, within a block (16 samples) */
    start(); set[MV_SQ_QUANT] = MV_SQQ_16;
    run_ticks(96); note(0, 60, 10);
    nlog = 0; run(pass + 4000u);
    if (nlog) {
        uint32_t want = pass + BEAT, got = log_[0].t;   /* pass 2, beat 2 */
        CHECK(got + 16u >= want && got <= want + 16u, "beat 2 of pass 2 at %u (want %u, within a block)", got, want);
    } else CHECK(0, "the note played");

    /* 15. TRANSPOSE (phase 2): ROOT from the first note-on; a key sets the offset (latched, +-24); a note-on plays
     *     at the offset now; a note sounding keeps its pitch and its note-off ends that pitch; CLEAR forgets it */
    start(); set[MV_SQ_QUANT] = MV_SQQ_OFF; set[MV_SQ_LENUP] = 1;   /* 1 bar */
    CHECK(!mv_seq_xpose_on(&s, set, 0), "empty track: no TRANSPOSE (no ROOT)");
    run_ticks(10); note(0, 48, 60);              /* C3 at tick 10, 60 ticks long */
    run_ticks(100); note(0, 55, 60);             /* G3 at tick ~170 */
    CHECK(s.t[0].root == 48u, "ROOT: the first note (%u)", s.t[0].root);
    set[MV_SQ_XPOSE] = MV_SQX_LO;
    CHECK(!mv_seq_xpose_on(&s, set, 0), "TRANSPOSE L: not on UPPER");
    set[MV_SQ_XPOSE] = MV_SQX_UP;
    CHECK(mv_seq_xpose_on(&s, set, 0) && !mv_seq_xpose_on(&s, set, 1), "TRANSPOSE U: on UPPER (LOWER empty: off)");
    mv_seq_transpose(&s, 0, 53);                /* F3: +5 */
    CHECK(s.t[0].xpose == 5, "offset +5 (%d)", s.t[0].xpose);
    nlog = 0; run(T2S(MV_SEQ_BAR) + 2000u);
    CHECK(nlog >= 4u && log_[0].note == 53u && log_[1].note == 53u && !log_[1].vel, "C3 plays as F3, and ends F3");
    CHECK(nlog >= 4u && log_[2].note == 60u, "G3 plays as C4 (%u)", nlog >= 3u ? log_[2].note : 0u);
    /* a change while a note sounds: it keeps its pitch, its note-off ends that pitch */
    while (s.t[0].tick != 20u) run(16);         /* to tick 20 of a pass: C3 (as F3) sounding */
    nlog = 0; mv_seq_transpose(&s, 0, 36);      /* -12 */
    run_ticks(80);                               /* past its note-off (tick 70) */
    CHECK(nlog == 1u && log_[0].note == 53u && !log_[0].vel, "its note-off ends F3 (the pitch it started at)");
    nlog = 0; run_ticks(150);
    CHECK(nlog >= 1u && log_[0].note == 43u && log_[0].vel, "the next note-on at -12: G3 -> G2 (%u)", nlog ? log_[0].note : 0u);
    mv_seq_transpose(&s, 0, 127); CHECK(s.t[0].xpose == MV_SEQ_XMAX, "held to +24 (%d)", s.t[0].xpose);
    mv_seq_transpose(&s, 0, 0);   CHECK(s.t[0].xpose == -MV_SEQ_XMAX, "held to -24 (%d)", s.t[0].xpose);
    i = s.ver; mv_seq_clear(&s, 0, emit, 0);
    CHECK(s.t[0].root == MV_SEQ_NOROOT && s.t[0].xpose == 0 && s.ver != i, "CLEAR: ROOT and offset forgotten, ver moved");
    i = s.ver; mv_seq_key(&s, set, 0, 60, 100);
    CHECK(s.ver != i && s.t[0].root == 60u, "a write moves ver; the new first note is ROOT");

    /* 16. a factory clip from flash (Kris 7 Nov): the same notes as from RAM, any length, more than MV_SEQ_NEV */
    {
        static uint8_t rom[4096];
        static uint32_t ref_t[512], ref_n[512], ref_v[512];
        uint32_t nb = 0, nev = 0, last = 0, vel = 100, nref, t, n, v, k, same;
        mv_seq_rd_t r;
        /* 11 bars, 200 notes (400 events: past the RAM track's 256), velocities 100 and 70 */
        for (k = 0; k < 200u; k++) {
            uint32_t on = k * 21u, off = on + 10u, vv = k % 7u ? 100u : 70u, ev2[2][3] = {{on, 40u + k % 30u, vv}, {off, 40u + k % 30u, 0}};
            for (j = 0; j < 2u; j++) {
                uint32_t d = (ev2[j][0] - last) << 1 | (ev2[j][2] && ev2[j][2] != vel);
                do { rom[nb++] = (uint8_t)((d & 127u) | (d > 127u ? 128u : 0u)); d >>= 7; } while (d);
                rom[nb++] = (uint8_t)(ev2[j][1] | (ev2[j][2] ? 128u : 0u));
                if (ev2[j][2] && ev2[j][2] != vel) { rom[nb++] = (uint8_t)ev2[j][2]; vel = ev2[j][2]; }
                last = ev2[j][0]; nev++;
            }
        }
        CHECK(nb < 1000u, "coded: %u bytes for %u events (about 2 a event)", nb, nev);
        mv_seq_rd_start(&r, rom, nb);
        for (k = 0, same = 1; mv_seq_rd_next(&r, &t, &n, &v); k++)
            same &= t == (k / 2u) * 21u + (k & 1u) * 10u && n == 40u + (k / 2u) % 30u && v == (k & 1u ? 0u : (k / 2u) % 7u ? 100u : 70u);
        CHECK(same && k == 400u, "the reader gives back every event (%u)", k);
        start(); set[MV_SQ_LENUP] = 11;
        mv_seq_load_rom(&s, 0, rom, nb, nev, 40, emit, 0);
        CHECK(s.t[0].rom && s.t[0].n == 0u && s.t[0].root == 40u, "loaded from flash: no RAM events, ROOT kept");
        run(16);                                /* (tick 0 played: the load came at the start) */
        nlog = 0; k = s.ticks + 11u * MV_SEQ_BAR;
        while (s.ticks < k) run(16);            /* exactly one pass, from tick 1 */
        nref = nlog < 512u ? nlog : 512u;
        for (k = 0; k < nref; k++) { ref_t[k] = tick_at(log_[k].t); ref_n[k] = log_[k].note; ref_v[k] = log_[k].vel; }
        CHECK(nref == 400u, "an 11-bar pass plays all 400 events (%u)", nref);
        /* pass 2 the same (the reader back at the start) */
        nlog = 0; k = s.ticks + 11u * MV_SEQ_BAR;
        while (s.ticks < k) run(16);
        for (k = 0, same = nlog == 400u; k < 400u && same; k++)
            same = log_[k].note == ref_n[k] && log_[k].vel == ref_v[k] && (tick_at(log_[k].t) - ref_t[k]) % (11u * MV_SEQ_BAR) <= 1u;
        CHECK(same, "pass 2 plays the same (%u events)", nlog);
        /* LENGTH shortened to 2 bars and back: the reader follows */
        set[MV_SQ_LENUP] = 2; nlog = 0; run(T2S(2u * MV_SEQ_BAR));
        CHECK(nlog > 60u && nlog < 80u, "2 bars of it: %u events (about 73)", nlog);
        /* REC over it: into RAM (the first 256 events) */
        set[MV_SQ_LENUP] = 11;
        mv_seq_unrom(&s, 0);
        CHECK(!s.t[0].rom && s.t[0].n == MV_SEQ_NEV, "into RAM: %u events", s.t[0].n);
        /* CLEAR leaves flash; a RAM LOAD does too */
        mv_seq_load_rom(&s, 1, rom, nb, nev, 40, emit, 0);
        mv_seq_clear(&s, 1, emit, 0);
        CHECK(!s.t[1].rom && !s.t[1].n, "CLEAR: off flash, empty");
        /* TRANSPOSE applies from flash too */
        mv_seq_load_rom(&s, 1, rom, nb, nev, 40, emit, 0);
        set[MV_SQ_LENLO] = 11; mv_seq_transpose(&s, 1, 45);
        nlog = 0; run(T2S(11u * MV_SEQ_BAR));   /* (a whole pass: the 40s come every 630 ticks) */
        for (k = 0, same = 0; k < nlog && k < 4096u; k++) same |= log_[k].part == 1u && log_[k].vel && log_[k].note == 45u;
        CHECK(same, "transposed +5 from flash (40 -> 45)");
    }

    printf("mv_seq_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
