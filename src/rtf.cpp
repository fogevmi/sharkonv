/* Чтение RTF (Word, WordPad, LibreOffice). Свой разбор: группы, управляющие слова,
   кодовые страницы шрифтов (\fcharset), \uN, стили и уровни заголовков (\outlinelevel,
   «heading N»), сноски (\chftn + {\footnote}), таблицы (\cell/\row), картинки PNG/JPEG. */
#include "convert.h"

namespace {

enum Dest { D_TEXT, D_SKIP, D_FONTTBL, D_STYLESHEET, D_INFO, D_TITLE, D_AUTHOR, D_PICT };

struct RtfState {
    Dest dest;
    bool bold, italic, strike, sup, sub, hidden;
    int uc;         /* сколько символов замены пропускать после \uN */
    int cp;         /* кодовая страница текущего шрифта */
    int sink;       /* -1 — основной текст, иначе номер сноски в notes */
    /* свойства абзаца */
    int style, outline;
    bool intbl, list;
    RtfState() : dest(D_TEXT), bold(false), italic(false), strike(false), sup(false), sub(false),
                 hidden(false), uc(1), cp(1252), sink(-1), style(0), outline(-1), intbl(false), list(false) {}
};

struct Seg {
    std::string html; /* уже экранированный текст или <img …/> */
    int fmt;          /* биты: 1 жирный, 2 курсив, 4 зачёркнутый, 8 sup, 16 sub */
};

struct Para {
    std::vector<Seg> segs;
    void add(const std::string& h, int fmt) {
        if (h.empty()) return;
        if (!segs.empty() && segs.back().fmt == fmt && segs.back().html.compare(0, 4, "<img") != 0 &&
            h.compare(0, 4, "<img") != 0) {
            segs.back().html += h;
            return;
        }
        Seg s;
        s.html = h;
        s.fmt = fmt;
        segs.push_back(s);
    }
    std::string render() const {
        std::string out;
        for (size_t i = 0; i < segs.size(); i++) {
            std::string h = segs[i].html;
            int f = segs[i].fmt;
            if (f & 8) h = "<sup>" + h + "</sup>";
            if (f & 16) h = "<sub>" + h + "</sub>";
            if (f & 4) h = "<s>" + h + "</s>";
            if (f & 1) h = "<strong>" + h + "</strong>";
            if (f & 2) h = "<em>" + h + "</em>";
            out += h;
        }
        return out;
    }
    void clear() { segs.clear(); }
};

/* 0 — «как у документа» (ANSI_CHARSET / DEFAULT_CHARSET) */
int charsetToCp(int cs) {
    switch (cs) {
        case 0:
        case 1: return 0;
        case 2: return 1252; /* Symbol: байты — не буквы, лишь бы не кириллица */
        case 204: return 1251;
        case 238: return 1250;
        case 161: return 1253;
        case 162: return 1254;
        case 177: return 1255;
        case 178: return 1256;
        case 186: return 1257;
        case 163: return 1258;
        case 222: return 874;
        case 128: return 932;
        case 134: return 936;
        case 129: return 949;
        case 136: return 950;
        default: return 1252;
    }
}

std::string bytesToUtf8(const std::string& bytes, int cp) {
    if (bytes.empty()) return std::string();
    int n = MultiByteToWideChar((UINT)cp, 0, bytes.data(), (int)bytes.size(), NULL, 0);
    if (n <= 0) return cpToUtf8(bytes, 1252);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar((UINT)cp, 0, bytes.data(), (int)bytes.size(), &w[0], n);
    return wToUtf8(w);
}

std::string hexToBytes(const std::string& hex) {
    std::string out;
    int hi = -1;
    for (size_t i = 0; i < hex.size(); i++) {
        char c = hex[i];
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { out += (char)(hi * 16 + v); hi = -1; }
    }
    return out;
}

class RtfReader {
public:
    RtfReader(Book& b) : book(b), docCp(0), skipChars(0), pendingNote(-1), styleNum(-1),
                         fontNum(-1), pictType(0), cur(0), guessedCp(1252), deffFont(0) {
        chapters.push_back(Chapter());
    }

