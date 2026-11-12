# SPDX-License-Identifier: GPL-3.0-only
"""Sound-match: the start-up check for the Python packages the matcher needs. Standard library only, so it can run
before any of them is imported; match.py and serve.py call require() first and stop with the fix if one is missing.
"""
import importlib.util
import sys

PACKAGES = ["numpy", "scipy", "cma", "soundfile", "matplotlib"]   # what tools/match/ imports beyond the stdlib
INSTALL = ("python3 -m pip install --user --break-system-packages " + " ".join(PACKAGES))
NO_PIP = "sudo apt update && sudo apt install -y python3-pip"


def missing() -> list:
    """The packages of PACKAGES this Python cannot import."""
    return [p for p in PACKAGES if importlib.util.find_spec(p) is None]


def require():
    """Exit with the install command if anything is missing (does nothing when all are there)."""
    gone = missing()
    if not gone:
        return
    sys.exit("\nSound match needs these Python packages, not installed here: " + ", ".join(gone) + "\n\n"
             "Install them once, in WSL (Ubuntu):\n\n    " + INSTALL + "\n\n"
             "If that says 'No module named pip', first run:\n\n    " + NO_PIP + "\n\n"
             "then the line above again, then start sound match again.\n")
