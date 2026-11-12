/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance panel: implementation (pages, buttons, look and MIDI in mv_ui.h). */
#include "mv_ui.h"
#include "mv_build.h"                          /* MV_DEBUG: the DEBUG page (fm1-dock, 12 Nov) */

/* what a knob edits: a patch byte (JP offset 0..247), or one of these (from 0x100: never a patch byte) */
#define P_NONE 0xFFFFu                          /* a spare knob (decision 3: left empty) */
#define P_MIDICH 0x100u                         /* GLOBAL: settings, not patch bytes */
#define P_VOICES 0x101u
#define P_TEMPO 0x102u                          /* GLOBAL: the arp's tempo (BPM) */
#define P_LOCAL 0x103u                          /* MIDI (Kris 7 Nov, DAW use): LOCAL ON / OFF (mv_ui_t.midi bits) */
#define P_CLKSRC 0x104u                         /* MIDI: CLOCK AUTO / INT */
#define P_CLKOUT 0x105u                         /* MIDI: CLK OUT OFF / ON */
#define IS_MIDI(p) ((p) >= P_LOCAL && (p) <= P_CLKOUT)
#define P_RV 0x110u                             /* REVERB: P_RV + MV_RV_* (global settings, mv_reverb.h) */
#define P_ARP 0x120u                            /* ARP: P_ARP + MV_ARP_* (global settings, mv_arp.h) */
#define P_SV 0x130u                             /* SAVE: P_SV + 0 TO, 1 CURSOR, 2 CHAR, 3 MODE */
#define P_PF 0x140u                             /* PERFORM: P_PF + MV_PF_* (the program's performance bytes) */
#define P_PANEL 0x150u                          /* PERFORM: PANEL, the part(s) the pages edit (the panel's own) */
#define P_DEBUG 0x160u                          /* DEBUG: marks the page (KNOB1: nothing; mvance.c draws it, mv_debug.c) */
#define P_LSYNC 0x170u                          /* LFO 3/3: LFO1 SYNC of the part PANEL edits (a performance byte,
                                                   MV_PF_SYNCUP / LO: Kris 5 Nov, on the LFO page) */
#define P_CSYNC 0x171u                          /* MULTI-FX: CHORUS SYNC RATE, UPPER's (MV_PF_CSYNUP: the FX are UPPER's,
                                                   like the FX bytes beside it; Kris 7 Nov, 6a) */
#define P_DSYNC 0x172u                          /* DELAY 2/2: DELAY SYNC, UPPER's (MV_PF_DSYNUP); TIME shows it while on */
#define P_SQ 0x180u                             /* SEQ: P_SQ + MV_SQ_* (global settings, mv_seq.h) */
#define P_SQLEN 0x190u                          /* SEQ: LENGTH of the selected track (PANEL's part; BOTH both) */
#define P_SQBAR 0x191u                          /* ROLL: the bar shown (marks the page: mvance.c draws it, mv_roll.c) */
#define P_SQPITCH 0x192u                        /* ROLL: the lowest row shown */
#define P_SQROOT 0x193u                         /* ROLL: TRANSPOSE's ROOT of the selected track (RAM, with its notes) */
#define P_CL 0x1A0u                             /* CLIP: P_CL + 0 CLIP, 1 CURSOR, 2 CHAR, 3 MODE (the panel's own) */
#define TEMPO_MIN 40
#define TEMPO_MAX 240
#define IS_PATCH(p) ((p) < 0x100u)

typedef struct {
    const char *title;
    uint8_t btn;                                /* the section button (BTN_...) that reaches it */
    uint8_t sub, nsub;                          /* page sub+1 of nsub within the section (header "1/2") */
    uint16_t p[4];                              /* what each knob edits (a patch byte or P_...), left to right */
} ui_page_t;

static const ui_page_t PAGES[] = {
    {"HOME", BTN_HOME, 0, 1, {41, 42, 66, P_RV + MV_RV_LEVEL}},  /* home knobs (Kris 3 Nov): CUTOFF RESO, DELAY send, REVERB
                                                                   (the global LEVEL); drawn by mv_home.c */
    {"LFO", BTN_LFO, 0, 3, {16, 17, 18, 24}},                  /* LFO1 WAVE RATE FADE, LFO1 & ENV DEST */
    {"LFO", BTN_LFO, 1, 3, {19, 20, 25, 26}},                  /* LFO2 RATE, DEPTH SELECT, PITCH LFO1 / LFO2 */
    {"LFO", BTN_LFO, 2, 3, {P_LSYNC, P_NONE, P_NONE, P_NONE}}, /* LFO1 SYNC (the part's: a performance byte) */
    {"OSC 1", BTN_EDIT, 0, 1, {30, 31, 32, P_NONE}},           /* WAVE CTRL1 CTRL2 */
    {"OSC 2", BTN_EDIT, 0, 2, {33, 37, 38, 34}},               /* WAVE CTRL1 CTRL2 SYNC */
    {"OSC 2", BTN_EDIT, 1, 2, {35, 36, P_NONE, P_NONE}},       /* RANGE FINE/WIDE */
    {"OSC COMMON", BTN_EDIT, 0, 1, {21, 22, 23, 73}},          /* RING X-MOD BALANCE SHIFT */
    {"PITCH ENV", BTN_ENV, 0, 1, {27, 28, 29, P_NONE}},        /* DEPTH ATTACK DECAY */
    {"FILTER", BTN_SEL, 0, 2, {39, 40, 41, 42}},               /* TYPE SLOPE CUTOFF RESO */
    {"FILTER", BTN_SEL, 1, 2, {43, 44, 45, 46}},               /* KEY FOLLOW, LFO1, LFO2, ENV DEPTH */
    {"FILTER ENV", BTN_ENV, 0, 1, {47, 48, 49, 50}},           /* A D S R */
    {"AMP", BTN_SEL, 0, 1, {51, 52, 53, 58}},                  /* LEVEL, LFO1 (pan), LFO2, LFO1 MOD (pan mode) */
    {"AMP ENV", BTN_ENV, 0, 1, {54, 55, 56, 57}},              /* A D S R */
    {"TONE", BTN_FX, 0, 1, {59, 60, 245, P_NONE}},             /* BASS TREBLE, PATCH GAIN */
    {"MULTI-FX", BTN_FX, 0, 1, {61, 62, P_CSYNC, P_NONE}},     /* TYPE (12 = DISTORTION), LEVEL, CHORUS SYNC RATE */
    {"DELAY", BTN_FX, 0, 2, {63, 64, 65, 66}},                 /* TYPE TIME FEEDBACK LEVEL */
    {"DELAY", BTN_FX, 1, 2, {P_DSYNC, P_NONE, P_NONE, P_NONE}}, /* DELAY SYNC (Kris 7 Nov) */
    {"REVERB", BTN_FX, 0, 2, {P_RV + MV_RV_LEVEL, P_RV + MV_RV_SIZE, P_RV + MV_RV_HIDAMP, P_RV + MV_RV_LODAMP}},
    {"REVERB", BTN_FX, 1, 2, {P_RV + MV_RV_LOWPASS, P_RV + MV_RV_HIPASS, P_RV + MV_RV_FREEZE, P_NONE}},
    {"PLAY MODE", BTN_GLO, 0, 2, {71, 72, 69, 70}},            /* MONO LEGATO PORTA, PORTA TIME */
    {"PLAY MODE", BTN_GLO, 1, 2, {243, 244, 67, 68}},          /* UNISON, DETUNE, BEND UP / DOWN */
#if MV_PARTS == 2                               /* (one part: no performance to set; mv_build.h) */
    {"PERFORM", BTN_GLO, 0, 2, {P_PF + MV_PF_MODE, P_PF + MV_PF_SPLIT, P_PF + MV_PF_BAL, P_PANEL}},   /* (Kris 3 Nov) */
    {"PERFORM", BTN_GLO, 1, 2, {P_PF + MV_PF_VSPLIT, P_PF + MV_PF_ARP, P_PF + MV_PF_CHLO, P_PF + MV_PF_CHUP}},
#endif
    {"GLOBAL", BTN_GLO, 0, 1, {P_MIDICH, P_VOICES, P_TEMPO, P_NONE}},
    {"MIDI", BTN_GLO, 0, 1, {P_LOCAL, P_CLKSRC, P_CLKOUT, P_NONE}},   /* DAW use (Kris 7 Nov): LOCAL, CLOCK, CLK OUT */
#if MV_DEBUG
    {"DEBUG", BTN_GLO, 0, 1, {P_DEBUG, P_NONE, P_NONE, P_NONE}},   /* the meter and the status (Kris 3 Nov): the knobs
                                                                   do nothing here */
#endif
    {"ARP", BTN_ARP, 0, 2, {P_ARP + MV_ARP_MODE, P_ARP + MV_ARP_RATE, P_ARP + MV_ARP_OCT, P_ARP + MV_ARP_GATE}},
    {"ARP", BTN_ARP, 1, 2, {P_ARP + MV_ARP_HOLD, P_ARP + MV_ARP_SWING, P_ARP + MV_ARP_CHANCE, P_ARP + MV_ARP_ORDER}},
#if MV_SEQ
    {"SEQ", BTN_SEQ, 0, 3, {P_SQLEN, P_SQ + MV_SQ_QUANT, P_SQ + MV_SQ_MUTELO, P_SQ + MV_SQ_MUTEUP}},   /* (Kris 6 Nov) */
    {"SEQ", BTN_SEQ, 1, 3, {P_SQBAR, P_SQPITCH, P_SQ + MV_SQ_XPOSE, P_SQROOT}},   /* the piano roll (phase 2) */
    {"SEQ", BTN_SEQ, 2, 3, {P_CL + 0, P_CL + 1, P_CL + 2, P_CL + 3}},          /* the clips (phase 3) */
#endif
    {"SAVE", BTN_SAVE, 0, 1, {P_SV + 0, P_SV + 1, P_SV + 2, P_SV + 3}},
};
#define NPAGES (sizeof PAGES / sizeof PAGES[0])
/* HOME's knob labels (mv_home.c draws them; the dump gives them to the cheat sheet): the delay's LEVEL is its send,
 * the reverb's LEVEL the global one */
