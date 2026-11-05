# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kris (bk40-home)
<#
.SYNOPSIS
  M-vance: unit tests, build and install on the FM-1 in one go (Windows).

.DESCRIPTION
  1. tests   WSL: sh tests/run_unit.sh             (skip with -SkipTests)
  2. build   WSL: sh build.sh                       -> build\mvance.fwsc
  3. install Windows Python: tools\fm1_install.py build\mvance.fwsc --yes
             (USB-MIDI: WSL cannot see the FM-1's MIDI port, so this step runs on Windows)
  Each step stops the script if it fails. The install needs no buttons: the running firmware is asked to enter
  update mode over USB-MIDI, the loader writes the package, the FM-1 restarts and its identity is checked.

  WSL builds this folder in place (the Windows tree, as /mnt/c/...), in the default distro, through a login
  shell, so JIELI_TOOLCHAIN / AC79_SDK set in your WSL profile are used.

  Once, on Windows:  py -m pip install mido python-rtmidi

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\flash.ps1
  .\flash.ps1 -SkipTests
  .\flash.ps1 -NoInstall          (tests and build only)
  .\flash.ps1 -Port "Felucca"     (pick the MIDI port by part of its name)
#>
param(
    [switch]$SkipTests,          # do not run tests/run_unit.sh first
    [switch]$NoInstall,          # stop after the build
    [string]$Port = "",          # MIDI port name filter for fm1_install.py (default: it finds the FM-1 itself)
    [switch]$NoPause             # do not wait for Enter at the end (when run from a terminal or a task)
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$pkg = Join-Path $root "build\mvance.fwsc"

function Finish([int]$code, [string]$msg) {
    if ($code -eq 0) { Write-Host "`n$msg" -ForegroundColor Green }
    else { Write-Host "`n$msg" -ForegroundColor Red }
    if (-not $NoPause) { Read-Host "Press Enter to close" | Out-Null }
    exit $code
}

# runs one command in WSL in this folder; returns its exit code
function Invoke-Wsl([string]$what, [string]$cmd) {
    Write-Host "`n== $what (WSL: $cmd)" -ForegroundColor Cyan
    # -e: bash gets $cmd as one argument, not re-parsed by another shell; | Out-Host: the output goes to the
    # console, not into this function's return value (which must be the exit code alone)
    & wsl.exe --cd "$root" -e bash -lc "$cmd" | Out-Host
    return $LASTEXITCODE
}

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { Finish 1 "wsl.exe not found: install WSL first." }

# ---- 1. unit tests (the host C compiler is needed for them; the device build is not)
if (-not $SkipTests) {
    $rc = Invoke-Wsl "unit tests" "command -v cc >/dev/null || { echo 'no cc in WSL: sudo apt install build-essential (or use -SkipTests)'; exit 1; }; sh tests/run_unit.sh"
    if ($rc -ne 0) { Finish $rc "Unit tests FAILED (exit $rc): nothing built or installed." }
}

# ---- 2. build
$before = if (Test-Path $pkg) { (Get-Item $pkg).LastWriteTime } else { [datetime]::MinValue }
$rc = Invoke-Wsl "build" "sh build.sh"
if ($rc -ne 0) { Finish $rc "Build FAILED (exit $rc): nothing installed." }
if (-not (Test-Path $pkg) -or (Get-Item $pkg).LastWriteTime -le $before) {
    Finish 1 "Build reported success but $pkg was not rewritten: not installing an old package."
}
Write-Host ("Package: {0} ({1:N0} bytes, {2})" -f $pkg, (Get-Item $pkg).Length, (Get-Item $pkg).LastWriteTime)
if ($NoInstall) { Finish 0 "Tests and build done (-NoInstall)." }

# ---- 3. install over USB-MIDI with Windows Python (the py launcher, else python)
$py = Get-Command py.exe -ErrorAction SilentlyContinue
if ($py) { $pyArgs = @("-3") } else { $py = Get-Command python.exe -ErrorAction SilentlyContinue; $pyArgs = @() }
if (-not $py) { Finish 1 "No Windows Python found (py.exe / python.exe). Install it, then: py -m pip install mido python-rtmidi" }
$inst = @((Join-Path $root "tools\fm1_install.py"), $pkg, "--yes")
if ($Port) { $inst += @("--port", $Port) }
Write-Host "`n== install (Windows: $($py.Name) tools\fm1_install.py)" -ForegroundColor Cyan
& $py.Source @pyArgs @inst
$rc = $LASTEXITCODE

# fm1_install.py's exit codes (its docstring)
$hint = @{
    0 = "Installed and identity checked: M-vance is running."
    1 = "Install cancelled or failed."
    2 = "Bad arguments or package, or mido / python-rtmidi missing: py -m pip install mido python-rtmidi"
    3 = "FM-1 not found. Is it plugged in and on? Close anything holding its MIDI port (bank.html in Chrome, a DAW): Windows MIDI ports are exclusive."
    4 = "Connection lost during the install. Run flash.ps1 -SkipTests again: an interrupted install is finished by the next one."
    5 = "Timeout: the loader or the restart never came. Run again; if the FM-1 stays black, hold OCT- at power-on (USB rescue)."
    6 = "Wrong model or another identity after the install (or the FM-1 is in another firmware's update mode)."
    130 = "Interrupted. If the FM-1 is in update mode, run the install again to finish it."
}
$msg = $hint[$rc]
if (-not $msg) { $msg = "Install ended with exit $rc." }
Finish $rc $msg
