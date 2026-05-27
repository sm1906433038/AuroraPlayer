# setup-libmpv.ps1
# ---------------------------------------------------------------------------
# Downloads the latest libmpv Windows dev SDK, extracts it into
# third_party/mpv/, and produces an MSVC-compatible libmpv-2.lib using
# the Visual Studio "lib.exe" tool.
#
# Requirements (must be installed first by the user):
#   * Visual Studio 2022 with the "Desktop development with C++" workload
#     (we use lib.exe and dumpbin.exe shipped with MSVC)
#   * 7-Zip (we shell out to its 7z.exe to extract the .7z archive)
#
# Run from a regular PowerShell:
#   PS> cd D:\AuroraPlayer
#   PS> .\scripts\setup-libmpv.ps1
#
# Idempotent: re-runs are cheap and safe.
# ---------------------------------------------------------------------------

[CmdletBinding()]
param(
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot ".."))
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # speeds up Invoke-WebRequest

function Write-Section($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)      { Write-Host "    [ok] $msg" -ForegroundColor Green }
function Write-Warn2($msg)   { Write-Host "    [!!] $msg" -ForegroundColor Yellow }
function Die($msg)           { Write-Host "    [x ] $msg" -ForegroundColor Red; exit 1 }

# ---------- paths ----------------------------------------------------------
$ThirdParty = Join-Path $ProjectRoot "third_party"
$MpvDir     = Join-Path $ThirdParty "mpv"
$IncludeDir = Join-Path $MpvDir "include"
$LibDir     = Join-Path $MpvDir "lib"
$TempDir    = Join-Path $ProjectRoot ".cache"

New-Item -ItemType Directory -Force -Path $ThirdParty, $MpvDir, $IncludeDir, $LibDir, $TempDir | Out-Null

# ---------- short-circuit --------------------------------------------------
$ExistingLib = Join-Path $LibDir "libmpv-2.lib"
$ExistingDll = Join-Path $LibDir "libmpv-2.dll"
$ExistingHdr = Join-Path $IncludeDir "mpv\client.h"
if ((Test-Path $ExistingLib) -and (Test-Path $ExistingDll) -and (Test-Path $ExistingHdr)) {
    Write-Section "libmpv already present at $MpvDir"
    Write-Ok "third_party/mpv/lib/libmpv-2.lib"
    Write-Ok "third_party/mpv/lib/libmpv-2.dll"
    Write-Ok "third_party/mpv/include/mpv/client.h"
    Write-Host "    (delete the folder to force a re-download)"
    exit 0
}