    bool parse(const std::string& s);
    std::vector<Chapter> chapters;

private:
    Book& book;
    RtfState st;
    std::vector<RtfState> stack;
    std::string bytes;      /* \'xx, ждущие перекодировки */
    int docCp;              /* \ansicpgN; 0 — не указан */
    int skipChars;          /* остаток символов замены после \uN */
    int pendingNote;        /* \chftn в тексте уже поставил ссылку на эту сноску */
    std::map<int, int> fontCp;
    std::map<int, std::string> styleName;
    std::map<int, int> styleOutline;
    int styleNum;
    std::string styleText;
    int fontNum;
    struct InfoPiece {
        bool raw;      /* байты в неизвестной пока кодировке; иначе — уже UTF-8 */
        std::string s;
    };
    std::vector<InfoPiece> infoText, infoTitle, infoAuthor;
    std::map<int, size_t> cpBytes; /* сколько байт текста пришло в каждой кодировке */

    std::string decodeInfo(const std::vector<InfoPiece>& pieces) const {
        int cp = docDefaultCp();
        size_t best = 0;
        for (std::map<int, size_t>::const_iterator it = cpBytes.begin(); it != cpBytes.end(); ++it)
            if (it->second > best) { best = it->second; cp = it->first; }
        std::string out;
        for (size_t i = 0; i < pieces.size(); i++)
            out += pieces[i].raw ? bytesToUtf8(pieces[i].s, cp) : pieces[i].s;
        return collapse(out);
    }

public:
    void finishInfo() {
        std::string t = decodeInfo(infoTitle), a = decodeInfo(infoAuthor);
        if (!t.empty() && book.meta.title.empty()) book.meta.title = t;
        if (!a.empty() && book.meta.authors.empty()) book.meta.authors.push_back(splitAuthor(a, false));
    }

private:
    std::string pictHex;
    int pictType;           /* 1 png, 2 jpeg, 0 — не умеем */
    Para body, note, cell;  /* текущие абзацы основного текста и сноски */
    std::vector<std::string> cells;
    std::vector<std::vector<std::string> > rows;
    size_t cur;

    /* кодовая страница документа: \ansicpgN, а при \ansicpg0 (так пишет Word) — по шрифтам:
       есть кириллические шрифты — значит, cp1251 */
    int guessedCp;
    int docDefaultCp() const { return docCp > 0 ? docCp : guessedCp; }
    int cpForFont(int f) const {
        std::map<int, int>::const_iterator it = fontCp.find(f);
        return it != fontCp.end() && it->second > 0 ? it->second : docDefaultCp();
    }
    void fontsDone() {
        std::map<int, int> count;
        for (std::map<int, int>::const_iterator it = fontCp.begin(); it != fontCp.end(); ++it)
            if (it->second > 0 && it->second != 1252) count[it->second]++;
        int best = 0;
        for (std::map<int, int>::const_iterator it = count.begin(); it != count.end(); ++it)
            if (best == 0 || it->second > count[best]) best = it->first;
        guessedCp = best ? best : 1252;
        st.cp = cpForFont(deffFont);
    }
    int deffFont;

    int fmt() const {
        return (st.bold ? 1 : 0) | (st.italic ? 2 : 0) | (st.strike ? 4 : 0) | (st.sup ? 8 : 0) | (st.sub ? 16 : 0);
    }
    Para& para() { return st.sink >= 0 ? note : body; }
    bool textDest() const { return st.dest == D_TEXT || st.dest == D_TITLE || st.dest == D_AUTHOR; }

