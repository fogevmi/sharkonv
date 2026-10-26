# Regression test: every input format -> every output format, then read the
# produced EPUB/MOBI/FB2/HTML/TXT back. Also checks that the exe imports nothing
# missing from Windows XP SP3. Usage: powershell -File tools\test.ps1
param([string[]]$TestData = @("$PSScriptRoot\..\tests", "$PSScriptRoot\..\..\fb2-converter\testdata"))

$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\.."
# one exe for everything: with switches it runs in console mode
$cli = "$root\build\fb2conv.exe"
$work = Join-Path $env:TEMP "fb2conv-test"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory $work | Out-Null

$formats = 'fb2', 'epub', 'html', 'txt', 'mobi'
$fail = 0; $pass = 0

function Run([string]$in, [string]$fmt, [string]$outDir) {
    New-Item -ItemType Directory -Force $outDir | Out-Null
    & $cli -f $fmt -o $outDir $in | Out-Null
    $base = [IO.Path]::GetFileNameWithoutExtension($in)
    $out = Join-Path $outDir "$base.$fmt"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $out) -or (Get-Item $out).Length -lt 100) {
        Write-Host "FAIL  $([IO.Path]::GetFileName($in)) -> $fmt" -ForegroundColor Red
        $script:fail++
        return $null
    }
    $script:pass++
    return $out
}

# 1) source inputs (fb2, cp1251 fb2, txt, html, docx) -> all outputs
$inputs = Get-ChildItem ($TestData | Where-Object { Test-Path $_ }) -File | Where-Object { $_.Extension -match '\.(fb2|txt|html|docx)$' }
$produced = @()
foreach ($f in $inputs) {
    foreach ($fmt in $formats) {
        $o = Run $f.FullName $fmt (Join-Path $work "1-$($f.BaseName)")
        if ($o) { $produced += $o }
    }
}

# 2) produced files (covers EPUB and MOBI as inputs) -> all outputs
foreach ($p in $produced | Where-Object { $_ -match '\.(epub|mobi)$' }) {
    foreach ($fmt in $formats) {
        Run $p $fmt (Join-Path $work ("2-" + [IO.Path]::GetFileName($p).Replace('.', '_'))) | Out-Null
    }
}

# 2b) every produced FB2 and every XML part of every EPUB must be well-formed XML
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$xmlBad = 0
foreach ($p in Get-ChildItem $work -Recurse -File -Include *.fb2, *.epub) {
    $parts = @()
    if ($p.Extension -eq '.fb2') { $parts += , @($p.Name, [IO.File]::ReadAllText($p.FullName)) }
    else {
        $z = [IO.Compression.ZipFile]::OpenRead($p.FullName)
        foreach ($e in $z.Entries | Where-Object { $_.Name -match '\.(xhtml|opf|ncx|xml)$' }) {
            $r = New-Object IO.StreamReader($e.Open(), [Text.Encoding]::UTF8)
            $parts += , @("$($p.Name)!$($e.FullName)", $r.ReadToEnd())
            $r.Close()
        }
        $z.Dispose()
    }
    foreach ($part in $parts) {
        $x = New-Object Xml.XmlDocument
        $x.XmlResolver = $null
        try { $x.LoadXml(($part[1] -replace '<!DOCTYPE html>', '')) }
        catch { $xmlBad++; Write-Host "FAIL  not well-formed: $($part[0]): $($_.Exception.InnerException.Message)" -ForegroundColor Red }
    }
}
if ($xmlBad) { $fail += $xmlBad } else { $pass++; Write-Host "OK    all FB2/EPUB XML parts are well-formed" }

