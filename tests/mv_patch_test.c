/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of firmware/src/mv_patch.c and the generated parameter table.
 *   cc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware/src -Ifirmware/gen -o build/mv_patch_test tests/mv_patch_test.c
 * Checks: the table is consistent, every parameter round-trips its whole range, the 2-byte morph encoding, the
 * MODE2 CC map, DT1 SysEx out and back in byte for byte, checksum and frame errors, a JP-8000 two-message dump. */
#include <stdio.h>
#include <string.h>
#include "mv_patch.c"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_table(void)
{
    uint32_t id, i, seen[MV_PATCH_BYTES] = {0};
    for (id = 0; id < MV_NPARAMS; id++) {
        const mv_param_t *d = &MV_PARAMS[id];
        CHECK(d->min <= d->def && d->def <= d->max, "param %u default outside range", (unsigned)id);
        CHECK(d->size == 1 || d->size == 2, "param %u size %u", (unsigned)id, d->size);
        CHECK(d->size == 2 || d->max <= 127, "param %u: 1 byte but max %u", (unsigned)id, d->max);
        CHECK(strlen(d->label) <= 7, "param %u label too long", (unsigned)id);
        CHECK(MV_PARAM_AT[d->off] == id, "MV_PARAM_AT[%u] != %u", d->off, (unsigned)id);
        for (i = 0; i < d->size; i++) seen[d->off + i]++;
        if (d->disp == MV_D_ENUM) CHECK(d->names != 0, "param %u enum without names", (unsigned)id);
        if (id) CHECK(MV_PARAMS[id - 1].off < d->off, "table not in byte order at %u", (unsigned)id);
    }
    for (i = MV_NAME_LEN; i < MV_PATCH_BYTES; i++)
        CHECK(seen[i] <= 1, "byte %u in two parameters", (unsigned)i);
    CHECK(MV_PARAMS[MV_P_CUTOFF].off == 41 && MV_PARAMS[MV_P_OSC1_WAVE].off == 30, "known offsets");
    CHECK(MV_PARAMS[MV_P_CUTOFF].cc == 74 && MV_PARAMS[MV_P_RESO].cc == 71, "known CCs");
    CHECK(MV_PARAMS[MV_P_OSC2_WAVE].max == 3 && strcmp(MV_PARAMS[MV_P_OSC2_WAVE].names[3], "NOISE") == 0,
          "OSC2 wave 3 is NOISE");
}

static void test_get_set(void)
{
    mv_patch_t p;
    uint32_t id;
    int32_t v;
    mv_patch_init(&p);
    CHECK(memcmp(p.b, "INIT PATCH      ", 16) == 0, "init name");
    for (id = 0; id < MV_NPARAMS; id++) {
        const mv_param_t *d = &MV_PARAMS[id];
        CHECK(mv_param_get(&p, id) == d->def, "param %u init %d != def %u", (unsigned)id,
              (int)mv_param_get(&p, id), d->def);
        for (v = d->min; v <= d->max; v++) {
            mv_param_set(&p, id, v);
            if (mv_param_get(&p, id) != v) { CHECK(0, "param %u: set %d got %d", (unsigned)id, (int)v,
                                                   (int)mv_param_get(&p, id)); break; }
        }
        mv_param_set(&p, id, -1000);
        CHECK(mv_param_get(&p, id) == d->min, "param %u clamps low", (unsigned)id);
        mv_param_set(&p, id, 1000);
        CHECK(mv_param_get(&p, id) == d->max, "param %u clamps high", (unsigned)id);
        mv_param_set(&p, id, d->def);
    }
    /* 2-byte encoding: 200 -> 01 48 (high bit first) */
    mv_param_set(&p, MV_P_C_CUTOFF_FREQUENCY, 200);
    CHECK(p.b[106] == 0x01 && p.b[107] == 0x48, "2-byte encoding %02X %02X", p.b[106], p.b[107]);
    CHECK(mv_param_rel(&p, MV_P_C_CUTOFF_FREQUENCY) == 73, "2-byte rel");
    CHECK(mv_param_rel(&p, MV_P_OSC2_RANGE) == 0 && mv_param_rel(&p, MV_P_OSC_SHIFT) == 0, "centres");
    mv_patch_init(&p);
    CHECK(memcmp(p.b, MV_PATCH_INIT, MV_PATCH_BYTES) == 0, "init again");
}