    void flushBytes();
    void putText(const std::string& utf8);
    void endPara();
    void endCell();
    void endRow();
    void flushTable();
    void control(const std::string& word, bool hasParam, int param);
    void groupEnd(const RtfState& inner);
    void addChapterBlock(const std::string& html, const RtfState& ps);
};

void RtfReader::flushBytes() {
    if (bytes.empty()) return;
    std::string b;
    b.swap(bytes);
    if (st.dest == D_TITLE || st.dest == D_AUTHOR) {
        /* {\info} идёт раньше текста, а кодировку надёжно видно только по тексту — ждём */
        InfoPiece p;
        p.raw = true;
        p.s = b;
        infoText.push_back(p);
        return;
    }
    if (st.dest == D_TEXT) cpBytes[st.cp] += b.size();
    putText(bytesToUtf8(b, st.cp));
}

void RtfReader::putText(const std::string& utf8) {
    if (utf8.empty()) return;
    switch (st.dest) {
        case D_TEXT:
            if (!st.hidden) para().add(escHtml(utf8), fmt());
            break;
        case D_TITLE:
        case D_AUTHOR: {
            InfoPiece p;
            p.raw = false;
            p.s = utf8;
            infoText.push_back(p);
            break;
        }
        case D_STYLESHEET:
            styleText += utf8;
            break;
        default:
            break;
    }
}

/* конец абзаца: в ячейку таблицы, в сноску или в главу */
void RtfReader::endPara() {
    flushBytes();
    if (st.dest != D_TEXT) return;
    if (st.sink >= 0) {
        std::string h = trim(collapseSpacesPreserveBr(note.render()));
        note.clear();
        if (!collapse(htmlToText(h)).empty() || h.find("<img") != std::string::npos)
            book.notes[(size_t)st.sink].blocks.push_back(Block::p(h));
        return;
    }
    std::string h = trim(collapseSpacesPreserveBr(body.render()));
    body.clear();
    if (st.intbl) {
        if (!h.empty()) {
            if (!cell.segs.empty()) cell.add("<br/>", 0);
            cell.add(h, 0);
        }
        return;
    }
    flushTable();
    addChapterBlock(h, st);
}

void RtfReader::addChapterBlock(const std::string& html, const RtfState& ps) {
    std::string plain = collapse(htmlToText(html));
    std::string name = toLowerRu(collapse(styleName.count(ps.style) ? styleName[ps.style] : std::string()));
    std::vector<Block>& blocks = chapters[cur].blocks;

    if (startsWith(name, "toc ") || startsWith(name, "оглавление ")) return;
    if ((name == "title" || name == "название" || name == "заголовок") && !plain.empty()) {
        if (book.meta.title.empty()) book.meta.title = plain;
        return;
    }
    int level = ps.outline;
    if (level < 0 && styleOutline.count(ps.style)) level = styleOutline[ps.style];
    if (level < 0) {
        const char* pfx[] = {"heading ", "заголовок "};
        for (int k = 0; k < 2 && level < 0; k++) {
            size_t pl = strlen(pfx[k]);
            if (name.compare(0, pl, pfx[k]) == 0 && name.size() > pl && isdigit((unsigned char)name[pl]))
                level = atoi(name.c_str() + pl) - 1;
        }
    }
    if (level >= 0 && level <= 8 && !plain.empty()) {
        Chapter c;
        c.level = level;
        c.title = html;
        chapters.push_back(c);
        cur = chapters.size() - 1;
        return;
    }
    if (html.empty()) {
        if (!blocks.empty() && blocks.back().type != Block::EMPTY) blocks.push_back(Block::empty());
        return;
    }
    if (plain.empty() && html.find("<img") != std::string::npos) {
        size_t pos = 0;
        while ((pos = html.find("src=\"images/", pos)) != std::string::npos) {
            size_t e = html.find('"', pos + 12);
            if (e == std::string::npos) break;
            Block b(Block::IMG);
            b.src = "images/" + html.substr(pos + 12, e - pos - 12);
            blocks.push_back(b);
            pos = e;
        }
        return;
    }
    Block b(ps.list ? Block::LI : Block::P);
    b.html = html;
    blocks.push_back(b);
}

void RtfReader::endCell() {
    endPara();
    cells.push_back(trim(cell.render()));
    cell.clear();
}

void RtfReader::endRow() {
    flushBytes();
    if (!cells.empty()) rows.push_back(cells);
    cells.clear();
}

void RtfReader::flushTable() {
    if (!cells.empty()) endRow();
    if (rows.empty()) return;
    std::string html = "<table>";
    for (size_t r = 0; r < rows.size(); r++) {
        html += "<tr>";
        for (size_t c = 0; c < rows[r].size(); c++) html += "<td>" + rows[r][c] + "</td>";
        html += "</tr>";
    }
    rows.clear();
    Block b(Block::RAW);
    b.html = html + "</table>";
    chapters[cur].blocks.push_back(b);
}

void RtfReader::groupEnd(const RtfState& inner) {
    if (inner.dest == D_PICT && pictType && !pictHex.empty() && st.dest == D_TEXT) {
        std::string raw = hexToBytes(pictHex);
        Bytes data(raw.begin(), raw.end());
        std::string name = addImage(book, pictType == 1 ? "image/png" : "image/jpeg", data);
        if (!name.empty()) {
            RtfState save = st;
            st.sink = inner.sink;
            para().add("<img src=\"images/" + name + "\" alt=\"\"/>", 0);
            st = save;
        }
    }
    if (inner.dest == D_PICT) { pictHex.clear(); pictType = 0; }
    if ((inner.dest == D_TITLE || inner.dest == D_AUTHOR) && st.dest != inner.dest) {
        std::vector<InfoPiece>& target = inner.dest == D_TITLE ? infoTitle : infoAuthor;
        if (target.empty()) target = infoText;
        infoText.clear();
    }
    /* конец {\footnote …}: недописанный абзац сноски */
    if (inner.sink >= 0 && st.sink != inner.sink) {
        RtfState save = st;
        st = inner;
        endPara();
        st = save;
    }
}

void RtfReader::control(const std::string& w, bool hasParam, int param) {
    /* назначения (меняют, куда идёт текст группы) */
    if (w == "fonttbl") { st.dest = D_FONTTBL; return; }
    if (w == "stylesheet") { st.dest = D_STYLESHEET; return; }
    if (w == "info") { st.dest = D_INFO; st.cp = docDefaultCp(); return; }
    if (w == "title" && st.dest == D_INFO) { flushBytes(); st.dest = D_TITLE; infoText.clear(); return; }
    if (w == "author" && st.dest == D_INFO) { flushBytes(); st.dest = D_AUTHOR; infoText.clear(); return; }
    if (w == "pict") { st.dest = D_PICT; pictHex.clear(); pictType = 0; return; }
    static const char* skipDest[] = {
        "colortbl", "listtable", "listoverridetable", "rsidtbl", "generator", "xmlnstbl", "themedata",
        "colorschememapping", "latentstyles", "datastore", "pgdsctbl", "header", "footer", "headerl",
        "headerr", "headerf", "footerl", "footerr", "footerf", "nonshppict", "object", "fldinst",
        "listtext", "pntext", "pntxta", "pntxtb", "comment", "annotation", "atnid", "atnauthor", "xe",
        "tc", "revtbl", "operator", "company", "doccomm", "keywords", "subject", "manager", "category",
        "bkmkstart", "bkmkend", "template", "filetbl", "mmathPr", "wgrffmtfilter", "ftnsep", "ftnsepc",
        "ftncn", "aftnsep", "aftnsepc", "aftncn", "userprops", "docvar", "printim", "creatim", "revtim",
        "buptim", "hlinkbase", "passwordhash", "sp", "picprop", "blipuid", "shpinst", "shptxt", NULL};
    for (int i = 0; skipDest[i]; i++)
        if (w == skipDest[i]) { st.dest = D_SKIP; return; }

    if (st.dest == D_PICT) {
        if (w == "pngblip") pictType = 1;
        else if (w == "jpegblip") pictType = 2;
        else if (w == "emfblip" || w == "wmetafile" || w == "dibitmap" || w == "wbitmap" || w == "macpict")
            pictType = 0;
        return;
    }
    if (st.dest == D_FONTTBL) {
        if (w == "f" && hasParam) fontNum = param;
        else if (w == "fcharset" && fontNum >= 0) fontCp[fontNum] = charsetToCp(param);
        else if (w == "cpg" && fontNum >= 0 && param > 0) fontCp[fontNum] = param;
        return;
    }
    if (st.dest == D_STYLESHEET) {
        if (w == "s" && hasParam) { styleNum = param; styleText.clear(); }
        else if (w == "cs" || w == "ds" || w == "ts") { styleNum = -1; styleText.clear(); }
        else if (w == "outlinelevel" && styleNum >= 0) styleOutline[styleNum] = param;
        return;
    }

    /* документ */
    if (w == "ansicpg") { docCp = param; if (param > 0) st.cp = param; return; }
    if (w == "deff") { deffFont = param; return; }
    if (w == "uc") { st.uc = hasParam ? param : 1; return; }
    if (w == "u") {
        flushBytes();
        int v = param < 0 ? param + 65536 : param;
        std::wstring ws(1, (wchar_t)v);
        putText(wToUtf8(ws));
        skipChars = st.uc;
        return;
    }
    if (w == "f") {
        flushBytes();
        st.cp = cpForFont(param);
        return;
    }
    if (!textDest()) return;

    bool on = !hasParam || param != 0;
    if (w == "par") { endPara(); return; }
    if (w == "line") { flushBytes(); para().add("<br/>", 0); return; }
    if (w == "tab") { putText(" "); return; }
    if (w == "cell") { endCell(); return; }
    if (w == "row") { endRow(); return; }
    if (w == "nestcell") { putText(" "); return; }
    if (w == "nestrow") { flushBytes(); para().add("<br/>", 0); return; }
    if (w == "pard") { flushBytes(); st.style = 0; st.outline = -1; st.intbl = false; st.list = false; return; }
    if (w == "plain") {
        flushBytes();
        st.bold = st.italic = st.strike = st.sup = st.sub = st.hidden = false;
        st.cp = cpForFont(deffFont);
        return;
    }
    if (w == "s") { st.style = param; return; }
    if (w == "outlinelevel") { st.outline = param; return; }
    if (w == "intbl") { st.intbl = true; return; }
    if (w == "ls" || w == "ilvl") { st.list = true; return; }
    if (w == "b") { flushBytes(); st.bold = on; return; }
    if (w == "i") { flushBytes(); st.italic = on; return; }
    if (w == "strike" || w == "striked") { flushBytes(); st.strike = on; return; }
    if (w == "super") { flushBytes(); st.sup = on; st.sub = false; return; }
    if (w == "sub") { flushBytes(); st.sub = on; st.sup = false; return; }
    if (w == "nosupersub") { flushBytes(); st.sup = st.sub = false; return; }
    if (w == "v") { flushBytes(); st.hidden = on; return; }
    if (w == "emdash") { putText("\xE2\x80\x94"); return; }
    if (w == "endash") { putText("\xE2\x80\x93"); return; }
    if (w == "lquote") { putText("\xE2\x80\x98"); return; }
    if (w == "rquote") { putText("\xE2\x80\x99"); return; }
    if (w == "ldblquote") { putText("\xE2\x80\x9C"); return; }
    if (w == "rdblquote") { putText("\xE2\x80\x9D"); return; }
    if (w == "bullet") { putText("\xE2\x80\xA2"); return; }
    if (w == "emspace" || w == "enspace" || w == "qmspace") { putText(" "); return; }

    /* сноски: \chftn в тексте — место ссылки, {\footnote …} — сама сноска */
    if (w == "chftn") {
        if (st.sink >= 0) return; /* номер внутри самой сноски */
        flushBytes();
        Note n;
        n.id = "rtfnote" + intToStr((long long)book.notes.size() + 1);
        n.title = intToStr((long long)book.notes.size() + 1);
        book.notes.push_back(n);
        pendingNote = (int)book.notes.size() - 1;
        body.add("<a href=\"#" + n.id + "\">[" + n.title + "]</a>", 0);
        return;
    }
    if (w == "footnote") {
        flushBytes();
        if (pendingNote < 0) {
            Note n;
            n.id = "rtfnote" + intToStr((long long)book.notes.size() + 1);
            n.title = intToStr((long long)book.notes.size() + 1);
            book.notes.push_back(n);
            pendingNote = (int)book.notes.size() - 1;
            body.add("<a href=\"#" + n.id + "\">[" + n.title + "]</a>", 0);
        }
        st.sink = pendingNote;
        pendingNote = -1;
        note.clear();
        st.bold = st.italic = st.strike = st.sup = st.sub = false;
        return;
    }
}

bool RtfReader::parse(const std::string& s) {
    const size_t n = s.size();
    size_t i = 0;
    bool starNext = false; /* \* — следующее назначение можно пропустить, если оно незнакомо */
    while (i < n) {
        char c = s[i];
        if (c == '{') {
            flushBytes();
            stack.push_back(st);
            if (stack.size() > 400) return false;
            i++;
            continue;
        }
        if (c == '}') {
            flushBytes();
            if (stack.empty()) break;
            RtfState inner = st;
            st = stack.back();
            stack.pop_back();
            groupEnd(inner);
            if (inner.dest == D_FONTTBL && st.dest != D_FONTTBL) fontsDone();
            if (inner.dest == D_STYLESHEET && st.dest != D_STYLESHEET) styleNum = -1;
            i++;
            continue;
        }
        if (c == '\\') {
            if (i + 1 >= n) break;
            char d = s[i + 1];
            if (!isalpha((unsigned char)d)) {
                i += 2;
                if (d == '\'') {
                    if (i + 2 <= n) {
                        std::string hx = s.substr(i, 2);
                        i += 2;
                        if (skipChars > 0) { skipChars--; continue; }
                        if (st.dest == D_PICT) continue;
                        bytes += (char)strtol(hx.c_str(), NULL, 16);
                    }
                    continue;
                }
                if (skipChars > 0) { skipChars--; continue; }
                if (d == '*') { starNext = true; continue; }
                if (d == '\\' || d == '{' || d == '}') { flushBytes(); putText(std::string(1, d)); continue; }
                if (d == '~') { putText("\xC2\xA0"); continue; }
                if (d == '_') { putText("-"); continue; }
                if (d == '\n' || d == '\r') { endPara(); continue; }
                continue; /* \- и прочие */
            }
            size_t p = i + 1;
            while (p < n && isalpha((unsigned char)s[p])) p++;
            std::string word = s.substr(i + 1, p - i - 1);
            bool hasParam = false, neg = false;
            int param = 0;
            if (p < n && s[p] == '-') { neg = true; p++; }
            while (p < n && isdigit((unsigned char)s[p])) {
                hasParam = true;
                param = param * 10 + (s[p] - '0');
                if (param > 100000000) param = 100000000;
                p++;
            }
            if (neg) param = -param;
            if (p < n && s[p] == ' ') p++;
            i = p;
            if (word == "bin" && hasParam && param > 0) { i += (size_t)param; continue; }
            if (skipChars > 0 && word != "u") { skipChars--; continue; }
            if (starNext) {
                starNext = false;
                /* знакомые «звёздные» назначения, которые нужно читать */
                if (word != "shppict" && word != "footnote" && word != "chftn") {
                    bool known = word == "fldinst" || word == "pict";
                    if (!known) { st.dest = D_SKIP; continue; }
                }
            }
            if (word == "shppict") continue;
            if (st.dest == D_SKIP) continue;
            control(word, hasParam, param);
            continue;
        }
        if (c == '\r' || c == '\n') { i++; continue; }
        if (st.dest == D_PICT) {
            pictHex += c;
            i++;
            continue;
        }
        if (skipChars > 0) { skipChars--; i++; continue; }
        if (st.dest == D_STYLESHEET && c == ';') {
            flushBytes();
            if (styleNum >= 0) styleName[styleNum] = collapse(styleText);
            styleText.clear();
            i++;
            continue;
        }
        if ((unsigned char)c >= 0x80) { bytes += c; i++; continue; }
        flushBytes();
        size_t e = i;
        while (e < n && s[e] != '\\' && s[e] != '{' && s[e] != '}' && s[e] != '\r' && s[e] != '\n' &&
               (unsigned char)s[e] < 0x80 && !(st.dest == D_STYLESHEET && s[e] == ';'))
            e++;
        if (e == i) e = i + 1;
        putText(s.substr(i, e - i));
        i = e;
    }
    flushBytes();
    endPara();
    flushTable();
    return true;
}

} /* namespace */

