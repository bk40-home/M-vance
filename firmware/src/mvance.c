/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance: a JP-8080 style synth for the M-VAVE FM-1 (JT-8000 DSP, ported to fixed point), built on Dinghy.
 * Stage 6 start: the M-vance voice engine v0 (mv_voice.c) plays the 248-byte JP patch; the parts not ported yet
 * are listed there. The patch is the INIT patch until SysEx / the panel can change it; the JP CCs edit it.
 * Dinghy's note: the smallest FM-1 firmware to build your own on. A sine wave on the keys, and that is all.
 * This is the file to change. Everything below it (platform.c) is done: updates, the USB rescue,
 * MIDI (USB, the TRS jack), USB audio, keys, knobs, LEDs, the screen, flash storage; the FM-1 and the SMK-37.
 *
 *   keys       play the engine (MV_NV voices); the notes go out over USB-MIDI too, on the MIDI channel
 *   OCT- OCT+  the octave (kept in flash); the other one pressed while the first is held: PART SELECT (mv_ui.h,
 *              Kris 6 Nov: OCT- LOWER, OCT+ UPPER, both BOTH; the first press's octave step is undone)
 *   panel      (mv_ui.c, Stage 7) the JP-8080's sections as pages: LFO EDIT ENV SEL FX GLO step through their
 *              section's pages (the button lit), HOME the home screen, ALGORITHM every page in turn; KNOB1..4 edit
 *              the page, SELECT single steps on the last one touched; edits go out as the JP CCs. GLOBAL: the MIDI
 *              channel (OMNI: in all, out 1; 1..16, kept in flash) and the voice limit (1..8, for the meter).
 *              PERFORM (GLO, Kris 3 Nov): a program is a performance (UPPER + LOWER patch, SINGLE / DUAL / SPLIT,
 *              mv_voice.h); touching SPLIT and pressing a key sets the split to that key; PANEL picks the part the
 *              pages and the CCs in edit
 *   HOME       (mv_home.c, Kris 3 Nov) the saw-fan header, the program (layered: both names), KNOB1..4 CUTOFF,
 *              RESO, DELAY send, REVERB, the voices, a scope of the output
 *   PRESETS    the programs, 1..64 (mv_prog.h: the factory set is docs/presets/ (a .mid or .syx) via tools/mid2bank.c); the
 *              number and name on the home screen, * when edited (SAVE lit: edits not written). MIDI program change
 *              in (on the channel) selects one, and a selection goes out as one
 *   SAVE       its page: TO, CURSOR / CHAR (the name), MODE WRITE / SWAP (reorder) / FACTORY; SAVE again does it,
 *              written to flash once nothing sounds
 *   ARP        a tap: the arpeggiator on / off (lit); held 0.5 s: its pages (mv_arp.h, Skiff's arp). TEMPO on GLOBAL;
 *              a MIDI clock, while it comes, sets the tempo instead, and START restarts the pattern
 *   SEQ        (Kris 6 Nov, mv_seq.h) its page: LENGTH (the selected track's), QUANT, MUTE L / U. PLAY: the
 *              transport (lit while running, a blink each beat). REC: a tap arms / disarms the selected track (lit
 *              while one is armed), held 1 s: CLEAR it (REC again: yes). MIDI START / CONTINUE start it from bar 1,
 *              held to the clock's pulses; STOP stops it. The settings are kept in flash, the notes are not (RAM).
 *              SEQ 2/2 (phase 2): the piano roll (mv_roll.h) and TRANSPOSE / ROOT: a transposing track's keys set its
 *              offset (key - ROOT) instead of playing. SEQ 3/3 (phase 3): the clips (mv_clip.h): SAVE does LOAD / SAVE;
 *              a SAVE is written to flash at the next STOP, once nothing sounds
 *   MV_PROFILE build (DINGHY_DEFS=-DMV_PROFILE=1 ./build.sh): DEBUG adds where the time goes, OSC / FLT / ETC
 *              as % of the buffer (average); off (the default), none of it is compiled in
 *   DEBUG      GLO's last page (mv_debug.h): render time of a 5.8 ms buffer, average of the last 1/4 s (us, %) and
 *              the peak held over the last 3 s (%, orange past 90 %), voices sounding / the limit, octave, MIDI
 *              channel, where notes came from, an incoming clock, the firmware version. No footer elsewhere
 *   MASTER     the volume (platform.c)
 *   MIDI in    notes from USB or the TRS jack play the engine; pitch bend (BEND UP / DOWN of the patch), CC1 mod
 *              wheel (brings LFO2 in), the JP MODE2 CCs edit the patch; a MIDI clock shows its tempo and blinks
 *              DEBUG; START / STOP run the sequencer (PLAY shows the sequencer now: Kris 6 Nov, 7a)
 *   SMK-37     the keys, the outer keys too, the pads (white keys F3..G5, lit while they sound), the pitch wheel
 *              bends by the patch's BEND UP / DOWN
 *
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division.
 * tests/host.c runs this file on a computer: the screen as a PNG, the sound as a WAV.
 * What platform.c offers: docs/API.md. The chip's rules: docs/RULES.md. */
#define APP_NAME "M-VANCE"                     /* the rescue and crash screens (platform.c) */
#define STORE_MAGIC 0x434E564Du               /* "MVNC": our settings never read another fork's (store.h) */
static void app_idle(void);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#include <stdint.h>
static void app_sysex(const uint8_t *p, uint32_t n);
#define APP_SYSEX app_sysex                     /* a Roland SysEx from USB: the program transfer (mv_xfer.h) */
#include "platform.c"
#include "mv_patch.h"                          /* the JP-8080 patch: table, CC map, SysEx (Stage 4) */
#include "mv_voice.h"                          /* the voice engine; its code is mv_dsp.c, built apart at -O2 */
#include "mv_prog.c"                           /* the programs: in this unit, it saves with platform.c (static) */
#include "mv_xfer.c"                           /* the program transfer over SysEx (Kris 4 Nov: F1 + F2) */
#include "mv_clip.c"                           /* the sequencer's clips (Kris 6 Nov, phase 3): saves with platform.c */
#include "mv_ui.c"                             /* the panel pages: in this unit, it draws with gfx.c (static) */
#include "mv_home.h"                           /* the home screen (its code: after the colours, below) */
#include "mv_debug.h"                          /* the DEBUG page (its code: after the colours, below) */
#include "mv_roll.h"                           /* the piano roll, SEQ 2/2 (its code: after the colours, below) */
#if BOARD_SMK37
#include "smk_wheel.h"
#endif

/* ---- the sound: the M-vance engine (mv_voice.c). The ISR renders; the main loop only queues notes. */
static mv_engine_t eng;

/* render-time meter: the ISR times each buffer with the 24 MHz counter and keeps a sum, a count and the peak;
 * the main loop reads and clears them (the clear is a request the ISR carries out, so only the ISR writes) */
static volatile uint32_t cpu_sum, cpu_n, cpu_peak, cpu_clear;

/* the home scope's ring (mv_home.h): the ISR writes (L+R)/2 of every 2nd frame after the render, the main loop
 * reads a snapshot; scope_on (the main loop sets it while HOME shows) keeps the ISR from doing it otherwise */
static int16_t scope_ring[MV_SCOPE_N];
static volatile uint32_t scope_w, scope_on;

#if MV_PROFILE
uint32_t mv_prof_ticks(void) { return fm1_ticks(); }   /* the engine's stage timer (mv_voice.c, MV_PROFILE only) */
#endif

/* ---- MIDI clock out (Kris 7 Nov, DAW use, 3a): 24 F8 a quarter note while CLK OUT is on and the FM-1 runs on its own
 * tempo, counted on arp_beat (a quarter in samples: the arp's and the LFO sync's own clock), so the computer and the
 * arp stay locked. Per buffer: the frame each F8 falls on goes to midi_rt_at (platform.c), which sends it at that
 * moment. PLAY: the F8s still waiting are dropped, and START then beat 1's F8 go at this buffer's frame 0, in order. mco_acc counts frames x 24 since the last F8; one falls every arp_beat of it (exact: no rounding drift).
 * Cost: a compare per buffer while off; a divide per buffer while on. */
static volatile uint32_t mco_live;              /* main loop: CLK OUT on and no clock coming in (or CLOCK INT) */
static volatile uint32_t mco_sync;              /* main loop: PLAY: START and beat 1 at the next buffer's start */
static uint32_t mco_acc;                        /* ISR only */
static void mco_block(uint32_t n)               /* audio ISR, once a buffer of n frames */
{
    uint32_t beat = eng.arp_beat, f = 0, need;
    if (!mco_live || !beat) { mco_acc = 0; return; }   /* off (switched on: the first F8 one tick later) */
    if (mco_sync) {                             /* PLAY: the waiting F8s out of the way, START, beat 1's F8 at frame 0 */
        mco_sync = 0;
        midi_rt_flush();
        midi_rt_at(0, 0xFAu);
        mco_acc = beat;
    }
    for (;;) {                                  /* (at most one F8 a buffer at 240 BPM: the loop ends at once) */
        need = mco_acc >= beat ? 0u : (beat - mco_acc + 23u) / 24u;   /* frames to the next F8 */
        if (f + need >= n) { mco_acc += 24u * (n - f); return; }
        f += need;
        midi_rt_at(f, 0xF8u);
        mco_acc = mco_acc + 24u * need - beat;  /* (0..23: the part of a frame past the F8) */
    }
}

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t t0 = fm1_ticks(), dt;
    mco_block(n);                               /* the MIDI clock out (nothing while CLK OUT is off) */
    mv_engine_render(&eng, out, n);
    if (scope_on) {                             /* the scope: 1 store per 2 frames (n is even: 256) */
        uint32_t i, w = scope_w;
        for (i = 0; i + 1u < n; i += 2u) {
            int32_t m = (out[2u * i] + out[2u * i + 1u]) >> 1;
            scope_ring[w++ & (MV_SCOPE_N - 1u)] = (int16_t)(m > 32767 ? 32767 : m < -32768 ? -32768 : m);
        }
        scope_w = w & (MV_SCOPE_N - 1u);
    }
    dt = fm1_ticks() - t0;                      /* (wraps safely: unsigned subtraction) */
    if (cpu_clear) {
        cpu_sum = 0; cpu_n = 0; cpu_peak = 0; cpu_clear = 0;
#if MV_PROFILE
        eng.prof_osc = 0; eng.prof_flt = 0;
#endif
    }
    cpu_sum += dt;
    cpu_n++;
    if (dt > cpu_peak) cpu_peak = dt;
}

/* ---- notes (main loop) */

static int quiet(void) { return mv_engine_quiet(&eng); }   /* nothing sounds, not even a release */

/* ---- the screen: the home screen (mv_home.c), the DEBUG page (mv_debug.c) */
#define C_ACC RGB(255, 128, 0)                  /* M-vance orange (as the JT-8000 controller's accent) */
#define C_KEY RGB(210, 210, 210)
#define C_OFF RGB(90, 90, 90)
#define KEY0 53u                                /* key 0 = F3 */
#include "mv_home.c"                           /* the home screen (Kris 3 Nov, H1): after the colours it draws with */

/* the buttons that light for the page shown (the section buttons, HOME and SEQ) */
static const uint8_t UI_LED_BTN[8] = {BTN_HOME, BTN_LFO, BTN_EDIT, BTN_ENV, BTN_SEL, BTN_FX, BTN_GLO, BTN_SEQ};

/* the buffer's length in us: 256 frames at 44.1 kHz = 5.8 ms (the CPU figures are % of it) */
#define BUF_US (HALF_FRAMES * 1000000u / FS)
#include "mv_debug.c"                          /* the DEBUG page (GLO): the meter and the status, off the other screens */
#include "mv_roll.c"                           /* the piano roll (SEQ 2/2, Kris 6 Nov: sequencer phase 2) */
static mv_roll_t roll;                          /* what it shows (main loop only) */

/* ---- MIDI in: from the main loop, and from app_idle while the screen draws (idle_ok) */
/* what survives a power-off (up to SECTOR_MAX bytes): the octave, the MIDI channel, the reverb's and the arp's settings
 * and the tempo (global: the JP patch has neither; Kris 2 Nov R3a, 1 Nov 6a). A load needs the size to match, so a new
 * field resets everything once: SPARE bytes are kept for the next ones (take one from it, keep the size). This
 * layout (34 bytes) is new with the arp: the first boot after it starts from the defaults, once */
#define SPARE 14u
static struct { uint8_t oct, ch, rv[MV_RV_N], arp[MV_ARP_N], tempo, prog, bank[4], seq[MV_SQ_N], seqf, midi,
                spare[SPARE - 6u - MV_SQ_N]; } set;
/* midi: one more spare (Kris 7 Nov, DAW use, 6a): the MIDI page's MV_MIDI_* bits (mv_ui.h). A record from before has 0
 * there: LOCAL ON, CLOCK AUTO, CLK OUT OFF, which is how it behaved */
/* prog: one of the spares (34 bytes still: the record loads as before). bank: 4 more (A1, Kris 3 Nov), the factory
 * bank the saved programs belong to (mv_prog_factory_id, little-endian; 0 = not recorded yet). seq: 5 more (Kris
 * 6 Nov, 6a), the sequencer's settings (MV_SQ_*): a record from before has 0 there, which no LENGTH is, so it gets
 * the defaults. seqf: one more (Kris 7 Nov, 2a): 1 = LENGTH in bars (1..32); 0, a record from before, had the
 * codes 1..4 for 1 / 2 / 4 / 8 bars, converted once at boot (and saved: converted twice would be wrong) */
#define SEQF_BARS 1u
static mv_prog_t prog;                          /* the 64 programs (RAM; flash blobs 0..3) */
static mv_clip_t clips;                         /* the sequencer's 12 clips: their directory, a SAVE waiting (slot 1) */
static uint32_t clip_req;                       /* a CLIP LOAD / SAVE to do (MV_CL_* + 1; 0 none): kept until it is
                                                   taken (a LOAD waits for the ISR, a SAVE for a steady copy) */
static uint32_t idle_ok, midi_src, clk_n, clk_t0, clk_bpm, clk_run, clk_beat_ms;
static uint32_t clk_last, clk_seen;            /* fm1_ms of the last F8 in, and one has come: CLK OUT stays quiet
                                                   while a clock arrives (CLOCK AUTO), so it never echoes one back */
/* The MIDI clock's tempo, averaged over its last 4 quarter notes (Kris 5 Nov, fix 2). One quarter is timed on the
 * 1 ms tick, so it reads e.g. 461, 462, 461 .. ms at 130 BPM: taken alone, the arp's beat and every synced LFO1 step
 * changed on almost every beat (and mv_engine_sync redid its 64-bit divide each time). The sum of 4 has 0.25 ms
 * resolution and stays put while the clock is steady; a new tempo is fully followed after 4 beats.
 * clk_qwin: the last 4 quarters (ms), a ring at clk_qi; clk_qn: how many are in it (0..4); clk_qsum: their sum. */
static uint32_t clk_qwin[4], clk_qi, clk_qn, clk_qsum;

static mv_ui_t ui;                              /* the panel pages (mv_ui.c) */
static uint32_t prog_req = 0xFF;                /* a MIDI program change for the main loop (0xFF none) */

/* ---- the FM-1's own keys (Kris 7 Nov, 1a / 2a): they follow PART SELECT / PANEL in a layered program, and send on
 * the channel of the part they play. UPPER: every key plays UPPER only; LOWER: LOWER only; BOTH: the mode decides as
 * before (DUAL both, SPLIT below the split LOWER); SINGLE: UPPER. A part's channel is its CH UP / CH LO, or the
 * global one where that is GLOBAL (0); MIDI in already listens the same way. A key remembers the channels its
 * note-on went out on, so its note-off goes there even if the part, the mode or a channel moved meanwhile. */
static uint32_t key_parts(uint32_t note)
{
    uint32_t mode = eng.perf[MV_PF_MODE];
    if (mode == MV_KEY_SINGLE || ui.panel == MV_UI_UPPER) return MV_TO_UP;
    if (ui.panel == MV_UI_LOWER) return MV_TO_LO;
    if (mode == MV_KEY_DUAL) return MV_TO_UP | MV_TO_LO;
    return note < eng.perf[MV_PF_SPLIT] ? MV_TO_LO : MV_TO_UP;   /* SPLIT, BOTH: the split key and up is UPPER's */
}
static uint32_t part_ch(uint32_t part)          /* 0..15: a part's MIDI channel out */
{
    uint32_t c = eng.perf[part == MV_PART_LO ? MV_PF_CHLO : MV_PF_CHUP];
    return c ? c - 1u : set.ch ? set.ch - 1u : 0u;   /* (GLOBAL: the global channel; OMNI sends on 1) */
}
/* LOCAL (Kris 7 Nov, DAW use, 1a): with LOCAL OFF the keys only go to MIDI out (the computer sends them back on MIDI in,
 * where they play, arpeggiate and record as any MIDI note). A key remembers whether it played here (KEY_LOCAL beside
 * its channels), so turning LOCAL over while it is held neither leaves a note hanging nor cuts an echoed one short */
#define KEY_LOCAL (1u << 16)
/* a key down: played on its part(s) (LOCAL ON), sent on their channel(s); returned: the channels (bit per channel) and
 * KEY_LOCAL if it sounds here; 0 nothing */
static uint32_t key_on(uint32_t note, uint32_t vel)
{
    uint32_t to, chm = 0, c;
    if (note > 127u || !vel) return 0;
    to = key_parts(note);
    if (!(ui.midi & MV_MIDI_LOCALOFF)) {        /* LOCAL ON: the sound plays the key */
        mv_engine_note_direct(&eng, note, vel, to);   /* (the JP has no fixed velocity -> amp: it is a morph target) */
        chm = KEY_LOCAL;
    }
    if (to & MV_TO_UP) chm |= 1u << part_ch(MV_PART_UP);
    if (to & MV_TO_LO) chm |= 1u << part_ch(MV_PART_LO);   /* (one channel for both: one note) */
    for (c = 0; c < 16u; c++) if (chm >> c & 1u) midi_send(0x90u | c, note, vel);
    return chm;
}
static void key_off(uint32_t note, uint32_t chm)   /* a key up: its note ends, on the channels its note-on used */
{
    uint32_t c;
    if (chm & KEY_LOCAL)                        /* it sounded here (LOCAL ON when it went down) */
        mv_engine_note_off(&eng, note);         /* (a key-up reaches every part: harmless where it did not play) */
    for (c = 0; c < 16u; c++) if (chm >> c & 1u) midi_send(0x80u | c, note, 0);
}
static uint32_t sel_dirty;                      /* a program was selected: the settings (its number) to save */

/* a program into the engine: its two patches and its performance bytes (the ISR reads them live; a new cut of the
 * voices, if the mode or VOICES moved, follows at the next buffer) */
static void prog_to_engine(uint32_t n)
{
    const uint8_t *pf = mv_prog_perf(&prog, n);
    uint32_t i;
    eng.patch = *mv_prog_get(&prog, n);
    eng.lower = *mv_prog_lower(&prog, n);
    for (i = 0; i < MV_PF_N; i++) eng.perf[i] = pf[i];
}

/* the factory bank question (A1): 1 while it shows (the panel waits; the keys play), and the bank to record */
static int bank_ask;

/* the program transfer (mv_xfer.h, Kris 4 Nov): the computer sends programs into slots, auditions one in the temporary
 * area, or asks for a dump. xfer_t: the last message either way (0 none yet); while one came within XFER_HOLD ms the
 * screen shows the transfer and nothing goes to flash (an erase holds the CPU ~50 ms: the next messages would be
 * lost, USB holds only one) */
#define XFER_HOLD 1500u
static mv_xfer_t xfer = {0, 0, 0xFF, 0, 0, 0, 0};
static uint32_t xfer_t, xfer_slots;             /* xfer_slots: a slot (or a dump) is in this transfer: the screen */
static int xfer_busy(void) { return xfer_t && fm1_ms - xfer_t < XFER_HOLD; }
static void prog_to_engine(uint32_t n);
static void app_sysex(const uint8_t *p, uint32_t n)   /* platform_poll: main loop */
{
    uint32_t r = mv_xfer_rx(&xfer, &prog, &eng, p, n);
    if (!(r & (MV_XF_SLOT | MV_XF_TEMP | MV_XF_REQ))) return;   /* (not ours, or refused: nothing changed) */
    if (!xfer_busy()) {                         /* a new transfer: its counts from here */
        xfer.rx_n = (uint16_t)(r & MV_XF_REQ ? 0u : 1u);
        if (!(r & MV_XF_REQ)) xfer.tx_n = 0;
        xfer_slots = 0;
    }
    xfer_t = fm1_ms;
    if (r & (MV_XF_SLOT | MV_XF_REQ)) xfer_slots = 1;   /* (an audition alone keeps the screen as it is) */
    if ((r & MV_XF_SLOT) && xfer.last_slot == ui.cur) prog_to_engine(ui.cur);   /* the program playing: heard now */
}
static uint32_t seqf_dirty;                     /* the settings (LENGTH converted to bars, 7 Nov) to save */
static uint32_t bank_dirty;                     /* the settings (the bank's id) to save: the main loop's dirty */
static void bank_record(uint32_t id)
{
    set.bank[0] = (uint8_t)id; set.bank[1] = (uint8_t)(id >> 8);
    set.bank[2] = (uint8_t)(id >> 16); set.bank[3] = (uint8_t)(id >> 24);
    bank_dirty = 1;
}

/* the program n plays: into the engine (the ISR reads the bytes live), its number kept, sent on MIDI out if asked */
static void prog_select(uint32_t n, int send)
{
    n %= MV_NPROG;
    prog_to_engine(n);                          /* (edits to the one before are dropped, as on the JP) */
    ui.cur = (uint8_t)n; set.prog = (uint8_t)n; sel_dirty = 1;
    if (send) midi_send(0xC0u | (set.ch ? set.ch - 1u : 0u), n, 0);
}

static void midi_in(void)
{
    uint8_t st, d1, d2;
    int src;
    while ((src = midi_get(&st, &d1, &d2)) != 0) {   /* 1 USB, 2 the TRS jack */
        uint32_t ty = st & 0xF0u;
        if (st >= 0xF8u && st <= 0xFCu && st != 0xF9u && (ui.midi & MV_MIDI_CLKINT))
            continue;                           /* CLOCK INT (Kris 7 Nov, 2a): clock and START / CONT / STOP ignored */
        if (st == 0xF8u) {                      /* clock: 24 a quarter note */
            clk_last = fm1_ms; clk_seen = 1;
            if (clk_n % 24u == 0u) {
                if (clk_n) {
                    uint32_t ms = fm1_ms - clk_t0;   /* a quarter's length */
                    if (clk_qn == 4u) clk_qsum -= clk_qwin[clk_qi];   /* full: the oldest quarter drops out */
                    else clk_qn++;
                    clk_qwin[clk_qi] = ms;
                    clk_qsum += ms;
                    clk_qi = (clk_qi + 1u) & 3u;
                    /* BPM over the window (rounded): what DEBUG shows, and nonzero says the clock is coming */
                    clk_bpm = clk_qsum ? (60000u * clk_qn + clk_qsum / 2u) / clk_qsum : 0;
                }
                clk_t0 = fm1_ms;
                clk_beat_ms = fm1_ms;
            }
            clk_n++;
            if (eng.seq_ext) eng.seq_pulse++;   /* the sequencer's lock: pulses since START */
            continue;
        }
        if (st == 0xFAu || st == 0xFBu) {       /* START: beat 1, the arp's step 1; both: the sequencer from bar 1,
                                                   held to the pulses (no song position: CONTINUE as START) */
            clk_run = 1;
            if (st == 0xFAu) { clk_n = 0; eng.arp_start = 1; }
            eng.seq_pulse = 0; eng.seq_ext = 1; eng.seq_run = 1; eng.seq_restart = 1;   /* (the count first) */
            continue;
        }
        if (st == 0xFCu) { clk_run = 0; eng.seq_run = 0; eng.seq_arm = 0; eng.seq_ext = 0; continue; }   /* STOP */
        {   /* the channel gate (the JT's PerfRouter): a part listens on its own channel (CH LO / CH UP), or on the
               global one when that is 0 (OMNI: all); in SINGLE only UPPER listens. Notes then go where the mode and
               the split say, within the parts that heard them */
            uint32_t ch = st & 15u, to = 0, cu = eng.perf[MV_PF_CHUP] ? eng.perf[MV_PF_CHUP] : set.ch;
            uint32_t cl = eng.perf[MV_PF_CHLO] ? eng.perf[MV_PF_CHLO] : set.ch;
            if (!cu || cu - 1u == ch) to |= MV_TO_UP;
            if (eng.perf[MV_PF_MODE] != MV_KEY_SINGLE && (!cl || cl - 1u == ch)) to |= MV_TO_LO;
            if (!to) continue;                  /* not our channel */
            if (ty == 0x90u && d2) { if (d1 < 128u) mv_engine_note_to(&eng, d1, d2, to); midi_src = (uint32_t)src; continue; }
            if (ty == 0x80u || ty == 0x90u) { mv_engine_note_to(&eng, d1, 0, to); continue; }
        }
        if (ty == 0xE0u) eng.bend14 = (int32_t)((uint32_t)d2 << 7 | d1) - 8192;   /* pitch bend */
        else if (ty == 0xB0u && d1 == 1u) eng.wheel = d2;                            /* mod wheel: LFO2 in */
        else if (ty == 0xB0u) {                 /* the JP MODE2 CCs: into the part(s) PANEL edits (not by channel, as the
                                                   JT: one keyboard on one channel can still edit either part) */
            if (eng.perf[MV_PF_MODE] == MV_KEY_SINGLE || ui.panel != MV_UI_LOWER) mv_cc_apply(&eng.patch, d1, d2);
            if (eng.perf[MV_PF_MODE] != MV_KEY_SINGLE && ui.panel != MV_UI_UPPER) mv_cc_apply(&eng.lower, d1, d2);
        }
        else if (ty == 0xC0u) prog_req = d1 % MV_NPROG;   /* program change: the main loop selects it (0 = 1) */
    }
}
static void app_idle(void)
{
    if (idle_ok) midi_in();
}

static void app_main(void)
{
    uint32_t prev = 0, k, t_change = 0, dirty = 0, r_oct = 0;
    uint32_t cpu_t = 0, cpu_slot[12] = {0}, cpu_i = 0;
    int dbg_all = 0;                            /* the DEBUG page was just entered: all its rows drawn */
    int xfer_shown = 0;                         /* the transfer screen is up (mv_home.c home_xfer) */
    uint32_t xfer_key = 0;                      /* what it shows */
    uint32_t prog_drawn = ~0u, r_prog = 0;      /* the home screen's program line; a SAVE to do */
    uint32_t knobs_drawn = ~0u, voices_drawn = ~0u, scope_t = 0;   /* the home screen's parts: what they show */
    int scope_live = 1;                         /* the scope showed a signal: refresh it (else drawn flat once) */
    uint8_t held[27] = {0};                     /* the note each key plays */
    uint32_t held_ch[27] = {0};                 /* and the channels it went out on, KEY_LOCAL (key_on) */
#if BOARD_SMK37
    static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* white key -> key */
    uint8_t pad_note[16] = {0}, ext_note[FM1_NKEY] = {0};
    uint32_t pad_ch[16] = {0}, ext_ch[FM1_NKEY] = {0};   /* (their channels out, as held_ch) */
    smk_pw_t wheel = {0};
    smk_pad_events = 1;                         /* the pads as events (smk_pad_take): they play here, lit while they sound */
#endif
    mv_engine_init(&eng);                       /* (before the ISR renders a note: nothing is queued yet) */
    {
        int bad = settings_load(&set, sizeof set) || (int8_t)set.oct < -2 || (int8_t)set.oct > 2 || set.ch > 16u;
        if (bad) set.oct = 0, set.ch = 0;
        for (k = 0; k < MV_RV_N; k++) {         /* the reverb's settings to the engine: defaults (off) when the load
                                                   failed, each held in range */
            if (bad || set.rv[k] > (k == MV_RV_FREEZE ? 1u : 127u)) set.rv[k] = 0;
            eng.rvb[k] = set.rv[k];
        }
        for (k = 0; k < MV_ARP_N; k++) {        /* the arp's: its defaults (off, UP, 1/16 ..) likewise */
            if (bad || set.arp[k] < MV_ARP_MIN[k] || set.arp[k] > MV_ARP_MAX[k]) set.arp[k] = MV_ARP_DEF[k];
            eng.arpb[k] = set.arp[k];
        }
        if (bad || set.tempo < 40u || set.tempo > 240u) set.tempo = 120;
        eng.tempo = set.tempo;
        set.midi = bad ? 0u : (uint8_t)(set.midi & MV_MIDI_MASK);   /* the MIDI page (unknown bits dropped) */
        if (!bad && set.seqf != SEQF_BARS && set.seq[MV_SQ_LENUP]) {   /* LENGTH codes 1..4 -> 1 / 2 / 4 / 8 bars */
            for (k = MV_SQ_LENUP; k <= MV_SQ_LENLO; k++)
                if (set.seq[k] >= 1u && set.seq[k] <= 4u) set.seq[k] = (uint8_t)(1u << (set.seq[k] - 1u));
            seqf_dirty = 1;                     /* (saved by the main loop, as the bank's id) */
        }
        if (set.seqf != SEQF_BARS) { set.seqf = SEQF_BARS; seqf_dirty = 1; }
        for (k = 0; k < MV_SQ_N; k++) {         /* the sequencer's: its defaults (2 bars, 1/16) for a record from
                                                   before it (no LENGTH there: 0) or out of range */
            if (bad || !set.seq[MV_SQ_LENUP] || set.seq[k] < MV_SQ_MIN[k] || set.seq[k] > MV_SQ_MAX[k]) set.seq[k] = MV_SQ_DEF[k];
            eng.seqb[k] = set.seq[k];
        }
    }

    mv_prog_load(&prog);                        /* the programs: flash, or the factory set where none is saved */
    {   /* the factory bank (A1, Kris 3 Nov): the programs saved belong to the bank of the firmware that saved them.
         * A firmware with another bank asks (SAVE: load it, HOME: keep them); with nothing saved the new bank plays
         * already and is just recorded. Once a boot: a 32 KB hash */
        uint32_t id = mv_prog_factory_id(), was = set.bank[0] | (uint32_t)set.bank[1] << 8 |
                                                  (uint32_t)set.bank[2] << 16 | (uint32_t)set.bank[3] << 24;
        int saved = mv_prog_saved(&prog);
#ifdef DINGHY_HOST
        if (getenv("MV_HOST_BANK_ASK")) { was = id ^ 1u; saved = 1; }   /* host only: as after a flash with a new bank */
#endif
        if (id != was) {
            if (saved) bank_ask = 1;            /* asked on the screen; the main loop takes the answer */
            else bank_record(id);
        }
    }
    if (set.prog >= MV_NPROG) set.prog = 0;
    prog_to_engine(set.prog);                   /* the program last played (the ISR reads the bytes live) */
    mv_ui_init(&ui, &prog);
    ui.midi = set.midi;                         /* the MIDI page's settings (loaded above) */
    mv_clip_init(&clips);                       /* the clips' directory (flash slot 1: 4 sector reads) */
    mv_ui_set_clips(&ui, &clips);
    mv_roll_init(&roll);
    ui.cur = set.prog;
#ifdef DINGHY_HOST
    {   /* host only: MV_DUMP_PAGES=file writes the panel's pages as JSON and stops (tools/gen_cheatsheet.py) */
        const char *dump = getenv("MV_DUMP_PAGES");
        if (dump) { mv_ui_dump(dump, &eng); exit(0); }
    }
#endif
    palette_set(1);                             /* the family's AMBER (gfx.c): the panel's labels, values, rules */
    lcd_fill(0, 0, 240, 240, C_BLACK);
    home_header();

    for (;;) {
        uint32_t keys, down, up, rel, b, lit = 0, out_ch = set.ch ? set.ch - 1u : 0u, st;
        int32_t oct = (int8_t)set.oct, e;
        uint32_t base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        platform_poll();

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {                 /* (a key stops the note it started: the octave may change meanwhile) */
#if BOARD_SMK37
            uint32_t vel = fm1_in.key_vel[SMK_KEY_F3 + k];   /* the SMK's keys feel the velocity */
#else
            uint32_t vel = 100;
#endif
            if ((down >> k & 1u) && mv_ui_split_learn(&ui)) {   /* PERFORM's SPLIT just touched: the key IS the split */
                eng.perf[MV_PF_SPLIT] = (uint8_t)(base + k);
                mv_ui_learned(&ui);                 /* (one key: the next ones play) */
                held[k] = 0xFF;                     /* (it plays nothing: its key up sends nothing) */
                continue;
            }
            if (down >> k & 1u) { held[k] = (uint8_t)(base + k); held_ch[k] = key_on(held[k], vel); }
            if ((up >> k & 1u) && held[k] < 128u) key_off(held[k], held_ch[k]);
        }
#if BOARD_SMK37
        while ((e = smk_extkey_take()) >= 0) {  /* the keys left and right of the FM-1's 27: key << 8 | velocity */
            uint32_t key = (uint32_t)e >> 8, vel = (uint32_t)e & 0x7Fu;
            if (key >= FM1_NKEY) continue;
            if (vel) { ext_note[key] = (uint8_t)(base + key - SMK_KEY_F3); ext_ch[key] = key_on(ext_note[key], vel); }
            else if (ext_note[key]) { key_off(ext_note[key], ext_ch[key]); ext_note[key] = 0; }
        }
        while ((e = smk_pad_take()) >= 0) {     /* a pad: hardware pad << 8 | velocity (0 = let go) */
            uint32_t pad = ((uint32_t)e >> 8) & 15u, vel = (uint32_t)e & 0x7Fu;
            uint32_t w = pad < 8u ? pad + 8u : pad - 8u;   /* bottom row = white keys 1-8, top row 9-16 */
            if (vel) { pad_note[pad] = (uint8_t)(base + WHITE[w]); pad_ch[pad] = key_on(pad_note[pad], vel); }
            else if (pad_note[pad]) { key_off(pad_note[pad], pad_ch[pad]); pad_note[pad] = 0; }
        }
        {
            int32_t bd = smk_pw_bend(&wheel, fm1_in.wheel[0], fm1_ms);   /* 1/4096 semitone over +-2 st */
            eng.bend14 = mv_clamp(bd, -8192, 8191); /* +-2 st = +-8192: the MIDI scale; the patch sets the range */
        }
#endif

        b = fm1_input_edges(&rel);              /* buttons pressed since the last call (bit = button id) */
        r_oct = 0;
        if (bank_ask) {                         /* (the bank question: the octave only, as before) */
            if (b & (1u << BTN_OCT_DOWN) && oct > -2) { set.oct = (uint8_t)(oct - 1); dirty = 1; t_change = fm1_ms; }
            if (b & (1u << BTN_OCT_UP) && oct < 2) { set.oct = (uint8_t)(oct + 1); dirty = 1; t_change = fm1_ms; }
        } else {                                /* the octave, and PART SELECT on the two together (mv_ui.h) */
            r_oct = mv_ui_oct(&ui, &eng, b, rel, &set.oct);
            if (r_oct & MV_UI_SETTING) { dirty = 1; t_change = fm1_ms; }
        }
        if (bank_ask) {                         /* the factory bank question: SAVE loads it, HOME keeps the saved;
                                                   the panel waits (its turns dropped, not kept for later) */
            static const uint8_t ENCS[7] = {ENC_SELECT, ENC_ALGORITHM, ENC_PRESETS, ENC_KNOB1, ENC_KNOB2, ENC_KNOB3,
                                            ENC_KNOB4};   /* (by name: the SMK-37 numbers them otherwise) */
            for (k = 0; k < 7u; k++) (void)fm1_enc_take(ENCS[k]);
            if (b & (1u << BTN_SAVE | 1u << BTN_HOME)) {
                if (b >> BTN_SAVE & 1u) {       /* all 64 now; to flash record by record once nothing sounds */
                    mv_prog_factory_all(&prog);
                    prog_to_engine(ui.cur);
                }
                bank_record(mv_prog_factory_id());
                bank_ask = 0;
                lcd_fill(0, 0, 240, 240, C_BLACK);   /* home again, all of it */
                home_header();
                prog_drawn = ~0u; knobs_drawn = ~0u; voices_drawn = ~0u; scope_live = 1;
            }
            b = 0;                              /* (nothing else reaches the panel this pass) */
        }
        if (bank_dirty || seqf_dirty) { bank_dirty = 0; seqf_dirty = 0; dirty = 1; t_change = fm1_ms; }   /* the
                                                   bank's id, LENGTH's conversion: kept in flash */
        if (!bank_ask) {
            uint32_t r = mv_ui_input(&ui, &eng, b, rel, &set.ch, out_ch) | (r_oct & MV_UI_PAGE);   /* the panel */
            if (r & MV_UI_PROG) r_prog = 1;     /* (done below, with the programs) */
            if (r & MV_UI_CLIP) { clip_req = ui.cl_act; ui.cl_act = 0; }   /* (done below) */
            if (r & MV_UI_SETTING) {            /* GLOBAL MIDI CH / TEMPO, REVERB, ARP: kept in flash */
                for (k = 0; k < MV_RV_N; k++) set.rv[k] = eng.rvb[k];
                for (k = 0; k < MV_ARP_N; k++) set.arp[k] = eng.arpb[k];
                set.tempo = eng.tempo;
                for (k = 0; k < MV_SQ_N; k++) set.seq[k] = eng.seqb[k];
                set.midi = ui.midi;             /* MIDI: LOCAL, CLOCK, CLK OUT */
                dirty = 1; t_change = fm1_ms;
            }
            if (r & MV_UI_PAGE) {
                lcd_fill(0, 0, 240, 240, C_BLACK);  /* the whole screen (no footer now: it is the DEBUG page) */
                dbg_all = 1;
                if (mv_ui_home(&ui) && !mv_ui_overlay(&ui)) {   /* back home: everything there drawn again */
                    home_header();
                    prog_drawn = ~0u; knobs_drawn = ~0u; voices_drawn = ~0u; scope_live = 1;
                }
            }
        }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0)   /* PRESETS: the programs (wrapping 64 -> 1) */
            prog_select((uint32_t)(((int32_t)ui.cur + e % (int32_t)MV_NPROG + (int32_t)MV_NPROG) % (int32_t)MV_NPROG), 1);
        if (prog_req < MV_NPROG) { prog_select(prog_req, 0); prog_req = 0xFF; }   /* MIDI program change in */
        if (r_prog) {                           /* SAVE on its page: WRITE / SWAP / FACTORY (in RAM now, to flash
                                                   once nothing sounds) */
            uint32_t to = ui.act_to % MV_NPROG;
            if (ui.act == MV_SV_WRITE) mv_prog_write(&prog, to, &eng.patch, &eng.lower, eng.perf);
            else if (ui.act == MV_SV_SWAP) mv_prog_swap(&prog, ui.cur, to);
            else if (ui.act == MV_SV_FACTORY) {
                mv_prog_factory(&prog, to);
                if (to == ui.cur) prog_to_engine(to);
            }
            if (ui.act != MV_SV_FACTORY) { ui.cur = (uint8_t)to; set.prog = (uint8_t)to; dirty = 1; t_change = fm1_ms; }
            ui.act = MV_SV_NONE; r_prog = 0; prog_drawn = ~0u;
        }
        if (clip_req) {                         /* CLIP: LOAD into / SAVE from the selected track */
            uint32_t trk = mv_ui_track(&ui, &eng);
            int rc = clip_req == MV_CL_LOAD + 1u ? mv_clip_load(&clips, &eng, ui.cl_sel, trk)
                                                 : mv_clip_save(&clips, &eng, trk, ui.cl_sel, ui.cl_name);
            if (rc != MV_CLIP_AGAIN) {          /* (again: the next pass) */
                ui.cl_msg = (uint8_t)(rc == MV_CLIP_BUSY ? MV_CLM_BUSY : rc == MV_CLIP_EMPTY ? MV_CLM_EMPTY
                                      : rc == MV_CLIP_RO ? MV_CLM_RO : rc == MV_CLIP_LONG ? MV_CLM_LONG
                                      : clip_req == MV_CL_LOAD + 1u ? MV_CLM_LOADED : MV_CLM_SAVED);
                ui.cl_msg_t = fm1_ms;
                if (rc == MV_CLIP_OK && clip_req == MV_CL_LOAD + 1u) {   /* its LENGTH: a setting, kept */
                    for (k = 0; k < MV_SQ_N; k++) set.seq[k] = eng.seqb[k];
                    dirty = 1; t_change = fm1_ms;
                }
                clip_req = 0;
            }
        }
        (void)e;

        midi_in();
        if (clk_bpm && fm1_ms - clk_t0 > 2000u) {   /* the clock has stopped coming: forget its tempo */
            clk_bpm = 0; clk_run = 0;
            clk_qi = clk_qn = clk_qsum = 0;     /* (a clock that comes back starts a new average) */
        }
        if (sel_dirty) { sel_dirty = 0; dirty = 1; t_change = fm1_ms; }   /* the program's number: kept in flash */

        oct = (int8_t)set.oct;
        base = (uint32_t)((int32_t)KEY0 + 12 * oct);
        for (k = 0; k < 27u; k++)               /* what sounds, on the keys of this octave */
            if (mv_engine_sounding(&eng, base + k))
                lit |= 1u << k;
        for (k = 0; k < 27u; k++)
            fm1_led_key(KEY_LED(k), lit >> k & 1u);
        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        {   /* PLAY: the sequencer running, dark for the first 1/8 of each beat (a blink a beat); REC: a track armed,
               blinking with it while it records (Kris 6 Nov, 7a: the MIDI clock shows on DEBUG) */
            uint32_t run = eng.seq_run != 0u, blink = run && eng.seq_live && eng.seq.ticks % MV_SEQ_PPQN < MV_SEQ_PPQN / 8u;
            fm1_led_key(BTN_PLAY, run && !blink);
            fm1_led_key(BTN_REC, eng.seq_arm != 0u && !blink);
        }
        {   /* the program: edited (the patch differs from its slot: a 248-byte compare) -> *, SAVE lit; lit too while
             * a written program waits for silence to go to flash */
            int edited = mv_prog_edited(&prog, ui.cur, &eng.patch, &eng.lower, eng.perf);
            fm1_led_key(BTN_SAVE, edited || mv_prog_dirty(&prog) || PAGES[ui.page].btn == BTN_SAVE ||
                                  mv_clip_dirty(&clips) || mv_ui_clip_page(&ui));   /* (CLIP: SAVE does it; one waits) */
            if (mv_ui_home(&ui) && !bank_ask) {
                const uint8_t *nm = eng.patch.b, *ln = eng.lower.b;
                uint32_t lo = (ui.midi & MV_MIDI_LOCALOFF) != 0u;   /* LOCAL OFF shows on the program line */
                uint32_t mode = eng.perf[MV_PF_MODE], key = ui.cur | (uint32_t)edited << 7 | mode << 8 | lo << 10, i;
                for (i = 0; i < MV_PROG_NAME; i++) key = key * 31u + nm[i];
                if (mode) for (i = 0; i < MV_PROG_NAME; i++) key = key * 31u + ln[i];
                if (mode == MV_KEY_SPLIT) key = key * 31u + eng.perf[MV_PF_SPLIT];
                if (key != prog_drawn) { prog_drawn = key; home_program(ui.cur, nm, edited, mode, ln, eng.perf[MV_PF_SPLIT], (int)lo); }
            }
        }
        fm1_led_key(BTN_ARP, eng.arpb[MV_ARP_ON] != 0u);   /* the arp on */
        /* the arp's quarter note in samples: the MIDI clock's 4-quarter average while it comes (clk_bpm drops 2 s
         * after it stops), else TEMPO. 32-bit: FS * clk_qsum is at most 44100 * 4 * 2000 = 352.8 M (a quarter
         * over 2 s ends the clock). A divide a pass, main loop only; mv_engine_sync divides only on a change. */
        {   /* the MIDI page (Kris 7 Nov, DAW use): CLOCK INT taken while a clock was being followed lets go of it (the
             * sequencer stops if the clock was running it: no pulses would come to move it) */
            static uint32_t midi_was;
            if ((ui.midi & ~midi_was & MV_MIDI_CLKINT) && (clk_bpm || eng.seq_ext)) {
                clk_bpm = 0; clk_run = 0; clk_qi = clk_qn = clk_qsum = 0; clk_seen = 0;
                if (eng.seq_ext) { eng.seq_run = 0; eng.seq_arm = 0; eng.seq_ext = 0; }
            }
            midi_was = ui.midi;
        }
        {   /* CLK OUT (3a): on, and the FM-1 on its own tempo (CLOCK INT, or no F8 in for 0.5 s): never back to a
             * clock's source. PLAY (the sequencer started from the panel: seq_ext 0) sends START, then beat 1's F8 at
             * the next buffer, and restarts the arp's pattern with it, as an incoming START does; stopping sends STOP */
            static uint32_t mco_run;
            uint32_t ext = !(ui.midi & MV_MIDI_CLKINT) && clk_seen && fm1_ms - clk_last < 500u;
            uint32_t live = (ui.midi & MV_MIDI_CLKOUT) && !ext, run = eng.seq_run && !eng.seq_ext;
            mco_live = live;
            if (run != mco_run) {
                mco_run = run;
                if (live && run) {
                    mco_sync = 1;               /* START, then beat 1, from the next buffer (mco_block) */
                    eng.arp_start = 1;
                } else if (live) midi_send(0xFCu, 0, 0);   /* STOP */
            }
        }
        eng.arp_beat = clk_bpm && clk_qn ? FS * clk_qsum / (clk_qn * 1000u)
                                         : FS * 60u / (eng.tempo ? eng.tempo : 120u);
        mv_engine_sync(&eng);                   /* LFO1 SYNC: the parts' steps at this beat (a divide only on a change) */
        for (k = 0; k < 8u; k++)                /* the section shown: its button lit */
            fm1_led_key(UI_LED_BTN[k], mv_ui_button(&ui) == UI_LED_BTN[k]);
        if (bank_ask) { fm1_led_key(BTN_SAVE, 1); fm1_led_key(BTN_HOME, 1); }   /* the two answers lit */
#if BOARD_SMK37
        for (k = 0; k < 16u; k++) {             /* a pad glows lilac while its key sounds */
            uint32_t w = k < 8u ? k + 8u : k - 8u, on = lit >> WHITE[w] & 1u;
            padled_set(k, on ? 90 : 4, on ? 60 : 2, on ? 128 : 8);
        }
        padled_flush();                         /* (sends only what changed) */
#endif

        idle_ok = 1;                            /* the drawing below may read MIDI meanwhile (LCD_IDLE) */
        scope_on = mv_ui_home(&ui) && !mv_ui_overlay(&ui);   /* the scope ring: only while HOME shows */
        if (xfer.dump_n) {                      /* a requested dump: one message a pass (keys and MIDI keep going) */
            static uint8_t msg[MV_XF_MSG_MAX];
            uint32_t len = mv_xfer_next(&xfer, &prog, &eng, msg, sizeof msg);
            if (len && sysex_send(msg, len)) xfer.dump_n = 0;   /* the computer stopped reading: the dump ends */
            xfer_t = fm1_ms;
        }
        if (xfer_shown && !xfer_busy()) {       /* the transfer is over: the screen as it was */
            xfer_shown = 0;
            lcd_fill(0, 0, 240, 240, C_BLACK);
            if (mv_ui_home(&ui)) { home_header(); prog_drawn = ~0u; knobs_drawn = ~0u; voices_drawn = ~0u; scope_live = 1; }
            else { ui.redraw = 1; dbg_all = 1; }
        }
        if (bank_ask) {                         /* the factory bank question: drawn once */
            if (bank_ask == 1) { home_bank_ask(); bank_ask = 2; }
        } else if (xfer_slots && xfer_busy()) {   /* a transfer: what it does, and how far */
            uint32_t key = (uint32_t)xfer.rx_n | (uint32_t)xfer.tx_n << 12 | (uint32_t)xfer.last_slot << 24;
            if (!xfer_shown || key != xfer_key) {
                home_xfer(xfer.tx_n != 0u, xfer.tx_n ? xfer.tx_n : xfer.rx_n, xfer.tx_n ? (uint32_t)xfer.dump_pp : xfer.last_slot, !xfer_shown);
                xfer_shown = 1; xfer_key = key;
            }
        } else if (mv_ui_overlay(&ui)) {        /* PART SELECT / the CLEAR question: over the page (mv_ui.c) */
            mv_ui_overlay_draw(&ui, &eng);
        } else if (mv_ui_home(&ui)) {           /* HOME (mv_home.c): each part redrawn only when it changed */
            int touched = ui.last < 4u && fm1_ms - ui.t_touch < 3000u ? (int)ui.last : -1;   /* white for 3 s */
            if ((st = home_knobs_key(&eng, touched)) != knobs_drawn) { knobs_drawn = st; home_knobs(&eng, touched); }
            if ((st = home_voices_key(&eng)) != voices_drawn) { voices_drawn = st; home_voices(&eng); }
            if (scope_live && fm1_ms - scope_t >= 33u) {   /* the scope: 30 a second while there is a signal */
                scope_t = fm1_ms;
                scope_live = home_scope(scope_ring, scope_w);   /* (silent: drawn flat once, then left) */
            }
            if (!quiet()) scope_live = 1;
        } else {
            mv_ui_draw(&ui, &eng, set.ch);      /* a page: what changed on it */
            if (mv_ui_roll(&ui)) { mv_roll_draw(&roll, &ui, &eng, dbg_all); dbg_all = 0; }   /* SEQ 2/2: the roll */
        }
        if (fm1_ms - cpu_t >= 250u && !cpu_clear) {   /* the meter, 4 times a second: the average of the last quarter
                                                         second, and the peak HELD over the last 3 s (12 quarters).
                                                         Run on every page (cheap: it also hands the ISR its reset,
                                                         keeping cpu_sum in range); drawn on DEBUG only */
            uint32_t n = cpu_n, sum = cpu_sum, pk = 0;
            cpu_slot[cpu_i++ % 12u] = cpu_peak;
            for (k = 0; k < 12u; k++) if (cpu_slot[k] > pk) pk = cpu_slot[k];
            cpu_t = fm1_ms;
            cpu_clear = 1;
            if (mv_ui_debug(&ui) && !xfer_shown) {   /* the DEBUG page (GLO): the rows that changed */
                mv_debug_t d;
                d.avg_us = n ? sum / n / FM1_TICKS_PER_US : 0u;
                d.avg_pc = d.avg_us * 100u / BUF_US;
                d.peak_pc = pk / FM1_TICKS_PER_US * 100u / BUF_US;
                d.act = mv_engine_active(&eng); d.nv = eng.nv;
                d.oct = oct; d.ch = set.ch; d.src = midi_src; d.bpm = clk_bpm; d.running = clk_run;
#if MV_PROFILE
                {   /* each stage's share of the buffer: ticks over (buffers x ticks per 1 %) */
                    uint32_t per = n ? n * BUF_US * FM1_TICKS_PER_US / 100u : 1u, o = eng.prof_osc, fl = eng.prof_flt;
                    d.prof = 1;
                    d.osc_pc = o / per; d.flt_pc = fl / per; d.etc_pc = sum > o + fl ? (sum - o - fl) / per : 0u;
                }
#else
                d.prof = 0; d.osc_pc = d.flt_pc = d.etc_pc = 0;
#endif
                mv_debug_draw(&d, dbg_all);
                dbg_all = 0;
            }
        }
        idle_ok = 0;

        if (mv_prog_dirty(&prog) && quiet() && !xfer_busy())   /* a SAVE (or a transfer, once it is over): its blob
                                                   (16 programs) to flash, when silent; one a pass */
            (void)mv_prog_flush(&prog);
        if (mv_clip_dirty(&clips) && !eng.seq_run && quiet() && !xfer_busy())   /* a clip SAVE: at STOP, when silent
                                                   (an erase, ~50 ms, would stall the sequencer's time otherwise) */
            (void)mv_clip_flush(&clips);
        if (dirty && quiet() && !xfer_busy() && fm1_ms - t_change > 2000u) {   /* 2 s after the change, when silent (an erase stalls the sound) */
            settings_save(&set, sizeof set);
            dirty = 0;
        }
    }
}
