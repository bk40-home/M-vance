/* SPDX-License-Identifier: GPL-3.0-only */
/* M-vance effects: implementation (chain, laws, memory and CPU in mv_fx.h). Each block cites the JT FxChain code it
 * ports (Jteensy8000 src/core/dsp/FxChain.cpp / .h); tools/compare/cmp_fx.cpp checks it against that code. */
#include "mv_fx.h"
#include "mv_fixed.h"

#if defined(MV_FS) && MV_FX_FS != MV_FS
#error "mv_fx.c: MV_FX_FS must equal MV_FS (the engine's rate) in the firmware"
#endif

#define ONE 1048576                             /* 1.0 in the chain's Q20 */

/* ---- the lines (the platform pool: zeroed at boot, outside the 96 KB of RAM) */
static int32_t mline[MV_FX_MLEN] __attribute__((section(".pool")));      /* Multi-FX: x, Q20 */
static int16_t dline[2][MV_FX_DLEN] __attribute__((section(".pool")));   /* delay L / R: Q13 (+-4.0) */

/* ---- small helpers: control rate only (type / time changes), never per frame */

/* TPT one-pole gain G = g / (1 + g), g = tan(pi fc / fs), Q31 (FxChain.cpp tptOnePoleG). tan by its series to the
 * x^11 term: fc <= 7 kHz at 44.1 kHz is x <= 0.4987, error < 5e-6 */
static uint32_t tpt_g(uint32_t fc)
{
    const uint64_t PI_Q29 = 1686629713u;       /* pi x 2^29 */
    int64_t x = (int64_t)mv_udiv64_32((uint64_t)fc * PI_Q29 * 4u, MV_FX_FS);   /* pi fc / fs, Q31 */
    int64_t x2 = (x * x) >> 31, t;
    /* Horner, Q31: 1/3, 2/15, 17/315, 62/2835, 1382/155925 */
    t = 19032325;                               /* 1382/155925 x 2^31 */
    t = 46965498 + ((x2 * t) >> 31);            /* 62/2835 */
    t = 115898620 + ((x2 * t) >> 31);           /* 17/315 */
    t = 286331153 + ((x2 * t) >> 31);           /* 2/15 */
    t = 715827883 + ((x2 * t) >> 31);           /* 1/3 */
    t = 2147483648ll + ((x2 * t) >> 31);        /* 1 */
    t = (x * t) >> 31;                          /* tan x, Q31 */
    return mv_udiv64_32((uint64_t)t << 31, (uint32_t)(2147483648ll + t));
}

/* a duration in microseconds as samples, Q16 */
static int32_t us_q16(uint32_t us) { return (int32_t)mv_udiv64_32(((uint64_t)us * MV_FX_FS) << 16, 1000000u); }

/* ---- DISTORTION: the JT drive stage (FxChain.h applyDrive, FxChain.cpp computeDrive / computeDriveMakeup),
 * Amount a = LEVEL / 127:
 *   1. pre low shelf: x - 0.5 a LP140(x)
 *   2-4. biased algebraic sigmoid u / sqrt(1 + u^2), u = gain x + bias (gain 0.5 x 2^(6a), bias 0.35 a), with
 *        first-order ADAA in its closed form  y = (u + u1) / (F(u) + F(u1)) - f(bias),  F(u) = sqrt(1 + u^2)
 *   5. post top cut, blended in by a:  y += a (LP7k(y) - y)
 *   6. DC blocker, R = 1 - 2 pi 8 / fs
 *   7. makeup (computeDriveMakeup: the loudness of a -10 dBFS sine held)
 * The amount's constants per LEVEL come from DR_LVL (python, the JT's float32 formulas). F(u) = |u| + r(|u|),
 * r(t) = sqrt(1 + t^2) - t: a table to t = 8 (step 1/32, error < 4e-5), 1/(2t) - 1/(8t^3) past it */