bool readRTF(const Bytes& data, const std::string& fileName, Book& book, std::string* err) {
    std::string s(data.begin(), data.end());
    if (s.compare(0, 5, "{\\rtf") != 0) {
        if (err) *err = "Это не RTF";
        return false;
    }
    book.fileName = fileName;
    RtfReader r(book);
    if (!r.parse(s)) {
        if (err) *err = "Повреждённый RTF (слишком глубокая вложенность)";
        return false;
    }
    r.finishInfo();
    /* сноски без текста (битый файл) не нужны */
    std::vector<Note> notes;
    for (size_t i = 0; i < book.notes.size(); i++)
        if (!book.notes[i].blocks.empty()) notes.push_back(book.notes[i]);
    book.notes.swap(notes);

    book.chapters.clear();
    for (size_t i = 0; i < r.chapters.size(); i++) {
        bool has = !r.chapters[i].title.empty();
        for (size_t k = 0; k < r.chapters[i].blocks.size() && !has; k++)
            if (r.chapters[i].blocks[k].type != Block::EMPTY) has = true;
        if (has) book.chapters.push_back(r.chapters[i]);
    }
    normalizeChapterLevels(book.chapters);
    if (book.chapters.empty()) {
        if (err) *err = "В RTF не нашлось текста";
        return false;
    }
    return true;
}
