/* SPDX-License-Identifier: GPL-3.0-only */
/* mid2bank: a JP-8000 / JP-8080 bank (a .syx, or a .mid carrying the SysEx) -> firmware/gen/mv_factory.h, the
 * M-vance factory programs. Runs on the PC; it parses each DT1 message with the firmware's own mv_sysex_parse
 * (checksum, framing, the JP-8000 two-message form, the range clamp), so the factory set is byte for byte what the
 * device itself would read from that SysEx.
 *   cc -std=c99 -O2 -Ifirmware/src -Ifirmware/gen tools/mid2bank.c -o build/mid2bank
 *   build/mid2bank docs/presets/Alan_Marcero_Trance_A.mid > firmware/gen/mv_factory.h
 * Every user patch address 02 bb pp 00 in the file lands at program pp (0..63); the rest of the 64 stay INIT.
 * Performances (03 pp xx, Kris 3 Nov: layer / split) land at program pp too: the common block's name, KEY MODE
 * (0x10), SPLIT POINT (0x11) and BALANCE (0x13), the upper patch (03 pp 40/41) and the lower one (03 pp 42/43),
 * each parsed by the same mv_sysex_parse (the address moved to the temporary patch's, the checksum made again).
 * Their parts' LFO1 SYNC (03 pp 10 / 11, offset 5: the JP's part parameter, Kris 5 Nov) goes to MV_PF_SYNCUP / LO,
 * their DELAY SYNC (offset 4) to MV_PF_DSYNUP / LO and CHORUS SYNC RATE (offset 6) to MV_PF_CSYNUP / LO (Kris 7 Nov).
 * A program's name is its upper patch's bytes 0..15: a performance's name goes there. A program with no lower
 * patch gets a copy of its upper one (a single switched to DUAL then doubles its own sound).
 * An optional second argument names the list the file was made from (tools/make_bank.py passes its list), for the
 * header's comment. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mv_patch.c"

#define NPROG 64
#define NPERF 16                                /* MV_PF_N: the performance bytes a program stores */
static mv_patch_t bank[NPROG], lower[NPROG];
static uint8_t com[NPROG][NPERF], pname[NPROG][16];
static int got[NPROG], got_lo[NPROG], got_name[NPROG], bad_sum, bad_frame, not_ours, clamped[NPROG];
static const uint8_t PERF_DEF[NPERF] = {0, 60, 64, 3, 0, 0, 0};   /* MV_PERF_DEF (mv_voice.c): SINGLE, 60, 64, 4+4 */

static uint8_t rsum(const uint8_t *a, uint32_t n)   /* the Roland checksum of address + data */
{
    uint32_t s = 0, i;
    for (i = 0; i < n; i++) s += a[i];
    return (uint8_t)((128u - (s & 127u)) & 127u);
}

