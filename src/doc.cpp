/* Чтение Word 97–2003 (.doc) по спецификации [MS-DOC] и [MS-CFB], без сторонних библиотек.
   Контейнер OLE (FAT, DIFAT, мини-поток, каталог) -> потоки WordDocument, 0Table/1Table, Data,
   SummaryInformation. Текст — по таблице кусков (сжатые 8-битные и UTF-16 куски); свойства
   символов и абзацев — по страницам FKP и таблице стилей; уровни заголовков — по встроенному
   номеру стиля (sti 1–9 «heading N» на любом языке Word) и outline level; поля Word — их
   результат, а не код; сноски и концевые сноски — отдельные диапазоны текста; таблицы — метки
   ячеек и строк; картинки — записи OfficeArt (PNG/JPEG) в потоке Data. */
#include "convert.h"

namespace {

unsigned rd16(const std::string& s, size_t p) {
    return p + 2 <= s.size() ? (unsigned)(unsigned char)s[p] | ((unsigned)(unsigned char)s[p + 1] << 8) : 0;
}

unsigned rd32(const std::string& s, size_t p) {
    return p + 4 <= s.size() ? rd16(s, p) | (rd16(s, p + 2) << 16) : 0;
}

/* ============================ контейнер OLE ============================ */

class Cfb {
public:
    explicit Cfb(const std::string& d) : data(d), ss(512) {}
    bool open();
    bool stream(const char* name, std::string& out) const;

private:
    const std::string& data;
    unsigned ss;
    unsigned miniCutoff;
    std::vector<unsigned> fat, miniFat;
    std::string miniStream;
    struct Entry {
        std::string name; /* ASCII-часть имени, в нижнем регистре */
        int type;
        unsigned start;
        unsigned size;
    };
    std::vector<Entry> dir;

