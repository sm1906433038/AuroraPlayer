# make-icon.ps1
# Build a multi-resolution Windows .ico from a single high-res PNG.
#
# We embed each size as a PNG payload inside the ICO container (Vista+ supports
# this and Windows Explorer / shell, taskbar and File Properties all read it).
# That gives us crisp icons at every common display size without authoring
# bitmap variants by hand.
#
# Usage:
#   pwsh -NoProfile -ExecutionPolicy Bypass `
#        -File scripts/make-icon.ps1 `
#        -SourcePng resources/icons/app.png `
#        -OutputIco resources/icons/app.ico

[CmdletBinding()]
param(
    [string]$SourcePng = "$PSScriptRoot\..\resources\icons\app.png",
    [string]$OutputIco = "$PSScriptRoot\..\resources\icons\app.ico",
    [int[]] $Sizes     = @(256, 128, 64, 48, 32, 24, 16)
)

$ErrorActionPreference = 'Stop'

# ---- Resolve paths --------------------------------------------------------
$SourcePng = [System.IO.Path]::GetFullPath($SourcePng)
$OutputIco = [System.IO.Path]::GetFullPath($OutputIco)

if (-not (Test-Path -LiteralPath $SourcePng)) {
    Write-Error "Source PNG not found: $SourcePng"
    exit 1
}

$outDir = [System.IO.Path]::GetDirectoryName($OutputIco)
if (-not (Test-Path -LiteralPath $outDir)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}

Add-Type -AssemblyName System.Drawing

Write-Host "make-icon: $SourcePng  ->  $OutputIco"
Write-Host "make-icon: sizes = $($Sizes -join ', ')"

# ---- Normalise source to a true square --------------------------------------
# Two scenarios:
#   (a) Opaque source with dark padding (legacy): crop the content bbox by
#       thresholding on RGB-sum, then paste onto a square canvas filled with
#       the source's own background color so the band blends in.
#   (b) Source with a real alpha channel (modern, e.g. a rounded-icon design
#       on a transparent background): preserve transparency end-to-end —
#       detect bbox by alpha, paste onto a transparent square canvas so the
#       rendered .ico cells stay transparent at the corners.
#
# We auto-pick (a) or (b) by checking whether the source has any pixel with
# alpha < 255. A single fully-transparent corner pixel is enough to flip us
# into the alpha-preserving branch — which is the right thing to do because
# filling those transparent areas with a sampled "background color" (which
# would be (0,0,0,0) for a transparent corner) produces a solid black bar.
$src = [System.Drawing.Image]::FromFile($SourcePng)
try {
    # Force 32bppArgb so the LockBits scan below has predictable layout.
    $srcBmp = New-Object System.Drawing.Bitmap $src.Width, $src.Height,
                                               ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $gCopy  = [System.Drawing.Graphics]::FromImage($srcBmp)
    $gCopy.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $gCopy.DrawImage($src, 0, 0, $src.Width, $src.Height)
    $gCopy.Dispose()
} finally {
    $src.Dispose()
}

$srcW = $srcBmp.Width
$srcH = $srcBmp.Height

# Single LockBits scan: classify the source and compute both an alpha-based
# and an RGB-based content bbox so we can pick the right one below.
$rect = New-Object System.Drawing.Rectangle 0, 0, $srcW, $srcH
$data = $srcBmp.LockBits($rect,
            [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
try {
    $stride = $data.Stride
    $totalBytes = $stride * $srcH
    $buffer = New-Object byte[] $totalBytes
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buffer, 0, $totalBytes)

    $hasAlpha = $false
    $aMinX =  [int]::MaxValue; $aMinY =  [int]::MaxValue
    $aMaxX = -1;               $aMaxY = -1
    $cMinX =  [int]::MaxValue; $cMinY =  [int]::MaxValue
    $cMaxX = -1;               $cMaxY = -1
    $alphaThr = 16     # alpha cutoff — anything more opaque is "content"
    $rgbThr   = 24     # RGB-sum cutoff for the legacy (opaque) branch

    for ($y = 0; $y -lt $srcH; $y++) {
        $rowBase = $y * $stride
        for ($x = 0; $x -lt $srcW; $x++) {
            $p = $rowBase + ($x * 4)
            $b = $buffer[$p]
            $g = $buffer[$p + 1]
            $r = $buffer[$p + 2]
            $a = $buffer[$p + 3]

            if ($a -lt 255) { $hasAlpha = $true }

            if ($a -gt $alphaThr) {
                if ($x -lt $aMinX) { $aMinX = $x }
                if ($x -gt $aMaxX) { $aMaxX = $x }
                if ($y -lt $aMinY) { $aMinY = $y }
                if ($y -gt $aMaxY) { $aMaxY = $y }
            }
            if (($r + $g + $b) -gt $rgbThr) {
                if ($x -lt $cMinX) { $cMinX = $x }
                if ($x -gt $cMaxX) { $cMaxX = $x }
                if ($y -lt $cMinY) { $cMinY = $y }
                if ($y -gt $cMaxY) { $cMaxY = $y }
            }
        }
    }
} finally {
    $srcBmp.UnlockBits($data)
}

if ($hasAlpha) {
    # Branch (b): preserve transparency.
    if ($aMaxX -lt 0) {
        $aMinX = 0; $aMinY = 0; $aMaxX = $srcW - 1; $aMaxY = $srcH - 1
    }
    $minX = $aMinX; $minY = $aMinY; $maxX = $aMaxX; $maxY = $aMaxY
} else {
    # Branch (a): legacy opaque-with-dark-padding source.
    if ($cMaxX -lt 0) {
        $cMinX = 0; $cMinY = 0; $cMaxX = $srcW - 1; $cMaxY = $srcH - 1
    }
    $minX = $cMinX; $minY = $cMinY; $maxX = $cMaxX; $maxY = $cMaxY
}

$bbW  = $maxX - $minX + 1
$bbH  = $maxY - $minY + 1
$side = [Math]::Max($bbW, $bbH)

Write-Host ("make-icon: source {0}x{1}; alpha={2}; content bbox {3}x{4} at ({5},{6}); square canvas {7}x{7}" -f $srcW, $srcH, $hasAlpha, $bbW, $bbH, $minX, $minY, $side)

# Compose onto a square canvas. Fill color depends on the source kind.
$square  = New-Object System.Drawing.Bitmap $side, $side,
                                            ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$gSquare = [System.Drawing.Graphics]::FromImage($square)

if ($hasAlpha) {
    # Transparent canvas — corners and any other transparent regions in the
    # source stay transparent in every .ico cell.
    $gSquare.Clear([System.Drawing.Color]::Transparent)
} else {
    # Sample the source's outer padding color and fill the band with it
    # (so the legacy "dark gradient" look blends seamlessly).
    $bg = $srcBmp.GetPixel(0, 0)
    if ($bg.A -lt 255) {
        $bg = [System.Drawing.Color]::FromArgb(255, $bg.R, $bg.G, $bg.B)
    }
    $gSquare.Clear($bg)
}

$gSquare.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$gSquare.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
$gSquare.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$gSquare.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
# Critical: copy RGBA verbatim, do NOT blend over the (transparent) canvas.
$gSquare.CompositingMode    = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy

# Paste the source's content bbox centered on the square canvas.
$srcContent = New-Object System.Drawing.Rectangle $minX, $minY, $bbW, $bbH
$pasteX     = [int][Math]::Round(($side - $bbW) / 2.0)
$pasteY     = [int][Math]::Round(($side - $bbH) / 2.0)
$dstContent = New-Object System.Drawing.Rectangle $pasteX, $pasteY, $bbW, $bbH
$gSquare.DrawImage($srcBmp, $dstContent, $srcContent,
                   [System.Drawing.GraphicsUnit]::Pixel)
$gSquare.Dispose()
$srcBmp.Dispose()

# ---- Render PNG payloads at every target size ------------------------------
$payloads = New-Object System.Collections.Generic.List[byte[]]
foreach ($size in $Sizes) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size,
                                            ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.CompositingMode    = [System.Drawing.Drawing2D.CompositingMode]::SourceOver
    $g.DrawImage($square, 0, 0, $size, $size)
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $payloads.Add($ms.ToArray())
    $ms.Dispose()
    $bmp.Dispose()
}
$square.Dispose()

# ---- Build the ICO container ----------------------------------------------
# Layout:
#   ICONDIR        (6 bytes)         : reserved=0 / type=1 / count=N
#   ICONDIRENTRY[] (16 bytes each)   : per-image metadata + offset
#   <image data>   (variable)        : PNG bytes per image, in order

$n        = $payloads.Count
$dirSize  = 6 + ($n * 16)
$out      = New-Object System.IO.MemoryStream
$bw       = New-Object System.IO.BinaryWriter $out

# ICONDIR
$bw.Write([uint16]0)   # reserved
$bw.Write([uint16]1)   # type 1 = icon (vs 2 = cursor)
$bw.Write([uint16]$n)  # image count

# Directory entries
$offset = $dirSize
for ($i = 0; $i -lt $n; $i++) {
    $size  = $Sizes[$i]
    $data  = $payloads[$i]
    # 256-pixel size is encoded as 0 in the 1-byte width/height fields.
    $wh    = if ($size -ge 256) { 0 } else { $size }
    $bw.Write([byte]$wh)              # width
    $bw.Write([byte]$wh)              # height
    $bw.Write([byte]0)                # color count (0 = 32bpp)
    $bw.Write([byte]0)                # reserved
    $bw.Write([uint16]1)              # color planes
    $bw.Write([uint16]32)             # bits per pixel
    $bw.Write([uint32]$data.Length)   # data size
    $bw.Write([uint32]$offset)        # offset into file
    $offset += $data.Length
}

# Image data
for ($i = 0; $i -lt $n; $i++) {
    $bw.Write($payloads[$i])
}

$bw.Flush()
[System.IO.File]::WriteAllBytes($OutputIco, $out.ToArray())
$bw.Dispose()
$out.Dispose()

$sz = (Get-Item -LiteralPath $OutputIco).Length
Write-Host ("make-icon: wrote {0} ({1:N0} bytes, {2} sizes)" -f $OutputIco, $sz, $n)
