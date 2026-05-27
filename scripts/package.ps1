# package.ps1
# ---------------------------------------------------------------------------
# Bundle a self-contained portable zip of AuroraPlayer for distribution.
#
# Usage:
#   PS> .\scripts\package.ps1                  # CUDA build, version 0.1.0
#   PS> .\scripts\package.ps1 -Version 0.2.0
#   PS> .\scripts\package.ps1 -Variant cpu     # skip CUDA DLLs
#
# Outputs:
#   dist\AuroraPlayer-<Version>-win64-<Variant>.zip
#   dist\AuroraPlayer-<Version>-win64-<Variant>\  (uncompressed staging)
# ---------------------------------------------------------------------------

[CmdletBinding()]
param(
    [string]$Version = '0.1.0',
    [ValidateSet('cuda','cpu')]
    [string]$Variant = 'cuda',
    [string]$BuildBin = ''
)

$ErrorActionPreference = 'Stop'

function Write-Section($m) { Write-Host ""; Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok($m)      { Write-Host "    [ok] $m" -ForegroundColor Green }
function Write-Warn($m)    { Write-Host "    [!]  $m" -ForegroundColor Yellow }
function Die($m)           { Write-Host "    [x]  $m" -ForegroundColor Red; exit 1 }

$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $BuildBin) { $BuildBin = Join-Path $ProjectRoot "build\bin" }

# ---------- 0. Verify build -----------------------------------------------
Write-Section "Verifying build"
$exe = Join-Path $BuildBin "AuroraPlayer.exe"
if (-not (Test-Path $exe)) { Die "AuroraPlayer.exe not found at $exe — run scripts\build.ps1 first." }
Write-Ok "exe: $exe"

# ---------- 1. Prepare staging dir ----------------------------------------
$distDir   = Join-Path $ProjectRoot "dist"
$stageName = "AuroraPlayer-$Version-win64-$Variant"
$stageDir  = Join-Path $distDir $stageName
$zipPath   = Join-Path $distDir "$stageName.zip"

Write-Section "Preparing staging dir"
if (Test-Path $stageDir) {
    Remove-Item -Recurse -Force $stageDir
}
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
Write-Ok "stage: $stageDir"

# ---------- 2. Copy build/bin -> stage ------------------------------------
Write-Section "Copying build artifacts"
Copy-Item -Path (Join-Path $BuildBin "*") -Destination $stageDir -Recurse -Force

# Drop a vc_redist.exe we accidentally have lying inside build\bin into the
# stage as well (useful for users without admin rights to get MSVC runtime).
# (Already covered by the recursive copy above — just note it for the user.)
$vcRedist = Get-ChildItem $stageDir -Filter 'vc_redist*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($vcRedist) { Write-Ok "Bundled VC++ redist: $($vcRedist.Name)" }

# ---------- 3. CUDA runtime DLLs (variant=cuda only) ----------------------
if ($Variant -eq 'cuda') {
    Write-Section "Bundling CUDA runtime DLLs"
    if (-not $env:CUDA_PATH) {
        Write-Warn "CUDA_PATH not set; trying default v12.8 install path"
        $env:CUDA_PATH = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8"
    }
    $cudaBin = Join-Path $env:CUDA_PATH "bin"
    if (-not (Test-Path $cudaBin)) { Die "CUDA bin dir not found: $cudaBin" }
    Write-Ok "CUDA bin: $cudaBin"

    # cuBLAS pulls in a couple of giant data DLLs (.dll's named cublasLt*) that
    # contain GEMM kernels for various GPU architectures. We need at least the
    # base ones — ggml-cuda.dll links against cudart + cublas + cublasLt.
    $cudaDlls = @(
        'cudart64_12.dll',
        'cublas64_12.dll',
        'cublasLt64_12.dll'
    )
    foreach ($dll in $cudaDlls) {
        $src = Join-Path $cudaBin $dll
        if (-not (Test-Path $src)) {
            Write-Warn "Missing $dll — runtime may fail on user machines without CUDA Toolkit"
            continue
        }
        Copy-Item -Path $src -Destination $stageDir -Force
        $sz = [math]::Round((Get-Item $src).Length / 1MB, 1)
        Write-Ok ("Copied {0,-22}  ({1} MB)" -f $dll, $sz)
    }
}

