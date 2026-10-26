# Генерация app.ico (BMP-формат, совместим с Windows XP).
# Рисуем «книгу» на синем фоне.
Add-Type -AssemblyName System.Drawing

$out = Join-Path $PSScriptRoot '..\src\app.ico'
$out = [IO.Path]::GetFullPath($out)
$sizes = @(16, 24, 32, 48, 64, 128)

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

$images = @()
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.Clear([System.Drawing.Color]::Transparent)

    # фон — скруглённый квадрат с градиентом
    $bgPath = New-RoundedPath 0 0 $s $s ($s * 0.20)
    $rect = New-Object System.Drawing.RectangleF(0, 0, $s, $s)
    $c1 = [System.Drawing.Color]::FromArgb(255, 62, 124, 232)
    $c2 = [System.Drawing.Color]::FromArgb(255, 22, 62, 158)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 45.0)
    $g.FillPath($brush, $bgPath)
    $brush.Dispose()

    # книга
    $bx = $s * 0.20; $by = $s * 0.18; $bw = $s * 0.60; $bh = $s * 0.64
    $pagePath = New-RoundedPath $bx $by $bw $bh ($s * 0.05)
    $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
    $g.FillPath($white, $pagePath)

    # корешок
    $spine = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 200, 216, 245))
    $g.FillRectangle($spine, [single]($bx + $s * 0.02), [single]($by + $s * 0.03),
                     [single]($s * 0.09), [single]($bh - $s * 0.06))

    if ($s -ge 32) {
        # надпись FB2
        $fs = [single]($s * 0.20)
        $font = New-Object System.Drawing.Font('Arial', $fs, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
        $blue = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 26, 74, 170))
        $sf = New-Object System.Drawing.StringFormat
        $sf.Alignment = [System.Drawing.StringAlignment]::Center
        $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
        $tr = New-Object System.Drawing.RectangleF(($bx + $s * 0.10), $by, ($bw - $s * 0.12), $bh)
        $g.DrawString('FB2', $font, $blue, $tr, $sf)
        $sf.Dispose(); $font.Dispose(); $blue.Dispose()
    } else {
        # мелкий размер — три строки вместо текста
        $line = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 40, 90, 190))
        for ($i = 0; $i -lt 3; $i++) {
            $ly = $by + $s * (0.26 + $i * 0.17)
            $g.FillRectangle($line, [single]($bx + $s * 0.17), [single]$ly,
                             [single]($s * (0.34 - $i * 0.05)), [single][Math]::Max(1, $s * 0.06))
        }
        $line.Dispose()
    }

    # лёгкая тень-обводка книги
    $pen = New-Object System.Drawing.Pen -ArgumentList @(
        [System.Drawing.Color]::FromArgb(60, 10, 30, 80),
        [single][Math]::Max(1, $s * 0.02))
    $g.DrawPath($pen, $pagePath)
    $pen.Dispose()

    $white.Dispose(); $spine.Dispose(); $pagePath.Dispose(); $bgPath.Dispose()
    $g.Dispose()

    # пиксели BGRA снизу вверх
    $data = $bmp.LockBits((New-Object System.Drawing.Rectangle(0, 0, $s, $s)),
                          [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $stride = $data.Stride
    $bytes = New-Object byte[] ($stride * $s)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)

    $xor = New-Object System.Collections.Generic.List[byte]
    for ($y = $s - 1; $y -ge 0; $y--) {
        $off = $y * $stride
        for ($k = 0; $k -lt $s * 4; $k++) { $xor.Add($bytes[$off + $k]) }
    }
    $andStride = [int]([Math]::Ceiling($s / 32.0) * 4)
    $andMask = New-Object byte[] ($andStride * $s)

    $ms = New-Object System.IO.MemoryStream
    $bw2 = New-Object System.IO.BinaryWriter($ms)
    $bw2.Write([uint32]40)
    $bw2.Write([int32]$s)
    $bw2.Write([int32]($s * 2))
    $bw2.Write([uint16]1)
    $bw2.Write([uint16]32)
    $bw2.Write([uint32]0)
    $bw2.Write([uint32]($s * $s * 4))
    $bw2.Write([uint32]0); $bw2.Write([uint32]0); $bw2.Write([uint32]0); $bw2.Write([uint32]0)
    $bw2.Write($xor.ToArray())
    $bw2.Write($andMask)
    $bw2.Flush()
    $images += ,@($s, $ms.ToArray())
    $bw2.Dispose(); $ms.Dispose(); $bmp.Dispose()
}

$fs = [IO.File]::Create($out)
$w = New-Object System.IO.BinaryWriter($fs)
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($img in $images) {
    $s = [int]$img[0]; $len = [int]$img[1].Length
    $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $w.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))
    $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$len); $w.Write([uint32]$offset)
    $offset += $len
}
foreach ($img in $images) { $w.Write([byte[]]$img[1]) }
$w.Dispose(); $fs.Dispose()
"OK: $out ($((Get-Item $out).Length) bytes)"