static void test_cc(void)
{
    mv_patch_t p;
    mv_patch_init(&p);
    CHECK(mv_cc_apply(&p, 74, 33) == MV_P_CUTOFF && mv_param_get(&p, MV_P_CUTOFF) == 33, "CC74 cutoff");
    CHECK(mv_cc_apply(&p, 21, 127) == MV_P_OSC2_RANGE && mv_param_get(&p, MV_P_OSC2_RANGE) == 50, "CC21 range top");
    CHECK(mv_cc_apply(&p, 21, 0) == MV_P_OSC2_RANGE && mv_param_get(&p, MV_P_OSC2_RANGE) == 0, "CC21 range bottom");
    CHECK(mv_cc_apply(&p, 21, 64) == MV_P_OSC2_RANGE && mv_param_get(&p, MV_P_OSC2_RANGE) == 25, "CC21 range centre");
    CHECK(mv_cc_apply(&p, 1, 64) == -1, "CC1 is the mod wheel, not a patch parameter");
    {   /* the panel's CC out (mv_cc_value) comes back as the same value through mv_cc_apply, every value of every
         * parameter with a CC (Stage 7: a DAW that records the panel plays it back exactly) */
        uint32_t id;
        int32_t v;
        for (id = 0; id < MV_NPARAMS; id++) {
            const mv_param_t *d = &MV_PARAMS[id];
            if (d->cc == MV_NONE) continue;
            for (v = d->min; v <= d->max; v++) {
                uint32_t c = mv_cc_value(id, v);
                CHECK(c <= 127u && mv_cc_apply(&p, d->cc, c) == (int32_t)id && mv_param_get(&p, id) == v,
                      "CC round trip: param %u value %d via CC %u = %u", (unsigned)id, (int)v, d->cc, (unsigned)c);
            }
        }
    }
    CHECK(mv_cc_apply(&p, 200, 0) == -1 && mv_cc_apply(&p, 74, 200) == -1, "out of range");
}

