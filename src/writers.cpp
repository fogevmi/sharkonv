/* Писатели: FB2, EPUB, HTML, TXT, MOBI. */
#include "convert.h"
#include "markup.h"
#include "zip.h"

/* ====================== HTML-фрагмент -> FB2 =========================== */

static Bytes toBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

static std::string fragToFB2(const std::string& html, std::set<std::string>& used);

static std::string inlineToFB2(const MNode* node, std::set<std::string>& used) {
    std::string s;
    for (size_t i = 0; i < node->kids.size(); i++) {
        const MNode* ch = node->kids[i].get();
        if (ch->text) { s += escXml(ch->value); continue; }
        const std::string& t = ch->name;
        std::string inner = inlineToFB2(ch, used);
        if (t == "strong" || t == "b") s += "<strong>" + inner + "</strong>";
        else if (t == "em" || t == "i") s += "<emphasis>" + inner + "</emphasis>";
        else if (t == "s" || t == "strike" || t == "del") s += "<strikethrough>" + inner + "</strikethrough>";
        else if (t == "sub") s += "<sub>" + inner + "</sub>";
        else if (t == "sup") s += "<sup>" + inner + "</sup>";
        else if (t == "code" || t == "tt") s += "<code>" + inner + "</code>";
        else if (t == "u") s += inner;
        else if (t == "a") {
            std::string href = trim(ch->attr("href"));
            if (!href.empty() && lowerAscii(href).compare(0, 11, "javascript:") != 0)
                s += "<a l:href=\"" + escXml(href) + "\">" + (inner.empty() ? escXml(href) : inner) + "</a>";
            else
                s += inner;
        } else if (t == "img") {
            std::string src = ch->attr("src");
            std::string name = pathBase(src);
            if (!name.empty()) {
                used.insert(name);
                s += "<image l:href=\"#" + escXml(name) + "\"/>";
            }
        } else if (t == "br") {
            s += " ";
        } else {
            s += inner;
        }
    }
    return s;
}

static std::string fragToFB2(const std::string& html, std::set<std::string>& used) {
    if (html.empty()) return std::string();
    MNode frag = parseMarkup(html, true);
    return inlineToFB2(&frag, used);
}

static std::string htmlTableToFB2(const std::string& html, std::set<std::string>& used) {
    MNode doc = parseMarkup(html, true);
    std::vector<const MNode*> tables = findNodes(&doc, "table");
    if (tables.empty()) return std::string();
    std::string out = "<table>";
    std::vector<const MNode*> rows = findNodes(tables[0], "tr");
    for (size_t r = 0; r < rows.size(); r++) {
        out += "<tr>";
        for (size_t c = 0; c < rows[r]->kids.size(); c++) {
            const MNode* cell = rows[r]->kids[c].get();
            if (cell->text) continue;
            if (cell->name != "td" && cell->name != "th") continue;
            std::string tag = cell->name == "th" ? "th" : "td";
            out += "<" + tag;
            std::string cs = cell->attr("colspan");
            std::string rs = cell->attr("rowspan");
            int csi = atoi(cs.c_str());
            int rsi = atoi(rs.c_str());
            if (csi > 1) out += " colspan=\"" + intToStr(csi) + "\"";
            if (rsi > 1) out += " rowspan=\"" + intToStr(rsi) + "\"";
            out += ">";
            out += inlineToFB2(cell, used);
            out += "</" + tag + ">";
        }
        out += "</tr>";
    }
    return out + "</table>";
}

/* ============================ FB2 ====================================== */

/* «2024-05-01» -> «2024» для тега <year> */
static std::string yearOnly(const std::string& s) {
    std::string d;
    for (size_t i = 0; i < s.size() && d.size() < 4; i++)
        if (s[i] >= '0' && s[i] <= '9') d += s[i];
    return d.size() == 4 ? d : s;
}

static std::string fb2AuthorXml(const Author& a, const std::string& ind) {    std::vector<std::string> parts;
    if (!a.first.empty()) parts.push_back(ind + "  <first-name>" + escXml(a.first) + "</first-name>");
    if (!a.middle.empty()) parts.push_back(ind + "  <middle-name>" + escXml(a.middle) + "</middle-name>");
    if (!a.last.empty()) parts.push_back(ind + "  <last-name>" + escXml(a.last) + "</last-name>");
    if (parts.empty() && !a.nick.empty())
        parts.push_back(ind + "  <nickname>" + escXml(a.nick) + "</nickname>");
    if (parts.empty()) return std::string();
    std::string out = ind + "<author>\n";
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += "\n";
        out += parts[i];
    }
    out += "\n" + ind + "</author>";
    return out;
}

static void fb2BlocksOut(const std::vector<Block>& blocks, std::vector<std::string>& out,
                         const std::string& ind, std::set<std::string>& used) {
    for (size_t i = 0; i < blocks.size(); i++) {
        const Block& b = blocks[i];
        switch (b.type) {
            case Block::P: {
                std::string h = fragToFB2(b.html, used);
                if (!trim(h).empty())
                    out.push_back(ind + "<p" + (b.id.empty() ? "" : " id=\"" + escXml(b.id) + "\"") +
                                  ">" + h + "</p>");
                break;
            }
            case Block::SUB:
                out.push_back(ind + "<subtitle>" + fragToFB2(b.html, used) + "</subtitle>");
                break;
            case Block::EMPTY:
                if (!out.empty() && out[out.size() - 1].find("<empty-line/>") == std::string::npos)
                    out.push_back(ind + "<empty-line/>");
                break;
            case Block::IMG: {
                std::string name = pathBase(b.src);
                if (!name.empty()) {
                    used.insert(name);
                    out.push_back(ind + "<image l:href=\"#" + escXml(name) + "\"/>");
                }
                break;
            }
            case Block::QUOTE: {
                const char* tag = (b.cls == "epigraph") ? "epigraph" : "cite";
                out.push_back(ind + "<" + tag + ">");
                fb2BlocksOut(b.blocks, out, ind + "  ", used);
                out.push_back(ind + "</" + tag + ">");
                break;
            }
            case Block::POEM: {
                out.push_back(ind + "<poem>");
                size_t start = 0;
                if (!b.blocks.empty() && b.blocks[0].type == Block::SUB) {
                    out.push_back(ind + "  <title><p>" + fragToFB2(b.blocks[0].html, used) + "</p></title>");
                    start = 1;
                }
                std::vector<Block> rest(b.blocks.begin() + start, b.blocks.end());
                fb2BlocksOut(rest, out, ind + "  ", used);
                out.push_back(ind + "</poem>");
                break;
            }
            case Block::STANZA:
                out.push_back(ind + "<stanza>");
                fb2BlocksOut(b.blocks, out, ind + "  ", used);
                out.push_back(ind + "</stanza>");
                break;
            case Block::V:
                out.push_back(ind + "<v>" + fragToFB2(b.html, used) + "</v>");
                break;
            case Block::AUTHOR:
                out.push_back(ind + "<text-author>" + fragToFB2(b.html, used) + "</text-author>");
                break;
            case Block::LI:
                out.push_back(ind + "<p>• " + fragToFB2(b.html, used) + "</p>");
                break;
            case Block::PRE:
                out.push_back(ind + "<code>" + escXml(unescapeEntities(b.html)) + "</code>");
                break;
            case Block::RAW: {
                std::string t = htmlTableToFB2(b.html, used);
                if (!t.empty()) out.push_back(ind + t);
                break;
            }
            default:
                break;
        }
    }
}

