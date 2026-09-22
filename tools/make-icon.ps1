# Generates assets/pdfboard.ico (256x256 PNG-in-ICO) with no third-party tools.
# The mark matches the app language: blue rounded tile, white page, red ink
# stroke. Re-run it if the brand colours in Theme.h change.
[CmdletBinding()]
param(
    [string]$OutFile = (Join-Path $PSScriptRoot '..\assets\pdfboard.ico')
)

Add-Type -AssemblyName System.Drawing

$size = 256
$bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.Clear([System.Drawing.Color]::Transparent)

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# Blue rounded tile
$tile = New-RoundedPath 8 8 240 240 52
$g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 0x1A, 0x73, 0xE8))), $tile)

# White page with a folded corner
$page = New-Object System.Drawing.Drawing2D.GraphicsPath
$page.AddPolygon(@(
    (New-Object System.Drawing.PointF(74, 56)),
    (New-Object System.Drawing.PointF(150, 56)),
    (New-Object System.Drawing.PointF(184, 90)),
    (New-Object System.Drawing.PointF(184, 200)),
    (New-Object System.Drawing.PointF(74, 200))
))
$g.FillPath([System.Drawing.Brushes]::White, $page)
$fold = New-Object System.Drawing.Drawing2D.GraphicsPath
$fold.AddPolygon(@(
    (New-Object System.Drawing.PointF(150, 56)),
    (New-Object System.Drawing.PointF(184, 90)),
    (New-Object System.Drawing.PointF(150, 90))
))
$g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 0xC9, 0xDD, 0xF8))), $fold)

# Red ink stroke across the page
$pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 0xD3, 0x2F, 0x2F), 14)
$pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawLine($pen, 92, 148, 172, 168)

$g.Dispose()
$pen.Dispose()

# PNG payload
$ms = New-Object System.IO.MemoryStream
$bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
$png = $ms.ToArray()
$ms.Dispose()
$bmp.Dispose()

# ICO container: 1 entry, 256x256 (0 means 256), 32bpp, PNG payload
$dir = New-Object System.IO.DirectoryInfo($OutFile)
if (-not $dir.Exists) { New-Item -ItemType Directory -Force -Path $dir.FullName | Out-Null }

$fs = [System.IO.File]::Create($OutFile)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([UInt16]0)          # reserved
$bw.Write([UInt16]1)          # type: icon
$bw.Write([UInt16]1)          # image count
$bw.Write([Byte]0)            # width  (0 = 256)
$bw.Write([Byte]0)            # height (0 = 256)
$bw.Write([Byte]0)            # palette
$bw.Write([Byte]0)            # reserved
$bw.Write([UInt16]1)          # colour planes
$bw.Write([UInt16]32)         # bits per pixel
$bw.Write([UInt32]$png.Length)
$bw.Write([UInt32]22)         # offset: 6 + 16
$bw.Write($png)
$bw.Close()
$fs.Close()

Write-Output ("wrote {0} ({1} bytes)" -f (Resolve-Path $OutFile).Path, (Get-Item $OutFile).Length)