static void test_sysex(void)
{
    mv_patch_t a, b;
    uint8_t m[MV_SYSEX_MAX], m2[MV_SYSEX_MAX], m3[MV_SYSEX_MAX];
    uint32_t n, id, k;
    int32_t pn = 99;
    mv_patch_init(&a);
    for (id = 0; id < MV_NPARAMS; id++)         /* a patch unlike the init one: every parameter moved */
        mv_param_set(&a, id, MV_PARAMS[id].min + (int32_t)((id * 37u) % (MV_PARAMS[id].max - MV_PARAMS[id].min + 1u)));
    memcpy(a.b, "Spit'n Slide Bs ", 16);
    n = mv_sysex_build(&a, 0x10, MV_ADDR_TEMP_UPPER, m, sizeof m);
    CHECK(n == 11u + MV_PATCH_BYTES + 1u, "built %u bytes", (unsigned)n);
    mv_patch_init(&b);
    CHECK(mv_sysex_parse(m, n, &b, &pn) == MV_SX_OK && pn == -1, "parse temp");
    CHECK(memcmp(a.b, b.b, MV_PATCH_BYTES) == 0, "temp round trip byte for byte");
    /* a user slot, B37 = 64 + 2*8 + 6 = patch 86 */
    n = mv_sysex_build(&a, 0x10, mv_user_addr(86), m, sizeof m);
    CHECK(m[6] == 0x02 && m[7] == 0x01 && m[8] == 0x2C && m[9] == 0x00, "user address %02X %02X %02X %02X",
          m[6], m[7], m[8], m[9]);
    mv_patch_init(&b);
    CHECK(mv_sysex_parse(m, n, &b, &pn) == MV_SX_OK && pn == 86, "parse user, patch %d", (int)pn);
    CHECK(memcmp(a.b, b.b, MV_PATCH_BYTES) == 0, "user round trip");
    /* Roland checksum of the RQ1 body 02 00 00 00 00 00 01 78: 128 - (0x7B % 128) = 0x05 */
    {
        static const uint8_t rq[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x78};
        CHECK(rol_sum(rq, 8) == 0x05, "Roland checksum %02X", rol_sum(rq, 8));
    }
    /* errors: checksum, missing F7, a system address, another model */
    memcpy(m2, m, n); m2[n - 2] ^= 1;
    mv_patch_init(&b);
    CHECK(mv_sysex_parse(m2, n, &b, 0) == MV_SX_BAD_SUM && memcmp(b.b, MV_PATCH_INIT, MV_PATCH_BYTES) == 0,
          "bad checksum writes nothing");
    memcpy(m2, m, n); m2[n - 1] = 0;
    CHECK(mv_sysex_parse(m2, n, &b, 0) == MV_SX_BAD_FRAME, "no F7");
    memcpy(m2, m, n); m2[6] = 0x00;
    CHECK(mv_sysex_parse(m2, n, &b, 0) == MV_SX_NOT_OURS, "system area ignored");
    memcpy(m2, m, n); m2[4] = 0x07;
    CHECK(mv_sysex_parse(m2, n, &b, 0) == MV_SX_NOT_OURS, "other model ignored");
    /* JP-8000 style: the first 239 bytes at 01 00 40 00, the extension (unison, gain..) at 01 00 41 72 */
    m3[0] = 0xF0; m3[1] = 0x41; m3[2] = 0x10; m3[3] = 0x00; m3[4] = 0x06; m3[5] = 0x12;
    m3[6] = 0x01; m3[7] = 0x00; m3[8] = 0x40; m3[9] = 0x00;
    for (k = 0; k < 239; k++) m3[10 + k] = a.b[k];
    m3[10 + 239] = rol_sum(m3 + 6, 4 + 239); m3[10 + 240] = 0xF7;
    mv_patch_init(&b);
    CHECK(mv_sysex_parse(m3, 251, &b, 0) == MV_SX_OK, "JP-8000 part 1");
    CHECK(memcmp(a.b, b.b, 239) == 0 && b.b[243] == MV_PATCH_INIT[243], "part 1 only touches 0..238");
    m3[8] = 0x41; m3[9] = 0x72;                 /* patch byte 128 + 114 = 242 */
    for (k = 0; k < 6; k++) m3[10 + k] = a.b[242 + k];
    m3[16] = rol_sum(m3 + 6, 4 + 6); m3[17] = 0xF7;
    CHECK(mv_sysex_parse(m3, 18, &b, 0) == MV_SX_OK, "JP-8000 extension");
    CHECK(memcmp(a.b + 242, b.b + 242, 6) == 0, "extension lands on 242..247");
    /* sanitize: a foreign patch with bytes out of range comes back in range */
    mv_patch_init(&b);
    b.b[30] = 99; b.b[0] = 3; b.b[74] = 0x7F; b.b[75] = 0x7F;
    CHECK(mv_patch_sanitize(&b) == 3, "sanitize count");
    CHECK(b.b[30] == 6 && b.b[0] == ' ' && mv_param_get(&b, MV_P_C_LFO1_RATE) == 254, "sanitize values");
}

int main(void)
{
    test_table();
    test_get_set();
    test_cc();
    test_sysex();
    printf("mv_patch_test: %d checks, %d failed (%u parameters)\n", checks, fails, (unsigned)MV_NPARAMS);
    return fails != 0;
}
