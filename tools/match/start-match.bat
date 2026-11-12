@echo off
rem SPDX-License-Identifier: GPL-3.0-only
rem Sound match in the browser: double-click this. It starts tools/match/serve.py in WSL (Ubuntu) from the
rem M-vance folder and opens http://localhost:8765. Close this window (or Ctrl+C) to stop it.
rem Once, in WSL: python3 -m pip install --user --break-system-packages numpy scipy cma soundfile matplotlib
cd /d "%~dp0..\.."
wsl.exe --cd "%CD%" python3 tools/match/serve.py
pause