    size_t sectorOffset(unsigned sec) const { return ((size_t)sec + 1) * ss; }
    std::string chain(unsigned start, size_t size, bool mini) const;
};

std::string Cfb::chain(unsigned start, size_t size, bool mini) const {
    std::string out;
    const std::vector<unsigned>& table = mini ? miniFat : fat;
    unsigned unit = mini ? 64 : ss;
    unsigned sec = start;
    size_t guard = 0, limit = table.size() + 1;
    while (sec < 0xFFFFFFFA && out.size() < size && guard++ < limit) {
        size_t off = mini ? (size_t)sec * 64 : sectorOffset(sec);
        const std::string& src = mini ? miniStream : data;
        if (off >= src.size()) break;
        out.append(src, off, std::min((size_t)unit, src.size() - off));
        if (sec >= table.size()) break;
        sec = table[sec];
    }
    if (out.size() > size) out.resize(size);
    return out;
}

bool Cfb::open() {
    if (data.size() < 512 || data.compare(0, 8, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) != 0) return false;
    unsigned shift = rd16(data, 0x1E);
    if (shift != 9 && shift != 12) return false;
    ss = 1u << shift;
    unsigned dirStart = rd32(data, 0x30);
    miniCutoff = rd32(data, 0x38);
    unsigned miniFatStart = rd32(data, 0x3C);
    unsigned difatSec = rd32(data, 0x44);

    std::vector<unsigned> fatSectors;
    for (unsigned i = 0; i < 109; i++) {
        unsigned v = rd32(data, 0x4C + 4 * i);
        if (v < 0xFFFFFFFA) fatSectors.push_back(v);
    }
    for (size_t guard = 0; difatSec < 0xFFFFFFFA && guard < 100000; guard++) {
        size_t base = sectorOffset(difatSec);
        if (base + ss > data.size()) break;
        for (unsigned k = 0; k + 1 < ss / 4; k++) {
            unsigned v = rd32(data, base + 4 * k);
            if (v < 0xFFFFFFFA) fatSectors.push_back(v);
        }
        difatSec = rd32(data, base + ss - 4);
    }
    for (size_t i = 0; i < fatSectors.size(); i++) {
        size_t base = sectorOffset(fatSectors[i]);
        if (base + ss > data.size()) continue;
        for (unsigned k = 0; k < ss / 4; k++) fat.push_back(rd32(data, base + 4 * k));
    }

    std::string d = chain(dirStart, (size_t)-1 / 2, false);
    for (size_t p = 0; p + 128 <= d.size(); p += 128) {
        Entry e;
        unsigned nameLen = rd16(d, p + 0x40);
        for (unsigned k = 0; k + 2 <= nameLen && k < 64; k += 2) {
            unsigned c = rd16(d, p + k);
            if (c == 0) break;
            e.name += (char)(c < 128 ? tolower((int)c) : '?');
        }
        e.type = (unsigned char)d[p + 0x42];
        e.start = rd32(d, p + 0x74);
        e.size = rd32(d, p + 0x78);
        dir.push_back(e);
    }
    if (dir.empty() || dir[0].type != 5) return false;
    miniStream = chain(dir[0].start, dir[0].size, false);
    std::string mf = chain(miniFatStart, (size_t)-1 / 2, false);
    for (size_t p = 0; p + 4 <= mf.size(); p += 4) miniFat.push_back(rd32(mf, p));
    return true;
}

bool Cfb::stream(const char* name, std::string& out) const {
    std::string want = lowerAscii(name);
    for (size_t i = 0; i < dir.size(); i++) {
        if (dir[i].type != 2 || dir[i].name != want) continue;
        if (dir[i].size > 512u * 1024 * 1024) return false;
        out = chain(dir[i].start, dir[i].size, dir[i].size < miniCutoff);
        return true;
    }
    return false;
}

/* ============================== свойства ============================== */

/* размер операнда sprm по spra (MS-DOC 2.2.5.1) */
size_t sprmOperandSize(unsigned sprm, const std::string& g, size_t p) {
    switch ((sprm >> 13) & 7) {
        case 0: case 1: return 1;
        case 2: case 4: case 5: return 2;
        case 3: return 4;
        case 7: return 3;
        default:
            /* sprmTDefTable: cb на 2 байта = длина остатка + 1 */
            if (sprm == 0xD608 || sprm == 0xD606) return 2 + (rd16(g, p) ? rd16(g, p) - 1 : 0);
            if (sprm == 0xC615) {                                              /* sprmPChgTabs */
                unsigned cb = p < g.size() ? (unsigned char)g[p] : 0;
                return cb == 255 ? 1 : 1 + cb;
            }
            return 1 + (p < g.size() ? (unsigned char)g[p] : 0);
    }
}

struct CharProps {
    bool bold, italic, strike, special;
    int iss;           /* 1 — надстрочный, 2 — подстрочный */
    unsigned picLoc;   /* sprmCPicLocation: смещение картинки в потоке Data */
    bool hasPic;
    bool hidden;
    CharProps() : bold(false), italic(false), strike(false), special(false), iss(0), picLoc(0), hasPic(false), hidden(false) {}
};

struct ParaProps {
    unsigned istd;
    int outline;       /* 0..8, -1 — не задан */
    bool inTable, ttp, list;
    std::vector<int> cellEdges; /* sprmTDefTable в метке строки: границы ячеек (в twips) */
    ParaProps() : istd(0), outline(-1), inTable(false), ttp(false), list(false) {}
};

/* переключатели (bold и т. п.): 0 — выкл, 1 — вкл, 0x80 — как в стиле, 0x81 — наоборот */
bool toggle(unsigned v) { return v == 1 || v == 0x81; }

void applyChpx(const std::string& g, CharProps& c) {
    size_t p = 0;
    while (p + 2 <= g.size()) {
        unsigned sprm = rd16(g, p);
        p += 2;
        size_t n = sprmOperandSize(sprm, g, p);
        unsigned v = p < g.size() ? (unsigned char)g[p] : 0;
        switch (sprm) {
            case 0x0835: c.bold = toggle(v); break;
            case 0x0836: c.italic = toggle(v); break;
            case 0x0837: case 0x2A53: c.strike = toggle(v); break;
            case 0x083C: c.hidden = toggle(v); break;  /* sprmCFVanish — скрытый текст */
            case 0x2A48: c.iss = (int)v; break;        /* sprmCIss */
            case 0x0855: c.special = v != 0; break;    /* sprmCFSpec */
            case 0x6A03: c.picLoc = rd32(g, p); c.hasPic = true; break;
            default: break;
        }
        p += n;
    }
}

void applyPapx(const std::string& g, ParaProps& pp) {
    size_t p = 0;
    while (p + 2 <= g.size()) {
        unsigned sprm = rd16(g, p);
        p += 2;
        size_t n = sprmOperandSize(sprm, g, p);
        unsigned v = p < g.size() ? (unsigned char)g[p] : 0;
        switch (sprm) {
            case 0x2640: pp.outline = v <= 8 ? (int)v : -1; break; /* sprmPOutLvl */
            case 0x2416: pp.inTable = v != 0; break;               /* sprmPFInTable */
            case 0x2417: pp.ttp = v != 0; break;                   /* sprmPFTtp */
            case 0x244B: if (v) pp.inTable = true; break;          /* sprmPFInnerTableCell */
            case 0x244C: if (v) { pp.inTable = true; pp.ttp = true; } break;
            case 0x6649: if (rd32(g, p)) pp.inTable = true; break; /* sprmPItap */
            case 0x460B: pp.list = rd16(g, p) != 0; break;         /* sprmPIlfo */
            case 0xD608: {                                         /* sprmTDefTable: cb, itcMac, rgdxaCenter */
                unsigned itc = p + 2 < g.size() ? (unsigned char)g[p + 2] : 0;
                pp.cellEdges.clear();
                for (unsigned k = 0; k <= itc && k < 64 && p + 3 + 2 * k + 2 <= g.size(); k++)
                    pp.cellEdges.push_back((short)rd16(g, p + 3 + 2 * k));
                break;
            }
            default: break;
        }
        p += n;
    }
}

struct FcRun {
    unsigned fc0, fc1;
    std::string grpprl;
    unsigned istd;
};

/* PlcBteChpx / PlcBtePapx -> страницы FKP по 512 байт в WordDocument -> прогоны по FC */
void readFkps(const std::string& wd, const std::string& tbl, unsigned fcPlc, unsigned lcbPlc, bool para,
              std::vector<FcRun>& runs) {
    if (lcbPlc < 8 || (size_t)fcPlc + lcbPlc > tbl.size()) return;
    unsigned n = (lcbPlc - 4) / 8;
    for (unsigned i = 0; i < n; i++) {
        unsigned pn = rd32(tbl, fcPlc + 4 * (n + 1) + 4 * i) & 0x3FFFFF;
        size_t page = (size_t)pn * 512;
        if (page + 512 > wd.size()) continue;
        std::string fkp = wd.substr(page, 512);
        unsigned crun = (unsigned char)fkp[511];
        for (unsigned k = 0; k < crun && k < 101; k++) {
            FcRun r;
            r.fc0 = rd32(fkp, 4 * k);
            r.fc1 = rd32(fkp, 4 * (k + 1));
            r.istd = 0;
            if (!para) {
                unsigned off = (unsigned char)fkp[4 * (crun + 1) + k] * 2u;
                if (off && off < 511) {
                    unsigned cb = (unsigned char)fkp[off];
                    r.grpprl = fkp.substr(off + 1, std::min((size_t)cb, (size_t)511 - off - 1));
                }
            } else {
                unsigned off = (unsigned char)fkp[4 * (crun + 1) + 13 * k] * 2u;
                if (off && off < 511) {
                    unsigned cb = (unsigned char)fkp[off];
                    size_t start = off + 1, len;
                    if (cb == 0) {
                        len = 2u * (unsigned char)fkp[off + 1];
                        start = off + 2;
                    } else {
                        len = 2u * cb - 1;
                    }
                    if (start + len > 511) len = 511 > start ? 511 - start : 0;
                    std::string px = fkp.substr(start, len);
                    r.istd = rd16(px, 0);
                    r.grpprl = px.size() > 2 ? px.substr(2) : std::string();
                }
            }
            runs.push_back(r);
        }
    }
    for (size_t a = 1; a < runs.size(); a++)
        for (size_t b = a; b > 0 && runs[b].fc0 < runs[b - 1].fc0; b--) std::swap(runs[b], runs[b - 1]);
}

const FcRun* findRun(const std::vector<FcRun>& runs, unsigned fc) {
    size_t lo = 0, hi = runs.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (runs[mid].fc1 <= fc) lo = mid + 1;
        else hi = mid;
    }
    if (lo < runs.size() && runs[lo].fc0 <= fc && fc < runs[lo].fc1) return &runs[lo];
    return NULL;
}

