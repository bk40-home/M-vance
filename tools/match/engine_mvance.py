# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match (WP0): the M-vance backend of the Engine interface. Renders on the host build (tests/host.c: the
firmware's own app and engine, compiled for the PC), as tools/audition.py does. Everything M-vance-specific in the
match tool lives here (proposal section 7, rule 7).

  with MvanceEngine() as eng:                          (builds the host once into its own temporary folder)
      p = eng.init_patch()
      audio = eng.render(p, note=60, vel=100, gate_ms=2000, total_ms=6300)   # float32, (frames, 2), 44.1 kHz

How one take is made (proposal WP0): the patch goes as ONE DT1 to the temporary upper patch (01 00 40 00) in a .syx
file, and a host script plays it:

  10 midi 0xC0 0 0             program 1 first, so every take starts from the same program state
  50 syx <folder>/cand.syx     the candidate patch (the host's "syx" line, not "sx": tests/host.c reads a script line
                               into 256 characters, which cuts a 260-byte message short)
  300 midi 0x90 <note> <vel>   key down at NOTE_ON_MS
  <300 + gate> midi 0x80 <note> 0
  <total> end

The host runs with the repo root as its working folder and writes OUT.wav (16-bit stereo, 44.1 kHz) and two screen
grabs (OUT-1000.ppm, OUT-3000.ppm, ignored) into this engine's own temporary folder, so parallel workers (one
MvanceEngine each) never share files. A fresh host process per take: nothing (reverb, held notes) carries over.
"""
import os
import re
import shutil
import subprocess
import tempfile
import wave
import weakref
from pathlib import Path
from typing import Dict, List, Optional, Sequence

import numpy as np

try:                                    # run as tools/match/x.py (its folder on sys.path) or as a package
    from . import jp_patch
    from .engine import (Engine, Param, KIND_CONT, KIND_ENUM, KIND_SWITCH, GROUP_FIXED, SAMPLE_RATE)
except ImportError:
    import jp_patch
    from engine import (Engine, Param, KIND_CONT, KIND_ENUM, KIND_SWITCH, GROUP_FIXED, SAMPLE_RATE)

ROOT = Path(__file__).resolve().parents[2]   # the M-vance repo (tools/match/ -> ..)

# script timing (ms from the host's start; the host runs one platform_poll per millisecond)
PC_MS = 10                              # ms: program change to program 1
SYX_MS = 50                             # ms: the candidate patch arrives
NOTE_ON_MS = 300                        # ms: key down (WP1's onset alignment starts here)
FULL_SCALE = 32768.0                    # int16 -> float: the host clamps to +-32767, so output is within -1..1
HOST_PATH_MAX = 199                     # characters: tests/host.c reads a "syx" path with %199s (no spaces either)

# the host build, as tools/audition.py compiles it (APP_VER only names the build)
HOST_SOURCES = ["firmware/src/mvance.c", "firmware/src/mv_dsp.c"]
HOST_FLAGS = ["-O2", "-Wno-unused-function", "-DDINGHY_HOST", '-DAPP_VER="match"', "-Ifirmware/hal",
              "-Ifirmware/src", "-Ifirmware/gen"]

# search groups by patch byte (proposal section 6, Stage 3). Every byte not listed is GROUP_FIXED (left at INIT:
# key follow 43, LFO2 19 20 26 45 53, bend / porta / mono / legato 67..72, morph 74..238, the name 0..15)
GROUPS: Dict[str, Sequence[int]] = {
    "G1": [30, 31, 32, 33, 34, 35, 36, 37, 38, 21, 22, 23, 73],     # oscillators
    "G2": [39, 40, 41, 42],                                         # filter
    "G3": [54, 55, 56, 57],                                         # amp envelope
    "G4": [46, 47, 48, 49, 50],                                     # filter envelope
    "G5": [16, 17, 18, 24, 25, 27, 28, 29, 44, 52, 58],             # pitch envelope, LFO1
    "G6": [59, 60, 61, 62, 63, 64, 65, 66],                         # tone and FX
    "G7": [243, 244, 245],                                          # voice: unison, unison detune, patch gain
    "LEVEL": [51],                                                  # AMP LEVEL: in no section 6 group; section 5
}                                                                   # fits it last, from the RMS ratio
GROUP_OF = {off: g for g, offs in GROUPS.items() for off in offs}
ATTACK_TAUS = 1.6094379                 # ln 5: an attack (charging toward 1.25) reaches full after this many taus


def _nearest_log(table: Sequence[float], value: float) -> int:
    """The index of the table entry nearest value on a log scale (tables are positive and monotonic)."""
    t = np.log(np.maximum(np.asarray(table, dtype=np.float64), 1e-12))
    return int(np.argmin(np.abs(t - np.log(max(value, 1e-12)))))


def build_host(root: Path, exe: Path) -> Path:
    """Compile the host build (tests/host.c through firmware/src/mvance.c) to exe; returns exe. Needs cc."""
    exe.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cc"] + HOST_FLAGS + ["-o", str(exe)] + HOST_SOURCES, cwd=root, check=True)
    return exe


def read_wav(path: Path) -> np.ndarray:
    """A 16-bit stereo WAV as float32 (frames, 2), /32768. Raises ValueError for any other format."""
    with wave.open(str(path)) as w:
        if (w.getnchannels(), w.getsampwidth(), w.getframerate()) != (2, 2, SAMPLE_RATE):
            raise ValueError(f"{path}: expected 16-bit stereo {SAMPLE_RATE} Hz")
        raw = w.readframes(w.getnframes())
    return (np.frombuffer(raw, dtype="<i2").reshape(-1, 2).astype(np.float32) / FULL_SCALE)


def _gen_text(root: Path) -> str:
    """firmware/gen/mv_params.h (made by tools/gen_params.py from the JE table and tools/mvance-params.yaml)."""
    return (root / "firmware/gen/mv_params.h").read_text()


def _yaml_keys(root: Path) -> Dict[str, int]:
    """KEY -> patch byte from tools/mvance-params.yaml (the same reading as tools/make_bank.py param_keys)."""
    keys = {}
    for line in (root / "tools/mvance-params.yaml").read_text().splitlines():
        m = re.match(r"\s*(\d+):\s*\{key:\s*([A-Z0-9_]+)", line)
        if m:
            keys[m.group(2)] = int(m.group(1))
    return keys


def load_params(root: Path = ROOT) -> List[Param]:
    """Every MV_PARAMS entry of firmware/gen/mv_params.h as a Param, in id order (= patch-byte order).

    Extras for display: "label" (panel), "section", "names" (value names), "centre" and "disp" (how the
    value shows: num, signed, enum, cent, range).
    Range and INIT value: the MV_PARAMS row (the JE table merged with the YAML: what the firmware clamps to).
    Key and law: the mv_param_id enum and its comment ("byte N: law; ..."). Kind: law enum or octaves -> enum, law switch ->
    switch; a law-less named value (status STORE / DROP) -> switch when its names are OFF / ON, else enum; the rest
    cont. Raises ValueError if the YAML's key -> byte map disagrees with the header (run tools/gen_params.py)."""
    txt = _gen_text(root)
    ids = re.findall(r"MV_P_([A-Z0-9_]+) = (\d+),\s*/\* byte (\d+): ([a-z0-9_]+);", txt)
    body = txt.split("MV_PARAMS[MV_NPARAMS] = {")[1].split("};")[0]
    rows = re.findall(r"\{(\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (MV_D_\w+), MV_SEC_(\w+), \w+, \d+, "
                      r"\"([^\"]*)\", (\w+)\}", body)
    names = {m.group(1): re.findall(r'"([^"]*)"', m.group(2))
             for m in re.finditer(r"MV_N_([A-Z0-9_]+)\[\]\s*=\s*\{([^}]*)\}", txt)}
    if not ids or len(ids) != len(rows):
        raise ValueError(f"mv_params.h: {len(ids)} parameter ids but {len(rows)} MV_PARAMS rows")
    yaml_keys = _yaml_keys(root)
    out: List[Param] = []
    for (key, pid, byte, law), (off, size, lo, hi, dflt, centre, disp, sec, label, nm) in zip(ids, rows):
        if int(byte) != int(off):
            raise ValueError(f"mv_params.h: MV_P_{key} says byte {byte}, its MV_PARAMS row byte {off}")
        if key in yaml_keys and yaml_keys[key] != int(off):
            raise ValueError(f"mvance-params.yaml puts {key} at byte {yaml_keys[key]}, mv_params.h at {off}: "
                             "run tools/gen_params.py")
        vnames = names.get(key, []) if nm != "0" else []
        if law in ("enum", "octaves"):          # (octaves: OSC COMMON SHIFT, 5 discrete octaves -2..+2: tried
            kind = KIND_ENUM                    #  each in turn, not slid by the optimiser; Kris 12 Nov)
        elif law == "switch":
            kind = KIND_SWITCH
        elif disp == "MV_D_ENUM":
            kind = KIND_SWITCH if vnames == ["OFF", "ON"] else KIND_ENUM
        else:
            kind = KIND_CONT
        p = Param(offset=int(off), key=key, min=int(lo), max=int(hi), kind=kind,
                  group=GROUP_OF.get(int(off), GROUP_FIXED), size=int(size), default=int(dflt))
        p.update({"label": label, "section": sec, "names": vnames, "centre": int(centre),   # (for the report)
                  "disp": disp[5:].lower()})
        out.append(p)
    return out


