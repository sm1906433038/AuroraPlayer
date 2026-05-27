# build.ps1
# ---------------------------------------------------------------------------
# One-shot build wrapper. Uses the Ninja generator (much faster than
# MSBuild and avoids the platform-toolset version mismatches that occur
# on newer VS installs where v143 is missing or has been renumbered).
#
# Usage:
#   PS> .\scripts\build.ps1                # Release
#   PS> .\scripts\build.ps1 -Config Debug
#   PS> .\scripts\build.ps1 -Clean         # nuke build/ first
#   PS> .\scripts\build.ps1 -Run           # launch AuroraPlayer.exe after build
# ---------------------------------------------------------------------------

[CmdletBinding()]
param(
    [ValidateSet('Release','Debug','RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Run,
    [string]$QtRoot = $env:QT_ROOT,
    # MSVC toolset version (e.g. '14.42'). Auto-picked below when empty:
    # if CUDA Toolkit is installed we prefer 14.42 because the very-newest
    # cl.exe (14.51+) makes nvcc's cudafe++ segfault. Override by passing
    # `-Toolset 14.51` (or whatever) to force a specific version.
    [string]$Toolset = ''
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $ProjectRoot

function Write-Section($m) { Write-Host ""; Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok($m)      { Write-Host "    [ok] $m" -ForegroundColor Green }
function Die($m)           { Write-Host "    [x ] $m" -ForegroundColor Red; exit 1 }

# ---------- ensure libmpv is in place --------------------------------------
$lib = Join-Path $ProjectRoot "third_party\mpv\lib\libmpv-2.lib"
if (-not (Test-Path $lib)) {
    Write-Section "libmpv not set up — running setup-libmpv.ps1 first"
    & (Join-Path $PSScriptRoot "setup-libmpv.ps1")
}

# ---------- locate Qt ------------------------------------------------------
Write-Section "Locating Qt 6"
if (-not $QtRoot) {
    $candidates = @()
    foreach ($drv in 'C','D','E') {
        $root = "${drv}:\Qt"
        if (Test-Path $root) {
            $candidates += Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '^6\.' } |
                ForEach-Object {
                    Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue |
                        Where-Object { $_.Name -eq 'msvc2022_64' -or $_.Name -eq 'msvc2019_64' }
                }
        }
    }
    if ($candidates.Count -eq 0) {
        Die "No Qt 6 install found. Install Qt 6.5+ via https://www.qt.io/download-qt-installer (component: Qt 6.x → MSVC 2022 64-bit). Then re-run, or set `$env:QT_ROOT to the kit folder."
    }
    $QtRoot = ($candidates | Sort-Object FullName -Descending | Select-Object -First 1).FullName
}
if (-not (Test-Path (Join-Path $QtRoot "bin\windeployqt.exe"))) {
    Die "Path $QtRoot does not look like a Qt kit (missing bin\windeployqt.exe)"
}
Write-Ok "Qt: $QtRoot"

# ---------- locate Visual Studio + import dev environment ------------------
Write-Section "Locating Visual Studio 2022"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Die "vswhere.exe not found — install Visual Studio 2022 with the C++ workload."
}
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) { Die "VS2022 with C++ tools not found." }
Write-Ok "VS:  $vsPath"

# Auto-pick MSVC toolset when caller didn't request one. Rule of thumb:
# if a CUDA Toolkit is present and we haven't been told to skip it, prefer
# a 14.42-or-older cl.exe — newer ones make nvcc segfault.
if (-not $Toolset) {
    $cudaInstalled = ($env:CUDA_PATH -and (Test-Path $env:CUDA_PATH)) `
                     -and -not $env:AURORAPLAYER_DISABLE_CUDA
    if ($cudaInstalled) {
        $msvcRoot = Join-Path $vsPath "VC\Tools\MSVC"
        if (Test-Path $msvcRoot) {
            $preferred = Get-ChildItem $msvcRoot -Directory |
                Where-Object { $_.Name -like "14.4*" } |
                Sort-Object Name | Select-Object -Last 1
            if ($preferred) {
                # vcvars wants "14.42" form, not "14.42.34433".
                $Toolset = ($preferred.Name -split '\.')[0..1] -join '.'
                Write-Host "    Auto-selected toolset $Toolset for CUDA compatibility (use -Toolset to override)"
            }
        }
    }
}

# Bring cl.exe / link.exe / Windows SDK headers onto PATH if not already.
# We always re-enter when a specific Toolset was requested, since a previously
# loaded dev-shell may have selected a different cl.exe.
$needDevShell = $Toolset -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)
if ($needDevShell) {
    Write-Host "    Importing VS dev environment$(if ($Toolset) { " (toolset=$Toolset)" })..."
    # Call VsDevCmd.bat directly: Launch-VsDevShell.ps1's parameter surface
    # varies across VS updates, but VsDevCmd.bat has accepted -vcvars_ver=
    # since 2019. We invoke it inside cmd.exe, then ask cmd to dump the
    # resulting environment and we re-apply it to the current PowerShell.
    $vsDevCmd = Join-Path $vsPath "Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path $vsDevCmd)) { Die "VsDevCmd.bat missing at $vsDevCmd" }

    $extraArgs = "-arch=amd64 -host_arch=amd64 -no_logo"
    if ($Toolset) { $extraArgs += " -vcvars_ver=$Toolset" }

    $cmdLine = "`"$vsDevCmd`" $extraArgs && set"
    $envDump = & cmd.exe /d /c $cmdLine 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host ($envDump -join "`n")
        Die "VsDevCmd.bat failed (exit $LASTEXITCODE)"
    }
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2]
        }
    }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Die "Could not load MSVC into PATH. Open 'x64 Native Tools Command Prompt for VS 2022' and re-run from there."
}
Write-Ok "cl.exe: $((Get-Command cl.exe).Source)"

