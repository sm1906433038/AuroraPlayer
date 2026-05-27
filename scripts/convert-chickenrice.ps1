# convert-chickenrice.ps1
# ---------------------------------------------------------------------------
# One-shot SafeTensors → ggml converter for community Whisper fine-tunes
# (海南鸡 / ChickenRice and friends).
#
# Why this exists:
#   HuggingFace ships these fine-tunes in transformers / SafeTensors format
#   (a folder with model.safetensors + config.json + tokenizer files). The
#   whisper.cpp runtime AuroraPlayer is built on can only load the ggml/gguf
#   binary format. This script does the one-time conversion locally.
#
# What it does:
#   1. Locates Python 3.10+ on PATH (offers next steps if missing).
#   2. Creates an isolated venv under  .cache\whisper-convert-venv  so we
#      don't pollute your system Python.
#   3. Installs the minimal dependency set (CPU-only torch, transformers,
#      numpy, openai-whisper) — about 500 MB on first run, cached after.
#   4. Calls whisper.cpp's convert-h5-to-ggml.py (vendored under
#      build\_deps\whisper_cpp-src\models\).
#   5. Moves the resulting f16 ggml file into the AuroraPlayer models folder
#      with a name ModelManager recognises.
#
# Usage:
#   # auto-pick the first folder under <AppData>/AuroraPlayer/AuroraPlayer/models that
#   # looks like a SafeTensors checkpoint (has model.safetensors + config.json):
#   PS> .\scripts\convert-chickenrice.ps1
#
#   # explicit source folder:
#   PS> .\scripts\convert-chickenrice.ps1 `
#         -SourceDir "C:\path\to\chickenrice...-st"
#
#   # custom output name (must end in .bin):
#   PS> .\scripts\convert-chickenrice.ps1 `
#         -OutputName "ggml-chickenrice-v2.bin"
#
#   # just verify deps without running the conversion:
#   PS> .\scripts\convert-chickenrice.ps1 -CheckOnly
# ---------------------------------------------------------------------------

[CmdletBinding()]
param(
    [string]$SourceDir   = '',
    [string]$ModelsDir   = '',
    [string]$OutputName  = 'ggml-chickenrice-v2.bin',
    [switch]$Force,
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $ProjectRoot

function Write-Section($m) { Write-Host ""; Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok($m)      { Write-Host "    [ok] $m" -ForegroundColor Green }
function Write-Warn($m)    { Write-Host "    [!]  $m" -ForegroundColor Yellow }
function Die($m)           { Write-Host "    [x]  $m" -ForegroundColor Red; exit 1 }

# ---------- 0. Defaults / discovery ----------------------------------------

# ModelsDir defaults to the QStandardPaths::AppDataLocation that AuroraPlayer uses:
#   %AppData%\AuroraPlayer\AuroraPlayer\models   (Windows; %AppData% = %APPDATA% env var)
if (-not $ModelsDir) {
    $ModelsDir = Join-Path $env:APPDATA "AuroraPlayer\AuroraPlayer\models"
}
if (-not (Test-Path $ModelsDir)) {
    Write-Section "Creating models dir"
    New-Item -ItemType Directory -Path $ModelsDir | Out-Null
    Write-Ok $ModelsDir
}

# Auto-detect a SafeTensors source folder under $ModelsDir if not given.
if (-not $SourceDir) {
    Write-Section "Auto-detecting SafeTensors source folder under $ModelsDir"
    $candidates = Get-ChildItem $ModelsDir -Directory -ErrorAction SilentlyContinue |
        Where-Object {
            Test-Path (Join-Path $_.FullName 'model.safetensors')
        }
    if ($candidates.Count -eq 0) {
        Die @"
No SafeTensors source folder found under $ModelsDir.
Either:
  - drop the HuggingFace repo folder there (must contain model.safetensors + config.json), then re-run, OR
  - pass an explicit path:  -SourceDir "C:\path\to\hf-repo-folder"
"@
    }
    if ($candidates.Count -gt 1) {
        Write-Warn "Multiple candidates found; using the largest one:"
        $candidates | ForEach-Object {
            Write-Host ("        " + $_.FullName)
        }
    }
    $SourceDir = ($candidates |
        Sort-Object @{ Expression = {
            (Get-Item (Join-Path $_.FullName 'model.safetensors')).Length
        }; Descending = $true } |
        Select-Object -First 1).FullName
}
$SourceDir = (Resolve-Path $SourceDir).Path
Write-Ok "Source: $SourceDir"

# Sanity-check required files in the HF repo folder.
$requiredFiles = @('model.safetensors', 'config.json', 'vocab.json', 'added_tokens.json')
foreach ($f in $requiredFiles) {
    $p = Join-Path $SourceDir $f
    if (-not (Test-Path $p)) {
        Die "Missing required file: $p (this doesn't look like a HuggingFace Whisper checkpoint folder)"
    }
}
Write-Ok "Source folder has all required HF files"

$finalPath = Join-Path $ModelsDir $OutputName
if ((Test-Path $finalPath) -and -not $Force) {
    Write-Warn "Output already exists: $finalPath"
    Write-Warn "Pass -Force to overwrite. Skipping conversion."
    if (-not $CheckOnly) { exit 0 }
}

# ---------- 1. Locate Python -----------------------------------------------

Write-Section "Locating Python 3.10+"
$python = Get-Command python.exe -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command py.exe -ErrorAction SilentlyContinue }
if (-not $python) {
    Die @"
Python not found on PATH.
Install Python 3.10+ from https://www.python.org/downloads/ (tick
"Add python.exe to PATH" in the installer), then re-open PowerShell.
"@
}