def load_init_patch(root: Path = ROOT) -> bytes:
    """MV_PATCH_INIT of firmware/gen/mv_params.h: the 248-byte INIT patch, as the firmware has it."""
    m = re.search(r"MV_PATCH_INIT\[248\]\s*=\s*\{([^}]*)\}", _gen_text(root))
    if not m:
        raise ValueError("firmware/gen/mv_params.h: no MV_PATCH_INIT")
    return jp_patch.check_patch([int(x) for x in m.group(1).replace("\n", " ").split(",") if x.strip()])


def load_factory(root: Path = ROOT) -> List[dict]:
    """The 64 factory programs of firmware/gen/mv_factory.h: [{slot 1..64, name, patch (248), lower (248),
    perf (16)}]. (A SINGLE program is perf[0] == 0; DUAL 1, SPLIT 2: one patch cannot carry those.)"""
    return parse_factory((root / "firmware/gen/mv_factory.h").read_text())


def parse_factory(txt: str) -> List[dict]:
    """parse a header in tools/mid2bank.c's output format (MV_FACTORY, MV_FACTORY_LO, MV_FACTORY_PERF) as
    load_factory describes."""
    def rows(tag):
        body = txt.split(tag)[1].split("};")[0]
        return [bytes(int(x) for x in re.sub(r"/\*.*?\*/", "", r, flags=re.S).split(",") if x.strip())
                for r in re.findall(r"\{([^{}]*)\}", body)]
    up, lo, pf = rows("MV_FACTORY[MV_NPROG]"), rows("MV_FACTORY_LO[MV_NPROG]"), rows("MV_FACTORY_PERF[MV_NPROG]")
    return [{"slot": i + 1, "name": jp_patch.patch_name(u), "patch": u, "lower": l, "perf": p}
            for i, (u, l, p) in enumerate(zip(up, lo, pf))]