# ---------- locate Ninja ---------------------------------------------------
Write-Section "Locating Ninja"
$ninja = Get-Command ninja.exe -ErrorAction SilentlyContinue
if (-not $ninja) {
    $vsNinja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    if (Test-Path $vsNinja) {
        $env:PATH = "$(Split-Path $vsNinja);$env:PATH"
        $ninja = Get-Command ninja.exe -ErrorAction SilentlyContinue
    }
}
if (-not $ninja) { Die "ninja.exe not found. Install Ninja or include 'C++ CMake tools for Windows' in the VS Installer." }
Write-Ok "Ninja: $($ninja.Source)"

# ---------- locate CMake ---------------------------------------------------
Write-Section "Locating CMake"
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if (-not $cmake) {
    $vsCMake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
    if (Test-Path (Join-Path $vsCMake "cmake.exe")) {
        $env:PATH = "$vsCMake;$env:PATH"
        $cmake = Get-Command cmake.exe
    }
}
if (-not $cmake) { Die "cmake.exe not found." }
Write-Ok "CMake: $($cmake.Source)"

# ---------- configure ------------------------------------------------------
$buildDir = Join-Path $ProjectRoot "build"
if ($Clean -and (Test-Path $buildDir)) {
    Write-Section "Cleaning $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

Write-Section "Configuring (Ninja, $Config)"
$qtPrefix = ($QtRoot -replace '\\','/')

& cmake -S . -B $buildDir -G "Ninja" `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_PREFIX_PATH=$qtPrefix" `
    "-DCMAKE_C_COMPILER=cl" `
    "-DCMAKE_CXX_COMPILER=cl"

if ($LASTEXITCODE -ne 0) { Die "cmake configure failed" }

# ---------- build ----------------------------------------------------------
Write-Section "Building"
& cmake --build $buildDir -j
if ($LASTEXITCODE -ne 0) { Die "build failed" }

# ---------- locate produced exe -------------------------------------------
$exe = Join-Path $buildDir "bin\AuroraPlayer.exe"
if (-not (Test-Path $exe)) {
    $found = Get-ChildItem $buildDir -Recurse -Filter AuroraPlayer.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { $exe = $found.FullName }
}

Write-Section "Done"
if (Test-Path $exe) {
    Write-Ok "Built: $exe"
    if ($Run) {
        Write-Section "Running"
        & $exe
    }
} else {
    Die "Build completed but AuroraPlayer.exe was not found under $buildDir"
}