static const int32_t DR_LVL[128][4] = {         /* by LEVEL: gain (Q16), bias (Q20), f(bias) (Q20), makeup (Q16) */
    {  32768,      0,      0, 132171}, {  33859,   2890,   2890, 127986}, {  34986,   5780,   5779, 123941}, {  36151,   8669,   8669, 120031},
    {  37354,  11559,  11558, 116251}, {  38598,  14449,  14448, 112598}, {  39882,  17339,  17336, 109067}, {  41210,  20228,  20225, 105655},
    {  42582,  23118,  23113, 102356}, {  43999,  26008,  26000,  99169}, {  45464,  28898,  28887,  96088}, {  46978,  31788,  31773,  93111},
    {  48541,  34677,  34658,  90234}, {  50157,  37567,  37543,  87453}, {  51827,  40457,  40427,  84767}, {  53552,  43347,  43310,  82171},
    {  55335,  46236,  46192,  79663}, {  57177,  49126,  49072,  77239}, {  59080,  52016,  51952,  74898}, {  61047,  54906,  54831,  72636},
    {  63079,  57796,  57708,  70451}, {  65179,  60685,  60584,  68340}, {  67349,  63575,  63459,  66302}, {  69591,  66465,  66332,  64333},
    {  71908,  69355,  69203,  62432}, {  74301,  72244,  72074,  60596}, {  76775,  75134,  74942,  58823}, {  79331,  78024,  77809,  57111},
    {  81971,  80914,  80674,  55459}, {  84700,  83804,  83537,  53864}, {  87520,  86693,  86399,  52325}, {  90433,  89583,  89258,  50840},
    {  93444,  92473,  92115,  49406}, {  96554,  95363,  94971,  48024}, {  99769,  98252,  97824,  46690}, { 103090, 101142, 100675,  45403},
    { 106522, 104032, 103524,  44162}, { 110068, 106922, 106370,  42966}, { 113732, 109812, 109214,  41813}, { 117518, 112701, 112056,  40701},
    { 121430, 115591, 114895,  39630}, { 125472, 118481, 117732,  38597}, { 129649, 121371, 120566,  37603}, { 133965, 124260, 123397,  36645},
    { 138425, 127150, 126226,  35723}, { 143033, 130040, 129051,  34836}, { 147794, 132930, 131874,  33981}, { 152714, 135819, 134694,  33159},
    { 157798, 138709, 137511,  32368}, { 163051, 141599, 140325,  31607}, { 168478, 144489, 143136,  30876}, { 174087, 147379, 145944,  30173},
    { 179882, 150268, 148749,  29497}, { 185870, 153158, 151550,  28848}, { 192058, 156048, 154348,  28225}, { 198451, 158938, 157143,  27626},
    { 205058, 161827, 159934,  27051}, { 211884, 164717, 162722,  26500}, { 218937, 167607, 165506,  25971}, { 226225, 170497, 168287,  25463},
    { 233756, 173387, 171064,  24977}, { 241538, 176276, 173837,  24510}, { 249578, 179166, 176607,  24063}, { 257887, 182056, 179372,  23635},
    { 266472, 184946, 182134,  23225}, { 275342, 187835, 184892,  22832}, { 284508, 190725, 187646,  22456}, { 293979, 193615, 190397,  22097},
    { 303765, 196505, 193143,  21752}, { 313878, 199395, 195884,  21423}, { 324326, 202284, 198622,  21107}, { 335123, 205174, 201356,  20806},
    { 346279, 208064, 204085,  20518}, { 357806, 210954, 206810,  20242}, { 369717, 213843, 209531,  19978}, { 382025, 216733, 212247,  19726},
    { 394742, 219623, 214959,  19485}, { 407883, 222513, 217666,  19255}, { 421461, 225403, 220369,  19035}, { 435491, 228292, 223067,  18825},
    { 449988, 231182, 225760,  18624}, { 464968, 234072, 228449,  18431}, { 480446, 236962, 231133,  18248}, { 496440, 239851, 233813,  18072},
    { 512966, 242741, 236487,  17904}, { 530042, 245631, 239157,  17744}, { 547687, 248521, 241822,  17590}, { 565919, 251411, 244482,  17443},
    { 584758, 254300, 247136,  17303}, { 604224, 257190, 249786,  17169}, { 624338, 260080, 252431,  17040}, { 645122, 262970, 255071,  16917},
    { 666598, 265859, 257705,  16799}, { 688788, 268749, 260335,  16687}, { 711717, 271639, 262959,  16579}, { 735410, 274529, 265578,  16476},
    { 759891, 277419, 268191,  16377}, { 785187, 280308, 270799,  16282}, { 811325, 283198, 273402,  16191}, { 838334, 286088, 276000,  16104},
    { 866241, 288978, 278592,  16021}, { 895078, 291867, 281178,  15941}, { 924874, 294757, 283759,  15865}, { 955662, 297647, 286335,  15791},
    { 987476, 300537, 288904,  15721}, {1020348, 303427, 291469,  15653}, {1054315, 306316, 294027,  15588}, {1089412, 309206, 296580,  15526},
    {1125678, 312096, 299127,  15466}, {1163150, 314986, 301669,  15409}, {1201871, 317875, 304204,  15354}, {1241880, 320765, 306734,  15302},
    {1283221, 323655, 309258,  15251}, {1325939, 326545, 311776,  15203}, {1370078, 329434, 314289,  15157}, {1415687, 332324, 316795,  15113},
    {1462814, 335214, 319295,  15070}, {1511510, 338104, 321789,  15030}, {1561827, 340994, 324278,  14991}, {1613818, 343883, 326760,  14954},
    {1667542, 346773, 329236,  14918}, {1723053, 349663, 331706,  14885}, {1780412, 352553, 334170,  14852}, {1839680, 355442, 336628,  14822},
    {1900922, 358332, 339080,  14792}, {1964202, 361222, 341525,  14764}, {2029589, 364112, 343964,  14738}, {2097152, 367002, 346398,  14713},
};
static const int32_t DR_R[258] = {              /* r(i / 32), Q20 (python: round((sqrt(1 + t^2) - t) * 2^20)) */
    1048576, 1016320, 985086, 954870, 925664, 897459, 870241, 843995, 818703, 794347,
    770904, 748351, 726664, 705817, 685785, 666540, 648056, 630304, 613257, 596888,
    581171, 566078, 551583, 537661, 524288, 511439, 499091, 487222, 475810, 464835,
    454277, 444116, 434334, 424915, 415841, 407097, 398668, 390539, 382696, 375128,
    367821, 360763, 353944, 347353, 340980, 334815, 328850, 323075, 317483, 312066,
    306816, 301727, 296791, 292003, 287356, 282845, 278464, 274209, 270073, 266053,
    262144, 258342, 254642, 251041, 247535, 244121, 240795, 237554, 234395, 231315,
    228312, 225382, 222523, 219733, 217009, 214349, 211751, 209212, 206732, 204307,
    201937, 199620, 197353, 195135, 192965, 190841, 188762, 186726, 184733, 182780,
    180867, 178993, 177156, 175355, 173590, 171858, 170160, 168495, 166861, 165258,
    163684, 162139, 160623, 159134, 157672, 156236, 154825, 153439, 152077, 150739,
    149423, 148130, 146858, 145608, 144378, 143169, 141980, 140809, 139658, 138524,
    137409, 136312, 135231, 134167, 133120, 132088, 131072, 130071, 129086, 128114,
    127158, 126215, 125286, 124370, 123467, 122577, 121700, 120835, 119982, 119141,
    118312, 117493, 116686, 115890, 115105, 114329, 113565, 112810, 112065, 111329,
    110604, 109887, 109180, 108481, 107792, 107111, 106438, 105774, 105118, 104470,
    103829, 103197, 102572, 101955, 101344, 100742, 100146, 99557, 98975, 98399,
    97830, 97268, 96712, 96163, 95619, 95082, 94550, 94024, 93505, 92990,
    92482, 91979, 91481, 90988, 90501, 90019, 89542, 89070, 88603, 88141,
    87684, 87231, 86783, 86339, 85900, 85465, 85035, 84609, 84187, 83770,
    83356, 82946, 82541, 82139, 81741, 81348, 80957, 80571, 80188, 79809,
    79433, 79061, 78692, 78327, 77965, 77606, 77251, 76899, 76550, 76204,
    75861, 75521, 75185, 74851, 74520, 74192, 73867, 73545, 73225, 72909,
    72595, 72284, 71975, 71669, 71365, 71064, 70766, 70470, 70177, 69886,
    69597, 69311, 69027, 68745, 68466, 68189, 67914, 67641, 67371, 67102,
    66836, 66572, 66310, 66050, 65792, 65536, 65282, 65030,
};
static uint32_t dr_pre_g, dr_post_g, dr_dc_r;  /* Q31, set by mv_fx_init (rate constants) */