static bool writeFB2(Book& book, Bytes& out, std::string* err) {
    normalizeBook(book);
    const Meta& m = book.meta;
    std::set<std::string> used;
    std::vector<std::string> lines;

    std::string series;
    if (!m.seriesName.empty()) {
        series = "<sequence name=\"" + escXml(m.seriesName) + "\"" +
                 (m.seriesNum.empty() ? "" : " number=\"" + escXml(m.seriesNum) + "\"") + "/>";
    }

    std::string genres;
    if (m.genres.empty()) {
        genres = "    <genre>prose</genre>";
    } else {
        for (size_t i = 0; i < m.genres.size(); i++) {
            if (i) genres += "\n";
            genres += "    <genre>" + escXml(m.genres[i]) + "</genre>";
        }
    }

    std::string annotationHtml;
    if (!m.annotation.empty()) {
        std::vector<std::string> ps;
        MNode div = parseMarkup(m.annotation, true);
        /* если в аннотации есть блочные элементы — по одному <p> на элемент */
        bool hasBlocks = false;
        for (size_t i = 0; i < div.kids.size(); i++)
            if (!div.kids[i]->text && (div.kids[i]->name == "p" || div.kids[i]->name == "div"))
                hasBlocks = true;
        if (hasBlocks) {
            for (size_t i = 0; i < div.kids.size(); i++) {
                const MNode* ch = div.kids[i].get();
                if (ch->text) {
                    std::string h = fragToFB2(escXml(ch->value), used);
                    if (!trim(h).empty()) ps.push_back("      <p>" + h + "</p>");
                } else {
                    std::string h = inlineToFB2(ch, used);
                    if (!trim(h).empty()) ps.push_back("      <p>" + h + "</p>");
                }
            }
        } else {
            std::string h = inlineToFB2(&div, used);
            if (!trim(h).empty()) ps.push_back("      <p>" + h + "</p>");
        }
        if (!ps.empty()) {
            annotationHtml = "    <annotation>\n";
            for (size_t i = 0; i < ps.size(); i++) {
                if (i) annotationHtml += "\n";
                annotationHtml += ps[i];
            }
            annotationHtml += "\n    </annotation>";
        }
    }

    std::string coverXml;
    if (!book.coverName.empty() && book.images.find(book.coverName) != book.images.end()) {
        coverXml = "    <coverpage>\n      <image l:href=\"#" + escXml(book.coverName) + "\"/>\n    </coverpage>";
        used.insert(book.coverName);
    }

    lines.push_back("<?xml version=\"1.0\" encoding=\"utf-8\"?>");
    lines.push_back("<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\" xmlns:l=\"http://www.w3.org/1999/xlink\">");
    lines.push_back("  <description>");
    lines.push_back("    <title-info>");
    lines.push_back(genres);
    for (size_t i = 0; i < m.authors.size(); i++) {
        std::string a = fb2AuthorXml(m.authors[i], "    ");
        if (!a.empty()) lines.push_back(a);
    }
    lines.push_back("    <book-title>" + escXml(m.title) + "</book-title>");
    if (!annotationHtml.empty()) lines.push_back(annotationHtml);
    if (!m.keywords.empty()) lines.push_back("    <keywords>" + escXml(m.keywords) + "</keywords>");
    if (!m.year.empty())
        lines.push_back("    <date value=\"" + escXml(m.year) + "\">" + escXml(yearOnly(m.year)) +
                        "</date>");
    if (!coverXml.empty()) lines.push_back(coverXml);
    lines.push_back("    <lang>" + escXml(m.lang.empty() ? "ru" : m.lang) + "</lang>");
    if (!m.srcLang.empty()) lines.push_back("    <src-lang>" + escXml(m.srcLang) + "</src-lang>");
    for (size_t i = 0; i < m.translators.size(); i++) {
        std::string x = fb2AuthorXml(m.translators[i], "    ");
        if (x.empty()) continue;
        x = replaceAll(x, "<author>", "<translator>");
        x = replaceAll(x, "</author>", "</translator>");
        lines.push_back(x);
    }
    if (!series.empty()) lines.push_back("    " + series);
    lines.push_back("    </title-info>");
    lines.push_back("    <document-info>");
    lines.push_back("      <author>\n        <nickname>fb2-converter</nickname>\n      </author>");
    lines.push_back("      <program-used>" + escXml(m.program) + "</program-used>");
    lines.push_back("      <date value=\"" + isoDateNow() + "\">" + isoDateNow() + "</date>");
    lines.push_back("      <id>" + uuidStr() + "</id>");
    lines.push_back("      <version>1.0</version>");
    lines.push_back("    </document-info>");
    std::vector<std::string> pub;
    if (!m.publisher.empty()) pub.push_back("      <publisher>" + escXml(m.publisher) + "</publisher>");
    if (!m.city.empty()) pub.push_back("      <city>" + escXml(m.city) + "</city>");
    if (!m.year.empty()) pub.push_back("      <year>" + escXml(yearOnly(m.year)) + "</year>");
    if (!m.isbn.empty()) pub.push_back("      <isbn>" + escXml(m.isbn) + "</isbn>");
    if (!pub.empty()) {
        lines.push_back("    <publish-info>");
        for (size_t i = 0; i < pub.size(); i++) lines.push_back(pub[i]);
        lines.push_back("    </publish-info>");
    }
    lines.push_back("  </description>");

    /* тело */
    std::vector<ChapterNode> tree = buildTree(book.chapters);
    std::vector<std::string> bodyOut;

    struct Emitter {
        std::vector<std::string>& out;
        std::set<std::string>& used;
        void emit(const ChapterNode& node, const std::string& ind) {
            bool hasContent = !node.title.empty();
            if (node.blocks) {
                for (size_t i = 0; i < node.blocks->size() && !hasContent; i++)
                    if ((*node.blocks)[i].type != Block::EMPTY) hasContent = true;
            }
            if (!node.children.empty()) hasContent = true;
            if (!hasContent) return;
            out.push_back(ind + "<section>");
            if (!node.title.empty())
                out.push_back(ind + "  <title><p>" + fragToFB2(node.title, used) + "</p></title>");
            if (node.blocks) fb2BlocksOut(*node.blocks, out, ind + "  ", used);
            for (size_t i = 0; i < node.children.size(); i++) emit(node.children[i], ind + "  ");
            out.push_back(ind + "</section>");
        }
    };
    Emitter em = {bodyOut, used};

    if (!tree.empty()) {
        for (size_t i = 0; i < tree.size(); i++) em.emit(tree[i], "  ");
    } else {
        std::vector<Block> blocks;
        for (size_t i = 0; i < book.chapters.size(); i++) {
            if (!book.chapters[i].title.empty()) blocks.push_back(Block::sub(book.chapters[i].title));
            for (size_t k = 0; k < book.chapters[i].blocks.size(); k++)
                blocks.push_back(book.chapters[i].blocks[k]);
        }
        bodyOut.push_back("  <section>");
        fb2BlocksOut(blocks, bodyOut, "    ", used);
        bodyOut.push_back("  </section>");
    }
    /* ссылки на сноски помечаем type="note": по нему читалки показывают сноску во всплывающем окне */
    if (!book.notes.empty()) {
        std::set<std::string> noteIds;
        for (size_t i = 0; i < book.notes.size(); i++)
            if (!book.notes[i].id.empty()) noteIds.insert(book.notes[i].id);
        for (size_t i = 0; i < bodyOut.size(); i++) {
            std::string& s = bodyOut[i];
            size_t pos = 0;
            while ((pos = s.find("<a l:href=\"#", pos)) != std::string::npos) {
                size_t q = s.find('"', pos + 12);
                if (q == std::string::npos) break;
                if (noteIds.count(s.substr(pos + 12, q - pos - 12))) s.insert(q + 1, " type=\"note\"");
                pos = q;
            }
        }
    }
    lines.push_back("  <body>");
    for (size_t i = 0; i < bodyOut.size(); i++) lines.push_back(bodyOut[i]);
    lines.push_back("  </body>");

    if (!book.notes.empty()) {
        lines.push_back("  <body name=\"notes\">");
        for (size_t i = 0; i < book.notes.size(); i++) {
            const Note& n = book.notes[i];
            lines.push_back("    <section" + (n.id.empty() ? std::string() : " id=\"" + escXml(n.id) + "\"") + ">");
            if (!n.title.empty())
                lines.push_back("      <title><p>" + fragToFB2(n.title, used) + "</p></title>");
            fb2BlocksOut(n.blocks, lines, "      ", used);
            lines.push_back("    </section>");
        }
        lines.push_back("  </body>");
    }

    for (std::map<std::string, Image>::iterator it = book.images.begin(); it != book.images.end(); ++it) {
        if (used.find(it->first) == used.end()) continue;
        lines.push_back("  <binary id=\"" + escXml(it->first) + "\" content-type=\"" +
                        escXml(it->second.mime) + "\">");
        std::string b64 = b64encode(it->second.bytes);
        for (size_t i = 0; i < b64.size(); i += 76)
            lines.push_back(b64.substr(i, 76));
        lines.push_back("  </binary>");
    }
    lines.push_back("</FictionBook>");

    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += "\n";
        text += lines[i];
    }
    out.assign(text.begin(), text.end());
    (void)err;
    return true;
}

/* ============================== EPUB =================================== */

