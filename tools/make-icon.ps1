# Draws res\app.ico: three usage bars on a dark rounded square (16-256 px).
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Resolve-Path "$PSScriptRoot\.."
$sizes = 16, 24, 32, 48, 64, 256
$pngs = foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    function Round($x, $y, $w, $h, $r) {
        $p = New-Object System.Drawing.Drawing2D.GraphicsPath
        $d = [Math]::Max(1, $r * 2)
        $p.AddArc($x, $y, $d, $d, 180, 90); $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
        $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90); $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
        $p.CloseFigure(); $p
    }
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 32, 32, 32))), (Round 0 0 ($s - 1) ($s - 1) ($s * 0.22)))
    $m = $s * 0.2; $w = $s - 2 * $m; $bh = [Math]::Max(1.5, $s * 0.11); $gap = ($s - 2 * $m - 3 * $bh) / 2
    $bars = @(@(0.45, [System.Drawing.Color]::FromArgb(255, 0x60, 0xCD, 0xFF)),
              @(0.70, [System.Drawing.Color]::FromArgb(255, 0xFC, 0xE1, 0x00)),
              @(0.90, [System.Drawing.Color]::FromArgb(255, 0xFF, 0x99, 0xA4)))
    for ($i = 0; $i -lt 3; $i++) {
        $y = $m + $i * ($bh + $gap)
        $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(60, 255, 255, 255))), (Round $m $y $w $bh ($bh / 2)))
        $g.FillPath((New-Object System.Drawing.SolidBrush $bars[$i][1]), (Round $m $y ($w * $bars[$i][0]) $bh ($bh / 2)))
    }
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    , $ms.ToArray()
}
$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $out
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $b = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$b); $bw.Write([byte]$b); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32); $bw.Write([UInt32]$pngs[$i].Length); $bw.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
[System.IO.File]::WriteAllBytes("$root\res\app.ico", $out.ToArray())
"Wrote res\app.ico"
