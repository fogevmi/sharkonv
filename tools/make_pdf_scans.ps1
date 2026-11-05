# Builds test PDFs in tests\corpus, without Sharkonv's own code (only .NET crypto and the TIFF encoder):
#   crypt-rc4-40.pdf, crypt-rc4-128.pdf, crypt-aes-128.pdf, crypt-aes-256.pdf - empty user password
#   crypt-password.pdf - needs a real password, must be refused with a reason
#   ccitt-g4.pdf, ccitt-g3.pdf - one 1-bit picture, stream taken from a real TIFF encoder
param([string]$Out = (Join-Path $PSScriptRoot '..\tests\corpus'))
Add-Type -AssemblyName System.Drawing
$latin = [Text.Encoding]::GetEncoding(28591)
$pad = [byte[]](0x28,0xBF,0x4E,0x5E,0x4E,0x75,0x8A,0x41,0x64,0x00,0x4E,0x56,0xFF,0xFA,0x01,0x08,0x2E,0x2E,0x00,0xB6,0xD0,0x68,0x3E,0x80,0x2F,0x0C,0xA9,0xFE,0x64,0x53,0x69,0x7A)

function Md5([byte[]]$b) { [Security.Cryptography.MD5]::Create().ComputeHash($b) }
function Sha([int]$bits, [byte[]]$b) {
    $h = switch ($bits) { 256 { [Security.Cryptography.SHA256]::Create() } 384 { [Security.Cryptography.SHA384]::Create() } default { [Security.Cryptography.SHA512]::Create() } }
    $h.ComputeHash($b)
}
function Rc4([byte[]]$key, [byte[]]$data) {
    $s = 0..255
    $j = 0
    for ($i = 0; $i -lt 256; $i++) { $j = ($j + $s[$i] + $key[$i % $key.Length]) -band 255; $t = $s[$i]; $s[$i] = $s[$j]; $s[$j] = $t }
    $o = New-Object byte[] $data.Length
    $a = 0; $b = 0
    for ($k = 0; $k -lt $data.Length; $k++) {
        $a = ($a + 1) -band 255; $b = ($b + $s[$a]) -band 255
        $t = $s[$a]; $s[$a] = $s[$b]; $s[$b] = $t
        $o[$k] = [byte]($data[$k] -bxor $s[($s[$a] + $s[$b]) -band 255])
    }
    ,$o
}
function AesCbc([byte[]]$key, [byte[]]$iv, [byte[]]$data, [bool]$pkcs) {
    $a = [Security.Cryptography.Aes]::Create()
    $a.Mode = 'CBC'
    $a.Padding = $(if ($pkcs) { 'PKCS7' } else { 'None' })
    $a.Key = $key
    $a.IV = $iv
    $e = $a.CreateEncryptor()
    ,$e.TransformFinalBlock($data, 0, $data.Length)
}
function JoinBytes() { $l = New-Object Collections.Generic.List[byte]; foreach ($x in $args) { if ($x) { $l.AddRange([byte[]]$x) } }; ,$l.ToArray() }
function Hex([byte[]]$b) { '<' + (($b | ForEach-Object { '{0:X2}' -f $_ }) -join '') + '>' }
function Sub([byte[]]$b, [int]$o, [int]$n) { $r = New-Object byte[] $n; [Array]::Copy($b, $o, $r, 0, $n); ,$r }

# hash 2.B of ISO 32000-2 for the empty password
function R6Hash([byte[]]$salt) {
    $k = Sha 256 $salt
    $i = 0
    $e = $null
    while ($i -lt 64 -or $e[$e.Length - 1] -gt ($i - 32)) {
        $k1 = New-Object Collections.Generic.List[byte]
        for ($r = 0; $r -lt 64; $r++) { $k1.AddRange([byte[]]$k) }
        $e = AesCbc (Sub $k 0 16) (Sub $k 16 16) $k1.ToArray() $false
        $m = 0
        for ($b = 0; $b -lt 16; $b++) { $m += $e[$b] }
        $k = Sha $(switch ($m % 3) { 0 { 256 } 1 { 384 } default { 512 } }) $e
        $i++
    }
    ,(Sub $k 0 32)
}

