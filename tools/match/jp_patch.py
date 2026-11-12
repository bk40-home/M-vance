# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match (WP0): a 248-byte JP-8080 patch as Roland DT1 SysEx, in the three forms the match tool writes.

  temp   01 00 40 00, one message: the TEMPORARY upper patch, what plays now (not stored). The render harness sends
         this (engine_mvance.py). firmware/src/mv_patch.h MV_ADDR_TEMP_UPPER.
  user   02 bb pp 00, one message: USER PATCH n (0..127, bb = n / 64, pp = 2 * (n % 64)), as the firmware's own
         (match.py writes the found patch this way, as user patch 1: the form every tool reads; Kris 12 Nov, A1)
         mv_sysex_build(.., mv_user_addr(n)) writes it. On M-vance user patch n < 64 is the upper patch of slot n+1
         (firmware/src/mv_xfer.h).
  perf   03 pp 00 / 40 / 42 / 70, four messages: PERFORMANCE pp as tools/make_bank.py writes each factory program
         (common block: name, KEY MODE, SPLIT, PANEL SELECT, BALANCE; the upper patch; the lower patch; M-vance's
         16 performance bytes). A JP-8080 reads the first three, M-vance and tools/mid2bank all four.

Message layout (all forms): F0 41 dev 00 06 12 a3 a2 a1 a0 data.. sum F7, sum = (128 - (address + data) mod 128)
mod 128. dev is 10h, the JP's default (as make_bank.py and mv_xfer.h's MV_XF_DEV).

Engine-neutral: it knows the JP-8080 SysEx format, not the M-vance engine, so a JT-8000 backend can use it too.
test_wp0.py checks the user and perf forms by parsing them back with the firmware's own mv_sysex_parse
(tools/mid2bank.c), and the temp form by rendering it.
"""
from pathlib import Path
from typing import Iterable, List, Optional, Sequence

PATCH_BYTES = 248                       # bytes: a JP-8080 patch (a JP-8000 patch is the first 239)
NAME_BYTES = 16                         # bytes: the name at the start of a patch (ASCII, space padded)
DEV_ID = 0x10                           # Roland device id the messages go out with (the JP's default, 17)
HEADER = (0xF0, 0x41)                   # SysEx start, Roland
MODEL_DT1 = (0x00, 0x06, 0x12)          # JP-8000/8080 model id 00 06, command DT1 (data set)
ADDR_TEMP_UPPER = (0x01, 0x00, 0x40, 0x00)   # the temporary upper patch (mv_patch.h MV_ADDR_TEMP_UPPER)
NUSER = 128                             # user patches a JP-8080 has (2 banks of 64)
NPERF_SLOTS = 64                        # performance slots (M-vance programs 1..64)
NPERF_BYTES = 16                        # M-vance's own performance bytes (mv_perf.h MV_PF_*, mv_xfer.h area 70)
AREA_COMMON, AREA_UPPER, AREA_LOWER, AREA_EXT = 0x00, 0x40, 0x42, 0x70   # performance areas (mv_xfer.h)
# MV_PERF_DEF (mid2bank.c / make_bank.py PERF_DEF): KEY MODE SINGLE, SPLIT 60, BALANCE 64, VOICE SPLIT 3 (4+4),
# channels global, ARP AUTO, every sync OFF
PERF_DEF = bytes([0, 60, 64, 3, 0, 0, 0] + [0] * 9)


def check_patch(patch: Sequence[int]) -> bytes:
    """The patch as bytes, after checking it is PATCH_BYTES long and 7-bit (a DT1 carries nothing else).
    Raises ValueError otherwise. Range clamping is the receiver's job (mv_sysex_parse sanitizes)."""
    b = bytes(patch)
    if len(b) != PATCH_BYTES:
        raise ValueError(f"a JP-8080 patch is {PATCH_BYTES} bytes, not {len(b)}")
    if any(v > 0x7F for v in b):
        raise ValueError("a patch byte is over 127: SysEx data is 7-bit")
    return b


def roland_sum(addr: Iterable[int], data: Iterable[int]) -> int:
    """The Roland checksum of address + data: the value that brings their sum to 0 mod 128."""
    return (128 - (sum(addr) + sum(data)) % 128) % 128