# 3) structure survives round trips: the FB2 section tree (titles with nesting
#    depth), series and note titles must match the direct FB2 output.
function Shape([string]$fb2, [bool]$series = $true) {
    $x = New-Object Xml.XmlDocument
    $x.Load($fb2)
    $ns = New-Object Xml.XmlNamespaceManager $x.NameTable
    $ns.AddNamespace('f', 'http://www.gribuser.ru/xml/fictionbook/2.0')
    $tree = foreach ($s in $x.SelectNodes('//f:body[not(@name)]//f:section[f:title]', $ns)) {
        $depth = $s.SelectNodes('ancestor::f:section', $ns).Count
        ('  ' * $depth) + ($s.SelectSingleNode('f:title', $ns).InnerText.Trim())
    }
    $seq = $x.SelectSingleNode('//f:title-info/f:sequence', $ns)
    $ids = @{}
    $notes = foreach ($n in $x.SelectNodes('//f:body[@name="notes"]/f:section', $ns)) {
        $ids[$n.GetAttribute('id')] = 1
        $n.SelectSingleNode('f:title', $ns).InnerText
    }
    # note links that are marked type="note" and point to an existing note
    $links = @($x.SelectNodes('//f:body[not(@name)]//f:a[@type="note"]', $ns) |
               Where-Object { $ids.ContainsKey($_.GetAttribute('href', 'http://www.w3.org/1999/xlink').TrimStart('#')) }).Count
    $s = if ($series -and $seq) { "$($seq.GetAttribute('name'))#$($seq.GetAttribute('number'))" } else { '-' }
    @($tree) + "series=$s" + "notes=$($notes -join ',') links=$links" -join "`n"
}
foreach ($f in $inputs | Where-Object { $_.Extension -eq '.fb2' }) {
    $direct = Join-Path $work "1-$($f.BaseName)\$($f.BaseName).fb2"
    if (-not (Test-Path $direct)) { continue }
    foreach ($via in 'epub', 'mobi') {
        # MOBI has no series field, so series is only compared for EPUB
        $want = Shape $direct ($via -eq 'epub')
        $rt = Join-Path $work "2-$($f.BaseName)_$via\$($f.BaseName).fb2"
        $got = Shape $rt ($via -eq 'epub')
        if ($got -eq $want) { $pass++; Write-Host "OK    structure $($f.Name) -> $via -> fb2" }
        else {
            $fail++
            Write-Host "FAIL  structure $($f.Name) -> $via -> fb2" -ForegroundColor Red
            Write-Host "  want:`n$want`n  got:`n$got"
        }
    }
}

# 4) XP compatibility: imported functions must exist in XP SP3 system DLLs
$objdump = 'C:\dev\mingw32\bin\objdump.exe'
$notOnXp = 'GetTickCount64|InitializeCriticalSectionEx|GetLocaleInfoEx|CompareStringEx|' +
           'LCMapStringEx|InitOnceExecuteOnce|CreateSymbolicLink|GetFinalPathNameByHandle|' +
           'SetFileInformationByHandle|GetFileInformationByHandleEx|CreateEventEx|' +
           'InitializeSRWLock|AcquireSRWLock|SleepConditionVariable|WakeConditionVariable|' +
           'FlsAlloc|SetDllDirectory|GetDpiForWindow|SetProcessDpiAware|TaskDialog'
# 3b) books made by other programs (tests\corpus, see tools\make_corpus.ps1) -> FB2.
#     Each file has <name>.expect: the section tree that must come out, then checks:
#     title=, series=name#num, notes=N (notes and type="note" links), has=strong,emphasis,…
function Fb2Facts([string]$fb2) {
    $x = New-Object Xml.XmlDocument
    $x.Load($fb2)
    $ns = New-Object Xml.XmlNamespaceManager $x.NameTable
    $ns.AddNamespace('f', 'http://www.gribuser.ru/xml/fictionbook/2.0')
    $tree = foreach ($s in $x.SelectNodes('//f:body[not(@name)]//f:section[f:title]', $ns)) {
        ('  ' * $s.SelectNodes('ancestor::f:section', $ns).Count) + ($s.SelectSingleNode('f:title', $ns).InnerText.Trim() -replace '\s+', ' ')
    }
    $seq = $x.SelectSingleNode('//f:title-info/f:sequence', $ns)
    $ids = @{}
    $bodyText = ($x.SelectNodes('//f:body[not(@name)]', $ns) | ForEach-Object { $_.InnerText }) -join ' '
    $dupes = @()
    foreach ($n in $x.SelectNodes('//f:body[@name="notes"]/f:section', $ns)) {
        $ids[$n.GetAttribute('id')] = 1
        # the text of a note must not stay in the main text as well
        foreach ($p in $n.SelectNodes('f:p', $ns)) {
            $t = $p.InnerText.Trim()
            if ($t.Length -ge 8 -and $bodyText.Contains($t)) { $dupes += $t }
        }
    }
    $links = @($x.SelectNodes('//f:body[not(@name)]//f:a[@type="note"]', $ns) |
               Where-Object { $ids.ContainsKey($_.GetAttribute('href', 'http://www.w3.org/1999/xlink').TrimStart('#')) }).Count
    $has = @{}
    foreach ($tag in 'strong', 'emphasis', 'table', 'poem', 'cite', 'epigraph', 'image') {
        $has[$tag] = $x.SelectNodes("//f:body[not(@name)]//f:$tag", $ns).Count -gt 0
    }
    # pictures in the main text that point to an existing <binary>
    $bins = @{}
    foreach ($b in $x.SelectNodes('//f:binary', $ns)) { $bins[$b.GetAttribute('id')] = $b.InnerText.Length }
    $images = @($x.SelectNodes('//f:body[not(@name)]//f:image', $ns) |
                Where-Object { $bins[$_.GetAttribute('href', 'http://www.w3.org/1999/xlink').TrimStart('#')] -gt 100 }).Count
    # first table as text: rows by "|", cells by ";", colspan as "*N"
    $tables = @()
    foreach ($tn in $x.SelectNodes('//f:body[not(@name)]//f:table', $ns)) {
        $tables += (@($tn.SelectNodes('f:tr', $ns) | ForEach-Object {
            (@($_.SelectNodes('f:td|f:th', $ns) | ForEach-Object {
                $c = ($_.InnerText -replace '\s+', ' ').Trim()
                $span = $_.GetAttribute('colspan')
                $rs = $_.GetAttribute('rowspan')
                if ($span -and [int]$span -gt 1) { $c += "*$span" }
                if ($rs -and [int]$rs -gt 1) { $c += "^$rs" }
                $c
            }) -join ';')
        }) -join '|')
    }
    @{
        images = $images
        table  = if ($tables.Count -ge 1) { $tables[0] } else { '' }
        table2 = if ($tables.Count -ge 2) { $tables[1] } else { '' }
        tree   = (@($tree) -join "`n")
        title  = $x.SelectSingleNode('//f:title-info/f:book-title', $ns).InnerText
        series = if ($seq) { "$($seq.GetAttribute('name'))#$($seq.GetAttribute('number'))" } else { '' }
        notes  = $ids.Count
        links  = $links
        dupes  = $dupes
        has    = $has
    }
}

# paragraph texts of the main text (titles excluded), whitespace collapsed
function Fb2Paras([string]$fb2) {
    $x = New-Object Xml.XmlDocument
    $x.Load($fb2)
    $ns = New-Object Xml.XmlNamespaceManager $x.NameTable
    $ns.AddNamespace('f', 'http://www.gribuser.ru/xml/fictionbook/2.0')
    @($x.SelectNodes('//f:body[not(@name)]//f:section/f:p', $ns) | ForEach-Object { ($_.InnerText -replace '\s+', ' ').Trim() })
}

$corpus = "$root\tests\corpus"
# files compared with another one (same=) go after the rest, so the reference is converted first
$expects = Get-ChildItem $corpus -Filter *.expect -ErrorAction SilentlyContinue |
           Sort-Object { if ((Get-Content $_.FullName -Raw) -match '(?m)^same=') { 1 } else { 0 } }, Name
foreach ($exp in $expects) {
    $src = Join-Path $corpus $exp.BaseName
    $name = $exp.BaseName
    $outDir = Join-Path $work ("c-" + $name.Replace('.', '_'))
    New-Item -ItemType Directory -Force $outDir | Out-Null
    $log = & $cli -f fb2 -o $outDir $src | Out-String
    $fb2 = Join-Path $outDir ([IO.Path]::GetFileNameWithoutExtension($src) + '.fb2')
    if (-not (Test-Path $fb2)) {
        $fail++; Write-Host "FAIL  corpus $name -> fb2: $(($log.Trim() -split "`r?`n" | Select-Object -First 1))" -ForegroundColor Red
        continue
    }
    $lines = [IO.File]::ReadAllLines($exp.FullName, [Text.Encoding]::UTF8)
    $wantTree = ($lines | Where-Object { $_ -and $_ -notmatch '^\w+=' }) -join "`n"
    $facts = Fb2Facts $fb2
    $problems = @()
    if ($facts.dupes) { $problems += "note text also left in the main text: $($facts.dupes -join ' | ')" }
    # a table where the book has none = text taken for a table (columns, lists, justified lines)
    if (-not ($lines -match '^table=') -and $facts.table) { $problems += "unexpected table: $($facts.table.Substring(0, [Math]::Min(120, $facts.table.Length)))" }
    if ($facts.tree -ne $wantTree) { $problems += "tree:`n    want:`n      $($wantTree -replace "`n", "`n      ")`n    got:`n      $($facts.tree -replace "`n", "`n      ")" }
    foreach ($l in $lines) {
        # match here, not in Where-Object: $Matches from a pipeline block does not reach the loop
        if ($l -notmatch '^(\w+)=(.*)$') { continue }
        $k = $Matches[1]; $v = $Matches[2]
        switch ($k) {
            'title'  { if ($facts.title -ne $v) { $problems += "title: want '$v' got '$($facts.title)'" } }
            'images' { if ($facts.images -ne [int]$v) { $problems += "images: want $v got $($facts.images)" } }
            'table'  { if ($facts.table -ne $v) { $problems += "table:`n    want: $v`n    got:  $($facts.table)" } }
            'table2' { if ($facts.table2 -ne $v) { $problems += "table2:`n    want: $v`n    got:  $($facts.table2)" } }
            'series' { if ($facts.series -ne $v) { $problems += "series: want '$v' got '$($facts.series)'" } }
            'notes'  { if ($facts.notes -ne [int]$v -or $facts.links -ne [int]$v) { $problems += "notes: want $v got $($facts.notes) notes / $($facts.links) note links" } }
            'has'    { foreach ($t in $v -split ',') { if (-not $facts.has[$t]) { $problems += "missing <$t>" } } }
            'paras'  {
                # paragraphs of the main text, one per line, word for word
                $a = @([IO.File]::ReadAllLines((Join-Path $corpus $v), [Text.Encoding]::UTF8) | Where-Object { $_ })
                $b = Fb2Paras $fb2
                for ($k = 0; $k -lt [Math]::Max($a.Count, $b.Count); $k++) {
                    if ($a[$k] -ne $b[$k]) {
                        $problems += "paragraph $($k + 1) of $($a.Count)/$($b.Count):`n    want: $($a[$k])`n    got:  $($b[$k])"
                        break
                    }
                }
            }
            'same'   {
                $ref = Join-Path $work ("c-" + $v.Replace('.', '_'))
                $refFb2 = Join-Path $ref ([IO.Path]::GetFileNameWithoutExtension($v) + '.fb2')
                if (-not (Test-Path $refFb2)) { $problems += "same: $v was not converted"; break }
                $a = Fb2Paras $refFb2; $b = Fb2Paras $fb2
                for ($k = 0; $k -lt [Math]::Max($a.Count, $b.Count); $k++) {
                    if ($a[$k] -ne $b[$k]) {
                        $problems += "paragraph $($k + 1) of $($a.Count)/$($b.Count) differs from ${v}:`n    want: $($a[$k])`n    got:  $($b[$k])"
                        break
                    }
                }
            }
        }
    }
    if ($problems) { $fail++; Write-Host "FAIL  corpus $name -> fb2`n  $($problems -join "`n  ")" -ForegroundColor Red }
    else { $pass++; Write-Host "OK    corpus $name -> fb2" }
}

# 3c) pixels of the images taken from tests\corpus\handmade.pdf (tools\make_pdf_sample.ps1):
#     8-bit gray, 4-bit palette, 1-bit with Decode [1 0], CMYK — against their known patterns
Add-Type -AssemblyName System.Drawing
$hm = Join-Path $work 'c-handmade_pdf\handmade.fb2'
if (Test-Path $hm) {
    $x = New-Object Xml.XmlDocument
    $x.Load($hm)
    $ns = New-Object Xml.XmlNamespaceManager $x.NameTable
    $ns.AddNamespace('f', 'http://www.gribuser.ru/xml/fictionbook/2.0')
    $refs = @($x.SelectNodes('//f:body[not(@name)]//f:image', $ns) | ForEach-Object { $_.GetAttribute('href', 'http://www.w3.org/1999/xlink').TrimStart('#') })
    $expect = @(
        { param($px, $py) $v = ($px * 6) % 256; @($v, $v, $v) },
        { param($px, $py) $i = ($px + $py) % 16; @(($i * 16), (255 - $i * 16), (($i * 7 * 16) % 256)) },
        { param($px, $py) $v = if ((([Math]::Floor($px / 5) + [Math]::Floor($py / 5)) % 2) -eq 1) { 0 } else { 255 }; @($v, $v, $v) },
        { param($px, $py) @((255 - $px * 6), (255 - $py * 8), 255) })
    $names = 'gray 8-bit', 'palette 4-bit', '1-bit Decode[1 0]', 'CMYK'
    for ($k = 0; $k -lt 4; $k++) {
        $bad = 'missing'
        if ($k -lt $refs.Count) {
            $bin = $x.SelectSingleNode("//f:binary[@id='$($refs[$k])']", $ns)
            $bmp = New-Object Drawing.Bitmap (New-Object IO.MemoryStream (, [Convert]::FromBase64String($bin.InnerText)))
            $bad = 0
            for ($py = 0; $py -lt 30; $py++) {
                for ($px = 0; $px -lt 40; $px++) {
                    $want = & $expect[$k] $px $py
                    $c = $bmp.GetPixel($px, $py)
                    if ([Math]::Abs($c.R - $want[0]) -gt 1 -or [Math]::Abs($c.G - $want[1]) -gt 1 -or [Math]::Abs($c.B - $want[2]) -gt 1) { $bad++ }
                }
            }
            $bmp.Dispose()
        }
        if ($bad -eq 0) { $pass++; Write-Host "OK    PDF image pixels: $($names[$k])" }
        else { $fail++; Write-Host "FAIL  PDF image pixels: $($names[$k]): $bad wrong pixels of 1200" -ForegroundColor Red }
    }
}

# 3d) DjVu text layer (BZZ + zone tree) page by page against <name>.reftext.json: the text
#     Wikimedia Commons extracted from the same files with DjVuLibre. OCR junk (control
#     characters, broken UTF-8) is dropped on both sides, as cleanText does.
function DjvuNorm([string]$s) { ((($s -replace [string][char]0xFFFD, '') -replace '[\u0000-\u001F\u007F]', ' ') -replace '\s+', ' ').Trim() }
foreach ($ref in Get-ChildItem $corpus -Filter *.reftext.json -ErrorAction SilentlyContinue) {
    $name = $ref.Name -replace '\.reftext\.json$', ''
    $raw = Join-Path $work "$name.djvu.raw"
    $env:FB2CONV_DJVUDEBUG = $raw
    $null = & $cli -f fb2 -o (Join-Path $work 'djvu-raw') (Join-Path $corpus "$name.djvu") | Out-String
    $env:FB2CONV_DJVUDEBUG = ''
    if (-not (Test-Path $raw)) { $fail++; Write-Host "FAIL  DjVu text layer ${name}: nothing decoded" -ForegroundColor Red; continue }
    # ConvertFrom-Json passes a JSON array down the pipeline as one object: assign it first
    $want = [IO.File]::ReadAllText($ref.FullName, [Text.Encoding]::UTF8) | ConvertFrom-Json
    $got = [IO.File]::ReadAllText($raw, [Text.Encoding]::UTF8).Split([char]12)
    $bad = @()
    for ($k = 0; $k -lt $want.Count; $k++) { if ((DjvuNorm $want[$k]) -ne (DjvuNorm $got[$k])) { $bad += $k + 1 } }
    if ($bad) { $fail++; Write-Host "FAIL  DjVu text layer ${name}: pages $($bad -join ', ') of $($want.Count) differ" -ForegroundColor Red }
    else { $pass++; Write-Host "OK    DjVu text layer ${name}: $($want.Count) pages identical" }
}
# a scan without a text layer is refused with a reason, not turned into an empty book
$nt = Join-Path $corpus 'djvu-notext.djvu'
if (Test-Path $nt) {
    $log = & $cli -f fb2 -o (Join-Path $work 'djvu-notext') $nt | Out-String
    if ($LASTEXITCODE -eq 1 -and $log -match (-join [char[]](0x0442, 0x0435, 0x043A, 0x0441, 0x0442, 0x043E, 0x0432))) {
        $pass++; Write-Host "OK    DjVu without text layer -> refused with a reason"
    } else { $fail++; Write-Host "FAIL  DjVu without text layer: exit $LASTEXITCODE, $($log.Trim())" -ForegroundColor Red }
}

# 5) console mode of the GUI exe: output reaches a pipe, exit codes are right
$ver = (& $cli -v | Out-String).Trim()
$null = & $cli -f nosuch "$work\x.fb2" | Out-String; $badFmt = $LASTEXITCODE
$help = & $cli -h | Out-String
if ($ver -match '^FB2 .+ \d+\.\d+$' -and $badFmt -eq 2 -and $help -match 'start /wait') {
    $pass++; Write-Host "OK    console mode: '$ver', bad format -> exit 2, help printed"
} else {
    $fail++; Write-Host "FAIL  console mode: ver='$ver' badFmt=$badFmt help=$($help.Length) chars" -ForegroundColor Red
}

# 6) zero traces: the real open dialog writes history to the registry; with
#    NoTraceGuard the registry must stay byte-for-byte the same
$nt = "$work\notrace_test.exe"
$gpp = 'C:\dev\mingw32\bin\g++.exe'
$env:PATH = "C:\dev\mingw32\bin;$env:PATH"
cmd /c "`"$gpp`" -m32 -D_WIN32_WINNT=0x0501 -DUNICODE -D_UNICODE -O2 `"$root\tools\notrace_test.cpp`" `"$root\src\notrace.cpp`" -o `"$nt`" -static -lshlwapi -ladvapi32 -lcomdlg32 -lshell32 -luser32"
# a brand-new folder each run, so Windows has to create shellbags for it
$ntDir = Join-Path $work ("dlg-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $ntDir | Out-Null
Copy-Item "$root\tests\deep-toc.fb2" $ntDir
$ntOut = & $nt "$ntDir\deep-toc.fb2" | Out-String
Write-Host $ntOut.Trim()
if ($LASTEXITCODE -eq 0) { $pass++; Write-Host "OK    no registry traces from the open dialog" }
else { $fail++; Write-Host "FAIL  registry traces:`n$ntOut" -ForegroundColor Red }

foreach ($exe in "$root\build\fb2conv.exe") {
    $imports = & $objdump -p $exe | Select-String '^\s+[0-9a-f]+\s+\d+\s+(\w+)' |
               ForEach-Object { $_.Matches[0].Groups[1].Value }
    $bad = $imports | Where-Object { $_ -match "^($notOnXp)" }
    $sub = (& $objdump -p $exe | Select-String 'MajorOSystemVersion|MajorSubsystemVersion') -join '; '
    if ($bad) {
        Write-Host "FAIL  $([IO.Path]::GetFileName($exe)) imports non-XP API: $($bad -join ', ')" -ForegroundColor Red
        $fail++
    } else {
        Write-Host "OK    $([IO.Path]::GetFileName($exe)): $($imports.Count) imports, all XP-safe ($sub)"
    }
}

Write-Host ""
Write-Host "Conversions passed: $pass, failed: $fail"
Write-Host "Output: $work"
exit ([int]($fail -ne 0))