/* ============================== стили ============================== */

struct Style {
    unsigned sti, base;
    std::string name;
    int outline;
    bool list; /* стиль задаёт нумерацию/маркер (sprmPIlfo) */
    Style() : sti(0x0FFE), base(0x0FFF), outline(-1), list(false) {}
};

void readStyles(const std::string& tbl, unsigned fc, unsigned lcb, std::vector<Style>& out) {
    if (lcb < 4 || (size_t)fc + lcb > tbl.size()) return;
    std::string st = tbl.substr(fc, lcb);
    unsigned cbStshi = rd16(st, 0);
    unsigned cstd = rd16(st, 2);
    unsigned cbBase = rd16(st, 4);
    size_t p = 2 + cbStshi;
    for (unsigned i = 0; i < cstd && p + 2 <= st.size(); i++) {
        unsigned cb = rd16(st, p);
        p += 2;
        Style s;
        if (cb && p + cb <= st.size()) {
            std::string std = st.substr(p, cb);
            s.sti = rd16(std, 0) & 0x0FFF;
            unsigned stk = rd16(std, 2) & 0x000F;
            s.base = rd16(std, 2) >> 4;
            size_t q = cbBase;
            unsigned cch = rd16(std, q);
            std::wstring w;
            for (unsigned k = 0; k < cch && q + 2 + 2 * k + 2 <= std.size(); k++) w += (wchar_t)rd16(std, q + 2 + 2 * k);
            s.name = toLowerRu(wToUtf8(w));
            q += 2 + 2 * cch + 2;
            /* у стиля абзаца первым идёт UPX абзаца: cb, istd, grpprl */
            if (stk == 1 && q + 2 <= std.size()) {
                unsigned cbUpx = rd16(std, q);
                if (q + 2 + cbUpx <= std.size() && cbUpx >= 2) {
                    ParaProps pp;
                    applyPapx(std.substr(q + 4, cbUpx - 2), pp);
                    s.outline = pp.outline;
                    s.list = pp.list;
                }
            }
        }
        out.push_back(s);
        p += cb;
    }
}

