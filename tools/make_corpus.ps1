# Builds tests\corpus\: books made by OTHER programs (or reproducing their structure),
# each with a .expect file describing the FB2 that must come out of it.
#  - word.*      : made by Microsoft Word itself via COM (docx, htm, rtf, txt, pdf, odt)
#  - epub-*.epub : hand-made copies of typical third-party EPUB layouts
# Word is needed only to regenerate word.*; the files are kept in the repository.
param([switch]$SkipWord)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
$out = "$root\tests\corpus"
New-Item -ItemType Directory -Force $out | Out-Null
Add-Type -AssemblyName System.Drawing, System.IO.Compression, System.IO.Compression.FileSystem
$utf8 = New-Object Text.UTF8Encoding $false

function Expect([string]$name, [string]$text) {
    [IO.File]::WriteAllText("$out\$name.expect", ($text.Trim() -replace "`r?`n", "`n") + "`n", $utf8)
}

# Full tree of the reference book. Format of .expect:
#   tree lines (two spaces per nesting level), then key=value checks.
$fullTree = @'
Часть первая
  Глава 1. Начало
    1.1. Подраздел
  Глава 2. Продолжение
Часть вторая
  Глава 3. Финал
Эпилог
'@

# ----------------------------------------------------------------- Word ----
if (-not $SkipWord) {
    $png = "$out\_pic.png"
    $bmp = New-Object Drawing.Bitmap 120, 80
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.Clear([Drawing.Color]::SteelBlue); $g.FillEllipse([Drawing.Brushes]::Gold, 30, 10, 60, 60)
    $g.Dispose(); $bmp.Save($png, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()

    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.DisplayAlerts = 0
    try {
        $doc = $word.Documents.Add()
        # document properties are reachable from PowerShell only through late binding
        $props = $doc.BuiltInDocumentProperties
        foreach ($kv in @(@('Title', 'Книга из Word'), @('Author', 'Анна Вордова'))) {
            $p = [__ComObject].InvokeMember('Item', 'GetProperty', $null, $props, @($kv[0]))
            [__ComObject].InvokeMember('Value', 'SetProperty', $null, $p, @($kv[1])) | Out-Null
        }
        $sel = $word.Selection
        function Para([int]$style, [string]$text) {
            $sel.Style = $doc.Styles.Item($style); $sel.TypeText($text); $sel.TypeParagraph()
        }
        function Body([scriptblock]$fill) { $sel.Style = $doc.Styles.Item(-1); & $fill; $sel.TypeParagraph() }
        function Run([string]$t, [int]$b = 0, [int]$i = 0) {
            $sel.Font.Bold = $b; $sel.Font.Italic = $i; $sel.TypeText($t); $sel.Font.Bold = 0; $sel.Font.Italic = 0
        }
        # after Footnotes.Add the cursor stays BEFORE the new reference mark: move it to the end
        function Note([string]$t) { $null = $doc.Footnotes.Add($sel.Range, [Type]::Missing, $t); $null = $sel.EndKey(6) }

        Para -63 'Книга из Word'                       # Title style
        Para -2 'Часть первая'                         # Heading 1
        Para -3 'Глава 1. Начало'                      # Heading 2
        Body { Run 'Обычный текст, '; Run 'жирное слово' 1; Run ' и '; Run 'курсивное слово' 0 1; Run ', а здесь сноска'; Note 'Текст первой сноски.'; Run '.' }
        Body { Run 'Второй абзац первой главы.' }
        Para -4 '1.1. Подраздел'                       # Heading 3
        Body { Run 'Список ниже:' }
        $sel.Style = $doc.Styles.Item(-49)            # List Bullet
        $sel.TypeText('первый пункт'); $sel.TypeParagraph(); $sel.TypeText('второй пункт'); $sel.TypeParagraph()
        Body { Run 'Таблица:' }
        $t = $doc.Tables.Add($sel.Range, 2, 2)
        $t.Borders.Enable = 1
        $t.Cell(1, 1).Range.Text = 'Ключ'; $t.Cell(1, 2).Range.Text = 'Значение'
        $t.Cell(2, 1).Range.Text = 'Один'; $t.Cell(2, 2).Range.Text = '1'
        $sel.EndKey(6) | Out-Null                      # wdStory
        $sel.TypeParagraph()
        Para -3 'Глава 2. Продолжение'
        $sel.Style = $doc.Styles.Item(-1)
        $null = $sel.InlineShapes.AddPicture($png)
        $sel.TypeParagraph()
        Body { Run 'Текст после картинки со второй сноской'; Note 'Вторая сноска.'; Run '.' }
        Para -2 'Часть вторая'
        Para -3 'Глава 3. Финал'
        Body { Run 'Последняя глава.' }
        Para -2 'Эпилог'
        Body { Run 'Конец книги.' }

        $base = "$out\word"
        $doc.SaveAs2("$base.docx", 16)                 # wdFormatXMLDocument
        $doc.SaveAs2("$base.doc", 0)                    # Word 97-2003
        $doc.SaveAs2("$base.htm", 10)                  # wdFormatFilteredHTML
        $doc.SaveAs2("$base.rtf", 6)                   # wdFormatRTF
        $doc.SaveAs2("$base.odt", 23)                  # wdFormatOpenDocumentText
        $doc.SaveAs2("$base.txt", 7, $false, '', $false, '', $false, $false, $false, $false, $false, 65001) # UTF-8 text
        # PDF with bookmarks made from headings (wdExportCreateHeadingBookmarks = 1)
        $doc.ExportAsFixedFormat("$base.pdf", 17, $false, 0, 0, 1, 1, 0, $true, $true, 1, $true, $true, $false)
        $doc.Close(0)
        # A long book: justified multi-line paragraphs, automatic hyphenation, page numbers in
        # the footer, paragraphs crossing page breaks. The PDF versions are checked against
        # the DOCX version of the same document word for word.
        $doc = $word.Documents.Add()
        $props = $doc.BuiltInDocumentProperties
        $p = [__ComObject].InvokeMember('Item', 'GetProperty', $null, $props, @('Title'))
        [__ComObject].InvokeMember('Value', 'SetProperty', $null, $p, @('Длинная книга')) | Out-Null
        $doc.AutoHyphenation = $true
        $doc.Sections.Item(1).Footers.Item(1).PageNumbers.Add(1, $true) | Out-Null
        $sel = $word.Selection
        $words = 'вечер дорога старый город письмо окно ветер солнце человек время история дом река лес ' +
                 'сердце мысль разговор переписка невероятный обстоятельство воспоминание приключение ' +
                 'путешественник библиотека электричество железнодорожный самостоятельность'
        $words = $words -split ' '
        $rnd = New-Object Random 42
        function Sentence {
            $n = $rnd.Next(6, 14)
            $s = (1..$n | ForEach-Object { $words[$rnd.Next($words.Count)] }) -join ' '
            $s.Substring(0, 1).ToUpper() + $s.Substring(1) + '.'
        }
        foreach ($ch in 1..3) {
            $sel.Style = $doc.Styles.Item(-2)
            $sel.TypeText("Глава $ch"); $sel.TypeParagraph()
            foreach ($pi in 1..5) {
                $sel.Style = $doc.Styles.Item(-1)
                $sel.ParagraphFormat.Alignment = 3       # по ширине
                $sel.ParagraphFormat.FirstLineIndent = 28
                $text = (1..$rnd.Next(4, 9) | ForEach-Object { Sentence }) -join ' '
                if ($ch -eq 2 -and $pi -eq 2) {
                    $sel.TypeText($text + ' Здесь ')
                    $sel.Font.Italic = 1; $sel.TypeText('выделенные слова'); $sel.Font.Italic = 0
                    $sel.TypeText(' и сноска')
                    $null = $doc.Footnotes.Add($sel.Range, [Type]::Missing, 'Сноска длинной книги.'); $null = $sel.EndKey(6)
                    $sel.TypeText('.')
                } else {
                    $sel.TypeText($text)
                }
                $sel.TypeParagraph()
            }
        }
        $base = "$out\word-long"
        $doc.SaveAs2("$base.docx", 16)
        $doc.SaveAs2("$base.doc", 0)                    # Word 97-2003
        $doc.ExportAsFixedFormat("$base.pdf", 17, $false, 0, 0, 1, 1, 0, $true, $true, 1, $true, $true, $false)
        $doc.ExportAsFixedFormat("$base-nobm.pdf", 17, $false, 0, 0, 1, 1, 0, $true, $true, 0, $true, $true, $false)
        $doc.Close(0)

        # Pictures and a table: a JPEG photo (stays DCT in the PDF), a PNG with transparency
        # (raw pixels + soft mask in the PDF), a bordered table with a merged header cell and
        # a cell with a manual line break.
        $jpg = "$out\_photo.jpg"; $pngA = "$out\_alpha.png"
        $bmp = New-Object Drawing.Bitmap 300, 200
        $g = [Drawing.Graphics]::FromImage($bmp)
        $br = New-Object Drawing.Drawing2D.LinearGradientBrush (New-Object Drawing.Rectangle 0, 0, 300, 200), ([Drawing.Color]::DarkGreen), ([Drawing.Color]::LightYellow), 30
        $g.FillRectangle($br, 0, 0, 300, 200); $g.FillEllipse([Drawing.Brushes]::OrangeRed, 200, 30, 60, 60)
        $g.Dispose(); $bmp.Save($jpg, [Drawing.Imaging.ImageFormat]::Jpeg); $bmp.Dispose()
        $bmp = New-Object Drawing.Bitmap 160, 120, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [Drawing.Graphics]::FromImage($bmp)
        $g.Clear([Drawing.Color]::Transparent); $g.FillEllipse([Drawing.Brushes]::RoyalBlue, 20, 10, 120, 100)
        $g.Dispose(); $bmp.Save($pngA, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()

        $doc = $word.Documents.Add()
        $sel = $word.Selection
        $sel.Style = $doc.Styles.Item(-2); $sel.TypeText('Иллюстрации'); $sel.TypeParagraph()
        $sel.Style = $doc.Styles.Item(-1); $sel.TypeText('Фотография ниже.'); $sel.TypeParagraph()
        $null = $sel.InlineShapes.AddPicture($jpg); $sel.TypeParagraph()
        $sel.TypeText('Подпись после фото.'); $sel.TypeParagraph()
        $null = $sel.InlineShapes.AddPicture($pngA); $sel.TypeParagraph()
        $sel.TypeText('Текст после прозрачной картинки.'); $sel.TypeParagraph()
        $sel.Style = $doc.Styles.Item(-2); $sel.TypeText('Таблица'); $sel.TypeParagraph()
        $sel.Style = $doc.Styles.Item(-1); $sel.TypeText('Хронология событий:'); $sel.TypeParagraph()
        $t = $doc.Tables.Add($sel.Range, 4, 2)
        $t.Borders.Enable = 1
        $t.Cell(1, 1).Merge($t.Cell(1, 2))
        $t.Cell(1, 1).Range.Text = 'Хронология'
        $t.Cell(2, 1).Range.Text = 'Год'; $t.Cell(2, 2).Range.Text = 'Событие'
        $t.Cell(3, 1).Range.Text = '1812'; $t.Cell(3, 2).Range.Text = 'Отечественная война'
        $t.Cell(4, 1).Range.Text = '1861'; $t.Cell(4, 2).Range.Text = "Отмена крепостного$([char]11)права"
        $sel.EndKey(6) | Out-Null
        $sel.TypeText('Текст после таблицы.'); $sel.TypeParagraph()
        $base = "$out\word-media"
        $doc.SaveAs2("$base.docx", 16)
        $doc.SaveAs2("$base.doc", 0)                    # Word 97-2003
        $doc.ExportAsFixedFormat("$base.pdf", 17, $false, 0, 0, 1, 1, 0, $true, $true, 1, $true, $true, $false)
        $doc.Close(0)
        Remove-Item $jpg, $pngA

        # Columns: an article with a one-column introduction, then two columns with
        # chapters (paragraphs flow from one column into the next), then one column again;
        # and a whole book set in three narrow columns.
        function LongPara {
            $t = (1..$rnd.Next(5, 9) | ForEach-Object { Sentence }) -join ' '
            $sel.Style = $doc.Styles.Item(-1)
            $sel.ParagraphFormat.Alignment = 3
            $sel.ParagraphFormat.FirstLineIndent = 20
            $sel.TypeText($t); $sel.TypeParagraph()
        }
        foreach ($cols in 2, 3) {
            $doc = $word.Documents.Add()
            $doc.AutoHyphenation = $true
            $sel = $word.Selection
            $name = if ($cols -eq 2) { 'Статья в две колонки' } else { 'Книга в три колонки' }
            $sel.Style = $doc.Styles.Item(-63); $sel.TypeText($name); $sel.TypeParagraph()
            if ($cols -eq 2) {
                LongPara
                $sel.InsertBreak(3)                                   # раздел на той же странице
                $doc.Sections.Item(2).PageSetup.TextColumns.SetCount(2)
            } else {
                $doc.Sections.Item(1).PageSetup.TextColumns.SetCount(3)
            }
            foreach ($ch in 1..2) {
                $sel.Style = $doc.Styles.Item(-2); $sel.TypeText("Глава $ch"); $sel.TypeParagraph()
                foreach ($k in 1..4) { LongPara }
            }
            if ($cols -eq 2) {
                $sel.InsertBreak(3)
                $doc.Sections.Item(3).PageSetup.TextColumns.SetCount(1)
                LongPara
            }
            $base = "$out\word-cols$cols"
            $doc.SaveAs2("$base.docx", 16)
            $doc.ExportAsFixedFormat("$base.pdf", 17, $false, 0, 0, 1, 1, 0, $true, $true, 1, $true, $true, $false)
            $doc.Close(0)
        }

        # A book in two columns the way books are set: first-line indents, no spacing between
        # paragraphs, sentence ends falling on column bottoms (Word does not stretch the last line
        # of a column there, so only the indent tells a new paragraph from a continued one).
        $doc = $word.Documents.Add()
        $doc.AutoHyphenation = $true
        $doc.Sections.Item(1).PageSetup.TextColumns.SetCount(2)
        $sel = $word.Selection
        $same = ('Длинный абзац для проверки колонок, в котором много слов и предложений. ' * 12)
        foreach ($ch in 1..6) {
            $sel.Style = $doc.Styles.Item(-2); $sel.TypeText("Глава $ch"); $sel.TypeParagraph()
            $sel.Style = $doc.Styles.Item(-1)
            $sel.ParagraphFormat.FirstLineIndent = 20; $sel.ParagraphFormat.SpaceAfter = 0; $sel.ParagraphFormat.Alignment = 3
            foreach ($k in 1..10) { $sel.TypeText($same); $sel.TypeParagraph() }
        }
        $base = "$out\word-cols2i"
        $doc.SaveAs2("$base.docx", 16)
        $doc.ExportAsFixedFormat("$base.pdf", 17, $false, 0, 0, 1, 1, 0, $true, $true, 1, $true, $true, $false)
        $doc.Close(0)

        # Tables without borders: numbers with spaces inside, a cell whose text wraps to a
        # second line, and a bulleted list next to them (a list must not become a table).
        $doc = $word.Documents.Add()
        $sel = $word.Selection
        $sel.Style = $doc.Styles.Item(-2); $sel.TypeText('Таблицы без рамок'); $sel.TypeParagraph()
        $sel.Style = $doc.Styles.Item(-1); $sel.TypeText('Крупные города:'); $sel.TypeParagraph()
        $rows = @(@('Город', 'Население', 'Год основания'), @('Москва', '13 000 000', '1147'),
                  @('Тверь', '400 000', '1135'), @('Псков', '190 000', '903'))
        $t = $doc.Tables.Add($sel.Range, 4, 3)
        $t.Borders.Enable = 0
        for ($r = 0; $r -lt 4; $r++) { for ($c = 0; $c -lt 3; $c++) { $t.Cell($r + 1, $c + 1).Range.Text = $rows[$r][$c] } }
        $sel.EndKey(6) | Out-Null
        $sel.TypeText('Список рядом с таблицей:'); $sel.TypeParagraph()
        $sel.Style = $doc.Styles.Item(-49)
        $sel.TypeText('первый пункт списка'); $sel.TypeParagraph(); $sel.TypeText('второй пункт списка'); $sel.TypeParagraph()
        $sel.Style = $doc.Styles.Item(-1); $sel.TypeText('События:'); $sel.TypeParagraph()
        $t = $doc.Tables.Add($sel.Range, 2, 2)
        $t.Borders.Enable = 0
        $t.Columns.Item(1).Width = 60; $t.Columns.Item(2).Width = 150
        $t.Cell(1, 1).Range.Text = '1812'; $t.Cell(1, 2).Range.Text = 'Отечественная война'
        $t.Cell(2, 1).Range.Text = '1861'; $t.Cell(2, 2).Range.Text = 'Отмена крепостного права в Российской империи'
        $sel.EndKey(6) | Out-Null
        $sel.TypeText('Текст после таблиц.'); $sel.TypeParagraph()
        $base = "$out\word-grid"
        $doc.SaveAs2("$base.docx", 16)
        $doc.SaveAs2("$base.doc", 0)                    # Word 97-2003
        $doc.ExportAsFixedFormat("$base.pdf", 17, $false, 0, 0, 1, 1, 0, $true, $true, 1, $true, $true, $false)
        $doc.Close(0)
    } finally {
        $word.Quit()
        [Runtime.InteropServices.Marshal]::ReleaseComObject($word) | Out-Null
    }
    Remove-Item $png
    if (Test-Path "$out\word_files") { Remove-Item "$out\word_files" -Recurse -Force }
}

foreach ($f in 'docx', 'htm', 'rtf', 'odt', 'pdf') {
    Expect "word.$f" ($fullTree + "`ntitle=Книга из Word`nnotes=2`nhas=strong,emphasis,table`nimages=1`ntable=Ключ;Значение|Один;1")
}
# plain text keeps only what can be recognised from words: «Часть», «Глава», «Эпилог»
# Word 97-2003 .doc: the same books, checked like DOCX and word for word against it
Expect 'word.doc' ($fullTree + "`ntitle=Книга из Word`nnotes=2`nhas=strong,emphasis,table`nimages=1`ntable=Ключ;Значение|Один;1`nsame=word.docx")
Expect 'word.txt' @'
Часть первая
  Глава 1. Начало
  Глава 2. Продолжение
Часть вторая
  Глава 3. Финал
Эпилог
'@

$longTree = "Глава 1`nГлава 2`nГлава 3"
Expect 'word-long.docx' ($longTree + "`nnotes=1")
# same=FILE: every paragraph must match the FB2 made from FILE word for word
Expect 'word-long.pdf' ($longTree + "`nnotes=1`nsame=word-long.docx`nhas=emphasis")
Expect 'word-long-nobm.pdf' ($longTree + "`nnotes=1`nsame=word-long.docx`nhas=emphasis")

# images=N: pictures in the main text; table=rows|separated;by cells, *N = colspan
$mediaTable = 'table=Хронология*2|Год;Событие|1812;Отечественная война|1861;Отмена крепостного права'
Expect 'word-media.docx' "Иллюстрации`nТаблица`nimages=2`n$mediaTable"
Expect 'word-media.pdf' "Иллюстрации`nТаблица`nimages=2`n$mediaTable`nsame=word-media.docx"

Expect 'word-long.doc' ($longTree + "`nnotes=1`nsame=word-long.docx")
Expect 'word-media.doc' "Иллюстрации`nТаблица`nimages=2`n$mediaTable`nsame=word-media.docx"
Expect 'word-cols2.docx' "Глава 1`nГлава 2`ntitle=Статья в две колонки"
Expect 'word-cols2.pdf' "Глава 1`nГлава 2`ntitle=Статья в две колонки`nsame=word-cols2.docx"
Expect 'word-cols3.docx' "Глава 1`nГлава 2`ntitle=Книга в три колонки"
Expect 'word-cols3.pdf' "Глава 1`nГлава 2`ntitle=Книга в три колонки`nsame=word-cols3.docx"
$sixTree = (1..6 | ForEach-Object { "Глава $_" }) -join "`n"
Expect 'word-cols2i.docx' $sixTree
Expect 'word-cols2i.pdf' "$sixTree`nsame=word-cols2i.docx"
$gridT1 ='table=Город;Население;Год основания|Москва;13 000 000;1147|Тверь;400 000;1135|Псков;190 000;903'
$gridT2 = 'table2=1812;Отечественная война|1861;Отмена крепостного права в Российской империи'
Expect 'word-grid.docx' "Таблицы без рамок`n$gridT1`n$gridT2"
Expect 'word-grid.doc' "Таблицы без рамок`n$gridT1`n$gridT2`nsame=word-grid.docx"
Expect 'word-grid.pdf' "Таблицы без рамок`n$gridT1`n$gridT2`nsame=word-grid.docx"

# ----------------------------------------------------------------- EPUB ----
function Epub([string]$name, [hashtable]$files) {
    $path = "$out\$name.epub"
    if (Test-Path $path) { Remove-Item $path }
    $z = [IO.Compression.ZipFile]::Open($path, 'Create')
    $e = $z.CreateEntry('mimetype', 'NoCompression')
    $w = New-Object IO.StreamWriter($e.Open(), [Text.Encoding]::ASCII); $w.Write('application/epub+zip'); $w.Close()
    foreach ($k in $files.Keys | Sort-Object) {
        $e = $z.CreateEntry($k, 'Optimal')
        $w = New-Object IO.StreamWriter($e.Open(), $utf8); $w.Write($files[$k]); $w.Close()
    }
    $z.Dispose()
}
$container = '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>'
function Xhtml([string]$body) { "<?xml version='1.0' encoding='utf-8'?>`n<html xmlns=`"http://www.w3.org/1999/xhtml`"><head><title>x</title></head><body>$body</body></html>" }

# 1) Calibre: several chapters in one file, TOC points to #anchors, notes at the end of
#    the last file, paragraphs with calibre classes.
Epub 'epub-calibre' @{
    'META-INF/container.xml' = $container
    'OEBPS/content.opf' = @'
<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="uuid_id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
<dc:title>Калибровка</dc:title><dc:creator opf:role="aut">Каля Брова</dc:creator><dc:language>ru</dc:language>
<meta name="calibre:series" content="Серия К"/><meta name="calibre:series_index" content="2.0"/>
</metadata>
<manifest><item id="html1" href="index_split_000.html" media-type="application/xhtml+xml"/>
<item id="html2" href="index_split_001.html" media-type="application/xhtml+xml"/>
<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest>
<spine toc="ncx"><itemref idref="html1"/><itemref idref="html2"/></spine></package>
'@
    'OEBPS/toc.ncx' = @'
<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head/><docTitle><text>Калибровка</text></docTitle><navMap>
<navPoint id="a1" playOrder="1"><navLabel><text>Часть первая</text></navLabel><content src="index_split_000.html#p1"/>
  <navPoint id="a2" playOrder="2"><navLabel><text>Глава 1</text></navLabel><content src="index_split_000.html#c1"/></navPoint>
  <navPoint id="a3" playOrder="3"><navLabel><text>Глава 2</text></navLabel><content src="index_split_000.html#c2"/></navPoint>
</navPoint>
<navPoint id="a4" playOrder="4"><navLabel><text>Часть вторая</text></navLabel><content src="index_split_001.html#p2"/>
  <navPoint id="a5" playOrder="5"><navLabel><text>Глава 3</text></navLabel><content src="index_split_001.html#c3"/></navPoint>
</navPoint></navMap></ncx>
'@
    'OEBPS/index_split_000.html' = (Xhtml @'
<h1 class="calibre1" id="p1">Часть первая</h1>
<h2 class="calibre2" id="c1">Глава 1</h2>
<p class="calibre3">Текст главы с <span class="bold">жирным</span>, <i class="calibre4">курсивом</i> и сноской<a href="index_split_001.html#fn1" class="calibre5"><sup class="calibre6">1</sup></a>.</p>
<h2 class="calibre2" id="c2">Глава 2</h2>
<p class="calibre3">Вторая глава.</p>
'@)
    'OEBPS/index_split_001.html' = (Xhtml @'
<h1 class="calibre1" id="p2">Часть вторая</h1>
<h2 class="calibre2" id="c3">Глава 3</h2>
<p class="calibre3">Третья глава.</p>
<hr class="calibre7"/>
<p class="calibre3" id="fn1"><a href="index_split_000.html#r1">1</a> Текст сноски Calibre.</p>
'@)
}
Expect 'epub-calibre.epub' @'
Часть первая
  Глава 1
  Глава 2
Часть вторая
  Глава 3
series=Серия К#2
notes=1
has=emphasis
'@

# 2) Russian EPUB converted from FB2 (fb2epub, LitRes style): one file per section,
#    titles are <div class="title"><p>…</p></div> — no <h1> at all. Only the TOC
#    knows the structure.
Epub 'epub-fb2style' @{
    'META-INF/container.xml' = $container
    'OEBPS/content.opf' = @'
<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Без заголовков</dc:title><dc:creator>Фэ Бэдва</dc:creator><dc:language>ru</dc:language></metadata>
<manifest><item id="s1" href="Text/section1.xhtml" media-type="application/xhtml+xml"/>
<item id="s2" href="Text/section2.xhtml" media-type="application/xhtml+xml"/>
<item id="s3" href="Text/section3.xhtml" media-type="application/xhtml+xml"/>
<item id="s4" href="Text/section4.xhtml" media-type="application/xhtml+xml"/>
<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest>
<spine toc="ncx"><itemref idref="s1"/><itemref idref="s2"/><itemref idref="s3"/><itemref idref="s4"/></spine></package>
'@
    'OEBPS/toc.ncx' = @'
<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head/><docTitle><text>x</text></docTitle><navMap>
<navPoint id="n1" playOrder="1"><navLabel><text>Книга первая</text></navLabel><content src="Text/section1.xhtml"/>
  <navPoint id="n2" playOrder="2"><navLabel><text>Глава I</text></navLabel><content src="Text/section2.xhtml"/></navPoint>
  <navPoint id="n3" playOrder="3"><navLabel><text>Глава II</text></navLabel><content src="Text/section3.xhtml"/></navPoint>
</navPoint>
<navPoint id="n4" playOrder="4"><navLabel><text>Послесловие</text></navLabel><content src="Text/section4.xhtml"/></navPoint>
</navMap></ncx>
'@
    'OEBPS/Text/section1.xhtml' = (Xhtml '<div class="title1"><p class="title">Книга первая</p></div><div class="epigraph"><p>Эпиграф к книге.</p><p class="text-author">Автор эпиграфа</p></div>')
    'OEBPS/Text/section2.xhtml' = (Xhtml '<div class="title2"><p class="title">Глава I</p></div><p>Первая глава <strong>с жирным</strong>.</p><div class="poem"><div class="stanza"><p class="v">Строка стиха</p><p class="v">Вторая строка</p></div></div>')
    'OEBPS/Text/section3.xhtml' = (Xhtml '<div class="title2"><p class="title">Глава II</p></div><p>Вторая <em>глава</em>.</p>')
    'OEBPS/Text/section4.xhtml' = (Xhtml '<div class="title1"><p class="title">Послесловие</p></div><p>Конец.</p>')
}
Expect 'epub-fb2style.epub' @'
Книга первая
  Глава I
  Глава II
Послесловие
has=strong,emphasis,poem
'@

# 3) Standard Ebooks style EPUB 3: nav.xhtml, <section epub:type="chapter">, parts in
#    their own files, endnotes in endnotes.xhtml with backlinks.
Epub 'epub-se' @{
    'META-INF/container.xml' = $container
    'OEBPS/content.opf' = @'
<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="uid">x</dc:identifier><dc:title>Стандарт</dc:title><dc:creator>Эс Иbooks</dc:creator><dc:language>ru</dc:language>
<meta property="belongs-to-collection" id="c">Серия С</meta><meta refines="#c" property="group-position">5</meta></metadata>
<manifest><item id="nav" href="toc.xhtml" media-type="application/xhtml+xml" properties="nav"/>
<item id="p1" href="text/part-1.xhtml" media-type="application/xhtml+xml"/>
<item id="c1" href="text/chapter-1.xhtml" media-type="application/xhtml+xml"/>
<item id="c2" href="text/chapter-2.xhtml" media-type="application/xhtml+xml"/>
<item id="en" href="text/endnotes.xhtml" media-type="application/xhtml+xml"/></manifest>
<spine><itemref idref="p1"/><itemref idref="c1"/><itemref idref="c2"/><itemref idref="en"/></spine></package>
'@
    'OEBPS/toc.xhtml' = @'
<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Содержание</title></head><body>
<nav epub:type="toc"><h2>Содержание</h2><ol>
<li><a href="text/part-1.xhtml">Часть I</a><ol>
  <li><a href="text/chapter-1.xhtml">Глава первая</a></li>
  <li><a href="text/chapter-2.xhtml">Глава вторая</a></li></ol></li>
<li><a href="text/endnotes.xhtml">Примечания</a></li></ol></nav>
<nav epub:type="landmarks"><ol><li><a href="text/chapter-1.xhtml" epub:type="bodymatter">Текст</a></li></ol></nav></body></html>
'@
    'OEBPS/text/part-1.xhtml' = (Xhtml '<section id="part-1" epub:type="part"><h2 epub:type="title">Часть I</h2></section>')
    'OEBPS/text/chapter-1.xhtml' = (Xhtml '<section id="part-1" epub:type="part"><section id="chapter-1" epub:type="chapter"><h3 epub:type="title">Глава первая</h3><p>Текст со сноской<a href="endnotes.xhtml#note-1" id="noteref-1" epub:type="noteref">1</a> и <i>курсивом</i>.</p><blockquote><p>Цитата.</p></blockquote></section></section>')
    'OEBPS/text/chapter-2.xhtml' = (Xhtml '<section id="part-1" epub:type="part"><section id="chapter-2" epub:type="chapter"><h3 epub:type="title">Глава вторая</h3><p>Ещё сноска<a href="endnotes.xhtml#note-2" id="noteref-2" epub:type="noteref">2</a>.</p></section></section>')
    'OEBPS/text/endnotes.xhtml' = (Xhtml '<section id="endnotes" epub:type="endnotes"><h2 epub:type="title">Примечания</h2><ol><li id="note-1" epub:type="endnote"><p>Первая концевая сноска. <a href="chapter-1.xhtml#noteref-1" epub:type="backlink">↩</a></p></li><li id="note-2" epub:type="endnote"><p>Вторая. <a href="chapter-2.xhtml#noteref-2" epub:type="backlink">↩</a></p></li></ol></section>')
}
Expect 'epub-se.epub' @'
Часть I
  Глава первая
  Глава вторая
series=Серия С#5
notes=2
has=emphasis,cite
'@

# ------------------------------------------------------------ plain HTML ----
# 4) pandoc-style HTML: footnotes in <section class="footnotes"><ol><li id="fn1">
[IO.File]::WriteAllText("$out\html-pandoc.html", @'
<!DOCTYPE html>
<html lang="ru"><head><meta charset="utf-8"><title>Пандок</title><meta name="author" content="Пан Док"></head><body>
<h1>Пандок</h1>
<h2 id="part">Раздел первый</h2>
<p>Текст со сноской<a href="#fn1" class="footnote-ref" id="fnref1" role="doc-noteref"><sup>1</sup></a>, <strong>жирным</strong> и <em>курсивом</em>.</p>
<h3 id="sub">Подраздел</h3>
<ol><li>пункт</li><li>пункт</li></ol>
<h2 id="part2">Раздел второй</h2>
<p>Ещё сноска<a href="#fn2" class="footnote-ref" id="fnref2" role="doc-noteref"><sup>2</sup></a>.</p>
<section class="footnotes" role="doc-endnotes"><hr><ol>
<li id="fn1"><p>Первая сноска.<a href="#fnref1" class="footnote-back" role="doc-backlink">↩︎</a></p></li>
<li id="fn2"><p>Вторая сноска.<a href="#fnref2" class="footnote-back" role="doc-backlink">↩︎</a></p></li>
</ol></section></body></html>
'@, $utf8)
Expect 'html-pandoc.html' @'
Раздел первый
  Подраздел
Раздел второй
notes=2
has=strong,emphasis
'@

Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize