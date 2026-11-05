/* Писатели DOCX и RTF (1.9). Общий разбор: блоки книги -> абзацы из кусочков текста с оформлением,
   таблицы приводятся к сетке (colspan/rowspan), ссылки на сноски становятся настоящими сносками. */
#include "convert.h"
#include "markup.h"
#include "zip.h"
#include <algorithm>

namespace {

struct Run {
    enum { TEXT, BR, NOTE, IMG };
    int kind;
    std::string text;      /* текст; для NOTE — id сноски; для IMG — имя картинки */
    std::string href;      /* внешняя ссылка */
    bool bold, italic, under, strike, sup, sub, mono;
    Run() : kind(TEXT), bold(false), italic(false), under(false), strike(false), sup(false), sub(false), mono(false) {}
};
typedef std::vector<Run> Runs;

struct Ctx {
    const Book* book;
    std::set<std::string> noteIds;
    std::set<std::string> referenced;  /* сноски, на которые есть ссылка в тексте */
    explicit Ctx(const Book& b) : book(&b) {
        for (size_t i = 0; i < b.notes.size(); i++) noteIds.insert(b.notes[i].id);
    }
};

std::string imageNameOf(const std::string& src) { return pathBase(src); }

void collectInline(const MNode* n, const Run& st, Runs& out, Ctx& cx, int depth) {
    if (depth > 60) return;
    for (size_t i = 0; i < n->kids.size(); i++) {
        const MNode* k = n->kids[i].get();
        if (k->text) {
            if (k->value.empty()) continue;
            Run r = st;
            r.kind = Run::TEXT;
            r.text = k->value;
            out.push_back(r);
            continue;
        }
        const std::string& t = k->name;
        Run s = st;
        if (t == "br") {
            Run r = st;
            r.kind = Run::BR;
            out.push_back(r);
            continue;
        }
        if (t == "img") {
            Run r;
            r.kind = Run::IMG;
            r.text = imageNameOf(k->attr("src"));
            out.push_back(r);
            continue;
        }
        if (t == "a") {
            std::string href = trim(k->attr("href"));
            if (!href.empty() && href[0] == '#') {
                std::string id = href.substr(1);
                if (cx.noteIds.count(id)) {
                    /* ссылка на сноску: её текст («[1]») заменит автонумерация Word */
                    Run r;
                    r.kind = Run::NOTE;
                    r.text = id;
                    out.push_back(r);
                    cx.referenced.insert(id);
                    continue;
                }
            } else if (startsWith(lowerAscii(href), "http") || startsWith(lowerAscii(href), "mailto:")) {
                s.href = href;
            }
        }
        if (t == "b" || t == "strong") s.bold = true;
        else if (t == "i" || t == "em") s.italic = true;
        else if (t == "u") s.under = true;
        else if (t == "s" || t == "strike" || t == "del") s.strike = true;
        else if (t == "sup") s.sup = true;
        else if (t == "sub") s.sub = true;
        else if (t == "code" || t == "tt" || t == "pre") s.mono = true;
        size_t before = out.size();
        collectInline(k, s, out, cx, depth + 1);
        /* блочные теги внутри ячейки: перенос строки между ними */
        if ((t == "p" || t == "div" || t == "li" || t == "h1" || t == "h2" || t == "h3") && out.size() > before && i + 1 < n->kids.size()) {
            Run r;
            r.kind = Run::BR;
            out.push_back(r);
        }
    }
}

Runs parseInline(const std::string& html, Ctx& cx) {
    MNode root = parseMarkup("<x>" + html + "</x>", true);
    Runs out;
    const MNode* x = findNode(&root, "x");
    collectInline(x ? x : &root, Run(), out, cx, 0);
    return out;
}

/* ------------------------------- таблицы --------------------------------- */

struct Cell {
    Runs runs;
    int cols, rows;
    bool head;
    Cell() : cols(1), rows(1), head(false) {}
};

struct Table {
    int nrows, ncols;
    std::vector<Cell> cells;
    std::vector<int> grid;     /* [строка * ncols + столбец] -> ячейка или -1 */
    Table() : nrows(0), ncols(0) {}
    int at(int r, int c) const { return grid[(size_t)r * ncols + c]; }
};

void findRows(const MNode* n, std::vector<const MNode*>& rows, int depth) {
    if (depth > 8) return;
    for (size_t i = 0; i < n->kids.size(); i++) {
        const MNode* k = n->kids[i].get();
        if (k->text) continue;
        if (k->name == "tr") rows.push_back(k);
        else if (k->name == "thead" || k->name == "tbody" || k->name == "tfoot") findRows(k, rows, depth + 1);
    }
}

void buildTable(const MNode* tbl, Ctx& cx, Table& t) {
    std::vector<const MNode*> rows;
    findRows(tbl, rows, 0);
    t.nrows = (int)rows.size();
    /* сначала размер сетки: занятые rowspan места сдвигают ячейки ниже */
    std::vector<std::vector<int> > g((size_t)t.nrows);
    for (size_t r = 0; r < rows.size(); r++) {
        size_t c = 0;
        std::vector<int>& line = g[r];
        for (size_t i = 0; i < rows[r]->kids.size(); i++) {
            const MNode* k = rows[r]->kids[i].get();
            if (k->text || (k->name != "td" && k->name != "th")) continue;
            Cell cell;
            cell.head = k->name == "th";
            cell.cols = std::min(100, std::max(1, atoi(k->attr("colspan").c_str())));
            cell.rows = std::min(1000, std::max(1, atoi(k->attr("rowspan").c_str())));
            if ((size_t)cell.rows > rows.size() - r) cell.rows = (int)(rows.size() - r);
            collectInline(k, Run(), cell.runs, cx, 0);
            int id = (int)t.cells.size();
            t.cells.push_back(cell);
            while (c < line.size() && line[c] >= 0) c++;
            for (int rr = 0; rr < cell.rows; rr++) {
                std::vector<int>& l2 = g[r + (size_t)rr];
                if (l2.size() < c + (size_t)cell.cols) l2.resize(c + (size_t)cell.cols, -1);
                for (int cc = 0; cc < cell.cols; cc++) l2[c + (size_t)cc] = id;
            }
            c += (size_t)cell.cols;
        }
    }
    for (size_t r = 0; r < g.size(); r++) t.ncols = std::max(t.ncols, (int)g[r].size());
    if (t.ncols > 200) t.ncols = 200;
    t.grid.assign((size_t)t.nrows * t.ncols, -1);
    for (size_t r = 0; r < g.size(); r++)
        for (size_t c = 0; c < g[r].size() && c < (size_t)t.ncols; c++) t.grid[r * t.ncols + c] = g[r][c];
}

/* --------------------------- размеры картинок ---------------------------- */

bool imageSize(const Bytes& b, int& w, int& h) {
    size_t n = b.size();
    if (n > 24 && b[0] == 0x89 && b[1] == 'P') {
        w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
        h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
        return w > 0 && h > 0;
    }
    if (n > 10 && b[0] == 'G' && b[1] == 'I') {
        w = b[6] | (b[7] << 8);
        h = b[8] | (b[9] << 8);
        return w > 0 && h > 0;
    }
    if (n > 26 && b[0] == 'B' && b[1] == 'M') {
        w = b[18] | (b[19] << 8) | (b[20] << 16);
        h = b[22] | (b[23] << 8) | (b[24] << 16);
        return w > 0 && h > 0;
    }
    if (n > 4 && b[0] == 0xFF && b[1] == 0xD8) {
        size_t p = 2;
        while (p + 9 < n) {
            if (b[p] != 0xFF) { p++; continue; }
            unsigned m = b[p + 1];
            if (m == 0xFF) { p++; continue; }
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                h = (b[p + 5] << 8) | b[p + 6];
                w = (b[p + 7] << 8) | b[p + 8];
                return w > 0 && h > 0;
            }
            if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7)) { p += 2; continue; }
            p += 2 + ((size_t)(b[p + 2] << 8) | b[p + 3]);
        }
    }
    return false;
}

/* вписать в страницу: не шире 6 дюймов и не выше 8.5 (при 96 точках на дюйм) */
void fitPage(int& w, int& h) {
    const double maxW = 576, maxH = 816;
    double k = std::min(1.0, std::min(maxW / w, maxH / h));
    w = std::max(1, (int)(w * k));
    h = std::max(1, (int)(h * k));
}

const Image* findImage(const Book& b, const std::string& name) {
    std::map<std::string, Image>::const_iterator it = b.images.find(name);
    return it == b.images.end() ? NULL : &it->second;
}

bool coverUsedInText(const Book& book) {
    for (size_t i = 0; i < book.chapters.size(); i++)
        for (size_t k = 0; k < book.chapters[i].blocks.size(); k++) {
            const Block& b = book.chapters[i].blocks[k];
            if (b.type == Block::IMG && imageNameOf(b.src) == book.coverName) return true;
            if (b.html.find(book.coverName) != std::string::npos) return true;
        }
    return false;
}

/* ================================= DOCX ================================== */

struct DocxPart {
    std::string rels;
    int nextRel;
    DocxPart() : nextRel(100) {}
    std::string addRel(const std::string& type, const std::string& target, bool external) {
        std::string id = "rId" + intToStr(nextRel++);
        rels += "<Relationship Id=\"" + id + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/" +
                type + "\" Target=\"" + escXml(target) + "\"" + (external ? " TargetMode=\"External\"" : "") + "/>";
        return id;
    }
};

class Docx {
public:
    Docx(Book& b) : book(b), cx(b), pictureId(1) {}
    bool write(Bytes& out);

private:
    Book& book;
    Ctx cx;
    std::map<std::string, int> noteNum;
    std::vector<std::string> noteOrder;
    std::set<std::string> usedExt;
    std::map<std::string, std::string> mediaName;
    std::vector<ZipEntry> media;
    int pictureId;
    DocxPart mainRels, noteRels;

    std::string runProps(const Run& r);
    std::string runsXml(const Runs& rs, DocxPart& part, bool inNote);
    std::string para(const std::string& style, const Runs& rs, DocxPart& part, bool inNote, const std::string& extra = std::string());
    std::string picture(const std::string& name, DocxPart& part);
    std::string blocks(const std::vector<Block>& bl, DocxPart& part, bool inNote, const std::string& style);
    std::string table(const MNode* tbl, DocxPart& part, bool inNote);
    std::string raw(const std::string& html, DocxPart& part, bool inNote);
    int noteId(const std::string& id);
    std::string styles();
};

std::string Docx::runProps(const Run& r) {
    std::string p;
    if (r.mono) p += "<w:rFonts w:ascii=\"Courier New\" w:hAnsi=\"Courier New\" w:cs=\"Courier New\"/>";
    if (r.bold) p += "<w:b/>";
    if (r.italic) p += "<w:i/>";
    if (r.strike) p += "<w:strike/>";
    if (r.under) p += "<w:u w:val=\"single\"/>";
    if (r.sup) p += "<w:vertAlign w:val=\"superscript\"/>";
    else if (r.sub) p += "<w:vertAlign w:val=\"subscript\"/>";
    return p.empty() ? p : "<w:rPr>" + p + "</w:rPr>";
}

int Docx::noteId(const std::string& id) {
    std::map<std::string, int>::iterator it = noteNum.find(id);
    if (it != noteNum.end()) return it->second;
    int n = (int)noteNum.size() + 1;
    noteNum[id] = n;
    noteOrder.push_back(id);
    return n;
}

std::string Docx::picture(const std::string& name, DocxPart& part) {
    const Image* im = findImage(book, name);
    if (!im) return std::string();
    int w = 0, h = 0;
    if (!imageSize(im->bytes, w, h)) return std::string(); /* SVG, WebP — Word их так не вставит */
    std::string ext = lowerAscii(pathExt(name));
    if (ext != "png" && ext != "jpg" && ext != "jpeg" && ext != "gif" && ext != "bmp") return std::string();
    usedExt.insert(ext);
    std::map<std::string, std::string>::iterator mi = mediaName.find(name);
    if (mi == mediaName.end()) {
        std::string zn = "image" + intToStr((long long)mediaName.size() + 1) + "." + ext;
        mediaName[name] = zn;
        media.push_back(ZipEntry("word/media/" + zn, im->bytes, true));
        mi = mediaName.find(name);
    }
    std::string rid = part.addRel("image", "media/" + mi->second, false);
    fitPage(w, h);
    long long cx0 = (long long)w * 9525, cy0 = (long long)h * 9525;
    int id = pictureId++;
    std::string sz = "cx=\"" + intToStr(cx0) + "\" cy=\"" + intToStr(cy0) + "\"";
    return "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent " + sz +
           "/><wp:docPr id=\"" + intToStr(id) + "\" name=\"Picture " + intToStr(id) +
           "\"/><a:graphic xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\">"
           "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
           "<pic:pic xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:nvPicPr><pic:cNvPr id=\"" +
           intToStr(id) + "\" name=\"" + escXml(name) + "\"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed=\"" + rid +
           "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext " + sz +
           "/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>";
}