/* ============================= документ ============================= */

struct Para {
    std::string html;
    ParaProps pp;
    bool cellEnd; /* абзац закончился меткой ячейки (0x07) */
    Para() : cellEnd(false) {}
};

class DocReader {
public:
    DocReader(Book& b) : book(b) {}
    bool read(const std::string& file, std::string* err);

private:
    Book& book;
    std::string wd, tbl, dataStm;
    struct Piece {
        unsigned cp0, cp1, fc;
        bool compressed;
    };
    std::vector<Piece> pieces;
    std::vector<FcRun> chpx, papx;
    std::vector<Style> styles;
    std::map<unsigned, std::string> noteAt; /* CP ссылки на сноску -> id сноски */
    int noteSeq;

    unsigned fibPair(unsigned idx, bool lcb) const;
    int styleLevel(unsigned istd, int depth) const;
    bool styleList(unsigned istd, int depth) const {
        if (istd >= styles.size() || depth > 10) return false;
        const Style& s = styles[istd];
        if (s.list || s.name.find("list") != std::string::npos || s.name.find("список") != std::string::npos) return true;
        return s.base < styles.size() && s.base != istd && styleList(s.base, depth + 1);
    }
    unsigned styleSti(unsigned istd, int depth) const;
    std::string pictureHtml(unsigned off);
    void walk(unsigned cp0, unsigned cp1, std::vector<Para>& out, bool noteText);
    void readNotes(unsigned plcRefIdx, unsigned plcTxtIdx, unsigned subdocStart, const char* prefix);
    unsigned fcRgFcLcb, cbRgFcLcb;
};

unsigned DocReader::fibPair(unsigned idx, bool lcb) const {
    if (idx * 2 + 1 >= cbRgFcLcb * 2) return 0;
    return rd32(wd, fcRgFcLcb + 8 * idx + (lcb ? 4 : 0));
}

unsigned DocReader::styleSti(unsigned istd, int depth) const {
    if (istd >= styles.size() || depth > 10) return 0x0FFE;
    const Style& s = styles[istd];
    if (s.sti != 0x0FFE) return s.sti;
    return s.base < styles.size() ? styleSti(s.base, depth + 1) : 0x0FFE;
}

/* уровень заголовка по стилю: встроенный «heading N» (sti 1–9), outline level, родитель */
int DocReader::styleLevel(unsigned istd, int depth) const {
    if (istd >= styles.size() || depth > 10) return -1;
    const Style& s = styles[istd];
    if (s.sti >= 1 && s.sti <= 9) return (int)s.sti - 1;
    if (s.outline >= 0) return s.outline;
    if (s.base < styles.size() && s.base != istd) return styleLevel(s.base, depth + 1);
    return -1;
}

