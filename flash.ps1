# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kris (bk40-home)
<#
.SYNOPSIS
  M-vance: build the firmware and flash the FM-1, build the web simulator, rebuild the factory bank, or serve the
  sim - from one script (Windows). Run with no arguments for an interactive menu; pass switches to skip it.

.DESCRIPTION
  Run  .\flash.ps1  with no arguments and pick from a menu. Or drive it directly:
    -Build firmware (default)  WSL unit tests, WSL `sh build.sh` -> build\mvance.fwsc, install over USB-MIDI.
    -Build sim                 WSL `sh sim/build.sh` -> docs\sim (WebAssembly). Needs Emscripten (emsdk) in WSL.
    -Build all                 the firmware flow, then the simulator.
    -Serve                     after a sim build (or on its own), serve docs\ on http://localhost:<ServePort>.
    -Bank <file.syx>           rebuild the factory bank first (make_bank --from-device); path is repo-relative.
    -SkipTests / -NoInstall    as before.

  WSL builds this folder in place (/mnt/c/...) through a login shell, so JIELI_TOOLCHAIN / AC79_SDK (firmware)
  and the emsdk env (sim) set in your WSL profile are used.
  Once, on Windows:   py -m pip install mido python-rtmidi
  Once, in WSL (sim): install emsdk, then add  source ~/emsdk/emsdk_env.sh  to your ~/.bashrc

.EXAMPLE
  .\flash.ps1                                   menu
  .\flash.ps1 -Build sim -Serve                 build the simulator and serve it
  .\flash.ps1 -Build all -Serve                 firmware (tests+build+flash), then sim, then serve
  .\flash.ps1 -Bank mvance-bank-renamed.syx     rebuild the factory bank first, then build + flash
