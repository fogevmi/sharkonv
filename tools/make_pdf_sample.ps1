# Writes tests\corpus\handmade.pdf byte by byte, without Word: a classic xref table,
# the standard Helvetica font (no ToUnicode, no widths), headings only by font size,
# four images of kinds Word never produces and a table drawn with stroked lines
# that has a vertically merged cell. tools\test.ps1 checks the pixels of every image
# against the patterns below.
$ErrorActionPreference = 'Stop'
$out = (Resolve-Path "$PSScriptRoot\..\tests\corpus").Path
$enc = [Text.Encoding]::GetEncoding(28591)   # bytes 1:1

$W = 40; $H = 30
function GrayPix { param($x, $y) ($x * 6) % 256 }
function IdxPix { param($x, $y) ($x + $y) % 16 }
function BitPix { param($x, $y) ([int][Math]::Floor($x / 5) + [int][Math]::Floor($y / 5)) % 2 }

# 8-bit gray, written as ASCIIHex
$gray = New-Object Text.StringBuilder
for ($y = 0; $y -lt $H; $y++) { for ($x = 0; $x -lt $W; $x++) { [void]$gray.Append((GrayPix $x $y).ToString('X2')) } }
$grayData = $gray.ToString() + '>'
# 4-bit indexed, 16-colour palette
$idx = New-Object byte[] ($H * $W / 2)
for ($y = 0; $y -lt $H; $y++) { for ($x = 0; $x -lt $W; $x += 2) { $idx[$y * $W / 2 + $x / 2] = [byte](((IdxPix $x $y) -shl 4) -bor (IdxPix ($x + 1) $y)) } }
$pal = New-Object Text.StringBuilder
for ($i = 0; $i -lt 16; $i++) { [void]$pal.Append(('{0:X2}{1:X2}{2:X2}' -f ($i * 16), (255 - $i * 16), (($i * 7 * 16) % 256))) }
# 1-bit with Decode [1 0]: sample 1 is black
$rowBytes = [int][Math]::Ceiling($W / 8)
$bits = New-Object byte[] ($rowBytes * $H)
for ($y = 0; $y -lt $H; $y++) { for ($x = 0; $x -lt $W; $x++) { if ((BitPix $x $y) -eq 1) { $bits[$y * $rowBytes + [Math]::Floor($x / 8)] = $bits[$y * $rowBytes + [Math]::Floor($x / 8)] -bor (0x80 -shr ($x % 8)) } } }
# 8-bit CMYK: cyan grows to the right, magenta downwards
$cmyk = New-Object byte[] ($W * $H * 4)
for ($y = 0; $y -lt $H; $y++) { for ($x = 0; $x -lt $W; $x++) { $o = ($y * $W + $x) * 4; $cmyk[$o] = [byte]($x * 6); $cmyk[$o + 1] = [byte]($y * 8) } }

$content = @"
BT /F1 18 Tf 72 760 Td (Chapter One) Tj ET
BT /F1 11 Tf 72 730 Td (This page was written by hand, without any office program.) Tj ET
q 80 0 0 60 72 640 cm /ImG Do Q
q 80 0 0 60 172 640 cm /ImI Do Q
q 80 0 0 60 272 640 cm /ImB Do Q
q 80 0 0 60 372 640 cm /ImC Do Q
BT /F1 11 Tf 72 610 Td (A table with stroked borders follows.) Tj ET
0.5 w
72 590 m 372 590 l S
72 570 m 372 570 l S
172 550 m 372 550 l S
72 530 m 372 530 l S
72 590 m 72 530 l S
172 590 m 172 530 l S
272 590 m 272 530 l S
372 590 m 372 530 l S
BT /F1 11 Tf 80 576 Td (Head1) Tj ET
BT /F1 11 Tf 180 576 Td (Head2) Tj ET
BT /F1 11 Tf 280 576 Td (Head3) Tj ET
BT /F1 11 Tf 80 546 Td (Span) Tj ET
BT /F1 11 Tf 180 556 Td (b1) Tj ET
BT /F1 11 Tf 280 556 Td (c1) Tj ET
BT /F1 11 Tf 180 536 Td (b2) Tj ET
BT /F1 11 Tf 280 536 Td (c2) Tj ET
BT /F1 11 Tf 72 500 Td (Text after the table.) Tj ET
BT /F1 18 Tf 72 460 Td (Chapter Two) Tj ET
BT /F1 11 Tf 72 430 Td (The end.) Tj ET
"@

$objs = New-Object 'System.Collections.Generic.List[string]'
$objs.Add('<< /Type /Catalog /Pages 2 0 R >>')
$objs.Add('<< /Type /Pages /Kids [3 0 R] /Count 1 >>')
$objs.Add('<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> /XObject << /ImG 6 0 R /ImI 7 0 R /ImB 8 0 R /ImC 9 0 R >> >> >>')
$objs.Add("<< /Length $($content.Length) >>`nstream`n$content`nendstream")
$objs.Add('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>')
$objs.Add("<< /Type /XObject /Subtype /Image /Width $W /Height $H /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /ASCIIHexDecode /Length $($grayData.Length) >>`nstream`n$grayData`nendstream")
$idxStr = $enc.GetString($idx)
$objs.Add("<< /Type /XObject /Subtype /Image /Width $W /Height $H /ColorSpace [/Indexed /DeviceRGB 15 <$($pal.ToString())>] /BitsPerComponent 4 /Length $($idx.Length) >>`nstream`n$idxStr`nendstream")
$bitStr = $enc.GetString($bits)
$objs.Add("<< /Type /XObject /Subtype /Image /Width $W /Height $H /ColorSpace /DeviceGray /BitsPerComponent 1 /Decode [1 0] /Length $($bits.Length) >>`nstream`n$bitStr`nendstream")
$cmykStr = $enc.GetString($cmyk)
$objs.Add("<< /Type /XObject /Subtype /Image /Width $W /Height $H /ColorSpace /DeviceCMYK /BitsPerComponent 8 /Length $($cmyk.Length) >>`nstream`n$cmykStr`nendstream")
$objs.Add('<< /Title (Handmade sample) /Author (Test Writer) >>')