/* картинка: PICF в потоке Data, за ним записи OfficeArt; PNG/JPEG берём из BLIP */
std::string DocReader::pictureHtml(unsigned off) {
    if ((size_t)off + 0x44 > dataStm.size()) return std::string();
    unsigned lcb = rd32(dataStm, off);
    unsigned cbHeader = rd16(dataStm, off + 4);
    unsigned mm = rd16(dataStm, off + 6);
    size_t end = std::min((size_t)off + lcb, dataStm.size());
    size_t p = off + cbHeader;
    if (mm == 0x0066 && p < end) p += 1 + (unsigned char)dataStm[p];
    for (int guard = 0; p + 8 <= end && guard < 64; guard++) {
        unsigned verInst = rd16(dataStm, p), type = rd16(dataStm, p + 2), len = rd32(dataStm, p + 4);
        size_t body = p + 8;
        if (body + len > end) break;
        size_t blip = 0;
        if (type == 0xF007) { /* FBSE: 36 байт заголовка, имя, затем сам BLIP */
            unsigned cbName = body + 33 < end ? (unsigned char)dataStm[body + 33] : 0;
            blip = body + 36 + cbName;
        } else if (type >= 0xF018 && type <= 0xF117) {
            blip = p;
        }
        if (blip && blip + 8 <= end) {
            unsigned bInst = rd16(dataStm, blip) >> 4, bType = rd16(dataStm, blip + 2), bLen = rd32(dataStm, blip + 4);
            int uids = 1;
            const char* mime = NULL;
            if (bType == 0xF01E) { mime = "image/png"; if (bInst == 0x6E1) uids = 2; }
            else if (bType == 0xF01D || bType == 0xF02A) { mime = "image/jpeg"; if (bInst == 0x46B || bInst == 0x6E3 || bInst == 0x6E5) uids = 2; }
            else if (bType == 0xF029) { mime = "image/tiff"; if (bInst == 0x6E5) uids = 2; }
            size_t dataStart = blip + 8 + 16 * uids + 1;
            if (mime && strcmp(mime, "image/tiff") != 0 && bLen > (unsigned)(16 * uids + 1) && dataStart + (bLen - 16 * uids - 1) <= dataStm.size()) {
                Bytes img(dataStm.begin() + dataStart, dataStm.begin() + dataStart + (bLen - 16 * uids - 1));
                std::string name = addImage(book, mime, img);
                if (!name.empty()) return "<img src=\"images/" + name + "\" alt=\"\"/>";
            }
        }
        (void)verInst;
        p = body + len;
    }
    return std::string();
}

/* Текст диапазона CP -> абзацы с HTML-разметкой и свойствами */
void DocReader::walk(unsigned cp0, unsigned cp1, std::vector<Para>& out, bool noteText) {
    std::wstring run;
    int runFmt = -1;
    std::string html;
    std::vector<int> fields; /* 0 — код поля (пропускаем), 1 — результат */
    bool skippedNoteMark = false;

    struct Flush {
        static void now(std::wstring& run, int& fmt, std::string& html) {
            if (run.empty()) return;
            std::string h = escHtml(wToUtf8(run));
            if (fmt & 8) h = "<sup>" + h + "</sup>";
            if (fmt & 16) h = "<sub>" + h + "</sub>";
            if (fmt & 4) h = "<s>" + h + "</s>";
            if (fmt & 1) h = "<strong>" + h + "</strong>";
            if (fmt & 2) h = "<em>" + h + "</em>";
            html += h;
            run.clear();
        }
    };

    for (size_t pi = 0; pi < pieces.size(); pi++) {
        const Piece& pc = pieces[pi];
        unsigned a = std::max(pc.cp0, cp0), b = std::min(pc.cp1, cp1);
        for (unsigned cp = a; cp < b; cp++) {
            unsigned fc;
            wchar_t ch;
            if (pc.compressed) {
                fc = pc.fc + (cp - pc.cp0);
                if (fc >= wd.size()) break;
                char c8 = wd[fc];
                wchar_t w = (unsigned char)c8;
                if ((unsigned char)c8 >= 0x80) MultiByteToWideChar(1252, 0, &c8, 1, &w, 1);
                ch = w;
            } else {
                fc = pc.fc + 2 * (cp - pc.cp0);
                if (fc + 2 > wd.size()) break;
                ch = (wchar_t)rd16(wd, fc);
            }
            CharProps c;
            const FcRun* cr = findRun(chpx, fc);
            if (cr) applyChpx(cr->grpprl, c);

            /* поля: 0x13 код 0x14 результат 0x15 */
            if (ch == 0x13) { fields.push_back(0); continue; }
            if (ch == 0x14) { if (!fields.empty()) fields.back() = 1; continue; }
            if (ch == 0x15) { if (!fields.empty()) fields.pop_back(); continue; }
            bool inCode = false;
            for (size_t k = 0; k < fields.size(); k++) if (fields[k] == 0) inCode = true;
            if (inCode) continue;

            if (ch == 0x0D || ch == 0x07) {
                Flush::now(run, runFmt, html);
                Para p;
                p.html = trim(collapseSpacesPreserveBr(html));
                const FcRun* pr = findRun(papx, fc);
                if (pr) {
                    p.pp.istd = pr->istd;
                    applyPapx(pr->grpprl, p.pp);
                }
                p.cellEnd = ch == 0x07;
                out.push_back(p);
                html.clear();
                continue;
            }
            if (c.hidden) continue;
            if (c.special) {
                Flush::now(run, runFmt, html);
                if (ch == 0x01 && c.hasPic) html += pictureHtml(c.picLoc);
                else if (ch == 0x02) {
                    if (noteText) { skippedNoteMark = true; continue; } /* номер в самой сноске */
                    std::map<unsigned, std::string>::iterator it = noteAt.find(cp);
                    if (it != noteAt.end()) html += "<a href=\"#" + it->second + "\">[" + it->second.substr(4) + "]</a>";
                }
                continue;
            }
            if (ch == 0x0B) { Flush::now(run, runFmt, html); html += "<br/>"; continue; }
            if (ch == 0x09) ch = L' ';
            else if (ch == 0x1E) ch = L'-';
            else if (ch == 0x1F || ch == 0x0C || ch == 0x0E) continue;
            else if (ch < 0x20) continue;
            int fmt = (c.bold ? 1 : 0) | (c.italic ? 2 : 0) | (c.strike ? 4 : 0) | (c.iss == 1 ? 8 : 0) | (c.iss == 2 ? 16 : 0);
            if (fmt != runFmt) {
                Flush::now(run, runFmt, html);
                runFmt = fmt;
            }
            run += ch;
        }
    }
    Flush::now(run, runFmt, html);
    if (!trim(html).empty()) {
        Para p;
        p.html = trim(collapseSpacesPreserveBr(html));
        out.push_back(p);
    }
    (void)skippedNoteMark;
}