static inline int32_t dr_f(int32_t u)          /* sqrt(1 + u^2), Q20 */
{
    uint32_t t = (uint32_t)(u < 0 ? -u : u), i, fr;
    if (t < (8u << 20)) {
        i = t >> 15; fr = t & 0x7FFFu;          /* step 1/32 */
        return (int32_t)t + DR_R[i] + (int32_t)(((int64_t)(DR_R[i + 1] - DR_R[i]) * fr) >> 15);
    } else {
        int32_t r1 = (int32_t)mv_udiv64_32(1ull << 39, t);   /* 1 / (2t), Q20 */
        int32_t r3 = (int32_t)(((((int64_t)r1 * r1) >> 20) * r1) >> 20);   /* (1 / (2t))^3 = 1 / (8t^3) */
        return (int32_t)t + r1 - r3;
    }
}

static inline int32_t drive_one(mv_fx_t *f, int32_t x)   /* x, y: Q20 */
{
    int32_t v, lp, u, fu, y, dc;
    int64_t num;
    uint32_t q, den;
    v = mv_mul_q31ef(x - f->dr_pre, dr_pre_g, &f->dr_e[0]); lp = f->dr_pre + v; f->dr_pre = lp + v;   /* 1 */
    x -= (int32_t)(((int64_t)f->dr_pdep * lp) >> 16);
    u = (int32_t)(((int64_t)mv_clamp(x, -(1 << 25), 1 << 25) * f->dr_gain) >> 16) + f->dr_bias;   /* 2: |u| <= 1024 */
    fu = dr_f(u);
    num = (int64_t)u + f->dr_u1;                /* 3: ADAA, (u + u1) / (F + F1), |y| < 1 */
    den = (uint32_t)fu + (uint32_t)f->dr_f1;
    q = mv_udiv64_32((uint64_t)(num < 0 ? -num : num) << 20, den);
    y = (num < 0 ? -(int32_t)q : (int32_t)q) - f->dr_bout;                             /* 4 */
    f->dr_u1 = u; f->dr_f1 = fu;
    v = mv_mul_q31ef(y - f->dr_post, dr_post_g, &f->dr_e[1]); lp = f->dr_post + v; f->dr_post = lp + v;   /* 5 */
    y += (int32_t)(((int64_t)f->dr_mix * (lp - y)) >> 16);
    dc = y - f->dr_dcx + mv_mul_q31ef(f->dr_dcy, dr_dc_r, &f->dr_e[2]);                      /* 6 */
    f->dr_dcx = y; f->dr_dcy = dc;
    return (int32_t)(((int64_t)dc * f->dr_mk) >> 16);                                  /* 7 */
}

void mv_fx_drive(mv_fx_t *f, int32_t *bus, uint32_t n)
{
    uint32_t i;
    if (!f->on_drive) return;
    for (i = 0; i < n; i++)                     /* Q15 in, Q15 out (Q20 inside) */
        bus[i] = (drive_one(f, mv_clamp(bus[i], -(1 << 25), 1 << 25) << 5) + 16) >> 5;
}

/* ---- MULTI-FX: the JP pair engine (FxChain.cpp kJpModTypes, prepareJpMod, processModulationJp) */
enum { MFX_TRI, MFX_SINE, MFX_UNI, MFX_STATIC };
typedef struct {
    uint16_t c_us, d_us;                        /* tap centre c, depth D (us) */
    uint8_t shape;
    uint16_t rate_mhz;                          /* LFO rate (mHz) */
    int32_t g, g3, fb;                          /* Q16 */
    uint16_t hp_hz;
    uint8_t poles;
} mfx_type_t;
static const mfx_type_t MFX_TYPES[12] = {      /* kJpModTypes verbatim (ms -> us, Hz -> mHz, gains Q16) */
    { 21800,  7900, MFX_TRI,      70,  51773,       0,      0,    70, 2},   /* SUPER CHORUS SLW */
    {  7150,  1310, MFX_TRI,     420,  51970,       0,      0,    70, 2},   /* SUPER CHORUS MID */
    { 14790,   200, MFX_SINE,   4040,  48824,       0,      0,   400, 1},   /* SUPER CHORUS FST */
    { 21900,  4400, MFX_TRI,     140,  57147,       0,      0,  1100, 1},   /* SUPER CHORUS CLR (provisional on the JT) */
    {  2030,   580, MFX_TRI,     315,  76218,   53936,      0,  1100, 1},   /* FLANGER SLOW */
    {  2010,  1280, MFX_TRI,     357,  67961,   60490,      0,   100, 1},   /* FLANGER DEEP */
    {  1800,   250, MFX_SINE,   1680,  60293,   52888,      0,   560, 2},   /* FLANGER FAST */
    {   300,   105, MFX_UNI,     180,  74973,       0,  22938,  1000, 1},   /* DEEP PHASING SLW */
    {  9810,  8100, MFX_TRI,      80,  58786,  -52494,      0,   400, 1},   /* JET PHASING */
    { 17660,   240, MFX_TRI,     167,  54657,  -54657,      0,   560, 1},   /* TWISTING */
    { 12135,   485, MFX_STATIC,     0,  49480,       0,      0,  1500, 1},   /* FREEZE PHASE 1 */
    { 35340,   490, MFX_STATIC,     0,  47055,       0,      0,  1500, 1},   /* FREEZE PHASE 2 */
};