static const char *const HOME_LABEL[4] = {"CUTOFF", "RESO", "DELAY", "REVERB"};

/* the one panel (mv_ui_init): col_value reads the SAVE page's state from it */
static const mv_ui_t *ui_self;
static uint32_t page_of(uint32_t b);

/* the part a patch byte is edited in (PANEL, Kris 3 Nov): LOWER only when the panel says so and the program is
 * layered; TONE / MULTI-FX / DELAY (bytes 59..66) always UPPER's, the one chain's. both: also the other part */
static int fx_byte(uint32_t p) { return p >= 59u && p <= 66u; }
static uint32_t ui_part(const mv_engine_t *e, uint32_t p, int *both)
{
    uint32_t panel = ui_self ? ui_self->panel : MV_UI_UPPER;
    *both = 0;
    if (e->perf[MV_PF_MODE] == MV_KEY_SINGLE || fx_byte(p)) return MV_PART_UP;
    if (panel == MV_UI_BOTH) { *both = 1; return MV_PART_UP; }
    return panel == MV_UI_LOWER ? MV_PART_LO : MV_PART_UP;
}
/* CLIP: the name to edit from the clip chosen: its own, or CLIPnn when it is empty */
static void clip_name_from(mv_ui_t *u)
{
    uint32_t i, k = u->cl_sel;
    if (k >= MV_CLIP_N) return;                 /* (a factory clip: its own name shows; the edited one is kept) */
    if (u->clips && u->clips->dir[k].used) {
        for (i = 0; i < MV_CLIP_NAME; i++) u->cl_name[i] = u->clips->dir[k].name[i];
        return;
    }
    for (i = 0; i < 4u; i++) u->cl_name[i] = "CLIP"[i];
    u->cl_name[4] = (char)('0' + (k + 1u) / 10u); u->cl_name[5] = (char)('0' + (k + 1u) % 10u);
}

uint32_t mv_ui_track(const mv_ui_t *u, const mv_engine_t *e)
{
    return e->perf[MV_PF_MODE] != MV_KEY_SINGLE && u->panel == MV_UI_LOWER ? MV_PART_LO : MV_PART_UP;
}
static const mv_patch_t *ui_patch(const mv_engine_t *e, uint32_t p)   /* what a column shows */
{
    int both;
    return ui_part(e, p, &both) == MV_PART_LO ? &e->lower : &e->patch;
}

/* the section buttons (a list: the board's button ids differ, FM-1 0..13, SMK-37 0..17) */
static const uint8_t SECTION_BTN[6] = {BTN_LFO, BTN_EDIT, BTN_ENV, BTN_SEL, BTN_FX, BTN_GLO};
static const char *const SECTION_NAME[6] = {"LFO", "EDIT", "ENV", "SEL", "FX", "GLO"};

static const char *btn_name(uint32_t b)
{
    uint32_t i;
    for (i = 0; i < 6u; i++) if (SECTION_BTN[i] == b) return SECTION_NAME[i];
    return b == BTN_ARP ? "ARP" : b == BTN_SAVE ? "SAVE" : b == BTN_HOME ? "HOME" : b == BTN_SEQ ? "SEQ" : "";
}

/* ---- what a column shows */

/* the label: OSC CTRL1 / CTRL2 take the wave's own names (the JE-8086's), a few read better spelled out than the
 * table's 7-character forms, the rest are the table's (mv_params.h) */
static const char *col_label(const mv_engine_t *e, uint32_t p)
{
    static const char *const C1A[7] = {"DETUNE", "OFFSET", "CUTOFF", "HARMON", "PW", "SHAPE", "SHAPE"};
    static const char *const C1B[7] = {"MIX", "LFO1", "RESO", "FEEDBK", "PWM", "LFO1", "LFO1"};
    static const char *const C2A[4] = {"PW", "SHAPE", "SHAPE", "CUTOFF"};
    static const char *const C2B[4] = {"PWM", "LFO1", "LFO1", "RESO"};
    const uint8_t *b = ui_patch(e, p)->b;
    switch (p) {
    case P_PF + MV_PF_MODE: return "MODE";
    case P_PF + MV_PF_SPLIT: return "SPLIT";
    case P_PF + MV_PF_BAL: return "BALANCE";
    case P_PF + MV_PF_VSPLIT: return "VOICES";
    case P_PF + MV_PF_ARP: return "ARP";
    case P_PF + MV_PF_CHLO: return "CH LO";
    case P_PF + MV_PF_CHUP: return "CH UP";
    case P_PANEL: return "PANEL";
    case P_LSYNC: return "SYNC 1";
    case P_CSYNC: case P_DSYNC: return "SYNC";
    case P_MIDICH: return "MIDI CH";
    case P_VOICES: return "VOICES";
    case P_RV + MV_RV_LEVEL: return "LEVEL";
    case P_RV + MV_RV_SIZE: return "SIZE";
    case P_RV + MV_RV_HIDAMP: return "HI DAMP";
    case P_RV + MV_RV_LODAMP: return "LO DAMP";
    case P_RV + MV_RV_LOWPASS: return "LOWPASS";
    case P_RV + MV_RV_HIPASS: return "HIPASS";
    case P_RV + MV_RV_FREEZE: return "FREEZE";
    case P_TEMPO: return "TEMPO";
    case P_LOCAL: return "LOCAL";
    case P_CLKSRC: return "CLOCK";
    case P_CLKOUT: return "CLK OUT";
    case P_SV + 0: return "TO";
    case P_SV + 1: return "CURSOR";
    case P_SV + 2: return "CHAR";
    case P_SV + 3: return "MODE";
    case P_ARP + MV_ARP_MODE: return "MODE";
    case P_ARP + MV_ARP_RATE: return "RATE";
    case P_ARP + MV_ARP_OCT: return "OCTAVE";
    case P_ARP + MV_ARP_GATE: return "GATE";
    case P_ARP + MV_ARP_HOLD: return "HOLD";
    case P_ARP + MV_ARP_SWING: return "SWING";
    case P_ARP + MV_ARP_CHANCE: return "CHANCE";
    case P_ARP + MV_ARP_ORDER: return "ORDER";
    case P_SQLEN: return "LENGTH";
    case P_SQ + MV_SQ_QUANT: return "QUANT";
    case P_SQ + MV_SQ_MUTELO: return "MUTE L";
    case P_SQ + MV_SQ_MUTEUP: return "MUTE U";
    case P_SQBAR: return "BAR";
    case P_SQPITCH: return "PITCH";
    case P_SQ + MV_SQ_XPOSE: return "TRANSP";
    case P_SQROOT: return "ROOT";
    case P_CL + 0: return "CLIP";
    case P_CL + 1: return "CURSOR";
    case P_CL + 2: return "CHAR";
    case P_CL + 3: return "MODE";
    case 31: return C1A[b[30] < 7u ? b[30] : 5u];
    case 32: return C1B[b[30] < 7u ? b[30] : 5u];
    case 37: return C2A[b[33] & 3u];
    case 38: return C2B[b[33] & 3u];
    case 16: case 19: return p == 16u ? "WAVE" : "RATE 2";
    case 17: return "RATE 1";
    case 20: return "DEPTH>";
    case 22: return "X-MOD";
    case 24: return "DEST";
    case 25: return "P.LFO1";
    case 26: return "P.LFO2";
    case 43: return "KEY FLW";
    case 46: return "ENV DEP";
    case 58: return "PAN";
    case 67: return "BEND +";
    case 68: return "BEND -";
    case 70: return "TIME";
    case 244: return "DETUNE";
    default: return IS_PATCH(p) ? MV_PARAMS[MV_PARAM_AT[p]].label : "";
    }
}

/* short names for the choices (7 characters or fewer: one column); 0 = a number. The table's own lists
 * (MV_N_...) are for SysEx / logs and longer than a column */