std::string Docx::runsXml(const Runs& rs, DocxPart& part, bool inNote) {
    std::string s;
    for (size_t i = 0; i < rs.size(); i++) {
        const Run& r = rs[i];
        std::string one;
        if (r.kind == Run::TEXT) {
            one = "<w:r>" + runProps(r) + "<w:t xml:space=\"preserve\">" + escXml(r.text) + "</w:t></w:r>";
        } else if (r.kind == Run::BR) {
            one = "<w:r><w:br/></w:r>";
        } else if (r.kind == Run::IMG) {
            one = picture(r.text, part);
        } else if (r.kind == Run::NOTE) {
            if (inNote) continue; /* сноска внутри сноски в Word невозможна */
            one = "<w:r><w:rPr><w:rStyle w:val=\"FootnoteReference\"/></w:rPr><w:footnoteReference w:id=\"" +
                  intToStr(noteId(r.text)) + "\"/></w:r>";
        }
        if (!r.href.empty() && r.kind == Run::TEXT)
            one = "<w:hyperlink r:id=\"" + part.addRel("hyperlink", r.href, true) + "\">" + one + "</w:hyperlink>";
        s += one;
    }
    return s;
}

std::string Docx::para(const std::string& style, const Runs& rs, DocxPart& part, bool inNote, const std::string& extra) {
    std::string pp = (style.empty() ? std::string() : "<w:pStyle w:val=\"" + style + "\"/>") + extra;
    return "<w:p>" + (pp.empty() ? std::string() : "<w:pPr>" + pp + "</w:pPr>") + runsXml(rs, part, inNote) + "</w:p>";
}

std::string Docx::table(const MNode* tbl, DocxPart& part, bool inNote) {
    Table t;
    buildTable(tbl, cx, t);
    if (!t.nrows || !t.ncols) return std::string();
    const int total = 9000;
    int cw = total / t.ncols;
    std::string s = "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/><w:tblW w:w=\"5000\" w:type=\"pct\"/></w:tblPr><w:tblGrid>";
    for (int c = 0; c < t.ncols; c++) s += "<w:gridCol w:w=\"" + intToStr(cw) + "\"/>";
    s += "</w:tblGrid>";
    for (int r = 0; r < t.nrows; r++) {
        s += "<w:tr>";
        for (int c = 0; c < t.ncols;) {
            int id = t.at(r, c);
            if (id < 0) {
                s += "<w:tc><w:tcPr><w:tcW w:w=\"" + intToStr(cw) + "\" w:type=\"dxa\"/></w:tcPr><w:p/></w:tc>";
                c++;
                continue;
            }
            const Cell& cell = t.cells[(size_t)id];
            bool start = r == 0 || t.at(r - 1, c) != id;
            int span = 1;
            while (c + span < t.ncols && t.at(r, c + span) == id) span++;
            std::string tp = "<w:tcW w:w=\"" + intToStr(cw * span) + "\" w:type=\"dxa\"/>";
            if (span > 1) tp += "<w:gridSpan w:val=\"" + intToStr(span) + "\"/>";
            if (cell.rows > 1) tp += start ? "<w:vMerge w:val=\"restart\"/>" : "<w:vMerge/>";
            s += "<w:tc><w:tcPr>" + tp + "</w:tcPr>";
            if (start) {
                Runs rs = cell.runs;
                if (cell.head) for (size_t i = 0; i < rs.size(); i++) rs[i].bold = true;
                s += para("TableText", rs, part, inNote);
            } else {
                s += "<w:p/>";
            }
            s += "</w:tc>";
            c += span;
        }
        s += "</w:tr>";
    }
    s += "</w:tbl><w:p/>";
    return s;
}

std::string Docx::raw(const std::string& html, DocxPart& part, bool inNote) {
    MNode root = parseMarkup("<x>" + html + "</x>", true);
    const MNode* x = findNode(&root, "x");
    if (!x) return std::string();
    std::string s;
    Runs loose;
    for (size_t i = 0; i < x->kids.size(); i++) {
        const MNode* k = x->kids[i].get();
        if (!k->text && k->name == "table") {
            if (!loose.empty()) { s += para(std::string(), loose, part, inNote); loose.clear(); }
            s += table(k, part, inNote);
        } else if (!k->text) {
            MNode dummy;
            Runs tmp;
            collectInline(k, Run(), tmp, cx, 0);
            loose.insert(loose.end(), tmp.begin(), tmp.end());
        } else if (!collapse(k->value).empty()) {
            Run r;
            r.text = k->value;
            loose.push_back(r);
        }
    }
    if (!loose.empty()) s += para(std::string(), loose, part, inNote);
    return s;
}

std::string Docx::blocks(const std::vector<Block>& bl, DocxPart& part, bool inNote, const std::string& style) {
    std::string s;
    for (size_t i = 0; i < bl.size(); i++) {
        const Block& b = bl[i];
        switch (b.type) {
            case Block::SUB:
                s += para("BookSubtitle", parseInline(b.html, cx), part, inNote);
                break;
            case Block::EMPTY:
                s += "<w:p/>";
                break;
            case Block::IMG: {
                Runs rs;
                Run r;
                r.kind = Run::IMG;
                r.text = imageNameOf(b.src);
                rs.push_back(r);
                s += para("Picture", rs, part, inNote);
                break;
            }
            case Block::QUOTE:
                s += blocks(b.blocks, part, inNote, b.cls == "epigraph" ? "Epigraph" : "BookQuote");
                break;
            case Block::POEM:
                s += blocks(b.blocks, part, inNote, "Verse");
                break;
            case Block::STANZA:
                s += blocks(b.blocks, part, inNote, "Verse");
                s += "<w:p><w:pPr><w:pStyle w:val=\"Verse\"/></w:pPr></w:p>";
                break;
            case Block::V:
                s += para("Verse", parseInline(b.html, cx), part, inNote);
                break;
            case Block::AUTHOR:
                s += para("BookAuthor", parseInline(b.html, cx), part, inNote);
                break;
            case Block::LI:
                s += para("ListBullet", parseInline(b.html, cx), part, inNote);
                break;
            case Block::PRE: {
                Runs rs;
                std::string t = unescapeEntities(b.html);
                size_t pos = 0;
                while (pos <= t.size()) {
                    size_t nl = t.find('\n', pos);
                    if (nl == std::string::npos) nl = t.size();
                    Run r;
                    r.mono = true;
                    r.text = t.substr(pos, nl - pos);
                    if (!r.text.empty()) rs.push_back(r);
                    if (nl >= t.size()) break;
                    Run br;
                    br.kind = Run::BR;
                    rs.push_back(br);
                    pos = nl + 1;
                }
                s += para("Code", rs, part, inNote);
                break;
            }
            case Block::RAW:
                s += raw(b.html, part, inNote);
                break;
            default:
                if (!b.html.empty()) s += para(style, parseInline(b.html, cx), part, inNote);
                break;
        }
    }
    return s;
}