static void mfx_build(mv_fx_t *f, uint32_t type)   /* prepareJpMod: a type change (and the LFO / high-pass restart) */
{
    const mfx_type_t *t = &MFX_TYPES[type];
    f->m_type = (uint8_t)type;
    f->m_c = us_q16(t->c_us);
    f->m_d = us_q16(t->d_us);
    f->m_c2 = mv_clamp(2 * f->m_c, 0, f->m_cap);
    f->m_shape = t->shape;
    f->m_inc = t->shape == MFX_STATIC ? 0u : mv_udiv64_32((uint64_t)t->rate_mhz << 32, 1000u * MV_FX_FS);
    f->m_ph = 0;
    f->m_g = t->g; f->m_g3 = t->g3; f->m_fb = t->fb;
    f->m_poles = t->poles;
    f->m_hpg = tpt_g(t->hp_hz);
    f->m_hs[0] = f->m_hs[1] = 0;
    f->m_he[0] = f->m_he[1] = 0;
    f->m_lastw = 0;
}

/* the LFO (FxChain.cpp jpTri, FastMath.h fastSin01), phase Q32 -> -1..+1 in Q30 */
static inline int32_t mfx_tri(uint32_t ph)
{
    return ph < 0x80000000u ? (int32_t)ph - (1 << 30) : (int32_t)(0xC0000000u - ph);   /* -1 + 4p / 3 - 4p */
}
static inline int32_t mfx_sin(uint32_t ph)    /* the parabola + one refinement pass (-60 dB THD), negated */
{
    int32_t x = (int32_t)(ph >> 1) - (1 << 30), y, yy;                  /* 2p - 1, Q30 */
    y = 4 * (x - (int32_t)(((int64_t)x * (x < 0 ? -x : x)) >> 30));   /* 4 (x - x|x|) */
    yy = (int32_t)(((int64_t)y * (y < 0 ? -y : y)) >> 30) - y;         /* y|y| - y */
    return -(y + (int32_t)(((int64_t)yy * 241591910) >> 30));          /* 0.225 x that + y, negated */
}

/* a read d samples (Q16) behind the write index (modReadTap: linear, the newer sample is i0 + 1) */
static inline int32_t mfx_tap(uint32_t w, int32_t d)
{
    int32_t pos = (int32_t)(w << 16) - d, a, b;
    uint32_t i0, i1;
    if (pos < 0) pos += (int32_t)(MV_FX_MLEN << 16);
    i0 = (uint32_t)pos >> 16;
    i1 = i0 + 1u == MV_FX_MLEN ? 0u : i0 + 1u;
    a = mline[i0]; b = mline[i1];
    return a + (int32_t)(((int64_t)(b - a) * (pos & 0xFFFF)) >> 16);
}