static const char *choice_name(uint32_t p, uint32_t v)
{
    static const char *const OFFON[2] = {"OFF", "ON"};
    static const char *const LW[4] = {"TRI", "SAW", "SQR", "RANDOM"};
    static const char *const DEST[3] = {"OSC1+2", "OSC2", "X-MOD"};
    static const char *const DSEL[3] = {"PITCH", "FILTER", "AMP"};
    static const char *const W1[7] = {"SSAW", "TRIMOD", "NOISE", "FBK", "SQR", "SAW", "TRI"};
    static const char *const W2[4] = {"SQR", "SAW", "TRI", "NOISE"};
    static const char *const FT[3] = {"HPF", "BPF", "LPF"};
    static const char *const SL[2] = {"-12", "-24"};
    static const char *const PAN[3] = {"OFF", "AUTO", "MANUAL"};
    static const char *const GAIN[3] = {"0dB", "+6dB", "+12dB"};
    static const char *const MFX[13] = {"SC SLW", "SC MID", "SC FST", "SC CLR", "FL SLOW", "FL DEEP", "FL FAST",
                                        "DEEP PH", "JET PH", "TWIST", "FREEZE1", "FREEZE2", "DIST"};
    static const char *const DLY[5] = {"PAN L>R", "PAN R>L", "PAN SHT", "MONO S", "MONO L"};
    static const char *const AMODE[MV_AM_N] = {"UP", "DN", "UPDN", "RND", "ORD", "REPEAT", "DNUP", "UP+8", "CONV",
                                               "DIVG", "PINKY", "THUMB", "WALK", "CHORD"};   /* (Skiff's N_AMODE) */
    static const char *const ARATE[MV_ARP_NRATE] = {"1/4", "1/8", "1/16", "1/32", "8T", "16T", "1/2", "1/1", "2BAR",
                                                    "4BAR"};                                   /* (Skiff's N_DIV) */
    static const char *const AORDER[2] = {"NOTE", "PLAYED"};
    static const char *const SVMODE[3] = {"WRITE", "SWAP", "FACTORY"};
    static const char *const KMODE[3] = {"SINGLE", "DUAL", "SPLIT"};
    static const char *const PANEL[3] = {"UPPER", "LOWER", "BOTH"};
    static const char *const VSPL[7] = {"1+7", "2+6", "3+5", "4+4", "5+3", "6+2", "7+1"};   /* LOWER + UPPER */
    static const char *const ARPP[4] = {"AUTO", "UPPER", "LOWER", "BOTH"};
    static const char *const SQQ[MV_SQQ_N] = {"OFF", "1/16", "1/8", "1/16T", "1/8T"};
    static const char *const SQX[MV_SQX_N] = {"OFF", "LOWER", "UPPER", "BOTH"};   /* TRANSPOSE: its tracks */
    static const char *const CLM[2] = {"LOAD", "SAVE"};
    static const char *const LOCAL[2] = {"ON", "OFF"};   /* (the bit is LOCAL OFF: 0 = ON, today's) */
    static const char *const CSRC[2] = {"AUTO", "INT"};
    static const char *const SYNC[MV_CSYNC_N] = {"OFF", "1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.", "1/4",
                                                 "1/2T", "1/4.", "1/2", "1/1T", "1/2.", "1/1", "2/1T", "1/1.",
                                                 "2/1", "3 MEAS", "4 MEAS", "5 MEAS", "6 MEAS", "7 MEAS",
                                                 "8 MEAS", "LFO1"};   /* (the JE's lFO1Sync, short: . = dotted; DELAY
                                                   SYNC is its first MV_DSYNC_N, CHORUS SYNC RATE all of it) */
    switch (p) {
    case 16: return LW[v & 3u];
    case 20: return DSEL[v < 3u ? v : 0u];
    case 24: return DEST[v < 3u ? v : 0u];
    case 30: return W1[v < 7u ? v : 0u];
    case 33: return W2[v & 3u];
    case 39: return FT[v < 3u ? v : 2u];
    case 40: return SL[v & 1u];
    case 58: return PAN[v < 3u ? v : 0u];
    case 245: return GAIN[v < 3u ? v : 0u];
    case 61: return MFX[v < 13u ? v : 0u];
    case 63: return DLY[v < 5u ? v : 0u];
    case 21: case 34: case 69: case 71: case 72: case 243: case P_RV + MV_RV_FREEZE: case P_ARP + MV_ARP_HOLD:
    case P_SQ + MV_SQ_MUTELO: case P_SQ + MV_SQ_MUTEUP:
        return OFFON[v & 1u];
    case P_SQ + MV_SQ_QUANT: return SQQ[v < MV_SQQ_N ? v : MV_SQQ_16];
    case P_SQ + MV_SQ_XPOSE: return SQX[v < MV_SQX_N ? v : 0u];
    case P_CL + 3: return CLM[v & 1u];
    case P_LOCAL: return LOCAL[v & 1u];
    case P_CLKSRC: return CSRC[v & 1u];
    case P_CLKOUT: return OFFON[v & 1u];
    case P_ARP + MV_ARP_MODE: return AMODE[v < MV_AM_N ? v : 0u];
    case P_ARP + MV_ARP_RATE: return ARATE[v < MV_ARP_NRATE ? v : 2u];
    case P_ARP + MV_ARP_ORDER: return AORDER[v & 1u];
    case P_SV + 3: return SVMODE[v < 3u ? v : 0u];
    case P_PF + MV_PF_MODE: return KMODE[v < 3u ? v : 0u];
    case P_PF + MV_PF_VSPLIT: return VSPL[v < 7u ? v : 3u];
    case P_PF + MV_PF_ARP: return ARPP[v & 3u];
    case P_PANEL: return PANEL[v < 3u ? v : 0u];
    case P_LSYNC: return SYNC[v < MV_LFO1_SYNC_N ? v : 0u];
    case P_DSYNC: return SYNC[v < MV_DSYNC_N ? v : 0u];
    case P_CSYNC: return SYNC[v < MV_CSYNC_N ? v : 0u];
    default: return 0;
    }
}

#define BAR_NONE 0u                             /* a choice: just the value (decision 4) */
#define BAR_UP 1u                               /* a number: filled from the bottom */
#define BAR_MID 2u                              /* a signed number: from the middle line */

static void copy7(char *s, const char *a)
{
    uint32_t i = 0;
    while (a[i] && i < 7u) { s[i] = a[i]; i++; }
    s[i] = 0;
}

/* a number into s (+ in front of a signed positive) */
static void num(char *s, int32_t v, int sign)
{
    char t[8];
    uint32_t n = 0, a = (uint32_t)(v < 0 ? -v : v), i = 0;
    do { t[n++] = (char)('0' + a % 10u); a /= 10u; } while (a && n < 6u);
    if (v < 0) s[i++] = '-';
    else if (sign && v > 0) s[i++] = '+';
    while (n) s[i++] = t[--n];
    s[i] = 0;
}

/* the MIDI page: a parameter's bit in mv_ui_t.midi, and its value 0 / 1 */
static uint32_t midi_mask(uint32_t p)
{
    return p == P_LOCAL ? MV_MIDI_LOCALOFF : p == P_CLKSRC ? MV_MIDI_CLKINT : MV_MIDI_CLKOUT;
}
static uint32_t midi_bit(uint32_t midi, uint32_t p) { return midi & midi_mask(p) ? 1u : 0u; }

/* the value as text (s: 8 chars), *mode BAR_..., *fill 0..1000 (BAR_UP) or -1000..1000 (BAR_MID) */
static void col_value(const mv_engine_t *e, uint32_t p, uint32_t midi_ch, char *s, uint32_t *mode, int32_t *fill)
{
    const mv_param_t *d;
    const char *c;
    int32_t v;
    *mode = BAR_NONE; *fill = 0;
    if (p == P_MIDICH) {                        /* 0 = OMNI: in on all, out on 1 */
        if (midi_ch) num(s, (int32_t)midi_ch, 0); else copy7(s, "OMNI");
        return;
    }
    if (p == P_VOICES) {
        num(s, (int32_t)e->nv, 0);
        *mode = BAR_UP; *fill = (int32_t)e->nv * 1000 / (int32_t)MV_NV;
        return;
    }
    if (p == P_PANEL) { copy7(s, choice_name(p, ui_self ? ui_self->panel : 0u)); return; }
    if (IS_MIDI(p)) { copy7(s, choice_name(p, midi_bit(ui_self ? ui_self->midi : 0u, p))); return; }   /* MIDI page */
    if (p == P_LSYNC || p == 17u) {             /* LFO1 SYNC of the part edited; RATE 1 shows it in place of the
                                                   number while that part is synced (the JP ignores RATE then) */
        int both;
        uint32_t sy = e->perf[MV_PF_SYNC(ui_part(e, p, &both))];
        if (p == P_LSYNC || sy) { copy7(s, choice_name(P_LSYNC, sy)); return; }
    }
    if (p == P_CSYNC) { copy7(s, choice_name(p, e->perf[MV_PF_CSYNUP])); return; }
    if (p == P_DSYNC || p == 64u) {             /* DELAY SYNC; TIME shows it in place of its number while synced */
        uint32_t sy = e->perf[MV_PF_DSYNUP];
        if (p == P_DSYNC || sy) { copy7(s, choice_name(P_DSYNC, sy)); return; }
    }
    if (p >= P_PF && p < P_PF + MV_PF_N) {      /* PERFORM: the program's performance bytes */
        static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        uint32_t k = p - P_PF, pv = e->perf[k];
        if ((c = choice_name(p, pv)) != 0) { copy7(s, c); return; }
        if (k == MV_PF_SPLIT) {                 /* the split as a note name: C4 = 60 */
            uint32_t i = 0;
            for (c = NN[pv % 12u]; *c; c++) s[i++] = *c;
            if (pv < 12u) s[i++] = '-', s[i++] = '1';
            else s[i++] = (char)('0' + pv / 12u - 1u);
            s[i] = 0;
            return;
        }
        if ((k == MV_PF_CHLO || k == MV_PF_CHUP) && !pv) { copy7(s, "GLOBAL"); return; }
        num(s, (int32_t)pv, 0);
        if (k == MV_PF_BAL) { *mode = BAR_MID; *fill = ((int32_t)pv - 64) * 1000 / 64; }
        return;
    }
    if (p >= P_SV && p < P_SV + 4u) {           /* SAVE: values only (the page's two lines show the names) */
        const mv_ui_t *u = ui_self;
        uint32_t c;
        if (p == P_SV + 0u) num(s, u->sv_to + 1, 0);
        else if (p == P_SV + 1u) num(s, u->sv_pos + 1, 0);
        else if (p == P_SV + 2u) {              /* the character under the cursor; a space shown as SPC */
            c = e->patch.b[u->sv_pos % MV_PROG_NAME];
            if (c == ' ') copy7(s, "SPC"); else { s[0] = (char)c; s[1] = 0; }
        } else copy7(s, choice_name(p, u->sv_mode));
        return;
    }
    if (p == P_SQLEN) {                         /* SEQ LENGTH: the selected track's (BOTH: UPPER's shown) */
        int both;
        uint32_t v = e->seqb[MV_SQ_LEN(ui_part(e, 0u, &both))], i = 0;   /* (Kris 7 Nov, 2a) 1..32 bars: "12 BAR" */
        if (v >= 10u) s[i++] = (char)('0' + v / 10u);
        s[i++] = (char)('0' + v % 10u);
        s[i++] = ' '; s[i++] = 'B'; s[i++] = 'A'; s[i++] = 'R'; s[i] = 0;
        return;
    }
    if (p >= P_SQ && p < P_SQ + MV_SQ_N) { copy7(s, choice_name(p, e->seqb[p - P_SQ])); return; }   /* (all choices) */
    if (p >= P_CL && p < P_CL + 4u) {           /* CLIP: values only (the page's two lines show the names) */
        const mv_ui_t *u = ui_self;
        if (!u) { s[0] = 0; return; }
        if (p == P_CL + 0u) {                   /* 1..12, then the factory clips F1, F2 .. */
            if (u->cl_sel < MV_CLIP_N) num(s, u->cl_sel + 1, 0);
            else { s[0] = 'F'; num(s + 1, (int32_t)(u->cl_sel - MV_CLIP_N) + 1, 0); }
        }
        else if (p == P_CL + 1u) num(s, u->cl_pos + 1, 0);
        else if (p == P_CL + 2u) {              /* the character under the cursor; a space shown as SPC */
            char ch = u->cl_name[u->cl_pos % MV_CLIP_NAME];
            if (ch == ' ') copy7(s, "SPC"); else { s[0] = ch; s[1] = 0; }
        } else copy7(s, choice_name(p, u->cl_mode));
        return;
    }
    if (p == P_SQBAR) { num(s, (int32_t)(ui_self ? ui_self->roll_bar : 0u) + 1, 0); return; }   /* (the ROLL page
                                                   draws its own: these are for the edit line's and dump's sake) */
    if (p == P_SQPITCH) { num(s, ui_self ? ui_self->roll_lo : 0, 0); return; }
    if (p == P_SQROOT) {
        uint32_t r = e->seq.t[ui_self ? mv_ui_track(ui_self, e) : 0u].root;
        if (r == MV_SEQ_NOROOT) copy7(s, "-"); else num(s, (int32_t)r, 0);
        return;
    }
    if (p == P_TEMPO) {                         /* BPM (the MIDI clock's, when one comes, rules the arp) */
        num(s, e->tempo, 0);
        *mode = BAR_UP; *fill = (e->tempo - TEMPO_MIN) * 1000 / (TEMPO_MAX - TEMPO_MIN);
        return;
    }
    if (p >= P_ARP && p < P_ARP + MV_ARP_N) {   /* ARP: choices by name, numbers with a bar over their range */
        uint32_t k = p - P_ARP, av = e->arpb[k];
        if ((c = choice_name(p, av)) != 0) { copy7(s, c); return; }
        num(s, (int32_t)av, 0);
        *mode = BAR_UP;
        *fill = MV_ARP_MAX[k] > MV_ARP_MIN[k] ? ((int32_t)av - MV_ARP_MIN[k]) * 1000 / (MV_ARP_MAX[k] - MV_ARP_MIN[k]) : 0;
        return;
    }
    if (p >= P_RV && p < P_RV + MV_RV_N) {      /* REVERB: 0..127, FREEZE OFF / ON */
        uint32_t rv = e->rvb[p - P_RV];
        if ((c = choice_name(p, rv)) != 0) { copy7(s, c); return; }
        num(s, (int32_t)rv, 0);
        *mode = BAR_UP; *fill = (int32_t)rv * 1000 / 127;
        return;
    }
    d = &MV_PARAMS[MV_PARAM_AT[p]];
    v = mv_param_get(ui_patch(e, p), MV_PARAM_AT[p]);
    if ((c = choice_name(p, (uint32_t)v)) != 0) { copy7(s, c); return; }
    if (d->disp == MV_D_SIGNED || d->disp == MV_D_CENT || d->disp == MV_D_RANGE) {
        int32_t up = d->max - d->centre, dn = d->centre - d->min, span = up > dn ? up : dn;
        if (d->disp == MV_D_RANGE && (v == d->min || v == d->max))   /* OSC2 RANGE's ends: WIDE */
            copy7(s, v == d->min ? "-WIDE" : "+WIDE");
        else
            num(s, v - d->centre, 1);
        *mode = BAR_MID; *fill = span ? (v - (int32_t)d->centre) * 1000 / span : 0;
        return;
    }
    num(s, v, 0);
    *mode = BAR_UP; *fill = d->max > d->min ? (v - d->min) * 1000 / (d->max - d->min) : 0;
}