std::string Docx::styles() {
    struct S {
        static std::string p(const char* id, const char* name, const char* based, const char* ppr, const char* rpr, bool q = true) {
            std::string s = std::string("<w:style w:type=\"paragraph\" w:styleId=\"") + id + "\"><w:name w:val=\"" + name + "\"/>";
            if (based[0]) s += std::string("<w:basedOn w:val=\"") + based + "\"/>";
            s += "<w:next w:val=\"Normal\"/>";
            if (q) s += "<w:qFormat/>";
            if (ppr[0]) s += std::string("<w:pPr>") + ppr + "</w:pPr>";
            if (rpr[0]) s += std::string("<w:rPr>") + rpr + "</w:rPr>";
            return s + "</w:style>";
        }
    };
    std::string lang = book.meta.lang.empty() ? "ru" : book.meta.lang;
    std::string s =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:docDefaults><w:rPrDefault><w:rPr>"
        "<w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\" w:cs=\"Times New Roman\" w:eastAsia=\"Times New Roman\"/>"
        "<w:sz w:val=\"24\"/><w:szCs w:val=\"24\"/><w:lang w:val=\"" + escXml(lang) + "\"/></w:rPr></w:rPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/>"
        "<w:pPr><w:spacing w:after=\"60\"/><w:ind w:firstLine=\"340\"/><w:jc w:val=\"both\"/></w:pPr></w:style>"
        "<w:style w:type=\"character\" w:default=\"1\" w:styleId=\"DefaultParagraphFont\"><w:name w:val=\"Default Paragraph Font\"/></w:style>"
        "<w:style w:type=\"table\" w:default=\"1\" w:styleId=\"TableNormal\"><w:name w:val=\"Normal Table\"/><w:tblPr><w:tblCellMar>"
        "<w:left w:w=\"108\" w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/></w:tblCellMar></w:tblPr></w:style>"
        "<w:style w:type=\"table\" w:styleId=\"TableGrid\"><w:name w:val=\"Table Grid\"/><w:basedOn w:val=\"TableNormal\"/><w:tblPr><w:tblBorders>"
        "<w:top w:val=\"single\" w:sz=\"4\" w:color=\"auto\"/><w:left w:val=\"single\" w:sz=\"4\" w:color=\"auto\"/>"
        "<w:bottom w:val=\"single\" w:sz=\"4\" w:color=\"auto\"/><w:right w:val=\"single\" w:sz=\"4\" w:color=\"auto\"/>"
        "<w:insideH w:val=\"single\" w:sz=\"4\" w:color=\"auto\"/><w:insideV w:val=\"single\" w:sz=\"4\" w:color=\"auto\"/>"
        "</w:tblBorders></w:tblPr></w:style>"
        "<w:style w:type=\"character\" w:styleId=\"FootnoteReference\"><w:name w:val=\"footnote reference\"/><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr></w:style>";
    s += S::p("Title", "Title", "Normal", "<w:spacing w:before=\"240\" w:after=\"120\"/><w:ind w:firstLine=\"0\"/><w:jc w:val=\"center\"/>", "<w:b/><w:sz w:val=\"44\"/>");
    /* «heading N» — встроенные имена: по ним Word строит оглавление и навигацию на любом языке */
    const int sizes[6] = {36, 32, 28, 26, 24, 24};
    for (int i = 1; i <= 6; i++) {
        std::string id = "Heading" + intToStr(i), name = "heading " + intToStr(i);
        std::string ppr = "<w:keepNext/><w:spacing w:before=\"280\" w:after=\"120\"/><w:ind w:firstLine=\"0\"/><w:jc w:val=\"center\"/><w:outlineLvl w:val=\"" + intToStr(i - 1) + "\"/>";
        std::string rpr = "<w:b/><w:sz w:val=\"" + intToStr(sizes[i - 1]) + "\"/>";
        s += S::p(id.c_str(), name.c_str(), "Normal", ppr.c_str(), rpr.c_str());
    }
    s += S::p("BookSubtitle", "Book Subtitle", "Normal", "<w:spacing w:before=\"120\" w:after=\"120\"/><w:ind w:firstLine=\"0\"/><w:jc w:val=\"center\"/>", "<w:b/>");
    s += S::p("BookAuthorLine", "Book Author Line", "Normal", "<w:ind w:firstLine=\"0\"/><w:jc w:val=\"center\"/>", "<w:sz w:val=\"28\"/>");
    s += S::p("BookQuote", "Book Quote", "Normal", "<w:ind w:left=\"567\" w:right=\"567\" w:firstLine=\"0\"/>", "<w:i/>");
    s += S::p("Epigraph", "Epigraph", "Normal", "<w:ind w:left=\"3402\" w:firstLine=\"0\"/>", "<w:i/>");
    s += S::p("Verse", "Verse", "Normal", "<w:spacing w:after=\"0\"/><w:ind w:left=\"1134\" w:firstLine=\"0\"/><w:jc w:val=\"left\"/>", "");
    s += S::p("BookAuthor", "Book Author", "Normal", "<w:ind w:firstLine=\"0\"/><w:jc w:val=\"right\"/>", "<w:i/>");
    s += S::p("Picture", "Picture", "Normal", "<w:spacing w:before=\"120\" w:after=\"120\"/><w:ind w:firstLine=\"0\"/><w:jc w:val=\"center\"/>", "");
    s += S::p("Code", "Code", "Normal", "<w:ind w:firstLine=\"0\"/><w:jc w:val=\"left\"/>", "<w:rFonts w:ascii=\"Courier New\" w:hAnsi=\"Courier New\" w:cs=\"Courier New\"/><w:sz w:val=\"20\"/>");
    s += S::p("TableText", "Table Text", "Normal", "<w:spacing w:after=\"0\"/><w:ind w:firstLine=\"0\"/><w:jc w:val=\"left\"/>", "");
    s += S::p("ListBullet", "List Bullet", "Normal", "<w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr><w:ind w:left=\"567\" w:hanging=\"283\" w:firstLine=\"0\"/>", "");
    s += S::p("FootnoteText", "footnote text", "Normal", "<w:spacing w:after=\"0\"/><w:ind w:firstLine=\"0\"/>", "<w:sz w:val=\"20\"/>");
    return s + "</w:styles>";
}