static inline int32_t mfx_one(mv_fx_t *f, int32_t x)   /* x: Q20 in; returns W after the high-pass, Q20 */
{
    int32_t d, w, a;
    uint32_t s;
    mline[f->m_w] = f->m_fb ? x + (int32_t)(((int64_t)f->m_fb * f->m_lastw) >> 16) : x;   /* write first */
    if (f->m_shape == MFX_STATIC) d = f->m_d;
    else {
        int32_t lfo;
        f->m_ph += f->m_inc;
#ifdef MV_FX_CMP_PHASE
        f->m_ph = MV_FX_CMP_PHASE(f->m_ph);     /* compare builds only (tools/compare/cmp_fx.cpp): the JT's float phase */
#endif
        lfo = f->m_shape == MFX_SINE ? mfx_sin(f->m_ph) : mfx_tri(f->m_ph);
        d = f->m_shape == MFX_UNI ? (int32_t)(((int64_t)f->m_d * ((1 << 30) + lfo)) >> 31)   /* 0..D */
                                  : (int32_t)(((int64_t)f->m_d * lfo) >> 30);                 /* -D..+D */
    }
    a = mfx_tap(f->m_w, mv_clamp(f->m_c + d, 0, f->m_cap)) - mfx_tap(f->m_w, mv_clamp(f->m_c - d, 0, f->m_cap));
    w = (int32_t)(((int64_t)f->m_g * a) >> 16);
    if (f->m_g3) w += (int32_t)(((int64_t)f->m_g3 * mfx_tap(f->m_w, f->m_c2)) >> 16);   /* the fixed 2c tap */
    f->m_lastw = w;                             /* the loop takes W before the high-pass */
    for (s = 0; s < f->m_poles; s++) {          /* hp = x - lp, TPT one-poles */
        int32_t v = mv_mul_q31ef(w - f->m_hs[s], f->m_hpg, &f->m_he[s]), lp = f->m_hs[s] + v;
        f->m_hs[s] = lp + v;
        w -= lp;
    }
    f->m_w = f->m_w + 1u == MV_FX_MLEN ? 0u : f->m_w + 1u;
    return w;
}

/* ---- DELAY (FxChain.cpp kDelayPresets, setDelayTime, prepareDelay, processDelay; FxChain.h slewTap;
 *      the JP import law JP8000SyxImporter.cpp kDelayTypeMap / Step 5) */
static const uint16_t DLY_MAX_MS[5] = {625, 625, 156, 156, 1250};   /* by JP type byte: the TIME knob's top (manual) */
static const uint8_t DLY_RATIO[5][2] = {       /* the JT preset's R / L tap ratio, by JP type byte (kDelayTypeMap) */
    {25, 12},                                   /* PANNING L->R -> PAN_LR 120 / 250 ms: R = L x 250/120 (confirmed) */
    {12, 25},                                   /* PANNING R->L -> PAN_RL 250 / 120 */
    {2, 1},                                     /* PANNING SHORT -> PAN_STEREO 200 / 400 */
    {1, 1},                                     /* MONO SHORT */
    {1, 1},                                     /* MONO LONG */
};
#define DLY_STEP_MAX 819                        /* kTimeSlewMaxStep 0.2 samples a sample, Q12 */
#define DLY_STEP_MIN 41                         /* kSlewMinStep 0.01, Q12 */
#define DLY_CAP ((int32_t)((MV_FX_DLEN - 2u) << 12))

static inline int32_t dly_slew(int32_t cur, int32_t tgt, uint32_t coef)   /* slewTap, Q12 */
{
    int32_t delta = tgt - cur, step;
    if (!delta) return tgt;
    step = (int32_t)(((int64_t)delta * coef) >> 31);
    step = mv_clamp(step, -DLY_STEP_MAX, DLY_STEP_MAX);
    if (step >= 0) { if (step < DLY_STEP_MIN) step = DLY_STEP_MIN; }
    else if (step > -DLY_STEP_MIN) step = -DLY_STEP_MIN;
    if ((step >= 0 && step >= delta) || (step < 0 && step <= delta)) return tgt;   /* arrived */
    return cur + step;
}

static inline int32_t dly_tap(const int16_t *l, uint32_t w, int32_t d)   /* d samples (Q12) behind w; Q20 out */
{
    int32_t pos = (int32_t)(w << 12) - d, a, b;
    uint32_t i0, i1;
    if (pos < 0) pos += (int32_t)(MV_FX_DLEN << 12);
    i0 = (uint32_t)pos >> 12;
    i1 = i0 + 1u == MV_FX_DLEN ? 0u : i0 + 1u;
    a = (int32_t)l[i0] << 7; b = (int32_t)l[i1] << 7;   /* Q13 -> Q20 */
    return a + (int32_t)(((int64_t)(b - a) * (pos & 0xFFF)) >> 12);
}

/* Q20 -> the line's Q13, held to +-4.0 (3a: was Q14, +-2.0). Truncated toward zero, not rounded: with feedback a rounded line keeps a
 * 1-2 LSB value going round for ever (0.79 x 2 LSB rounds back to 2: a limit cycle at -78 dBFS, so the tail never
 * ends and the engine never rests); toward zero every pass shrinks it to 0 */