uint32_t mv_ui_button(const mv_ui_t *u) { return PAGES[u->page].btn; }
int mv_ui_roll(const mv_ui_t *u) { return PAGES[u->page].p[0] == P_SQBAR; }
int mv_ui_clip_page(const mv_ui_t *u) { return PAGES[u->page].p[0] == P_CL; }
void mv_ui_set_clips(mv_ui_t *u, const mv_clip_t *c) { u->clips = c; clip_name_from(u); }
int mv_ui_debug(const mv_ui_t *u) { return PAGES[u->page].p[0] == P_DEBUG; }

int mv_ui_split_learn(const mv_ui_t *u)
{
    return !u->learned && u->last < 4u && PAGES[u->page].p[u->last] == P_PF + MV_PF_SPLIT && fm1_ms - u->t_touch < 3000u;
}
void mv_ui_learned(mv_ui_t *u) { u->t_touch = fm1_ms; u->learned = 1; }   /* one key: the next ones play again */

/* ---- input */

void mv_ui_init(mv_ui_t *u, const mv_prog_t *prog)
{
    uint32_t k;
    ui_self = u;
    u->prog = prog; u->cur = 0; u->sv_to = 0; u->sv_pos = 0; u->sv_mode = 0; u->act = MV_SV_NONE; u->act_to = 0;
    u->drawn_sv = ~0u; u->arp_down = 0; u->arp_did = 0; u->arp_t = 0;
    u->page = 0; u->last = 0xFF; u->redraw = 1; u->t_touch = 0; u->drawn_line = ~0u;
    u->panel = MV_UI_UPPER; u->drawn_tag = 0; u->learned = 0;
    u->ovl = MV_OV_NONE; u->ovl_t = 0; u->drawn_ovl = ~0u; u->ovl_track = MV_PART_UP;
    u->oct_held = 0; u->oct_undo = 0; u->rec_t = 0; u->rec_down = 0; u->rec_did = 0;
    u->roll_bar = 0; u->roll_lo = 48; u->roll_fit = 1;
    u->clips = 0; u->cl_sel = 0; u->cl_pos = 0; u->cl_mode = MV_CL_LOAD; u->cl_act = 0; u->cl_msg = MV_CLM_NONE;
    u->cl_msg_t = 0;
    u->midi = 0;                                /* MIDI page: today's behaviour until the settings load it */
    for (k = 0; k < MV_CLIP_NAME; k++) u->cl_name[k] = "CLIP01"[k];
    for (k = 0; k < 4u; k++) { u->t_knob[k] = 0; u->drawn[k] = ~0u; }
}

/* a part's MIDI channel out (0..15): its CH UP / CH LO, or out_ch (the global one) where that is GLOBAL (0) */
static uint32_t part_out_ch(const mv_engine_t *e, uint32_t part, uint32_t out_ch)
{
    uint32_t c = e->perf[part == MV_PART_LO ? MV_PF_CHLO : MV_PF_CHUP];
    return c ? c - 1u : out_ch;
}

/* a column's value moved by step (detents, signed): clamp, store, send the CC (decision 5). Returns
 * MV_UI_SETTING for the MIDI channel */
