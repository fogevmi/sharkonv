/* Чтение DjVu с текстовым слоем (книги, распознанные FineReader/OCR и сохранённые в DjVu).
   Контейнер IFF: одностраничный FORM:DJVU, многостраничный FORM:DJVM со всеми страницами в
   одном файле (bundled) или каталогом, ссылающимся на файлы страниц рядом (indirect).
   Текст (TXTz/TXTa), оглавление (NAVM), каталог (DIRM) и аннотации сжаты BZZ: свой декодер
   ZP-кодера и обратного преобразования Барроуза–Уилера по алгоритму формата DjVu.
   Изображения страниц (JB2, IW44) не декодируются: книга получается текстовой. */
#include "convert.h"
#include "djvu_ztable.h"
#include <set>

namespace {

/* ============================ ZP-декодер ============================= */

class Zp {
public:
    Zp(const std::string& src, size_t start) : s(src), pos(start), a(0), code(0), fence(0), buffer(0),
                                               scount(0), delay(25), broken(false) {
        for (int i = 0; i < 256; i++) {
            ffzt[i] = 0;
            for (int j = i; j & 0x80; j <<= 1) ffzt[i]++;
        }
        code = (unsigned)readByte() << 8;
        code |= (unsigned)readByte();
        preload();
        fence = code >= 0x8000 ? 0x7fff : code;
    }

    /* бит с адаптивным контекстом */
    int bit(unsigned char& ctx) {
        unsigned z = a + kZpTable[ctx].p;
        if (z <= fence) { a = z; return ctx & 1; }
        return sub(ctx, z);
    }

    /* бит без контекста (вероятность 1/2) */
    int raw() {
        unsigned z = 0x8000 + (a >> 1);
        if (z > code) {
            lps(z);
            return 1;
        }
        mps(z);
        return 0;
    }

    bool failed() const { return broken; }

private:
    const std::string& s;
    size_t pos;
    unsigned a, code, fence, buffer;
    int scount, delay;
    bool broken; /* данные кончились раньше времени */
    unsigned char ffzt[256];