/* a performance's message (03 pp xx yy): its common block's JP bytes, or one of its two patches */
static void take_perf(const uint8_t *msg, uint32_t n)
{
    static uint8_t m[4096];
    uint32_t pp = msg[7], area = msg[8], off = msg[9], i, len;
    int32_t no = -1;
    mv_patch_t tmp, *dst;
    int r;
    if (pp >= NPROG || n < 12u || msg[n - 1u] != 0xF7u) { not_ours++; return; }
    if (rsum(msg + 6, n - 8u) != msg[n - 2u]) { bad_sum++; return; }
    len = n - 12u;
    if (area == 0x00u) {                        /* common: name 0..15, KEY MODE 0x10, SPLIT 0x11, BALANCE 0x13 */
        for (i = 0; i < len; i++) {
            uint32_t a = off + i, v = msg[10u + i] & 127u;
            if (a < 16u) { pname[pp][a] = (uint8_t)v; got_name[pp] = 1; }
            else if (a == 0x10u) com[pp][0] = (uint8_t)(v > 2u ? 0u : v);
            else if (a == 0x11u) com[pp][1] = (uint8_t)v;
            else if (a == 0x13u) com[pp][2] = (uint8_t)v;
        }
        return;
    }
    if (area == 0x70u) {                        /* M-vance's own: the 16 performance bytes (mv_xfer.h), as they are */
        for (i = 0; i < len && off + i < NPERF; i++) com[pp][off + i] = (uint8_t)(msg[10u + i] & 127u);
        if (com[pp][0] > 2u) com[pp][0] = 0;
        return;
    }
    if (area == 0x10u || area == 0x11u) {       /* a part (upper / lower): its syncs are ours (offsets 4, 5, 6) */
        static const uint8_t TO[3] = {9u, 7u, 11u}, TOP[3] = {11u, 23u, 24u};   /* DELAY, LFO1, CHORUS: byte, count */
        for (i = 0; i < len; i++)
            if (off + i >= 4u && off + i <= 6u) {
                uint32_t k = off + i - 4u, v = msg[10u + i] & 127u;
                com[pp][TO[k] + (area & 1u)] = (uint8_t)(v < TOP[k] ? v : 0u);   /* (out of range: OFF) */
            }
        return;
    }
    if (area < 0x40u || area > 0x43u || n > sizeof m) { not_ours++; return; }   /* (08: not ours) */
    dst = area < 0x42u ? &bank[pp] : &lower[pp];
    for (i = 0; i < n; i++) m[i] = msg[i];      /* as the temporary patch (01 00 40/41 yy): the parser's own area */
    m[6] = 0x01u; m[7] = 0x00u; m[8] = (uint8_t)(0x40u | (area & 1u));
    m[n - 2u] = rsum(m + 6, n - 8u);
    tmp = *dst;
    r = mv_sysex_parse(m, n, &tmp, &no);
    if (r == MV_SX_OK) { *dst = tmp; if (area < 0x42u) got[pp] = 1; else got_lo[pp] = 1; }
    else if (r == MV_SX_BAD_SUM) bad_sum++;
    else if (r == MV_SX_BAD_FRAME) bad_frame++;
    else not_ours++;
}

static void take(const uint8_t *msg, uint32_t n)     /* msg: F0 .. F7 */
{
    int32_t no = -2;
    mv_patch_t tmp;
    int r;
    if (n < 12 || msg[0] != 0xF0) return;
    if (msg[1] == 0x41 && msg[4] == 0x06 && msg[5] == 0x12 && msg[6] == 0x03) { take_perf(msg, n); return; }
    /* the parser writes into the patch it is given: find the program first, by its rule (addr_offset: user area
     * 02 bb pp xx, patch = bb x 64 + pp / 2; the odd page pp + 1 holds the patch's bytes 128..247) */
    no = (msg[6] == 0x02 && msg[7] <= 1) ? (int32_t)((uint32_t)msg[7] << 6 | (uint32_t)msg[8] >> 1) : -1;
    if (no < 0 || no >= NPROG) { not_ours++; return; }
    tmp = bank[no];
    r = mv_sysex_parse(msg, n, &tmp, &no);
    if (r == MV_SX_OK && no >= 0 && no < NPROG) { bank[no] = tmp; got[no] = 1; }
    else if (r == MV_SX_BAD_SUM) bad_sum++;
    else if (r == MV_SX_BAD_FRAME) bad_frame++;
    else not_ours++;
}

static uint32_t vlq(const uint8_t *b, uint32_t *i)
{
    uint32_t v = 0, c;
    do { c = b[(*i)++]; v = v << 7 | (c & 127u); } while (c & 128u);
    return v;
}