# If we found py.exe (Windows launcher), prefer it as `py -3.10` or similar
# so we don't accidentally use a Python 2 alias. Plain python is fine too.
$pyExe = $python.Source
$verRaw = & $pyExe --version 2>&1
if ($verRaw -notmatch 'Python\s+(\d+)\.(\d+)\.(\d+)') {
    Die "Could not parse Python version: $verRaw"
}
$major = [int]$matches[1]; $minor = [int]$matches[2]
if (($major -lt 3) -or ($major -eq 3 -and $minor -lt 10)) {
    Die "Python $major.$minor is too old. Need 3.10 or newer."
}
Write-Ok "Python: $pyExe ($verRaw)"

# ---------- 2. Create / reuse virtualenv -----------------------------------

$venvDir = Join-Path $ProjectRoot ".cache\whisper-convert-venv"
$venvPython = Join-Path $venvDir "Scripts\python.exe"
$venvPip    = Join-Path $venvDir "Scripts\pip.exe"

if (-not (Test-Path $venvPython)) {
    Write-Section "Creating venv at $venvDir"
    & $pyExe -m venv $venvDir
    if ($LASTEXITCODE -ne 0) { Die "venv create failed" }
}
Write-Ok "Venv: $venvDir"

# ---------- 3. Probe / install conversion dependencies ---------------------

# Probe whether all required packages are already importable.
# IMPORTANT: in -CheckOnly mode we MUST NOT trigger any network install —
# the whole point of that flag is to dry-run the environment cheaply.
$probeScript = @"
import importlib.util, sys
mods = ['torch', 'transformers', 'numpy', 'whisper', 'safetensors']
missing = [m for m in mods if importlib.util.find_spec(m) is None]
if missing:
    print('MISSING ' + ','.join(missing))
    sys.exit(1)
print('all present')
sys.exit(0)
"@
$probeFile = Join-Path $env:TEMP "prompv-convert-probe.py"
$probeScript | Set-Content -Encoding UTF8 $probeFile
$probeOut = & $venvPython $probeFile 2>&1
$haveDeps = ($LASTEXITCODE -eq 0)
Remove-Item $probeFile -ErrorAction SilentlyContinue

if ($haveDeps) {
    Write-Ok "Dependencies already installed"
} elseif ($CheckOnly) {
    Write-Section "CheckOnly: dependencies are NOT installed yet"
    Write-Host "    Probe says: $probeOut" -ForegroundColor DarkGray
    Write-Host "    Re-run without -CheckOnly to install (~500 MB download, one-time)." -ForegroundColor DarkGray
} else {
    Write-Section "Upgrading pip inside venv"
    & $venvPython -m pip install --upgrade pip --disable-pip-version-check 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Die "pip upgrade failed" }
    Write-Ok "pip ready"

    Write-Section "Installing torch (CPU-only) + transformers + openai-whisper + safetensors"
    Write-Host "    This is a one-time download of ~500 MB. Subsequent runs reuse the venv." -ForegroundColor DarkGray
    # CPU-only torch (~200 MB) is plenty — conversion is single-pass on CPU.
    # We skip the CUDA wheel (which would be 2+ GB) deliberately.
    & $venvPip install `
        --index-url https://download.pytorch.org/whl/cpu `
        --extra-index-url https://pypi.org/simple `
        torch
    if ($LASTEXITCODE -ne 0) { Die "torch install failed" }

    & $venvPip install transformers numpy safetensors openai-whisper
    if ($LASTEXITCODE -ne 0) { Die "transformers/whisper install failed" }
    Write-Ok "Dependencies installed"
}

# ---------- 4. Locate the conversion script --------------------------------

