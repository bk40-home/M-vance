/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Copyright (C) 2026 Charles Vestal (the analog key rows: fm1-velocity, fm1-monk) */
/* The analog key rows (velocity and pressure), from felucca-bsp boards/fm1/fm1_input.h (commit 2bd9dbe), its code
 * unchanged, for Dinghy's scan (fm1_input.h includes it, MPL-2.0 kept in this file; fm1-kit's fk_touch is the app's
 * side). Off unless fm1_an.con is set: then fm1_input_tick starts a conversion on the column it has just latched and
 * takes it at the next tick.
 *
 * With fm1_an.con set (the converter's clock, FM1_ADC_CON), the tick starts a conversion on row PA7 (frame phase 0)
 * or PA8 (phase 1) of the column it has just latched and takes the result at the next tick, so each key on those rows
 * is read once per two frames (~2.2 ms): 1023 open, falling as the key's carbon pill is pressed. The MASTER pot takes
 * the slot of (phase 1, column 0, PA8 = OCT-) and lands in fm1_an.master; every FM1_BATT_EVERY-th of its slots reads
 * the battery instead (fm1_an.batt, 0 until the first), as the main loop cannot while the scan owns the converter.
 * The app gets each reading:
 *   FM1_INPUT_ANALOG(col, rowbit, v, driven)   rowbit 3 = PA7, 4 = PA8; the key is FM1_KEYMAP[rowbit][col]
 * With fm1_an.drive set, a key for which FM1_INPUT_DRIVE(col, rowbit) is nonzero (a held key) is read with its row
 * driven high by a weak output instead of pulled up: the pull-up alone sees little but the matrix diode, the driver
 * reads the pill's resistance. The row is an input with its pull-up again at the next tick, before the rows are read.
 * With fm1_an.probe set, a third frame phase reads the top row, PA6 (keys 20..26, columns 0..6, no converter of its
 * own), through PA10, its LED line: PA10 an input with its pull-up, PA6 driven, so a pressed key lifts the column's
 * low level, the more the harder it is pressed. FM1_INPUT_PROBE(col, rowbit, v, pressed) gets each reading, with
 * the scan's last view of that key. A lit LED in a column lifts its keys' readings: FM1_INPUT_LED_OK(col, rowbit)
 * answers 0 for a held key, whose column's LEDs then stay dark on the ticks that read it. */
#pragma once
#include <stdint.h>
#include "fm1_adc.h"

#ifndef FM1_INPUT_ANALOG
#define FM1_INPUT_ANALOG(col, rowbit, v, driven) ((void)0)
#endif
#ifndef FM1_INPUT_DRIVE
#define FM1_INPUT_DRIVE(col, rowbit) 0
#endif
#ifndef FM1_INPUT_PROBE
#define FM1_INPUT_PROBE(col, rowbit, v, pressed) ((void)0)
#endif
#ifndef FM1_INPUT_LED_OK
#define FM1_INPUT_LED_OK(col, rowbit) 1
#endif
#define FM1_ADC_LED_PA10 2u
#define FM1_PROBE_COLS 7u
#ifndef FM1_BATT_EVERY               /* the pot's slots per battery reading (one slot each 2 or 3 frames: ~0.2 s) */
#define FM1_BATT_EVERY 64u           /* (felucca-bsp: 6400 us / the tick's 100 us at 10 kHz, Dinghy's rate) */
#endif
static volatile struct {
    uint16_t con;                /* the conversion's clock / start delay (FM1_ADC_CON); 0 = analog off */
    uint8_t drive;               /* a held key read with its row driven (FM1_INPUT_DRIVE) */
    uint8_t probe;               /* the top row read through PA10 (FM1_INPUT_PROBE) */
    uint8_t pend, col, rowbit;   /* a conversion runs for (col, rowbit); rowbit 0 = the pot */
    uint8_t driven;              /* the pending conversion's row is driven high (released at the take) */
    uint8_t probing;             /* the pending conversion is the probe (PA10 / PA6 released at the take) */
    uint8_t phase;               /* frame phase: 0 PA7, 1 PA8, 2 the probe */
    uint16_t master;             /* the MASTER pot, 0..1023 */
    uint16_t batt;               /* the battery's divider, 0..1023 (0: not read yet) */
    uint8_t batt_n;              /* the pot's slots since the battery's */
    uint8_t batting;             /* the pending conversion is the battery's */
    uint16_t timeouts;           /* conversions not done after a tick (the clock too slow) */
} fm1_an;

/* a row driven high by a weak output: rowbit 3 = PA7, 4 = PA8, 2 = PA6 (the probe). hd0 1 for a held key (HD0 set:
 * Velo's DRIVE 2, the reading spread over the pressure), 0 for the probe (the weakest, as it was measured) */
