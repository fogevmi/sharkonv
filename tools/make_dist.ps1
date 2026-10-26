# Packs the built program into dist\ and two archives:
#   dist\fb2conv-<ver>.zip      - what users download (one exe + README.txt)
#   fb2-converter-cpp-src.zip   - sources that build with one build.bat call
# Run build.bat and tools\test.ps1 first.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
$ver = ([regex]'APP_VERSION L"([\d.]+)"').Match([IO.File]::ReadAllText("$root\src\main.cpp")).Groups[1].Value
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

function Zip([string]$zipPath, [hashtable]$files) {
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    $z = [IO.Compression.ZipFile]::Open($zipPath, 'Create')
    foreach ($k in ($files.Keys | Sort-Object)) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $files[$k], $k, 'Optimal') | Out-Null
    }
    $z.Dispose()
}

# dist\ : README.txt as UTF-8 with BOM + CRLF, so Notepad on XP shows it correctly
$dist = "$root\dist"
New-Item -ItemType Directory -Force $dist | Out-Null
# the exe may be running from dist\ (locked): replace it only when the build differs
Get-ChildItem $dist | Where-Object { $_.Name -ne 'fb2conv.exe' } | Remove-Item -Recurse -Force
$old = "$dist\fb2conv.exe"
if (-not (Test-Path $old) -or (Get-FileHash $old).Hash -ne (Get-FileHash "$root\build\fb2conv.exe").Hash) {
    Copy-Item "$root\build\fb2conv.exe" $dist -Force
}
$readme = [IO.File]::ReadAllText("$root\dist-readme.txt") -replace "`r?`n", "`r`n"
[IO.File]::WriteAllText("$dist\README.txt", $readme, (New-Object Text.UTF8Encoding $true))

$userZip = "$dist\fb2conv-$ver.zip"
Zip $userZip @{
    'fb2conv/fb2conv.exe' = "$dist\fb2conv.exe"
    'fb2conv/README.txt'  = "$dist\README.txt"
}

$src = @{}
foreach ($f in Get-ChildItem "$root\src", "$root\tools", "$root\tests" -File -Recurse) {
    $rel = $f.FullName.Substring($root.Length + 1).Replace('\', '/')
    $src["fb2-converter-cpp/" + $rel] = $f.FullName
}
foreach ($n in 'build.bat', 'README.md', 'dist-readme.txt') { $src["fb2-converter-cpp/$n"] = "$root\$n" }
Zip "$root\fb2-converter-cpp-src.zip" $src

Get-ChildItem $dist, "$root\fb2-converter-cpp-src.zip" -File |
    Select-Object Name, Length, @{n = 'SHA1'; e = { (Get-FileHash $_.FullName -Algorithm SHA1).Hash.Substring(0, 12) } } |
    Format-Table -AutoSize