    int readByte() {
        if (pos < s.size()) return (unsigned char)s[pos++];
        if (--delay < 1) broken = true;
        return 0xff;
    }
    void preload() {
        while (scount <= 24) {
            buffer = (buffer << 8) | (unsigned)readByte();
            scount += 8;
        }
    }
    int ffz(unsigned x) const { return x >= 0xff00 ? ffzt[x & 0xff] + 8 : ffzt[(x >> 8) & 0xff]; }
    void lps(unsigned z) {
        z = 0x10000 - z;
        a += z;
        code += z;
        int shift = ffz(a);
        scount -= shift;
        a = (a << shift) & 0xffff;
        code = ((code << shift) & 0xffff) | ((buffer >> scount) & ((1u << shift) - 1));
        if (scount < 16) preload();
        fence = code >= 0x8000 ? 0x7fff : code;
    }
    void mps(unsigned z) {
        scount -= 1;
        a = (z << 1) & 0xffff;
        code = ((code << 1) & 0xffff) | ((buffer >> scount) & 1);
        if (scount < 16) preload();
        fence = code >= 0x8000 ? 0x7fff : code;
    }
    int sub(unsigned char& ctx, unsigned z) {
        int b = ctx & 1;
        unsigned d = 0x6000 + ((z + a) >> 2);
        if (z > d) z = d;
        if (z > code) {
            ctx = kZpTable[ctx].dn;
            lps(z);
            return b ^ 1;
        }
        if (a >= kZpTable[ctx].m) ctx = kZpTable[ctx].up;
        mps(z);
        return b;
    }
};

/* ============================ BZZ ============================ */

int bzzBinary(Zp& zp, unsigned char* ctx, int bits) {
    int n = 1, m = 1 << bits;
    ctx = ctx - 1;
    while (n < m) n = (n << 1) | zp.bit(ctx[n]);
    return n - m;
}

/* Распаковка BZZ: блоки «длина (24 бита) + MTF-коды в контекстах ZP + обратный BWT» */
bool bzzDecode(const std::string& in, size_t start, std::string& out, size_t limit) {
    out.clear();
    if (start >= in.size()) return false;
    Zp zp(in, start);
    unsigned char ctx[300];
    memset(ctx, 0, sizeof(ctx));
    const int FREQMAX = 4, CTXIDS = 3;
    std::vector<unsigned char> data;
    std::vector<unsigned> posn;
    for (int block = 0; block < 100000; block++) {
        int size = 0;
        for (int n = 1; n < (1 << 24);) { n = (n << 1) | zp.raw(); if (n >= (1 << 24)) size = n - (1 << 24); }
        if (zp.failed()) return !out.empty();
        if (size == 0) return true;
        if (size > 4096 * 1024 || out.size() + size > limit) return false;
        data.assign((size_t)size, 0);
        int fshift = 0;
        if (zp.raw()) { fshift++; if (zp.raw()) fshift++; }
        unsigned char mtf[256];
        for (int i = 0; i < 256; i++) mtf[i] = (unsigned char)i;
        unsigned freq[FREQMAX];
        memset(freq, 0, sizeof(freq));
        unsigned fadd = 4;
        int mtfno = 3, markerpos = -1;
        for (int i = 0; i < size; i++) {
            int ctxid = CTXIDS - 1;
            if (ctxid > mtfno) ctxid = mtfno;
            unsigned char* cx = ctx;
            if (zp.bit(cx[ctxid])) mtfno = 0;
            else if (zp.bit((cx += CTXIDS)[ctxid])) mtfno = 1;
            else if (zp.bit((cx += CTXIDS)[0])) mtfno = 2 + bzzBinary(zp, cx + 1, 1);
            else if (zp.bit((cx += 1 + 1)[0])) mtfno = 4 + bzzBinary(zp, cx + 1, 2);
            else if (zp.bit((cx += 1 + 3)[0])) mtfno = 8 + bzzBinary(zp, cx + 1, 3);
            else if (zp.bit((cx += 1 + 7)[0])) mtfno = 16 + bzzBinary(zp, cx + 1, 4);
            else if (zp.bit((cx += 1 + 15)[0])) mtfno = 32 + bzzBinary(zp, cx + 1, 5);
            else if (zp.bit((cx += 1 + 31)[0])) mtfno = 64 + bzzBinary(zp, cx + 1, 6);
            else if (zp.bit((cx += 1 + 63)[0])) mtfno = 128 + bzzBinary(zp, cx + 1, 7);
            else {
                mtfno = 256; /* маркер конца строки BWT */
                data[(size_t)i] = 0;
                markerpos = i;
                continue;
            }
            data[(size_t)i] = mtf[mtfno];
            /* MTF с поправкой на частоты */
            fadd = fadd + (fadd >> fshift);
            if (fadd > 0x10000000) {
                fadd >>= 24;
                for (int k = 0; k < FREQMAX; k++) freq[k] >>= 24;
            }
            unsigned fc = fadd;
            if (mtfno < FREQMAX) fc += freq[mtfno];
            int k;
            for (k = mtfno; k >= FREQMAX; k--) mtf[k] = mtf[k - 1];
            for (; k > 0 && fc >= freq[k - 1]; k--) {
                mtf[k] = mtf[k - 1];
                freq[k] = freq[k - 1];
            }
            mtf[k] = data[(size_t)i];
            freq[k] = fc;
        }
        if (zp.failed() || markerpos < 1 || markerpos >= size) return !out.empty();
        /* обратное преобразование Барроуза–Уилера */
        posn.assign((size_t)size, 0);
        int count[256];
        memset(count, 0, sizeof(count));
        for (int i = 0; i < size; i++) {
            if (i == markerpos) continue;
            unsigned char c = data[(size_t)i];
            posn[(size_t)i] = ((unsigned)c << 24) | ((unsigned)count[c] & 0xffffff);
            count[c]++;
        }
        int last = 1;
        for (int i = 0; i < 256; i++) {
            int t = count[i];
            count[i] = last;
            last += t;
        }
        std::vector<unsigned char> outb((size_t)size, 0);
        int i = 0;
        last = size - 1;
        int guard = 0;
        while (last > 0 && guard++ <= size) {
            unsigned n = posn[(size_t)i];
            unsigned char c = (unsigned char)(n >> 24);
            outb[(size_t)--last] = c;
            i = count[c] + (int)(n & 0xffffff);
            if (i < 0 || i >= size) return !out.empty();
        }
        if (i != markerpos) return !out.empty();
        out.append((const char*)&outb[0], (size_t)size - 1);
    }
    return true;
}

/* ============================ IFF ============================ */

unsigned be16(const std::string& s, size_t p) { return p + 2 <= s.size() ? ((unsigned)(unsigned char)s[p] << 8) | (unsigned char)s[p + 1] : 0; }
unsigned be24(const std::string& s, size_t p) { return p + 3 <= s.size() ? ((unsigned)(unsigned char)s[p] << 16) | be16(s, p + 1) : 0; }
unsigned be32(const std::string& s, size_t p) { return p + 4 <= s.size() ? ((unsigned)(unsigned char)s[p] << 24) | be24(s, p + 1) : 0; }

struct Chunk {
    std::string id;
    size_t off, size; /* данные чанка */
};

/* чанки внутри FORM (данные FORM начинаются с 4-байтного типа) */
std::vector<Chunk> chunksOf(const std::string& f, size_t start, size_t end) {
    std::vector<Chunk> out;
    size_t p = start;
    while (p + 8 <= end && p + 8 <= f.size() && out.size() < 100000) {
        Chunk c;
        c.id = f.substr(p, 4);
        c.size = be32(f, p + 4);
        c.off = p + 8;
        if (c.off + c.size > end || c.off + c.size > f.size()) c.size = std::min(end, f.size()) - c.off;
        out.push_back(c);
        p = c.off + c.size + (c.size & 1);
    }
    return out;
}

/* FORM в позиции p: тип формы и её чанки */
bool formAt(const std::string& f, size_t p, std::string& type, std::vector<Chunk>& chunks) {
    if (p + 12 > f.size() || f.compare(p, 4, "FORM") != 0) return false;
    size_t size = be32(f, p + 4);
    size_t end = std::min(f.size(), p + 8 + size);
    type = f.substr(p + 8, 4);
    chunks = chunksOf(f, p + 12, end);
    return true;
}

/* ============================ текстовый слой ============================ */

struct Rect { int x0, y0, x1, y1; };

struct Zone {
    int type; /* 1 страница, 2 колонка, 3 область, 4 абзац, 5 строка, 6 слово, 7 символ */
    Rect r;
    int start, len;
};

struct TxtReader {
    const std::string& s;
    size_t p;
    int maxtext;
    std::vector<Zone> lines, paras;
    Rect page;
    bool bad;
    TxtReader(const std::string& src, size_t at, int mt) : s(src), p(at), maxtext(mt), bad(false) {
        page.x0 = page.y0 = 0; page.x1 = page.y1 = 0;
    }