static void fm1__row_drive(uint32_t rowbit, uint32_t hd0)
{
    uint32_t m = 1u << (rowbit == 3u ? 7 : rowbit == 4u ? 8 : 6);
    if (hd0)
        FM1_PR(FM1_PA, FM1_HD0) |= m;
    else
        FM1_PR(FM1_PA, FM1_HD0) &= ~m;
    FM1_PR(FM1_PA, FM1_HD) &= ~m;
    FM1_PR(FM1_PA, FM1_OUT) |= m;
    FM1_PR(FM1_PA, FM1_DIR) &= ~m;
}
static void fm1__row_release(uint32_t rowbit)
{
    uint32_t m = 1u << (rowbit == 3u ? 7 : rowbit == 4u ? 8 : 6);
    FM1_PR(FM1_PA, FM1_DIR) |= m;                  /* an input with its pull-up again */
    FM1_PR(FM1_PA, FM1_OUT) &= ~m;
    FM1_PR(FM1_PA, FM1_HD0) &= ~m;
}
/* PA10 (the LED line of row PA6): an input with a pull-up for the probe, an output again after */
static void fm1__pa10_probe(int on)
{
    uint32_t m = 1u << 10;
    if (on) {
        FM1_PR(FM1_PA, FM1_OUT) &= ~m;
        FM1_PR(FM1_PA, FM1_PU) |= m;
        FM1_PR(FM1_PA, FM1_DIR) |= m;
    } else {
        FM1_PR(FM1_PA, FM1_PU) &= ~m;
        FM1_PR(FM1_PA, FM1_DIR) &= ~m;
    }
}
/* the conversion started on the previous tick: its result to the app (FM1_INPUT_ANALOG / _PROBE) or the pot */
static void fm1__an_take(void)
{
    uint32_t driven = fm1_an.driven, probing = fm1_an.probing, batting = fm1_an.batting;
    (void)driven;                                  /* (FM1_INPUT_ANALOG may ignore it) */
    fm1_an.pend = 0;
    fm1_an.batting = 0;
    if (driven || probing)
        fm1__row_release(fm1_an.rowbit);
    if (probing)
        fm1__pa10_probe(0);
    fm1_an.driven = fm1_an.probing = 0;
    if (fm1_adc_done()) {
        uint32_t v = fm1_adc_take();
        if (probing)
            FM1_INPUT_PROBE(fm1_an.col, fm1_an.rowbit, v, (fm1_in.raw[fm1_an.col] >> fm1_an.rowbit) & 1u);
        else if (fm1_an.rowbit)
            FM1_INPUT_ANALOG(fm1_an.col, fm1_an.rowbit, v, driven);
        else if (batting)
            fm1_an.batt = (uint16_t)(v ? v : 1u);
        else
            fm1_an.master = (uint16_t)v;
    } else {
        fm1_adc_abort();
        fm1_an.timeouts++;
    }
}
/* column n is latched: light it (lit) and start its conversion, to be taken at the next tick */
static void fm1__an_start(uint32_t n, uint32_t lit)
{
    uint32_t ch, rowbit;
    if (fm1_an.phase == 2u) {                      /* the probe frame: the top row through PA10, keys only */
        if (n >= FM1_PROBE_COLS) {
            fm1__led_lines(lit);
            return;
        }
        fm1__led_lines(lit & ~(1u << 2));
        fm1__pa10_probe(1);
        fm1__row_drive(2u, 0u);
        fm1_an.col = (uint8_t)n;
        fm1_an.rowbit = 2u;
        fm1_an.probing = 1;
        fm1_adc_start(FM1_ADC_LED_PA10, fm1_an.con);
        fm1_an.pend = 1;
        return;
    }
    if (fm1_an.phase && n == 0u) {                 /* the pot's slot (now and then the battery's) */
        ch = FM1_ADC_MASTER;
        rowbit = 0;
        if (++fm1_an.batt_n >= FM1_BATT_EVERY) {
            fm1_an.batt_n = 0;
            fm1_an.batting = 1;
            ch = FM1_ADC_BATT;
        }
    } else {
        ch = fm1_an.phase ? FM1_ADC_ROW_PA8 : FM1_ADC_ROW_PA7;
        rowbit = fm1_an.phase ? 4u : 3u;
    }
    fm1__led_lines(rowbit && !FM1_INPUT_LED_OK(n, rowbit) ? 0u : lit);   /* a held key: no LED current in its column */
    fm1_an.col = (uint8_t)n;
    fm1_an.rowbit = (uint8_t)rowbit;
    if (rowbit && fm1_an.drive && FM1_INPUT_DRIVE(n, rowbit)) {
        fm1__row_drive(rowbit, 1u);
        fm1_an.driven = 1;
    }
    fm1_adc_start(ch, fm1_an.con);
    fm1_an.pend = 1;
}