$pdf = New-Object Text.StringBuilder
[void]$pdf.Append("%PDF-1.4`n%" + [char]0xE2 + [char]0xE3 + [char]0xCF + [char]0xD3 + "`n")
$offsets = @()
for ($i = 0; $i -lt $objs.Count; $i++) {
    $offsets += $pdf.Length
    [void]$pdf.Append("$($i + 1) 0 obj`n$($objs[$i])`nendobj`n")
}
$xref = $pdf.Length
[void]$pdf.Append("xref`n0 $($objs.Count + 1)`n0000000000 65535 f `n")
foreach ($o in $offsets) { [void]$pdf.Append(('{0:D10} 00000 n ' -f $o) + "`n") }
[void]$pdf.Append("trailer`n<< /Size $($objs.Count + 1) /Root 1 0 R /Info $($objs.Count) 0 R >>`nstartxref`n$xref`n%%EOF`n")
[IO.File]::WriteAllBytes("$out\handmade.pdf", $enc.GetBytes($pdf.ToString()))

$utf8 = New-Object Text.UTF8Encoding $false
# ^N in a table cell = rowspan
[IO.File]::WriteAllText("$out\handmade.pdf.expect", "Chapter One`nChapter Two`ntitle=Handmade sample`nimages=4`ntable=Head1;Head2;Head3|Span^2;b1;c1|b2;c2`n", $utf8)
"written $out\handmade.pdf ($($pdf.Length) bytes)"

# --- handmade-cols.pdf: two columns the way LaTeX sets them, not Word: ragged right,
# a narrow 12 pt gutter, 10 pt text, the right column starting 7 pt lower than the left
# one, paragraphs flowing from the left column into the right one.
function PdfFile([string]$path, [string]$content, [string]$title) {
    $o = New-Object 'System.Collections.Generic.List[string]'
    $o.Add('<< /Type /Catalog /Pages 2 0 R >>')
    $o.Add('<< /Type /Pages /Kids [3 0 R] /Count 1 >>')
    $o.Add('<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>')
    $o.Add("<< /Length $($content.Length) >>`nstream`n$content`nendstream")
    $o.Add('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>')
    $o.Add("<< /Title ($title) >>")
    $sb = New-Object Text.StringBuilder
    [void]$sb.Append("%PDF-1.4`n")
    $offs = @()
    for ($i = 0; $i -lt $o.Count; $i++) { $offs += $sb.Length; [void]$sb.Append("$($i + 1) 0 obj`n$($o[$i])`nendobj`n") }
    $xr = $sb.Length
    [void]$sb.Append("xref`n0 $($o.Count + 1)`n0000000000 65535 f `n")
    foreach ($q in $offs) { [void]$sb.Append(('{0:D10} 00000 n ' -f $q) + "`n") }
    [void]$sb.Append("trailer`n<< /Size $($o.Count + 1) /Root 1 0 R /Info $($o.Count) 0 R >>`nstartxref`n$xr`n%%EOF`n")
    [IO.File]::WriteAllBytes($path, $enc.GetBytes($sb.ToString()))
}
$words = 'river', 'garden', 'letter', 'window', 'evening', 'journey', 'library', 'mountain', 'station',
         'conversation', 'remarkable', 'circumstance', 'memory', 'traveller', 'electricity', 'the', 'and', 'of'
$rnd = New-Object Random 7
$paras = @()
foreach ($k in 1..6) {
    $n = $rnd.Next(30, 55)
    $s = (1..$n | ForEach-Object { $words[$rnd.Next($words.Count)] }) -join ' '
    $paras += $s.Substring(0, 1).ToUpper() + $s.Substring(1) + '.'
}
# ragged right: Helvetica is about 0.5 em per letter, 10 pt text in a 230 pt column
$sbc = New-Object Text.StringBuilder
[void]$sbc.Append("BT /F1 16 Tf 50 740 Td (Two Column Article) Tj ET`n")
$colX = @(50, 302); $colTop = @(700, 693); $col = 0; $y = $colTop[0]
foreach ($p in $paras) {
    $line = ''
    $ws = $p -split ' '
    for ($i = 0; $i -le $ws.Count; $i++) {
        $next = if ($i -lt $ws.Count) { if ($line) { "$line $($ws[$i])" } else { $ws[$i] } } else { $null }
        if ($i -eq $ws.Count -or $next.Length * 5.1 -gt 230) {
            [void]$sbc.Append("BT /F1 10 Tf $($colX[$col]) $y Td ($line) Tj ET`n")
            $y -= 12
            if ($y -lt 420 -and $col -eq 0) { $col = 1; $y = $colTop[1] }
            $line = if ($i -lt $ws.Count) { $ws[$i] } else { '' }
        } else { $line = $next }
    }
    $y -= 8                                      # space between paragraphs
    if ($y -lt 420 -and $col -eq 0) { $col = 1; $y = $colTop[1] }
}
PdfFile "$out\handmade-cols.pdf" $sbc.ToString() 'Two Column Article'
[IO.File]::WriteAllText("$out\handmade-cols.paras", (($paras -join "`n") + "`n"), $utf8)
[IO.File]::WriteAllText("$out\handmade-cols.pdf.expect", "title=Two Column Article`nparas=handmade-cols.paras`n", $utf8)
"written $out\handmade-cols.pdf"