bool Docx::write(Bytes& out) {
    normalizeBook(book);
    const Meta& m = book.meta;
    std::string body;
    /* титул: обложка, название, автор, серия, аннотация */
    if (!book.coverName.empty() && !coverUsedInText(book)) {
        Runs rs;
        Run r;
        r.kind = Run::IMG;
        r.text = book.coverName;
        rs.push_back(r);
        body += para("Picture", rs, mainRels, false);
    }
    {
        Runs rs;
        Run r;
        r.text = m.title;
        rs.push_back(r);
        body += para("Title", rs, mainRels, false);
    }
    std::string al = authorsLine(book);
    if (!al.empty()) {
        Runs rs;
        Run r;
        r.text = al;
        rs.push_back(r);
        body += para("BookAuthorLine", rs, mainRels, false);
    }
    if (!m.seriesName.empty()) {
        Runs rs;
        Run r;
        r.text = m.seriesName + (m.seriesNum.empty() ? std::string() : ", " + m.seriesNum);
        rs.push_back(r);
        body += para("BookAuthorLine", rs, mainRels, false);
    }
    if (!m.annotation.empty()) {
        std::string ann = collapse(htmlToText(m.annotation));
        if (!ann.empty()) {
            Runs rs;
            Run r;
            r.text = ann;
            r.italic = true;
            rs.push_back(r);
            body += para("BookQuote", rs, mainRels, false);
        }
    }
    /* главы */
    for (size_t i = 0; i < book.chapters.size(); i++) {
        const Chapter& ch = book.chapters[i];
        if (!ch.title.empty()) {
            int lvl = std::min(6, std::max(0, ch.level) + 1);
            body += para("Heading" + intToStr(lvl), parseInline(ch.title, cx), mainRels, false);
        }
        body += blocks(ch.blocks, mainRels, false, std::string());
    }
    /* сноски, на которые нет ссылки в тексте: иначе они пропали бы */
    std::vector<const Note*> orphan;
    for (size_t i = 0; i < book.notes.size(); i++)
        if (!cx.referenced.count(book.notes[i].id)) orphan.push_back(&book.notes[i]);
    if (!orphan.empty()) {
        Runs rs;
        Run r;
        r.text = "Примечания";
        rs.push_back(r);
        body += para("Heading1", rs, mainRels, false);
        for (size_t i = 0; i < orphan.size(); i++) {
            if (!orphan[i]->title.empty()) {
                Runs t = parseInline(orphan[i]->title, cx);
                for (size_t k = 0; k < t.size(); k++) t[k].bold = true;
                body += para(std::string(), t, mainRels, false);
            }
            body += blocks(orphan[i]->blocks, mainRels, false, std::string());
        }
    }
    /* сами сноски: номера 1.. в порядке первых ссылок */
    std::string fn =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:footnotes xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\">"
        "<w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>"
        "<w:footnote w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:r><w:continuationSeparator/></w:r></w:p></w:footnote>";
    for (size_t i = 0; i < noteOrder.size(); i++) {
        const Note* n = NULL;
        for (size_t k = 0; k < book.notes.size(); k++)
            if (book.notes[k].id == noteOrder[i]) n = &book.notes[k];
        if (!n) continue;
        std::string inner = blocks(n->blocks, noteRels, true, "FootnoteText");
        /* метка сноски — в начало первого абзаца */
        const std::string mark = "<w:r><w:rPr><w:rStyle w:val=\"FootnoteReference\"/></w:rPr><w:footnoteRef/></w:r><w:r><w:t xml:space=\"preserve\"> </w:t></w:r>";
        size_t p = inner.find("<w:p>");
        if (p == std::string::npos) {
            inner = "<w:p><w:pPr><w:pStyle w:val=\"FootnoteText\"/></w:pPr>" + mark + "</w:p>";
        } else {
            size_t after = p + 5;
            if (inner.compare(after, 7, "<w:pPr>") == 0) {
                size_t e = inner.find("</w:pPr>", after);
                after = e + 8;
            }
            inner.insert(after, mark);
        }
        fn += "<w:footnote w:id=\"" + intToStr(noteNum[noteOrder[i]]) + "\">" + inner + "</w:footnote>";
    }
    fn += "</w:footnotes>";

    std::string doc =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\"><w:body>" + body +
        "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1134\" w:right=\"1134\" w:bottom=\"1134\" w:left=\"1134\" "
        "w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>";

    std::string types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>";
    for (std::set<std::string>::iterator it = usedExt.begin(); it != usedExt.end(); ++it)
        types += "<Default Extension=\"" + *it + "\" ContentType=\"" + mimeFromExt(*it) + "\"/>";
    const std::string wml = "application/vnd.openxmlformats-officedocument.wordprocessingml.";
    types += "<Override PartName=\"/word/document.xml\" ContentType=\"" + wml + "document.main+xml\"/>"
             "<Override PartName=\"/word/styles.xml\" ContentType=\"" + wml + "styles+xml\"/>"
             "<Override PartName=\"/word/settings.xml\" ContentType=\"" + wml + "settings+xml\"/>"
             "<Override PartName=\"/word/numbering.xml\" ContentType=\"" + wml + "numbering+xml\"/>"
             "<Override PartName=\"/word/footnotes.xml\" ContentType=\"" + wml + "footnotes+xml\"/>"
             "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/></Types>";
    const std::string relHead = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    const std::string oT = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
    std::string rootRels = relHead +
        "<Relationship Id=\"rId1\" Type=\"" + oT + "officeDocument\" Target=\"word/document.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/></Relationships>";
    std::string docRels = relHead +
        "<Relationship Id=\"rId1\" Type=\"" + oT + "styles\" Target=\"styles.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"" + oT + "settings\" Target=\"settings.xml\"/>"
        "<Relationship Id=\"rId3\" Type=\"" + oT + "numbering\" Target=\"numbering.xml\"/>"
        "<Relationship Id=\"rId4\" Type=\"" + oT + "footnotes\" Target=\"footnotes.xml\"/>" + mainRels.rels + "</Relationships>";
    std::string settings =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><w:settings xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:footnotePr><w:footnote w:id=\"-1\"/><w:footnote w:id=\"0\"/></w:footnotePr>"
        "<w:compat><w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"15\"/></w:compat></w:settings>";
    std::string numbering =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:abstractNum w:abstractNumId=\"0\"><w:multiLevelType w:val=\"singleLevel\"/><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/>"
        "<w:lvlText w:val=\"\xE2\x80\xA2\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"567\" w:hanging=\"283\"/></w:pPr></w:lvl></w:abstractNum>"
        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num></w:numbering>";
    std::string now = isoDateTimeNow();
    std::string core =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><cp:coreProperties "
        "xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:dcterms=\"http://purl.org/dc/terms/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
        "<dc:title>" + escXml(m.title) + "</dc:title><dc:creator>" + escXml(al) + "</dc:creator>" +
        (m.annotation.empty() ? std::string() : "<dc:description>" + escXml(collapse(htmlToText(m.annotation))) + "</dc:description>") +
        "<dc:language>" + escXml(m.lang) + "</dc:language>"
        "<dcterms:created xsi:type=\"dcterms:W3CDTF\">" + escXml(now) + "</dcterms:created></cp:coreProperties>";

    std::vector<ZipEntry> z;
    z.push_back(ZipEntry("[Content_Types].xml", Bytes(types.begin(), types.end()), false));
    z.push_back(ZipEntry("_rels/.rels", Bytes(rootRels.begin(), rootRels.end()), false));
    z.push_back(ZipEntry("docProps/core.xml", Bytes(core.begin(), core.end()), false));
    z.push_back(ZipEntry("word/document.xml", Bytes(doc.begin(), doc.end()), false));
    z.push_back(ZipEntry("word/_rels/document.xml.rels", Bytes(docRels.begin(), docRels.end()), false));
    std::string st = styles();
    z.push_back(ZipEntry("word/styles.xml", Bytes(st.begin(), st.end()), false));
    z.push_back(ZipEntry("word/settings.xml", Bytes(settings.begin(), settings.end()), false));
    z.push_back(ZipEntry("word/numbering.xml", Bytes(numbering.begin(), numbering.end()), false));
    z.push_back(ZipEntry("word/footnotes.xml", Bytes(fn.begin(), fn.end()), false));
    if (!noteRels.rels.empty()) {
        std::string nr = relHead + noteRels.rels + "</Relationships>";
        z.push_back(ZipEntry("word/_rels/footnotes.xml.rels", Bytes(nr.begin(), nr.end()), false));
    }
    for (size_t i = 0; i < media.size(); i++) z.push_back(media[i]);
    out = zipWrite(z);
    return !out.empty();
}