function ObjKey([byte[]]$fileKey, [int]$num, [bool]$aes) {
    $m = JoinBytes $fileKey ([byte[]]@(($num -band 255), (($num -shr 8) -band 255), (($num -shr 16) -band 255), 0, 0))
    if ($aes) { $m = JoinBytes $m ([Text.Encoding]::ASCII.GetBytes('sAlT')) }
    ,(Sub (Md5 $m) 0 ([Math]::Min($fileKey.Length + 5, 16)))
}

$rng = New-Object Random 42
function RandBytes([int]$n) { $b = New-Object byte[] $n; $rng.NextBytes($b); ,$b }

function WriteCrypt([string]$name, [string]$mode) {
    $id = RandBytes 16
    $o = RandBytes 32
    $p = -44
    $pb = [BitConverter]::GetBytes([int]$p)
    $aes = $mode -eq 'aes128' -or $mode -eq 'aes256'
    $encDict = ''
    switch ($mode) {
        'rc440' {
            $key = Sub (Md5 (JoinBytes $pad $o $pb $id)) 0 5
            $u = Rc4 $key $pad
            $encDict = "/Filter/Standard/V 1/R 2/O $(Hex $o)/U $(Hex $u)/P $p"
        }
        { $_ -eq 'rc4128' -or $_ -eq 'aes128' -or $_ -eq 'password' } {
            $k = Md5 (JoinBytes $pad $o $pb $id)
            for ($i = 0; $i -lt 50; $i++) { $k = Md5 $k }
            $key = $k
            $x = Rc4 $key (Md5 (JoinBytes $pad $id))
            for ($i = 1; $i -le 19; $i++) {
                $ki = New-Object byte[] 16
                for ($j = 0; $j -lt 16; $j++) { $ki[$j] = [byte]($key[$j] -bxor $i) }
                $x = Rc4 $ki $x
            }
            $u = JoinBytes $x (RandBytes 16)
            if ($mode -eq 'password') { $u = RandBytes 32 } # the empty password does not fit
            if ($mode -eq 'aes128') {
                $encDict = "/Filter/Standard/V 4/R 4/Length 128/CF<</StdCF<</CFM/AESV2/AuthEvent/DocOpen/Length 16>>>>/StmF/StdCF/StrF/StdCF/O $(Hex $o)/U $(Hex $u)/P $p"
            } else {
                $encDict = "/Filter/Standard/V 2/R 3/Length 128/O $(Hex $o)/U $(Hex $u)/P $p"
            }
        }
        'aes256' {
            $key = RandBytes 32
            $vs = RandBytes 8; $ks = RandBytes 8
            $u = JoinBytes (R6Hash $vs) $vs $ks
            $ue = AesCbc (R6Hash $ks) (New-Object byte[] 16) $key $false
            $encDict = "/Filter/Standard/V 5/R 6/Length 256/CF<</StdCF<</CFM/AESV3/AuthEvent/DocOpen/Length 32>>>>/StmF/StdCF/StrF/StdCF/O $(Hex (RandBytes 48))/U $(Hex $u)/OE $(Hex (RandBytes 32))/UE $(Hex $ue)/Perms $(Hex (RandBytes 16))/P $p/EncryptMetadata true"
        }
    }
    function Enc([int]$num, [byte[]]$d) {
        if ($mode -eq 'aes256') { $iv = RandBytes 16; return ,(JoinBytes $iv (AesCbc $key $iv $d $true)) }
        if ($mode -eq 'aes128') { $iv = RandBytes 16; return ,(JoinBytes $iv (AesCbc (ObjKey $key $num $true) $iv $d $true)) }
        ,(Rc4 (ObjKey $key $num $false) $d)
    }
    $title = Enc 6 ([Text.Encoding]::ASCII.GetBytes("Encrypted sample $mode"))
    $text = 'BT /F1 14 Tf 72 700 Td (The quick brown fox opens a locked book without any password.) Tj ET'
    $content = Enc 4 ([Text.Encoding]::ASCII.GetBytes($text))
    $ms = New-Object IO.MemoryStream
    $off = @{}
    function W([string]$s) { $b = $latin.GetBytes($s); $ms.Write($b, 0, $b.Length) }
    W "%PDF-1.7`n"
    $off[1] = $ms.Position; W "1 0 obj`n<</Type/Catalog/Pages 2 0 R>>`nendobj`n"
    $off[2] = $ms.Position; W "2 0 obj`n<</Type/Pages/Kids[3 0 R]/Count 1>>`nendobj`n"
    $off[3] = $ms.Position; W "3 0 obj`n<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Resources<</Font<</F1 5 0 R>>>>/Contents 4 0 R>>`nendobj`n"
    $off[4] = $ms.Position; W "4 0 obj`n<</Length $($content.Length)>>`nstream`n"; $ms.Write($content, 0, $content.Length); W "`nendstream`nendobj`n"
    $off[5] = $ms.Position; W "5 0 obj`n<</Type/Font/Subtype/Type1/BaseFont/Helvetica/Encoding/WinAnsiEncoding>>`nendobj`n"
    $off[6] = $ms.Position; W "6 0 obj`n<</Title $(Hex $title)>>`nendobj`n"
    $off[7] = $ms.Position; W "7 0 obj`n<<$encDict>>`nendobj`n"
    $xr = $ms.Position
    W "xref`n0 8`n0000000000 65535 f `n"
    for ($i = 1; $i -le 7; $i++) { W ('{0:D10} 00000 n `n' -f $off[$i]).Replace('`n', "`n") }
    W "trailer`n<</Size 8/Root 1 0 R/Info 6 0 R/Encrypt 7 0 R/ID[$(Hex $id)$(Hex $id)]>>`nstartxref`n$xr`n%%EOF`n"
    [IO.File]::WriteAllBytes((Join-Path $Out $name), $ms.ToArray())
}