    void zone(const Zone* parent, const Zone* prev, Zone& z, int depth) {
        if (bad || depth > 12 || p + 15 > s.size()) { bad = true; return; }
        z.type = (unsigned char)s[p];
        int x = (int)be16(s, p + 1) - 0x8000, y = (int)be16(s, p + 3) - 0x8000;
        int w = (int)be16(s, p + 5) - 0x8000, h = (int)be16(s, p + 7) - 0x8000;
        z.start = (int)be16(s, p + 9) - 0x8000;
        z.len = (int)be24(s, p + 11);
        p += 14;
        if (z.type < 1 || z.type > 7 || (parent && z.type <= parent->type)) { bad = true; return; }
        if (prev) {
            if (z.type == 1 || z.type == 4 || z.type == 5) {
                x += prev->r.x0;
                y = prev->r.y0 - (y + h);
            } else {
                x += prev->r.x1;
                y += prev->r.y0;
            }
            z.start += prev->start + prev->len;
        } else if (parent) {
            x += parent->r.x0;
            y = parent->r.y1 - (y + h);
            z.start += parent->start;
        }
        z.r.x0 = x; z.r.y0 = y; z.r.x1 = x + w; z.r.y1 = y + h;
        if (p + 3 > s.size()) { bad = true; return; }
        int n = (int)be24(s, p);
        p += 3;
        if (z.start < 0 || z.start + z.len > maxtext || n > 1000000) { bad = true; return; }
        if (z.type == 1) page = z.r;
        if (z.type == 4) paras.push_back(z);
        if (z.type == 5) lines.push_back(z);
        Zone prevChild;
        bool have = false;
        for (int i = 0; i < n && !bad; i++) {
            Zone c;
            zone(&z, have ? &prevChild : NULL, c, depth + 1);
            prevChild = c;
            have = true;
        }
    }
};

struct DLine {
    int page;
    Rect r;
    std::string text;
    int para;     /* номер зоны-абзаца на странице, -1 — нет */
    int heading;  /* уровень главы, -1 — обычная строка */
    bool drop;
    bool geom;    /* настоящие координаты; без зон x1 — длина строки в символах */
    int col;      /* колонка на странице: 0 — левая (или единственная), 1 — правая */
    DLine() : page(0), para(-1), heading(-1), drop(false), geom(true), col(0) {}
};

std::string cleanText(const std::string& t) {
    /* в слоях после OCR бывают обрывки UTF-8, NUL и прочие управляющие символы —
       XML их не допускает: управляющие становятся пробелом, битые байты и знак замены U+FFFD выпадают */
    std::string out;
    out.reserve(t.size());
    for (size_t i = 0; i < t.size();) {
        unsigned char c = (unsigned char)t[i];
        if (c < 0x80) {
            out += (c < 0x20 || c == 0x7F) ? ' ' : (char)c;
            i++;
            continue;
        }
        int n = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 ? (c <= 0xEF ? 2 : 0) : c >= 0xC2 ? 1 : 0;
        bool ok = n > 0;
        unsigned cp = n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; ok && k <= n; k++) {
            if (i + k >= t.size() || ((unsigned char)t[i + k] & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | ((unsigned char)t[i + k] & 0x3F);
        }
        if (ok && ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
                   (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0xFFFD && cp <= 0xFFFF) ||(cp >= 0x80 && cp < 0xA0)))
            ok = false;
        if (!ok) { i++; continue; }
        out.append(t, i, (size_t)n + 1);
        i += (size_t)n + 1;
    }
    return collapse(out);
}

/* одна страница: строки текстового слоя с координатами (верх страницы — большие y) */
void pageLines(const std::string& txt, int pageNo, std::vector<DLine>& out, int& pageHeight) {
    if (txt.size() < 3) return;
    int textsize = (int)be24(txt, 0);
    if (textsize <= 0 || 3 + (size_t)textsize > txt.size()) return;
    std::string text = txt.substr(3, (size_t)textsize);
    size_t p = 3 + (size_t)textsize;
    std::vector<DLine> lines;
    if (p < txt.size() && txt[p] == 1) {
        TxtReader tr(txt, p + 1, textsize);
        Zone root;
        tr.zone(NULL, NULL, root, 0);
        if (tr.page.y1 > 0) pageHeight = tr.page.y1;
        for (size_t i = 0; i < tr.lines.size(); i++) {
            const Zone& z = tr.lines[i];
            DLine l;
            l.page = pageNo;
            l.r = z.r;
            l.text = cleanText(text.substr((size_t)z.start, (size_t)z.len));
            for (size_t k = 0; k < tr.paras.size(); k++)
                if (z.start >= tr.paras[k].start && z.start < tr.paras[k].start + tr.paras[k].len) l.para = (int)k;
            if (!l.text.empty()) lines.push_back(l);
        }
    }
    if (lines.empty()) {
        /* зон нет — строки по переводам строк; абзацы — по разделителю абзацев DjVu,
           а если его нет — потом по длине строк. Координаты условные: строка за строкой
           сверху вниз, ширина — число символов */
        bool seps = text.find_first_of("\x1f\x0b\x1d") != std::string::npos;
        size_t a = 0;
        int para = 0;
        while (a <= text.size()) {
            size_t e = text.find_first_of("\n\x1f\x0b\x1d", a);
            if (e == std::string::npos) e = text.size();
            DLine l;
            l.page = pageNo;
            l.geom = false;
            l.text = cleanText(text.substr(a, e - a));
            l.r.x0 = 0;
            l.r.x1 = 0;
            for (size_t k = 0; k < l.text.size(); k++)
                if (((unsigned char)l.text[k] & 0xC0) != 0x80) l.r.x1++;
            l.para = seps ? para : -1;
            if (!l.text.empty()) lines.push_back(l);
            if (e < text.size() && text[e] != '\n') para++;
            if (e >= text.size()) break;
            a = e + 1;
        }
        int h = (int)lines.size() * 12;
        for (size_t i = 0; i < lines.size(); i++) {
            lines[i].r.y1 = h - (int)i * 12;
            lines[i].r.y0 = lines[i].r.y1 - 10;
        }
        if (h > 0) pageHeight = h;
    }
    /* порядок зон в дереве (колонка -> область -> абзац -> строка) и есть порядок чтения */
    out.insert(out.end(), lines.begin(), lines.end());
}

/* (metadata (title "…") (author "…")) из аннотаций */
void readMetadata(const std::string& ant, Book& book) {
    size_t m = ant.find("(metadata");
    if (m == std::string::npos) return;
    const char* keys[] = {"title", "author"};
    for (int k = 0; k < 2; k++) {
        size_t p = ant.find(std::string("(") + keys[k], m);
        if (p == std::string::npos) continue;
        size_t q = ant.find('"', p);
        if (q == std::string::npos) continue;
        std::string v;
        for (size_t i = q + 1; i < ant.size() && ant[i] != '"'; i++) {
            if (ant[i] == '\\' && i + 1 < ant.size()) { v += ant[++i]; continue; }
            v += ant[i];
        }
        v = cleanText(v);
        if (v.empty()) continue;
        if (k == 0 && book.meta.title.empty()) book.meta.title = v;
        if (k == 1 && book.meta.authors.empty()) book.meta.authors.push_back(splitAuthor(v, false));
    }
}

struct Bookmark {
    std::string title, url;
    int level;
};

std::string normKey(const std::string& s) {
    std::string low = toLowerRu(collapse(s)), out;
    for (size_t i = 0; i < low.size(); i++) {
        unsigned char c = (unsigned char)low[i];
        if (c >= 0x80 || isalnum(c)) out += (char)c;
    }
    return out;
}

bool endsSentence(const std::string& t) {
    if (t.empty()) return false;
    char c = t[t.size() - 1];
    return c == '.' || c == '!' || c == '?' || c == ':' || c == ';' || c == '"' || c == ')' ||
           (t.size() >= 2 && (t.compare(t.size() - 2, 2, "\xC2\xBB") == 0)) ||
           (t.size() >= 3 && t.compare(t.size() - 3, 3, "\xE2\x80\xA6") == 0);
}

bool startsLower(const std::string& t) {
    unsigned cp = utf8CharAt(t, 0);
    return (cp >= 'a' && cp <= 'z') || (cp >= 0x430 && cp <= 0x44F) || cp == 0x451;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); i++) if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