static inline int16_t dly_q13(int32_t v)
{
    v = v >= 0 ? v >> 7 : -((-v) >> 7);
    return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

static void dly_times(mv_fx_t *f, uint32_t type, uint32_t time)   /* setDelayTime with the import law */
{
    uint32_t num = time * DLY_MAX_MS[type];     /* ms x 127 */
    int32_t l;
    if (num < 10u * 127u) num = 10u * 127u;     /* kMinDelayMs 10 */
    l = (int32_t)mv_udiv64_32((uint64_t)num * MV_FX_FS * 4096u + 63500u, 127000u);   /* samples, Q12 */
    f->d_tl = mv_clamp(l, 1 << 12, DLY_CAP);
    f->d_tr = mv_clamp((int32_t)mv_udiv64_32((uint64_t)l * DLY_RATIO[type][0] + DLY_RATIO[type][1] / 2u,
                                             DLY_RATIO[type][1]), 1 << 12, DLY_CAP);
    if (f->d_cl < 0) { f->d_cl = f->d_tl; f->d_cr = f->d_tr; }   /* the first time: no glide in from nothing */
}

static void dly_mute(mv_fx_t *f)               /* the old audio in the lines never plays: one tap's depth of input */
{
    int32_t deep = f->d_tl > f->d_tr ? f->d_tl : f->d_tr;
    f->d_mute = ((uint32_t)deep >> 12) + 2u;
}

/* ---- set-up */

void mv_fx_init(mv_fx_t *f)
{
    const uint64_t TWO_PI_Q28 = 1686629713u;   /* 2 pi x 2^28 */
    uint32_t k;
    for (k = 0; k < sizeof *f; k++) ((uint8_t *)f)[k] = 0;
    for (k = 0; k < 6u; k++) f->key[k] = 0xFF;  /* build everything on the first mv_fx_set */
    f->m_cap = (int32_t)((MV_FX_MLEN - 2u) << 16);
    f->m_type = 0xFF;
    f->d_cl = f->d_cr = -1;
    f->quiet = ~0u >> 1;                        /* nothing ringing */
    /* the one-pole slew of the delay tap: 1 - exp(-1 / (0.15 fs)) = x - x^2 / 2, x = 20 / (3 fs) (Q31) */
    {
        uint32_t x = mv_udiv64_32((20ull << 31) + 3u * MV_FX_FS / 2u, 3u * MV_FX_FS);
        f->d_slew = x - (uint32_t)(((uint64_t)x * x) >> 32);
    }
    dr_pre_g = tpt_g(140u);                     /* kDrivePreHz */
    dr_post_g = tpt_g(7000u);                   /* kDrivePostHz */
    dr_dc_r = 0x80000000u - mv_udiv64_32(8u * TWO_PI_Q28 * 8u, MV_FX_FS);   /* 1 - 2 pi 8 / fs */
}

void mv_fx_set(mv_fx_t *f, const uint8_t *b)
{
    uint32_t mtype = b[61] > 12u ? 0u : b[61], mlvl = b[62] & 127u, dtype = b[63] > 4u ? 0u : b[63];
    uint32_t dtime = b[64] & 127u, dfbk = b[65] & 127u, dlvl = b[66] & 127u;
    int was_drive = f->on_drive, was_dly = f->on_dly;
    if (f->key[0] == mtype && f->key[1] == mlvl && f->key[2] == dtype && f->key[3] == dtime && f->key[4] == dfbk &&
        f->key[5] == dlvl)
        return;                                 /* nothing moved: no work (the usual case) */
    /* MULTI-FX / DISTORTION (setModEffect / setModMix; the importer's byte 12 = drive 100 %, mod off) */
    f->on_drive = (uint8_t)(mtype == 12u && mlvl);   /* LEVEL = the drive Amount; 0 = bypass (applyDrive) */
    f->on_mfx = (uint8_t)(mtype < 12u && mlvl);
    f->dr_gain = DR_LVL[mlvl][0]; f->dr_bias = DR_LVL[mlvl][1]; f->dr_bout = DR_LVL[mlvl][2]; f->dr_mk = DR_LVL[mlvl][3];
    if (f->on_drive && !was_drive) {            /* off -> on: clean filters, ADAA history at rest = the bias */
        f->dr_pre = f->dr_post = f->dr_dcx = f->dr_dcy = 0;   /* (resetDriveState: the first output is then 0) */
        f->dr_e[0] = f->dr_e[1] = f->dr_e[2] = 0;
        f->dr_u1 = f->dr_bias; f->dr_f1 = dr_f(f->dr_bias);
    }
    f->dr_pdep = (int32_t)((mlvl * 32768u + 63u) / 127u);   /* 0.5 a */
    f->dr_mix = (int32_t)((mlvl * 65536u + 63u) / 127u);    /* a */
    if (mtype < 12u && mtype != f->m_type) mfx_build(f, mtype);
    f->m_wet = (int32_t)((mlvl * mlvl * 65536u + 8064u) / 16129u);   /* (level / 127)^2, Q16 (the JP's square law) */
    /* DELAY */
    f->on_dly = (uint8_t)(dlvl != 0u);
    if (dtype != f->key[2] || dtime != f->key[3]) dly_times(f, dtype, dtime);
    if (f->on_dly && (!was_dly || dtype != f->key[2])) dly_mute(f);   /* a type change (setDelayEffect) or a start */
    f->d_fb = (int32_t)((dfbk * 65536u + 63u) / 127u);
    if (f->d_fb > 64881) f->d_fb = 64881;       /* 0.99 */
    f->d_wet = (int32_t)((dlvl * 65536u + 63u) / 127u);   /* a send: the dry stays at unity (1b) */
    f->key[0] = (uint8_t)mtype; f->key[1] = (uint8_t)mlvl; f->key[2] = (uint8_t)dtype;
    f->key[3] = (uint8_t)dtime; f->key[4] = (uint8_t)dfbk; f->key[5] = (uint8_t)dlvl;
}

int mv_fx_ringing(const mv_fx_t *f)
{
    return (f->on_mfx | f->on_dly | f->on_drive) && f->quiet < MV_FX_DLEN + MV_FX_MLEN;
}

/* ---- the chain after TONE */

#define QUIET 64                                /* 2 LSB of the Q15 out: below it the tail counts as silent */

void mv_fx_out(mv_fx_t *f, const int32_t *bus, int32_t *out, uint32_t n)
{
    uint32_t i;
    if (!(f->on_drive | f->on_mfx | f->on_dly)) {   /* no stage: the bus to both sides, as before the effects */
        for (i = 0; i < n; i++) { out[2u * i] += bus[i]; out[2u * i + 1u] += bus[i]; }
        return;
    }
    for (i = 0; i < n; i++) {
        int32_t x = mv_clamp(bus[i], -(1 << 25), 1 << 25) << 5, l = x, r = x, loud = x < 0 ? -x : x, a;
        if (f->on_mfx) {                        /* MULTI-FX: L = x + wet W, R = x - wet W */
            int32_t w = mfx_one(f, x), wet = (int32_t)(((int64_t)f->m_wet * w) >> 16);
            l = x + wet; r = x - wet;
            a = w < 0 ? -w : w; if (a > loud) loud = a;
        }
        if (f->on_dly) {                        /* DELAY: read, write in + feedback x read, out in + wet x read (a send) */
            int32_t dl, dr;
            f->d_cl = dly_slew(f->d_cl, f->d_tl, f->d_slew);
            f->d_cr = dly_slew(f->d_cr, f->d_tr, f->d_slew);
            dl = dly_tap(dline[0], f->d_w, f->d_cl);
            dr = dly_tap(dline[1], f->d_w, f->d_cr);
            if (f->d_mute) {                    /* fresh lines: write the input only, play dry */
                dline[0][f->d_w] = dly_q13(l); dline[1][f->d_w] = dly_q13(r);
                f->d_mute--;
            } else {
                dline[0][f->d_w] = dly_q13(l + (int32_t)(((int64_t)f->d_fb * dl) >> 16));
                dline[1][f->d_w] = dly_q13(r + (int32_t)(((int64_t)f->d_fb * dr) >> 16));
                l += (int32_t)(((int64_t)f->d_wet * dl) >> 16);
                r += (int32_t)(((int64_t)f->d_wet * dr) >> 16);
                a = dl < 0 ? -dl : dl; if (a > loud) loud = a;
                a = dr < 0 ? -dr : dr; if (a > loud) loud = a;
            }
            f->d_w = f->d_w + 1u == MV_FX_DLEN ? 0u : f->d_w + 1u;
        }
        /* (no limiter here any more: the engine's FX limiter, an mv_lim, takes this output: 1a) */
        out[2u * i] += (l + 16) >> 5;           /* Q20 -> Q15 */
        out[2u * i + 1u] += (r + 16) >> 5;
        f->quiet = loud > QUIET ? 0u : f->quiet + 1u;
    }
}
