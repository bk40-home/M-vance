# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kris (bk40-home)
<#
.SYNOPSIS
  JT-80x0 on VIBE FM: build the .wasm / .zip, run the tests, show the zip for VIBE FM VST, start the emulator - from one
  script (Windows), as flash.ps1 does for the FM-1. Run with no arguments for a menu; pass switches to skip it.

.DESCRIPTION
  Run  .\vibe.ps1  with no arguments and pick from a menu. Or drive it directly (switches combine; they run in this
  order: build, tests, zip, emulator):
    -Build        WSL `sh vibe/build.sh`: the engine copied in, the SDK's build, then vibe-check (~1 min more)
                  -> <sdk>\build\jt80x0.wasm and jt80x0.zip
    -Quick        the same build without vibe-check (CHECK=0)
    -Test         WSL `sh vibe/test/run.sh`: the .wasm against main.c natively, the pages, the DAW, saving, the
                  sequencer (needs a build first)
    -Zip          open Explorer on jt80x0.zip: drag it onto the VIBE FM VST window (it replaces the one there)
    -Emulator     the VIBE in your browser (the SDK's tools/emulator.py, http://127.0.0.1:<Port>); pick JT-80x0 in the
                  top bar. It lists every build in <sdk>\build: build again, reload the page. Ctrl+C here stops it.
    -Sdk <path>   the vibe-fm-sdk checkout (default: next to this repo, ..\vibe-fm-sdk)
    -Port <n>     the emulator's port (default 8766)

  WSL builds this folder in place (/mnt/c/...) through a login shell, so zig (Zig 0.13.0 on PATH, or ZIG=...), node and
  python3 from your WSL profile are used. The emulator runs on Windows Python (py, else python) when there is one, else
  in WSL.

.EXAMPLE
  .\vibe.ps1                       menu
  .\vibe.ps1 -Quick -Emulator      build (no vibe-check), then the emulator
  .\vibe.ps1 -Build -Test          build with vibe-check, then every test
  .\vibe.ps1 -Quick -Zip           build, then show the zip to drag into VIBE FM VST
#>
param(
    [switch]$Build,
    [switch]$Quick,
    [switch]$Test,
    [switch]$Zip,
    [switch]$Emulator,
    [string]$Sdk = "",
    [int]$Port = 8766,
    [switch]$NoPause
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

function Finish([int]$code, [string]$msg) {
    if ($code -eq 0) { Write-Host "`n$msg" -ForegroundColor Green } else { Write-Host "`n$msg" -ForegroundColor Red }
    if (-not $NoPause) { Read-Host "Press Enter to close" | Out-Null }
    exit $code
}

# WSL's output is UTF-8: read as such (else the SDK's dashes show garbled)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

# An interactive login shell (-lic): ~/.bashrc in full, as in your WSL terminal. Ubuntu's ~/.bashrc stops early in a
# shell that is not interactive, before the lines added at its end - nvm's Node, zig on PATH - so -lc finds neither.
# (No double quotes in what goes to wsl.exe: Windows PowerShell strips them - see flash.ps1's -Defs.)
function Invoke-Wsl([string]$what, [string]$cmd) {
    Write-Host "`n== $what (WSL: $cmd)" -ForegroundColor Cyan
    & wsl.exe --cd "$root" -e bash -lic "$cmd" | Out-Host
    return $LASTEXITCODE
}

# ---- the SDK ----
if (-not $Sdk) { $Sdk = Join-Path (Split-Path $root -Parent) "vibe-fm-sdk" }
$Sdk = $Sdk.Trim().Trim('"')
if (-not (Test-Path -LiteralPath (Join-Path $Sdk "tools\build.sh"))) { Finish 1 "No VIBE FM SDK at $Sdk (-Sdk <path>)." }
$Sdk = (Resolve-Path -LiteralPath $Sdk).Path
$wasm = Join-Path $Sdk "build\jt80x0.wasm"
$zipFile = Join-Path $Sdk "build\jt80x0.zip"

# ---- what to run ----
$doBuild = $false; $doCheck = $true; $doTest = $false; $doZip = $false; $doEmu = $false
if ($PSBoundParameters.Count -eq 0 -or ($PSBoundParameters.Count -eq 1 -and $NoPause)) {
    Write-Host ""
    Write-Host "  JT-80x0 on VIBE FM" -ForegroundColor Yellow
    Write-Host "  1  Build + vibe-check, then the tests   (default)"
    Write-Host "  2  Build, quick (no vibe-check)"
    Write-Host "  3  Build quick, then show the zip (drag it onto VIBE FM VST)"
    Write-Host "  4  Build quick, then the emulator (browser)"
    Write-Host "  5  Tests only (the last build)"
    Write-Host "  6  Emulator only (the last build)"
    Write-Host "  7  Show the zip only"
    Write-Host "  Q  Quit"
    $choice = Read-Host "Choose [1]"
    if ([string]::IsNullOrWhiteSpace($choice)) { $choice = "1" }
    switch ($choice.ToUpper()) {
        "1" { $doBuild = $true; $doTest = $true }
        "2" { $doBuild = $true; $doCheck = $false }
        "3" { $doBuild = $true; $doCheck = $false; $doZip = $true }
        "4" { $doBuild = $true; $doCheck = $false; $doEmu = $true }
        "5" { $doTest = $true }
        "6" { $doEmu = $true }
        "7" { $doZip = $true }
        "Q" { Finish 0 "Nothing to do." }
        default { Finish 1 "Unknown choice '$choice'." }
    }
} else {
    $doBuild = [bool]($Build -or $Quick)
    $doCheck = -not $Quick
    $doTest = [bool]$Test
    $doZip = [bool]$Zip
    $doEmu = [bool]$Emulator
    if (-not ($doBuild -or $doTest -or $doZip -or $doEmu)) { Finish 1 "Nothing to do: -Build, -Quick, -Test, -Zip or -Emulator." }
}

if (($doBuild -or $doTest) -and -not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { Finish 1 "wsl.exe not found: install WSL first." }

# ---- the SDK's path as WSL sees it (C:\GitHub\vibe-fm-sdk -> /mnt/c/GitHub/vibe-fm-sdk) ----
$sdkWsl = "/mnt/" + $Sdk.Substring(0, 1).ToLower() + ($Sdk.Substring(2) -replace '\\', '/')
if ($sdkWsl -match "'") { Finish 1 "The SDK's path has a ' in it: $Sdk" }

# ---- build ----
if ($doBuild) {
    $check = if ($doCheck) { "" } else { "CHECK=0 " }
    $rc = Invoke-Wsl "Build JT-80x0 for VIBE FM" "VIBE_SDK='$sdkWsl' ${check}sh vibe/build.sh"
    if ($rc -ne 0) { Finish $rc "Build failed (see above)." }
    Write-Host "`nBuilt: $wasm" -ForegroundColor Green
    Write-Host "       $zipFile"
}

# ---- tests ----
if ($doTest) {
    if (-not (Test-Path -LiteralPath $wasm)) { Finish 1 "No build yet ($wasm): build first." }
    $rc = Invoke-Wsl "Tests" "VIBE_SDK='$sdkWsl' sh vibe/test/run.sh"
    if ($rc -ne 0) { Finish $rc "Tests FAILED (see above)." }
    Write-Host "`nAll tests passed." -ForegroundColor Green
}

# ---- the zip, for VIBE FM VST ----
if ($doZip) {
    if (-not (Test-Path -LiteralPath $zipFile)) { Finish 1 "No zip yet ($zipFile): build first." }
    Write-Host "`nDrag jt80x0.zip onto the VIBE FM VST window: it installs and plays (a new one replaces the old)." -ForegroundColor Cyan
    Start-Process explorer.exe -ArgumentList "/select,`"$zipFile`""
}

# ---- the emulator (last: it runs until Ctrl+C) ----
if ($doEmu) {
    if (-not (Test-Path -LiteralPath $wasm)) { Finish 1 "No build yet ($wasm): build first." }
    $url = "http://127.0.0.1:$Port"
    Write-Host "`n== The emulator: $url  (choose JT-80x0 in the top bar; Chrome or Edge for MIDI; Ctrl+C here stops it)" -ForegroundColor Cyan
    $py = Get-Command py -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command python -ErrorAction SilentlyContinue }
    if ($py -and $py.Source -notmatch 'WindowsApps') {   # (the Store's python stub is no Python)
        & $py.Source (Join-Path $Sdk "tools\emulator.py") $Port   # (it opens the browser itself)
    } else {
        Write-Host "(no Python on Windows: running it in WSL)" -ForegroundColor DarkGray
        Start-Process $url
        & wsl.exe --cd "$Sdk" -e bash -lic "python3 tools/emulator.py $Port" | Out-Host
    }
    Finish 0 "Emulator stopped."
}

Finish 0 "Done."