int main(int argc, char **argv)
{
    static uint8_t d[1 << 20], msg[4096];
    FILE *f;
    size_t n;
    uint32_t i, k;
    int p, total = 0;
    if (argc < 2 || !(f = fopen(argv[1], "rb"))) { fprintf(stderr, "usage: mid2bank bank.mid|bank.syx\n"); return 2; }
    n = fread(d, 1, sizeof d, f);
    fclose(f);
    for (p = 0; p < NPROG; p++) { mv_patch_init(&bank[p]); mv_patch_init(&lower[p]); memcpy(com[p], PERF_DEF, NPERF); }
    if (n > 14 && !memcmp(d, "MThd", 4)) {      /* a Standard MIDI File: the SysEx events of every track */
        i = 8 + (uint32_t)(d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7]);
        while (i + 8 <= n) {
            uint32_t len = (uint32_t)(d[i + 4] << 24 | d[i + 5] << 16 | d[i + 6] << 8 | d[i + 7]), j = i + 8, end = j + len;
            uint8_t rs = 0;
            if (memcmp(d + i, "MTrk", 4)) break;
            while (j < end) {
                uint32_t ln;
                uint8_t s;
                (void)vlq(d, &j);               /* delta time */
                s = d[j];
                if (s == 0xF0 || s == 0xF7) {   /* SysEx: F0 len data (the data ends with F7) */
                    j++; ln = vlq(d, &j);
                    if (s == 0xF0 && ln + 1u <= sizeof msg) { msg[0] = 0xF0; memcpy(msg + 1, d + j, ln); take(msg, ln + 1u); }
                    j += ln;
                } else if (s == 0xFF) {         /* meta */
                    j += 2; ln = vlq(d, &j); j += ln;
                } else {                        /* channel messages (running status) */
                    if (s & 0x80u) { rs = s; j++; }
                    j += ((rs & 0xF0u) == 0xC0u || (rs & 0xF0u) == 0xD0u) ? 1u : 2u;
                }
            }
            i = end;
        }
    } else {                                    /* a .syx: F0 .. F7 one after another */
        for (i = 0; i < n; i++)
            if (d[i] == 0xF0) { for (k = i; k < n && d[k] != 0xF7; k++) {} if (k < n) take(d + i, k - i + 1u); i = k; }
    }
    for (p = 0; p < NPROG; p++) {
        if (got_name[p]) memcpy(bank[p].b, pname[p], 16);   /* a performance's name is the program's */
        if (!got_lo[p]) lower[p] = bank[p];     /* no lower patch: a copy of the upper */
        clamped[p] = (int)mv_patch_sanitize(&bank[p]) + (int)mv_patch_sanitize(&lower[p]);
        total += got[p];
    }
    fprintf(stderr, "mid2bank: %d programs read, %d bad checksums, %d bad frames, %d messages not ours\n", total, bad_sum,
            bad_frame, not_ours);
    printf("/* generated by tools/mid2bank.c from %s%s%s: do not edit. The M-vance factory programs (64 JP-8080\n"
           " * patches, 248 bytes each, as the JP stores them; program n = user patch n of the file). %d read from the file. */\n",
           argv[1], argc > 2 ? ", made by tools/make_bank.py from " : "", argc > 2 ? argv[2] : "", total);
    printf("#pragma once\n#include <stdint.h>\n#define MV_NPROG %d\n", NPROG);
    printf("static const uint8_t MV_FACTORY[MV_NPROG][%u] = {\n", (unsigned)MV_PATCH_BYTES);
    for (p = 0; p < NPROG; p++) {
        printf("    {   /* %2d %.16s%s */\n", p + 1, (const char *)bank[p].b, got[p] ? "" : " (not in the file: INIT)");
        for (k = 0; k < MV_PATCH_BYTES; k++) printf("%s%3u,%s", k % 24u ? "" : "        ", bank[p].b[k], k % 24u == 23u || k + 1u == MV_PATCH_BYTES ? "\n" : " ");
        printf("    },\n");
    }
    printf("};\n");
    printf("/* each program's LOWER patch (DUAL / SPLIT; a copy of the upper where the file has none) */\n");
    printf("static const uint8_t MV_FACTORY_LO[MV_NPROG][%u] = {\n", (unsigned)MV_PATCH_BYTES);
    for (p = 0; p < NPROG; p++) {
        printf("    {   /* %2d %.16s%s */\n", p + 1, (const char *)lower[p].b, got_lo[p] ? "" : " (= the upper)");
        for (k = 0; k < MV_PATCH_BYTES; k++) printf("%s%3u,%s", k % 24u ? "" : "        ", lower[p].b[k], k % 24u == 23u || k + 1u == MV_PATCH_BYTES ? "\n" : " ");
        printf("    },\n");
    }
    printf("};\n");
    printf("/* each program's performance bytes (MV_PF_*: KEY MODE, SPLIT, BALANCE, VOICES, CH LO / UP, ARP ..) */\n");
    printf("static const uint8_t MV_FACTORY_PERF[MV_NPROG][%u] = {\n", NPERF);
    for (p = 0; p < NPROG; p++) {
        printf("    {");
        for (k = 0; k < NPERF; k++) printf("%u%s", com[p][k], k + 1u < NPERF ? ", " : "");
        printf("},%s\n", com[p][0] ? (com[p][0] == 1u ? "   /* DUAL */" : "   /* SPLIT */") : "");
    }
    printf("};\n");
    for (p = 0; p < NPROG; p++) if (clamped[p]) fprintf(stderr, "  program %d: %d values out of range, clamped\n", p + 1, clamped[p]);
    return 0;
}