#>
param(
    [ValidateSet('firmware','sim','all')]
    [string]$Build = 'firmware',
    [switch]$SkipTests,
    [switch]$NoInstall,
    [string]$Bank = "",
    [switch]$Serve,
    [int]$ServePort = 8000,
    [string]$Port = "",
    [switch]$NoPause
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$pkg = Join-Path $root "build\mvance.fwsc"

function Finish([int]$code, [string]$msg) {
    if ($code -eq 0) { Write-Host "`n$msg" -ForegroundColor Green } else { Write-Host "`n$msg" -ForegroundColor Red }
    if (-not $NoPause) { Read-Host "Press Enter to close" | Out-Null }
    exit $code
}

function Invoke-Wsl([string]$what, [string]$cmd) {
    Write-Host "`n== $what (WSL: $cmd)" -ForegroundColor Cyan
    & wsl.exe --cd "$root" -e bash -lc "$cmd" | Out-Host
    return $LASTEXITCODE
}

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { Finish 1 "wsl.exe not found: install WSL first." }

# what to run, as plain flags (set from the menu, or from the switches above)
$doTests = $false; $doFirmware = $false; $doInstall = $false; $doSim = $false; $doServe = $false; $bankFile = $Bank

if ($PSBoundParameters.Count -eq 0) {
    # ---- interactive menu (no arguments were given) ----
    Write-Host ""
    Write-Host "  M-vance" -ForegroundColor Yellow
    Write-Host "  1  Firmware: test, build, flash the FM-1   (default)"
    Write-Host "  2  Firmware: test and build only (no flash)"
    Write-Host "  3  Simulator: build (docs\sim)"
    Write-Host "  4  Simulator: build and serve"
    Write-Host "  5  Everything: firmware flash + simulator + serve"
    Write-Host "  6  Rebuild factory bank from a .syx, then firmware flash"
    Write-Host "  7  Serve docs only (no build)"
    Write-Host "  Q  Quit"
    $choice = Read-Host "Choose [1]"
    if ([string]::IsNullOrWhiteSpace($choice)) { $choice = "1" }
    switch ($choice.ToUpper()) {
        "1" { $doTests = $true; $doFirmware = $true; $doInstall = $true }
        "2" { $doTests = $true; $doFirmware = $true }
        "3" { $doSim = $true }
        "4" { $doSim = $true; $doServe = $true }
        "5" { $doTests = $true; $doFirmware = $true; $doInstall = $true; $doSim = $true; $doServe = $true }
        "6" { $bankFile = Read-Host "Factory bank .syx (relative to the repo)"
              if ([string]::IsNullOrWhiteSpace($bankFile)) { Finish 1 "No bank file given." }
              $doTests = $true; $doFirmware = $true; $doInstall = $true }
        "7" { $doServe = $true }
        "Q" { Finish 0 "Nothing to do." }
        default { Finish 1 "Unknown choice '$choice'." }
    }
} else {
    # ---- driven by switches ----
    $doFirmware = $Build -in @('firmware','all')
    $doSim      = $Build -in @('sim','all')
    $doTests    = $doFirmware -and -not $SkipTests
    $doInstall  = $doFirmware -and -not $NoInstall
    $doServe    = [bool]$Serve
}

# ---- 0. rebuild the factory bank first (optional): make_bank --from-device <file.syx>
if ($bankFile) {
    $rc = Invoke-Wsl "bank" "python3 tools/make_bank.py --from-device '$bankFile'"
    if ($rc -ne 0) { Finish $rc "Bank rebuild FAILED (exit $rc): nothing else built." }
}

# ---- 1. unit tests (the host C compiler is needed for them; the device build is not)
if ($doTests) {
    $rc = Invoke-Wsl "unit tests" "command -v cc >/dev/null || { echo 'no cc in WSL: sudo apt install build-essential (or skip tests)'; exit 1; }; sh tests/run_unit.sh"
    if ($rc -ne 0) { Finish $rc "Unit tests FAILED (exit $rc): nothing built or installed." }
}

# ---- 2. firmware build (+ flash over USB-MIDI)
if ($doFirmware) {
    $before = if (Test-Path $pkg) { (Get-Item $pkg).LastWriteTime } else { [datetime]::MinValue }
    $rc = Invoke-Wsl "build" "sh build.sh"
    if ($rc -ne 0) { Finish $rc "Build FAILED (exit $rc): nothing installed." }
    if (-not (Test-Path $pkg) -or (Get-Item $pkg).LastWriteTime -le $before) {
        Finish 1 "Build reported success but $pkg was not rewritten: not installing an old package."
    }
    Write-Host ("Package: {0} ({1:N0} bytes, {2})" -f $pkg, (Get-Item $pkg).Length, (Get-Item $pkg).LastWriteTime)

    if ($doInstall) {
        # install with Windows Python (USB-MIDI: WSL cannot see the FM-1's MIDI port, so this runs on Windows)
        $py = Get-Command py.exe -ErrorAction SilentlyContinue
        if ($py) { $pyArgs = @("-3") } else { $py = Get-Command python.exe -ErrorAction SilentlyContinue; $pyArgs = @() }
        if (-not $py) { Finish 1 "No Windows Python found. Install it, then: py -m pip install mido python-rtmidi" }
        $inst = @((Join-Path $root "tools\fm1_install.py"), $pkg, "--yes")
        if ($Port) { $inst += @("--port", $Port) }
        Write-Host "`n== install (Windows: $($py.Name) tools\fm1_install.py)" -ForegroundColor Cyan
        & $py.Source @pyArgs @inst
        $rc = $LASTEXITCODE
        $hint = @{
            0   = "Installed and identity checked: M-vance is running."
            1   = "Install cancelled or failed."
            2   = "Bad arguments or package, or mido / python-rtmidi missing: py -m pip install mido python-rtmidi"
            3   = "FM-1 not found. Is it plugged in and on? Close anything holding its MIDI port (bank.html in Chrome, a DAW): Windows MIDI ports are exclusive."
            4   = "Connection lost during the install. Run again (option 1 / -SkipTests): an interrupted install is finished by the next one."
            5   = "Timeout: the loader or the restart never came. Run again; if the FM-1 stays black, hold OCT- at power-on (USB rescue)."
            6   = "Wrong model or another identity after the install (or the FM-1 is in another firmware's update mode)."
            130 = "Interrupted. If the FM-1 is in update mode, run the install again to finish it."
        }
        $msg = $hint[$rc]; if (-not $msg) { $msg = "Install ended with exit $rc." }
        if ($rc -ne 0) { Finish $rc $msg }        # firmware flash failed: stop (don't go on to the sim)
        Write-Host "`n$msg" -ForegroundColor Green
    } else {
        Write-Host "`nFirmware built: $pkg" -ForegroundColor Green
    }
}

# ---- 3. simulator build (WebAssembly into docs\sim)
if ($doSim) {
    $rc = Invoke-Wsl "simulator" "[ -f ~/emsdk/emsdk_env.sh ] && . ~/emsdk/emsdk_env.sh >/dev/null 2>&1; command -v emcc >/dev/null || { echo 'no emcc in WSL: install emsdk and source ~/emsdk/emsdk_env.sh'; exit 1; }; sh sim/build.sh"
    if ($rc -ne 0) { Finish $rc "Simulator build FAILED (exit $rc)." }
    Write-Host "`nSimulator built -> docs\sim" -ForegroundColor Green
}

# ---- 4. serve docs\ for the simulator / bank editor (blocks until Ctrl-C)
if ($doServe) {
    Write-Host "`n== serve  http://localhost:$ServePort/sim/   (bank editor: /sim/bank/editor.html)   Ctrl-C to stop" -ForegroundColor Cyan
    Invoke-Wsl "serve" "python3 -m http.server --directory docs $ServePort" | Out-Null
    Finish 0 "Server stopped."
}

Finish 0 "Done."