/* ================================== RTF ================================== */

/* Текст: ASCII как есть, остальное \uN с заменой из cp1251 для старых программ */
std::string rtfText(const std::string& utf8) {
    std::wstring w = utf8ToW(utf8);
    std::string s;
    s.reserve(w.size() * 3);
    for (size_t i = 0; i < w.size(); i++) {
        wchar_t c = w[i];
        if (c == L'\\' || c == L'{' || c == L'}') { s += '\\'; s += (char)c; }
        else if (c == L'\n') s += "\\line ";
        else if (c == L'\t') s += "\\tab ";
        else if (c == L'\r') continue;
        else if (c < 0x80) { if (c >= 0x20) s += (char)c; }
        else {
            s += "\\u" + intToStr((long long)(short)c);
            char mb[4];
            BOOL used = FALSE;
            int n = WideCharToMultiByte(1251, 0, &c, 1, mb, sizeof(mb), NULL, &used);
            if (n == 1 && !used) {
                static const char* hex = "0123456789abcdef";
                s += "\\'";
                s += hex[((unsigned char)mb[0]) >> 4];
                s += hex[((unsigned char)mb[0]) & 15];
            } else {
                s += '?';
            }
        }
    }
    return s;
}

class Rtf {
public:
    explicit Rtf(Book& b) : book(b), cx(b) {}
    bool write(Bytes& out);

private:
    Book& book;
    Ctx cx;
    std::string runs(const Runs& rs, bool inNote);
    std::string para(const char* props, const Runs& rs, bool inNote);
    std::string picture(const std::string& name);
    std::string blocks(const std::vector<Block>& bl, bool inNote, const char* props);
    std::string table(const MNode* tbl, bool inNote);
    std::string raw(const std::string& html, bool inNote);
    std::string noteText(const std::string& id);
};

/* стили: см. таблицу в write() */
const char* const P_NORMAL = "\\s0\\fi284\\sa60\\qj ";
const char* const P_QUOTE = "\\s10\\li567\\ri567\\fi0\\sa60\\qj\\i ";
const char* const P_EPI = "\\s11\\li3402\\fi0\\sa60\\qj\\i ";
const char* const P_VERSE = "\\s12\\li1134\\fi0\\sa0\\ql ";
const char* const P_AUTHOR = "\\s13\\fi0\\qr\\i ";
const char* const P_SUB = "\\s14\\fi0\\sb120\\sa120\\qc\\b ";
const char* const P_PIC = "\\s15\\fi0\\sb120\\sa120\\qc ";
const char* const P_CODE = "\\s16\\fi0\\ql\\f1\\fs20 ";
const char* const P_LIST = "\\s17\\li567\\fi-283\\sa60\\qj ";
const char* const P_NOTE = "\\s18\\fi0\\sa0\\fs20 ";
const char* const P_CELL = "\\fi0\\sa0\\ql\\intbl ";

std::string Rtf::picture(const std::string& name) {
    const Image* im = findImage(book, name);
    if (!im) return std::string();
    std::string ext = lowerAscii(pathExt(name));
    const char* blip = ext == "png" ? "\\pngblip" : ((ext == "jpg" || ext == "jpeg") ? "\\jpegblip" : NULL);
    int w = 0, h = 0;
    if (!blip || !imageSize(im->bytes, w, h)) return std::string(); /* в RTF только PNG и JPEG */
    int pw = w, ph = h;
    fitPage(w, h);
    std::string s = "{\\pict";
    s += blip;
    s += "\\picw" + intToStr(pw) + "\\pich" + intToStr(ph) + "\\picwgoal" + intToStr(w * 15) + "\\pichgoal" + intToStr(h * 15) + " ";
    static const char* hex = "0123456789abcdef";
    s.reserve(s.size() + im->bytes.size() * 2 + im->bytes.size() / 64 + 8);
    for (size_t i = 0; i < im->bytes.size(); i++) {
        s += hex[im->bytes[i] >> 4];
        s += hex[im->bytes[i] & 15];
        if ((i & 63) == 63) s += "\r\n";
    }
    return s + "}";
}

