/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of firmware/src/mv_fixed.h: mv_udiv64_32 (two-digit long division) against the exact quotient, over
 * the filter's and the Super Saw's real operands and random ones. Host only. */
#include <stdio.h>
#include "mv_fixed.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    uint64_t x = 88172645463325252ull;
    uint32_t i, bad = 0, sh;
    for (i = 0; i < 2000000u; i++) {           /* random: d any non-zero, n with n >> 32 < d */
        uint32_t d;
        uint64_t n;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        d = (uint32_t)(x >> (x & 31u)) | 1u;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        n = ((x >> 32) % d) << 32 | (uint32_t)x;
        if (mv_udiv64_32(n, d) != (uint32_t)(n / d)) bad++;
    }
    CHECK(bad == 0, "%u random quotients wrong", bad);
    for (sh = 0; sh < 6u; sh++)                 /* the filter: 2^(56 - sh) / den, den in [2^26, 2^32) */
        for (i = 0; i < 100000u; i++) {
            uint32_t d = (1u << 26) + i * 40959u;
            uint64_t n = 1ull << (56 - sh);
            if ((n >> 32) >= d) continue;
            CHECK(mv_udiv64_32(n, d) == (uint32_t)(n / d), "filter %u / %u", (unsigned)(n >> 32), d);
        }
    CHECK(mv_udiv64_32(0xFFFFFFFEFFFFFFFFull, 0xFFFFFFFFu) == 0xFFFFFFFFu, "largest");
    CHECK(mv_udiv64_32(5, 1) == 5u && mv_udiv64_32(1ull << 32, 2) == 0x80000000u, "small divisors");
    printf("mv_fixed_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