Write-Section "Locating convert-h5-to-ggml.py"
$convertScript = Join-Path $ProjectRoot "build\_deps\whisper_cpp-src\models\convert-h5-to-ggml.py"
if (-not (Test-Path $convertScript)) {
    Die @"
Conversion script not found at:
  $convertScript

It only exists after CMake configure has run (the whisper.cpp source tree
is fetched via FetchContent). Build AuroraPlayer at least once first:
  scripts\build.cmd
"@
}
Write-Ok $convertScript

if ($CheckOnly) {
    Write-Section "CheckOnly mode finished"
    if ($haveDeps) {
        Write-Ok "Environment is ready. Re-run without -CheckOnly to convert."
    } else {
        Write-Warn "Re-run without -CheckOnly to download deps and convert (~500 MB one-time)."
    }
    exit 0
}

# ---------- 5. Locate whisper repo assets (post-install) -------------------

# The convert-h5-to-ggml.py script wants the path to the OpenAI whisper repo
# so it can read whisper/assets/mel_filters.npz. The `openai-whisper` pip
# package vendors exactly that file inside its package dir, so we just hand
# the venv's site-packages folder over.
Write-Section "Locating mel_filters.npz from openai-whisper"
$whisperLocate = @"
import os, whisper
# whisper.__path__[0] is .../site-packages/whisper; the convert script
# expects the parent folder (so that <dir>/whisper/assets/... resolves).
pkg_dir = os.path.dirname(whisper.__path__[0])
mel = os.path.join(pkg_dir, 'whisper', 'assets', 'mel_filters.npz')
assert os.path.exists(mel), f'mel_filters.npz not found at {mel}'
print(pkg_dir)
"@
$whisperLocateFile = Join-Path $env:TEMP "prompv-locate-whisper.py"
$whisperLocate | Set-Content -Encoding UTF8 $whisperLocateFile
$pkgDir = & $venvPython $whisperLocateFile 2>&1 | Select-Object -Last 1
Remove-Item $whisperLocateFile -ErrorAction SilentlyContinue
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $pkgDir)) {
    Die "Could not locate openai-whisper assets: $pkgDir"
}
Write-Ok "Whisper assets dir: $pkgDir"

# ---------- 6. Run conversion ----------------------------------------------

# The script writes <dir_out>/ggml-model.bin (f16, ~3 GB for large-v2/v3).
# We run it with the staging dir set to .cache\whisper-convert-out so the
# original models folder doesn't briefly contain a generic "ggml-model.bin".
$stageDir = Join-Path $ProjectRoot ".cache\whisper-convert-out"
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
Remove-Item (Join-Path $stageDir "ggml-model.bin") -ErrorAction SilentlyContinue

# Patch the vendored convert-h5-to-ggml.py for BFloat16 compatibility.
# Many HF fine-tunes (incl. recent ChickenRice variants) ship weights as
# torch.bfloat16, which numpy cannot consume directly — `.numpy()` raises
# "Got unsupported ScalarType BFloat16". We force-cast to float32 first.
# We copy & patch into the stage dir so we never touch the FetchContent
# source tree (a `-Clean` build would wipe a direct edit).
$patchedScript = Join-Path $stageDir "convert-h5-to-ggml.py"
# CRITICAL: read & write the script with explicit UTF-8 encoding.
# PowerShell 5's `Get-Content -Raw` defaults to the system ANSI codepage
# (CP936/GBK on Chinese Windows). The convert script contains non-ASCII
# Latin-1 chars (¡ ¬ ® ÿ) inside `bytes_to_unicode()` — if these get
# round-tripped through CP936 they become unrelated CJK chars (隆 卢 庐 每),
# silently destroying the byte_decoder map. The model still converts, but
# every high-byte token gets double-encoded and you end up with mojibake
# Chinese in every subtitle. Took a while to track down — please don't
# remove this UTF-8 encoding without leaving the comment.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$content = [System.IO.File]::ReadAllText($convertScript, $utf8NoBom)

# Patch 1: BFloat16 → float32 before .numpy().
#   Many recent HF fine-tunes ship bf16 SafeTensors, which numpy refuses.
$needle1  = 'data = list_vars[src].squeeze().numpy()'
$replace1 = 'data = list_vars[src].squeeze().to(torch.float32).numpy()  # patched: bf16/f16 -> f32'
if ($content.Contains($needle1)) {
    $content = $content.Replace($needle1, $replace1)
    Write-Ok "Patch 1: BFloat16 compatibility applied"
} elseif ($content.Contains('.to(torch.float32).numpy()')) {
    Write-Ok "Patch 1: already BFloat16-safe"
} else {
    Write-Warn "Patch 1: pattern not found, conversion may fail on bf16 weights"
}