/* ============================ документ ============================ */

class DjvuReader {
public:
    DjvuReader(Book& b, const std::string& path) : book(b), filePath(path) {}
    bool read(const std::string& f, std::string* err);

private:
    Book& book;
    std::string filePath;
    struct Component {
        std::string id, name, title;
        int type; /* 0 include, 1 page, 2 thumbnails, 3 shared anno */
        size_t offset;
    };
    std::vector<Component> comps;
    std::vector<std::string> extFiles; /* indirect: содержимое файлов страниц */

    bool componentData(size_t idx, const std::string& f, std::string& data, size_t& at);
    void textOfForm(const std::string& f, const std::vector<Chunk>& ch, std::string& txt, int depth);
};

/* текст страницы: TXTz/TXTa в самой форме или во включённом (INCL) файле */
void DjvuReader::textOfForm(const std::string& f, const std::vector<Chunk>& ch, std::string& txt, int depth) {
    for (size_t i = 0; i < ch.size() && txt.empty(); i++) {
        if (ch[i].id == "TXTa") txt = f.substr(ch[i].off, ch[i].size);
        else if (ch[i].id == "TXTz") bzzDecode(f.substr(ch[i].off, ch[i].size), 0, txt, 64u * 1024 * 1024);
        else if (ch[i].id == "ANTa") readMetadata(f.substr(ch[i].off, ch[i].size), book);
        else if (ch[i].id == "ANTz" || ch[i].id == "METz") {
            std::string a;
            bzzDecode(f.substr(ch[i].off, ch[i].size), 0, a, 8u * 1024 * 1024);
            readMetadata(a, book);
        } else if (ch[i].id == "METa") readMetadata(f.substr(ch[i].off, ch[i].size), book);
    }
    if (!txt.empty() || depth > 3) return;
    for (size_t i = 0; i < ch.size() && txt.empty(); i++) {
        if (ch[i].id != "INCL") continue;
        std::string id = trim(f.substr(ch[i].off, ch[i].size));
        for (size_t k = 0; k < comps.size(); k++) {
            if (comps[k].id != id) continue;
            std::string data;
            size_t at;
            std::string type;
            std::vector<Chunk> sub;
            if (componentData(k, f, data, at) && formAt(data, at, type, sub)) textOfForm(data, sub, txt, depth + 1);
            break;
        }
    }
}