static std::string blocksToHtml(const std::vector<Block>& blocks, bool epub) {
    std::string s;
    for (size_t i = 0; i < blocks.size(); i++) {
        const Block& b = blocks[i];
        switch (b.type) {
            case Block::P:
                s += "<p" + (b.id.empty() ? std::string() : " id=\"" + escHtml(b.id) + "\"") + ">" + b.html + "</p>";
                break;
            case Block::SUB:
                s += "<p class=\"subtitle\">" + b.html + "</p>";
                break;
            case Block::EMPTY:
                s += "<p class=\"empty\">&#160;</p>";
                break;
            case Block::IMG: {
                std::string src = b.src;
                if (epub && startsWith(src, "images/")) src = "../" + src;
                s += "<div class=\"picture\"><img src=\"" + escHtml(src) + "\" alt=\"\"/></div>";
                break;
            }
            case Block::QUOTE:
                s += "<blockquote class=\"" + escHtml(b.cls.empty() ? "cite" : b.cls) + "\">" +
                     blocksToHtml(b.blocks, epub) + "</blockquote>";
                break;
            case Block::POEM:
                s += "<div class=\"poem\">" + blocksToHtml(b.blocks, epub) + "</div>";
                break;
            case Block::STANZA:
                s += "<div class=\"stanza\">" + blocksToHtml(b.blocks, epub) + "</div>";
                break;
            case Block::V:
                s += "<p class=\"v\">" + b.html + "</p>";
                break;
            case Block::AUTHOR:
                s += "<p class=\"author\">" + b.html + "</p>";
                break;
            case Block::LI:
                s += "<p class=\"li\">• " + b.html + "</p>";
                break;
            case Block::PRE:
                s += "<pre>" + b.html + "</pre>";
                break;
            case Block::RAW: {
                std::string t = b.html;
                if (epub) {
                    /* images/ -> ../images/ */
                    std::string low = lowerAscii(t);
                    std::string res;
                    size_t pos = 0;
                    for (;;) {
                        size_t p = low.find("src=\"images/", pos);
                        if (p == std::string::npos) { res.append(t, pos, std::string::npos); break; }
                        res.append(t, pos, p - pos);
                        res += "src=\"../images/";
                        pos = p + 12;
                    }
                    t = res;
                }
                s += t;
                break;
            }
            default:
                if (!b.html.empty()) s += "<p>" + b.html + "</p>";
                break;
        }
    }
    return s;
}

static const char* kEpubCss =
    "body{font-family:Georgia,\"Times New Roman\",serif;line-height:1.45;margin:0 4%;text-align:justify;hyphens:auto}\n"
    "h1,h2,h3,h4{text-align:center;text-indent:0;line-height:1.25;margin:1.4em 0 .9em;font-weight:bold;page-break-after:avoid}\n"
    "h1{font-size:1.5em}h2{font-size:1.3em}h3{font-size:1.15em}h4{font-size:1.05em}\n"
    "p{text-indent:1.15em;margin:0 0 .3em}\n"
    "p.subtitle{text-align:center;text-indent:0;font-weight:bold;margin:1em 0}\n"
    "p.empty{margin:0;height:.8em}\n"
    "p.author{text-align:right;text-indent:0;font-style:italic}\n"
    "p.v{margin:0 0 .2em;text-indent:1.5em}\n"
    "p.li{margin:0 0 .25em 1em;text-indent:-1em}\n"
    "blockquote{margin:.8em 1.6em;font-style:italic}\n"
    "blockquote.epigraph{font-size:.95em;margin:.8em 2em}\n"
    ".poem{margin:.8em 0 .8em 1.6em}\n"
    ".stanza{margin:0 0 .7em}\n"
    "div.picture{text-align:center;margin:1em 0}\n"
    "div.picture img{max-width:100%}\n"
    "pre{white-space:pre-wrap;font-family:\"Courier New\",monospace;font-size:.9em}\n"
    "table{border-collapse:collapse;margin:1em 0;width:100%}\n"
    "td,th{border:1px solid #999;padding:.25em .4em;vertical-align:top}\n"
    "a{color:inherit;text-decoration:none}\n"
    ".titlepage{text-align:center;margin-top:18%}\n"
    ".titlepage .t{font-size:1.9em;font-weight:bold;line-height:1.25}\n"
    ".titlepage .a{font-size:1.2em;margin-top:1.4em}\n"
    ".titlepage .s{font-size:1em;color:#555;margin-top:.6em}\n"
    ".cover{text-align:center;margin:0;padding:0}\n"
    ".cover img{max-width:100%;max-height:100%}\n"
    ".notes p{text-indent:0;margin:.4em 0;font-size:.95em}\n";

static std::string xhtmlHead(const std::string& title, const std::string& lang) {
    std::string t = escHtml(collapse(htmlToText(title)));
    if (t.empty()) t = "Книга";
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n"
           "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"" + escXml(lang) +
           "\" lang=\"" + escXml(lang) + "\">\n<head>\n<meta charset=\"utf-8\"/>\n<title>" + t +
           "</title>\n<link rel=\"stylesheet\" type=\"text/css\" href=\"../styles.css\"/>\n</head>\n<body>\n";
}