# ---------- locate (or bootstrap) a 7z extractor ---------------------------
# We need to unpack a .7z archive. Order of preference:
#   1. A full 7-Zip install (7z.exe)         — handles everything.
#   2. The portable command-line build (7za.exe).
#   3. Auto-download 7zr.exe (~600 KB, official, signed) — *zero install*.
Write-Section "Locating a .7z extractor"
$SevenZip = $null
foreach ($p in @(
        "C:\Program Files\7-Zip\7z.exe",
        "C:\Program Files (x86)\7-Zip\7z.exe",
        "$env:ProgramFiles\7-Zip\7z.exe",
        "$env:ProgramFiles\7-Zip\7za.exe")) {
    if (Test-Path $p) { $SevenZip = $p; break }
}
if (-not $SevenZip) {
    foreach ($name in @('7z.exe','7za.exe','7zr.exe')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { $SevenZip = $cmd.Source; break }
    }
}
if (-not $SevenZip) {
    # Bootstrap: grab 7zr.exe (the standalone .7z-only extractor) into .cache/
    $SevenZip = Join-Path $TempDir "7zr.exe"
    if (-not (Test-Path $SevenZip)) {
        Write-Host "    7-Zip not installed — downloading portable 7zr.exe (~600 KB)"
        try {
            Invoke-WebRequest -Uri "https://www.7-zip.org/a/7zr.exe" `
                              -OutFile $SevenZip -UseBasicParsing
        } catch {
            Die "Failed to download 7zr.exe: $($_.Exception.Message). Either install 7-Zip from https://www.7-zip.org/ or place 7zr.exe at $SevenZip manually."
        }
    }
}
Write-Ok "7z tool: $SevenZip"

# ---------- locate Visual Studio Developer environment ---------------------
Write-Section "Locating Visual Studio (for lib.exe / dumpbin.exe)"

function Import-VsDevEnv {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        Die "vswhere.exe not found. Install Visual Studio 2022 with the C++ workload."
    }
    $vsPath = & $vswhere -latest -products * `
                          -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                          -property installationPath
    if (-not $vsPath) {
        Die "No VS install with C++ tools found. In Visual Studio Installer, modify your install and tick 'Desktop development with C++'."
    }
    $devShell = Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1"
    if (-not (Test-Path $devShell)) {
        Die "Launch-VsDevShell.ps1 missing at $devShell"
    }
    # The dev shell script changes directories. Save & restore.
    Push-Location
    try {
        & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    } finally {
        Pop-Location
    }
}

if (-not (Get-Command lib.exe -ErrorAction SilentlyContinue)) {
    Import-VsDevEnv
}
if (-not (Get-Command lib.exe -ErrorAction SilentlyContinue)) {
    Die "Could not bring lib.exe into PATH. Open 'x64 Native Tools Command Prompt for VS 2022' and re-run this script from there."
}
Write-Ok "MSVC tools on PATH"

# ---------- pick a libmpv SDK build ---------------------------------------
# shinchiro publishes dev SDK builds to SourceForge as:
#     mpv-dev-x86_64-vN-YYYYMMDD-git-XXXXXXX.7z  (vN = LZMA2 variant)
# We query the project's RSS feed to discover the most recent file, then
# download directly from the *real* mirror URL (NOT the /download HTML
# redirect, which would yield an interstitial page).
Write-Section "Resolving latest libmpv dev SDK"
$FeedUrl = "https://sourceforge.net/projects/mpv-player-windows/rss?path=/libmpv"
$Archive = Join-Path $TempDir "mpv-dev.7z"

# Stale 0-byte / HTML files from a previous failed run must go.
if (Test-Path $Archive) {
    $sz = (Get-Item $Archive).Length
    if ($sz -lt 1MB) {
        Write-Warn2 "Removing previous bogus download ($sz bytes)"
        Remove-Item $Archive -Force
    }
}

if (-not (Test-Path $Archive)) {
    Write-Host "    Fetching feed: $FeedUrl"
    [xml]$feed = (Invoke-WebRequest -Uri $FeedUrl -UseBasicParsing).Content
    # Items look like:
    #   <link>https://.../libmpv/mpv-dev-x86_64-v3-20260101-git-abcdef.7z/download</link>
    #   <pubDate>Wed, 01 Jan 2026 …</pubDate>
    $items = $feed.rss.channel.item |
             Where-Object { $_.link -match 'mpv-dev-x86_64-.*\.7z/download' }
    if (-not $items) { Die "RSS feed returned no libmpv-dev entries — SourceForge layout may have changed." }

    # The pubDate field uses a non-standard suffix ("UT"), so we instead
    # sort by the YYYYMMDD datestamp embedded in the filename — which the
    # publisher guarantees increases monotonically.
    $picked = $items |
        ForEach-Object {
            $tag = if ($_.link -match 'mpv-dev-x86_64-(?:v\d+-)?(\d{8})') { $Matches[1] } else { '00000000' }
            [pscustomobject]@{ link = $_.link; tag = $tag }
        } |
        Sort-Object tag -Descending |
        Select-Object -First 1

    # Direct mirror URL: replace /download → ?viasf=1 (forces direct binary).
    $directUrl = ($picked.link -replace '/download$', '') + "?viasf=1"
    Write-Host "    Latest: $($picked.link)"
    Write-Host "    Downloading via $directUrl"

    # SourceForge picks a mirror on first hit and serves an HTTP 302 to it.
    # Invoke-WebRequest follows redirects by default; a UA string avoids the
    # "please wait" interstitial that bots otherwise see.
    $headers = @{ 'User-Agent' = 'curl/8.0' }
    Invoke-WebRequest -Uri $directUrl -OutFile $Archive `
                      -UseBasicParsing -MaximumRedirection 10 -Headers $headers

    # Sanity: anything < 5 MB is certainly not the real SDK (it's ~30 MB).
    $size = (Get-Item $Archive).Length
    if ($size -lt 5MB) {
        Remove-Item $Archive -Force
        Die "Downloaded file is only $size bytes — mirror returned a placeholder. Retry, or manually download `n  $($picked.link)`nand place the .7z file at $Archive then re-run this script."
    }
}
Write-Ok "Archive: $Archive ($([math]::Round((Get-Item $Archive).Length/1MB,1)) MB)"

# ---------- extract --------------------------------------------------------
Write-Section "Extracting"
$ExtractDir = Join-Path $TempDir "mpv-dev"
if (Test-Path $ExtractDir) { Remove-Item -Recurse -Force $ExtractDir }
New-Item -ItemType Directory -Force -Path $ExtractDir | Out-Null
& $SevenZip x -y -o"$ExtractDir" $Archive | Out-Null
Write-Ok "Extracted to $ExtractDir"

# ---------- layout files ---------------------------------------------------
Write-Section "Installing into third_party/mpv"

# Copy headers (mpv/*.h)
$IncSrc = Get-ChildItem -Path $ExtractDir -Recurse -Filter "client.h" |
          Where-Object { $_.DirectoryName -match "\\mpv$" } |
          Select-Object -First 1
if (-not $IncSrc) { Die "Did not find mpv/client.h inside the archive" }
$IncSrcDir = $IncSrc.DirectoryName
$DstMpvInc = Join-Path $IncludeDir "mpv"
if (Test-Path $DstMpvInc) { Remove-Item -Recurse -Force $DstMpvInc }
Copy-Item $IncSrcDir -Destination $DstMpvInc -Recurse
Write-Ok "Headers → $DstMpvInc"

# Copy DLL
$DllSrc = Get-ChildItem -Path $ExtractDir -Recurse -Filter "libmpv-2.dll" |
          Select-Object -First 1
if (-not $DllSrc) {
    # Older archives ship "mpv-2.dll" instead.
    $DllSrc = Get-ChildItem -Path $ExtractDir -Recurse -Filter "mpv-2.dll" |
              Select-Object -First 1
}
if (-not $DllSrc) { Die "Did not find libmpv-2.dll in the archive" }
Copy-Item $DllSrc.FullName -Destination (Join-Path $LibDir "libmpv-2.dll") -Force
Write-Ok "DLL → $LibDir\libmpv-2.dll"

# Some archives include a ready-to-use libmpv.lib / mpv.lib. Prefer it if present.
$PreBuiltLib = Get-ChildItem -Path $ExtractDir -Recurse -Include "libmpv.lib","mpv.lib","libmpv-2.lib" |
               Select-Object -First 1
if ($PreBuiltLib) {
    Copy-Item $PreBuiltLib.FullName (Join-Path $LibDir "libmpv-2.lib") -Force
    Write-Ok "Pre-built MSVC import lib → $LibDir\libmpv-2.lib"
} else {
    # ---------- generate libmpv-2.lib from the DLL --------------------------
    Write-Section "Generating libmpv-2.lib from DLL exports"

    $DllPath = Join-Path $LibDir "libmpv-2.dll"
    $DefPath = Join-Path $LibDir "libmpv-2.def"
    $LibPath = Join-Path $LibDir "libmpv-2.lib"

    $exportsTxt = & dumpbin /EXPORTS $DllPath
    if ($LASTEXITCODE -ne 0) { Die "dumpbin failed" }

    # dumpbin output:
    #     ordinal hint   RVA            name
    #     1       0      00001000       mpv_abort_async_command
    # We grab the 4th column where the second is hex and the third is hex.
    $names = $exportsTxt |
        Where-Object { $_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)' } |
        ForEach-Object { ($_ -split '\s+', 5)[4] } |
        Where-Object { $_ -and $_ -notmatch '^\(' }

    if (-not $names -or $names.Count -lt 10) {
        Die "Could not parse exports from $DllPath (got $($names.Count) names)"
    }

    "LIBRARY libmpv-2"   | Out-File -Encoding ascii $DefPath
    "EXPORTS"            | Out-File -Encoding ascii -Append $DefPath
    $names               | Out-File -Encoding ascii -Append $DefPath
    Write-Ok ".def written ($($names.Count) symbols)"

    & lib /nologo /def:$DefPath /name:libmpv-2.dll /out:$LibPath /machine:x64 | Out-Null
    if ($LASTEXITCODE -ne 0) { Die "lib.exe failed to create $LibPath" }
    Write-Ok ".lib generated → $LibPath"
}

# ---------- summary --------------------------------------------------------
Write-Section "Done"
Get-ChildItem $LibDir | Format-Table Name, Length -AutoSize
Get-ChildItem $IncludeDir -Recurse -File | Select-Object -First 5 | Format-Table FullName
Write-Host ""
Write-Host "Next step:" -ForegroundColor Cyan
Write-Host "    .\scripts\build.ps1"