# ---------- 4. App-local MSVC runtime (optional belt-and-suspenders) ------
Write-Section "Bundling MSVC runtime DLLs (app-local)"
$sysDir = "$env:SystemRoot\System32"
$msvcDlls = @(
    'vcruntime140.dll',
    'vcruntime140_1.dll',
    'msvcp140.dll',
    'msvcp140_1.dll',
    'msvcp140_2.dll'
)
foreach ($dll in $msvcDlls) {
    $src = Join-Path $sysDir $dll
    if (Test-Path $src) {
        Copy-Item -Path $src -Destination $stageDir -Force
        Write-Ok "Copied $dll"
    } else {
        Write-Warn "Missing system $dll (skipping)"
    }
}

# ---------- 5. Generate user-facing docs ----------------------------------
# Templates live in scripts/templates/ as standalone UTF-8 files. We render
# them with simple {{TOKEN}} substitution. Keeping CJK strings out of this
# .ps1 sidesteps Windows PowerShell 5.1's ANSI-by-default parser.
Write-Section "Writing README / LICENSES"

$tmplDir = Join-Path $PSScriptRoot "templates"

# Read templates as raw UTF-8 bytes -> string (Get-Content -Raw -Encoding UTF8
# works for files with or without BOM on PS 5.1+).
$readmeTmpl = Get-Content -Raw -Encoding UTF8 -Path (Join-Path $tmplDir "README.template.txt")

$cudaFrag = ''
if ($Variant -eq 'cuda') {
    $cudaFrag = Get-Content -Raw -Encoding UTF8 -Path (Join-Path $tmplDir "README.cuda-fragment.txt")
}

# String.Replace is plain literal, no regex meta issues. PowerShell can't
# split a method chain across lines (the dot must follow the receiver on
# the same line), so we apply replacements sequentially.
$readme = $readmeTmpl
$readme = $readme.Replace('{{VERSION}}', $Version)
$readme = $readme.Replace('{{VARIANT}}', $Variant)
$readme = $readme.Replace('{{CUDA_REQUIREMENTS}}', $cudaFrag)

$readmePath = Join-Path $stageDir "README.txt"
# UTF-8 with BOM so Notepad (and even Windows 10 pre-1903 Notepad) opens it
# without mojibake.
$utf8Bom = New-Object System.Text.UTF8Encoding($true)
[System.IO.File]::WriteAllText($readmePath, $readme, $utf8Bom)
Write-Ok "README.txt"

$licensesSrc = Join-Path $tmplDir "THIRD-PARTY-LICENSES.txt"
Copy-Item -Path $licensesSrc -Destination (Join-Path $stageDir "THIRD-PARTY-LICENSES.txt") -Force
Write-Ok "THIRD-PARTY-LICENSES.txt"

# ---------- 6. Zip --------------------------------------------------------
Write-Section "Creating zip archive"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $zipPath -CompressionLevel Optimal -Force

$zipSize  = (Get-Item $zipPath).Length
$stageBytes = (Get-ChildItem $stageDir -Recurse -File | Measure-Object Length -Sum).Sum
Write-Section "Done"
Write-Host ("    Stage : {0}  ({1:N1} MB)" -f $stageDir, ($stageBytes / 1MB))
Write-Host ("    Zip   : {0}  ({1:N1} MB)" -f $zipPath,  ($zipSize / 1MB))
Write-Host ""
Write-Host "    Test on a clean Windows machine before distributing." -ForegroundColor Yellow
