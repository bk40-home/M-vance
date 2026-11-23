# JT-80X0 as an fm1-dock module

[fm1-dock](https://joakimeriksson.github.io/fm1-dock/) is one firmware for the FM-1 that holds several synths as
modules. You switch between them by holding HOME. This folder builds JT-80x0 as one of those modules, from this
repository's own sources (`firmware/src`).

## What the module is

It is JT-80x0 with the switches in `firmware/src/mv_build.h` set to fit a module's 272 KiB of RAM:

| Kept | Not in the module |
|---|---|
| The engine: oscillators, sync, filters, envelopes, LFOs | LOWER, DUAL / SPLIT, PERFORM, part select: every program plays SINGLE |
| FX, the delay (lines of 900 ms), the plate reverb, the limiter | The sequencer, clips and piano roll (PLAY, REC and SEQ do nothing) |
| The arp, the LFO / delay / chorus sync, MIDI clock in over USB | The DEBUG page (fm1-dock shows the render load itself) |
| The panel pages and the home screen on the screen kit (knobs, graphs, themes), SAVE; SELECT as the tempo | The SysEx program transfer (the patch library and the editor's Link FM-1) |
| All 64 factory programs (UPPER only), and your saves | USB audio playback from the computer. USB *recording* is fm1-dock's, and stays. |

- **Storage.** The programs live in two of the module's flash banks: programs 1–32 in bank 3, 33–64 in bank 4, 8064 B each. A SAVE writes only the bank it changed. The settings live in the module's settings record.
- **Separate from the standalone firmware.** Neither the module nor the standalone JT-80x0 touches the other's saves.
- **MIDI clock out.** It goes out from the main loop, within a few ms of its beat. In the standalone firmware it goes out exactly on its sample.
- **Clock on the TRS jack.** fm1-dock ignores clock on the TRS jack, so use USB for clock.

## Build

From Windows, in this folder:

```powershell
.\flash.ps1 -Build dock        # or .\flash.ps1, menu 9
```

This runs the unit tests, then fm1-dock's `tools/hbmod.py` in WSL. It writes `build\JT80X0.dock`.
- **fm1-dock checkout.** The first time, it clones fm1-dock to `~/fm1-dock` in WSL. Set `FM1_DOCK` to use another checkout.
- **Toolchain.** It uses the same JieLi toolchain (`JIELI_TOOLCHAIN`) as the firmware build.

Each tagged release (v2.5.1 on) also has a ready-built `JT80X0-<version>.dock` among its files. GitHub's release
workflow builds it from the tagged source with fm1-dock's builder, tests it, and attests it ("built from source"), as
it does the firmware.

## Put it on the FM-1

1. **Install fm1-dock** from its page (Chrome or Edge: *Install fm1-dock*). It replaces the JT-80x0 firmware on the FM-1. You can put JT-80x0 back any time with `.\flash.ps1` or from the hub.
2. **Deploy the module.** On the [console](https://joakimeriksson.github.io/fm1-dock/#console), use *Deploy a module* and choose `build\JT80X0.dock`.
3. **Play it.** Hold HOME for 1.5 s, then choose JT-80X0.

## Check it on the computer

```sh
sh tests/run_dock_host.sh      # (WSL) FM1_DOCK=~/fm1-dock
```

This builds the module against fm1-dock's own headers. A stand-in host then drives it through its descriptor. It checks:
- it plays a chord and is silent after;
- the LEDs are sent only when they change;
- PRESETS changes the program;
- a SAVE is written to its bank;
- it starts again from that flash.

It also prints the module's RAM against the 272 KiB limit.

## Files

| File | What it is |
|---|---|
| `module.cfg` | fm1-dock's build description: the sources and the switches |
| `mvance_module.c` | The module's table (`HB_DESC`) and its calls, around `firmware/src/mvance.c` |
| `mv_dock_platform.inc` | `platform.c`'s calls on fm1-dock's services, included by `platform.c` when `DINGHY_DOCK` is set |