# Patch 2: tolerate vocab tokens that aren't byte-level BPE encoded.
#   ChickenRice / Chinese-fine-tuned variants add raw CJK chars to vocab.json
#   (e.g. "中国") that aren't keys of byte_decoder. The original script does
#   `bytearray([byte_decoder[c] for c in key[0]])` and dies with KeyError.
#   We fall back to UTF-8 encoding for any char not in the BPE byte map.
$needle2  = '    text = bytearray([byte_decoder[c] for c in key[0]])'
$replace2 = @'
    # patched: tolerate non-BPE tokens (raw CJK in fine-tuned vocabs)
    text = bytearray()
    for _c in key[0]:
        if _c in byte_decoder:
            text.append(byte_decoder[_c])
        else:
            text.extend(_c.encode('utf-8'))
'@
if ($content.Contains($needle2)) {
    $content = $content.Replace($needle2, $replace2)
    Write-Ok "Patch 2: CJK vocab tokens handled"
} elseif ($content.Contains('# patched: tolerate non-BPE tokens')) {
    Write-Ok "Patch 2: already CJK-safe"
} else {
    Write-Warn "Patch 2: pattern not found, conversion may fail on CJK vocab tokens"
}

[System.IO.File]::WriteAllText($patchedScript, $content, $utf8NoBom)

Write-Section "Converting SafeTensors → ggml (f16). This takes 3–10 minutes."
Write-Host "    Source : $SourceDir"      -ForegroundColor DarkGray
Write-Host "    Whisper: $pkgDir"         -ForegroundColor DarkGray
Write-Host "    Stage  : $stageDir"       -ForegroundColor DarkGray
Write-Host "    Script : $patchedScript"  -ForegroundColor DarkGray
Write-Host "    Tip    : the script prints one line per tensor (~1260 total" -ForegroundColor DarkGray
Write-Host "             for large-v2). If you don't see lines flowing, "    -ForegroundColor DarkGray
Write-Host "             it's still loading SafeTensors weights, ~30s."      -ForegroundColor DarkGray
Write-Host ""

# -u = unbuffered stdout/stderr so progress shows up in real time,
# otherwise PowerShell buffers >1000 lines and the run looks frozen.
& $venvPython -u $patchedScript $SourceDir $pkgDir $stageDir
if ($LASTEXITCODE -ne 0) { Die "convert-h5-to-ggml.py failed (exit $LASTEXITCODE)" }

$stagedFile = Join-Path $stageDir "ggml-model.bin"
if (-not (Test-Path $stagedFile)) {
    Die "Conversion reported success but ggml-model.bin not found at $stagedFile"
}

# Quick sanity check on the magic bytes. whisper.cpp writes the magic as a
# native-byte-order int, which on Windows is little-endian — so the on-disk
# byte order is reversed from the ASCII string. Read 4 bytes and compare
# the reconstructed LE uint32 against the known magic values.
#
# Stream-read because the f16 ggml is ~3 GB and ReadAllBytes() refuses
# files >2 GB on PowerShell 5.
$bytes = New-Object byte[] 4
$fs = [System.IO.File]::OpenRead($stagedFile)
try { [void]$fs.Read($bytes, 0, 4) } finally { $fs.Close() }
$magic = [BitConverter]::ToUInt32($bytes, 0)
$hex   = ($bytes | ForEach-Object { '{0:x2}' -f $_ }) -join ' '
$known = @(0x67676d6c, 0x67676d66, 0x67676a74, 0x46554747)  # ggml/ggmf/ggjt/GGUF
if (-not ($known -contains $magic)) {
    Die ("Output file has wrong magic ({0} on disk, uint32 LE = 0x{1:x8}); " +
         "expected one of ggml/ggmf/ggjt/GGUF") -f $hex, $magic
}
Write-Ok ("Magic verified: {0} on disk = 0x{1:x8}" -f $hex, $magic)

# ---------- 7. Move into AuroraPlayer models dir ---------------------------

if (Test-Path $finalPath) { Remove-Item -Force $finalPath }
Move-Item -Path $stagedFile -Destination $finalPath
Remove-Item -Recurse -Force $stageDir -ErrorAction SilentlyContinue

$sizeMB = [math]::Round((Get-Item $finalPath).Length / 1MB, 1)
Write-Section "Done"
Write-Ok "Output: $finalPath  ($sizeMB MB)"
Write-Host ""
Write-Host "Open AuroraPlayer → AI 字幕 → 字幕生成…, the dropdown entry" -ForegroundColor White
Write-Host "  '海南鸡 v2 · 日→中专用'" -ForegroundColor White
Write-Host "should now show ✓ 已安装 pointing at this file." -ForegroundColor White
