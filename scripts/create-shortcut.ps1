# create-shortcut.ps1
# ---------------------------------------------------------------------------
# Creates a Start-Menu and (optionally) Desktop shortcut to AuroraPlayer.exe so the
# user can launch it like any other installed program.
#
# Usage:
#   .\scripts\create-shortcut.ps1               # Start Menu only
#   .\scripts\create-shortcut.ps1 -Desktop      # also put one on the Desktop
#   .\scripts\create-shortcut.ps1 -Remove       # remove all created shortcuts
# ---------------------------------------------------------------------------

[CmdletBinding()]
param(
    [switch]$Desktop,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$exe         = Join-Path $ProjectRoot "build\bin\AuroraPlayer.exe"
$workDir     = Split-Path $exe -Parent

# Standard per-user shortcut destinations.
$startMenuDir = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
$startMenuLnk = Join-Path $startMenuDir "AuroraPlayer.lnk"
$desktopLnk   = Join-Path ([Environment]::GetFolderPath('Desktop')) "AuroraPlayer.lnk"

function New-Lnk([string]$LnkPath) {
    $shell = New-Object -ComObject WScript.Shell
    $sc    = $shell.CreateShortcut($LnkPath)
    $sc.TargetPath       = $exe
    $sc.WorkingDirectory = $workDir
    $sc.IconLocation     = "$exe,0"
    $sc.Description      = "AuroraPlayer (晨曦影音) — a pro-grade media player (libmpv + Qt)"
    $sc.WindowStyle      = 1     # normal window
    $sc.Save()
    Write-Host "    [ok] $LnkPath" -ForegroundColor Green
}

if ($Remove) {
    foreach ($p in @($startMenuLnk, $desktopLnk)) {
        if (Test-Path $p) {
            Remove-Item $p -Force
            Write-Host "    [rm] $p" -ForegroundColor Yellow
        }
    }
    exit 0
}

if (-not (Test-Path $exe)) {
    Write-Host "AuroraPlayer.exe not built yet. Run scripts\build.ps1 first." -ForegroundColor Red
    exit 1
}

Write-Host "==> Creating shortcuts" -ForegroundColor Cyan
New-Lnk $startMenuLnk
if ($Desktop) { New-Lnk $desktopLnk }

Write-Host ""
Write-Host "Done. Press the Windows key and type 'AuroraPlayer' to launch." -ForegroundColor Cyan
