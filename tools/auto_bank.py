#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Write a bank list from every bank file in docs/presets (Kris 3 Nov: drop a file in, get it in the bank).

  python3 tools/auto_bank.py                  -> tools/bank/auto.txt (the list, as tools/bank/factory.txt)
  python3 tools/make_bank.py --auto           does this, then builds the bank from auto.txt

Each .mid / .syx in docs/presets becomes a source (A, B, C .. in file-name order). The 64 slots are then filled with
a broad selection, the same groups as the hand-made factory list:

  slots  1-16 leads (LD)   17-28 pads (PD)   29-40 basses (BS)   41-48 plucks, arps (PL, AR)
        49-56 brass, strings, bells, keys (BR, ST, BL)   57-64 effects and the rest (FX)

  - a program's group comes from its name: the prefix (LD, BA, PD, PLK ..) or, for performance names like
    "DST Lange BS", the last word; a name with neither goes to the last group
  - within a group the files take turns, so every file is in the bank
  - a layered performance (DUAL / SPLIT) is taken whole (D:P26), a single performance by its upper patch
  - left out: a patch whose sound (bytes 16..247) is already in, and the "empty" ones (the same sound in three or
    more places of the files: an init patch saved under many names)
  - a group short of programs leaves its slots to the others (the files' turns again, each program then listed in
    its own group), so the 64 are always filled
  - names keep the source's (a performance's: its own), the group's two letters in front ("DST Lange BS" ->
    "BS Lange"); a name without a tag stays as it is; 16 characters at most

The list it writes is an ordinary one: edit it (names, a changed value, a different pick) and build it with
python3 tools/make_bank.py tools/bank/auto.txt. tools/bank/factory.txt, the hand-made list, is not touched.
"""
import collections
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_bank as mb                                   # its readers: the same parse as the build

ROOT = mb.ROOT
NPROG = mb.NPROG
# a name's tag -> the group's two letters
TAGS = {"LD": "LD", "LEAD": "LD", "PD": "PD", "PAD": "PD", "BA": "BS", "BS": "BS", "BASS": "BS",
        "PL": "PL", "PLK": "PL", "PLUCK": "PL", "AR": "AR", "ARP": "AR", "SQ": "AR", "SEQ": "AR",
        "BR": "BR", "BRS": "BR", "BRASS": "BR", "ST": "ST", "STR": "ST", "STRINGS": "ST",
        "BL": "BL", "BEL": "BL", "BELL": "BL", "KY": "BL", "KEY": "BL", "KEYS": "BL", "EP": "BL", "ORG": "BL",
        "FX": "FX", "SFX": "FX", "SE": "FX", "PERC": "FX", "DR": "FX"}
# the groups in slot order, with their size: the same layout as the hand-made factory list
GROUPS = [(("LD",), 16), (("PD",), 12), (("BS",), 12), (("PL", "AR"), 8), (("BR", "ST", "BL"), 8), (("FX", None), 8)]
GENERIC = {"INIT PATCH", "JP AU/VST PLUGIN", ""}         # names that say nothing: use the performance's


def text(b):
    return bytes(x if 32 <= x <= 126 else 63 for x in b[:16]).decode("ascii").rstrip()


def tag_of(name):
    """(the group's letters or None, the name without its tag)."""
    w = name.split()
    if w and w[0].upper() in TAGS:
        return TAGS[w[0].upper()], " ".join(w[1:])
    if len(w) > 1 and w[-1].upper() in TAGS:             # "DST Lange BS": the tag at the end (drop a maker prefix
        rest = w[:-1]                                    # of three capitals, DST, as the tag now leads)
        if len(rest) > 1 and rest[0].isupper() and len(rest[0]) <= 3:
            rest = rest[1:]
        return TAGS[w[-1].upper()], " ".join(rest)
    return None, name


def candidates(letter, pats):
    """Every program a source offers: (ref, name, group, sound fingerprint)."""
    out = []
    for key, v in pats.items():
        if key.startswith("U"):                          # a user patch
            out.append((f"{letter}:{int(key[1:])}", text(v), bytes(v[16:])))
        elif isinstance(v, tuple):                       # a whole performance P26: (common, upper, lower)
            com, up, lo = v
            pname, layered = text(com), com[0x10] in (1, 2)
            name = pname if pname.upper() not in GENERIC else text(up)   # (the performance's name: the author's)
            ref = f"{letter}:{key}" if layered else f"{letter}:{key}A"
            out.append((ref, name or pname, bytes(up[16:]) + (bytes(lo[16:]) if layered else b"")))
    res = []
    for ref, name, sound in out:
        g, bare = tag_of(name)
        res.append({"ref": ref, "name": name, "group": g, "bare": bare or name, "sound": sound})
    # (sort: user patches by number, performances by number; the file's own order)
    def order(c):
        r = c["ref"].split(":")[1]
        return int(r[1:].rstrip("AB")) if r.startswith("P") else int(r)
    return sorted(res, key=order)


def main():
    files = sorted(p for p in (ROOT / "docs/presets").iterdir() if p.suffix.lower() in (".mid", ".syx"))
    if not files:
        mb.fail("no .mid / .syx in docs/presets")
    letters = [chr(ord("A") + i) for i in range(len(files))]
    pools = {}
    for L, f in zip(letters, files):
        pools[L] = candidates(L, mb.bank_patches(f))
    seen = collections.Counter(c["sound"] for L in letters for c in pools[L])
    for L in letters:                                    # the empty ones out (the same sound 3+ times)
        pools[L] = [c for c in pools[L] if seen[c["sound"]] < 3]

    taken, used = [], set()

    def pick(groups, n):
        """n programs of these groups, the files taking turns."""
        got, idx = [], {L: 0 for L in letters}
        while len(got) < n:
            moved = False
            for L in letters:
                pool = pools[L]
                while idx[L] < len(pool):
                    c = pool[idx[L]]; idx[L] += 1
                    if c["sound"] in used or (groups is not None and c["group"] not in groups):
                        continue
                    got.append(c); used.add(c["sound"]); moved = True
                    break
                if len(got) == n:
                    break
            if not moved:
                break
        return got

    for groups, n in GROUPS:
        taken.append(pick(groups, n))
    short = NPROG - sum(len(t) for t in taken)
    extra = pick(None, short) if short else []           # a group short: the rest from anything left, in turns
    for c in extra:                                      # each into its own group's section (slots stay grouped)
        gi = next((i for i, (gs, _) in enumerate(GROUPS) if c["group"] in gs), len(GROUPS) - 1)
        taken[gi].append(c)
    flat = [c for t in taken for c in t]
    if len(flat) < NPROG:
        mb.fail(f"only {len(flat)} different programs in docs/presets: 64 are needed")

    out = ["# M-vance bank, written by tools/auto_bank.py from every file in docs/presets: do not edit by hand if",
           "# you will run it again (it is rewritten); copy it to a list of your own to keep changes. The format:",
           "# tools/bank/factory.txt. Build: python3 tools/make_bank.py tools/bank/auto.txt",
           ""]
    out += [f"source {L} {f.relative_to(ROOT).as_posix()}" for L, f in zip(letters, files)]
    out.append("")
    titles = ["leads", "pads", "basses", "plucks and arps", "brass, strings, bells, keys", "effects and the rest"]
    slot = 1
    for gi, t in enumerate(taken):
        if not t:
            continue
        out.append(f"# ---- {titles[gi]} ({len(t)})")
        for c in t:
            g = c["group"]                       # (no tag: the name as it is, no chip on the home screen)
            name = (f"{g} {c['bare']}" if g else c["name"])[:16].rstrip()
            out.append(f'{slot:2}  {c["ref"]:<8} "{name.replace(chr(34), chr(39))}"{"":{17 - len(name)}}# {c["name"]}')
            slot += 1
        out.append("")
    dest = ROOT / "tools/bank/auto.txt"
    with open(dest, "w", newline="\n") as f:          # (LF on Windows too)
        f.write("\n".join(out) + "\n")
    counts = collections.Counter(c["ref"][0] for c in flat)
    print(f"auto_bank: {dest.relative_to(ROOT).as_posix()}: 64 programs from " +
          ", ".join(f"{f.name} ({counts[L]})" for L, f in zip(letters, files)))


if __name__ == "__main__":
    main()
