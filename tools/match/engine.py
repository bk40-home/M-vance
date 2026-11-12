# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match (WP0): the Engine interface. Everything in tools/match/ except engine_<name>.py talks to a synth only
through this class, so a second backend (the JT-8000, WP8b) can be added without touching the matcher.

An engine is a patch format plus a way to play one note of a patch offline:

  params()                                  the bytes a matcher may move, with their range, kind and search group
  init_patch()                              the engine's INIT patch
  render(patch, note, vel, gate_ms, total_ms)  one held note -> float32 audio, shape (frames, 2), SAMPLE_RATE Hz
  export(patch, path)                       the patch as a file the synth (or its editor) loads

Design: sound-match-proposal-2026-11-11.md, sections 3 and 7 (WP0).
"""
from abc import ABC, abstractmethod
from pathlib import Path
from typing import List, TypedDict

import numpy as np

SAMPLE_RATE = 44100                     # Hz: every render and every measure works at this rate (proposal section 5)

# Param["kind"] values
KIND_CONT = "cont"                      # a continuous value: the optimiser moves it (byte = round(min + x * (max - min)))
KIND_ENUM = "enum"                      # a choice among named values: enumerated, never optimised
KIND_SWITCH = "switch"                  # OFF / ON: enumerated, never optimised

# Param["group"] value for a byte no search stage moves (left at INIT, proposal section 6)
GROUP_FIXED = "fixed"


class Param(TypedDict):
    """One searchable unit of a patch. The first six keys are the WP0 interface; the rest are extras a backend may
    give (a matcher must not need them)."""
    offset: int                         # first patch byte
    key: str                            # the engine's name for it (M-vance: MV_P_<key>, tools/mvance-params.yaml)
    min: int                            # lowest stored value
    max: int                            # highest stored value
    kind: str                           # KIND_CONT / KIND_ENUM / KIND_SWITCH
    group: str                          # search group ("G1".."G7", "LEVEL") or GROUP_FIXED
    size: int                           # stored width in bytes (2: a 0..254 value as two 7-bit bytes, high first)
    default: int                        # the INIT value


class Engine(ABC):
    """A synth the matcher can render. Implementations hold their own build and temporary folder; use as a context
    manager (or call close()) so the temporary files go when the work is done."""

    SAMPLE_RATE = SAMPLE_RATE           # Hz: render() output rate
    PATCH_BYTES = 0                     # bytes: the length of a patch (set by the backend)

    @abstractmethod
    def params(self) -> List[Param]:
        """Every parameter of the patch, in patch-byte order."""

    @abstractmethod
    def init_patch(self) -> bytes:
        """The INIT patch, PATCH_BYTES long."""

    @abstractmethod
    def render(self, patch: bytes, note: int, vel: int, gate_ms: int, total_ms: int) -> np.ndarray:
        """One note of the patch from silence: key down at the backend's fixed onset, held gate_ms, the take ending at
        total_ms. Returns float32 samples in -1..1, shape (frames, 2), SAMPLE_RATE Hz. Deterministic: the same call
        gives the same array, bit for bit."""

    @abstractmethod
    def export(self, patch: bytes, path: Path) -> Path:
        """Write the patch as a file the synth loads; returns the path written."""

    def invert(self, key: str, value: float):
        """The stored byte whose law gives value (a time in s, a frequency in Hz, a level 0..1: whatever the
        backend documents for key), or None when the backend has no law for key (default). Used to turn the
        analysis's measurements into seed bytes (WP2)."""
        return None

    def close(self) -> None:
        """Release the backend's temporary files (default: nothing to release)."""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False
