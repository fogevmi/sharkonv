# Builds tests\corpus\handmade.mobi byte by byte, straight from the format description
# (MobileRead wiki, Calibre's reader), without Sharkonv's own code. It covers what files from
# kindlegen and Calibre have and Sharkonv's writer does not produce:
#   * PalmDOC compression with all four kinds of codes: literal, 1-8 raw bytes,
#     back reference, and "space + character" (0xC0-0xFF);
#   * trailing entries after every text record (extra data flags 0b11: multibyte byte and
#     one more entry sized in backward base-128);
#   * two text records, EXTH with author and title, big-endian everywhere.
# The expected paragraphs are written to handmade-mobi.paras.
$root = (Resolve-Path "$PSScriptRoot\..").Path
$out = "$root\tests\corpus\handmade.mobi"
$utf8 = New-Object Text.UTF8Encoding $false

$paras = @(
    'Hello Hello Hello world: the shark eats every book.',
    'Привет, мир! Акула съедает любую книгу.',
    'Second record starts here, with a repeat repeat repeat.'
)
$html1 = "<html><head><title>Handmade MOBI</title></head><body><h1>Chapter one</h1><p>$($paras[0])</p><p>$($paras[1])</p>"
$html2 = "<mbp:pagebreak/><h1>Chapter two</h1><p>$($paras[2])</p></body></html>"

function Compress([byte[]]$src) {
    $o = New-Object System.Collections.Generic.List[byte]
    $i = 0
    while ($i -lt $src.Length) {
        # back reference: the longest earlier match of 3..10 bytes within 2047 bytes
        $bestLen = 0; $bestDist = 0
        for ($d = 1; $d -le [Math]::Min(2047, $i); $d++) {
            $l = 0
            while ($l -lt 10 -and $i + $l -lt $src.Length -and $src[$i + $l] -eq $src[$i - $d + $l]) { $l++ }
            if ($l -gt $bestLen) { $bestLen = $l; $bestDist = $d }
        }
        if ($bestLen -ge 3) {
            $v = 0x8000 -bor ($bestDist -shl 3) -bor ($bestLen - 3)
            $o.Add([byte](($v -shr 8) -band 0xFF)); $o.Add([byte]($v -band 0xFF))
            $i += $bestLen
            continue
        }
        $c = $src[$i]
        if ($c -eq 0x20 -and $i + 1 -lt $src.Length -and $src[$i + 1] -ge 0x40 -and $src[$i + 1] -le 0x7F) {
            $o.Add([byte]($src[$i + 1] -bxor 0x80))      # space + character
            $i += 2
        } elseif ($c -eq 0 -or ($c -ge 0x09 -and $c -le 0x7F)) {
            $o.Add($c); $i++                              # literal
        } else {
            $n = 0                                        # 1-8 raw bytes (UTF-8 here)
            while ($n -lt 8 -and $i + $n -lt $src.Length -and -not ($src[$i + $n] -ge 0x09 -and $src[$i + $n] -le 0x7F)) { $n++ }
            $o.Add([byte]$n)
            for ($k = 0; $k -lt $n; $k++) { $o.Add($src[$i + $k]) }
            $i += $n
        }
    }
    return $o.ToArray()
}

# in PowerShell the comma binds tighter than -band, and 0xFFFFFFFF is -1: hence the brackets and [long]
function BE16([long]$v) { [byte[]]@((($v -shr 8) -band 0xFF), ($v -band 0xFF)) }
function BE32([long]$v) { $v = $v -band 0xFFFFFFFFL; [byte[]]@((($v -shr 24) -band 0xFF), (($v -shr 16) -band 0xFF), (($v -shr 8) -band 0xFF), ($v -band 0xFF)) }
function Put([byte[]]$buf, [int]$at, [byte[]]$v) { [Array]::Copy($v, 0, $buf, $at, $v.Length) }

$raw1 = $utf8.GetBytes($html1); $raw2 = $utf8.GetBytes($html2)
# trailing data: multibyte byte (0 overlapping bytes -> 0x00), then an entry of 3 bytes whose
# size is the last byte, backward base-128 with the high bit set: 0x83
$trail = [byte[]](0x00, 0x55, 0x66, 0x83)
$rec1 = [byte[]]((Compress $raw1) + $trail)
$rec2 = [byte[]]((Compress $raw2) + $trail)
$textLen = $raw1.Length + $raw2.Length

