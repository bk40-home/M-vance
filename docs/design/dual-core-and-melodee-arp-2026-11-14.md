# M-Vsynth2: the second core (from Melodee) and Melodee's arp — findings, 2026-11-14

(A copy of the M-Vsynth2 project doc of the same name, kept here for the M-vance port: `dual-core-handover-2026-11-14.md`.)

**Status (14 Nov, evening): the second core is in, as one change (C0 and C1 together), host-tested, not yet built
or run on the FM-1.** See section 6 for what changed and the checks on the chip. The arp is unchanged (section 5,
waiting for Kris's choice).

Source read: `keremimo/melodee` at `165d177` (2 Nov 2026, release 1.6.1 merged into `next`), GPL-3.0-only, the
same licence as M-Vsynth2. Melodee is Felucca-derived FM-1 firmware; its dual-core code credits Felucca 1.6.1
(hugelton/Felucca PR 247) and the AC79 SDK 1.2.1.

## 1. Where M-Vsynth2 stood

One core. Voices are most of the load (002: V 59 of 76; 068: V 25 of 54), so a split of voices across cores is the
largest gain left.

## 2. What Melodee has

| File | What it does |
| --- | --- |
| `firmware/hal/fm1_multicore.h` | Starts CPU1: its IRQ bank cleared, entry written to `0x01C7FFF8`, release through `0x1EEE004`; waits 10 ms for "ready"; CPU1's stack-limit registers; `csync` load/store for shared words |
| `firmware/crt0.S` (`fm1_core1_start`) | CPU1 entry: `cli`, its own user/supervisor stacks, `rti` into `fm1_core1_main` |
| `firmware/hal/fm1_core1_stop.h` | Holds CPU1 safely (masks the delayed bus-fault flags first) |
| `firmware/hal/fm1_core1_bar.h` | Remembers "CPU1 retired" across soft resets until power-off |
| `firmware/hal/fm1_power.h` | The stock V15 supply setup (SYSVDD 1.38 V, DCDC on), applied before CPU1 |
| `firmware/src/audio_worker.h` | The mailbox: one job at a time, a check word on each request, join with a 10 ms timeout; on a fault, misread or stall CPU1 is retired and CPU0 renders alone |
| `fm1_irq.h`, `fm1_vec.S` | A CPU1 fault holds CPU1 and lets CPU0 resume instead of the crash screen |

Facts that shape the design:

- **Some FM-1 units' CPU1 misreads shared SRAM** now and then (melodee#17). Melodee checks every request and falls
  back to one core for the session. M-Vsynth2 stays correct and playable on one core; the overload guard
  (`voice_shed`) is the backstop there.
- CPU1 runs with no interrupts; only its polling loop must be in RAM. Jobs may run from XIP. CPU0 joins before
  anything else touches the voices; flash writes run with IRQs off, so no job is ever outstanding then.
- The PC guard is enabled only after CPU1 has started (it starts through the mask ROM).
- Melodee also reads the SYS clock (320 or 360 MHz, as the boot loader set it) and can turn 7 of the data cache's
  ways into 28 KB of extra RAM. Neither is used here.

## 3. The split as built

Each 16-frame block, after events and control (CPU0): every other sounding voice (by index) goes to CPU1, which
sums them into the job's own lines; CPU0 renders the rest, joins, adds the job's lines, then runs the bus and
limiter. The sums are integers, so the output is the same whatever the split: the host check (`-DVS_PAR_TEST`,
the job run inline through the split path) gives bit-identical output on all 379 compare cases. If CPU1 is not
online or is retired, CPU0 renders every voice, as before.

## 4. Stages

| Stage | What | Status |
| --- | --- | --- |
| C0 + C1 | Melodee's start/stop/bar/worker/power files, the linker sections, the split | Done on the computer; FM-1 checks below |
| C2 | Docs: README (Voices, credits), LICENSING.md (the second core's row) | Done |

## 5. Melodee's arp

`firmware/src/seq.c` (`arp_add`, `arp_remove`, `arp_step`): UP, DOWN, UP/DOWN, RANDOM, REPEAT; octaves; swing;
gate; probability; HOLD (latch); microtonal scales. It is written around Melodee's tracks and sequencer
(`track_t`, `trk_note_on`, recording, `seq_div_samples`), so it is not a drop-in.

M-Vsynth2's arp is Synth2's, ported 1:1 and checked against Synth2's reference render; patches set it with
Synth2's own parameters. Replacing it would change how factory and user patches play. Options for Kris:

- **a (recommended): keep Synth2's arp for patches;** add Melodee's extras (HOLD, swing, probability, REPEAT) only
  as GLOBAL performance settings that default to off, so a patch plays as in Synth2 unless you turn them on.
- **b:** replace the arp with Melodee's (patch behaviour changes; the arp compare cases would no longer apply).
- **c:** leave the arp as it is.

## 6. What changed (14 Nov) and the checks on the FM-1

New: `firmware/hal/fm1_dual.h` (the options; one-core build: `DINGHY_DEFS=-DMELODEE_DUAL_CORE=0`),
`fm1_multicore.h`, `fm1_core1_stop.h`, `fm1_core1_bar.h`, `fm1_power.h`, `firmware/src/audio_worker.h`.
Changed: `crt0.S` (CPU1 entry), `app.ld` (`.dsp_text` after `.ram_text`, 12 KiB CPU1 stacks after `.bss`),
`fm1_irq.h`, `fm1_vec.S` (CPU1 faults resume CPU0), `fm1_sys.h` (CPU1 held before any reset; `fm1_code_sync`),
`fm1_guard.h` (the PC window covers `.dsp_text`), `platform.c` (copy `.dsp_text`, supplies, start CPU1, PC guard
after), `vs_dsp.c` (the worker), `vs_voice.c` / `.h` (the split), `vs_ui.c` (HOME's foot shows `2CPU`),
`tools/build.py` (`.dsp_text` in the image; a check that CPU1's loop calls nothing outside RAM; RAM counts the
stacks), README, LICENSING.

On the FM-1:

1. `./build.sh` passes: the new note ".dsp_text (CPU1's loop): N insns, no call out of RAM"; RAM about 62 KB of 96.
2. Boots; HOME's foot shows `2CPU`.
3. Readings (CPU / V / B / PK): INIT 8 voices; 002 at 8 voices (was 76 / 59 / 0 / 77); 068 with 2 and 4 keys
   (was 54 / 25 / 20 / 56 with 2). Expect V near half.
4. Sound unchanged by ear on a few patches; held chords, fast repeated notes, a program change while playing.
5. A save while holding notes; a bank send; the firmware update (OTA) once.
6. Several cold power-ups and soft resets: `2CPU` each time (a unit with a weak CPU1 drops to one core and stays
   there until power-off: that is the fallback working, not a fault).