static uint32_t edit(mv_ui_t *u, mv_engine_t *e, uint32_t col, int32_t step, uint8_t *midi_ch, uint32_t out_ch)
{
    uint32_t p = PAGES[u->page].p[col];
    if (p == P_NONE || p == P_DEBUG) return 0;   /* (DEBUG: nothing to edit) */
    u->last = (uint8_t)col; u->t_touch = fm1_ms; u->learned = 0;   /* (a touch of SPLIT: its key learn again) */
    if (p == P_MIDICH) {
        uint8_t c = (uint8_t)mv_clamp((int32_t)*midi_ch + step, 0, 16);
        if (c == *midi_ch) return 0;
        *midi_ch = c;
        return MV_UI_SETTING;
    }
    if (p == P_VOICES) { e->nv = (uint32_t)mv_clamp((int32_t)e->nv + step, 1, (int32_t)MV_NV); return 0; }
    if (IS_MIDI(p)) {                           /* MIDI page: a turn right sets the bit, left clears it; kept in flash */
        uint32_t m = midi_mask(p), was = u->midi;
        u->midi = (uint8_t)(step > 0 ? u->midi | m : u->midi & ~m);
        return u->midi != was ? MV_UI_SETTING : 0u;
    }
    if (p == P_PANEL) { u->panel = (uint8_t)mv_clamp((int32_t)u->panel + step, 0, MV_UI_BOTH); return 0; }
    if (p == P_LSYNC) {                         /* into the program, the part PANEL says (BOTH: each by the step) */
        int both;
        uint32_t k = MV_PF_SYNC(ui_part(e, p, &both));
        e->perf[k] = (uint8_t)mv_clamp((int32_t)e->perf[k] + step, 0, (int32_t)MV_LFO1_SYNC_N - 1);
        if (both)
            e->perf[MV_PF_SYNCLO] = (uint8_t)mv_clamp((int32_t)e->perf[MV_PF_SYNCLO] + step, 0, (int32_t)MV_LFO1_SYNC_N - 1);
        return 0;
    }
    if (p == P_CSYNC || p == P_DSYNC) {         /* into the program, UPPER's (the FX are UPPER's) */
        uint32_t k = p == P_CSYNC ? MV_PF_CSYNUP : MV_PF_DSYNUP;
        int32_t top = (int32_t)(p == P_CSYNC ? MV_CSYNC_N : MV_DSYNC_N) - 1;
        e->perf[k] = (uint8_t)mv_clamp((int32_t)e->perf[k] + step, 0, top);
        return 0;
    }
    if (p >= P_PF && p < P_PF + MV_PF_N) {      /* PERFORM: into the program (SAVE keeps it; not sent: no JP CC) */
        static const uint8_t TOP[MV_PF_N] = {2, 127, 127, 6, 16, 16, 3, MV_LFO1_SYNC_N - 1u, MV_LFO1_SYNC_N - 1u,
                                             MV_DSYNC_N - 1u, MV_DSYNC_N - 1u, MV_CSYNC_N - 1u, MV_CSYNC_N - 1u};
        uint32_t k = p - P_PF;
        e->perf[k] = (uint8_t)mv_clamp((int32_t)e->perf[k] + step, 0, TOP[k]);
        return 0;
    }
    if (p >= P_SV && p < P_SV + 4u) {           /* SAVE: the target, the cursor, the character, the mode */
        if (p == P_SV + 0u) u->sv_to = (uint8_t)mv_clamp((int32_t)u->sv_to + step, 0, MV_NPROG - 1);
        else if (p == P_SV + 1u) u->sv_pos = (uint8_t)mv_clamp((int32_t)u->sv_pos + step, 0, MV_PROG_NAME - 1u);
        else if (p == P_SV + 2u) {              /* ASCII 32..125, as the JP's names; the patch shows edited (*) */
            uint8_t *c = &e->patch.b[u->sv_pos % MV_PROG_NAME];
            *c = (uint8_t)mv_clamp((int32_t)*c + step, 32, 125);
        } else u->sv_mode = (uint8_t)mv_clamp((int32_t)u->sv_mode + step, 0, 2);
        return 0;
    }
    if (p == P_SQLEN) {                         /* SEQ LENGTH, the selected track's (BOTH: each by the step) */
        int both;
        uint32_t k = MV_SQ_LEN(ui_part(e, 0u, &both)), was = e->seqb[k];
        e->seqb[k] = (uint8_t)mv_clamp((int32_t)was + step, MV_SQ_MIN[k], MV_SQ_MAX[k]);
        if (both)
            e->seqb[MV_SQ_LENLO] = (uint8_t)mv_clamp((int32_t)e->seqb[MV_SQ_LENLO] + step, MV_SQ_MIN[MV_SQ_LENLO], MV_SQ_MAX[MV_SQ_LENLO]);
        return MV_UI_SETTING;
    }
    if (p >= P_CL && p < P_CL + 4u) {           /* CLIP: the clip (its name comes with it), the cursor, a character,
                                                   the mode */
        if (p == P_CL + 0u) {
            uint32_t was = u->cl_sel;
            u->cl_sel = (uint8_t)mv_clamp((int32_t)u->cl_sel + step, 0, (int32_t)MV_CLIP_ALL - 1);
            if (u->cl_sel != was) clip_name_from(u);
        } else if (p == P_CL + 1u) u->cl_pos = (uint8_t)mv_clamp((int32_t)u->cl_pos + step, 0, MV_CLIP_NAME - 1u);
        else if (p == P_CL + 2u) {              /* ASCII 32..125, as the program names */
            char *ch = &u->cl_name[u->cl_pos % MV_CLIP_NAME];
            *ch = (char)mv_clamp((int32_t)*ch + step, 32, 125);
        } else u->cl_mode = (uint8_t)mv_clamp((int32_t)u->cl_mode + step, 0, 1);
        return 0;
    }
    if (p == P_SQBAR) {                         /* ROLL: the bar shown while stopped, within the track's loop */
        uint32_t nb = mv_seq_len(e->seqb, mv_ui_track(u, e)) / MV_SEQ_BAR;
        u->roll_bar = (uint8_t)mv_clamp((int32_t)u->roll_bar + step, 0, (int32_t)nb - 1);
        return 0;
    }
    if (p == P_SQPITCH) {                       /* ROLL: the rows (fitting stops until the page opens again) */
        u->roll_lo = (uint8_t)mv_clamp((int32_t)u->roll_lo + step, 0, 128 - (int32_t)MV_ROLL_ROWS);
        u->roll_fit = 0;
        return 0;
    }
    if (p == P_SQROOT) {                        /* ROLL: ROOT of the selected track (a byte the ISR reads; none yet:
                                                   from C4). RAM, with the notes: not a setting */
        mv_seq_track_t *T = &e->seq.t[mv_ui_track(u, e)];
        int32_t r = T->root == MV_SEQ_NOROOT ? 60 : T->root;
        T->root = (uint8_t)mv_clamp(r + step, 0, 127);
        return 0;
    }
    if (p >= P_SQ && p < P_SQ + MV_SQ_N) {      /* SEQ: QUANT, MUTE, TRANSPOSE: global, kept in flash */
        uint32_t k = p - P_SQ;
        int32_t v0 = e->seqb[k], v = mv_clamp(v0 + step, MV_SQ_MIN[k], MV_SQ_MAX[k]);
        if (v == v0) return 0;
        e->seqb[k] = (uint8_t)v;
        return MV_UI_SETTING;
    }
    if (p == P_TEMPO) {                         /* global, kept in flash */
        int32_t v0 = e->tempo, v = mv_clamp(v0 + step, TEMPO_MIN, TEMPO_MAX);
        if (v == v0) return 0;
        e->tempo = (uint8_t)v;
        return MV_UI_SETTING;
    }
    if (p >= P_ARP && p < P_ARP + MV_ARP_N) {   /* ARP: global, kept in flash (no JP CC: not sent) */
        uint32_t k = p - P_ARP;
        int32_t v0 = e->arpb[k], v = mv_clamp(v0 + step, MV_ARP_MIN[k], MV_ARP_MAX[k]);
        if (v == v0) return 0;
        e->arpb[k] = (uint8_t)v;
        return MV_UI_SETTING;
    }
    if (p >= P_RV && p < P_RV + MV_RV_N) {      /* REVERB: global, kept in flash (no JP CC: not sent) */
        int32_t top = p == P_RV + MV_RV_FREEZE ? 1 : 127, v0 = e->rvb[p - P_RV], v = mv_clamp(v0 + step, 0, top);
        if (v == v0) return 0;
        e->rvb[p - P_RV] = (uint8_t)v;
        return MV_UI_SETTING;
    }
    {   /* a patch byte, in the part PANEL says (BOTH: each part moved by the same step, from where it is) */
        uint32_t id = MV_PARAM_AT[p];
        const mv_param_t *d = &MV_PARAMS[id];
        int both;
        mv_patch_t *t = ui_part(e, p, &both) == MV_PART_LO ? &e->lower : &e->patch;
        int32_t v0 = mv_param_get(t, id), v = mv_clamp(v0 + step, d->min, d->max);
        if (both) mv_param_set(&e->lower, id, mv_clamp(mv_param_get(&e->lower, id) + step, d->min, d->max));
        if (v == v0) return 0;                  /* at an end: nothing to send */
        mv_param_set(t, id, v);
        if (d->cc != MV_NONE) {                 /* out on the edited part's channel (Kris 7 Nov, 3a: CH UP / CH LO,
                                                   or out_ch where it is GLOBAL); BOTH: LOWER's too, on its own
                                                   channel when that differs (one channel: once) */
            uint32_t cu = part_out_ch(e, t == &e->lower ? MV_PART_LO : MV_PART_UP, out_ch);
            midi_send(0xB0u | cu, d->cc, mv_cc_value(id, v));
            if (both) {
                uint32_t cl = part_out_ch(e, MV_PART_LO, out_ch);
                if (cl != cu) midi_send(0xB0u | cl, d->cc, mv_cc_value(id, mv_param_get(&e->lower, id)));
            }
        }
    }
    return 0;
}

/* the next page with button b after the one shown (looping), or b's first page when another section is shown */
static uint32_t next_page(uint32_t page, uint32_t b)
{
    uint32_t i;
    if (PAGES[page].btn != b) {
        for (i = 1; i < NPAGES; i++) if (PAGES[i].btn == b) return i;
        return page;
    }
    for (i = 1; i < NPAGES; i++)
        if (PAGES[(page + i) % NPAGES].btn == b) return (page + i) % NPAGES;
    return page;
}

static uint32_t page_of(uint32_t b)            /* the first page of button b (0 = none) */
{
    uint32_t i;
    for (i = 1; i < NPAGES; i++) if (PAGES[i].btn == b) return i;
    return 0;
}

/* an overlay closed: the page (or HOME) under it drawn again */
static uint32_t ovl_close(mv_ui_t *u)
{
    u->ovl = MV_OV_NONE;
    u->redraw = 1;
    return MV_UI_PAGE;
}

uint32_t mv_ui_oct(mv_ui_t *u, const mv_engine_t *e, uint32_t pressed, uint32_t released, uint8_t *oct)
{
    const uint32_t DN = 1u << BTN_OCT_DOWN, UP = 1u << BTN_OCT_UP;
    uint32_t r = 0, p = (pressed & DN ? 1u : 0u) | (pressed & UP ? 2u : 0u);
    int single = e->perf[MV_PF_MODE] == MV_KEY_SINGLE;
    if (u->ovl == MV_OV_PART && (pressed & ~(DN | UP))) r |= ovl_close(u);   /* another button: closed */
    if (p) {
        uint32_t held = u->oct_held | p;
        if (u->ovl == MV_OV_PART) {             /* PART SELECT: OCT- LOWER, OCT+ UPPER, both BOTH */
            if (!single) u->panel = (uint8_t)(held == 3u ? MV_UI_BOTH : p & 1u ? MV_UI_LOWER : MV_UI_UPPER);
            u->ovl_t = fm1_ms;
        } else if (held == 3u && MV_PARTS == 2) {   /* the other one while the first is held: PART SELECT opens (one
                                                   part: none; each press steps the octave) */
            if (p != 3u && u->oct_undo) {       /* the first press's octave step undone (both at once: none made) */
                *oct = (uint8_t)(int8_t)((int8_t)*oct - u->oct_undo);
                r |= MV_UI_SETTING;
            }
            u->ovl = MV_OV_PART; u->ovl_t = fm1_ms; u->drawn_ovl = ~0u;
            r |= MV_UI_PAGE;
        } else {                                /* one: the octave at once (-2 .. +2), the step kept for an undo */
            int32_t o = (int8_t)*oct, n = mv_clamp(o + (p & 1u ? -1 : 1), -2, 2);
            u->oct_undo = (int8_t)(n - o);
            if (n != o) { *oct = (uint8_t)(int8_t)n; r |= MV_UI_SETTING; }
        }
        u->oct_held = (uint8_t)held;
    }
    u->oct_held &= (uint8_t)~((released & DN ? 1u : 0u) | (released & UP ? 2u : 0u));
    if (!u->oct_held) u->oct_undo = 0;          /* (both up: the next press is a new one) */
    if (u->ovl == MV_OV_PART && !u->oct_held && fm1_ms - u->ovl_t >= 2000u) r |= ovl_close(u);   /* 2 s: closed */
    return r;
}