std::string Rtf::noteText(const std::string& id) {
    for (size_t i = 0; i < book.notes.size(); i++)
        if (book.notes[i].id == id) return blocks(book.notes[i].blocks, true, P_NOTE);
    return std::string();
}

std::string Rtf::runs(const Runs& rs, bool inNote) {
    std::string s;
    for (size_t i = 0; i < rs.size(); i++) {
        const Run& r = rs[i];
        if (r.kind == Run::BR) { s += "\\line "; continue; }
        if (r.kind == Run::IMG) { s += picture(r.text); continue; }
        if (r.kind == Run::NOTE) {
            if (inNote) continue;
            std::string body = noteText(r.text);
            /* знак сноски: первый \chftn в тексте, второй — в начале первого абзаца самой сноски */
            const std::string lead = std::string("\\pard\\plain") + P_NOTE, mark = "{\\super\\chftn} ";
            if (startsWith(body, lead.c_str())) body.insert(lead.size(), mark);
            else body = lead + mark + "\\par\r\n" + body;
            s += "{\\super\\chftn{\\footnote " + body + "}}";
            continue;
        }
        std::string fmt;
        if (r.bold) fmt += "\\b";
        if (r.italic) fmt += "\\i";
        if (r.under) fmt += "\\ul";
        if (r.strike) fmt += "\\strike";
        if (r.sup) fmt += "\\super";
        else if (r.sub) fmt += "\\sub";
        if (r.mono) fmt += "\\f1";
        std::string t = rtfText(r.text);
        if (!r.href.empty()) {
            s += "{\\field{\\*\\fldinst HYPERLINK \"" + rtfText(r.href) + "\"}{\\fldrslt{\\ul " + (fmt.empty() ? t : "{" + fmt + " " + t + "}") + "}}}";
        } else if (fmt.empty()) {
            s += t;
        } else {
            s += "{" + fmt + " " + t + "}";
        }
    }
    return s;
}

std::string Rtf::para(const char* props, const Runs& rs, bool inNote) {
    return std::string("\\pard\\plain") + props + runs(rs, inNote) + "\\par\r\n";
}

std::string Rtf::table(const MNode* tbl, bool inNote) {
    Table t;
    buildTable(tbl, cx, t);
    if (!t.nrows || !t.ncols) return std::string();
    const int total = 9638; /* ширина текста A4 при полях 2 см, твипы */
    int cw = total / t.ncols;
    std::string s;
    for (int r = 0; r < t.nrows; r++) {
        std::string defs, cells;
        for (int c = 0; c < t.ncols;) {
            int id = t.at(r, c);
            int span = 1;
            while (id >= 0 && c + span < t.ncols && t.at(r, c + span) == id) span++;
            bool start = id >= 0 && (r == 0 || t.at(r - 1, c) != id);
            if (id >= 0 && t.cells[(size_t)id].rows > 1) defs += start ? "\\clvmgf" : "\\clvmrg";
            defs += "\\clbrdrt\\brdrs\\brdrw10\\clbrdrl\\brdrs\\brdrw10\\clbrdrb\\brdrs\\brdrw10\\clbrdrr\\brdrs\\brdrw10";
            defs += "\\cellx" + intToStr((long long)(c + span) * cw);
            cells += std::string("\\pard\\plain") + P_CELL;
            if (id >= 0 && start) {
                Runs rs = t.cells[(size_t)id].runs;
                if (t.cells[(size_t)id].head) for (size_t i = 0; i < rs.size(); i++) rs[i].bold = true;
                cells += runs(rs, inNote);
            }
            cells += "\\cell ";
            c += span;
        }
        s += "\\trowd\\trgaph108" + defs + "\r\n" + cells + "\\row\r\n";
    }
    return s + "\\pard\\plain\\par\r\n";
}

std::string Rtf::raw(const std::string& html, bool inNote) {
    MNode root = parseMarkup("<x>" + html + "</x>", true);
    const MNode* x = findNode(&root, "x");
    if (!x) return std::string();
    std::string s;
    Runs loose;
    for (size_t i = 0; i < x->kids.size(); i++) {
        const MNode* k = x->kids[i].get();
        if (!k->text && k->name == "table") {
            if (!loose.empty()) { s += para(P_NORMAL, loose, inNote); loose.clear(); }
            s += table(k, inNote);
        } else if (!k->text) {
            Runs tmp;
            collectInline(k, Run(), tmp, cx, 0);
            loose.insert(loose.end(), tmp.begin(), tmp.end());
        } else if (!collapse(k->value).empty()) {
            Run r;
            r.text = k->value;
            loose.push_back(r);
        }
    }
    if (!loose.empty()) s += para(P_NORMAL, loose, inNote);
    return s;
}

std::string Rtf::blocks(const std::vector<Block>& bl, bool inNote, const char* props) {
    std::string s;
    for (size_t i = 0; i < bl.size(); i++) {
        const Block& b = bl[i];
        switch (b.type) {
            case Block::SUB: s += para(P_SUB, parseInline(b.html, cx), inNote); break;
            case Block::EMPTY: s += "\\pard\\plain\\par\r\n"; break;
            case Block::IMG: {
                Runs rs;
                Run r;
                r.kind = Run::IMG;
                r.text = imageNameOf(b.src);
                rs.push_back(r);
                s += para(P_PIC, rs, inNote);
                break;
            }
            case Block::QUOTE: s += blocks(b.blocks, inNote, b.cls == "epigraph" ? P_EPI : P_QUOTE); break;
            case Block::POEM: s += blocks(b.blocks, inNote, P_VERSE); break;
            case Block::STANZA:
                s += blocks(b.blocks, inNote, P_VERSE);
                s += std::string("\\pard\\plain") + P_VERSE + "\\par\r\n";
                break;
            case Block::V: s += para(P_VERSE, parseInline(b.html, cx), inNote); break;
            case Block::AUTHOR: s += para(P_AUTHOR, parseInline(b.html, cx), inNote); break;
            case Block::LI: {
                std::string body = runs(parseInline(b.html, cx), inNote);
                s += std::string("\\pard\\plain") + P_LIST + "{\\pntext\\'95\\tab}{\\*\\pn\\pnlvlblt\\pnf0\\pnindent283{\\pntxtb\\'95}}" + body + "\\par\r\n";
                break;
            }
            case Block::PRE: s += std::string("\\pard\\plain") + P_CODE + rtfText(unescapeEntities(b.html)) + "\\par\r\n"; break;
            case Block::RAW: s += raw(b.html, inNote); break;
            default:
                if (!b.html.empty()) s += para(props, parseInline(b.html, cx), inNote);
                break;
        }
    }
    return s;
}

