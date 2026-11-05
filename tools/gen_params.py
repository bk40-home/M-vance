#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generate firmware/gen/mv_params.h: the M-vance parameter table, keyed by JP-8080 patch byte.

  python3 tools/gen_params.py            (run from the repo root; the output is committed)

Inputs:
  docs/ref/parameterDescriptions_je.json  the JE-8086 table: range, default and value names of every patch byte
  tools/mvance-params.yaml                our overlay: C name, panel label, section, status, law, JT component

What comes out (all const, so it costs flash, not RAM):
  MV_P_<KEY>        the parameter index, in byte order
  MV_PARAMS[]       offset, size (1 or 2 bytes), range, default, centre, display kind, section, status, CC, label
  MV_PARAM_AT[248]  patch byte -> parameter index (0xFF: name, reserved, or the second byte of a pair)
  MV_CC_PARAM[128]  JP MODE2 CC number -> parameter index (0xFF: not mapped)
  MV_PATCH_INIT[248] the init patch: every byte at the JE default, the name "INIT PATCH"
The script fails (exit 1) on anything inconsistent, so a bad edit never reaches the firmware."""
import json, re, sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
JE = ROOT / "docs/ref/parameterDescriptions_je.json"
OVL = ROOT / "tools/mvance-params.yaml"
OUT = ROOT / "firmware/gen/mv_params.h"
NBYTES = 248
NAME_LEN = 16
INIT_NAME = "INIT PATCH"

SECTIONS = ["LFO1", "LFO2", "OSCC", "PITCH", "PENV", "OSC1", "OSC2", "FILT", "FENV",
            "AMP", "AENV", "FX", "VOICE", "CTRL", "VELO", "RSVD"]
STATUSES = ["HAVE", "ADAPT", "NEW", "STORE", "DROP"]
# value lists that are numbers, not names: shown as numbers (with a centre when signed)
NUMERIC = {"unsignedZero": None, "signed": "S", "signed255": "S", "LFO1DepthPan": "S", "bendRangeUp": None,
           "bendRangeDown": None, "oscBalance": "S", "oscShift": "S", "osc2RangeFine": "C", "osc2Range": "R",
           "assignOsc2RangeFineWide": "S", "osc2RangeWidePos": "C", "osc2RangeWideNeg": "C"}


def fail(msg):
    sys.exit(f"gen_params: {msg}")


def load_je():
    txt = JE.read_text(encoding="utf-8")
    txt = re.sub(r",(\s*[\]}])", r"\1", txt)          # the JE file has trailing commas: JSON does not
    j = json.loads(txt)
    byidx, byname = {}, {}
    for d in j["parameterdescriptions"]:
        if d.get("page", 0) != 0:
            continue
        i = d["index"]
        byname[d["name"]] = i
        if i not in byidx:                               # the first entry of an index is the generic one
            byidx[i] = dict(d)
        else:                                            # variants (per wave, rack/keyboard): the widest range
            byidx[i]["max"] = max(byidx[i].get("max", 127), d.get("max", 127))
    return j, byidx, byname


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def snake(name):
    s = re.sub(r"(?<!^)(?=[A-Z])", "_", name).upper()
    return re.sub(r"[^A-Z0-9_]", "", s)


def main():
    j, je, byname = load_je()
    lists = j["valuelists"]
    ovl = yaml.safe_load(OVL.read_text(encoding="utf-8"))["params"]
    params = []                                          # dicts, in byte order
    covered = [None] * NBYTES                            # byte -> param key (both bytes of a pair)
    for i in range(NAME_LEN):
        covered[i] = "NAME"
    for off in sorted(je):
        if off < NAME_LEN:
            continue
        d = je[off]
        mn, mx = d.get("min", 0), d.get("max", 127)
        size = 2 if mx > 127 else 1
        o = ovl.get(off)
        if o is None:                                    # the morph block: named from the JE
            nm = d["name"]
            if nm.startswith("Control"):
                key, sec = "C_" + snake(nm[len("Control"):]), "CTRL"
            elif nm.startswith("Velocity"):
                key, sec = "V_" + snake(nm[len("Velocity"):]), "VELO"
            else:
                fail(f"byte {off} ({nm}) has no overlay entry")
            lab = key[2:].replace("_", "")[:7]           # the label: the target's name, e.g. CUTOFFF..
            o = {"key": key, "label": lab, "sec": sec, "status": "STORE", "law": "morph", "jt": "-"}
        if len(o["label"]) > 7:
            fail(f"byte {off}: label {o['label']!r} is longer than 7")
        if o["sec"] not in SECTIONS or o["status"] not in STATUSES:
            fail(f"byte {off}: unknown section or status")
        tt = d.get("toText", "unsignedZero")
        names = None
        if tt in NUMERIC:
            disp = {"S": "SIGNED", "C": "CENT", "R": "RANGE", None: "NUM"}[NUMERIC[tt]]
        elif tt in lists and isinstance(lists[tt], list):
            names, disp = lists[tt], "ENUM"
            if len(names) < mx + 1:
                fail(f"byte {off}: {tt} has {len(names)} names for max {mx}")
            names = names[: mx + 1]
        else:
            fail(f"byte {off}: unknown value list {tt!r}")
        centre = (mx + 1) // 2 if disp in ("SIGNED", "CENT", "RANGE") else 0
        if tt == "oscShift":
            centre = 2
        if tt in ("assignOsc2RangeFineWide",):
            centre = 127                                 # those ranges are 77..177 / 27..227 around 127
        dfl = d.get("default", mn)
        if not mn <= dfl <= mx:
            fail(f"byte {off}: default {dfl} outside {mn}..{mx}")
        for k in range(size):
            if off + k >= NBYTES or covered[off + k] is not None:
                fail(f"byte {off + k} covered twice")
            covered[off + k] = o["key"]
        params.append(dict(off=off, size=size, min=mn, max=mx, dfl=dfl, centre=centre, disp=disp, names=names,
                           cc=0xFF, **o))
    keys = [p["key"] for p in params]
    if len(set(keys)) != len(keys):
        fail("duplicate keys: " + ", ".join(k for k in keys if keys.count(k) > 1))
    for off in ovl:
        if off not in je:
            fail(f"overlay byte {off} is not in the JE table")
    reserved = [i for i in range(NBYTES) if covered[i] is None]
    # CC map (JP MODE2): JE controllerMap names -> our parameter
    idx_of_off = {p["off"]: n for n, p in enumerate(params)}
    ccmap = [0xFF] * 128
    for e in j["controllerMap"]:
        off = byname.get(e["param"])
        if off is None or off not in idx_of_off:
            fail(f"CC {e['cc']}: {e['param']} not found")
        if ccmap[e["cc"]] != 0xFF:
            fail(f"CC {e['cc']} mapped twice")
        ccmap[e["cc"]] = idx_of_off[off]
        params[idx_of_off[off]]["cc"] = e["cc"]
    # init patch
    init = [0] * NBYTES
    nm = INIT_NAME.ljust(NAME_LEN)
    for i in range(NAME_LEN):
        init[i] = ord(nm[i])
    for p in params:
        if p["size"] == 2:
            init[p["off"]], init[p["off"] + 1] = p["dfl"] >> 7, p["dfl"] & 0x7F
        else:
            init[p["off"]] = p["dfl"]
    at = [0xFF] * NBYTES
    for n, p in enumerate(params):
        at[p["off"]] = n

    L = []
    w = L.append
    w("/* SPDX-License-Identifier: GPL-3.0-only */")
    w("/* GENERATED by tools/gen_params.py from docs/ref/parameterDescriptions_je.json and tools/mvance-params.yaml.")
    w(" * Do not edit: change the YAML (or the JE file) and run the script. Committed, like Dinghy's other generated")
    w(" * headers, so a build needs no Python. */")
    w("#pragma once")
    w("#include <stdint.h>")
    w("")
    w(f"#define MV_PATCH_BYTES {NBYTES}u          /* a JP-8080 patch; a JP-8000 patch is the first 239 */")
    w(f"#define MV_NAME_OFF 0u")
    w(f"#define MV_NAME_LEN {NAME_LEN}u")
    w(f"#define MV_NPARAMS {len(params)}u")
    w("#define MV_NONE 0xFFu                  /* 'no parameter' in MV_PARAM_AT / MV_CC_PARAM */")
    w("")
    w("enum mv_disp {                         /* how a value is shown (the stored byte is always 0..max) */")
    w("    MV_D_NUM,                          /* the number as stored */")
    w("    MV_D_SIGNED,                       /* value - centre (+-64, +-127, oct shift -2..+2) */")
    w("    MV_D_ENUM,                         /* names[value] */")
    w("    MV_D_CENT,                         /* value - centre, in cents (OSC2 FINE) */")
    w("    MV_D_RANGE                         /* OSC2 RANGE: 0 = -WIDE, 50 = +WIDE, else value - 25 semitones */")
    w("};")
    w("enum mv_sec { " + ", ".join(f"MV_SEC_{s}" for s in SECTIONS) + ", MV_NSEC };")
    w("enum mv_status { " + ", ".join(f"MV_ST_{s}" for s in STATUSES) + " };")
    w("")
    w("typedef struct {")
    w("    uint8_t off;                       /* first patch byte */")
    w("    uint8_t size;                      /* 1, or 2: 0..254 sent as two 7-bit bytes, high (0/1) first */")
    w("    uint8_t min, max, def;             /* stored range and init value */")
    w("    uint8_t centre;                    /* the zero of a signed display, else 0 */")
    w("    uint8_t disp, sec, status;         /* enum mv_disp, mv_sec, mv_status */")
    w("    uint8_t cc;                        /* its JP MODE2 CC number, MV_NONE if none */")
    w("    const char *label;                 /* panel label, <= 7 characters */")
    w("    const char *const *names;          /* value names (MV_D_ENUM), else 0 */")
    w("} mv_param_t;")
    w("")
    w("enum mv_param_id {")
    for n, p in enumerate(params):
        w(f"    MV_P_{p['key']} = {n},".ljust(36) + f"/* byte {p['off']}: {p['law']}; {p['jt']} */")
    w("};")
    w("")
    for p in params:
        if p["names"]:
            w(f"static const char *const MV_N_{p['key']}[] = {{" + ", ".join(c_str(s) for s in p["names"]) + "};")
    w("")
    w("static const mv_param_t MV_PARAMS[MV_NPARAMS] = {")
    for p in params:
        nm = f"MV_N_{p['key']}" if p["names"] else "0"
        w(f"    {{{p['off']}, {p['size']}, {p['min']}, {p['max']}, {p['dfl']}, {p['centre']}, MV_D_{p['disp']}, "
          f"MV_SEC_{p['sec']}, MV_ST_{p['status']}, {p['cc']}, {c_str(p['label'])}, {nm}}},")
    w("};")
    w("")

    def table(name, vals, per=16):
        w(f"static const uint8_t {name}[{len(vals)}] = {{")
        for i in range(0, len(vals), per):
            w("    " + ", ".join(str(v) for v in vals[i:i + per]) + ",")
        w("};")
    table("MV_PARAM_AT", at)
    table("MV_CC_PARAM", ccmap)
    table("MV_PATCH_INIT", init)
    w("")
    w("/* reserved bytes (in no JE parameter; kept as received): " + ", ".join(map(str, reserved)) + " */")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(L) + "\n", encoding="utf-8")
    print(f"gen_params: {len(params)} parameters, {sum(c != 0xFF for c in ccmap)} CCs, reserved bytes {reserved} -> {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