static bool writeEPUB(Book& book, Bytes& outBytes, std::string* err) {
    normalizeBook(book);
    const Meta& m = book.meta;
    std::string lang = m.lang.empty() ? "ru" : m.lang;
    std::string al = authorsLine(book);

    std::set<std::string> noteIds;
    for (size_t i = 0; i < book.notes.size(); i++)
        if (!book.notes[i].id.empty()) noteIds.insert(book.notes[i].id);

    std::vector<ZipEntry> entries;

    struct ChapFile { std::string file, title; int level; bool notes; };
    std::vector<ChapFile> chapterFiles;

    /* главы */
    int idx = 0;
    for (size_t i = 0; i < book.chapters.size(); i++) {
        const Chapter& ch = book.chapters[i];
        bool has = !ch.title.empty();
        for (size_t k = 0; k < ch.blocks.size() && !has; k++)
            if (ch.blocks[k].type != Block::EMPTY) has = true;
        if (!has) continue;
        idx++;
        std::string fname = "text/ch" + padNum(idx, 3) + ".xhtml";
        int lvl = std::min(4, ch.level + 1);
        std::string body;
        if (!ch.title.empty()) {
            body += "<h" + intToStr(lvl) + ">" + ch.title + "</h" + intToStr(lvl) + ">\n";
        }
        body += blocksToHtml(ch.blocks, true);
        if (!noteIds.empty()) {
            /* ссылки на примечания ведут в notes.xhtml */
            std::string res;
            size_t pos = 0;
            for (;;) {
                size_t p = body.find("href=\"#", pos);
                if (p == std::string::npos) { res.append(body, pos, std::string::npos); break; }
                res.append(body, pos, p - pos);
                size_t q = body.find('"', p + 7);
                if (q == std::string::npos) { res.append(body, p, std::string::npos); break; }
                std::string id = body.substr(p + 7, q - p - 7);
                if (noteIds.find(id) != noteIds.end()) res += "href=\"notes.xhtml#" + id + "\"";
                else res.append(body, p, q - p + 1);
                pos = q + 1;
            }
            body = res;
        }
        std::string xhtml = xhtmlHead(ch.title, lang) + "<div class=\"chapter\">\n" + body +
                            "\n</div>\n</body>\n</html>\n";
        Bytes b(xhtml.begin(), xhtml.end());
        entries.push_back(ZipEntry("OEBPS/" + fname, b, false));
        ChapFile cf;
        cf.file = fname;
        cf.title = ch.title;
        cf.level = ch.level;
        cf.notes = false;
        chapterFiles.push_back(cf);
    }

    /* примечания */
    if (!book.notes.empty()) {
        std::string body = "<h1>Примечания</h1>\n<div class=\"notes\">\n";
        for (size_t i = 0; i < book.notes.size(); i++) {
            const Note& n = book.notes[i];
            body += "<p" + (n.id.empty() ? std::string() : " id=\"" + escHtml(n.id) + "\"") + ">";
            if (!n.title.empty()) body += "<strong>" + n.title + "</strong> ";
            std::string inner = blocksToHtml(n.blocks, true);
            inner = replaceAll(inner, "</p>\n<p>", " ");
            inner = replaceAll(inner, "</p><p>", " ");
            if (startsWith(inner, "<p>")) inner = inner.substr(3);
            if (endsWithNoCase(inner, "</p>")) inner = inner.substr(0, inner.size() - 4);
            body += inner;
            body += "</p>\n";
        }
        body += "</div>\n";
        std::string xhtml = xhtmlHead("Примечания", lang) + body + "</body>\n</html>\n";
        Bytes b(xhtml.begin(), xhtml.end());
        entries.push_back(ZipEntry("OEBPS/text/notes.xhtml", b, false));
        ChapFile cf;
        cf.file = "text/notes.xhtml";
        cf.title = "Примечания";
        cf.level = 0;
        cf.notes = true;
        chapterFiles.push_back(cf);
    }

    /* титул */
    std::string tp = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n"
                     "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"" + escXml(lang) + "\">\n"
                     "<head><meta charset=\"utf-8\"/><title>" + escHtml(m.title) + "</title>\n"
                     "<link rel=\"stylesheet\" type=\"text/css\" href=\"styles.css\"/></head>\n<body>\n"
                     "<div class=\"titlepage\">\n<div class=\"t\">" + escHtml(m.title) + "</div>\n";
    if (!al.empty()) tp += "<div class=\"a\">" + escHtml(al) + "</div>\n";
    if (!m.seriesName.empty())
        tp += "<div class=\"s\">" + escHtml(m.seriesName + (m.seriesNum.empty() ? "" : " #" + m.seriesNum)) + "</div>\n";
    if (!m.publisher.empty())
        tp += "<div class=\"s\">" + escHtml(m.publisher + (m.city.empty() ? "" : ", " + m.city) +
                                             (m.year.empty() ? "" : ", " + m.year)) + "</div>\n";
    tp += "</div>\n</body>\n</html>";
    entries.push_back(ZipEntry("OEBPS/titlepage.xhtml", toBytes(tp), false));

    /* обложка */
    bool hasCover = !book.coverName.empty() && book.images.find(book.coverName) != book.images.end();
    if (hasCover) {
        std::string cover = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n"
                            "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"" + escXml(lang) + "\">\n"
                            "<head><meta charset=\"utf-8\"/><title>Обложка</title>\n"
                            "<link rel=\"stylesheet\" type=\"text/css\" href=\"styles.css\"/></head>\n"
                            "<body><div class=\"cover\"><img src=\"images/" + escHtml(book.coverName) +
                            "\" alt=\"Обложка\"/></div></body>\n</html>\n";
        entries.push_back(ZipEntry("OEBPS/cover.xhtml", toBytes(cover), false));
    }

    /* картинки */
    std::vector<std::string> imgIds, imgHrefs, imgTypes, imgNames;
    int im = 0;
    for (std::map<std::string, Image>::iterator it = book.images.begin(); it != book.images.end(); ++it) {
        entries.push_back(ZipEntry("OEBPS/images/" + it->first, it->second.bytes, false));
        im++;
        imgIds.push_back("img" + intToStr(im));
        imgHrefs.push_back("images/" + it->first);
        imgTypes.push_back(it->second.mime);
        imgNames.push_back(it->first);
    }

    entries.push_back(ZipEntry("OEBPS/styles.css", toBytes(std::string(kEpubCss)), false));

    /* пункты оглавления с уровнями без скачков (0, 1, 2 …), чтобы вложенность была валидной */
    std::vector<ChapFile> toc;
    int maxLvl = 0;
    for (size_t i = 0; i < chapterFiles.size(); i++) {
        if (chapterFiles[i].title.empty()) continue;
        ChapFile t = chapterFiles[i];
        int prev = toc.empty() ? -1 : toc.back().level;
        if (t.level < 0) t.level = 0;
        if (t.level > prev + 1) t.level = prev + 1;
        if (t.level > maxLvl) maxLvl = t.level;
        toc.push_back(t);
    }

    /* nav.xhtml */
    std::string nav = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n"
                      "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" xml:lang=\"" +
                      escXml(lang) + "\">\n<head><meta charset=\"utf-8\"/><title>Оглавление</title>\n"
                      "<link rel=\"stylesheet\" type=\"text/css\" href=\"styles.css\"/></head>\n<body>\n"
                      "<nav epub:type=\"toc\" id=\"toc\"><h1>Оглавление</h1><ol>\n";
    for (size_t i = 0; i < toc.size(); i++) {
        if (i > 0) {
            int prev = toc[i - 1].level;
            if (toc[i].level > prev) {
                nav += "\n<ol>\n";
            } else {
                nav += "</li>\n";
                for (int d = prev; d > toc[i].level; d--) nav += "</ol></li>\n";
            }
        }
        nav += "<li><a href=\"" + escHtml(toc[i].file) + "\">" + toc[i].title + "</a>";
    }
    if (!toc.empty()) {
        nav += "</li>\n";
        for (int d = toc.back().level; d > 0; d--) nav += "</ol></li>\n";
    }
    nav += "</ol></nav>\n<nav epub:type=\"landmarks\" hidden=\"hidden\"><ol>\n";
    nav += "<li><a epub:type=\"bodymatter\" href=\"" +
           escHtml(chapterFiles.empty() ? std::string("titlepage.xhtml") : chapterFiles[0].file) +
           "\">Начало</a></li>\n";
    nav += "</ol></nav>\n</body></html>";
    entries.push_back(ZipEntry("OEBPS/nav.xhtml", toBytes(nav), false));

    /* toc.ncx */
    std::string ncx = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                      "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n<head>\n"
                      "<meta name=\"dtb:uid\" content=\"" + uuidStr() + "\"/>\n"
                      "<meta name=\"dtb:depth\" content=\"" + intToStr(maxLvl + 1) + "\"/>\n"
                      "<meta name=\"dtb:totalPageCount\" content=\"0\"/>\n"
                      "<meta name=\"dtb:maxPageNumber\" content=\"0\"/>\n</head>\n"
                      "<docTitle><text>" + escHtml(m.title) + "</text></docTitle>\n"
                      "<docAuthor><text>" + escHtml(al) + "</text></docAuthor>\n<navMap>\n";
    for (size_t i = 0; i < toc.size(); i++) {
        if (i > 0) {
            int prev = toc[i - 1].level;
            if (toc[i].level <= prev)
                for (int d = prev; d >= toc[i].level; d--) ncx += "</navPoint>\n";
            else
                ncx += "\n";
        }
        std::string order = intToStr((long long)i + 1);
        ncx += "<navPoint id=\"np" + order + "\" playOrder=\"" + order + "\">" +
               "<navLabel><text>" + escHtml(htmlToText(toc[i].title)) + "</text></navLabel>" +
               "<content src=\"" + escHtml(toc[i].file) + "\"/>";
    }
    if (!toc.empty())
        for (int d = toc.back().level; d >= 0; d--) ncx += "</navPoint>\n";
    ncx += "</navMap></ncx>";
    entries.push_back(ZipEntry("OEBPS/toc.ncx", toBytes(ncx), false));

    /* content.opf */
    std::string uid = "urn:uuid:" + uuidStr();
    std::string opf = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\" xml:lang=\"" +
                      escXml(lang) + "\">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                      "<dc:identifier id=\"bookid\">" + escXml(uid) + "</dc:identifier>\n"
                      "<dc:title>" + escHtml(m.title) + "</dc:title>\n";
    if (!al.empty()) opf += "<dc:creator>" + escHtml(al) + "</dc:creator>\n";
    opf += "<dc:language>" + escXml(lang) + "</dc:language>\n";
    if (!m.publisher.empty()) opf += "<dc:publisher>" + escHtml(m.publisher) + "</dc:publisher>\n";
    if (m.year.size() >= 4 && isdigit((unsigned char)m.year[0]))
        opf += "<dc:date>" + escXml(m.year.substr(0, 10)) + "</dc:date>\n";
    for (size_t i = 0; i < m.genres.size(); i++)
        opf += "<dc:subject>" + escHtml(m.genres[i]) + "</dc:subject>\n";
    if (!m.annotation.empty())
        opf += "<dc:description>" + escHtml(htmlToText(m.annotation)) + "</dc:description>\n";
    if (!m.isbn.empty()) opf += "<dc:source>" + escHtml(m.isbn) + "</dc:source>\n";
    if (!m.seriesName.empty()) {
        opf += "<meta property=\"belongs-to-collection\" id=\"col\">" + escHtml(m.seriesName) + "</meta>\n";
        opf += "<meta refines=\"#col\" property=\"collection-type\">series</meta>\n";
        if (!m.seriesNum.empty())
            opf += "<meta refines=\"#col\" property=\"group-position\">" + escHtml(m.seriesNum) + "</meta>\n";
        /* EPUB 2-читалки (FBReader, AlReader, старые Sony/PocketBook) понимают только так */
        opf += "<meta name=\"calibre:series\" content=\"" + escHtml(m.seriesName) + "\"/>\n";
        if (!m.seriesNum.empty())
            opf += "<meta name=\"calibre:series_index\" content=\"" + escHtml(m.seriesNum) + "\"/>\n";
    }
    opf += "<meta property=\"dcterms:modified\">" + isoDateTimeNow() + "</meta>\n";
    if (hasCover) opf += "<meta name=\"cover\" content=\"cover-image\"/>\n";
    opf += "</metadata>\n<manifest>\n"
           "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
           "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
           "<item id=\"css\" href=\"styles.css\" media-type=\"text/css\"/>\n"
           "<item id=\"titlepage\" href=\"titlepage.xhtml\" media-type=\"application/xhtml+xml\"/>\n";
    if (hasCover) opf += "<item id=\"coverpage\" href=\"cover.xhtml\" media-type=\"application/xhtml+xml\"/>\n";
    for (size_t i = 0; i < chapterFiles.size(); i++)
        opf += "<item id=\"c" + intToStr((long long)i + 1) + "\" href=\"" + escHtml(chapterFiles[i].file) +
               "\" media-type=\"application/xhtml+xml\"/>\n";
    bool coverHasId = false;
    for (size_t i = 0; i < imgNames.size(); i++) {
        bool isCov = hasCover && imgNames[i] == book.coverName;
        if (isCov) coverHasId = true;
        opf += "<item id=\"" + (isCov ? std::string("cover-image") : imgIds[i]) + "\" href=\"" +
               escHtml(imgHrefs[i]) + "\" media-type=\"" + escHtml(imgTypes[i]) + "\"" +
               (isCov ? " properties=\"cover-image\"" : "") + "/>\n";
    }
    if (!coverHasId && hasCover) { /* ничего: картинка уже добавлена */ }
    opf += "</manifest>\n<spine toc=\"ncx\">\n<itemref idref=\"titlepage\"/>\n";
    if (hasCover) opf += "<itemref idref=\"coverpage\" linear=\"no\"/>\n";
    for (size_t i = 0; i < chapterFiles.size(); i++)
        opf += "<itemref idref=\"c" + intToStr((long long)i + 1) + "\"/>\n";
    opf += "</spine>\n<guide>\n";
    if (hasCover) opf += "<reference type=\"cover\" title=\"Обложка\" href=\"cover.xhtml\"/>\n";
    opf += "<reference type=\"text\" title=\"Начало\" href=\"" +
           escHtml(chapterFiles.empty() ? std::string("titlepage.xhtml") : chapterFiles[0].file) +
           "\"/>\n</guide>\n</package>";
    entries.push_back(ZipEntry("OEBPS/content.opf", toBytes(opf), false));

    std::string cont = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                       "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
                       "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
                       "</container>";
    entries.push_back(ZipEntry("META-INF/container.xml", toBytes(cont), false));

    std::vector<ZipEntry> all;
    std::string mime = "application/epub+zip";
    all.push_back(ZipEntry("mimetype", toBytes(mime), true));
    for (size_t i = 0; i < entries.size(); i++) all.push_back(entries[i]);

    outBytes = zipWrite(all);
    if (outBytes.empty()) {
        if (err) *err = "Не удалось собрать EPUB";
        return false;
    }
    return true;
}