def dt1(addr: Sequence[int], data: Sequence[int], dev: int = DEV_ID) -> bytes:
    """One DT1 message: F0 41 dev 00 06 12, the 4-byte address, the data, the checksum, F7."""
    if len(addr) != 4 or any(not 0 <= v <= 0x7F for v in list(addr) + list(data)):
        raise ValueError("a DT1 address is 4 bytes and every address / data byte is 0..127")
    return bytes(HEADER) + bytes([dev & 0x7F]) + bytes(MODEL_DT1) + bytes(addr) + bytes(data) + \
        bytes([roland_sum(addr, data), 0xF7])


def user_addr(patch_no: int) -> List[int]:
    """User patch 0..127 -> its address 02 bb pp 00 (the firmware's mv_user_addr: pp = 2 * (patch % 64), the odd
    page pp + 1 being the patch's bytes 128..247 when sent in two halves)."""
    if not 0 <= patch_no < NUSER:
        raise ValueError(f"user patch {patch_no}: 0..{NUSER - 1}")
    return [0x02, patch_no >> 6, (patch_no & 63) * 2, 0x00]


def temp_patch_syx(patch: Sequence[int], dev: int = DEV_ID) -> bytes:
    """The temp form: the whole patch to the temporary upper patch, 01 00 40 00, one message (260 bytes)."""
    return dt1(ADDR_TEMP_UPPER, check_patch(patch), dev)


def user_patch_syx(patch: Sequence[int], patch_no: int = 0, dev: int = DEV_ID) -> bytes:
    """The user form: the whole patch to user patch patch_no (0..127), one message (260 bytes)."""
    return dt1(user_addr(patch_no), check_patch(patch), dev)


def perf_syx(patch: Sequence[int], slot: int = 0, lower: Optional[Sequence[int]] = None,
             perf: Sequence[int] = PERF_DEF, dev: int = DEV_ID) -> bytes:
    """The perf form: performance slot (0..63) as tools/make_bank.py writes it, four messages:
         03 pp 00 00  common: the name (patch bytes 0..15), KEY MODE = perf[0], SPLIT = perf[1], PANEL SELECT 0
                      (UPPER), BALANCE = perf[2]
         03 pp 40 00  the upper patch (patch)
         03 pp 42 00  the lower patch (lower; default: a copy of the upper, as a SINGLE program has)
         03 pp 70 00  M-vance's 16 performance bytes (perf; default PERF_DEF: SINGLE, no arp, no sync)"""
    up = check_patch(patch)
    lo = check_patch(lower) if lower is not None else up
    pf = bytes(perf)
    if not 0 <= slot < NPERF_SLOTS:
        raise ValueError(f"performance slot {slot}: 0..{NPERF_SLOTS - 1}")
    if len(pf) != NPERF_BYTES:
        raise ValueError(f"the performance bytes are {NPERF_BYTES}, not {len(pf)}")
    com = up[:NAME_BYTES] + bytes([pf[0], pf[1], 0, pf[2]])
    return (dt1([0x03, slot, AREA_COMMON, 0x00], com, dev) + dt1([0x03, slot, AREA_UPPER, 0x00], up, dev) +
            dt1([0x03, slot, AREA_LOWER, 0x00], lo, dev) + dt1([0x03, slot, AREA_EXT, 0x00], pf, dev))


def write_syx(path: Path, data: bytes) -> Path:
    """Write SysEx bytes to a .syx file (parent folders made); returns the path."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def split_messages(data: bytes) -> List[bytes]:
    """The F0 .. F7 messages of a .syx, in order (anything between messages is skipped)."""
    out, start = [], None
    for i, v in enumerate(data):
        if v == 0xF0:
            start = i
        elif v == 0xF7 and start is not None:
            out.append(bytes(data[start:i + 1]))
            start = None
    return out


def checksum_ok(msg: bytes) -> bool:
    """True if a DT1 message's checksum is right (address + data + sum = 0 mod 128)."""
    return len(msg) >= 12 and sum(msg[6:-1]) % 128 == 0


def patch_name(patch: Sequence[int]) -> str:
    """The patch's name: bytes 0..15 as text, trailing spaces removed."""
    return "".join(chr(c) for c in bytes(patch)[:NAME_BYTES]).rstrip()


def with_name(patch: Sequence[int], name: str) -> bytes:
    """The patch with its name set: printable ASCII only (others become spaces), cut or padded to 16 characters."""
    clean = "".join(c if 32 <= ord(c) < 127 else " " for c in name)[:NAME_BYTES].ljust(NAME_BYTES)
    return clean.encode("ascii") + check_patch(patch)[NAME_BYTES:]