uint32_t mv_ui_input(mv_ui_t *u, mv_engine_t *e, uint32_t pressed, uint32_t released, uint8_t *midi_ch,
                     uint32_t out_ch)
{
    uint32_t k, old = u->page, r = 0;
    int32_t d;
    if (u->ovl == MV_OV_CLEAR && ((pressed & ~(1u << BTN_REC)) || fm1_ms - u->ovl_t >= 3000u))
        r |= ovl_close(u);                      /* the CLEAR question: another button, or 3 s: kept */
    if (u->ovl == MV_OV_LONG && (pressed || fm1_ms - u->ovl_t >= 2000u))
        r |= ovl_close(u);                      /* REC refused: any button, or 2 s */
#if MV_SEQ
    /* PLAY (Kris 6 Nov): the transport, both tracks from bar 1; stopping disarms (the ISR ends the notes) */
    if (pressed >> BTN_PLAY & 1u) {
        e->seq_ext = 0;                         /* (the panel's start: free, on TEMPO or the clock's tempo) */
        if (e->seq_run) { e->seq_run = 0; e->seq_arm = 0; }
        else e->seq_run = 1;
    }
    /* REC: a tap arms / disarms the selected track (and starts the transport); held 1 s: CLEAR it? (REC: yes) */
    if (pressed >> BTN_REC & 1u) {
        u->rec_down = 1; u->rec_did = 0; u->rec_t = fm1_ms;
        if (u->ovl == MV_OV_CLEAR) {            /* yes: that track emptied (its notes end) */
            e->seq_clear[u->ovl_track & 1u] = 1;
            u->rec_did = 1;
            r |= ovl_close(u);
        }
    }
    if (u->rec_down && !u->rec_did && fm1_ms - u->rec_t >= 1000u) {
        u->rec_did = 1;
        u->ovl = MV_OV_CLEAR; u->ovl_t = fm1_ms; u->drawn_ovl = ~0u; u->ovl_track = (uint8_t)mv_ui_track(u, e);
        r |= MV_UI_PAGE;
    }
    if (released >> BTN_REC & 1u) {
        uint32_t t = mv_ui_track(u, e);
        if (u->rec_down && !u->rec_did && !(e->seq_arm >> t & 1u) && e->seq.t[t].rom &&
            e->seq.t[t].rom_n > MV_SEQ_NEV) {   /* (Kris 7 Nov, 1a) a factory clip over MV_SEQ_NEV events plays from
                                                   flash but cannot be copied to RAM to record over: refused, said */
            u->ovl = MV_OV_LONG; u->ovl_t = fm1_ms; u->drawn_ovl = ~0u; u->ovl_track = (uint8_t)t;
            r |= MV_UI_PAGE;
        } else if (u->rec_down && !u->rec_did) {
            uint32_t bit = 1u << t;
            e->seq_arm = (uint8_t)(e->seq_arm ^ bit);
            if ((e->seq_arm & bit) && !e->seq_run) { e->seq_ext = 0; e->seq_run = 1; }   /* armed: running */
        }
        u->rec_down = 0;
    }
    if (pressed >> BTN_SEQ & 1u) u->page = (uint8_t)next_page(u->page, BTN_SEQ);   /* SEQ: its page */
#endif                                          /* (MV_SEQ 0: PLAY, REC and SEQ do nothing) */
    if (pressed & (1u << BTN_HOME)) u->page = 0;
    if (pressed >> BTN_SAVE & 1u) {             /* SAVE: its page (from the program playing); on it, do it */
        if (PAGES[u->page].p[0] == P_CL) {      /* (on the CLIP page: the clip's LOAD / SAVE, the page stays) */
            u->cl_act = (uint8_t)(u->cl_mode + 1u);
            r |= MV_UI_CLIP;
        } else if (PAGES[u->page].btn != BTN_SAVE) {
            u->page = (uint8_t)page_of(BTN_SAVE);
            u->sv_to = u->cur; u->sv_pos = 0; u->sv_mode = 0;
        } else {
            u->act = (uint8_t)(MV_SV_WRITE + u->sv_mode); u->act_to = u->sv_to;
            r |= MV_UI_PROG;
            u->page = 0;                        /* done: home (the program's name and number show) */
        }
    }
    /* ARP: a tap switches the arp on / off; held 0.5 s, its pages (the next one if one is shown) */
    if (pressed >> BTN_ARP & 1u) { u->arp_down = 1; u->arp_did = 0; u->arp_t = fm1_ms; }
    if (u->arp_down && !u->arp_did && fm1_ms - u->arp_t >= 500u) {
        u->arp_did = 1;
        u->page = (uint8_t)next_page(u->page, BTN_ARP);
    }
    if (released >> BTN_ARP & 1u) {
        if (u->arp_down && !u->arp_did) { e->arpb[MV_ARP_ON] = (uint8_t)!e->arpb[MV_ARP_ON]; r |= MV_UI_SETTING; }
        u->arp_down = 0;
    }
    for (k = 0; k < 6u; k++)                    /* a section button: its next page (looping), or its first */
        if (pressed >> SECTION_BTN[k] & 1u) u->page = (uint8_t)next_page(u->page, SECTION_BTN[k]);
    if ((d = fm1_enc_take(ENC_ALGORITHM)) != 0)   /* every page in panel order, HOME between the ends */
        u->page = (uint8_t)(((int32_t)u->page + d % (int32_t)NPAGES + (int32_t)NPAGES) % (int32_t)NPAGES);
    if (u->page != old) {
        uint32_t c;
        u->redraw = 1; u->last = 0xFF;
        if (PAGES[u->page].p[0] == P_SQBAR) u->roll_fit = 1;   /* ROLL opened: its rows fit the notes */
        for (c = 0; c < 4u; c++) (void)fm1_enc_take(ENC_KNOB1 + c);   /* nothing turned meanwhile lands here */
        return r | MV_UI_PAGE;
    }
    for (k = 0; k < 4u; k++) {                  /* KNOB1..4; a fast turn (detents < 40 ms apart) x 4, not choices */
        uint32_t p = PAGES[u->page].p[k];
        if ((d = fm1_enc_take(ENC_KNOB1 + k)) == 0) continue;
        if (u->ovl) r |= ovl_close(u);          /* a knob turned: the overlay goes, the page edits */
        if ((IS_PATCH(p) || p == P_TEMPO || (p >= P_RV && p < P_RV + MV_RV_N) || (p >= P_ARP && p < P_ARP + MV_ARP_N))
            && !choice_name(p, 0) && fm1_ms - u->t_knob[k] < 40u)
            d *= 4;                             /* numbers only: not choices, MIDI CH or VOICES */
        u->t_knob[k] = fm1_ms;
        r |= edit(u, e, k, d, midi_ch, out_ch);
    }
    if ((d = fm1_enc_take(ENC_SELECT)) != 0 && u->last < 4u)   /* SELECT: single steps (decision 6) */
        r |= edit(u, e, u->last, d, midi_ch, out_ch);
    return r;
}

/* ---- drawing: the header once a page, then a column or the edit line only when what it shows changed */

#define HEAD_H 24u                              /* the header: title, page n/m, the button that reaches it */
#define COL_Y 26u                               /* the columns: 4 x 60 px wide, 190 high (148 while the meter and
                                                   the status took the foot: they are on DEBUG now, Kris 3 Nov) */
#define COL_H 190u                              /* (a column, 60 x 190 = 11400 px, fits the canvas: CV_MAX) */
_Static_assert(60u * COL_H <= CV_MAX, "a column fits the canvas (platform.c's CV_MAX: 240 x 64 px)");
#define LINE_Y 220u                             /* the edit line: "FILTER CUTOFF 96" for 3 s (y 220-235) */

/* the part(s) a page edits in a layered program ("U", "L", "L+U"; PANEL), else 0: the pages of patch bytes
 * that are per part (not FX: the one chain; not GLO, ARP, SAVE) */
static const char *page_tag(const mv_ui_t *u, const mv_engine_t *e, const ui_page_t *pg)
{
    static const char *const TAG[3] = {"U", "L", "L+U"};
    int patch_page = pg->btn != BTN_FX && pg->btn != BTN_ARP && pg->btn != BTN_SAVE &&
                     !(pg->btn == BTN_GLO && !IS_PATCH(pg->p[0]));   /* (PLAY MODE is the part's; PERFORM, GLOBAL not) */
    return patch_page && e->perf[MV_PF_MODE] != MV_KEY_SINGLE ? TAG[u->panel % 3u] : 0;
}
static uint32_t tag_key(const mv_ui_t *u, const mv_engine_t *e, const ui_page_t *pg)
{
    const char *t = page_tag(u, e, pg);
    return t ? 1u + (uint32_t)(t[0] == 'L') + (uint32_t)(t[1] == '+') : 0u;
}

/* tag: the part(s) the page edits ("L", "U", "L+U") in a layered program, else 0 */
static void draw_header(const ui_page_t *pg, const char *tag)
{
    int32_t x;
    cv_begin(240, HEAD_H, C_BLACK);
    x = cv_text(6, 3, &FONT_S, pg->title, C_HI);
    if (pg->nsub > 1u) {
        char sub[4] = {(char)('1' + pg->sub), '/', (char)('0' + pg->nsub), 0};
        x = cv_text(x + 8, 3, &FONT_S, sub, C_AMB);
    }
    if (tag) cv_text(x + 8, 3, &FONT_S, tag, C_WHITE);   /* (the part: white, it changes what the knobs edit) */
    cv_text(234 - text_w(&FONT_S, btn_name(pg->btn)), 3, &FONT_S, btn_name(pg->btn), C_DIM);
    cv_rect(0, (int32_t)HEAD_H - 2, 240, 1, C_LINE);
    cv_blit(0, 0);
}

/* FONT_L (16 x 32) has ASCII 32..95 only: no lower case ("dB" -> FONT_S) */
static int fits_large(const char *s)
{
    uint32_t i;
    for (i = 0; s[i]; i++) if (s[i] > 95 || i >= 3u) return 0;
    return 1;
}