/* ============================== HTML =================================== */

static std::string dataUriFor(const Book& book, const std::string& name) {
    std::map<std::string, Image>::const_iterator it = book.images.find(name);
    if (it == book.images.end()) return std::string();
    return "data:" + it->second.mime + ";base64," + b64encode(it->second.bytes);
}

static std::string inlineImgs(const std::string& html, const Book& book) {
    std::string out;
    std::string low = lowerAscii(html);
    size_t pos = 0;
    for (;;) {
        size_t p = low.find("src=\"images/", pos);
        if (p == std::string::npos) { out.append(html, pos, std::string::npos); break; }
        out.append(html, pos, p - pos);
        size_t q = html.find('"', p + 12);
        if (q == std::string::npos) { out.append(html, p, std::string::npos); break; }
        std::string name = html.substr(p + 12, q - p - 12);
        std::string uri = dataUriFor(book, name);
        if (uri.empty()) {
            /* картинки нет — выкидываем тег целиком */
            size_t lt = html.rfind('<', p);
            size_t gt = html.find('>', q);
            if (lt == std::string::npos || lt < pos) lt = p;
            if (gt == std::string::npos) gt = q;
            size_t cut = p - lt;
            if (cut <= out.size()) out.erase(out.size() - cut);
            pos = gt + 1;
            continue;
        }
        out += "src=\"" + uri + "\"";
        pos = q + 1;
    }
    return out;
}

static const char* kHtmlCss =
    "body{max-width:44em;margin:0 auto;padding:2em 1.2em 4em;font:17px/1.6 Georgia,\"Times New Roman\",serif;color:#1b1b1b;background:#fbfaf7}\n"
    "h1{font-size:1.6em;text-align:center;margin:1.6em 0 1em}\n"
    "h2{font-size:1.35em;text-align:center;margin:1.5em 0 .8em}\n"
    "h3,h4{font-size:1.15em;text-align:center;margin:1.3em 0 .7em}\n"
    "p{margin:0 0 .55em;text-indent:1.4em;text-align:justify}\n"
    "p.subtitle{text-align:center;text-indent:0;font-weight:bold}\n"
    "p.author{text-align:right;text-indent:0;font-style:italic}\n"
    "p.v{margin:0 0 .15em;text-indent:1.6em}\n"
    "p.li{margin:0 0 .3em 1.2em;text-indent:-1em}\n"
    "blockquote{margin:1em 2em;font-style:italic}\n"
    ".poem{margin:1em 0 1em 2em}.stanza{margin-bottom:.8em}\n"
    ".cover{text-align:center;margin:1em 0 2em}.cover img{max-width:100%;max-height:90vh}\n"
    "div.picture{text-align:center;margin:1.2em 0}div.picture img{max-width:100%}\n"
    "table{border-collapse:collapse;margin:1em 0;width:100%}td,th{border:1px solid #bbb;padding:.3em .5em}\n"
    "pre{white-space:pre-wrap;font:14px/1.5 Consolas,monospace;background:#f0efe9;padding:.8em;border-radius:6px}\n"
    ".toc{background:#fff;border:1px solid #e3e0d6;border-radius:10px;padding:1em 1.4em;margin:0 0 2.4em}\n"
    ".toc-title{font-size:1.1em;margin:.2em 0 .6em}\n"
    ".toc ol{list-style:none;padding-left:0;margin:0}.toc li{margin:.18em 0}\n"
    ".toc .lvl1{padding-left:1.2em}.toc .lvl2{padding-left:2.4em}\n"
    ".toc a{color:#1b4fa8;text-decoration:none}.toc a:hover{text-decoration:underline}\n"
    ".meta{text-align:center;color:#666;margin:-.4em 0 2em}\n"
    "section{margin-bottom:2em}\n"
    "@media print{body{background:#fff;max-width:none}.toc{page-break-after:always}}\n";

