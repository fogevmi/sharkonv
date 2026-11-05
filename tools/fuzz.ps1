# Robustness: feeds sharkonv.exe damaged copies of binary books (truncated, random bytes
# overwritten) and checks that every run ends cleanly (exit 0 or 1) within a time limit.
# A crash (access violation and the like) or a hang is a failure.
param([int]$Count = 150, [string[]]$Files = @('word.doc', 'word-media.doc', 'word.pdf', 'word-media.pdf', 'handmade.pdf', 'word.docx', 'word.rtf', 'djvu-maugham.djvu', 'djvu-janas.djvu', 'handmade.mobi', 'calibre-word.mobi', 'handmade-huff.mobi', 'handmade-cp1251.mobi','handmade.pdb', 'calibre-word-long.pdb','calibre-word.azw3', 'calibre-word-media.azw3', 'handmade.fb3', 'crypt-rc4-128.pdf', 'crypt-aes-128.pdf', 'crypt-aes-256.pdf', 'ccitt-g4.pdf', 'ccitt-g3.pdf', 'ia-polar-18-jbig2.pdf', 'ia-polar-5-jbig2.pdf', 'ia-pulps-4-jpx.pdf', 'ia-polar-21-jpx.pdf', "$env:windir\Help\mui\0419\mmc.CHM"))
$root = (Resolve-Path "$PSScriptRoot\..").Path
$exe = "$root\build\sharkonv.exe"
$work = Join-Path $env:TEMP 'sharkonv-fuzz'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory $work | Out-Null
$rnd = New-Object Random 1234
$bad = 0; $runs = 0
foreach ($name in $Files) {
    # a full path is a file outside the repository (Windows help): skipped when it is not there
    $path = if ([IO.Path]::IsPathRooted($name)) { $name } else { "$root\tests\corpus\$name" }
    if (-not (Test-Path $path)) { Write-Host "skip   $name"; continue }
    $src = [IO.File]::ReadAllBytes($path)
    $ext = [IO.Path]::GetExtension($name)
    for ($i = 0; $i -lt $Count; $i++) {
        $b = [byte[]]$src.Clone()
        switch ($i % 3) {
            0 { $b = $b[0..($rnd.Next(16, $b.Length - 1))] }                              # обрезка
            1 { for ($k = 0; $k -lt 1 + $rnd.Next(20); $k++) { $b[$rnd.Next($b.Length)] = [byte]$rnd.Next(256) } } # байты
            2 { $p = $rnd.Next($b.Length - 64); for ($k = 0; $k -lt 64; $k++) { $b[$p + $k] = 0xFF } }            # блок мусора
        }
        $f = "$work\case$i$ext"
        [IO.File]::WriteAllBytes($f, $b)
        $pi = New-Object Diagnostics.ProcessStartInfo $exe, "-f fb2 -o `"$work\out`" `"$f`""
        $pi.UseShellExecute = $false; $pi.RedirectStandardOutput = $true; $pi.CreateNoWindow = $true
        $pr = [Diagnostics.Process]::Start($pi)
        $null = $pr.StandardOutput.ReadToEnd()
        $runs++
        if (-not $pr.WaitForExit(15000)) {
            $pr.Kill(); $bad++
            Copy-Item $f "$work\HANG-$([IO.Path]::GetFileName($name))-$i$ext"
            Write-Host "HANG   $name case $i" -ForegroundColor Red
        } elseif ($pr.ExitCode -ne 0 -and $pr.ExitCode -ne 1) {
            $bad++
            Copy-Item $f "$work\CRASH-$([IO.Path]::GetFileName($name))-$i$ext"
            Write-Host ("CRASH  $name case $i exit=0x{0:X8}" -f $pr.ExitCode) -ForegroundColor Red
        }
        Remove-Item $f
    }
    Write-Host "done   $name"
}
Write-Host "runs: $runs, crashes/hangs: $bad (kept in $work)"
exit ([int]($bad -ne 0))