static void draw_column(uint32_t col, const char *label, const char *val, uint32_t mode, int32_t fill, int sel)
{
    const int32_t w = 60;
    cv_begin(60, COL_H, C_BLACK);
    if (col) cv_rect(0, 4, 1, (int32_t)COL_H - 8, C_LINE);   /* a rule between the columns */
    if (*label) cv_text((w - text_w(&FONT_S, label)) / 2, 2, &FONT_S, label, sel ? C_WHITE : C_GRAY);
    if (*val) {
        const felucca_font_t *f = fits_large(val) ? &FONT_L : &FONT_S;
        cv_text((w - text_w(f, val)) / 2, f == &FONT_L ? 22 : 30, f, val, sel ? C_WHITE : C_HI);
    }
    if (mode != BAR_NONE) {                     /* the bar: an outline, filled from the bottom or the middle */
        const int32_t bx = 21, by = 62, bw = 18, bh = (int32_t)COL_H - 66, in = bh - 4;
        uint16_t c = sel ? C_HI : C_AMB;
        cv_rect(bx, by, bw, 1, C_LINE); cv_rect(bx, by + bh - 1, bw, 1, C_LINE);
        cv_rect(bx, by, 1, bh, C_LINE); cv_rect(bx + bw - 1, by, 1, bh, C_LINE);
        if (mode == BAR_UP) {
            int32_t h = fill * in / 1000;
            if (h > 0) cv_rect(bx + 2, by + 2 + in - h, bw - 4, h, c);
        } else {
            int32_t mid = by + bh / 2, h = (fill < 0 ? -fill : fill) * (in / 2) / 1000;
            cv_rect(bx + 1, mid, bw - 2, 1, C_DIM);
            if (h > 0) cv_rect(bx + 2, fill < 0 ? mid + 1 : mid - h, bw - 4, h, c);
        }
    }
    cv_blit(col * 60u, COL_Y);
}

/* a key of what a column shows (FNV-1a over the text, the bar, the highlight): redrawn only when it changes */
static uint32_t show_key(const char *a, const char *b, int32_t fill, uint32_t extra)
{
    uint32_t h = 2166136261u;
    for (; *a; a++) h = (h ^ (uint8_t)*a) * 16777619u;
    h = (h ^ 0xFFu) * 16777619u;
    for (; *b; b++) h = (h ^ (uint8_t)*b) * 16777619u;
    return (h ^ (uint32_t)fill) * 16777619u ^ extra;
}

void mv_ui_draw(mv_ui_t *u, const mv_engine_t *e, uint32_t midi_ch)
{
    const ui_page_t *pg = &PAGES[u->page];
    uint32_t k, key, mode;
    int32_t fill;
    char val[8], line[32];
    int fresh = fm1_ms - u->t_touch < 3000u;    /* the last edit is still highlighted / on the line */
    if (!u->redraw && tag_key(u, e, pg) != u->drawn_tag) {   /* the mode or PANEL moved: the header's tag */
        draw_header(pg, page_tag(u, e, pg));
        u->drawn_tag = tag_key(u, e, pg);
    }
    if (u->redraw) {
        draw_header(pg, page_tag(u, e, pg));
        u->drawn_tag = tag_key(u, e, pg);
        for (k = 0; k < 4u; k++) u->drawn[k] = ~0u;
        u->drawn_line = ~0u;
        u->redraw = 0;
    }
    if (pg->p[0] == P_DEBUG || pg->p[0] == P_SQBAR) return;   /* DEBUG, ROLL: the header only; mvance.c draws the
                                                   rest (mv_debug.c, mv_roll.c) */
    for (k = 0; k < 4u; k++) {
        uint32_t p = pg->p[k];
        int sel = fresh && u->last == k;
        const char *label = p == P_NONE ? "" : col_label(e, p);
        if (p == P_NONE) { val[0] = 0; mode = BAR_NONE; fill = 0; }
        else col_value(e, p, midi_ch, val, &mode, &fill);
        key = show_key(label, val, fill, mode << 1 | (uint32_t)sel);
        if (key != u->drawn[k]) { draw_column(k, label, val, mode, fill, sel); u->drawn[k] = key; u->drawn_sv = ~0u; }
    }
    if (pg->btn == BTN_SAVE) {                  /* the SAVE page's two lines, over the columns (a column redraw
                                                   clears them: drawn_sv is reset then) */
        static const char *const WHAT[3] = {"OVER ", "SWAP ", "RESET "};
        char a[32], b[32];
        const uint8_t *nm = e->patch.b, *tn;
        uint32_t i, j = 0, t = u->sv_to;
        for (i = 0; i < 5u; i++) a[j++] = "NAME "[i];   /* the name as edited, the cursor's character in [ ] */
        for (i = 0; i < MV_PROG_NAME; i++) {
            if (i == u->sv_pos) a[j++] = '[';
            a[j++] = (char)(nm[i] >= 32u && nm[i] <= 126u ? nm[i] : '?');
            if (i == u->sv_pos) a[j++] = ']';
        }
        a[j] = 0;
        tn = u->sv_mode == 2u ? MV_FACTORY[t] : mv_prog_get(u->prog, t)->b;   /* what TO holds (or will) */
        for (i = 0, j = 0; WHAT[u->sv_mode][i]; i++) b[j++] = WHAT[u->sv_mode][i];
        b[j++] = (char)('0' + (t + 1u) / 10u); b[j++] = (char)('0' + (t + 1u) % 10u); b[j++] = ' ';
        for (i = 0; i < MV_PROG_NAME; i++) b[j++] = (char)(tn[i] >= 32u && tn[i] <= 126u ? tn[i] : '?');
        b[j] = 0;
        key = show_key(a, b, 0, u->sv_mode);
        if (key != u->drawn_sv) {
            draw_text_box(0, COL_Y + 76u, 240, &FONT_S, a, C_HI, 1);
            draw_text_box(0, COL_Y + 104u, 240, &FONT_S, b, C_AMB, 1);
            u->drawn_sv = key;
        }
    }
#if MV_SEQ
    if (pg->p[0] == P_CL) {                     /* the CLIP page's two lines (as SAVE's): the name, what SAVE will do
                                                   (or what the last one came to, for 2 s) */
        static const char TRK[2] = {'U', 'L'};   /* the track letters */
        char a[32], b[40];
        const mv_clip_info_t *d = u->clips && u->cl_sel < MV_CLIP_N ? &u->clips->dir[u->cl_sel] : 0;
        const mv_fclip_t *f = mv_clip_factory(u->cl_sel);
        uint32_t i, j = 0, k = u->cl_sel + 1u, t = mv_ui_track(u, e), msg = fm1_ms - u->cl_msg_t < 2000u ? u->cl_msg : 0u;
        const char *w;
        if (f) {                                /* a factory clip: its name (not editable) */
            for (i = 0; f->name[i] && i < 16u; i++) a[j++] = f->name[i];
        } else {
            for (i = 0; i < 5u; i++) a[j++] = "NAME "[i];   /* the name as edited, the cursor's character in [ ] */
            for (i = 0; i < MV_CLIP_NAME; i++) {
                if (i == u->cl_pos) a[j++] = '[';
                a[j++] = u->cl_name[i];
                if (i == u->cl_pos) a[j++] = ']';
            }
        }
        a[j] = 0;
        j = 0;
        if (msg == MV_CLM_BUSY) w = "ANOTHER SAVE WAITS";
        else if (msg == MV_CLM_RO || (f && u->cl_mode == MV_CL_SAVE)) w = "FACTORY: LOAD ONLY";
        else if (f && !msg) w = "LOAD F";
        else if (u->clips && u->clips->err) w = "WRITE FAILED";
        else if (u->clips && u->clips->pend == u->cl_sel) w = "WRITTEN AT STOP";
        else if (msg == MV_CLM_LOADED) w = "LOADED > ";
        else if (msg == MV_CLM_EMPTY) w = "EMPTY: NOTHING";
        else if (msg == MV_CLM_LONG) w = "TOO LONG TO SAVE";
        else if (u->cl_mode == MV_CL_LOAD) w = d && d->used ? "LOAD " : "EMPTY ";
        else w = "SAVE ";
        for (; *w; w++) b[j++] = *w;
        if (msg == MV_CLM_LOADED) b[j++] = TRK[t];
        else if (f) {                           /* LOAD F5 > L (SAVE: the line above says it cannot) */
            if (!msg && u->cl_mode == MV_CL_LOAD) {
                uint32_t fk = u->cl_sel - MV_CLIP_N + 1u;
                if (fk >= 10u) b[j++] = (char)('0' + fk / 10u);
                b[j++] = (char)('0' + fk % 10u);
                b[j++] = ' '; b[j++] = '>'; b[j++] = ' '; b[j++] = TRK[t];
            }
        } else if (!msg && !(u->clips && (u->clips->err || u->clips->pend == u->cl_sel))) {
            if (u->cl_mode == MV_CL_SAVE) { b[j++] = TRK[t]; b[j++] = ' '; b[j++] = '>'; b[j++] = ' '; }
            b[j++] = (char)('0' + k / 10u); b[j++] = (char)('0' + k % 10u);
            if (d && d->used) {                 /* its name (SAVE: the one it replaces) */
                b[j++] = ' ';
                for (i = 0; i < MV_CLIP_NAME; i++) b[j++] = d->name[i];
                if (u->cl_mode == MV_CL_LOAD) { b[j++] = ' '; b[j++] = '>'; b[j++] = ' '; b[j++] = TRK[t]; }
            }
        }
        b[j] = 0;                               /* (at most 5 + 4 + 2 + 7 + 4 = 22) */
        key = show_key(a, b, (int32_t)msg, u->cl_mode);
        if (key != u->drawn_sv) {
            draw_text_box(0, COL_Y + 76u, 240, &FONT_S, a, C_HI, 1);
            draw_text_box(0, COL_Y + 104u, 240, &FONT_S, b, C_AMB, 1);
            u->drawn_sv = key;
        }
    }
#endif
    line[0] = 0;                                /* the edit line: "FILTER CUTOFF 96" */
    if (fresh && u->last < 4u) {
        const char *a;
        uint32_t i = 0, p = pg->p[u->last];
        col_value(e, p, midi_ch, val, &mode, &fill);
        for (a = pg->title; *a; a++) line[i++] = *a;
        line[i++] = ' ';
        for (a = col_label(e, p); *a; a++) line[i++] = *a;
        line[i++] = ' ';
        for (a = val; *a; a++) line[i++] = *a;   /* (<= 10 + 1 + 7 + 1 + 7: fits 32) */
        line[i] = 0;
    }
    key = show_key(line, "", 0, 0);
    if (key != u->drawn_line) { draw_text_box(0, LINE_Y, 240, &FONT_S, line, C_AMB, 1); u->drawn_line = key; }
}