bool Rtf::write(Bytes& out) {
    normalizeBook(book);
    const Meta& m = book.meta;
    std::string al = authorsLine(book);
    std::string head =
        "{\\rtf1\\ansi\\ansicpg1251\\uc1\\deff0\\deflang1049\r\n"
        "{\\fonttbl{\\f0\\froman\\fcharset204 Times New Roman;}{\\f1\\fmodern\\fcharset204 Courier New;}}\r\n"
        "{\\stylesheet{\\s0\\fi284\\sa60\\qj Normal;}";
    /* «heading N» — встроенные имена: по ним Word строит навигацию, а уровень задаёт \outlinelevel */
    static const int sizes[6] = {36, 32, 28, 26, 24, 24};
    for (int i = 0; i < 6; i++)
        head += "{\\s" + intToStr(i + 1) + "\\keepn\\fi0\\sb280\\sa120\\qc\\outlinelevel" + intToStr(i) + "\\b\\fs" + intToStr(sizes[i]) +
                "\\sbasedon0\\snext0 heading " + intToStr(i + 1) + ";}";
    head += "{\\s7\\fi0\\sb240\\sa120\\qc\\b\\fs44\\sbasedon0\\snext0 Title;}"
            "{\\s10\\li567\\ri567\\fi0\\sa60\\qj\\i\\sbasedon0 Book Quote;}"
            "{\\s11\\li3402\\fi0\\sa60\\qj\\i\\sbasedon0 Epigraph;}"
            "{\\s12\\li1134\\fi0\\sa0\\ql\\sbasedon0 Verse;}"
            "{\\s13\\fi0\\qr\\i\\sbasedon0 Book Author;}"
            "{\\s14\\fi0\\sb120\\sa120\\qc\\b\\sbasedon0 Book Subtitle;}"
            "{\\s15\\fi0\\sb120\\sa120\\qc\\sbasedon0 Picture;}"
            "{\\s16\\fi0\\ql\\f1\\fs20\\sbasedon0 Code;}"
            "{\\s17\\li567\\fi-283\\sa60\\qj\\sbasedon0 List Bullet;}"
            "{\\s18\\fi0\\sa0\\fs20\\sbasedon0 footnote text;}}\r\n";
    head += "{\\info{\\title " + rtfText(m.title) + "}" + (al.empty() ? std::string() : "{\\author " + rtfText(al) + "}") +
            (m.annotation.empty() ? std::string() : "{\\subject " + rtfText(collapse(htmlToText(m.annotation))) + "}") + "}\r\n";
    head += "\\paperw11906\\paperh16838\\margl1134\\margr1134\\margt1134\\margb1134\\ftnbj\\fet0\\fs24\r\n";

    std::string body;
    if (!book.coverName.empty() && !coverUsedInText(book)) {
        Runs rs;
        Run r;
        r.kind = Run::IMG;
        r.text = book.coverName;
        rs.push_back(r);
        body += para(P_PIC, rs, false);
    }
    body += "\\pard\\plain\\s7\\fi0\\sb240\\sa120\\qc\\b\\fs44 " + rtfText(m.title) + "\\par\r\n";
    if (!al.empty()) body += "\\pard\\plain\\fi0\\qc\\fs28 " + rtfText(al) + "\\par\r\n";
    if (!m.seriesName.empty())
        body += "\\pard\\plain\\fi0\\qc\\fs28 " + rtfText(m.seriesName + (m.seriesNum.empty() ? std::string() : ", " + m.seriesNum)) + "\\par\r\n";
    if (!m.annotation.empty()) {
        std::string ann = collapse(htmlToText(m.annotation));
        if (!ann.empty()) body += std::string("\\pard\\plain") + P_QUOTE + rtfText(ann) + "\\par\r\n";
    }
    for (size_t i = 0; i < book.chapters.size(); i++) {
        const Chapter& ch = book.chapters[i];
        if (!ch.title.empty()) {
            int lvl = std::min(5, std::max(0, ch.level));
            std::string pr = "\\s" + intToStr(lvl + 1) + "\\keepn\\fi0\\sb280\\sa120\\qc\\outlinelevel" + intToStr(lvl) + "\\b\\fs" + intToStr(sizes[lvl]) + " ";
            body += para(pr.c_str(), parseInline(ch.title, cx), false);
        }
        body += blocks(ch.blocks, false, P_NORMAL);
    }
    /* сноски без ссылок в тексте: иначе они пропали бы */
    bool first = true;
    for (size_t i = 0; i < book.notes.size(); i++) {
        if (cx.referenced.count(book.notes[i].id)) continue;
        if (first) {
            body += "\\pard\\plain\\s1\\keepn\\fi0\\sb280\\sa120\\qc\\outlinelevel0\\b\\fs36 " + rtfText("Примечания") + "\\par\r\n";
            first = false;
        }
        if (!book.notes[i].title.empty()) {
            Runs t = parseInline(book.notes[i].title, cx);
            for (size_t k = 0; k < t.size(); k++) t[k].bold = true;
            body += para(P_NORMAL, t, false);
        }
        body += blocks(book.notes[i].blocks, false, P_NORMAL);
    }
    std::string all = head + body + "}";
    out.assign(all.begin(), all.end());
    return true;
}

} /* namespace */

bool writeDOCX(Book& book, Bytes& out, std::string* err) {
    Docx d(book);
    if (!d.write(out)) {
        if (err) *err = "Не удалось собрать DOCX";
        return false;
    }
    return true;
}

bool writeRTF(Book& book, Bytes& out, std::string* err) {
    Rtf r(book);
    if (!r.write(out)) {
        if (err) *err = "Не удалось собрать RTF";
        return false;
    }
    return true;
}
