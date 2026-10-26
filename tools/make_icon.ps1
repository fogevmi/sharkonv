# Сборка src\app.ico из готовых картинок docs\icon\<размер>.png (логотип Sharkonv — акула,
# уменьшенная из docs\logo.webp). 16…128 пишутся 32-битным BMP — его понимает Windows XP;
# 256 — PNG внутри ICO (Vista и новее показывают его в проводнике, XP эту запись пропускает).
Add-Type -AssemblyName System.Drawing

$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out = "$root\src\app.ico"
$sizes = @(16, 24, 32, 48, 64, 128, 256)

$images = @()
foreach ($s in $sizes) {
    $png = "$root\docs\icon\$s.png"
    if ($s -ge 256) {
        $images += ,@($s, [IO.File]::ReadAllBytes($png))
        continue
    }
    $src = [System.Drawing.Image]::FromFile($png)
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.DrawImage($src, 0, 0, $s, $s)
    $g.Dispose(); $src.Dispose()

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
    # маска AND: 1 — прозрачный пиксель (для XP без альфа-канала, например в 256 цветах)
    $andStride = [int]([Math]::Ceiling($s / 32.0) * 4)
    $andMask = New-Object byte[] ($andStride * $s)
    for ($y = 0; $y -lt $s; $y++) {
        for ($x = 0; $x -lt $s; $x++) {
            if ($bytes[($s - 1 - $y) * $stride + $x * 4 + 3] -lt 128) {
                $andMask[$y * $andStride + [int][Math]::Floor($x / 8)] = $andMask[$y * $andStride + [int][Math]::Floor($x / 8)] -bor (0x80 -shr ($x % 8))
            }
        }
    }

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