bool DjvuReader::componentData(size_t idx, const std::string& f, std::string& data, size_t& at) {
    const Component& c = comps[idx];
    if (c.offset) {
        data = f;
        at = c.offset;
        if (at + 12 > f.size()) return false;  /* смещение из DIRM битого файла — за его концом */
        if (f.compare(at, 4, "FORM") != 0 && at >= 4 && f.compare(at - 4, 4, "FORM") == 0) at -= 4;
        return f.compare(at, 4, "FORM") == 0;
    }
    /* indirect: файл компонента рядом с файлом-оглавлением */
    std::string dir = filePath;
    size_t sl = dir.find_last_of("\\/");
    dir = sl == std::string::npos ? std::string() : dir.substr(0, sl + 1);
    std::string path = dir + (c.name.empty() ? c.id : c.name);
    if (c.name.find("..") != std::string::npos || c.name.find_first_of("\\/:") != std::string::npos) return false;
    Bytes b;
    std::string e;
    if (!readFileBytes(utf8ToW(path), b, &e) || b.size() < 16) return false;
    data.assign(b.begin(), b.end());
    at = data.compare(0, 4, "AT&T") == 0 ? 4 : 0;
    return data.compare(at, 4, "FORM") == 0;
}

bool DjvuReader::read(const std::string& f, std::string* err) {
    size_t at = f.compare(0, 4, "AT&T") == 0 ? 4 : 0;
    std::string type;
    std::vector<Chunk> top;
    if (!formAt(f, at, type, top)) {
        if (err) *err = "Повреждённый DjVu: нет формы FORM";
        return false;
    }

    std::vector<std::string> pageTexts;
    std::vector<int> pageHeights;
    std::vector<Bookmark> marks;
    std::map<std::string, int> pageById;

    if (type == "DJVU") {
        std::string txt;
        textOfForm(f, top, txt, 0);
        pageTexts.push_back(txt);
    } else if (type == "DJVM") {
        for (size_t i = 0; i < top.size(); i++) {
            if (top[i].id == "DIRM") {
                const std::string d = f.substr(top[i].off, top[i].size);
                unsigned ver = d.empty() ? 0 : (unsigned char)d[0];
                bool bundled = (ver & 0x80) != 0;
                ver &= 0x7f;
                unsigned n = be16(d, 1);
                size_t p = 3;
                comps.resize(n);
                for (unsigned k = 0; k < n; k++) {
                    comps[k].offset = 0;
                    comps[k].type = 1;
                    if (bundled) {
                        comps[k].offset = be32(d, p);
                        p += 4;
                        if (ver == 0) p += 3;
                    }
                }
                std::string z;
                bzzDecode(d, p, z, 16u * 1024 * 1024);
                size_t q = 0;
                if (ver > 0) q += 3 * (size_t)n; /* размеры */
                std::vector<unsigned> flags(n, 0);
                for (unsigned k = 0; k < n && q < z.size(); k++) flags[k] = (unsigned char)z[q++];
                for (unsigned k = 0; k < n; k++) {
                    unsigned fl = flags[k];
                    if (ver == 0) {
                        unsigned f1 = (fl & 1) ? 1u : 0u;
                        if (fl & 2) f1 |= 0x80;
                        if (fl & 4) f1 |= 0x40;
                        fl = f1;
                    }
                    comps[k].type = (int)(fl & 0x3f);
                    size_t e = z.find('\0', q);
                    if (e == std::string::npos) break;
                    comps[k].id = z.substr(q, e - q);
                    q = e + 1;
                    comps[k].name = comps[k].id;
                    if (fl & 0x80) { e = z.find('\0', q); if (e == std::string::npos) break; comps[k].name = z.substr(q, e - q); q = e + 1; }
                    if (fl & 0x40) { e = z.find('\0', q); if (e == std::string::npos) break; comps[k].title = z.substr(q, e - q); q = e + 1; }
                }
            } else if (top[i].id == "NAVM") {
                std::string z;
                bzzDecode(f.substr(top[i].off, top[i].size), 0, z, 16u * 1024 * 1024);
                unsigned n = be16(z, 0);
                size_t p = 2;
                std::vector<int> left; /* сколько детей осталось у предков */
                for (unsigned k = 0; k < n && p + 4 <= z.size(); k++) {
                    unsigned count = (unsigned char)z[p] | ((unsigned)(unsigned char)z[p + 1] << 8);
                    unsigned tl = be16(z, p + 2);
                    p += 4;
                    Bookmark b;
                    b.title = cleanText(z.substr(p, std::min((size_t)tl, z.size() - p)));
                    p += tl;
                    unsigned ul = be24(z, p);
                    p += 3;
                    b.url = p < z.size() ? z.substr(p, std::min((size_t)ul, z.size() - p)) : std::string();
                    p += ul;
                    while (!left.empty() && left.back() == 0) left.pop_back();
                    b.level = (int)left.size();
                    if (!left.empty()) left.back()--;
                    if (count) left.push_back((int)count);
                    marks.push_back(b);
                }
            } else if (top[i].id == "ANTa" || top[i].id == "METa") {
                readMetadata(f.substr(top[i].off, top[i].size), book);
            } else if (top[i].id == "ANTz" || top[i].id == "METz") {
                std::string a;
                bzzDecode(f.substr(top[i].off, top[i].size), 0, a, 8u * 1024 * 1024);
                readMetadata(a, book);
            }
        }
        for (size_t k = 0; k < comps.size(); k++) {
            std::string data, t;
            size_t pos;
            std::vector<Chunk> ch;
            if (comps[k].type == 3 && componentData(k, f, data, pos) && formAt(data, pos, t, ch)) {
                std::string dummy;
                textOfForm(data, ch, dummy, 0); /* общие аннотации — метаданные */
            }
            if (comps[k].type != 1) continue;
            std::string txt;
            if (componentData(k, f, data, pos) && formAt(data, pos, t, ch)) textOfForm(data, ch, txt, 0);
            pageById[comps[k].id] = (int)pageTexts.size();
            pageById[comps[k].name] = (int)pageTexts.size();
            if (!comps[k].title.empty()) pageById[comps[k].title] = (int)pageTexts.size();
            pageTexts.push_back(txt);
        }
    } else {
        if (err) *err = "Неизвестный тип DjVu: " + type;
        return false;
    }

    /* FB2CONV_DJVUDEBUG=файл — сырой текстовый слой каждой страницы, страницы через \f */
    const char* dbg = getenv("FB2CONV_DJVUDEBUG");
    if (dbg && *dbg) {
        FILE* df = fopen(dbg, "wb");
        if (df) {
            for (size_t p = 0; p < pageTexts.size(); p++) {
                const std::string& t = pageTexts[p];
                unsigned n = t.size() >= 3 ? be24(t, 0) : 0;
                if (n && 3 + (size_t)n <= t.size()) fwrite(t.data() + 3, 1, n, df);
                fputc('\f', df);
            }
            fclose(df);
        }
    }

    std::vector<DLine> lines;
    pageHeights.assign(pageTexts.size(), 0);
    size_t withText = 0;
    for (size_t p = 0; p < pageTexts.size(); p++) {
        if (!pageTexts[p].empty()) withText++;
        size_t before = lines.size();
        pageLines(pageTexts[p], (int)p, lines, pageHeights[p]);
        if (pageHeights[p] == 0 && lines.size() > before) {
            for (size_t i = before; i < lines.size(); i++) pageHeights[p] = std::max(pageHeights[p], lines[i].r.y1);
        }
    }
    if (lines.empty()) {
        if (err) *err = withText ? "Текстовый слой DjVu повреждён или пуст"
                                 : "В DjVu нет текстового слоя (страницы — только картинки; нужно распознавание текста)";
        return false;
    }

    /* типичная высота строки и поля страниц */
    std::vector<int> hs;
    for (size_t i = 0; i < lines.size(); i++) hs.push_back(lines[i].r.y1 - lines[i].r.y0);
    std::sort(hs.begin(), hs.end());
    double lh = hs[hs.size() / 2] > 0 ? hs[hs.size() / 2] : 10;
    /* две колонки (журнальная вёрстка): строки целиком в левой и в правой половине страницы —
       поля у каждой колонки свои, иначе любая строка левой колонки кажется короткой */
    std::map<int, int> minX, maxX;
    for (size_t i = 0; i < lines.size(); i++) {
        int p = lines[i].page;
        if (!minX.count(p) || lines[i].r.x0 < minX[p]) minX[p] = lines[i].r.x0;
        if (!maxX.count(p) || lines[i].r.x1 > maxX[p]) maxX[p] = lines[i].r.x1;
    }
    std::map<int, int> inLeft, inRight;
    for (size_t i = 0; i < lines.size(); i++) {
        if (!lines[i].geom) continue;
        int mid = (minX[lines[i].page] + maxX[lines[i].page]) / 2;
        if (lines[i].r.x1 < mid) inLeft[lines[i].page]++;
        else if (lines[i].r.x0 >= mid) inRight[lines[i].page]++;
    }
    for (size_t i = 0; i < lines.size(); i++) {
        int p = lines[i].page;
        if (inLeft[p] >= 3 && inRight[p] >= 3 && lines[i].r.x0 >= (minX[p] + maxX[p]) / 2) lines[i].col = 1;
    }
    std::map<int, std::vector<int> > lefts, rights;
    for (size_t i = 0; i < lines.size(); i++) {
        lefts[lines[i].page * 2 + lines[i].col].push_back(lines[i].r.x0);
        rights[lines[i].page * 2 + lines[i].col].push_back(lines[i].r.x1);
    }
    std::map<int, int> pLeft, pRight;  /* ключ — страница * 2 + колонка */
    for (std::map<int, std::vector<int> >::iterator it = lefts.begin(); it != lefts.end(); ++it) {
        std::vector<int>& l = it->second;
        std::vector<int>& r = rights[it->first];
        std::sort(l.begin(), l.end());
        std::sort(r.begin(), r.end());
        pLeft[it->first] = l[l.size() / 10];
        pRight[it->first] = r[std::min(r.size() - 1, r.size() * 9 / 10)];
    }

    /* колонтитулы и номера страниц: у края страницы — только цифры или повтор на многих страницах */
    std::map<std::string, std::set<int> > rep;
    for (size_t i = 0; i < lines.size(); i++) {
        int h = pageHeights[(size_t)lines[i].page];
        if (h <= 0) continue;
        bool edge = lines[i].r.y1 > h * 0.92 || lines[i].r.y0 < h * 0.08;
        if (!edge) continue;
        std::string key = lines[i].text;
        for (size_t k = 0; k < key.size(); k++) if (isdigit((unsigned char)key[k])) key[k] = '#';
        rep[key].insert(lines[i].page);
    }
    for (size_t i = 0; i < lines.size(); i++) {
        int h = pageHeights[(size_t)lines[i].page];
        if (h <= 0) continue;
        bool edge = lines[i].r.y1 > h * 0.92 || lines[i].r.y0 < h * 0.08;
        if (!edge) continue;
        std::string t = lines[i].text, key = t;
        for (size_t k = 0; k < key.size(); k++) if (isdigit((unsigned char)key[k])) key[k] = '#';
        std::string digits = replaceAll(replaceAll(t, " ", ""), "-", "");
        if (allDigits(digits) || (rep[key].size() >= 3 && rep[key].size() * 4 >= pageTexts.size()))
            lines[i].drop = true;
    }

    /* главы по закладкам: строка на странице закладки, начинающаяся с её текста */
    bool anyHeading = false;
    for (size_t m = 0; m < marks.size(); m++) {
        const Bookmark& b = marks[m];
        int page = -1;
        std::string u = b.url;
        if (!u.empty() && u[0] == '#') u = u.substr(1);
        if (allDigits(u)) page = atoi(u.c_str()) - 1;
        else if (pageById.count(u)) page = pageById[u];
        if (page < 0) continue;
        std::string want = normKey(b.title);
        bool found = false;
        for (size_t i = 0; i < lines.size() && !found; i++) {
            if (lines[i].page != page || lines[i].drop || lines[i].heading >= 0) continue;
            std::string acc;
            size_t j = i;
            for (; j < lines.size() && j < i + 3 && lines[j].page == page; j++) {
                acc += normKey(lines[j].text);
                if (acc.size() >= want.size()) break;
            }
            if (!want.empty() && acc.compare(0, want.size(), want) == 0) {
                lines[i].heading = b.level;
                lines[i].text = b.title;
                for (size_t k = i + 1; k <= j && k < lines.size(); k++) lines[k].drop = true;
                found = anyHeading = true;
            }
        }
        if (!found) {
            /* заголовка в тексте страницы нет — глава начинается с её первой строки */
            for (size_t i = 0; i < lines.size(); i++)
                if (lines[i].page >= page && !lines[i].drop) {
                    DLine h;
                    h.page = lines[i].page;
                    h.r = lines[i].r;
                    h.text = b.title;
                    h.heading = b.level;
                    lines.insert(lines.begin() + (long)i, h);
                    anyHeading = true;
                    break;
                }
        }
    }
    (void)anyHeading;

    /* абзацы: зоны абзацев, а без них — красная строка, короткая строка, интервал */
    /* зонам абзацев верим, только если они делят страницу: у иного OCR вся страница —
       одна зона-«абзац», тогда абзацы ищутся по геометрии */
    std::vector<std::set<int> > pageParas(pageHeights.size());
    std::vector<int> pageLineCount(pageHeights.size(), 0);
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].drop || lines[i].heading >= 0) continue;
        pageLineCount[(size_t)lines[i].page]++;
        if (lines[i].para >= 0) pageParas[(size_t)lines[i].page].insert(lines[i].para);
    }
    std::vector<bool> zonePara(pageHeights.size(), false);
    for (size_t p = 0; p < zonePara.size(); p++)
        zonePara[p] = !pageParas[p].empty() && (pageParas[p].size() >= 2 || pageLineCount[p] <= 6);
    std::vector<Chapter> chapters(1);
    std::string para;
    const DLine* prev = NULL;
    struct Out {
        static void flush(std::vector<Chapter>& ch, std::string& p) {
            std::string h = trim(p);
            p.clear();
            if (!h.empty()) ch.back().blocks.push_back(Block::p(h));
        }
    };
    /* FB2CONV_DJVUDEBUG: рядом с сырым текстом — раскладка строк (<файл>.layout) */
    FILE* lf = NULL;
    if (dbg && *dbg) lf = fopen((std::string(dbg) + ".layout").c_str(), "wb");
    if (lf) fprintf(lf, "lh=%.1f\n", lh);
    for (size_t i = 0; lf && i < lines.size(); i++) {
        const DLine& l = lines[i];
        fprintf(lf, "p%d.%d [%d..%d] x%d-%d y%d-%d para=%d%s%s h=%d | %s\n", l.page, l.col,
                pLeft[l.page * 2 + l.col], pRight[l.page * 2 + l.col],
                l.r.x0, l.r.x1, l.r.y0, l.r.y1, l.para, zonePara[(size_t)l.page] ? "z" : "", l.drop ? " DROP" : "",
                l.heading, l.text.c_str());
    }
    if (lf) fclose(lf);
    for (size_t i = 0; i < lines.size(); i++) {
        const DLine& l = lines[i];
        if (l.drop) continue;
        if (l.heading >= 0) {
            Out::flush(chapters, para);
            Chapter c;
            c.level = l.heading;
            c.title = escHtml(l.text);
            chapters.push_back(c);
            prev = NULL;
            continue;
        }
        bool newPara = prev == NULL;
        if (prev) {
            bool samePage = prev->page == l.page;
            int left = pLeft[l.page * 2 + l.col], right = pRight[prev->page * 2 + prev->col];
            bool zones = zonePara[(size_t)l.page] && zonePara[(size_t)prev->page];
            if (zones && samePage) newPara = l.para != prev->para;
            else if (zones && !samePage)
                newPara = endsSentence(prev->text) && !startsLower(l.text) && (prev->r.x1 < right - 3 * lh || l.r.x0 > left + lh);
            else if (!l.geom || !prev->geom) {
                /* только текст: абзац кончается короткой строкой с концом фразы */
                newPara = endsSentence(prev->text) && prev->r.x1 < right * 0.8;
            } else {
                if (samePage && prev->r.y0 - l.r.y1 > lh * 1.2) newPara = true;
                else if (l.r.x0 > left + lh * 0.9 && l.r.x0 < left + lh * 8) newPara = true;
                else if (prev->r.x1 < right - lh * 3 && endsSentence(prev->text)) newPara = true;
            }
            if (startsWith(l.text, "\xE2\x80\x94") || startsWith(l.text, "\xE2\x80\x93 ")) newPara = true;
        }
        std::string h = escHtml(l.text);
        if (newPara) {
            Out::flush(chapters, para);
            para = h;
        } else if (!para.empty() && endsWithNoCase(para, "\xC2\xAC")) {
            /* ¬ в OCR — всегда знак переноса, в том числе в словах КАПСОМ */
            para.erase(para.size() - 2);
            para += h;
        } else if (!para.empty() && para[para.size() - 1] == '-' && startsLower(l.text)) {
            /* перенос «сло-/во» */
            para.erase(para.size() - 1);
            para += h;
        } else {
            para += " " + h;
        }
        prev = &l;
    }
    Out::flush(chapters, para);

    for (size_t i = 0; i < chapters.size(); i++)
        if (!chapters[i].title.empty() || !chapters[i].blocks.empty()) book.chapters.push_back(chapters[i]);
    normalizeChapterLevels(book.chapters);
    if (book.chapters.empty()) {
        if (err) *err = "В DjVu не нашлось текста";
        return false;
    }
    return true;
}

} /* namespace */

bool readDJVU(const Bytes& data, const std::string& fileName, Book& book, std::string* err) {
    std::string s(data.begin(), data.end());
    book.fileName = fileName;
    DjvuReader r(book, fileName);
    return r.read(s, err);
}