static bool writeHTML(Book& book, Bytes& out, std::string* err) {
    normalizeBook(book);
    const Meta& m = book.meta;
    std::string al = authorsLine(book);

    struct NavItem { std::string id, title; int level; };
    std::vector<NavItem> nav;
    std::vector<std::string> parts;

    for (size_t i = 0; i < book.chapters.size(); i++) {
        const Chapter& ch = book.chapters[i];
        bool has = !ch.title.empty();
        for (size_t k = 0; k < ch.blocks.size() && !has; k++)
            if (ch.blocks[k].type != Block::EMPTY) has = true;
        if (!has) continue;
        std::string id = "ch" + intToStr((long long)nav.size() + 1);
        int lvl = std::min(4, ch.level + 1);
        parts.push_back("<section id=\"" + id + "\">");
        if (!ch.title.empty())
            parts.push_back("<h" + intToStr(lvl) + ">" + inlineImgs(ch.title, book) + "</h" + intToStr(lvl) + ">");
        parts.push_back(inlineImgs(blocksToHtml(ch.blocks, false), book));
        parts.push_back("</section>");
        if (!ch.title.empty()) {
            NavItem n;
            n.id = id;
            n.title = ch.title;
            n.level = ch.level;
            nav.push_back(n);
        }
    }
    if (!book.notes.empty()) {
        std::string id = "ch" + intToStr((long long)nav.size() + 1);
        NavItem n;
        n.id = id;
        n.title = "Примечания";
        n.level = 0;
        nav.push_back(n);
        parts.push_back("<section id=\"" + id + "\" class=\"notes\"><h1>Примечания</h1>");
        for (size_t i = 0; i < book.notes.size(); i++) {
            std::string body = blocksToHtml(book.notes[i].blocks, false);
            body = replaceAll(body, "</p>\n<p>", " ");
            body = replaceAll(body, "</p><p>", " ");
            if (startsWith(body, "<p>")) body = body.substr(3);
            if (endsWithNoCase(body, "</p>")) body = body.substr(0, body.size() - 4);
            std::string t = "<p" + (book.notes[i].id.empty() ? std::string() : " id=\"" + escHtml(book.notes[i].id) + "\"") + ">";
            if (!book.notes[i].title.empty()) t += "<strong>" + book.notes[i].title + "</strong> ";
            t += body + "</p>";
            parts.push_back(inlineImgs(t, book));
        }
        parts.push_back("</section>");
    }

    std::string toc;
    if (!nav.empty()) {
        toc = "<nav class=\"toc\"><h1 class=\"toc-title\">Оглавление</h1><ol>\n";
        for (size_t i = 0; i < nav.size(); i++) {
            toc += "<li class=\"lvl" + intToStr(nav[i].level) + "\"><a href=\"#" + nav[i].id + "\">" +
                   inlineImgs(nav[i].title, book) + "</a></li>\n";
        }
        toc += "</ol></nav>";
    }

    std::string coverHtml;
    if (!book.coverName.empty()) {
        std::string uri = dataUriFor(book, book.coverName);
        if (!uri.empty()) coverHtml = "<div class=\"cover\"><img src=\"" + uri + "\" alt=\"Обложка\"/></div>";
    }

    std::string head = "<!DOCTYPE html>\n<html lang=\"" + escHtml(m.lang.empty() ? "ru" : m.lang) + "\">\n<head>\n<meta charset=\"utf-8\">\n"
                       "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                       "<title>" + escHtml(m.title) + "</title>\n<style>" + kHtmlCss + "</style>\n</head>\n<body>\n";
    head += coverHtml;
    head += "<h1>" + escHtml(m.title) + "</h1>\n";
    if (!al.empty()) head += "<div class=\"meta\">" + escHtml(al) + "</div>\n";
    head += toc;
    for (size_t i = 0; i < parts.size(); i++) {
        head += "\n";
        head += parts[i];
    }
    head += "\n</body>\n</html>\n";

    out.assign(head.begin(), head.end());
    (void)err;
    return true;
}

/* =============================== TXT =================================== */

static std::string wrapText(const std::string& text, size_t width) {
    std::vector<std::string> words;
    std::string curWord;
    for (size_t i = 0; i <= text.size(); i++) {
        char c = i < text.size() ? text[i] : ' ';
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!curWord.empty()) { words.push_back(curWord); curWord.clear(); }
        } else {
            curWord += c;
        }
    }
    if (!curWord.empty()) words.push_back(curWord);

    std::string out;
    std::string line;
    size_t lineLen = 0;
    for (size_t i = 0; i < words.size(); i++) {
        size_t wl = utf8Len(words[i]);
        if (line.empty()) {
            line = words[i];
            lineLen = wl;
        } else if (lineLen + 1 + wl <= width) {
            line += " " + words[i];
            lineLen += 1 + wl;
        } else {
            out += line + "\n";
            line = words[i];
            lineLen = wl;
        }
    }
    if (!line.empty()) out += line;
    return out;
}

static std::string upperText(const std::string& s) {
    std::wstring w = utf8ToW(s);
    for (size_t i = 0; i < w.size(); i++) {
        if (w[i] >= L'a' && w[i] <= L'z') w[i] = (wchar_t)(w[i] - 32);
        else if (w[i] >= 0x430 && w[i] <= 0x44F) w[i] = (wchar_t)(w[i] - 32);
        else if (w[i] == 0x451) w[i] = 0x401;
    }
    return wToUtf8(w);
}

