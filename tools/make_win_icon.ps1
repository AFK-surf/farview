# Generates a multi-resolution Windows .ico (PNG-in-ICO, Vista+) from a square source PNG.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/make_win_icon.ps1 -InputPng assets/icon.png -OutputIco out/AppIcon.ico
param(
  [Parameter(Mandatory = $true)][string]$InputPng,
  [Parameter(Mandatory = $true)][string]$OutputIco
)
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

function New-RoundedRectPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
  $d = $r * 2.0
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $path.AddArc($x, $y, $d, $d, 180, 90)
  $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
  $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
  $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
  $path.CloseFigure()
  return $path
}

function Render-RoundedPng([System.Drawing.Image]$src, [int]$size) {
  $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  try {
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
      $g.Clear([System.Drawing.Color]::Transparent)
      $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
      $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
      $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
      $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality

      $k = $size / 1024.0
      $edge = [float](32.0 * $k)
      $side = [float](960.0 * $k)
      $radius = [float](192.0 * $k)
      $path = New-RoundedRectPath $edge $edge $side $side $radius
      try {
        $g.SetClip($path)
        $destRect = New-Object System.Drawing.RectangleF($edge, $edge, $side, $side)
        $g.DrawImage($src, $destRect)
      } finally {
        $path.Dispose()
      }
    } finally {
      $g.Dispose()
    }
    $ms = New-Object System.IO.MemoryStream
    try {
      $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
      # Unary comma prevents PowerShell from unrolling byte[] into object[] in the pipeline.
      return , $ms.ToArray()
    } finally {
      $ms.Dispose()
    }
  } finally {
    $bmp.Dispose()
  }
}

$resolvedIn = (Resolve-Path $InputPng).Path
$outParent = Split-Path -Parent $OutputIco
if ($outParent -and -not (Test-Path $outParent)) {
  New-Item -ItemType Directory -Force -Path $outParent | Out-Null
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 256)
$pngs = New-Object 'System.Collections.Generic.List[byte[]]'
$srcImg = [System.Drawing.Image]::FromFile($resolvedIn)
try {
  foreach ($s in $sizes) {
    [byte[]]$buf = Render-RoundedPng $srcImg $s
    if ($buf.Length -lt 64) {
      throw "make_win_icon: rendered ${s}x${s} PNG unexpectedly small ($($buf.Length) bytes)"
    }
    $pngs.Add($buf)
  }
} finally {
  $srcImg.Dispose()
}

$outStream = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($outStream)
try {
  # ICONDIR header
  $bw.Write([UInt16]0)
  $bw.Write([UInt16]1)
  $bw.Write([UInt16]$sizes.Count)

  $offset = [UInt32](6 + (16 * $sizes.Count))
  for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $b = if ($s -ge 256) { [byte]0 } else { [byte]$s }
    [byte[]]$bytes = $pngs[$i]
    $bw.Write($b)
    $bw.Write($b)
    $bw.Write([byte]0)
    $bw.Write([byte]0)
    $bw.Write([UInt16]1)
    $bw.Write([UInt16]32)
    $bw.Write([UInt32]$bytes.Length)
    $bw.Write([UInt32]$offset)
    $offset += [UInt32]$bytes.Length
  }
  $bw.Flush()
  for ($i = 0; $i -lt $sizes.Count; $i++) {
    [byte[]]$bytes = $pngs[$i]
    $outStream.Write($bytes, 0, $bytes.Length)
  }
  $icoBytes = $outStream.ToArray()
  if ($icoBytes.Length -ne $offset -or $icoBytes.Length -lt 4096) {
    throw "make_win_icon: ICO size mismatch (got $($icoBytes.Length), expected $offset)"
  }
  [System.IO.File]::WriteAllBytes($OutputIco, $icoBytes)
} finally {
  $bw.Dispose()
  $outStream.Dispose()
}

$kb = [Math]::Round((Get-Item $OutputIco).Length / 1KB, 1)
Write-Host "make_win_icon: $OutputIco (${kb} KB, $($sizes.Count) sizes)"