WriteCrypt 'crypt-rc4-40.pdf' 'rc440'
WriteCrypt 'crypt-rc4-128.pdf' 'rc4128'
WriteCrypt 'crypt-aes-128.pdf' 'aes128'
WriteCrypt 'crypt-aes-256.pdf' 'aes256'
WriteCrypt 'crypt-password.pdf' 'password'

# ---- CCITT: a 1-bit picture coded by the Windows TIFF encoder ----
function TiffStrip([byte[]]$t) {
    # strips of a little-endian TIFF -> one byte array (the encoder writes one image)
    $ifd = [BitConverter]::ToInt32($t, 4)
    $n = [BitConverter]::ToUInt16($t, $ifd)
    $res = @{}
    for ($i = 0; $i -lt $n; $i++) {
        $e = $ifd + 2 + 12 * $i
        $tag = [BitConverter]::ToUInt16($t, $e)
        $type = [BitConverter]::ToUInt16($t, $e + 2)
        $count = [BitConverter]::ToInt32($t, $e + 4)
        $sz = if ($type -eq 3) { 2 } else { 4 }
        $vals = @()
        $at = if ($count * $sz -le 4) { $e + 8 } else { [BitConverter]::ToInt32($t, $e + 8) }
        for ($k = 0; $k -lt $count; $k++) { $vals += $(if ($type -eq 3) { [BitConverter]::ToUInt16($t, $at + $k * 2) } else { [BitConverter]::ToInt32($t, $at + $k * 4) }) }
        $res[[int]$tag] = $vals
    }
    $l = New-Object Collections.Generic.List[byte]
    for ($i = 0; $i -lt $res[273].Count; $i++) { $l.AddRange([byte[]](Sub $t $res[273][$i] $res[279][$i])) }
    @{ data = $l.ToArray(); photometric = $res[262][0]; fillOrder = $(if ($res[266]) { $res[266][0] } else { 1 }); t4 = $(if ($res[292]) { $res[292][0] } else { 0 }); rows = $res[278][0] }
}