static bool writeTXT(Book& book, Bytes& out, std::string* err) {
    normalizeBook(book);
    const Meta& m = book.meta;
    std::vector<std::string> lines;
    lines.push_back(m.title);
    std::string al = authorsLine(book);
    if (!al.empty()) lines.push_back(al);
    lines.push_back("");

    struct Walker {
        std::vector<std::string>& out;
        void walk(const std::vector<Block>& blocks) {
            for (size_t i = 0; i < blocks.size(); i++) {
                const Block& b = blocks[i];
                switch (b.type) {
                    case Block::P: {
                        std::string t = htmlToText(b.html);
                        if (!t.empty()) out.push_back(wrapText(t, 78));
                        break;
                    }
                    case Block::SUB:
                        out.push_back("");
                        out.push_back(htmlToText(b.html));
                        out.push_back("");
                        break;
                    case Block::EMPTY:
                        out.push_back("");
                        break;
                    case Block::IMG:
                        out.push_back("[иллюстрация]");
                        break;
                    case Block::QUOTE:
                    case Block::POEM:
                        walk(b.blocks);
                        break;
                    case Block::STANZA:
                        out.push_back("");
                        walk(b.blocks);
                        break;
                    case Block::V:
                        out.push_back("    " + htmlToText(b.html));
                        break;
                    case Block::AUTHOR:
                        out.push_back("        " + htmlToText(b.html));
                        break;
                    case Block::LI:
                        out.push_back("  * " + wrapText(htmlToText(b.html), 74));
                        break;
                    case Block::PRE:
                        out.push_back(unescapeEntities(b.html));
                        break;
                    case Block::RAW: {
                        std::string t = htmlToText(b.html);
                        size_t pos = 0;
                        while (pos <= t.size()) {
                            size_t nl = t.find('\n', pos);
                            if (nl == std::string::npos) nl = t.size();
                            std::string L = trim(t.substr(pos, nl - pos));
                            if (!L.empty()) out.push_back(wrapText(L, 78));
                            if (nl == t.size()) break;
                            pos = nl + 1;
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    };
    Walker w = {lines};

    for (size_t i = 0; i < book.chapters.size(); i++) {
        if (!book.chapters[i].title.empty()) {
            lines.push_back("");
            lines.push_back(upperText(htmlToText(book.chapters[i].title)));
            lines.push_back("");
        }
        w.walk(book.chapters[i].blocks);
    }
    if (!book.notes.empty()) {
        lines.push_back("");
        lines.push_back("");
        lines.push_back("ПРИМЕЧАНИЯ");
        lines.push_back("");
        int n = 0;
        for (size_t i = 0; i < book.notes.size(); i++) {
            n++;
            std::vector<std::string> body;
            for (size_t k = 0; k < book.notes[i].blocks.size(); k++) {
                std::string t = htmlToText(book.notes[i].blocks[k].html);
                if (!t.empty()) body.push_back(wrapText(t, 74));
            }
            std::string line = "[" + intToStr(n) + "] ";
            for (size_t k = 0; k < body.size(); k++) {
                if (k) line += "\n    ";
                line += body[k];
            }
            lines.push_back(line);
            lines.push_back("");
        }
    }

    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += "\n";
        text += lines[i];
    }
    /* больше двух пустых строк подряд не нужно */
    text = replaceAll(text, "\n\n\n\n", "\n\n\n");
    text += "\n";

    /* UTF-8 с BOM: так «Блокнот» на XP показывает кириллицу правильно */
    out.clear();
    out.push_back(0xEF);
    out.push_back(0xBB);
    out.push_back(0xBF);
    out.insert(out.end(), text.begin(), text.end());
    (void)err;
    return true;
}

/* =============================== MOBI ================================== */

struct MobCtx {
    const Book* book;
    std::vector<std::string>* imageOrder;
    std::map<std::string, int>* imageIndex;
    std::vector<std::string>* linkOrder;
};

static const char* kMobiLinkToken = "@@@@@@@@@@"; /* ровно 10 символов, как и filepos */

static int mobiImgRec(MobCtx& c, const std::string& name) {
    std::map<std::string, int>::iterator it = c.imageIndex->find(name);
    if (it != c.imageIndex->end()) return it->second;
    c.imageOrder->push_back(name);
    int n = (int)c.imageOrder->size();
    (*c.imageIndex)[name] = n;
    return n;
}

static std::string mobiInline(const MNode* node, MobCtx& c) {
    std::string s;
    for (size_t i = 0; i < node->kids.size(); i++) {
        const MNode* ch = node->kids[i].get();
        if (ch->text) { s += escHtml(ch->value); continue; }
        const std::string& t = ch->name;
        std::string inner = mobiInline(ch, c);
        if (t == "strong" || t == "b") s += "<b>" + inner + "</b>";
        else if (t == "em" || t == "i") s += "<i>" + inner + "</i>";
        else if (t == "u") s += "<u>" + inner + "</u>";
        else if (t == "s" || t == "strike" || t == "del") s += "<strike>" + inner + "</strike>";
        else if (t == "sub") s += "<sub>" + inner + "</sub>";
        else if (t == "sup") s += "<sup>" + inner + "</sup>";
        else if (t == "code" || t == "tt") s += "<tt>" + inner + "</tt>";
        else if (t == "a") {
            std::string href = trim(ch->attr("href"));
            if (!href.empty() && href[0] == '#') {
                c.linkOrder->push_back(href.substr(1));
                s += "<a filepos=\"" + std::string(kMobiLinkToken) + "\">" +
                     (inner.empty() ? std::string("&#8226;") : inner) + "</a>";
            } else if (!href.empty()) {
                s += "<a href=\"" + escHtml(href) + "\">" + inner + "</a>";
            } else {
                s += inner;
            }
        } else if (t == "br") {
            s += "<br/>";
        } else if (t == "img") {
            std::string name = pathBase(ch->attr("src"));
            if (!name.empty() && c.book->images.find(name) != c.book->images.end())
                s += "<img recindex=\"" + padNum(mobiImgRec(c, name), 5) + "\"/>";
        } else {
            s += inner;
        }
    }
    return s;
}

static std::string mobiInlineStr(const std::string& html, MobCtx& c) {
    if (html.empty()) return std::string();
    MNode frag = parseMarkup(html, true);
    return mobiInline(&frag, c);
}

static void mobiBlocks(const std::vector<Block>& blocks, MobCtx& c, std::string& text) {
    for (size_t i = 0; i < blocks.size(); i++) {
        const Block& b = blocks[i];
        switch (b.type) {
            case Block::P:
                text += "<p>" + mobiInlineStr(b.html, c) + "</p>";
                break;
            case Block::SUB:
                text += "<p><b>" + mobiInlineStr(b.html, c) + "</b></p>";
                break;
            case Block::EMPTY:
                text += "<p>&nbsp;</p>";
                break;
            case Block::IMG: {
                std::string name = pathBase(b.src);
                if (c.book->images.find(name) != c.book->images.end())
                    text += "<div align=\"center\"><img recindex=\"" +
                            padNum(mobiImgRec(c, name), 5) + "\"/></div>";
                break;
            }
            case Block::QUOTE:
                text += "<blockquote>";
                mobiBlocks(b.blocks, c, text);
                text += "</blockquote>";
                break;
            case Block::POEM:
            case Block::STANZA:
                mobiBlocks(b.blocks, c, text);
                break;
            case Block::V:
                text += "<p>" + mobiInlineStr(b.html, c) + "</p>";
                break;
            case Block::AUTHOR:
                text += "<p align=\"right\"><i>" + mobiInlineStr(b.html, c) + "</i></p>";
                break;
            case Block::LI:
                text += "<p>&#8226; " + mobiInlineStr(b.html, c) + "</p>";
                break;
            case Block::PRE:
                text += "<pre>" + escHtml(unescapeEntities(b.html)) + "</pre>";
                break;
            case Block::RAW: {
                std::string t = b.html;
                t = replaceAll(t, "</td>", " | ");
                t = replaceAll(t, "</TD>", " | ");
                t = replaceAll(t, "</th>", " | ");
                t = replaceAll(t, "</TH>", " | ");
                t = replaceAll(t, "</tr>", "<br/>");
                t = replaceAll(t, "</TR>", "<br/>");
                text += "<p>" + mobiInlineStr(t, c) + "</p>";
                break;
            }
            default:
                break;
        }
    }
}

static void mobiPut32(Bytes& b, unsigned v) {
    b.push_back((unsigned char)(v & 0xFF));
    b.push_back((unsigned char)((v >> 8) & 0xFF));
    b.push_back((unsigned char)((v >> 16) & 0xFF));
    b.push_back((unsigned char)((v >> 24) & 0xFF));
}
static void mobiSet16(Bytes& b, size_t p, unsigned v) {
    if (p + 2 > b.size()) return;
    b[p] = (unsigned char)(v & 0xFF);
    b[p + 1] = (unsigned char)((v >> 8) & 0xFF);
}
static void mobiSet32(Bytes& b, size_t p, unsigned v) {
    if (p + 4 > b.size()) return;
    b[p] = (unsigned char)(v & 0xFF);
    b[p + 1] = (unsigned char)((v >> 8) & 0xFF);
    b[p + 2] = (unsigned char)((v >> 16) & 0xFF);
    b[p + 3] = (unsigned char)((v >> 24) & 0xFF);
}
static Bytes mobiU32Bytes(unsigned v) {
    Bytes b;
    mobiPut32(b, v);
    return b;
}
static Bytes mobiStrBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

static unsigned pdbDate() {
    /* секунды с 1904-01-01 по Гринвичу */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    unsigned long long sec = (u.QuadPart / 10000000ULL) - 11644473600ULL; /* с 1970 */
    return (unsigned)(sec + 2082844800ULL);
}

static bool writeMOBI(Book& book, Bytes& out, std::string* err) {
    normalizeBook(book);
    const Meta& m = book.meta;
    std::string al = authorsLine(book);

    MobCtx c;
    c.book = &book;
    std::vector<std::string> imageOrder;
    std::map<std::string, int> imageIndex;
    std::vector<std::string> linkOrder;
    c.imageOrder = &imageOrder;
    c.imageIndex = &imageIndex;
    c.linkOrder = &linkOrder;

    std::string text = "<html><head><title>" + escHtml(m.title) + "</title></head><body>";

    int coverRec = 0;
    if (!book.coverName.empty() && book.images.find(book.coverName) != book.images.end()) {
        coverRec = mobiImgRec(c, book.coverName);
        text += "<div align=\"center\"><img recindex=\"" + padNum(coverRec, 5) + "\"/></div>";
    }

    std::map<std::string, size_t> notePos;
    for (size_t i = 0; i < book.chapters.size(); i++) {
        const Chapter& ch = book.chapters[i];
        bool has = !ch.title.empty();
        for (size_t k = 0; k < ch.blocks.size() && !has; k++)
            if (ch.blocks[k].type != Block::EMPTY && ch.blocks[k].type != Block::IMG) has = true;
        if (!has) continue;
        if (!ch.title.empty()) {
            /* уровень главы -> h1..h4, иначе при обратном чтении вложенность теряется */
            std::string h = intToStr(std::min(4, std::max(0, ch.level) + 1));
            text += "<mbp:pagebreak/>";
            text += "<h" + h + ">" + mobiInlineStr(ch.title, c) + "</h" + h + ">";
        }
        mobiBlocks(ch.blocks, c, text);
    }

    if (!book.notes.empty()) {
        text += "<mbp:pagebreak/><h1>Примечания</h1>";
        for (size_t i = 0; i < book.notes.size(); i++) {
            const Note& n = book.notes[i];
            if (!n.id.empty()) notePos[n.id] = text.size();
            text += "<p><b>" + n.title + "</b> ";
            for (size_t k = 0; k < n.blocks.size(); k++) {
                if (n.blocks[k].html.empty()) continue;
                text += mobiInlineStr(n.blocks[k].html, c) + " ";
            }
            text += "</p>";
        }
    }
    text += "</body></html>";

    /* filepos: заглушка и номер одинаковой длины — позиции не съезжают */
    std::string finalText;
    {
        std::string token(kMobiLinkToken);
        size_t pos = 0, lk = 0;
        for (;;) {
            size_t p = text.find(token, pos);
            if (p == std::string::npos) { finalText.append(text, pos, std::string::npos); break; }
            finalText.append(text, pos, p - pos);
            std::string id = lk < linkOrder.size() ? linkOrder[lk] : std::string();
            lk++;
            unsigned long long np = 0;
            std::map<std::string, size_t>::iterator it = notePos.find(id);
            if (it != notePos.end()) np = (unsigned long long)it->second;
            std::string num = intToStr((long long)np);
            while (num.size() < 10) num = "0" + num;
            if (num.size() > 10) num = num.substr(num.size() - 10);
            finalText += num;
            pos = p + token.size();
        }
    }

    Bytes textBytes(finalText.begin(), finalText.end());
    const size_t REC = 4096;
    std::vector<Bytes> textRecords;
    for (size_t i = 0; i < textBytes.size(); i += REC)
        textRecords.push_back(Bytes(textBytes.begin() + i,
                                    textBytes.begin() + std::min(textBytes.size(), i + REC)));
    if (textRecords.empty()) textRecords.push_back(Bytes());

    std::vector<Bytes> imageBytes;
    for (size_t i = 0; i < imageOrder.size(); i++) {
        std::map<std::string, Image>::iterator it = book.images.find(imageOrder[i]);
        imageBytes.push_back(it == book.images.end() ? Bytes() : it->second.bytes);
    }

    /* EXTH */
    struct ExthRec { unsigned type; Bytes val; };
    std::vector<ExthRec> exth;
    if (!al.empty()) {
        ExthRec r; r.type = 100; r.val = mobiStrBytes(al); exth.push_back(r);
    }
    {
        ExthRec r; r.type = 503; r.val = mobiStrBytes(m.title); exth.push_back(r);
        ExthRec l; l.type = 524; l.val = mobiStrBytes(m.lang.empty() ? std::string("ru") : m.lang);
        exth.push_back(l);
        ExthRec t; t.type = 501; t.val = mobiStrBytes("EBOK"); exth.push_back(t);
    }
    if (coverRec > 0) {
        ExthRec a; a.type = 201; a.val = mobiU32Bytes((unsigned)(coverRec - 1)); exth.push_back(a);
        ExthRec b; b.type = 202; b.val = mobiU32Bytes((unsigned)(coverRec - 1)); exth.push_back(b);
    }

    unsigned exthLen = 12;
    for (size_t i = 0; i < exth.size(); i++) exthLen += 8 + (unsigned)exth[i].val.size();

    Bytes titleBytes = mobiStrBytes(m.title);
    size_t r0Size = 16 + 232 + exthLen + titleBytes.size();
    size_t padded = r0Size + ((4 - (r0Size % 4)) % 4);
    Bytes r0(padded, 0);

    /* PalmDOC-заголовок */
    mobiSet16(r0, 0, 1);                        /* без сжатия */
    mobiSet16(r0, 2, 0);
    mobiSet32(r0, 4, (unsigned)textBytes.size());
    mobiSet16(r0, 8, (unsigned)textRecords.size());
    mobiSet16(r0, 10, (unsigned)REC);
    mobiSet16(r0, 12, 0);                       /* encryption: нет */
    mobiSet16(r0, 14, 0);

    /* MOBI-заголовок */
    const size_t M = 16;
    r0[M] = 'M'; r0[M + 1] = 'O'; r0[M + 2] = 'B'; r0[M + 3] = 'I';
    mobiSet32(r0, M + 0x04, 232);
    mobiSet32(r0, M + 0x08, 2);
    mobiSet32(r0, M + 0x0C, 65001);
    mobiSet32(r0, M + 0x10, (unsigned)(GetTickCount() * 2654435761u) & 0x7FFFFFFF);
    mobiSet32(r0, M + 0x14, 6);
    {
        const size_t ffs[] = {0x18,0x1C,0x20,0x24,0x28,0x2C,0x30,0x34,0x38,0x3C};
        for (int i = 0; i < 10; i++) mobiSet32(r0, M + ffs[i], 0xFFFFFFFF);
    }
    unsigned firstNonBook = (unsigned)(1 + textRecords.size());
    mobiSet32(r0, M + 0x40, firstNonBook);
    mobiSet32(r0, M + 0x44, 16 + 232 + exthLen);
    mobiSet32(r0, M + 0x48, (unsigned)titleBytes.size());
    mobiSet32(r0, M + 0x4C, 9);
    mobiSet32(r0, M + 0x50, 9);
    mobiSet32(r0, M + 0x54, 9);
    mobiSet32(r0, M + 0x58, 6);
    mobiSet32(r0, M + 0x5C, imageBytes.empty() ? 0xFFFFFFFFu : firstNonBook);
    mobiSet32(r0, M + 0x60, 0);
    mobiSet32(r0, M + 0x64, 0);
    mobiSet32(r0, M + 0x68, 0);
    mobiSet32(r0, M + 0x6C, 0);
    mobiSet32(r0, M + 0x70, 0x50);
    mobiSet32(r0, M + 0x94, 0xFFFFFFFF);
    mobiSet32(r0, M + 0x98, 0xFFFFFFFF);
    mobiSet16(r0, M + 0xB0, 1);
    mobiSet16(r0, M + 0xB2, (unsigned)textRecords.size());
    mobiSet32(r0, M + 0xB4, 1);
    mobiSet32(r0, M + 0xB8, 0xFFFFFFFF);
    mobiSet32(r0, M + 0xC0, 0xFFFFFFFF);
    mobiSet32(r0, M + 0xC4, 0xFFFFFFFF);
    mobiSet32(r0, M + 0xCC, 0xFFFFFFFF);
    mobiSet32(r0, M + 0xD0, 0xFFFFFFFF);
    mobiSet32(r0, M + 0xD4, 0xFFFFFFFF);
    mobiSet16(r0, M + 0xDC, 0);
    mobiSet32(r0, M + 0xDE, 0xFFFFFFFF);

    /* EXTH */
    size_t P = M + 232;
    r0[P] = 'E'; r0[P + 1] = 'X'; r0[P + 2] = 'T'; r0[P + 3] = 'H';
    mobiSet32(r0, P + 4, exthLen);
    mobiSet32(r0, P + 8, (unsigned)exth.size());
    size_t q = P + 12;
    for (size_t i = 0; i < exth.size(); i++) {
        mobiSet32(r0, q, exth[i].type);
        mobiSet32(r0, q + 4, (unsigned)(8 + exth[i].val.size()));
        for (size_t k = 0; k < exth[i].val.size() && q + 8 + k < r0.size(); k++)
            r0[q + 8 + k] = exth[i].val[k];
        q += 8 + exth[i].val.size();
    }

    for (size_t i = 0; i < titleBytes.size() && P + exthLen + i < r0.size(); i++)
        r0[P + exthLen + i] = titleBytes[i];

    /* --- сборка PDB --- */
    std::vector<Bytes> records;
    records.push_back(r0);
    for (size_t i = 0; i < textRecords.size(); i++) records.push_back(textRecords[i]);
    for (size_t i = 0; i < imageBytes.size(); i++) records.push_back(imageBytes[i]);
    {
        Bytes eof;
        eof.push_back(0xE9); eof.push_back(0x8E); eof.push_back(0x0D); eof.push_back(0x0A);
        records.push_back(eof);
    }

    unsigned numRecords = (unsigned)records.size();
    size_t headerSize = 78 + (size_t)numRecords * 8 + 2;
    Bytes pdb(headerSize, 0);

    /* имя в кодировке файла: обрезаем по границе символа */
    {
        std::string up = upperText(m.title);
        std::string nm;
        for (size_t i = 0; i < up.size();) {
            unsigned char ch = (unsigned char)up[i];
            size_t len = ch < 0x80 ? 1 : ((ch & 0xE0) == 0xC0 ? 2 : ((ch & 0xF0) == 0xE0 ? 3 : 4));
            if (nm.size() + len > 31) break;
            nm.append(up, i, len);
            i += len;
        }
        for (size_t i = 0; i < nm.size(); i++) pdb[i] = (unsigned char)nm[i];
    }
    mobiSet16(pdb, 32, 0);
    mobiSet16(pdb, 34, 0);
    mobiSet32(pdb, 36, pdbDate());
    mobiSet32(pdb, 40, pdbDate());
    mobiSet32(pdb, 44, 0);
    mobiSet32(pdb, 48, 0);
    mobiSet32(pdb, 52, 0);
    mobiSet32(pdb, 56, 0);
    pdb[60] = 'B'; pdb[61] = 'O'; pdb[62] = 'O'; pdb[63] = 'K';
    pdb[64] = 'M'; pdb[65] = 'O'; pdb[66] = 'B'; pdb[67] = 'I';
    mobiSet32(pdb, 68, 0);
    mobiSet32(pdb, 72, 0);
    mobiSet16(pdb, 76, numRecords);

    size_t off = headerSize;
    for (unsigned i = 0; i < numRecords; i++) {
        mobiSet32(pdb, 78 + (size_t)i * 8, (unsigned)off);
        mobiSet32(pdb, 78 + (size_t)i * 8 + 4, (i << 24));
        off += records[i].size();
    }

    out = pdb;
    for (size_t i = 0; i < records.size(); i++)
        out.insert(out.end(), records[i].begin(), records[i].end());

    if (out.empty()) {
        if (err) *err = "Не удалось собрать MOBI";
        return false;
    }
    return true;
}

/* ============================== диспетчер ============================== */

bool writeBook(Book& book, const std::string& target, Bytes& out, std::string* err) {
    std::string t = lowerAscii(target);
    if (t == "fb2") return writeFB2(book, out, err);
    if (t == "epub") return writeEPUB(book, out, err);
    if (t == "html" || t == "htm") return writeHTML(book, out, err);
    if (t == "txt") return writeTXT(book, out, err);
    if (t == "mobi") return writeMOBI(book, out, err);
    if (err) *err = "Выходной формат не поддерживается: " + target;
    return false;
}