void DocReader::readNotes(unsigned refIdx, unsigned txtIdx, unsigned subdoc, const char* prefix) {
    unsigned fcRef = fibPair(refIdx, false), lcbRef = fibPair(refIdx, true);
    unsigned fcTxt = fibPair(txtIdx, false), lcbTxt = fibPair(txtIdx, true);
    if (lcbRef < 6 || lcbTxt < 8 || (size_t)fcRef + lcbRef > tbl.size() || (size_t)fcTxt + lcbTxt > tbl.size()) return;
    unsigned n = (lcbRef - 4) / 6;
    unsigned nt = lcbTxt / 4 - 1;
    for (unsigned i = 0; i < n && i < nt; i++) {
        unsigned refCp = rd32(tbl, fcRef + 4 * i);
        unsigned t0 = rd32(tbl, fcTxt + 4 * i), t1 = rd32(tbl, fcTxt + 4 * (i + 1));
        Note note;
        note.id = std::string(prefix) + intToStr((long long)++noteSeq);
        note.title = intToStr((long long)noteSeq);
        std::vector<Para> ps;
        walk(subdoc + t0, subdoc + t1, ps, true);
        for (size_t k = 0; k < ps.size(); k++)
            if (!collapse(htmlToText(ps[k].html)).empty()) note.blocks.push_back(Block::p(ps[k].html));
        if (note.blocks.empty()) continue;
        noteAt[refCp] = note.id;
        book.notes.push_back(note);
    }
}

