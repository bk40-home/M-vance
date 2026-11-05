#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Make the M-vance factory bank from a list (tools/bank/factory.txt says the format).

  python3 tools/make_bank.py [tools/bank/factory.txt]   the hand-made list (the default)
  python3 tools/make_bank.py --auto                      a list from every file in docs/presets (tools/auto_bank.py
                                                         writes tools/bank/auto.txt), then the bank from it
  python3 tools/make_bank.py --from-device FILE.syx      the FM-1's own programs (pulled with docs/bank.html) become
                                                         the factory bank: FILE is kept as tools/bank/device.syx,
                                                         factory.txt is rewritten to take every slot from it (the
                                                         old list kept in tools/bank/history/, each slot's origin
                                                         kept as its comment), then the bank is built from it

  1  reads each source bank (.mid or .syx of JP-8000/8080 DT1 messages: user patches 02 bb pp xx and the
     performances' patches 03 pp 4x), and assembles every patch (248 bytes, as the JP stores them)
  2  takes the 64 the list names, gives each its new name and applies its edits (keys from mvance-params.yaml)
  3  writes them as 64 PERFORMANCES to build/mvance-factory.syx (a JP-8080 performance bank: each a common block
     with the name, KEY MODE, SPLIT and BALANCE, its upper patch and its lower patch), which a JP-8080, the JT-8000
     and the M-vance (tools/mid2bank) all read
  4  runs tools/mid2bank on that file (built here if needed): the firmware's own parser and range clamp make
     firmware/gen/mv_factory.h, byte for byte what the device would read from the same SysEx

A program can also be designed from scratch (Kris 3 Nov, B3): its source INIT is the JP init patch (MV_PATCH_INIT of
firmware/gen/mv_params.h, as the firmware has it), and its edits may name a choice instead of its number
(OSC1_WAVE=SUPERSAW, FLT_TYPE=LPF, MFX_TYPE=SUPERCHORUSSLW: the names of mv_params.h, upper case, letters and
digits only). LOWER=INIT and L.KEY= edits design the lower patch the same way.

It stops on anything doubtful: a slot twice or missing, a patch not in its source, an unknown key, a name too long.
"""
import re
import shlex
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NPROG, PATCH = 64, 248
MODES = ["SINGLE", "DUAL", "SPLIT"]
NPERF = 16                                              # the performance bytes a program keeps (mv_perf.h MV_PF_*)
PERF_DEF = bytes([0, 60, 64, 3, 0, 0, 0] + [0] * 9)     # MV_PERF_DEF: SINGLE, split 60, balance 64, 4+4, channels
                                                        #  global, ARP AUTO
# the list's performance keys -> MV_PF_* index and its range (mv_prog_perf_ok)
PERF_KEYS = {"SPLIT": (1, 127), "BAL": (2, 127), "VSPLIT": (3, 6), "CHLO": (4, 16), "CHUP": (5, 16), "ARP": (6, 3)}
# LFO1 SYNC (Kris 5 Nov): a JP part parameter (03 pp 10 / 11 offset 5), kept in MV_PF_SYNCUP (7) / MV_PF_SYNCLO (8);
# in the list SYNC= / L.SYNC= take 0..22 or these names (the JE's lFO1Sync; D = dotted, nM = n measures)
SYNC_NAMES = ["OFF", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D", "1/4", "1/2T", "1/4D", "1/2", "1/1T", "1/2D",
              "1/1", "2/1T", "1/1D", "2/1", "3M", "4M", "5M", "6M", "7M", "8M"]
PF_SYNCUP, PF_SYNCLO = 7, 8
ARPP = ["AUTO", "UPPER", "LOWER", "BOTH"]


def fail(msg):
    sys.exit("make_bank: " + msg)


def smf_sysex(data):
    """The SysEx messages of a Standard MIDI File (every track), each as F0 .. F7."""
    def vlq(i):
        v = 0
        while True:
            c = data[i]; i += 1; v = (v << 7) | (c & 127)
            if not c & 128:
                return v, i
    out, i = [], 8 + int.from_bytes(data[4:8], "big")
    while i + 8 <= len(data) and data[i:i + 4] == b"MTrk":
        end = i + 8 + int.from_bytes(data[i + 4:i + 8], "big"); j = i + 8; rs = 0
        while j < end:
            _, j = vlq(j)
            s = data[j]
            if s in (0xF0, 0xF7):
                ln, j = vlq(j + 1)
                if s == 0xF0:
                    out.append(bytes([0xF0]) + data[j:j + ln])
                j += ln
            elif s == 0xFF:
                ln, j = vlq(j + 2); j += ln
            else:
                if s & 0x80:
                    rs = s; j += 1
                j += 1 if (rs & 0xF0) in (0xC0, 0xD0) else 2
        i = end
    return out


def bank_patches(path):
    """{'U12': 248 bytes, 'P26A': ...}: user patches by number (1..128), performance patches by P<n><A|B>."""
    data = path.read_bytes()
    if data[:4] == b"MThd":
        msgs = smf_sysex(data)
    else:
        msgs = [m.group(0) for m in re.finditer(rb"\xF0[^\xF7]*\xF7", data)]
    pats = {}
    coms = {}
    exts = {}
    syncs = {}                                          # 'P26A' / 'P26B': that part's LFO1 SYNC (0..22)
    for m in msgs:
        if len(m) < 12 or m[1] != 0x41 or m[3:6] != b"\x00\x06\x12":
            continue                                    # not a JP-8000/8080 DT1
        a, body = m[6:10], m[10:-2]
        if (sum(m[6:-2]) + m[-2]) & 0x7F:
            fail(f"{path.name}: a message with a bad checksum at {a.hex()}")
        if a[0] == 0x02 and a[1] <= 1:                  # user patch: 02 bb pp xx, odd pp = bytes 128..
            key, off = f"U{a[1] * 64 + a[2] // 2 + 1}", (a[2] & 1) * 128 + a[3]
        elif a[0] == 0x03 and 0x40 <= a[2] <= 0x43:     # performance pp: 40/41 upper, 42/43 lower
            key, off = f"P{a[1] + 1}{'A' if a[2] < 0x42 else 'B'}", (a[2] & 1) * 128 + a[3]
        elif a[0] == 0x03 and a[2] == 0x00:             # its common block: name, KEY MODE 0x10, SPLIT 0x11, BAL 0x13
            c = coms.setdefault(f"P{a[1] + 1}", bytearray(64))
            c[a[3]:a[3] + len(body)] = body[:64 - a[3]]
            continue
        elif a[0] == 0x03 and a[2] in (0x10, 0x11):     # a part (upper / lower): its LFO1 SYNC at offset 5
            if a[3] <= 5 < a[3] + len(body):
                v = body[5 - a[3]]
                syncs[f"P{a[1] + 1}{'A' if a[2] == 0x10 else 'B'}"] = v if v < len(SYNC_NAMES) else 0
            continue
        elif a[0] == 0x03 and a[2] == 0x70:             # M-vance's own: the 16 performance bytes (mv_xfer.h)
            x = exts.setdefault(f"P{a[1] + 1}", bytearray(PERF_DEF))
            x[a[3]:a[3] + len(body)] = body[:NPERF - a[3]]
            continue
        else:
            continue
        p = pats.setdefault(key, bytearray(PATCH))
        p[off:off + len(body)] = body[:PATCH - off]
    for k, c in coms.items():                           # a whole performance: P26 = its common + its two patches
        if k + "A" in pats:                             #  (+ M-vance's performance bytes when the file has them)
            pats[k] = (c, pats[k + "A"], pats.get(k + "B", pats[k + "A"]), exts.get(k))
    pats["_sync"] = syncs                               # (not a patch key: patch keys start U or P)
    return pats


def param_keys():
    """KEY -> patch byte, from tools/mvance-params.yaml (the same keys as MV_P_<KEY>)."""
    keys = {}
    for line in (ROOT / "tools/mvance-params.yaml").read_text().splitlines():
        m = re.match(r"\s*(\d+):\s*\{key:\s*([A-Z0-9_]+)", line)
        if m:
            keys[m.group(2)] = int(m.group(1))
    return keys


def init_patch():
    """The JP init patch, as the firmware has it (MV_PATCH_INIT in firmware/gen/mv_params.h): 248 bytes."""
    txt = (ROOT / "firmware/gen/mv_params.h").read_text()
    m = re.search(r"MV_PATCH_INIT\[248\]\s*=\s*\{([^}]*)\}", txt)
    if not m:
        fail("firmware/gen/mv_params.h: no MV_PATCH_INIT")
    b = bytearray(int(x) for x in m.group(1).replace("\n", " ").split(",") if x.strip())
    if len(b) != PATCH:
        fail(f"MV_PATCH_INIT has {len(b)} bytes, not {PATCH}")
    return b


def choice_names(keys):
    """KEY -> {NAME: value} from the MV_N_<KEY> lists of firmware/gen/mv_params.h ("SUPER SAW" -> SUPERSAW,
    "-24dB/oct" -> 24DBOCT): the choices an edit may name instead of their number."""
    txt = (ROOT / "firmware/gen/mv_params.h").read_text()
    out = {}
    for m in re.finditer(r"MV_N_([A-Z0-9_]+)\[\]\s*=\s*\{([^}]*)\}", txt):
        if m.group(1) in keys:
            names = re.findall(r'"([^"]*)"', m.group(2))
            out[m.group(1)] = {re.sub(r"[^A-Z0-9]", "", s.upper()): i for i, s in enumerate(names)}
    return out


def dt1(addr, body):
    """One Roland DT1 message (device 10h, the JP-8000/8080's model 00 06) with its checksum."""
    head = bytes([0xF0, 0x41, 0x10, 0x00, 0x06, 0x12]) + bytes(addr)
    s = (128 - (sum(addr) + sum(body)) % 128) % 128
    return head + bytes(body) + bytes([s, 0xF7])


def run_mid2bank(args):
    """tools/mid2bank.c, built when missing or older than its source, run on args (paths relative to the repo).
    Linux / macOS / WSL: cc. Windows (PowerShell): a native gcc / clang if there is one (build/mid2bank.exe), else
    the same through WSL (build/mid2bank, the Linux one), else it says what to do."""
    src = ROOT / "tools/mid2bank.c"
    flags = ["-std=c99", "-O2", "-Ifirmware/src", "-Ifirmware/gen", "tools/mid2bank.c", "-o"]
    if os.name != "nt":
        prefix, cc, tool = [], "cc", ROOT / "build/mid2bank"
    else:
        cc = shutil.which("gcc") or shutil.which("clang") or shutil.which("cc")
        if cc:
            prefix, tool = [], ROOT / "build/mid2bank.exe"
        elif shutil.which("wsl"):
            prefix, cc, tool = ["wsl"], "cc", ROOT / "build/mid2bank"   # (wsl starts in this folder, as /mnt/c/...)
        else:
            fail("mid2bank needs a C compiler: run this in WSL (python3 tools/make_bank.py), or install gcc on Windows")
    tool.parent.mkdir(exist_ok=True)
    if not tool.exists() or tool.stat().st_mtime < src.stat().st_mtime:
        subprocess.run(prefix + [cc] + flags + [tool.relative_to(ROOT).as_posix()], cwd=ROOT, check=True)
    exe = ["./" + tool.relative_to(ROOT).as_posix()] if prefix else [str(tool)]
    return subprocess.run(prefix + exe + args, cwd=ROOT, check=True, capture_output=True, text=True)


def from_device(path):
    """--from-device: the pulled bank as tools/bank/device.syx, factory.txt rewritten to take all 64 from it (P1, Kris
    4 Nov). The old list goes to tools/bank/history/factory-<time>.txt; each slot's line keeps where its program came
    from (the old line, as a comment), so the history of a sound stays in the list."""
    import datetime
    src = Path(path).resolve()
    perfs = bank_patches(src)
    missing = [s for s in range(1, NPROG + 1) if not isinstance(perfs.get(f"P{s}"), tuple)]
    if missing:
        fail(f"{src.name}: performances missing: {missing} (pull all 64 with docs/bank.html)")
    bank = ROOT / "tools/bank"
    old = (bank / "factory.txt").read_text()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M")
    (bank / "history").mkdir(exist_ok=True)
    (bank / "history" / f"factory-{stamp}.txt").write_text(old, newline="\n")
    (bank / "device.syx").write_bytes(src.read_bytes())
    lines = old.splitlines()
    head, i = [], 0                                     # the old header: up to the first source line
    while i < len(lines) and not lines[i].startswith("source"):
        head.append(lines[i]); i += 1
    origin, group, note = {}, {}, None                  # slot -> the old line (and its designed note); group titles
    for ln in lines[i:]:
        m = re.match(r"\s*(\d+)\s+(\S+)\s+(\"[^\"]*\"|'[^']*')\s*(.*)", ln)
        if ln.startswith("# ----"):
            nxt = re.search(r"(\d+)-\d+", ln)
            if nxt:
                group[int(nxt.group(1))] = ln
        elif re.match(r"# \d+: designed:", ln):
            note = ln.split("designed:", 1)[1].strip()
        elif m:
            frm, nm, rest = m.group(2), m.group(3).strip("\"'"), m.group(4)
            edits, _, com = rest.partition("#")
            o = f"{frm} {nm!r}" + (f" {edits.strip()}" if edits.strip() else "") + (f": {com.strip()}" if com.strip() else "")
            origin[int(m.group(1))] = o + (f" ({note})" if note and frm == "INIT" else "")
            note = None
    out = head + [f"# Pulled from the FM-1 on {stamp[:4]}-{stamp[4:6]}-{stamp[6:8]} (make_bank.py --from-device): every slot as the",
                  "# device had it, from tools/bank/device.syx; each line's comment says where the program came from before",
                  f"# (the list then: tools/bank/history/factory-{stamp}.txt).", "",
                  "source K tools/bank/device.syx"]
    for s in range(1, NPROG + 1):
        if s in group:
            out += ["", group[s]]
        c, up, lo, ext = perfs[f"P{s}"]
        name = bytes(up[0:16]).decode("ascii", "replace").rstrip()
        q = f"'{name}'" if '"' in name else f'"{name}"'
        out.append(f"{s:2d}  K:P{s:<3} {q:18s}  # was {origin.get(s, '?')}")
    (bank / "factory.txt").write_text("\n".join(out) + "\n", newline="\n")
    print(f"make_bank: tools/bank/factory.txt now takes all 64 from tools/bank/device.syx "
          f"(the old list: tools/bank/history/factory-{stamp}.txt)")


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--from-device":   # the device's programs: the new factory bank
        from_device(sys.argv[2])
        sys.argv[1:] = [str(ROOT / "tools/bank/factory.txt")]
    if len(sys.argv) > 1 and sys.argv[1] == "--auto":   # every file in docs/presets: tools/auto_bank.py writes the list
        import auto_bank
        auto_bank.main()
        sys.argv[1] = str(ROOT / "tools/bank/auto.txt")
    spec = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "tools/bank/factory.txt"
    keys = param_keys()
    names = choice_names(keys)
    sources, slots = {"INIT": {"INIT": init_patch()}}, {}
    for n, line in enumerate(spec.read_text().splitlines(), 1):
        body = line.split("#", 1)[0].strip()          # (a name may not hold a #)
        if not body:
            continue
        try:
            words = shlex.split(body)
        except ValueError as e:
            fail(f"{spec.name}:{n}: {e}")
        if words[0] == "source":
            sources[words[1]] = bank_patches(ROOT / words[2])
            continue
        slot, frm, name, opts = int(words[0]), words[1], words[2], words[3:]
        if not 1 <= slot <= NPROG or slot in slots:
            fail(f"{spec.name}:{n}: slot {slot} out of range or used twice")
        if len(name) > 16 or any(not 32 <= ord(c) <= 125 for c in name):
            fail(f"{spec.name}:{n}: name {name!r}: up to 16 characters, ASCII 32..125")

        def patch_of(ref):
            if ref == "INIT":                           # designed from the init patch (B3)
                return sources["INIT"]["INIT"]
            src, _, which = ref.partition(":")
            if src not in sources:
                fail(f"{spec.name}:{n}: source {src} not declared")
            key = which if which.startswith("P") else f"U{int(which)}"
            if key not in sources[src]:
                fail(f"{spec.name}:{n}: {ref} is not in source {src}")
            return sources[src][key]

        def sync_of(ref, part):
            # the LFO1 SYNC a patch was played with: its JP part's (P26A / P26B; a whole P26: part A or B), else OFF
            if ref == "INIT":
                return 0
            src, _, which = ref.partition(":")
            if not which.startswith("P"):
                return 0                                # a user patch: the JP keeps no sync with it
            key = which if which[-1] in "AB" else which + part
            return sources[src].get("_sync", {}).get(key, 0)
        got = patch_of(frm)
        perf = list(PERF_DEF)                           # KEY MODE SINGLE, SPLIT 60, BALANCE 64, 4+4 ..
        if isinstance(got, tuple):                      # a whole performance: its setup and both patches
            c, up, lo, ext = got
            if ext is not None:                         # pulled from an M-vance: all 16 as they were
                perf = list(ext)
            else:
                perf[0:3] = [c[0x10] if c[0x10] <= 2 else 0, c[0x11], c[0x13]]
                perf[PF_SYNCUP], perf[PF_SYNCLO] = sync_of(frm, "A"), sync_of(frm, "B")
            up, lo = bytearray(up), bytearray(lo)
        else:
            up = bytearray(got); lo = None
            perf[PF_SYNCUP] = sync_of(frm, "A")         # a performance's patch keeps its part's sync
            perf[PF_SYNCLO] = perf[PF_SYNCUP]           # (its lower: a copy of it, unless LOWER= says)
        sync_set = {}                                   # SYNC= / L.SYNC=: applied last, over what came with the patches
        for o in opts:                                  # the performance: LOWER=, MODE=, SPLIT=, BAL=
            k, eq, v = o.partition("=")
            if k == "LOWER" and eq:
                g2 = patch_of(v)
                lo = bytearray(g2[1] if isinstance(g2, tuple) else g2)
                perf[PF_SYNCLO] = sync_of(v, "A")       # (a whole performance as LOWER: its upper patch, as above)
            elif k in ("SYNC", "L.SYNC") and eq:
                x = SYNC_NAMES.index(v) if v in SYNC_NAMES else int(v) if v.isdigit() else -1
                if not 0 <= x < len(SYNC_NAMES):
                    fail(f"{spec.name}:{n}: {o}: 0..{len(SYNC_NAMES) - 1} or {' '.join(SYNC_NAMES)}")
                sync_set[PF_SYNCLO if k == "L.SYNC" else PF_SYNCUP] = x
            elif k == "MODE" and eq:
                if v not in MODES:
                    fail(f"{spec.name}:{n}: MODE={v}: SINGLE, DUAL or SPLIT")
                perf[0] = MODES.index(v)
            elif k in PERF_KEYS and eq:                 # SPLIT BAL VSPLIT CHLO CHUP ARP
                i, top = PERF_KEYS[k]
                x = ARPP.index(v) if k == "ARP" and v in ARPP else int(v) if v.isdigit() else -1
                if not 0 <= x <= top:
                    fail(f"{spec.name}:{n}: {o}: 0..{top}" + (" or AUTO UPPER LOWER BOTH" if k == "ARP" else ""))
                perf[i] = x
        for i, x in sync_set.items():
            perf[i] = x
        up[0:16] = name.ljust(16).encode("ascii")       # (the program's name: the upper patch's)
        if lo is None:
            lo = bytearray(up)                          # no lower patch: a copy of the upper (DUAL doubles it)
        for e in opts:
            if re.match(r"(LOWER|MODE|SPLIT|BAL|VSPLIT|CHLO|CHUP|ARP|SYNC|L\.SYNC)=", e):
                continue
            m = re.fullmatch(r"(L\.)?([A-Z0-9_]+)([=+-])([A-Z0-9]+)", e)
            if not m or m.group(2) not in keys:
                fail(f"{spec.name}:{n}: edit {e!r}: [L.]KEY=value, KEY+n or KEY-n with a key of mvance-params.yaml")
            p = lo if m.group(1) else up
            b, op, v = keys[m.group(2)], m.group(3), m.group(4)
            if v.isdigit():
                v = int(v)
            elif op == "=" and v in names.get(m.group(2), {}):
                v = names[m.group(2)][v]                # a choice by its name (SUPERSAW, LPF ..)
            else:
                fail(f"{spec.name}:{n}: edit {e!r}: {m.group(2)} has no choice {v} "
                     f"({', '.join(names.get(m.group(2), {})) or 'numbers only'})")
            p[b] = v if op == "=" else p[b] + v if op == "+" else p[b] - v
            if not 0 <= p[b] <= 127:
                fail(f"{spec.name}:{n}: edit {e!r} gives {p[b]}")
        if not all(0 <= x <= 127 for x in perf):
            fail(f"{spec.name}:{n}: SPLIT / BAL 0..127")
        slots[slot] = (up, lo, perf)
    missing = [s for s in range(1, NPROG + 1) if s not in slots]
    if missing:
        fail(f"slots without a program: {missing}")

    # tools/bank/public.txt (Kris 5 Nov, B): the INIT-only bank committed to GitHub, written to mv_factory_public.h.
    # Every other list writes the local bank mv_factory.h, which is git-ignored: its programs come from third-party
    # banks (LICENSING.md). firmware/src/mv_prog.h uses the local bank when it is there, else the public one.
    public = spec.name == "public.txt"
    out = ROOT / ("build/mvance-public.syx" if public else "build/mvance-factory.syx")
    out.parent.mkdir(exist_ok=True)
    syx = b""
    for s in range(1, NPROG + 1):                       # each a JP-8080 performance: common, upper, lower
        up, lo, perf = slots[s]
        com = bytes(up[0:16]) + bytes([perf[0], perf[1], 0, perf[2]])   # 0x12 PANEL SELECT: UPPER
        syx += dt1([0x03, s - 1, 0x00, 0x00], com) + dt1([0x03, s - 1, 0x40, 0x00], up) + dt1([0x03, s - 1, 0x42, 0x00], lo)
        syx += dt1([0x03, s - 1, 0x70, 0x00], bytes(perf))   # M-vance's own: all 16 performance bytes (mv_xfer.h)
    out.write_bytes(syx)
    print(f"make_bank: {out.relative_to(ROOT).as_posix()} ({NPROG} programs, {len(syx)} bytes)")

    h = run_mid2bank([out.relative_to(ROOT).as_posix(), spec.relative_to(ROOT).as_posix()])
    sys.stderr.write(h.stderr)
    if "clamped" in h.stderr:
        fail("a program has values out of range (above): fix its edit")
    header = "firmware/gen/mv_factory_public.h" if public else "firmware/gen/mv_factory.h"
    with open(ROOT / header, "w", newline="\n") as f:   # (LF on Windows too: as committed)
        f.write(h.stdout)
    print("make_bank: " + header)


if __name__ == "__main__":
    main()