/* the overlay (PART SELECT, the CLEAR question): a box over the page, drawn only when what it shows changed */
void mv_ui_overlay_draw(mv_ui_t *u, const mv_engine_t *e)
{
    static const char *const PART[3] = {"UPPER", "LOWER", "BOTH"};
    const char *title, *val, *hint;
    uint32_t key;
    if (u->ovl == MV_OV_PART) {
        int single = e->perf[MV_PF_MODE] == MV_KEY_SINGLE;
        title = "PART SELECT";
        val = single ? "SINGLE" : PART[u->panel % 3u];
        hint = single ? "ONE PART ONLY" : "OCT- L  OCT+ U  BOTH L+U";
    } else if (u->ovl == MV_OV_LONG) {
        title = "TOO LONG TO RECORD";
        val = u->ovl_track == MV_PART_LO ? "LOWER" : "UPPER";
        hint = "OVER 128 NOTES: PLAY ONLY";
    } else {
        title = "CLEAR TRACK?";
        val = u->ovl_track == MV_PART_LO ? "LOWER" : "UPPER";
        hint = "REC CLEARS  OTHER KEEPS";
    }
    key = show_key(title, val, (int32_t)u->ovl, 0);
    if (key == u->drawn_ovl) return;
    u->drawn_ovl = key;
    cv_begin(240, 64, C_BLACK);                 /* (FONT_L: upper case only, as these are) */
    cv_rect(0, 0, 240, 1, C_LINE); cv_rect(0, 63, 240, 1, C_LINE);
    cv_text((240 - text_w(&FONT_S, title)) / 2, 4, &FONT_S, title, C_GRAY);
    cv_text((240 - text_w(&FONT_L, val)) / 2, 26, &FONT_L, val, C_WHITE);
    cv_blit(0, 72);
    cv_begin(240, 20, C_BLACK);
    cv_text((240 - text_w(&FONT_S, hint)) / 2, 2, &FONT_S, hint, C_AMB);
    cv_blit(0, 144);
}

#ifdef DINGHY_HOST
/* ---- host only: the page table as JSON (tests/run_host.sh with MV_DUMP_PAGES=file; tools/gen_cheatsheet.py).
 * The cheat sheet is made from this, so it shows the panel exactly as the firmware has it: the pages in their
 * order, the button that reaches each, what KNOB1..4 edit, their ranges, choice names and CCs. Never in the
 * firmware (the device build has no DINGHY_HOST) */
static void dump_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) { if (*s == '"' || *s == '\\') fputc('\\', f); fputc(*s, f); }
    fputc('"', f);
}

/* the choice names of p over min..max, as a JSON list; 0 when p is a number */
static int dump_choices(FILE *f, uint32_t p, int32_t lo, int32_t hi)
{
    int32_t v;
    if (!choice_name(p, (uint32_t)lo)) return 0;
    fputs(", \"choices\": [", f);
    for (v = lo; v <= hi; v++) { if (v > lo) fputs(", ", f); dump_str(f, choice_name(p, (uint32_t)v)); }
    fputc(']', f);
    return 1;
}

static void mv_ui_dump(const char *path, mv_engine_t *e)
{
    static const uint8_t WAVE_OF[4] = {30, 30, 33, 33};   /* OSC1 CTRL1/2, OSC2 CTRL1/2: the wave byte their label follows */
    FILE *f = fopen(path, "w");
    uint32_t i, k, w;
    if (!f) return;
    fputs("{\"pages\": [\n", f);
    for (i = 0; i < NPAGES; i++) {              /* (HOME too: its knobs) */
        const ui_page_t *g = &PAGES[i];
        fprintf(f, "  {\"title\": "); dump_str(f, g->title);
        fprintf(f, ", \"button\": "); dump_str(f, btn_name(g->btn));
        fprintf(f, ", \"sub\": %u, \"nsub\": %u, \"knobs\": [", g->sub + 1u, g->nsub);
        for (k = 0; k < 4u; k++) {
            uint32_t p = g->p[k];
            int32_t lo = 0, hi = 127, centre = -1;
            const char *kind = "number";
            if (k) fputs(", ", f);
            if (p == P_NONE || p == P_DEBUG) { fputs("null", f); continue; }
            fprintf(f, "\n    {\"p\": %u, \"label\": ", p);
            dump_str(f, i == 0u ? HOME_LABEL[k] : col_label(e, p));
            if (IS_PATCH(p)) {
                const mv_param_t *d = &MV_PARAMS[MV_PARAM_AT[p]];
                lo = d->min; hi = d->max;
                if (d->disp == MV_D_SIGNED || d->disp == MV_D_CENT || d->disp == MV_D_RANGE) { kind = "signed"; centre = d->centre; }
                fprintf(f, ", \"byte\": %u, \"global\": false", p);
                if (d->cc != MV_NONE) fprintf(f, ", \"cc\": %u", d->cc);
                if (p == 31u || p == 32u || p == 37u || p == 38u) {   /* the label follows the wave */
                    uint32_t wb = WAVE_OF[(p == 31u ? 0u : p == 32u ? 1u : p == 37u ? 2u : 3u)];
                    uint8_t keep = e->patch.b[wb];
                    const mv_param_t *wd = &MV_PARAMS[MV_PARAM_AT[wb]];
                    fputs(", \"per_wave\": {", f);
                    for (w = wd->min; w <= wd->max; w++) {
                        e->patch.b[wb] = (uint8_t)w;
                        if (w > wd->min) fputs(", ", f);
                        dump_str(f, choice_name(wb, w)); fputs(": ", f); dump_str(f, col_label(e, p));
                    }
                    fputc('}', f);
                    e->patch.b[wb] = keep;
                }
            } else {
                fprintf(f, ", \"global\": %s", (p >= P_PF && p < P_PF + MV_PF_N) || p == P_LSYNC || p == P_CSYNC ||
                        p == P_DSYNC ||
                        (p >= P_SQBAR && p <= P_SQROOT) || (p >= P_CL && p < P_CL + 4u) ? "false" : "true");   /* (PERFORM, SYNC: the program's; ROLL's
                                                   view and ROOT: neither the program's nor kept) */
                if (p == P_MIDICH) { lo = 0; hi = 16; }
                else if (p == P_VOICES) { lo = 1; hi = (int32_t)MV_NV; }
                else if (p == P_TEMPO) { lo = TEMPO_MIN; hi = TEMPO_MAX; }
                else if (IS_MIDI(p)) { lo = 0; hi = 1; }
                else if (p >= P_ARP && p < P_ARP + MV_ARP_N) { lo = MV_ARP_MIN[p - P_ARP]; hi = MV_ARP_MAX[p - P_ARP]; }
                else if (p == P_RV + MV_RV_FREEZE) { lo = 0; hi = 1; }
                else if (p == P_SV + 0u) { lo = 1; hi = MV_NPROG; }
                else if (p == P_SV + 1u) { lo = 1; hi = MV_PROG_NAME; }
                else if (p == P_SV + 2u) { lo = 32; hi = 125; kind = "char"; }
                else if (p == P_SV + 3u) { lo = 0; hi = 2; }
                else if (p == P_PANEL) { lo = 0; hi = 2; }
                else if (p == P_LSYNC) { lo = 0; hi = (int32_t)MV_LFO1_SYNC_N - 1; }
                else if (p == P_CSYNC) { lo = 0; hi = (int32_t)MV_CSYNC_N - 1; }
                else if (p == P_DSYNC) { lo = 0; hi = (int32_t)MV_DSYNC_N - 1; }
                else if (p == P_SQLEN) { lo = 1; hi = (int32_t)MV_SEQ_MAXBAR; }
                else if (p == P_SQBAR) { lo = 1; hi = (int32_t)MV_SEQ_MAXBAR; }
                else if (p == P_SQPITCH) { lo = 0; hi = 128 - (int32_t)MV_ROLL_ROWS; }
                else if (p == P_SQROOT) { lo = 0; hi = 127; }
                else if (p == P_CL + 0u) { lo = 1; hi = MV_CLIP_N; }   /* (the user clips: the factory ones
                                                   are local to a build, not the public sheet's) */
                else if (p == P_CL + 1u) { lo = 1; hi = MV_CLIP_NAME; }
                else if (p == P_CL + 2u) { lo = 32; hi = 125; kind = "char"; }
                else if (p == P_CL + 3u) { lo = 0; hi = 1; }
                else if (p >= P_SQ && p < P_SQ + MV_SQ_N) { lo = MV_SQ_MIN[p - P_SQ]; hi = MV_SQ_MAX[p - P_SQ]; }
                else if (p >= P_PF && p < P_PF + MV_PF_N) {
                    static const uint8_t TOP[MV_PF_N] = {2, 127, 127, 6, 16, 16, 3, MV_LFO1_SYNC_N - 1u,
                                                         MV_LFO1_SYNC_N - 1u, MV_DSYNC_N - 1u, MV_DSYNC_N - 1u,
                                                         MV_CSYNC_N - 1u, MV_CSYNC_N - 1u};
                    lo = 0; hi = TOP[p - P_PF];
                }
            }
            if (dump_choices(f, p, lo, hi)) kind = "choice";
            fprintf(f, ", \"kind\": \"%s\", \"min\": %d, \"max\": %d", kind, (int)lo, (int)hi);
            if (centre >= 0) fprintf(f, ", \"centre\": %d", (int)centre);
            fputc('}', f);
        }
        fprintf(f, "]}%s\n", i + 1u < NPAGES ? "," : "");
    }
    fprintf(f, "], \"programs\": %u, \"voices\": %u}\n", (unsigned)MV_NPROG, (unsigned)MV_NV);
    fclose(f);
}
#endif
