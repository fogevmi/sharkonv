# Builds tests\corpus\handmade.fb3 by hand from the FB3 format description (github.com/gribuser/fb3),
# without Sharkonv's own code: package relations, description.xml with author and series, body.xml
# with nested sections, notes, a picture through body.xml.rels, a table, a list and a poem.
$root = (Resolve-Path "$PSScriptRoot\..").Path
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Drawing
$out = "$root\tests\corpus\handmade.fb3"
if (Test-Path $out) { Remove-Item $out -Force }

# a real PNG for the picture
$bmp = New-Object Drawing.Bitmap 40, 30
$g = [Drawing.Graphics]::FromImage($bmp)
$g.Clear([Drawing.Color]::SteelBlue)
$g.Dispose()
$ms = New-Object IO.MemoryStream
$bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)

# Cyrillic is written with code points: this script stays ASCII so that PowerShell 5.1 reads it correctly
function Ru([int[]]$c) { -join ($c | ForEach-Object { [char]$_ }) }
$part = Ru 0x427,0x430,0x441,0x442,0x44C,0x20,0x43F,0x435,0x440,0x432,0x430,0x44F   # Chast pervaya
$ch1 = Ru 0x413,0x43B,0x430,0x432,0x430,0x20,0x31                                    # Glava 1
$ch2 = Ru 0x413,0x43B,0x430,0x432,0x430,0x20,0x32                                    # Glava 2
$end = Ru 0x42D,0x43F,0x438,0x43B,0x43E,0x433                                        # Epilog
$name = Ru 0x41A,0x43D,0x438,0x433,0x430,0x20,0x46,0x42,0x33                         # Kniga FB3
$first = Ru 0x418,0x432,0x430,0x43D                                                  # Ivan
$last = Ru 0x41F,0x435,0x442,0x440,0x43E,0x432                                       # Petrov
$ser = Ru 0x421,0x435,0x440,0x438,0x44F                                              # Seriya

$parts = [ordered]@{}
$parts['[Content_Types].xml'] = '<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="png" ContentType="image/png"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/fb3/description.xml" ContentType="application/fb3-description+xml"/><Override PartName="/fb3/body.xml" ContentType="application/fb3-body+xml"/></Types>'
$parts['_rels/.rels'] = '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://www.fictionbook.org/FictionBook3/relationships/Book" Target="fb3/description.xml"/></Relationships>'
$parts['fb3/_rels/description.xml.rels'] = '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://www.fictionbook.org/FictionBook3/relationships/body" Target="body.xml"/></Relationships>'
$parts['fb3/_rels/body.xml.rels'] = '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="img1" Type="http://www.fictionbook.org/FictionBook3/relationships/image" Target="img/pic.png"/></Relationships>'
$parts['fb3/description.xml'] = "<?xml version=`"1.0`" encoding=`"UTF-8`"?><fb3-description xmlns=`"http://www.fictionbook.org/FictionBook3/description`" id=`"1`" version=`"1.0`"><title><main>$name</main></title><sequence name=`"$ser`" number=`"3`"/><fb3-relations><subject link=`"author`" id=`"a1`"><title><main>$first $last</main></title><first-name>$first</first-name><last-name>$last</last-name></subject></fb3-relations><lang>ru</lang><annotation><p>Annotation text.</p></annotation></fb3-description>"
$parts['fb3/body.xml'] = "<?xml version=`"1.0`" encoding=`"UTF-8`"?><fb3-body xmlns=`"http://www.fictionbook.org/FictionBook3/body`" xmlns:l=`"http://www.w3.org/1999/xlink`"><section id=`"s1`"><title><p>$part</p></title><section id=`"s2`"><title><p>$ch1</p></title><p>First paragraph with <strong>bold</strong> and <em>italic</em> words and a note<note role=`"footnote`" href=`"n1`">1</note>.</p><p><img src=`"img1`"/></p><table><tr><th>Key</th><th>Value</th></tr><tr><td>One</td><td>1</td></tr></table><ul><li>Item one</li><li>Item two</li></ul></section><section id=`"s3`"><title><p>$ch2</p></title><poem><stanza><p>Line one</p><p>Line two</p></stanza></poem><blockquote><p>Quoted words.</p></blockquote></section></section><section id=`"s4`"><title><p>$end</p></title><p>The end.</p></section><notes show=`"0`"><notebody id=`"n1`"><p>Text of the note.</p></notebody></notes></fb3-body>"

$zip = [IO.Compression.ZipFile]::Open($out, 'Create')
$utf8 = New-Object Text.UTF8Encoding $false
foreach ($k in $parts.Keys) {
    $e = $zip.CreateEntry($k)
    $s = $e.Open()
    $b = $utf8.GetBytes($parts[$k])
    $s.Write($b, 0, $b.Length)
    $s.Dispose()
}
$e = $zip.CreateEntry('fb3/img/pic.png')
$s = $e.Open()
$b = $ms.ToArray()
$s.Write($b, 0, $b.Length)
$s.Dispose()
$zip.Dispose()
"OK: $out"