function WriteCcitt([string]$name, [string]$comp, [int]$k) {
    $w = 400; $h = 300
    $bmp = New-Object Drawing.Bitmap $w, $h
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.Clear([Drawing.Color]::White)
    $f = New-Object Drawing.Font 'Arial', 28
    $g.DrawString('Sharkonv CCITT', $f, [Drawing.Brushes]::Black, 10, 20)
    $g.FillRectangle([Drawing.Brushes]::Black, 40, 110, 120, 60)
    $g.DrawEllipse((New-Object Drawing.Pen ([Drawing.Color]::Black), 5), 200, 100, 150, 150)
    for ($i = 0; $i -lt 30; $i++) { $g.DrawLine([Drawing.Pens]::Black, 20, 190 + $i * 3, 20 + $i * 5, 280) }
    $g.Dispose()
    $mono = $bmp.Clone((New-Object Drawing.Rectangle 0, 0, $w, $h), [Drawing.Imaging.PixelFormat]::Format1bppIndexed)
    $codec = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/tiff' }
    $ep = New-Object Drawing.Imaging.EncoderParameters 1
    $ep.Param[0] = New-Object Drawing.Imaging.EncoderParameter ([Drawing.Imaging.Encoder]::Compression), ([long][Drawing.Imaging.EncoderValue]::$comp)
    $ms = New-Object IO.MemoryStream
    $mono.Save($ms, $codec, $ep)
    $tf = TiffStrip $ms.ToArray()
    if ($tf.fillOrder -ne 1) { throw "$name : fill order $($tf.fillOrder)" }
    # photometric 1 (BlackIsZero) means the coded 'black' runs are white pixels here
    $parms = "/K $k/Columns $w/Rows $h"
    if ($tf.photometric -eq 1) { $parms += '/BlackIs1 true' }
    $data = $tf.data
    $s = New-Object IO.MemoryStream
    $off = @{}
    function W2([string]$x) { $b = $latin.GetBytes($x); $s.Write($b, 0, $b.Length) }
    W2 "%PDF-1.4`n"
    $off[1] = $s.Position; W2 "1 0 obj`n<</Type/Catalog/Pages 2 0 R>>`nendobj`n"
    $off[2] = $s.Position; W2 "2 0 obj`n<</Type/Pages/Kids[3 0 R]/Count 1>>`nendobj`n"
    $off[3] = $s.Position; W2 "3 0 obj`n<</Type/Page/Parent 2 0 R/MediaBox[0 0 $w $h]/Resources<</XObject<</I 5 0 R>>>>/Contents 4 0 R>>`nendobj`n"
    $c = "q $w 0 0 $h 0 0 cm /I Do Q"
    $off[4] = $s.Position; W2 "4 0 obj`n<</Length $($c.Length)>>`nstream`n$c`nendstream`nendobj`n"
    $off[5] = $s.Position; W2 "5 0 obj`n<</Type/XObject/Subtype/Image/Width $w/Height $h/BitsPerComponent 1/ColorSpace/DeviceGray/Filter/CCITTFaxDecode/DecodeParms<<$parms>>/Length $($data.Length)>>`nstream`n"
    $s.Write($data, 0, $data.Length)
    W2 "`nendstream`nendobj`n"
    $xr = $s.Position
    W2 "xref`n0 6`n0000000000 65535 f `n"
    for ($i = 1; $i -le 5; $i++) { W2 ('{0:D10} 00000 n `n' -f $off[$i]).Replace('`n', "`n") }
    W2 "trailer`n<</Size 6/Root 1 0 R>>`nstartxref`n$xr`n%%EOF`n"
    [IO.File]::WriteAllBytes((Join-Path $Out $name), $s.ToArray())
    "$name : $($data.Length) bytes, photometric $($tf.photometric), T4 options $($tf.t4)"
}
WriteCcitt 'ccitt-g4.pdf' 'CompressionCCITT4' -1
WriteCcitt 'ccitt-g3.pdf' 'CompressionCCITT3' 0
"OK"