$title = $utf8.GetBytes('Handmade MOBI')
$exthRecs = @(@(100, $utf8.GetBytes('Акулов, Сергей')), @(503, $title), @(524, $utf8.GetBytes('ru')))
$exthLen = 12; foreach ($r in $exthRecs) { $exthLen += 8 + $r[1].Length }
$r0 = New-Object byte[] (16 + 232 + $exthLen + $title.Length + 2)
Put $r0 0 (BE16 2)                       # PalmDOC compression
Put $r0 4 (BE32 $textLen)
Put $r0 8 (BE16 2)                       # two text records
Put $r0 10 (BE16 4096)
$M = 16
Put $r0 $M ([Text.Encoding]::ASCII.GetBytes('MOBI'))
Put $r0 ($M + 0x04) (BE32 232)
Put $r0 ($M + 0x08) (BE32 2)
Put $r0 ($M + 0x0C) (BE32 65001)
Put $r0 ($M + 0x10) (BE32 0x12345678)
Put $r0 ($M + 0x14) (BE32 6)
for ($k = 0x18; $k -lt 0x40; $k += 4) { Put $r0 ($M + $k) (BE32 0xFFFFFFFF) }
Put $r0 ($M + 0x40) (BE32 3)             # first non-book record
Put $r0 ($M + 0x44) (BE32 (16 + 232 + $exthLen))
Put $r0 ($M + 0x48) (BE32 $title.Length)
Put $r0 ($M + 0x4C) (BE32 0x19)
Put $r0 ($M + 0x58) (BE32 6)
Put $r0 ($M + 0x5C) (BE32 0xFFFFFFFF)    # no images
Put $r0 ($M + 0x70) (BE32 0x40)          # EXTH present
Put $r0 ($M + 0x94) (BE32 0xFFFFFFFF)
Put $r0 ($M + 0x98) (BE32 0xFFFFFFFF)
Put $r0 ($M + 0xB0) (BE16 1); Put $r0 ($M + 0xB2) (BE16 2)
Put $r0 ($M + 0xE0) (BE32 3)             # extra data flags: multibyte + one more entry
Put $r0 ($M + 0xE4) (BE32 0xFFFFFFFF)
$p = $M + 232
Put $r0 $p ([Text.Encoding]::ASCII.GetBytes('EXTH')); Put $r0 ($p + 4) (BE32 $exthLen); Put $r0 ($p + 8) (BE32 $exthRecs.Count)
$q = $p + 12
foreach ($r in $exthRecs) { Put $r0 $q (BE32 $r[0]); Put $r0 ($q + 4) (BE32 (8 + $r[1].Length)); Put $r0 ($q + 8) $r[1]; $q += 8 + $r[1].Length }
Put $r0 ($p + $exthLen) $title

$records = @($r0, $rec1, $rec2, [byte[]](0xE9, 0x8E, 0x0D, 0x0A))
$hdr = New-Object byte[] (78 + 8 * $records.Count + 2)
Put $hdr 0 ([Text.Encoding]::ASCII.GetBytes('Handmade_MOBI'))
Put $hdr 60 ([Text.Encoding]::ASCII.GetBytes('BOOKMOBI'))
Put $hdr 76 (BE16 $records.Count)
$off = $hdr.Length
for ($k = 0; $k -lt $records.Count; $k++) {
    Put $hdr (78 + 8 * $k) (BE32 $off)
    Put $hdr (82 + 8 * $k) (BE32 (2 * $k))
    $off += $records[$k].Length
}
$fs = [IO.File]::Create($out)
$fs.Write($hdr, 0, $hdr.Length)
foreach ($r in $records) { $fs.Write($r, 0, $r.Length) }
$fs.Close()
[IO.File]::WriteAllLines("$root\tests\corpus\handmade-mobi.paras", $paras, $utf8)
"OK: $out ($((Get-Item $out).Length) bytes, text $textLen bytes -> $($rec1.Length + $rec2.Length) compressed with trailers)"
