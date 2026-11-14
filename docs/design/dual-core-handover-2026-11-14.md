# Handover, 2026-11-14: the second core in M-vance (more voices)

M-Vsynth2 now renders half its voices on the FM-1's second core (CPU1), using Melodee's dual-core code. This note
says how to do the same in M-vance so it can play more than 8 voices. Everything is on the laptop:
`C:\GitHub\M-Vsynth2` (the source of the change, committed locally as `a676847`) and `C:\GitHub\M-vance`.

Read with `docs/design/dual-core-and-melodee-arp-2026-11-14.md` (in M-vance; also the project doc of the same name) (the M-Vsynth2 findings: what Melodee has, how the split
works, the FM-1 checks).

**Status of the source:** host-tested in M-Vsynth2 (bit-identical output with the split on, 379 compare cases), **not
yet built or run on an FM-1.** Do M-Vsynth2's FM-1 checks first if you can (that note, section 6): M-vance should take
code that has run on the chip.

## 1. What changes, in two halves

**The platform (shared Dinghy files): copied, the same in every firmware.** Six new files and seven edited ones. They
start CPU1, hold it safely, survive its faults and fall back to one core. Nothing M-vance-specific.

**The engine (M-vance's own): written for M-vance.** Its render loop hands every other sounding voice to CPU1 each
16-frame block and adds CPU1's sum back before the effects. This is the part that needs thought (section 4).

## 2. Before starting

- M-vance has uncommitted work from another session (the kit UI port: `mv_kit*`, `mv_kui*`, `mv_ui.*`, `mv_roll.c`,
  `mvance.c`, `mv_build.h`, `sim/mv_tiles.c`, `docs/sim/*`). Commit or set it aside first, so the dual-core change is
  its own commit.
- Take the "before" numbers on the FM-1 with the current M-vance: GLO → DEBUG, CPU average and peak, with 8 voices of
  a heavy patch (SUPER SAW, MULTI-FX, DELAY, REVERB on) and with INIT. Write them down.
- On the computer: `tests/run_host.sh` and keep `build/host/mvance.wav` as the reference.

## 3. The platform half (stage D0)

### 3.1 New files: copy from M-Vsynth2 as they are

| File | What |
| --- | --- |
| `firmware/hal/fm1_dual.h` | The options: `MELODEE_DUAL_CORE` (1 on; `DINGHY_DEFS=-DMELODEE_DUAL_CORE=0 ./build.sh` for one core) |
| `firmware/hal/fm1_multicore.h` | Starts CPU1, its stack limits, `csync` shared-word access, CPU1's polling loop (in `.dsp_text`) |
| `firmware/hal/fm1_core1_stop.h` | Holds CPU1 safely |
| `firmware/hal/fm1_core1_bar.h` | Remembers "CPU1 retired" until power-off |
| `firmware/hal/fm1_power.h` | The stock V15 supplies, set before CPU1 starts |
| `firmware/src/audio_worker.h` | The mailbox: submit, join (10 ms timeout), check; retires a faulting or misreading CPU1 |

```powershell
cd C:\GitHub
foreach ($f in 'firmware\hal\fm1_dual.h','firmware\hal\fm1_multicore.h','firmware\hal\fm1_core1_stop.h',
               'firmware\hal\fm1_core1_bar.h','firmware\hal\fm1_power.h','firmware\src\audio_worker.h') {
  Copy-Item "M-Vsynth2\$f" "M-vance\$f" }
```

### 3.2 Edited files: take M-Vsynth2's edits

See them with `git diff --no-index M-vance\<file> M-Vsynth2\<file>` (from `C:\GitHub`). Every hunk below is
dual-core; take all of them.

| File | The edits |
| --- | --- |
| `firmware/hal/fm1_irq.h` | `FM1_DBG_C0/C1`, `fm1_cnum()`, `#include "fm1_core1_stop.h"`, `fm1_core1_fault` and `fm1_core1_fault_c()`; `fm1_fault_c` returns early for a CPU1 fault |
| `firmware/hal/fm1_vec.S` | After `fm1_fault_c`, resume CPU0 (`rti`) in place of the endless loop; Melodee's copyright line |
| `firmware/hal/fm1_sys.h` | `fm1_code_sync()`; `fm1_core1_stop()` before every reset and before the update record |
| `firmware/hal/fm1_guard.h` | The PC guard's RAM window ends at `_guard_ram_end` (covers `.dsp_text`) |
| `firmware/crt0.S` | `fm1_core1_start`: CPU1's entry, its stacks, `rti` into `fm1_core1_main` |
| `firmware/app.ld` | `.dsp_text` after `.ram_text` (`_dsp_start/_dsp_end/_dsp_load`, `_guard_ram_end`); `.cpu1_stacks` (12 KiB) after `.bss` |
| `firmware/src/platform.c` | Includes `fm1_power.h`, `fm1_dual.h`; copies `.dsp_text` and `fm1_code_sync()`; `fm1_power_init()`; the PC guard enabled after CPU1 starts; `mv_core1_start(power_ok, early)` (the name changed from `vs_`) |
| `tools/build.py` | `.dsp_text` objcopied into the image at `_dsp_load`; disassembled; the check "CPU1's loop calls nothing outside RAM"; RAM counted to `_cpu1_stacks_hi` |

Do not take: `platform.c`'s `APP_SYSEX_ID` hunk is M-Vsynth2's own SysEx id. It is harmless (it defaults to `0x41`,
M-vance's) but not part of this change; leave it for a separate tidy-up. In `build.py`, take only the hunks named
above (the rest are the app's name, identity and colours).

### 3.3 M-vance's `mv_dsp.c`: the worker, as `vs_dsp.c`

```c
#if !defined(DINGHY_HOST) && !defined(DINGHY_WEBSIM)     /* (a computer, a browser: no second core, no hardware) */
#include "fm1_time.h"
#include "fm1_dual.h"
#if MELODEE_DUAL_CORE
#include "fm1_multicore.h"
#define MV_PAR_SUBMIT(fn, ctx) audio_worker_submit((fn), (ctx))   /* 1: CPU1 took the job */
#define MV_PAR_JOIN() audio_worker_join()      /* 1: its results are in; 0: CPU1 retired (one core from now) */
#define MV_PAR_CHECK() audio_worker_check()    /* between blocks: retire a CPU1 that faulted or misread */
int mv_core1_start(uint32_t power_ok, uint32_t early)   /* platform.c, at boot: 1 online */
{
    return fm1_core1_boot_allowed(power_ok, early) ? fm1_multicore_start() : 0;
}
int mv_core1_online(void) { return audio_worker_online; }
#endif
#endif
#ifndef MV_PAR_SUBMIT
int mv_core1_online(void) { return 0; }
#endif
```

Then, before any engine change: build, flash, boot. CPU1 starts and idles. Checks: the build notes ".dsp_text (CPU1's
loop): N insns, no call out of RAM"; it boots; the sound is unchanged; several cold boots and soft resets; one OTA
update. **Commit D0** ("Platform: the second core from Melodee (idle)").

## 4. The engine half (stage D1): the split in `mv_engine_render`

### 4.1 Where

`firmware/src/mv_voice.c`, `mv_engine_render`, the loop over `e->v[]` that calls `voice_block` (around line 762 at
`79aa241`). Everything else in the block (events, arp, sequencer, LFOs, BALANCE, DISTORTION, TONE, MULTI-FX, DELAY,
limiters, REVERB) stays on CPU0, before or after the voices.

### 4.2 How M-vance differs from M-Vsynth2

| | M-Vsynth2 | M-vance |
| --- | --- | --- |
| Voice output | Three lines (`bus`, `busl`, `busr`) | A mono bus a part: `pbus[0]` (UPPER), `pbus[1]` (LOWER: the same `bus` unless BALANCE is off centre, then `lbus`) |
| A voice's inputs | The engine and the block (`b`) | Its part's patch bytes (`part_b(e, pi)`), that part's `l1u`, `l1g`, `l2u`, `bend_q8`, and `wheel` |
| Overload guard | `voice_shed` drops a voice when a buffer runs long | **None.** Needed once there are more voices (4.5) |
| Profile build | — | `MV_PROFILE` makes `voice_block` add to `e->prof_osc` / `prof_flt`: a race on two cores (4.4) |

### 4.3 The job

```c
typedef struct {
    mv_engine_t *e;
    const uint8_t *pb[2];                       /* part_b(e, 0), part_b(e, 1) */
    int32_t l1u[2], l1g[2], l2u[2], bend_q8[2];
    uint32_t wheel, n, mix;
    uint8_t idx[MV_NV];
    int32_t bus[MV_CTL], lbus[MV_CTL];          /* CPU1's own lines: UPPER (and LOWER when mix) */
} par_job_t;
static par_job_t par_job __attribute__((aligned(32)));
static void par_run(void *ctx)                  /* (CPU1) */
{
    par_job_t *j = (par_job_t *)ctx;
    uint32_t i;
    int32_t *pb[2] = {j->bus, j->mix ? j->lbus : j->bus};
    for (i = 0; i < MV_CTL; i++) j->bus[i] = j->lbus[i] = 0;
    for (i = 0; i < j->n; i++) {
        mv_voice_t *v = &j->e->v[j->idx[i]];
        uint32_t pi = v->part;
        voice_block(j->e, j->pb[pi], v, pb[pi], j->l1u[pi], j->l1g[pi], j->l2u[pi], j->wheel, j->bend_q8[pi]);
    }
}
```

In the render loop, in place of the one `for (v ...)`: count the sounding voices; every other one (by index: the 2nd,
4th, ...) into `par_job.idx`, the rest into a local `own[]`; fill the job and `MV_PAR_SUBMIT(par_run, &par_job)`;
render `own[]` as now; if the submit said no, render the job's voices here too; else `MV_PAR_JOIN()` and add
`par_job.bus` into `bus` and `par_job.lbus` into `lbus` (or into `bus` when not `mix`). `MV_PAR_CHECK()` once at the
top of `mv_engine_render`. The no-second-core defaults, as `vs_voice.c` has them:

```c
#ifndef MV_PAR_SUBMIT
#ifdef MV_PAR_TEST
#define MV_PAR_SUBMIT(fn, ctx) ((fn)(ctx), 1)   /* the host's check: the job run at once, through the split path */
#else
#define MV_PAR_SUBMIT(fn, ctx) ((void)(fn), (void)(ctx), 0)
#endif
#define MV_PAR_JOIN() 1
#define MV_PAR_CHECK() ((void)0)
#endif
```

The sums are integers added in a different order, so the output is bit for bit the same as one core's.

### 4.4 What must be true (check each while writing it)

- [ ] `voice_block` writes only its own voice (`*p`) and the bus it is given. At `79aa241` it does `(void)e;` and the
  oscillator, filter, envelope and noise state are all in `mv_voice_t`. Check that every function it calls
  (`mv_osc_*`, `mv_ssaw_*`, `mv_jfbk_*`, `mv_flt_*`, `mv_env_*`, `mv_pitch_*`) has no writable `static` (a grep found
  none) and touches no part or engine state: no `P->last_inc`, glide or unison state written from inside it.
- [ ] The arp, the sequencer and the event drain run before the submit (they start and stop voices); nothing changes
  `e->v[]` between the submit and the join.
- [ ] `MV_PROFILE`: give CPU1's voices their own counters in the job, or `#error` when both are on. The profile build
  must not race.
- [ ] Main-loop writes to the patch bytes during a block: the same as today (the ISR already reads them while the
  main loop writes). No new rule.
- [ ] `par_job` is 32-byte aligned, as in M-Vsynth2 (the worker's cache handling assumes it).

### 4.5 Tests on the computer

- `-DMV_PAR_TEST` host build: `tests/run_host.sh` → its WAV **byte for byte** the same as the reference from section
  2, and the same for every script in `tests/scripts/` (DUAL and SPLIT with BALANCE off centre included: that is the
  `lbus` path). Add `MV_PAR_TEST` to `tests/run_tests.sh` so it stays checked.
- `tests/run_unit.sh`, `tests/run_websim_tiles.sh` (the sim has no second core: the one-core path).

On the FM-1 with 8 voices: DEBUG shows the CPU reading drop (M-Vsynth2 expects the voices' share to roughly halve);
the sound unchanged by ear; held chords; fast repeated notes; a program change while playing; DUAL and SPLIT with
BALANCE swept. **Commit D1** ("Engine: half the voices on the second core").

## 5. More voices (stage D2)

1. **Measure first.** After D1, with 8 voices of the heavy patch, read DEBUG's CPU. Each extra voice costs about
   half a voice's old share. Choose `MV_NV` from that, leaving 25 % headroom (M-vance's own rule: under 60 % with the
   full stack). Try 12 first, then 16.
2. **RAM.** `MV_NV` × `sizeof(mv_voice_t)` (print it in a host build) plus CPU1's 12 KiB of stacks. The build prints
   RAM against 96 KB; M-vance already holds three arrays of 64 programs (48.6 KB).
3. **One core must still be safe.** If CPU1 is retired (a weak unit, melodee#17) the extra voices land on CPU0 and the
   audio ISR overruns. Do both:
   - limit `eng.nv` to 8 while `mv_core1_online()` is 0 (the VOICES setting is kept, the cap applied at use);
   - add M-Vsynth2's overload guard (`voice_shed` in `vs_voice.c`: a buffer over its budget drops the oldest
     released voice, else the oldest held) as the backstop. It needs a ticks counter (`fm1_ticks`), as
     `vs_dsp.c`'s `VS_TICKS`.
4. **Where `MV_NV` shows** (all read it, check each at 12 and 16):
   - `mv_kui.c`: `kui_voices()` packs 2 bits a voice into a `uint32_t` (16 voices fit exactly; more do not);
     `kit_head(..., MV_NV)` draws a cell a voice in the header: check 16 cells fit its width; the voice and unison
     graphs (lines 565 to 680)
   - `mv_ui.c`: the VOICES setting's range and bar; the split of voices between UPPER and LOWER ("1+7 .. 7+1" in the
     README becomes 1+(N-1)); the JSON export's `voices`
   - `mv_home.c`: the HOME voice display
   - `slice()` in `mv_voice.c`: the parts' voice ranges
   - README (Voices row, Layer/split row), the cheat sheet, the web sim (one core: keep its `nv` at 8 or accept that it
     plays all N in the browser)
5. **Commit D2** ("N voices on two cores; 8 on one").

## 6. Builds that must stay one core

- **The fm1-dock module (`dock/`, `DINGHY_DOCK`)**: a module runs inside the host's firmware, which owns CPU1. Build
  it with `MELODEE_DUAL_CORE=0`, `MV_NV` back at 8 (in `dock/module.cfg` or the workflow), and check its host test
  `tests/run_dock_host.sh`.
- **SMK-37 builds (`BOARD=smk37`)**: the same chip family, but Melodee's code was only proven on the FM-1. Either
  build them with `MELODEE_DUAL_CORE=0` until tried on an SMK-37, or try one and record the result. M-Vsynth2 has the
  same open question.
- **Host and web sim**: no second core (`fm1_dual.h` is not included there); the `MV_PAR_TEST` host build is the
  check of the split.

## 7. On the FM-1 (the full check)

1. `./build.sh` passes; notes ".dsp_text … no call out of RAM"; RAM under 96 KB.
2. It boots; DEBUG shows 2 cores (add a CORES read-out as M-Vsynth2's HOME has: `mv_core1_online()`).
3. CPU readings at 8 voices, before and after D1; then at N voices with the heavy patch: under 60 %.
4. Sound unchanged at 8 voices (by ear; the host check covers the bits).
5. Held chords at N voices; fast notes; arp and sequencer running; DUAL and SPLIT with BALANCE swept; a program change
   while playing.
6. A save while holding notes; a bank send from the bank editor; one OTA update.
7. Several cold power-ups and soft resets: 2 cores each time. A unit with a weak CPU1 drops to one core, plays 8
   voices and stays there until power-off: the fallback working, not a fault.
8. `DINGHY_DEFS=-DMELODEE_DUAL_CORE=0 ./build.sh`: plays as v2.5.1 with 8 voices.

## 8. Licences and credits

Melodee (`keremimo/melodee` at `165d177`) is GPL-3.0-only, as M-vance. The copied files carry their notices (Kerem
Kilic, Ellic Studio; Felucca and Hügelton portions). Add a LICENSING.md row for the second core's files and a README
credit, as M-Vsynth2's (its LICENSING.md and README, "Voices" and credits).

## 9. To start the session (on the laptop)

Connect `C:\GitHub\M-vance` and `C:\GitHub\M-Vsynth2`, attach this project, and paste:

> Read `docs/design/dual-core-handover-2026-11-14.md` and `docs/design/dual-core-and-melodee-arp-2026-11-14.md` (in M-vance; also the project doc of the same name). Do
> stage D0 in M-vance (section 3): copy the six files, apply the seven files' dual-core edits from M-Vsynth2, add the
> worker to `mv_dsp.c`, and show me the diff and the host tests before D1. Do not change the voice count yet.