bool DocReader::read(const std::string& file, std::string* err) {
    Cfb cfb(file);
    if (!cfb.open()) {
        if (err) *err = "Повреждённый DOC: не удалось разобрать контейнер";
        return false;
    }
    if (!cfb.stream("WordDocument", wd) || wd.size() < 0x200) {
        if (err) *err = "В файле нет потока WordDocument — это не документ Word";
        return false;
    }
    if (rd16(wd, 0) != 0xA5EC) {
        if (err) *err = "Неизвестный формат Word";
        return false;
    }
    unsigned nFib = rd16(wd, 2);
    unsigned flags = rd16(wd, 0x0A);
    if (nFib < 101 || (nFib >= 101 && nFib <= 105)) {
        if (err) *err = "Это Word 6/95 — такие DOC пока не читаются. Пересохраните файл в Word или LibreOffice";
        return false;
    }
    if (flags & 0x0100) {
        if (err) *err = "DOC защищён паролем — сначала снимите пароль";
        return false;
    }
    cfb.stream((flags & 0x0200) ? "1Table" : "0Table", tbl);
    cfb.stream("Data", dataStm);

    /* FIB: FibBase(32) csw rgW cslw rgLw cbRgFcLcb rgFcLcb */
    size_t p = 32;
    unsigned csw = rd16(wd, p);
    p += 2 + 2 * csw;
    unsigned cslw = rd16(wd, p);
    size_t rgLw = p + 2;
    p += 2 + 4 * cslw;
    cbRgFcLcb = rd16(wd, p);
    fcRgFcLcb = (unsigned)(p + 2);
    unsigned ccpText = rd32(wd, rgLw + 4 * 3), ccpFtn = rd32(wd, rgLw + 4 * 4), ccpHdd = rd32(wd, rgLw + 4 * 5);
    unsigned ccpMcr = rd32(wd, rgLw + 4 * 6), ccpAtn = rd32(wd, rgLw + 4 * 7);

    /* таблица кусков (Clx -> Pcdt -> PlcPcd) */
    unsigned fcClx = fibPair(33, false), lcbClx = fibPair(33, true);
    if ((size_t)fcClx + lcbClx > tbl.size() || lcbClx == 0) {
        if (err) *err = "Повреждённый DOC: нет таблицы текста";
        return false;
    }
    std::string clx = tbl.substr(fcClx, lcbClx);
    size_t q = 0;
    while (q < clx.size() && clx[q] == 0x01) q += 3 + rd16(clx, q + 1); /* Prc — пропускаем */
    if (q + 5 > clx.size() || clx[q] != 0x02) {
        if (err) *err = "Повреждённый DOC: нет таблицы кусков";
        return false;
    }
    unsigned lcbPcd = rd32(clx, q + 1);
    std::string plc = clx.substr(q + 5, lcbPcd);
    unsigned np = lcbPcd >= 4 ? (lcbPcd - 4) / 12 : 0;
    for (unsigned i = 0; i < np; i++) {
        Piece pc;
        pc.cp0 = rd32(plc, 4 * i);
        pc.cp1 = rd32(plc, 4 * (i + 1));
        unsigned fcc = rd32(plc, 4 * (np + 1) + 8 * i + 2);
        pc.compressed = (fcc & 0x40000000) != 0;
        pc.fc = pc.compressed ? (fcc & 0x3FFFFFFF) / 2 : fcc & 0x3FFFFFFF;
        if (pc.cp1 > pc.cp0) pieces.push_back(pc);
    }

    readFkps(wd, tbl, fibPair(12, false), fibPair(12, true), false, chpx);
    readFkps(wd, tbl, fibPair(13, false), fibPair(13, true), true, papx);
    readStyles(tbl, fibPair(1, false), fibPair(1, true), styles);

    /* сноски и концевые сноски — до основного текста, чтобы ссылки знали id */
    noteSeq = 0;
    readNotes(2, 3, ccpText, "note");
    readNotes(46, 47, ccpText + ccpFtn + ccpHdd + ccpMcr + ccpAtn, "note");

    std::vector<Para> paras;
    walk(0, ccpText, paras, false);

    /* абзацы -> главы, таблицы, списки */
    std::vector<Chapter> chapters(1);
    struct Row {
        std::vector<std::string> cells;
        std::vector<int> edges; /* границы ячеек строки из sprmTDefTable */
    };
    std::vector<Row> rows;
    std::vector<std::string> cells;
    std::string cell;
    struct Table {
        /* Объединённая по горизонтали ячейка в DOC — просто одна широкая ячейка: colspan
           считается по общей сетке границ всех строк таблицы */
        static void flush(std::vector<Row>& rows, std::vector<std::string>& cells, std::string& cell,
                          std::vector<Chapter>& ch) {
            if (!cell.empty()) { cells.push_back(cell); cell.clear(); }
            if (!cells.empty()) { Row r; r.cells = cells; rows.push_back(r); cells.clear(); }
            if (rows.empty()) return;
            std::vector<int> grid;
            for (size_t r = 0; r < rows.size(); r++)
                for (size_t k = 0; k < rows[r].edges.size(); k++) grid.push_back(rows[r].edges[k]);
            std::sort(grid.begin(), grid.end());
            std::vector<int> uniq;
            for (size_t k = 0; k < grid.size(); k++)
                if (uniq.empty() || grid[k] - uniq.back() > 30) uniq.push_back(grid[k]);
            std::string html = "<table>";
            for (size_t r = 0; r < rows.size(); r++) {
                html += "<tr>";
                const std::vector<int>& e = rows[r].edges;
                for (size_t c = 0; c < rows[r].cells.size(); c++) {
                    int span = 1;
                    if (e.size() == rows[r].cells.size() + 1) {
                        span = 0;
                        for (size_t k = 0; k + 1 < uniq.size(); k++)
                            if (uniq[k] >= e[c] - 30 && uniq[k + 1] <= e[c + 1] + 30) span++;
                        if (span < 1) span = 1;
                    }
                    html += span > 1 ? "<td colspan=\"" + intToStr(span) + "\">" : std::string("<td>");
                    html += rows[r].cells[c] + "</td>";
                }
                html += "</tr>";
            }
            rows.clear();
            Block b(Block::RAW);
            b.html = html + "</table>";
            ch.back().blocks.push_back(b);
        }
    };
    for (size_t i = 0; i < paras.size(); i++) {
        const Para& pa = paras[i];
        if (pa.pp.inTable) {
            if (pa.pp.ttp) { /* метка конца строки таблицы */
                if (!cell.empty()) { cells.push_back(cell); cell.clear(); }
                Row row;
                row.cells = cells;
                row.edges = pa.pp.cellEdges;
                rows.push_back(row);
                cells.clear();
                continue;
            }
            if (!pa.html.empty()) cell += (cell.empty() ? "" : "<br/>") + pa.html;
            if (pa.cellEnd) { cells.push_back(cell); cell.clear(); }
            continue;
        }
        Table::flush(rows, cells, cell, chapters);

        unsigned sti = styleSti(pa.pp.istd, 0);
        if (sti >= 19 && sti <= 27) continue; /* строки оглавления Word (toc 1–9) */
        std::string plain = collapse(htmlToText(pa.html));
        std::string sname = pa.pp.istd < styles.size() ? styles[pa.pp.istd].name : std::string();
        if ((sti == 62 || sname == "title" || sname == "название" || sname == "заголовок") && !plain.empty()) {
            if (book.meta.title.empty()) book.meta.title = plain;
            continue;
        }
        int level = pa.pp.outline >= 0 ? pa.pp.outline : styleLevel(pa.pp.istd, 0);
        std::vector<Block>& blocks = chapters.back().blocks;
        if (level >= 0 && level <= 8 && !plain.empty()) {
            Chapter c;
            c.level = level;
            c.title = pa.html;
            chapters.push_back(c);
            continue;
        }
        if (pa.html.empty()) {
            if (!blocks.empty() && blocks.back().type != Block::EMPTY) blocks.push_back(Block::empty());
            continue;
        }
        if (plain.empty() && pa.html.find("<img") != std::string::npos) {
            size_t pos = 0;
            while ((pos = pa.html.find("src=\"images/", pos)) != std::string::npos) {
                size_t e = pa.html.find('"', pos + 12);
                if (e == std::string::npos) break;
                Block b(Block::IMG);
                b.src = "images/" + pa.html.substr(pos + 12, e - pos - 12);
                blocks.push_back(b);
                pos = e;
            }
            continue;
        }
        Block b(pa.pp.list || styleList(pa.pp.istd, 0) ? Block::LI : Block::P);
        b.html = pa.html;
        blocks.push_back(b);
    }
    Table::flush(rows, cells, cell, chapters);

    /* свойства документа: \005SummaryInformation */
    std::string si;
    if (cfb.stream("\x05SummaryInformation", si) && si.size() > 48) {
        unsigned off = rd32(si, 44);
        unsigned nProps = rd32(si, off + 4);
        int cp = 1252;
        std::map<unsigned, unsigned> where;
        for (unsigned i = 0; i < nProps && i < 200; i++) where[rd32(si, off + 8 + 8 * i)] = rd32(si, off + 12 + 8 * i);
        if (where.count(1)) cp = (int)rd16(si, off + where[1] + 4);
        struct Prop {
            static std::string str(const std::string& si, size_t at, int cp) {
                unsigned type = rd32(si, at), cch = rd32(si, at + 4);
                if (cch > 4096 || at + 8 + cch > si.size()) return std::string();
                if (type == 0x1F) {
                    std::wstring w;
                    for (unsigned k = 0; k < cch && at + 8 + 2 * k + 2 <= si.size(); k++) w += (wchar_t)rd16(si, at + 8 + 2 * k);
                    return collapse(wToUtf8(w));
                }
                if (type != 0x1E) return std::string();
                std::string b = si.substr(at + 8, cch);
                while (!b.empty() && b[b.size() - 1] == 0) b.erase(b.size() - 1);
                if (b.empty()) return b;
                int n = MultiByteToWideChar((UINT)(cp == 1200 ? 65001 : cp), 0, b.data(), (int)b.size(), NULL, 0);
                if (n <= 0) return collapse(b);
                std::wstring w((size_t)n, L'\0');
                MultiByteToWideChar((UINT)(cp == 1200 ? 65001 : cp), 0, b.data(), (int)b.size(), &w[0], n);
                return collapse(wToUtf8(w));
            }
        };
        if (where.count(2)) {
            std::string t = Prop::str(si, off + where[2], cp);
            if (!t.empty()) book.meta.title = t;
        }
        if (where.count(4)) {
            std::string a = Prop::str(si, off + where[4], cp);
            if (!a.empty()) book.meta.authors.push_back(splitAuthor(a, false));
        }
    }

    for (size_t i = 0; i < chapters.size(); i++) {
        bool has = !chapters[i].title.empty();
        for (size_t k = 0; k < chapters[i].blocks.size() && !has; k++)
            if (chapters[i].blocks[k].type != Block::EMPTY) has = true;
        if (has) book.chapters.push_back(chapters[i]);
    }
    normalizeChapterLevels(book.chapters);
    if (book.chapters.empty()) {
        if (err) *err = "В DOC не нашлось текста";
        return false;
    }
    return true;
}

} /* namespace */

bool readDOC(const Bytes& data, const std::string& fileName, Book& book, std::string* err) {
    std::string s(data.begin(), data.end());
    book.fileName = fileName;
    DocReader r(book);
    return r.read(s, err);
}