class MvanceEngine(Engine):
    """M-vance on the host build. One instance = one temporary folder = one worker; the host binary can be shared
    (host=) so a pool builds it once."""

    PATCH_BYTES = jp_patch.PATCH_BYTES  # bytes: a JP-8080 patch
    NOTE_ON_MS = NOTE_ON_MS             # ms: where the key goes down in every take

    def __init__(self, root: Optional[Path] = None, host: Optional[Path] = None, tmp_root: Optional[Path] = None):
        """root: the M-vance repo (default: this file's). host: an already built host binary (default: build one
        here). tmp_root: where the temporary folder goes (default: the system's; its path must stay short)."""
        self.root = Path(root or ROOT).resolve()
        self.dir = Path(tempfile.mkdtemp(prefix="mvm", dir=tmp_root))
        self._finalizer = weakref.finalize(self, shutil.rmtree, str(self.dir), True)
        self.syx_path = self.dir / "cand.syx"
        self.script_path = self.dir / "play.txt"
        self.out_stem = self.dir / "take"
        if len(str(self.syx_path)) > HOST_PATH_MAX or any(c.isspace() for c in str(self.syx_path)):
            raise ValueError(f"{self.syx_path}: the host reads a syx path of <= {HOST_PATH_MAX} characters, no spaces")
        self.host = Path(host).resolve() if host else build_host(self.root, self.dir / "mvance-host")
        self._params: Optional[List[Param]] = None
        self._laws: Optional[Dict[str, list]] = None

    # --- the Engine interface ---
    def params(self) -> List[Param]:
        """Every parameter (load_params), read once."""
        if self._params is None:
            self._params = load_params(self.root)
        return [dict(p) for p in self._params]      # (copies: a caller may edit them)

    def init_patch(self) -> bytes:
        """MV_PATCH_INIT."""
        return load_init_patch(self.root)

    def render(self, patch: bytes, note: int, vel: int, gate_ms: int, total_ms: int) -> np.ndarray:
        """The patch through 01 00 40 00, one note: key down at NOTE_ON_MS, up gate_ms later, the take ends at
        total_ms (ms from the host's start). float32 (frames, 2), 44.1 kHz."""
        self._check_note(note, vel, gate_ms, total_ms)
        jp_patch.write_syx(self.syx_path, jp_patch.temp_patch_syx(patch))
        return self._play([f"{PC_MS} midi 0xC0 0 0", f"{SYX_MS} syx {self.syx_path}"], note, vel, gate_ms, total_ms)

    def export(self, patch: bytes, path: Path, form: str = "temp", slot: int = 0) -> Path:
        """The patch as a .syx: form "temp" (01 00 40 00, plays at once, not stored), "user" (user patch slot
        0..127 = the upper patch of program slot+1 on M-vance) or "perf" (performance slot 0..63, as
        tools/make_bank.py writes a program)."""
        if form == "temp":
            data = jp_patch.temp_patch_syx(patch)
        elif form == "user":
            data = jp_patch.user_patch_syx(patch, slot)
        elif form == "perf":
            data = jp_patch.perf_syx(patch, slot)
        else:
            raise ValueError(f"export form {form!r}: temp, user or perf")
        return jp_patch.write_syx(Path(path), data)

    def close(self) -> None:
        """Remove this engine's temporary folder (its host binary too, unless one was given)."""
        self._finalizer()

    def invert(self, key: str, value: float):
        """The byte nearest value under key's law (docs/ref/jt_laws.json, the tables the firmware's are built
        from; nearest on a log scale for times and frequencies):
          AENV_A / FENV_A / PENV_A   s: time to full (attack charges toward 1.25: full at ln 5 = 1.61 tau)
          AENV_D / AENV_R / FENV_D / FENV_R / PENV_D / LFO1_FADE   s: time constant tau
          AENV_S / FENV_S            0..1: level (linear, byte / 127)
          CUTOFF                     Hz: the filter's cutoff (flt_cut_hz)
          LFO1_RATE                  Hz
        None for any other key."""
        laws = self.laws()
        if key in ("AENV_A", "FENV_A", "PENV_A"):
            return _nearest_log(laws["env_tau_s"], value / ATTACK_TAUS) if value > 0 else 0
        if key in ("AENV_D", "AENV_R", "FENV_D", "FENV_R", "PENV_D", "LFO1_FADE"):
            return _nearest_log(laws["env_tau_s"], value)
        if key in ("AENV_S", "FENV_S"):
            return int(round(min(max(value, 0.0), 1.0) * 127))
        if key == "CUTOFF":
            return _nearest_log(laws["flt_cut_hz"], value)
        if key == "LFO1_RATE":
            return _nearest_log(laws["lfo1_hz"], value)
        return None

    def laws(self) -> Dict[str, list]:
        """docs/ref/jt_laws.json (its trailing commas removed, as tools/gen_params.py does), read once."""
        if self._laws is None:
            import json
            txt = (self.root / "docs/ref/jt_laws.json").read_text()
            self._laws = json.loads(re.sub(r",(\s*[\]}])", r"\1", txt))
        return self._laws

    # --- M-vance only ---
    def render_program(self, slot: int, note: int, vel: int, gate_ms: int, total_ms: int) -> np.ndarray:
        """A stored program (1..64) selected by program change, played as render() plays a patch: the reference
        render() is checked against (WP0 acceptance 1)."""
        if not 1 <= slot <= 64:
            raise ValueError(f"program {slot}: 1..64")
        self._check_note(note, vel, gate_ms, total_ms)
        return self._play([f"{PC_MS} midi 0xC0 {slot - 1} 0"], note, vel, gate_ms, total_ms)

    @staticmethod
    def _check_note(note: int, vel: int, gate_ms: int, total_ms: int) -> None:
        """Raise ValueError for a take the script cannot express (note / velocity out of MIDI range, the key-up
        not before the end)."""
        if not 0 <= note <= 127 or not 1 <= vel <= 127:
            raise ValueError(f"note {note} (0..127), velocity {vel} (1..127)")
        if gate_ms < 1 or total_ms <= NOTE_ON_MS + gate_ms:
            raise ValueError(f"gate {gate_ms} ms and total {total_ms} ms: the key-up ({NOTE_ON_MS} + gate) must "
                             "come before the end")

    def _play(self, setup: List[str], note: int, vel: int, gate_ms: int, total_ms: int) -> np.ndarray:
        """Write the script (setup lines, then the note and the end), run the host, read its WAV."""
        lines = setup + [f"{NOTE_ON_MS} midi 0x90 {note} {vel}", f"{NOTE_ON_MS + gate_ms} midi 0x80 {note} 0",
                         f"{total_ms} end"]
        self.script_path.write_text("\n".join(lines) + "\n")
        wav = self.out_stem.with_suffix(".wav")
        if wav.exists():
            os.remove(wav)                          # (never read a previous take by mistake)
        subprocess.run([str(self.host), str(self.out_stem), str(self.script_path)], cwd=self.root, check=True,
                       capture_output=True)
        return read_wav(wav)
