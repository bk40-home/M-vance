# Licensing

M-vance is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too. M-vance is built on Dinghy (zvenson) and keeps its platform; the table
below says who made what.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The SMK-37 board layer: `firmware/hal/smk_input.h`, `fm1_flash_map.h`, the SMK-37 parts of `fm1_cc.h`, `fm1_flash.h`, `fm1_lcd_hw.h`, `lcd.c`, `ota.c`, `bootguard.h`, `firmware/loader/`, `tools/fm1pkg_make.py`, `tools/fm1_install.py`, `tools/build.py`; `firmware/src/smk_wheel.h`; `tests/ota_test.c`, `ldr_test.c`, `install_test.py`, `smk_wheel_test.c`, `smk_pkg_test.py` | Copyright (C) 2026 M. Geisler (genkigenki/makkuro), from [genkigenki/sloop-fm1](https://github.com/genkigenki/sloop-fm1) branch `smk37` (the reference port of the [fm1-to-smk37](https://github.com/genkigenki/fm1-to-smk37) kit) |
| The TRS MIDI IN `firmware/src/midi_uart.c` | Felucca's, as sloopDX carries it, with the ring read by content from Felucca [Salt] 0.9-salt14 by ChanceTheMaker (found by keremimo) |
| The update loader's screen: `firmware/loader/ldr_screen.c`, `tests/ldr_screen_test.c` | Copyright (C) 2026 zvenson (from OberVave, the Firmware Hub's look); the idea, a loader that draws, its 5 x 7 letters and the order of things from [Skiff](https://github.com/illuminus5/skiff) by illuminus5, GPL-3.0 |
| From OberVave: the storage (blobs, slots), `LCD_IDLE`, the SMK-37 wiring of `platform.c`, `store.h`, the pad event queue in `smk_input.h`, `tests/smk_flash_test.c`, `tests/run_tests.sh`, the CI workflow | Copyright (C) 2026 zvenson |
| The app `firmware/src/dinghy.c`, the wiring `platform.c`, the host mode `tests/host.c`, the Dinghy docs (`docs/API.md`, `RULES.md`, `MAKE-IT-YOURS.md`, `SMK37.md`) | zvenson |
| **M-vance:** the app `firmware/src/mvance.c`, the engine and panel `firmware/src/mv_*` (except `mv_arp.*`), the generators `tools/gen_*.py`, `tools/mid2bank.c`, `tools/make_bank.py`, `tools/gen_logo.py`, the logo `assets/logo.*`, `assets/icon.*`, `tools/mvance-params.yaml`, the compare tools `tools/compare/`, the tests `tests/mv_*`, `tests/run_unit.sh`, `run_compare.sh`, `run_docs.sh`, the M-vance host scripts in `tests/scripts/`, the law fits in `tools/laws/`, the web page and cheat sheet `docs/index.html`, `docs/cheatsheet.html`, `docs/img/` | Copyright (C) 2026 Kris (bk40-home), ported from his JT-8000 (Jteensy8000); the plate reverb after the JT-8000's `PlateReverb` |
| The arpeggiator `firmware/src/mv_arp.c`, `mv_arp.h` | Ported from [Skiff](https://github.com/illuminus5/skiff) (`firmware/src/seq.c`): Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (Felucca); illuminus5 (Skiff's changes), GPL-3.0 |
| The USB audio output `firmware/src/uac_out.h`, `uac_out.c` and its test `tests/uac_out_test.c` | **MPL-2.0**, not GPL (each file's header): ported from [felucca-bsp](https://github.com/hugelton/felucca-bsp) `bsp/chip/usb.c` (`FELUCCA_UAC_OUT`, commit 773b0b9), Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; the integer port Copyright (C) 2026 Kris (bk40-home). MPL-2.0 is compatible with the GPL (its section 3.3): the firmware as a whole stays GPL-3.0-only, these files keep their MPL-2.0 headers and their changes are published under it. The descriptor test `tests/uac_desc_test.c` is M-vance's (GPL-3.0-only) |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

| The factory programs: `firmware/gen/mv_factory.h`, the 64-program bank committed to this repository, built by `tools/make_bank.py` from `tools/bank/factory.txt`, which takes all 64 from `tools/bank/mvance.syx`. The programs derive from **Alan Marcero's** Trance A and B banks and **Duston's** Trance Classics Vol. 3, each renamed and edited by Kris (on-device parameter edits) | The programs derive from the source banks, which remain their authors'. M-vance's own code and the bank format are GPL-3.0; the derived programs are not the project's to relicense. A release of this bank credits Alan Marcero and Duston and goes out only with their agreement, or with the derived programs replaced by original ones | `tools/bank/factory.txt`, `tools/bank/mvance.syx`, `firmware/gen/mv_factory.h` |

## The logo

`assets/logo.svg` / `logo.png` (the wordmark) and `assets/icon.svg` / `icon.png` (the square icon) are M-vance's own,
made by `tools/gen_logo.py`: seven fanned sawtooth waves (the Super Saw) under a plate with "M-VANCE" in the FM-1's
large font. Copyright (C) 2026 Kris (bk40-home), GPL-3.0-only like the rest; the letters are the Terminus font
(SIL OFL 1.1, `assets/fonts/Terminus-LICENSE.txt`) as rendered into `firmware/gen/felucca_font.h`. They replace
Dinghy's designburg signet, which is not used anywhere in M-vance.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE", "FM-1", "SMK-37" and "MKE-P37"
belong to their owners. Dinghy is not made or endorsed by them. JP-8000 and JP-8080 are trademarks of Roland
Corporation: M-vance is not made by, endorsed by or affiliated with Roland, and these marks do not appear in the
firmware.
