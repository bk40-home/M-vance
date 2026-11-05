/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* The USB descriptors of firmware/src/usb.c as M-vance builds it (FELUCCA_UAC, 48K, no CDC), walked on this computer:
 *   with the audio output (FELUCCA_UAC_OUT=1, the default): lengths add up, 4 interfaces, the AC header lists
 *   interfaces 1 2 3 and its class-specific length covers the 4 terminals, terminal links, EP 0x04 isochronous
 *   adaptive 196 bytes 1 ms, interface 3 alt 0 with no endpoint and alt 1 with one, 44.1 kHz only, bcdDevice 3.60.
 *   With "dump": the device and configuration descriptors in hex, so a build with -DFELUCCA_UAC_OUT=0 can be compared
 *   byte for byte with the original usb.c's (tests/run_unit.sh).
 *   cc -std=c99 -O1 -Wall -Wextra -Werror -Wno-unused-function -Wno-int-to-pointer-cast tests/uac_desc_test.c
 *   (-Wno-int-to-pointer-cast: hal/'s register addresses on a 64-bit computer; the 32-bit chip has none of these)
 * Only get_desc() is called: nothing here touches a register (the SIE code is never reached, so never emitted). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HALF_FRAMES 256u
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define FELUCCA_OTA 0
#define FELUCCA_CDC 0
#define FELUCCA_UAC 1
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#ifndef USB_C
#define USB_C "../firmware/src/usb.c"
#endif
#include USB_C

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char **argv)
{
    const uint8_t *dev, *cfg;
    uint16_t dl, cl;
    uint32_t i;
    CHECK(get_desc(0x0100, &dev, &dl) && get_desc(0x0200, &cfg, &cl), "get_desc");
    if (argc > 1 && !strcmp(argv[1], "dump")) {
        for (i = 0; i < dl; i++)
            printf("%02x%s", dev[i], i + 1 < dl ? " " : "\n");
        for (i = 0; i < cl; i++)
            printf("%02x%s", cfg[i], i + 1 < cl ? " " : "\n");
        return 0;
    }
#if !FELUCCA_UAC_OUT
    printf("uac_desc_test: built without the output: use \"dump\"\n");
    return 1;
#else
    uint32_t ifaces = 0, seen_if = 0, cs_len = 0, cs_start = 0, ep04 = 0, ep84 = 0, cur_if = 99, cur_alt = 99;
    uint32_t if3_eps[2] = {9, 9}, as_term[4] = {0, 0, 0, 0}, ot4_src = 0, it3 = 0, ot4 = 0, rates3 = 0;
    CHECK(dev[12] == 0x60 && dev[13] == 0x03, "bcdDevice %02x%02x, want 0360", dev[13], dev[12]);
    CHECK(cl == (cfg[2] | cfg[3] << 8), "wTotalLength %u, the array %u", cfg[2] | cfg[3] << 8, cl);
    CHECK(cl == 101u + 74u + 3u + 74u, "configuration %u bytes, want 252", cl);
    for (i = 0; i < cl;) {
        const uint8_t *d = cfg + i;
        CHECK(d[0] >= 2u && i + d[0] <= cl, "descriptor at %u: length %u", i, d[0]);
        if (d[0] < 2u)
            break;
        if (d[1] == 4) {                        /* interface */
            cur_if = d[2];
            cur_alt = d[3];
            if (!(seen_if & (1u << d[2]))) {
                seen_if |= 1u << d[2];
                ifaces++;
            }
            if (d[2] == UAC_ASO_IF && d[3] < 2u)
                if3_eps[d[3]] = d[4];
        } else if (d[1] == 0x24 && cur_if == 0) {   /* audio control, class-specific */
            if (d[2] == 1) {
                cs_start = i;
                cs_len = d[5] | d[6] << 8;
                CHECK(d[7] == 3 && d[8] == 1 && d[9] == 2 && d[10] == 3, "AC header collection %u: %u %u %u",
                      d[7], d[8], d[9], d[10]);
            } else if (d[2] == 2 && d[3] == 3) {
                it3 = 1;
                CHECK(d[4] == 0x01 && d[5] == 0x01 && d[7] == 2, "input terminal 3: type %02x%02x, %u ch", d[5], d[4], d[7]);
            } else if (d[2] == 3 && d[3] == 4) {
                ot4 = 1;
                ot4_src = d[7];
                CHECK(d[4] == 0x01 && d[5] == 0x03, "output terminal 4: type %02x%02x, want 0301 speaker", d[5], d[4]);
            }
        } else if (d[1] == 0x24 && cur_if >= 2u && cur_if <= 3u && cur_alt == 1u) {   /* audio streaming */
            if (d[2] == 1)
                as_term[cur_if] = d[3];
            if (d[2] == 2 && cur_if == UAC_ASO_IF) {
                rates3 = d[7];
                CHECK(d[4] == 2 && d[5] == 2 && d[6] == 16 && d[7] == 1 && (d[8] | d[9] << 8 | d[10] << 16) == 44100u,
                      "output format: %u ch, %u bytes, %u bits, %u rates", d[4], d[5], d[6], d[7]);
            }
        } else if (d[1] == 5) {                 /* endpoint */
            if (d[2] == 0x04) {
                ep04 = 1;
                CHECK(cur_if == UAC_ASO_IF && cur_alt == 1u, "EP 0x04 in interface %u alt %u", cur_if, cur_alt);
                CHECK(d[3] == 0x09 && (d[4] | d[5] << 8) == 196 && d[6] == 1, "EP 0x04: attr %02x, %u bytes, %u",
                      d[3], d[4] | d[5] << 8, d[6]);
            }
            if (d[2] == 0x84)
                ep84 = 1;
        }
        i += d[0];
    }
    CHECK(i == cl, "the descriptors add up to %u, not %u", i, cl);
    CHECK(cfg[4] == ifaces && ifaces == 4u, "bNumInterfaces %u, %u described", cfg[4], ifaces);
    {   /* the AC class-specific block: from the header to the next interface */
        uint32_t j = cs_start, sum = 0;
        while (j < cl && cfg[j + 1] == 0x24) {
            sum += cfg[j];
            j += cfg[j];
        }
        CHECK(cs_len == sum && sum == 53u, "AC wTotalLength %u, its descriptors %u", cs_len, sum);
    }
    CHECK(it3 && ot4 && ot4_src == 3u, "terminals 3 / 4 (4 from %u)", ot4_src);
    CHECK(as_term[2] == 2u && as_term[3] == 3u, "streaming links: IF2 -> %u, IF3 -> %u", as_term[2], as_term[3]);
    CHECK(if3_eps[0] == 0u && if3_eps[1] == 1u, "IF3 endpoints: alt 0 %u, alt 1 %u", if3_eps[0], if3_eps[1]);
    CHECK(ep04 && ep84 && rates3 == 1u, "endpoints 0x04 %u 0x84 %u", ep04, ep84);
    printf(fails ? "uac_desc_test: %d FAILED\n" : "uac_desc_test: ok (%u bytes, %u interfaces, bcdDevice 3.60)\n",
           fails ? (unsigned)fails : cl, ifaces);
    return fails != 0;
#endif
}
