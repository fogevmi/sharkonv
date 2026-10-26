/* Чтение PDF: текстовый слой -> книга.
   Свой разбор PDF (объекты, xref-таблицы и xref-потоки, объектные потоки, фильтры Flate/LZW/
   ASCIIHex/ASCII85 с предикторами), шрифты (ToUnicode, WinAnsi/MacRoman, Differences с именами
   глифов, ширины), исполнение потоков страниц (матрицы, Tj/TJ, формы XObject).
   Затем восстановление вёрстки: строки, абзацы, переносы, колонтитулы; главы — из закладок
   (Outlines), а без них — по размеру шрифта; сноски — надстрочный номер в тексте + мелкий
   текст с тем же номером внизу страницы. Сканы без текстового слоя не распознаются (это OCR). */
#include "convert.h"
#include <zlib.h>
#include <math.h>

namespace {

/* ============================== объекты =============================== */

enum PType { P_NULL, P_BOOL, P_NUM, P_STR, P_NAME, P_ARR, P_DICT, P_REF, P_OP };

struct PObj {
    PType t;
    double num;
    std::string str; /* строка (байты), имя или оператор */
    int ref, gen;
    std::vector<PObj> arr;
    std::vector<std::pair<std::string, PObj> > dict;
    bool hasStream;
    size_t streamPos, streamLen; /* сырые байты потока в файле или в буфере objstm */
    std::string streamData;      /* если поток пришёл не из файла */
    PObj() : t(P_NULL), num(0), ref(0), gen(0), hasStream(false), streamPos(0), streamLen(0) {}
    const PObj* get(const char* key) const {
        for (size_t i = 0; i < dict.size(); i++)
            if (dict[i].first == key) return &dict[i].second;
        return NULL;
    }
    int intVal() const { return (int)floor(num + 0.5); }
};

const PObj kNull;

bool isWs(unsigned char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == 0; }
bool isDelim(unsigned char c) { return strchr("()<>[]{}/%", c) != NULL && c != 0; }

struct Lexer {
    const std::string& s;
    size_t p, end;
    Lexer(const std::string& src, size_t from, size_t to) : s(src), p(from), end(to) {}

    void skipWs() {
        while (p < end) {
            unsigned char c = (unsigned char)s[p];
            if (isWs(c)) { p++; continue; }
            if (c == '%') {
                while (p < end && s[p] != '\n' && s[p] != '\r') p++;
                continue;
            }
            break;
        }
    }

    std::string literal() {
        std::string out;
        int depth = 1;
        p++; /* ( */
        while (p < end) {
            char c = s[p++];
            if (c == '\\') {
                if (p >= end) break;
                char e = s[p++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case '\r': if (p < end && s[p] == '\n') p++; break;
                    case '\n': break;
                    default:
                        if (e >= '0' && e <= '7') {
                            int v = e - '0';
                            for (int k = 0; k < 2 && p < end && s[p] >= '0' && s[p] <= '7'; k++) v = v * 8 + (s[p++] - '0');
                            out += (char)(v & 0xFF);
                        } else {
                            out += e;
                        }
                }
                continue;
            }
            if (c == '(') depth++;
            else if (c == ')') { if (--depth == 0) break; }
            out += c;
        }
        return out;
    }

    std::string hexStr() {
        std::string out;
        p++; /* < */
        int hi = -1;
        while (p < end && s[p] != '>') {
            char c = s[p++];
            int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                    (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (v < 0) continue;
            if (hi < 0) hi = v;
            else { out += (char)(hi * 16 + v); hi = -1; }
        }
        if (hi >= 0) out += (char)(hi * 16);
        if (p < end) p++;
        return out;
    }

    std::string name() {
        std::string out;
        p++; /* / */
        while (p < end && !isWs((unsigned char)s[p]) && !isDelim((unsigned char)s[p])) {
            if (s[p] == '#' && p + 2 < end && isxdigit((unsigned char)s[p + 1]) && isxdigit((unsigned char)s[p + 2])) {
                out += (char)strtol(s.substr(p + 1, 2).c_str(), NULL, 16);
                p += 3;
                continue;
            }
            out += s[p++];
        }
        return out;
    }

    /* число, ссылка «n g R», ключевое слово; в потоках страниц — оператор */
    bool parse(PObj& o, int depth, bool allowRef) {
        o = PObj();
        skipWs();
        if (p >= end || depth > 200) return false;
        unsigned char c = (unsigned char)s[p];
        if (c == '(') { o.t = P_STR; o.str = literal(); return true; }
        if (c == '<') {
            if (p + 1 < end && s[p + 1] == '<') {
                p += 2;
                o.t = P_DICT;
                for (;;) {
                    skipWs();
                    if (p >= end) return true;
                    if (s[p] == '>' && p + 1 < end && s[p + 1] == '>') { p += 2; break; }
                    if (s[p] != '/') { PObj junk; if (!parse(junk, depth + 1, allowRef)) return true; continue; }
                    std::string key = name();
                    PObj v;
                    if (!parse(v, depth + 1, allowRef)) return true;
                    o.dict.push_back(std::make_pair(key, v));
                }
                return true;
            }
            o.t = P_STR;
            o.str = hexStr();
            return true;
        }
        if (c == '[') {
            p++;
            o.t = P_ARR;
            for (;;) {
                skipWs();
                if (p >= end) return true;
                if (s[p] == ']') { p++; break; }
                PObj v;
                if (!parse(v, depth + 1, allowRef)) return true;
                o.arr.push_back(v);
            }
            return true;
        }
        if (c == '/') { o.t = P_NAME; o.str = name(); return true; }
        if (c == ')' || c == '>' || c == ']' || c == '}' || c == '{') { p++; o.t = P_OP; o.str = std::string(1, (char)c); return true; }
        if (isdigit(c) || c == '-' || c == '+' || c == '.') {
            size_t st = p;
            p++;
            while (p < end && (isdigit((unsigned char)s[p]) || s[p] == '.' || s[p] == '-' || s[p] == 'e' || s[p] == 'E')) p++;
            o.t = P_NUM;
            o.num = strtod(s.substr(st, p - st).c_str(), NULL);
            if (allowRef && o.num >= 0 && s.substr(st, p - st).find('.') == std::string::npos) {
                /* «12 0 R»? */
                size_t save = p;
                skipWs();
                size_t g0 = p;
                while (p < end && isdigit((unsigned char)s[p])) p++;
                if (p > g0) {
                    int gen = atoi(s.substr(g0, p - g0).c_str());
                    skipWs();
                    if (p < end && s[p] == 'R' && (p + 1 >= end || isWs((unsigned char)s[p + 1]) || isDelim((unsigned char)s[p + 1]))) {
                        p++;
                        o.t = P_REF;
                        o.ref = o.intVal();
                        o.gen = gen;
                        return true;
                    }
                }
                p = save;
            }
            return true;
        }
        size_t st = p;
        while (p < end && !isWs((unsigned char)s[p]) && !isDelim((unsigned char)s[p])) p++;
        if (p == st) { p++; o.t = P_OP; o.str = std::string(1, (char)c); return true; }
        std::string w = s.substr(st, p - st);
        if (w == "true" || w == "false") { o.t = P_BOOL; o.num = w == "true" ? 1 : 0; return true; }
        if (w == "null") { o.t = P_NULL; return true; }
        o.t = P_OP;
        o.str = w;
        return true;
    }
};

/* ============================== фильтры =============================== */

bool inflateAll(const std::string& in, std::string& out, bool raw) {
    out.clear();
    if (in.empty()) return false;
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, raw ? -15 : 15 + 32) != Z_OK) return false;
    zs.next_in = (Bytef*)in.data();
    zs.avail_in = (uInt)in.size();
    char buf[65536];
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = (Bytef*)buf;
        zs.avail_out = sizeof(buf);
        rc = inflate(&zs, Z_NO_FLUSH);
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (out.size() > 400u * 1024 * 1024) break;
    }
    inflateEnd(&zs);
    /* битые потоки (обрезанный хвост) — берём, что распаковалось */
    return rc == Z_STREAM_END || !out.empty();
}

std::string lzwDecode(const std::string& in, int early) {
    std::string out;
    std::vector<std::string> dict;
    for (int i = 0; i < 256; i++) dict.push_back(std::string(1, (char)i));
    dict.push_back(std::string()); dict.push_back(std::string()); /* 256 clear, 257 eod */
    int bits = 9;
    unsigned long acc = 0;
    int nacc = 0;
    std::string prev;
    bool havePrev = false;
    for (size_t i = 0; i < in.size(); i++) {
        acc = (acc << 8) | (unsigned char)in[i];
        nacc += 8;
        while (nacc >= bits) {
            int code = (int)((acc >> (nacc - bits)) & ((1u << bits) - 1));
            nacc -= bits;
            if (code == 256) {
                dict.resize(258);
                bits = 9;
                havePrev = false;
                continue;
            }
            if (code == 257) return out;
            std::string entry;
            if (code < (int)dict.size() && (code < 256 || !dict[code].empty())) entry = dict[code];
            else if (havePrev) entry = prev + prev[0];
            else return out;
            out += entry;
            if (havePrev) dict.push_back(prev + entry[0]);
            prev = entry;
            havePrev = true;
            int size = (int)dict.size() + early;
            if (size >= 4096) bits = 12;
            else if (size >= 2048) bits = 12;
            else if (size >= 1024) bits = 11;
            else if (size >= 512) bits = 10;
        }
    }
    return out;
}

std::string ascii85(const std::string& in) {
    std::string out;
    unsigned long v = 0;
    int n = 0;
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '~') break;
        if (isWs((unsigned char)c)) continue;
        if (c == 'z' && n == 0) { out.append(4, '\0'); continue; }
        if (c < '!' || c > 'u') continue;
        v = v * 85 + (unsigned long)(c - '!');
        if (++n == 5) {
            for (int k = 3; k >= 0; k--) out += (char)((v >> (k * 8)) & 0xFF);
            v = 0;
            n = 0;
        }
    }
    if (n > 1) {
        for (int k = n; k < 5; k++) v = v * 85 + 84;
        for (int k = 3; k >= 5 - n; k--) out += (char)((v >> (k * 8)) & 0xFF);
    }
    return out;
}

std::string asciiHex(const std::string& in) {
    std::string out;
    int hi = -1;
    for (size_t i = 0; i < in.size(); i++) {
        char c = in[i];
        if (c == '>') break;
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { out += (char)(hi * 16 + v); hi = -1; }
    }
    if (hi >= 0) out += (char)(hi * 16);
    return out;
}

/* PNG-предикторы (xref-потоки и картинки), TIFF 2 — редкость */
std::string unpredict(const std::string& in, const PObj* parms) {
    if (!parms || parms->t != P_DICT) return in;
    const PObj* pr = parms->get("Predictor");
    int pred = pr ? pr->intVal() : 1;
    if (pred < 10) return in;
    const PObj* co = parms->get("Columns");
    const PObj* cl = parms->get("Colors");
    const PObj* bp = parms->get("BitsPerComponent");
    int cols = co ? co->intVal() : 1, colors = cl ? cl->intVal() : 1, bpc = bp ? bp->intVal() : 8;
    int bpp = std::max(1, colors * bpc / 8);
    int row = (cols * colors * bpc + 7) / 8;
    if (row <= 0) return in;
    std::string out;
    std::string prior((size_t)row, '\0');
    size_t i = 0;
    while (i + 1 + (size_t)row <= in.size()) {
        int type = (unsigned char)in[i++];
        std::string cur = in.substr(i, (size_t)row);
        i += (size_t)row;
        for (int k = 0; k < row; k++) {
            int a = k >= bpp ? (unsigned char)cur[k - bpp] : 0;
            int b = (unsigned char)prior[k];
            int c = k >= bpp ? (unsigned char)prior[k - bpp] : 0;
            int x = (unsigned char)cur[k];
            switch (type) {
                case 1: x += a; break;
                case 2: x += b; break;
                case 3: x += (a + b) / 2; break;
                case 4: {
                    int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
                    x += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    break;
                }
                default: break;
            }
            cur[k] = (char)(x & 0xFF);
        }
        out += cur;
        prior = cur;
    }
    return out;
}

/* ============================== документ ============================== */

struct XEntry {
    int type;    /* 1 — смещение в файле, 2 — в объектном потоке */
    size_t off;  /* type 1 */
    int stm, idx; /* type 2 */
    XEntry() : type(0), off(0), stm(0), idx(0) {}
};

class PdfDoc {
public:
    explicit PdfDoc(const std::string& data) : encrypted(false), s(data) {}
    bool open();
    const PObj& resolve(const PObj& o, int depth = 0);
    const PObj& object(int num);
    bool streamBytes(const PObj& stm, std::string& out);
    const PObj& trailer() const { return trail; }
    bool encrypted;

private:
    const std::string& s;
    std::map<int, XEntry> xref;
    std::map<int, PObj> cache;
    std::map<int, std::string> objStmData;
    std::set<int> loading;
    PObj trail;

    bool readXref(size_t off, int depth);
    bool readXrefStream(const PObj& o, size_t objPos);
    void scanObjects();
    bool parseIndirect(size_t off, PObj& out);
    bool loadFromStream(int stm, int idx, int num, PObj& out);
};

bool PdfDoc::parseIndirect(size_t off, PObj& out) {
    Lexer lx(s, off, s.size());
    PObj a, b, kw;
    if (!lx.parse(a, 0, false) || a.t != P_NUM) return false;
    if (!lx.parse(b, 0, false) || b.t != P_NUM) return false;
    if (!lx.parse(kw, 0, false) || kw.t != P_OP || kw.str != "obj") return false;
    if (!lx.parse(out, 0, true)) return false;
    size_t save = lx.p;
    lx.skipWs();
    if (out.t == P_DICT && s.compare(lx.p, 6, "stream") == 0) {
        size_t p = lx.p + 6;
        if (p < s.size() && s[p] == '\r') p++;
        if (p < s.size() && s[p] == '\n') p++;
        out.hasStream = true;
        out.streamPos = p;
        long len = -1;
        const PObj* L = out.get("Length");
        if (L && L->t == P_NUM) len = L->intVal();
        else if (L && L->t == P_REF && !loading.count(L->ref)) {
            const PObj& lv = resolve(*L);
            if (lv.t == P_NUM) len = lv.intVal();
        }
        /* длина проверяется: за ней должно идти endstream, иначе ищем сами */
        bool ok = len >= 0 && p + (size_t)len <= s.size();
        if (ok) {
            size_t q = p + (size_t)len;
            while (q < s.size() && isWs((unsigned char)s[q])) q++;
            ok = s.compare(q, 9, "endstream") == 0;
        }
        if (!ok) {
            size_t e = s.find("endstream", p);
            if (e == std::string::npos) e = s.size();
            size_t q = e;
            if (q > p && s[q - 1] == '\n') q--;
            if (q > p && s[q - 1] == '\r') q--;
            len = (long)(q - p);
        }
        out.streamLen = (size_t)len;
    } else {
        lx.p = save;
    }
    return true;
}

bool PdfDoc::readXrefStream(const PObj& o, size_t objPos) {
    (void)objPos;
    std::string data;
    if (!streamBytes(o, data)) return false;
    const PObj* W = o.get("W");
    if (!W || W->t != P_ARR || W->arr.size() < 3) return false;
    int w[3] = {W->arr[0].intVal(), W->arr[1].intVal(), W->arr[2].intVal()};
    int rowLen = w[0] + w[1] + w[2];
    if (rowLen <= 0) return false;
    std::vector<int> index;
    const PObj* I = o.get("Index");
    if (I && I->t == P_ARR) for (size_t i = 0; i < I->arr.size(); i++) index.push_back(I->arr[i].intVal());
    else { const PObj* sz = o.get("Size"); index.push_back(0); index.push_back(sz ? sz->intVal() : 0); }
    size_t pos = 0;
    for (size_t k = 0; k + 1 < index.size(); k += 2) {
        int first = index[k], count = index[k + 1];
        for (int i = 0; i < count && pos + (size_t)rowLen <= data.size(); i++) {
            long f[3] = {w[0] == 0 ? 1 : 0, 0, 0};
            size_t q = pos;
            for (int j = 0; j < 3; j++) {
                if (w[j] == 0) continue;
                long v = 0;
                for (int b = 0; b < w[j]; b++) v = (v << 8) | (unsigned char)data[q++];
                f[j] = v;
            }
            pos += (size_t)rowLen;
            int num = first + i;
            if (xref.count(num)) continue; /* более новая запись уже есть */
            XEntry e;
            if (f[0] == 1) { e.type = 1; e.off = (size_t)f[1]; }
            else if (f[0] == 2) { e.type = 2; e.stm = (int)f[1]; e.idx = (int)f[2]; }
            else continue;
            xref[num] = e;
        }
    }
    return true;
}

bool PdfDoc::readXref(size_t off, int depth) {
    if (depth > 50 || off >= s.size()) return false;
    Lexer lx(s, off, s.size());
    lx.skipWs();
    if (s.compare(lx.p, 4, "xref") == 0) {
        lx.p += 4;
        for (;;) {
            PObj a;
            size_t save = lx.p;
            if (!lx.parse(a, 0, false)) return false;
            if (a.t == P_OP && a.str == "trailer") break;
            PObj b;
            if (a.t != P_NUM || !lx.parse(b, 0, false) || b.t != P_NUM) { lx.p = save; break; }
            int first = a.intVal(), count = b.intVal();
            for (int i = 0; i < count; i++) {
                PObj o, g, f;
                if (!lx.parse(o, 0, false) || !lx.parse(g, 0, false) || !lx.parse(f, 0, false)) return false;
                int num = first + i;
                if (f.t == P_OP && f.str == "n" && !xref.count(num)) {
                    XEntry e;
                    e.type = 1;
                    e.off = (size_t)o.num;
                    xref[num] = e;
                } else if (!xref.count(num)) {
                    XEntry e; /* свободный — помечаем, чтобы старые таблицы его не воскресили */
                    xref[num] = e;
                }
            }
        }
        PObj t;
        if (!lx.parse(t, 0, true) || t.t != P_DICT) return false;
        if (trail.t != P_DICT) trail = t;
        const PObj* xs = t.get("XRefStm");
        if (xs && xs->t == P_NUM) {
            PObj so;
            if (parseIndirect((size_t)xs->num, so)) readXrefStream(so, (size_t)xs->num);
        }
        const PObj* prev = t.get("Prev");
        if (prev && prev->t == P_NUM) readXref((size_t)prev->num, depth + 1);
        return true;
    }
    PObj o;
    if (!parseIndirect(lx.p, o) || o.t != P_DICT) return false;
    const PObj* type = o.get("Type");
    if (!type || type->str != "XRef") return false;
    if (trail.t != P_DICT) { trail = o; trail.hasStream = false; }
    readXrefStream(o, lx.p);
    const PObj* prev = o.get("Prev");
    if (prev && prev->t == P_NUM) readXref((size_t)prev->num, depth + 1);
    return true;
}

/* запасной путь для битых файлов: найти все «n g obj» прямо в тексте файла */
void PdfDoc::scanObjects() {
    xref.clear();
    size_t p = 0;
    while ((p = s.find("obj", p)) != std::string::npos) {
        size_t e = p;
        p += 3;
        if (e == 0 || (p < s.size() && !isWs((unsigned char)s[p]) && !isDelim((unsigned char)s[p]))) continue;
        size_t q = e;
        while (q > 0 && isWs((unsigned char)s[q - 1])) q--;
        size_t g1 = q;
        while (q > 0 && isdigit((unsigned char)s[q - 1])) q--;
        if (q == g1) continue;
        size_t g0 = q;
        while (q > 0 && isWs((unsigned char)s[q - 1])) q--;
        size_t n1 = q;
        while (q > 0 && isdigit((unsigned char)s[q - 1])) q--;
        if (q == n1 || g0 == g1) continue;
        int num = atoi(s.substr(q, n1 - q).c_str());
        XEntry x;
        x.type = 1;
        x.off = q;
        xref[num] = x; /* позднее определение побеждает, как при инкрементальном сохранении */
    }
    /* объекты внутри объектных потоков */
    std::map<int, XEntry> base = xref;
    for (std::map<int, XEntry>::iterator it = base.begin(); it != base.end(); ++it) {
        PObj o;
        if (!parseIndirect(it->second.off, o) || !o.hasStream) continue;
        const PObj* type = o.get("Type");
        if (type && type->str == "XRef" && trail.t != P_DICT) { trail = o; trail.hasStream = false; }
        if (!type || type->str != "ObjStm") continue;
        std::string data;
        if (!streamBytes(o, data)) continue;
        const PObj* N = o.get("N");
        Lexer lx(data, 0, data.size());
        for (int i = 0; N && i < N->intVal(); i++) {
            PObj a, b;
            if (!lx.parse(a, 0, false) || !lx.parse(b, 0, false)) break;
            if (!xref.count(a.intVal()) || xref[a.intVal()].type != 1) {
                XEntry x;
                x.type = 2;
                x.stm = it->first;
                x.idx = i;
                xref[a.intVal()] = x;
            }
        }
    }
    if (trail.t != P_DICT || !trail.get("Root")) {
        for (std::map<int, XEntry>::iterator it = xref.begin(); it != xref.end(); ++it) {
            const PObj& o = object(it->first);
            const PObj* type = o.get("Type");
            if (o.t == P_DICT && type && type->str == "Catalog") {
                trail = PObj();
                trail.t = P_DICT;
                PObj r;
                r.t = P_REF;
                r.ref = it->first;
                trail.dict.push_back(std::make_pair(std::string("Root"), r));
                break;
            }
        }
    }
}

bool PdfDoc::open() {
    if (s.compare(0, 5, "%PDF-") != 0 && s.find("%PDF-") > 1024) return false;
    size_t sx = s.rfind("startxref");
    bool ok = false;
    if (sx != std::string::npos) {
        Lexer lx(s, sx + 9, s.size());
        PObj off;
        if (lx.parse(off, 0, false) && off.t == P_NUM) ok = readXref((size_t)off.num, 0);
    }
    if (!ok || trail.t != P_DICT || !trail.get("Root") || !resolve(*trail.get("Root")).get("Pages")) {
        trail = PObj();
        cache.clear();
        scanObjects();
    }
    encrypted = trail.get("Encrypt") != NULL;
    return trail.t == P_DICT && trail.get("Root");
}

bool PdfDoc::loadFromStream(int stm, int idx, int num, PObj& out) {
    std::map<int, std::string>::iterator it = objStmData.find(stm);
    if (it == objStmData.end()) {
        const PObj& so = object(stm);
        std::string data;
        if (!so.hasStream || !streamBytes(so, data)) return false;
        it = objStmData.insert(std::make_pair(stm, data)).first;
    }
    const std::string& data = it->second;
    const PObj& so = object(stm);
    const PObj* N = so.get("N");
    const PObj* F = so.get("First");
    if (!N || !F) return false;
    Lexer lx(data, 0, data.size());
    size_t off = 0;
    bool found = false;
    for (int i = 0; i < N->intVal(); i++) {
        PObj a, b;
        if (!lx.parse(a, 0, false) || !lx.parse(b, 0, false)) return false;
        if (i == idx || a.intVal() == num) { off = (size_t)b.num; found = true; if (a.intVal() == num) break; }
    }
    if (!found) return false;
    Lexer ox(data, (size_t)F->intVal() + off, data.size());
    return ox.parse(out, 0, true);
}

const PObj& PdfDoc::object(int num) {
    std::map<int, PObj>::iterator c = cache.find(num);
    if (c != cache.end()) return c->second;
    if (loading.count(num) || cache.size() > 2000000) return kNull;
    loading.insert(num);
    PObj o;
    std::map<int, XEntry>::iterator x = xref.find(num);
    if (x != xref.end()) {
        if (x->second.type == 1) parseIndirect(x->second.off, o);
        else if (x->second.type == 2) loadFromStream(x->second.stm, x->second.idx, num, o);
    }
    loading.erase(num);
    return cache[num] = o;
}

const PObj& PdfDoc::resolve(const PObj& o, int depth) {
    if (o.t != P_REF) return o;
    if (depth > 32) return kNull;
    return resolve(object(o.ref), depth + 1);
}

bool PdfDoc::streamBytes(const PObj& stm, std::string& out) {
    if (!stm.hasStream) return false;
    std::string data = stm.streamData.empty() ? s.substr(stm.streamPos, stm.streamLen) : stm.streamData;
    std::vector<std::string> filters;
    std::vector<const PObj*> parms;
    const PObj* f = stm.get("Filter");
    const PObj* dp = stm.get("DecodeParms");
    if (f) {
        const PObj& fr = resolve(*f);
        if (fr.t == P_NAME) { filters.push_back(fr.str); parms.push_back(dp ? &resolve(*dp) : NULL); }
        else if (fr.t == P_ARR)
            for (size_t i = 0; i < fr.arr.size(); i++) {
                filters.push_back(resolve(fr.arr[i]).str);
                const PObj* pi = NULL;
                if (dp) {
                    const PObj& dr = resolve(*dp);
                    if (dr.t == P_ARR && i < dr.arr.size()) pi = &resolve(dr.arr[i]);
                }
                parms.push_back(pi);
            }
    }
    for (size_t i = 0; i < filters.size(); i++) {
        const std::string& n = filters[i];
        std::string next;
        if (n == "FlateDecode" || n == "Fl") {
            if (!inflateAll(data, next, false) && !inflateAll(data, next, true)) return false;
            next = unpredict(next, parms[i]);
        } else if (n == "LZWDecode" || n == "LZW") {
            const PObj* ec = parms[i] ? parms[i]->get("EarlyChange") : NULL;
            next = unpredict(lzwDecode(data, ec ? ec->intVal() : 1), parms[i]);
        } else if (n == "ASCII85Decode" || n == "A85") {
            next = ascii85(data);
        } else if (n == "ASCIIHexDecode" || n == "AHx") {
            next = asciiHex(data);
        } else {
            out = data; /* картинки (DCT, JPX, CCITT) — как есть */
            return n == "DCTDecode";
        }
        data.swap(next);
    }
    out.swap(data);
    return true;
}

/* ============================== текст ================================= */

void appendUtf8(std::string& out, unsigned cp) {
    std::wstring w;
    if (cp >= 0x10000) {
        cp -= 0x10000;
        w += (wchar_t)(0xD800 + (cp >> 10));
        w += (wchar_t)(0xDC00 + (cp & 0x3FF));
    } else {
        w += (wchar_t)cp;
    }
    out += wToUtf8(w);
}

std::string utf16beToUtf8(const std::string& b) {
    std::wstring w;
    for (size_t i = 0; i + 1 < b.size(); i += 2) w += (wchar_t)(((unsigned char)b[i] << 8) | (unsigned char)b[i + 1]);
    return wToUtf8(w);
}

/* строка-текст PDF (заголовок, автор, закладка): UTF-16BE с BOM, UTF-8 с BOM или PDFDocEncoding */
std::string pdfText(const std::string& b) {
    if (b.size() >= 2 && (unsigned char)b[0] == 0xFE && (unsigned char)b[1] == 0xFF) return utf16beToUtf8(b.substr(2));
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF)
        return b.substr(3);
    return cpToUtf8(b, 1252);
}

/* имя глифа -> Unicode: uniXXXX, uXXXX, кириллица afii100xx и частые латинские имена */
unsigned glyphToUnicode(const std::string& g) {
    if (g.size() == 7 && g.compare(0, 3, "uni") == 0) return (unsigned)strtoul(g.c_str() + 3, NULL, 16);
    if (g.size() >= 5 && g.size() <= 7 && g[0] == 'u' && isxdigit((unsigned char)g[1]))
        return (unsigned)strtoul(g.c_str() + 1, NULL, 16);
    if (g.size() == 1) return (unsigned char)g[0];
    if (g.compare(0, 4, "afii") == 0) {
        int n = atoi(g.c_str() + 4);
        if (n >= 10017 && n <= 10022) return 0x410 + (n - 10017);
        if (n == 10023) return 0x401;
        if (n >= 10024 && n <= 10049) return 0x416 + (n - 10024);
        if (n >= 10065 && n <= 10070) return 0x430 + (n - 10065);
        if (n == 10071) return 0x451;
        if (n >= 10072 && n <= 10097) return 0x436 + (n - 10072);
        static const int ex[][2] = {{10050, 0x490}, {10051, 0x402}, {10052, 0x403}, {10053, 0x404}, {10054, 0x405},
                                    {10055, 0x406}, {10056, 0x407}, {10057, 0x408}, {10058, 0x409}, {10059, 0x40A},
                                    {10060, 0x40B}, {10061, 0x40C}, {10062, 0x40E}, {10145, 0x40F}, {10098, 0x491},
                                    {10099, 0x452}, {10100, 0x453}, {10101, 0x454}, {10102, 0x455}, {10103, 0x456},
                                    {10104, 0x457}, {10105, 0x458}, {10106, 0x459}, {10107, 0x45A}, {10108, 0x45B},
                                    {10109, 0x45C}, {10110, 0x45E}, {10193, 0x45F}, {61352, 0x2116}, {0, 0}};
        for (int i = 0; ex[i][0]; i++) if (ex[i][0] == n) return (unsigned)ex[i][1];
        return 0;
    }
    static const struct { const char* n; unsigned u; } names[] = {
        {"space", 32}, {"exclam", 33}, {"quotedbl", 34}, {"numbersign", 35}, {"dollar", 36}, {"percent", 37},
        {"ampersand", 38}, {"quotesingle", 39}, {"parenleft", 40}, {"parenright", 41}, {"asterisk", 42},
        {"plus", 43}, {"comma", 44}, {"hyphen", 45}, {"period", 46}, {"slash", 47}, {"zero", 48}, {"one", 49},
        {"two", 50}, {"three", 51}, {"four", 52}, {"five", 53}, {"six", 54}, {"seven", 55}, {"eight", 56},
        {"nine", 57}, {"colon", 58}, {"semicolon", 59}, {"less", 60}, {"equal", 61}, {"greater", 62},
        {"question", 63}, {"at", 64}, {"bracketleft", 91}, {"backslash", 92}, {"bracketright", 93},
        {"asciicircum", 94}, {"underscore", 95}, {"grave", 96}, {"braceleft", 123}, {"bar", 124},
        {"braceright", 125}, {"asciitilde", 126}, {"quoteleft", 0x2018}, {"quoteright", 0x2019},
        {"quotedblleft", 0x201C}, {"quotedblright", 0x201D}, {"quotesinglbase", 0x201A}, {"quotedblbase", 0x201E},
        {"guillemotleft", 0xAB}, {"guillemotright", 0xBB}, {"endash", 0x2013}, {"emdash", 0x2014},
        {"bullet", 0x2022}, {"ellipsis", 0x2026}, {"nbspace", 0xA0}, {"section", 0xA7}, {"copyright", 0xA9},
        {"registered", 0xAE}, {"degree", 0xB0}, {"periodcentered", 0xB7}, {"fi", 0xFB01}, {"fl", 0xFB02},
        {"ff", 0xFB00}, {"ffi", 0xFB03}, {"ffl", 0xFB04}, {"dagger", 0x2020}, {"daggerdbl", 0x2021},
        {"minus", 0x2212}, {"numero", 0x2116}, {NULL, 0}};
    for (int i = 0; names[i].n; i++) if (g == names[i].n) return names[i].u;
    return 0;
}

struct Font {
    int bytes;                                /* 1 или 2 байта на код */
    std::map<unsigned, std::string> toUni;     /* ToUnicode */
    std::vector<std::string> simple;           /* 256 кодов простого шрифта -> UTF-8 */
    std::map<unsigned, double> widths;         /* ширина / 1000 */
    double defWidth;
    bool bold, italic;
    Font() : bytes(1), defWidth(0.5), bold(false), italic(false) {}

    std::string text(unsigned code) const {
        std::map<unsigned, std::string>::const_iterator it = toUni.find(code);
        if (it != toUni.end()) return it->second;
        if (bytes == 1 && code < simple.size()) return simple[code];
        return std::string();
    }
    double width(unsigned code) const {
        std::map<unsigned, double>::const_iterator it = widths.find(code);
        return it != widths.end() ? it->second : defWidth;
    }
};

/* ToUnicode CMap: bfchar / bfrange, codespacerange задаёт длину кода */
void parseToUnicode(const std::string& cm, Font& f) {
    Lexer lx(cm, 0, cm.size());
    std::vector<PObj> ops;
    PObj o;
    while (lx.parse(o, 0, false)) {
        if (o.t != P_OP) { ops.push_back(o); continue; }
        if (o.str == "endcodespacerange") {
            for (size_t i = 0; i + 1 < ops.size(); i += 2)
                if (ops[i].t == P_STR && !ops[i].str.empty()) f.bytes = std::max(1, std::min(2, (int)ops[i].str.size()));
        } else if (o.str == "endbfchar") {
            for (size_t i = 0; i + 1 < ops.size(); i += 2) {
                if (ops[i].t != P_STR || ops[i + 1].t != P_STR) continue;
                unsigned code = 0;
                for (size_t k = 0; k < ops[i].str.size(); k++) code = (code << 8) | (unsigned char)ops[i].str[k];
                f.toUni[code] = utf16beToUtf8(ops[i + 1].str);
            }
        } else if (o.str == "endbfrange") {
            for (size_t i = 0; i + 2 < ops.size(); i += 3) {
                if (ops[i].t != P_STR || ops[i + 1].t != P_STR) continue;
                unsigned lo = 0, hi = 0;
                for (size_t k = 0; k < ops[i].str.size(); k++) lo = (lo << 8) | (unsigned char)ops[i].str[k];
                for (size_t k = 0; k < ops[i + 1].str.size(); k++) hi = (hi << 8) | (unsigned char)ops[i + 1].str[k];
                if (hi < lo || hi - lo > 65535) continue;
                if (ops[i + 2].t == P_ARR) {
                    for (unsigned c = lo; c <= hi && c - lo < ops[i + 2].arr.size(); c++)
                        f.toUni[c] = utf16beToUtf8(ops[i + 2].arr[c - lo].str);
                } else if (ops[i + 2].t == P_STR && ops[i + 2].str.size() >= 2) {
                    std::string dst = ops[i + 2].str;
                    for (unsigned c = lo; c <= hi; c++) {
                        f.toUni[c] = utf16beToUtf8(dst);
                        /* последний байт растёт вместе с кодом */
                        for (size_t k = dst.size(); k-- > 0;) {
                            if ((unsigned char)dst[k] != 0xFF) { dst[k] = (char)((unsigned char)dst[k] + 1); break; }
                            dst[k] = 0;
                        }
                    }
                }
            }
        }
        if (o.str.compare(0, 3, "end") == 0 || o.str.compare(0, 5, "begin") == 0) ops.clear();
    }
}

std::string singleByteTable(int cp, unsigned code) {
    char c = (char)code;
    if (cp == 0) {
        /* StandardEncoding: кавычки отличаются от ASCII */
        if (code == 0x27) return "\xE2\x80\x99";
        if (code == 0x60) return "\xE2\x80\x98";
        return code < 0x80 ? std::string(1, c) : std::string();
    }
    std::wstring w(1, L'\0');
    if (MultiByteToWideChar((UINT)cp, 0, &c, 1, &w[0], 1) != 1) return std::string();
    return wToUtf8(w);
}

/* ============================== страница ============================== */

struct Mat {
    double a, b, c, d, e, f;
    Mat() : a(1), b(0), c(0), d(1), e(0), f(0) {}
    Mat(double A, double B, double C, double D, double E, double F) : a(A), b(B), c(C), d(D), e(E), f(F) {}
    Mat operator*(const Mat& m) const {
        return Mat(a * m.a + b * m.c, a * m.b + b * m.d, c * m.a + d * m.c, c * m.b + d * m.d,
                   e * m.a + f * m.c + m.e, e * m.b + f * m.d + m.f);
    }
};

struct TextItem {
    double x, y, x1, size;
    std::string text;
    bool bold, italic;
};

/* линия рамки: горизонтальная (pos = y, от a до b по x) или вертикальная (pos = x, от a до b по y) */
struct Rule {
    double pos, a, b;
};

/* картинка на странице: объект XObject и занятый прямоугольник */
struct ImagePlace {
    const PObj* obj;
    double x0, y0, x1, y1;
};

class PageReader {
public:
    /* шрифты общие для всего документа: ToUnicode разбирается один раз, а не на каждой странице */
    PageReader(PdfDoc& d, std::map<const PObj*, Font>& f)
        : doc(d), fonts(f), curX(0), curY(0), startX(0), startY(0) {}
    void run(const std::string& content, const PObj& resources, const Mat& base, int depth);
    std::vector<TextItem> items;
    std::vector<Rule> hRules, vRules;
    std::vector<ImagePlace> images;

private:
    PdfDoc& doc;
    std::map<const PObj*, Font>& fonts;

    /* текущий путь (в координатах страницы): прямоугольники и отрезки */
    struct Box { double x0, y0, x1, y1; };
    struct Seg2 { double x0, y0, x1, y1; };
    std::vector<Box> pathBoxes;
    std::vector<Seg2> pathSegs;
    double curX, curY, startX, startY;
    void paint(bool fill, bool stroke);
    void addRule(double x0, double y0, double x1, double y1);

    struct GState {
        Mat ctm;
        const Font* font;
        double fs, tc, tw, th, tl, rise;
        GState() : font(NULL), fs(10), tc(0), tw(0), th(1), tl(0), rise(0) {}
    };

    const Font& fontFor(const PObj& fd);
    void show(GState& g, Mat& tm, const std::string& bytes, std::string& run, double& runX, double& runY,
              bool& open, double& size);
};

const Font& PageReader::fontFor(const PObj& fdRef) {
    const PObj& fd = doc.resolve(fdRef);
    std::map<const PObj*, Font>::iterator it = fonts.find(&fd);
    if (it != fonts.end()) return it->second;
    Font f;
    const PObj* st = fd.get("Subtype");
    std::string sub = st ? doc.resolve(*st).str : std::string();
    const PObj* bf = fd.get("BaseFont");
    std::string base = bf ? lowerAscii(doc.resolve(*bf).str) : std::string();
    size_t plus = base.find('+');
    if (plus == 6) base = base.substr(7);
    f.bold = base.find("bold") != std::string::npos || base.find("black") != std::string::npos ||
             base.find("heavy") != std::string::npos || base.find("semibold") != std::string::npos ||
             base.find("demi") != std::string::npos;
    f.italic = base.find("italic") != std::string::npos || base.find("oblique") != std::string::npos ||
               base.find("kursiv") != std::string::npos;

    const PObj* desc = NULL;
    if (sub == "Type0") {
        f.bytes = 2;
        const PObj* df = fd.get("DescendantFonts");
        const PObj& dfa = df ? doc.resolve(*df) : kNull;
        const PObj& cid = dfa.t == P_ARR && !dfa.arr.empty() ? doc.resolve(dfa.arr[0]) : kNull;
        const PObj* dw = cid.get("DW");
        f.defWidth = (dw ? doc.resolve(*dw).num : 1000) / 1000.0;
        const PObj* W = cid.get("W");
        const PObj& wa = W ? doc.resolve(*W) : kNull;
        for (size_t i = 0; wa.t == P_ARR && i < wa.arr.size();) {
            const PObj& c0 = doc.resolve(wa.arr[i]);
            if (i + 1 >= wa.arr.size()) break;
            const PObj& nx = doc.resolve(wa.arr[i + 1]);
            if (nx.t == P_ARR) {
                for (size_t k = 0; k < nx.arr.size(); k++) f.widths[(unsigned)c0.intVal() + (unsigned)k] = doc.resolve(nx.arr[k]).num / 1000.0;
                i += 2;
            } else if (i + 2 < wa.arr.size()) {
                double w = doc.resolve(wa.arr[i + 2]).num / 1000.0;
                for (int c = c0.intVal(); c <= nx.intVal() && c - c0.intVal() < 65536; c++) f.widths[(unsigned)c] = w;
                i += 3;
            } else break;
        }
        desc = cid.get("FontDescriptor");
        const PObj* enc = fd.get("Encoding");
        if (enc && doc.resolve(*enc).t == P_NAME) {
            std::string e = doc.resolve(*enc).str;
            if (e.find("Identity") == std::string::npos && e.find("UCS2") == std::string::npos &&
                e.find("UTF16") == std::string::npos)
                f.bytes = 2; /* остальные CJK-кодировки: без CMap точный разбор невозможен */
        }
    } else {
        f.simple.assign(256, std::string());
        int cp = 1252;
        const PObj* enc = fd.get("Encoding");
        const PObj& er = enc ? doc.resolve(*enc) : kNull;
        std::string encName = er.t == P_NAME ? er.str : std::string();
        if (er.t == P_DICT && er.get("BaseEncoding")) encName = doc.resolve(*er.get("BaseEncoding")).str;
        if (encName == "MacRomanEncoding") cp = 10000;
        else if (encName == "StandardEncoding") cp = 0;
        for (unsigned c = 0; c < 256; c++) f.simple[c] = singleByteTable(cp, c);
        if (er.t == P_DICT) {
            const PObj* diff = er.get("Differences");
            const PObj& da = diff ? doc.resolve(*diff) : kNull;
            unsigned code = 0;
            for (size_t i = 0; da.t == P_ARR && i < da.arr.size(); i++) {
                const PObj& v = doc.resolve(da.arr[i]);
                if (v.t == P_NUM) code = (unsigned)v.intVal();
                else if (v.t == P_NAME) {
                    unsigned u = glyphToUnicode(v.str);
                    if (code < 256) {
                        if (u) { f.simple[code].clear(); appendUtf8(f.simple[code], u); }
                    }
                    code++;
                }
            }
        }
        const PObj* fc = fd.get("FirstChar");
        const PObj* ws = fd.get("Widths");
        const PObj& wa = ws ? doc.resolve(*ws) : kNull;
        int first = fc ? doc.resolve(*fc).intVal() : 0;
        for (size_t i = 0; wa.t == P_ARR && i < wa.arr.size(); i++)
            f.widths[(unsigned)(first + (int)i)] = doc.resolve(wa.arr[i]).num / 1000.0;
        desc = fd.get("FontDescriptor");
        if (sub == "Type3") f.defWidth = 0.5;
    }
    if (desc) {
        const PObj& d = doc.resolve(*desc);
        const PObj* flags = d.get("Flags");
        if (flags && (doc.resolve(*flags).intVal() & 64)) f.italic = true;
        const PObj* fw = d.get("FontWeight");
        if (fw && doc.resolve(*fw).num >= 600) f.bold = true;
        const PObj* ia = d.get("ItalicAngle");
        if (ia && fabs(doc.resolve(*ia).num) > 5) f.italic = true;
        const PObj* mw = d.get("MissingWidth");
        if (mw && doc.resolve(*mw).num > 0 && f.bytes == 1) f.defWidth = doc.resolve(*mw).num / 1000.0;
    }
    const PObj* tu = fd.get("ToUnicode");
    if (tu) {
        const PObj& ts = doc.resolve(*tu);
        std::string cm;
        if (ts.hasStream && doc.streamBytes(ts, cm)) {
            int bytes = f.bytes;
            parseToUnicode(cm, f);
            if (sub == "Type0") f.bytes = 2;
            else f.bytes = bytes;
        }
    }
    return fonts[&fd] = f;
}

void PageReader::show(GState& g, Mat& tm, const std::string& bytes, std::string& run, double& runX,
                      double& runY, bool& open, double& size) {
    if (!g.font) return;
    const Font& f = *g.font;
    for (size_t i = 0; i < bytes.size();) {
        unsigned code = (unsigned char)bytes[i++];
        if (f.bytes == 2 && i < bytes.size()) code = (code << 8) | (unsigned char)bytes[i++];
        Mat trm = Mat(g.fs * g.th, 0, 0, g.fs, 0, g.rise) * tm * g.ctm;
        if (!open) {
            runX = trm.e;
            runY = trm.f;
            Mat m = tm * g.ctm;
            size = fabs(g.fs) * sqrt(m.c * m.c + m.d * m.d);
            if (size < 0.1) size = fabs(g.fs);
            open = true;
        }
        run += f.text(code);
        double w = f.width(code) * g.fs + g.tc + ((f.bytes == 1 && code == 32) ? g.tw : 0);
        tm = Mat(1, 0, 0, 1, w * g.th, 0) * tm;
    }
}

void PageReader::addRule(double x0, double y0, double x1, double y1) {
    double dx = fabs(x1 - x0), dy = fabs(y1 - y0);
    Rule r;
    if (dy < 1.0 && dx > 2.0) {
        r.pos = (y0 + y1) / 2;
        r.a = std::min(x0, x1);
        r.b = std::max(x0, x1);
        hRules.push_back(r);
    } else if (dx < 1.0 && dy > 2.0) {
        r.pos = (x0 + x1) / 2;
        r.a = std::min(y0, y1);
        r.b = std::max(y0, y1);
        vRules.push_back(r);
    }
}

/* Рамки таблиц рисуют двумя способами: обводкой отрезков/прямоугольников (S) или заливкой
   тонких прямоугольников (так делает Word). Заливка толстых прямоугольников — фон, не линия. */
void PageReader::paint(bool fill, bool stroke) {
    for (size_t i = 0; i < pathBoxes.size(); i++) {
        const Box& b = pathBoxes[i];
        double w = b.x1 - b.x0, h = b.y1 - b.y0;
        if (stroke) {
            addRule(b.x0, b.y0, b.x1, b.y0);
            addRule(b.x0, b.y1, b.x1, b.y1);
            addRule(b.x0, b.y0, b.x0, b.y1);
            addRule(b.x1, b.y0, b.x1, b.y1);
        } else if (fill) {
            if (h <= 2.5 && w > 2.0 && w > h * 3) addRule(b.x0, (b.y0 + b.y1) / 2, b.x1, (b.y0 + b.y1) / 2);
            else if (w <= 2.5 && h > 2.0 && h > w * 3) addRule((b.x0 + b.x1) / 2, b.y0, (b.x0 + b.x1) / 2, b.y1);
        }
    }
    if (stroke)
        for (size_t i = 0; i < pathSegs.size(); i++) addRule(pathSegs[i].x0, pathSegs[i].y0, pathSegs[i].x1, pathSegs[i].y1);
    pathBoxes.clear();
    pathSegs.clear();
}

void PageReader::run(const std::string& content, const PObj& resources, const Mat& base, int depth) {
    if (depth > 8) return;
    const PObj& res = doc.resolve(resources);
    const PObj* fontRes = res.get("Font");
    const PObj& fontDict = fontRes ? doc.resolve(*fontRes) : kNull;
    const PObj* xoRes = res.get("XObject");
    const PObj& xoDict = xoRes ? doc.resolve(*xoRes) : kNull;

    GState g;
    g.ctm = base;
    std::vector<GState> stack;
    Mat tm, tlm;
    std::vector<PObj> ops;
    Lexer lx(content, 0, content.size());
    PObj o;

    std::string runText;
    double runX = 0, runY = 0, runSize = 0;
    bool runOpen = false;
    struct Flush {
        static void now(PageReader* self, GState& g, Mat& tm, std::string& t, double& x, double& y, bool& open, double size) {
            if (open && !t.empty()) {
                TextItem it;
                it.x = x;
                it.y = y;
                it.x1 = (tm * g.ctm).e;
                it.size = size;
                it.text = t;
                it.bold = g.font && g.font->bold;
                it.italic = g.font && g.font->italic;
                self->items.push_back(it);
            }
            t.clear();
            open = false;
        }
    };

    while (lx.parse(o, 0, false)) {
        if (o.t != P_OP) {
            ops.push_back(o);
            if (ops.size() > 4096) ops.erase(ops.begin());
            continue;
        }
        const std::string& op = o.str;
        size_t n = ops.size();
        #define NUM(i) (n > (size_t)(i) ? ops[n - 1 - (i)].num : 0.0)
        if (op == "BI") {
            /* встроенная картинка: пропускаем данные до EI */
            size_t e = content.find("ID", lx.p);
            if (e == std::string::npos) break;
            size_t q = e + 3;
            for (;;) {
                q = content.find("EI", q);
                if (q == std::string::npos) { lx.p = content.size(); break; }
                if (isWs((unsigned char)content[q - 1]) &&
                    (q + 2 >= content.size() || isWs((unsigned char)content[q + 2]))) { lx.p = q + 2; break; }
                q += 2;
            }
        } else if (op == "q") {
            stack.push_back(g);
        } else if (op == "Q") {
            if (!stack.empty()) { g = stack.back(); stack.pop_back(); }
        } else if (op == "cm" && n >= 6) {
            g.ctm = Mat(NUM(5), NUM(4), NUM(3), NUM(2), NUM(1), NUM(0)) * g.ctm;
        } else if (op == "BT") {
            tm = tlm = Mat();
        } else if (op == "ET") {
            Flush::now(this, g, tm, runText, runX, runY, runOpen, runSize);
        } else if (op == "Tf" && n >= 2) {
            Flush::now(this, g, tm, runText, runX, runY, runOpen, runSize);
            g.fs = NUM(0);
            const PObj& nameObj = ops[n - 2];
            const PObj* fr = fontDict.t == P_DICT ? fontDict.get(nameObj.str.c_str()) : NULL;
            g.font = fr ? &fontFor(*fr) : NULL;
        } else if (op == "Tc") g.tc = NUM(0);
        else if (op == "Tw") g.tw = NUM(0);
        else if (op == "Tz") g.th = NUM(0) / 100.0;
        else if (op == "TL") g.tl = NUM(0);
        else if (op == "Ts") g.rise = NUM(0);
        else if (op == "Td" || op == "TD" || op == "Tm" || op == "T*") {
            Flush::now(this, g, tm, runText, runX, runY, runOpen, runSize);
            if (op == "Tm" && n >= 6) tlm = Mat(NUM(5), NUM(4), NUM(3), NUM(2), NUM(1), NUM(0));
            else if (op == "T*") tlm = Mat(1, 0, 0, 1, 0, -g.tl) * tlm;
            else if (n >= 2) {
                if (op == "TD") g.tl = -NUM(0);
                tlm = Mat(1, 0, 0, 1, NUM(1), NUM(0)) * tlm;
            }
            tm = tlm;
        } else if ((op == "Tj" || op == "'" || op == "\"") && n >= 1) {
            if (op != "Tj") {
                Flush::now(this, g, tm, runText, runX, runY, runOpen, runSize);
                if (op == "\"" && n >= 3) { g.tw = NUM(2); g.tc = NUM(1); }
                tlm = Mat(1, 0, 0, 1, 0, -g.tl) * tlm;
                tm = tlm;
            }
            show(g, tm, ops[n - 1].str, runText, runX, runY, runOpen, runSize);
        } else if (op == "TJ" && n >= 1 && ops[n - 1].t == P_ARR) {
            const std::vector<PObj>& a = ops[n - 1].arr;
            for (size_t i = 0; i < a.size(); i++) {
                if (a[i].t == P_STR) show(g, tm, a[i].str, runText, runX, runY, runOpen, runSize);
                else if (a[i].t == P_NUM) {
                    double adj = -a[i].num / 1000.0 * g.fs * g.th;
                    tm = Mat(1, 0, 0, 1, adj, 0) * tm;
                    /* большой сдвиг вправо — это пробел между словами */
                    if (a[i].num < -180 && runOpen && !runText.empty() && runText[runText.size() - 1] != ' ')
                        runText += ' ';
                }
            }
        } else if (op == "m" && n >= 2) {
            const Mat& c = g.ctm;
            curX = startX = NUM(1) * c.a + NUM(0) * c.c + c.e;
            curY = startY = NUM(1) * c.b + NUM(0) * c.d + c.f;
        } else if (op == "l" && n >= 2) {
            const Mat& c = g.ctm;
            double x = NUM(1) * c.a + NUM(0) * c.c + c.e, y = NUM(1) * c.b + NUM(0) * c.d + c.f;
            Seg2 s = {curX, curY, x, y};
            pathSegs.push_back(s);
            curX = x;
            curY = y;
        } else if ((op == "c" && n >= 6) || ((op == "v" || op == "y") && n >= 4)) {
            const Mat& c = g.ctm; /* кривые в рамки таблиц не входят — только сдвигаем точку */
            curX = NUM(1) * c.a + NUM(0) * c.c + c.e;
            curY = NUM(1) * c.b + NUM(0) * c.d + c.f;
        } else if (op == "h") {
            Seg2 s = {curX, curY, startX, startY};
            pathSegs.push_back(s);
            curX = startX;
            curY = startY;
        } else if (op == "re" && n >= 4) {
            const Mat& c = g.ctm;
            double x = NUM(3), y = NUM(2), w = NUM(1), h = NUM(0);
            double px[4] = {x, x + w, x, x + w}, py[4] = {y, y, y + h, y + h};
            Box b = {1e18, 1e18, -1e18, -1e18};
            for (int k = 0; k < 4; k++) {
                double tx = px[k] * c.a + py[k] * c.c + c.e, ty = px[k] * c.b + py[k] * c.d + c.f;
                b.x0 = std::min(b.x0, tx); b.x1 = std::max(b.x1, tx);
                b.y0 = std::min(b.y0, ty); b.y1 = std::max(b.y1, ty);
            }
            pathBoxes.push_back(b);
            curX = startX = x;
            curY = startY = y;
        } else if (op == "S" || op == "s") {
            paint(false, true);
        } else if (op == "f" || op == "F" || op == "f*") {
            paint(true, false);
        } else if (op == "B" || op == "B*" || op == "b" || op == "b*") {
            paint(true, true);
        } else if (op == "n") {
            pathBoxes.clear(); /* путь для обрезки (W n) — не рисуется */
            pathSegs.clear();
        } else if (op == "Do" && n >= 1 && xoDict.t == P_DICT) {
            const PObj* xr = xoDict.get(ops[n - 1].str.c_str());
            if (xr) {
                const PObj& xo = doc.resolve(*xr);
                const PObj* st = xo.get("Subtype");
                if (st && doc.resolve(*st).str == "Image" && xo.hasStream) {
                    /* картинка занимает единичный квадрат, переведённый матрицей */
                    const Mat& c = g.ctm;
                    ImagePlace ip;
                    ip.obj = &xo;
                    ip.x0 = std::min(std::min(c.e, c.e + c.a), std::min(c.e + c.c, c.e + c.a + c.c));
                    ip.x1 = std::max(std::max(c.e, c.e + c.a), std::max(c.e + c.c, c.e + c.a + c.c));
                    ip.y0 = std::min(std::min(c.f, c.f + c.b), std::min(c.f + c.d, c.f + c.b + c.d));
                    ip.y1 = std::max(std::max(c.f, c.f + c.b), std::max(c.f + c.d, c.f + c.b + c.d));
                    images.push_back(ip);
                }
                if (st && doc.resolve(*st).str == "Form" && xo.hasStream) {
                    std::string sub;
                    if (doc.streamBytes(xo, sub)) {
                        Mat m;
                        const PObj* mm = xo.get("Matrix");
                        const PObj& ma = mm ? doc.resolve(*mm) : kNull;
                        if (ma.t == P_ARR && ma.arr.size() == 6)
                            m = Mat(ma.arr[0].num, ma.arr[1].num, ma.arr[2].num, ma.arr[3].num, ma.arr[4].num, ma.arr[5].num);
                        const PObj* xres = xo.get("Resources");
                        Flush::now(this, g, tm, runText, runX, runY, runOpen, runSize);
                        run(sub, xres ? *xres : resources, m * g.ctm, depth + 1);
                    }
                }
            }
        }
        #undef NUM
        ops.clear();
    }
    Flush::now(this, g, tm, runText, runX, runY, runOpen, runSize);
}

/* ============================== вёрстка =============================== */

struct Seg {
    std::string text;
    bool bold, italic, sup;
    double size;
};

struct Line {
    int page;
    double y, top, bottom, x0, x1, size;
    std::vector<TextItem> items;
    std::vector<Seg> segs;
    std::string text;
    bool drop;
    int heading;        /* уровень главы, -1 — обычная строка */
    std::string noteRef; /* строка — начало сноски с этим номером */
    int kind;           /* 0 — текст, 1 — картинка (img), 2 — таблица (html) */
    const PObj* img;
    std::string html;
    int region;         /* область страницы в порядке чтения (колонка или полоса на всю ширину) */
    Line() : page(0), y(0), top(0), bottom(0), x0(0), x1(0), size(0), drop(false), heading(-1), kind(0), img(NULL),
             region(0) {}
};

std::string normKey(const std::string& s) {
    std::string low = toLowerRu(collapse(s));
    std::string out;
    for (size_t i = 0; i < low.size(); i++) {
        unsigned char c = (unsigned char)low[i];
        if (c >= 0x80 || isalnum(c)) out += (char)c;
    }
    return out;
}

bool isBulletText(const std::string& t) {
    return startsWith(t, "\xE2\x80\xA2") || startsWith(t, "\xEF\x82\xB7") || startsWith(t, "\xE2\x97\x8F") ||
           startsWith(t, "\xE2\x96\xAA") || startsWith(t, "\xC2\xB7") || startsWith(t, "\xEF\x82\xA7");
}

std::string stripBullet(const std::string& t) {
    size_t p = 0;
    while (p < t.size() && ((unsigned char)t[p] & 0x80)) p++;
    if (p == 0) p = 1;
    return trim(t.substr(p));
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); i++) if (!isdigit((unsigned char)s[i])) return false;
    return true;
}

/* предметы одной страницы -> строки (по вертикальному перекрытию), сегменты, пробелы */
void buildLines(std::vector<TextItem>& items, int page, std::vector<Line>& out) {
    std::vector<Line> lines;
    for (size_t i = 0; i < items.size(); i++) {
        TextItem& it = items[i];
        if (collapse(it.text).empty() && it.text.find(' ') == std::string::npos) continue;
        double top = it.y + 0.8 * it.size, bottom = it.y - 0.2 * it.size;
        int best = -1;
        double bestOv = 0;
        for (size_t k = lines.size(); k-- > 0 && lines.size() - k <= 6;) {
            double ov = std::min(top, lines[k].top) - std::max(bottom, lines[k].bottom);
            double h = std::min(top - bottom, lines[k].top - lines[k].bottom);
            if (h > 0 && ov / h > bestOv) { bestOv = ov / h; best = (int)k; }
        }
        if (best >= 0 && bestOv >= 0.5) {
            Line& l = lines[(size_t)best];
            l.top = std::max(l.top, top);
            l.bottom = std::min(l.bottom, bottom);
            l.items.push_back(it);
        } else {
            Line l;
            l.page = page;
            l.top = top;
            l.bottom = bottom;
            l.items.push_back(it);
            lines.push_back(l);
        }
    }
    /* сверху вниз: порядок в потоке страницы не обязан совпадать с порядком чтения */
    for (size_t a = 1; a < lines.size(); a++)
        for (size_t b = a; b > 0 && lines[b].top > lines[b - 1].top + 0.01; b--) std::swap(lines[b], lines[b - 1]);
    for (size_t k = 0; k < lines.size(); k++) {
        Line& l = lines[k];
        /* основной размер — у предмета с наибольшим числом букв */
        std::map<int, size_t> bySize;
        for (size_t i = 0; i < l.items.size(); i++) bySize[(int)floor(l.items[i].size * 10 + 0.5)] += l.items[i].text.size();
        int mainKey = 0;
        size_t mainCount = 0;
        for (std::map<int, size_t>::iterator it = bySize.begin(); it != bySize.end(); ++it)
            if (it->second > mainCount) { mainCount = it->second; mainKey = it->first; }
        l.size = mainKey / 10.0;
        double baseY = 0;
        bool haveBase = false;
        for (size_t i = 0; i < l.items.size(); i++)
            if (fabs(l.items[i].size - l.size) < 0.15 && (!haveBase || l.items[i].y < baseY)) { baseY = l.items[i].y; haveBase = true; }
        l.y = haveBase ? baseY : l.items[0].y;

        /* слева направо */
        std::vector<TextItem>& it = l.items;
        for (size_t a = 1; a < it.size(); a++)
            for (size_t b = a; b > 0 && it[b].x < it[b - 1].x; b--) std::swap(it[b], it[b - 1]);
        l.x0 = it.front().x;
        l.x1 = it.back().x1;
        double prevEnd = -1e9;
        std::string prevText;
        double prevX = -1e9;
        for (size_t i = 0; i < it.size(); i++) {
            /* «жирный» двойной печатью: тот же текст почти в том же месте */
            if (it[i].text == prevText && fabs(it[i].x - prevX) < 0.3 * it[i].size) continue;
            Seg s;
            s.text = it[i].text;
            s.bold = it[i].bold;
            s.italic = it[i].italic;
            s.size = it[i].size;
            s.sup = it[i].size < 0.85 * l.size && it[i].y > l.y + 0.2 * l.size;
            double gap = it[i].x - prevEnd;
            bool needSpace = i > 0 && gap > 0.15 * l.size;
            if (needSpace && !l.segs.empty()) {
                Seg& p = l.segs.back();
                if (!p.text.empty() && p.text[p.text.size() - 1] != ' ' && s.text[0] != ' ') {
                    if (p.sup == s.sup && p.bold == s.bold && p.italic == s.italic) p.text += ' ';
                    else s.text = " " + s.text;
                }
            }
            if (!l.segs.empty() && l.segs.back().bold == s.bold && l.segs.back().italic == s.italic &&
                l.segs.back().sup == s.sup)
                l.segs.back().text += s.text;
            else
                l.segs.push_back(s);
            prevEnd = std::max(prevEnd, it[i].x1);
            prevText = it[i].text;
            prevX = it[i].x;
        }
        for (size_t i = 0; i < l.segs.size(); i++) l.text += l.segs[i].text;
        l.text = collapse(l.text);
        if (!l.text.empty()) out.push_back(l);
    }
}

std::string segsHtml(const std::vector<Seg>& segs, const std::map<std::string, std::string>* refIds) {
    std::string out;
    for (size_t i = 0; i < segs.size(); i++) {
        const Seg& s = segs[i];
        std::string t = escHtml(s.text);
        if (s.sup) {
            std::string num = collapse(s.text);
            if (refIds && refIds->count(num)) {
                out += "<a href=\"#" + refIds->find(num)->second + "\">[" + escHtml(num) + "]</a>";
                continue;
            }
            t = "<sup>" + t + "</sup>";
        }
        if (s.bold) t = "<strong>" + t + "</strong>";
        if (s.italic) t = "<em>" + t + "</em>";
        out += t;
    }
    return collapse(out);
}

/* ============================== картинки ============================== */

void put32(std::string& s, unsigned v) {
    s += (char)(v >> 24); s += (char)(v >> 16); s += (char)(v >> 8); s += (char)v;
}

void pngChunk(std::string& png, const char* type, const std::string& data) {
    put32(png, (unsigned)data.size());
    std::string td = std::string(type, 4) + data;
    png += td;
    put32(png, (unsigned)crc32(0, (const Bytef*)td.data(), (uInt)td.size()));
}

/* PNG из готовых строк пикселей (без байта фильтра). colorType: 0 серый, 2 RGB, 3 палитра,
   4 серый+альфа, 6 RGBA */
std::string encodePng(int w, int h, int colorType, int depth, const std::string& pixels, const std::string& palette) {
    int ch = colorType == 2 ? 3 : colorType == 4 ? 2 : colorType == 6 ? 4 : 1;
    size_t rowBytes = ((size_t)w * ch * depth + 7) / 8;
    std::string raw;
    raw.reserve((rowBytes + 1) * (size_t)h);
    for (int y = 0; y < h; y++) {
        raw += '\0';
        raw.append(pixels, (size_t)y * rowBytes, rowBytes);
    }
    uLongf zl = compressBound((uLong)raw.size());
    std::string z((size_t)zl, '\0');
    if (compress2((Bytef*)&z[0], &zl, (const Bytef*)raw.data(), (uLong)raw.size(), 9) != Z_OK) return std::string();
    z.resize((size_t)zl);
    std::string png("\x89PNG\r\n\x1a\n", 8), ihdr;
    put32(ihdr, (unsigned)w);
    put32(ihdr, (unsigned)h);
    ihdr += (char)depth;
    ihdr += (char)colorType;
    ihdr += std::string(3, '\0');
    pngChunk(png, "IHDR", ihdr);
    if (colorType == 3) pngChunk(png, "PLTE", palette);
    pngChunk(png, "IDAT", z);
    pngChunk(png, "IEND", std::string());
    return png;
}

/* цветовое пространство картинки */
struct ColorSpace {
    int comps;           /* компонентов в выборке */
    bool indexed, cmyk, invertTint;
    std::string palette; /* для Indexed: RGB по 3 байта */
    bool ok;
    ColorSpace() : comps(0), indexed(false), cmyk(false), invertTint(false), ok(false) {}
};

void cmykToRgb(double c, double m, double y, double k, unsigned char* rgb) {
    rgb[0] = (unsigned char)(255 * (1 - c) * (1 - k) + 0.5);
    rgb[1] = (unsigned char)(255 * (1 - m) * (1 - k) + 0.5);
    rgb[2] = (unsigned char)(255 * (1 - y) * (1 - k) + 0.5);
}

ColorSpace readColorSpace(PdfDoc& doc, const PObj& csIn, int depth) {
    ColorSpace cs;
    const PObj& c = doc.resolve(csIn);
    if (depth > 4) return cs;
    std::string name = c.t == P_NAME ? c.str : (c.t == P_ARR && !c.arr.empty() ? doc.resolve(c.arr[0]).str : std::string());
    if (name == "DeviceGray" || name == "CalGray" || name == "G") { cs.comps = 1; cs.ok = true; }
    else if (name == "DeviceRGB" || name == "CalRGB" || name == "RGB") { cs.comps = 3; cs.ok = true; }
    else if (name == "DeviceCMYK" || name == "CMYK") { cs.comps = 4; cs.cmyk = true; cs.ok = true; }
    else if (name == "ICCBased" && c.arr.size() >= 2) {
        const PObj& icc = doc.resolve(c.arr[1]);
        const PObj* N = icc.get("N");
        cs.comps = N ? doc.resolve(*N).intVal() : 3;
        cs.cmyk = cs.comps == 4;
        cs.ok = cs.comps == 1 || cs.comps == 3 || cs.comps == 4;
    } else if (name == "Separation" && c.arr.size() >= 3) {
        cs.comps = 1; /* одна краска: 1 — полная, то есть тёмная */
        cs.invertTint = true;
        cs.ok = true;
    } else if ((name == "Indexed" || name == "I") && c.arr.size() >= 4) {
        ColorSpace base = readColorSpace(doc, c.arr[1], depth + 1);
        int hival = doc.resolve(c.arr[2]).intVal();
        const PObj& lk = doc.resolve(c.arr[3]);
        std::string table;
        if (lk.t == P_STR) table = lk.str;
        else if (lk.hasStream) doc.streamBytes(lk, table);
        if (!base.ok || base.indexed || hival < 0 || hival > 255) return cs;
        for (int i = 0; i <= hival; i++) {
            unsigned char rgb[3] = {0, 0, 0};
            size_t at = (size_t)i * (size_t)base.comps;
            if (at + (size_t)base.comps > table.size()) break;
            const unsigned char* p = (const unsigned char*)table.data() + at;
            if (base.comps == 1) rgb[0] = rgb[1] = rgb[2] = base.invertTint ? (unsigned char)(255 - p[0]) : p[0];
            else if (base.comps == 3) { rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2]; }
            else cmykToRgb(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0, p[3] / 255.0, rgb);
            cs.palette.append((const char*)rgb, 3);
        }
        cs.comps = 1;
        cs.indexed = true;
        cs.ok = !cs.palette.empty();
    }
    return cs;
}

/* выборка: строки выровнены на байт; значение приведено к 0..255 (кроме индексов палитры) */
struct Samples {
    int w, h, bpc, comps;
    std::string data;
    size_t rowBytes() const { return ((size_t)w * comps * bpc + 7) / 8; }
    unsigned raw(int x, int y, int c) const {
        size_t bit = (size_t)y * rowBytes() * 8 + ((size_t)x * comps + c) * bpc;
        size_t byte = bit / 8;
        if (bpc == 16) return byte + 1 < data.size() ? (unsigned char)data[byte] : 0; /* старший байт */
        if (byte >= data.size()) return 0;
        unsigned v = (unsigned char)data[byte];
        if (bpc == 8) return v;
        return (v >> (8 - bpc - (bit % 8))) & ((1u << bpc) - 1);
    }
    unsigned scaled(int x, int y, int c) const {
        unsigned v = raw(x, y, c);
        if (bpc >= 8) return v;
        return v * 255 / ((1u << bpc) - 1);
    }
};

bool readSamples(PdfDoc& doc, const PObj& img, Samples& s, int comps) {
    const PObj* W = img.get("Width");
    const PObj* H = img.get("Height");
    const PObj* B = img.get("BitsPerComponent");
    s.w = W ? doc.resolve(*W).intVal() : 0;
    s.h = H ? doc.resolve(*H).intVal() : 0;
    s.bpc = B ? doc.resolve(*B).intVal() : 8;
    s.comps = comps;
    if (s.w <= 0 || s.h <= 0 || s.w > 20000 || s.h > 20000 || (s.bpc != 1 && s.bpc != 2 && s.bpc != 4 && s.bpc != 8 && s.bpc != 16))
        return false;
    if (!doc.streamBytes(img, s.data)) return false;
    size_t need = s.rowBytes() * (size_t)s.h;
    if (s.data.size() < need) s.data.resize(need, '\0'); /* обрезанный поток — дополняем */
    return true;
}

/* последний фильтр потока: DCT/JPX/CCITT/JBIG2 — сжатые картинки, остальное — сырые выборки */
std::string lastFilter(PdfDoc& doc, const PObj& img) {
    const PObj* f = img.get("Filter");
    if (!f) return std::string();
    const PObj& fr = doc.resolve(*f);
    if (fr.t == P_NAME) return fr.str;
    if (fr.t == P_ARR && !fr.arr.empty()) return doc.resolve(fr.arr.back()).str;
    return std::string();
}

/* картинка PDF -> JPEG (как есть) или PNG (собранный здесь) */
bool pdfImageFile(PdfDoc& doc, const PObj& img, std::string& mime, std::string& out) {
    const PObj* im = img.get("ImageMask");
    if (im && doc.resolve(*im).num != 0) return false; /* трафарет-заливка, не картинка */
    const PObj* csRef = img.get("ColorSpace");
    ColorSpace cs = csRef ? readColorSpace(doc, *csRef, 0) : ColorSpace();
    std::string filter = lastFilter(doc, img);
    if (filter == "DCTDecode" || filter == "DCT") {
        if (cs.cmyk) return false; /* CMYK-JPEG читалки показывают неверными цветами */
        mime = "image/jpeg";
        return doc.streamBytes(img, out) && out.size() > 2 && (unsigned char)out[0] == 0xFF;
    }
    if (filter == "JPXDecode" || filter == "CCITTFaxDecode" || filter == "CCF" || filter == "JBIG2Decode") return false;
    if (!cs.ok) return false;

    Samples s;
    if (!readSamples(doc, img, s, cs.comps)) return false;
    int w = s.w, h = s.h;
    bool invert = false;
    const PObj* dec = img.get("Decode");
    if (dec) {
        const PObj& d = doc.resolve(*dec);
        if (d.t == P_ARR && d.arr.size() >= 2 && doc.resolve(d.arr[0]).num > doc.resolve(d.arr[1]).num && !cs.indexed)
            invert = true;
    }
    if (cs.invertTint) invert = !invert;

    /* маска прозрачности */
    std::vector<unsigned char> alpha;
    std::vector<double> matte;
    const PObj* sm = img.get("SMask");
    if (sm) {
        const PObj& smo = doc.resolve(*sm);
        Samples a;
        if (smo.hasStream && lastFilter(doc, smo) != "DCTDecode" && readSamples(doc, smo, a, 1)) {
            alpha.resize((size_t)w * h);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    alpha[(size_t)y * w + x] = (unsigned char)a.scaled(x * a.w / w, y * a.h / h, 0);
            const PObj* mt = smo.get("Matte");
            const PObj& ma = mt ? doc.resolve(*mt) : kNull;
            for (size_t i = 0; ma.t == P_ARR && i < ma.arr.size(); i++) matte.push_back(doc.resolve(ma.arr[i]).num * 255);
            bool opaque = true;
            for (size_t i = 0; i < alpha.size() && opaque; i++) if (alpha[i] != 255) opaque = false;
            if (opaque) alpha.clear();
        }
    }

    std::string pix;
    if (alpha.empty() && (cs.indexed || cs.comps == 1) && s.bpc <= 8 && !invert) {
        /* палитра или серый без альфы — строки PDF и PNG совпадают побайтно */
        pix = s.data.substr(0, s.rowBytes() * (size_t)h);
        mime = "image/png";
        out = encodePng(w, h, cs.indexed ? 3 : 0, s.bpc, pix, cs.palette);
        return !out.empty();
    }
    bool gray = !cs.indexed && cs.comps == 1;
    int outCh = (gray ? 1 : 3) + (alpha.empty() ? 0 : 1);
    pix.resize((size_t)w * h * outCh);
    size_t o = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char px[4];
            if (cs.indexed) {
                unsigned idx = s.raw(x, y, 0);
                size_t at = (size_t)idx * 3;
                if (at + 3 <= cs.palette.size()) memcpy(px, cs.palette.data() + at, 3);
                else px[0] = px[1] = px[2] = 0;
            } else if (cs.cmyk) {
                cmykToRgb(s.scaled(x, y, 0) / 255.0, s.scaled(x, y, 1) / 255.0, s.scaled(x, y, 2) / 255.0,
                          s.scaled(x, y, 3) / 255.0, px);
            } else {
                for (int c = 0; c < cs.comps; c++) px[c] = (unsigned char)s.scaled(x, y, c);
            }
            int n = gray ? 1 : 3;
            if (invert) for (int c = 0; c < n; c++) px[c] = (unsigned char)(255 - px[c]);
            if (!alpha.empty()) {
                unsigned char a = alpha[(size_t)y * w + x];
                /* Matte: цвет записан уже умноженным на прозрачность — возвращаем исходный */
                if (!matte.empty() && a > 0 && a < 255)
                    for (int c = 0; c < n && c < (int)matte.size(); c++) {
                        double v = matte[c] + (px[c] - matte[c]) * 255.0 / a;
                        px[c] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
                    }
                px[n] = a;
            }
            for (int c = 0; c < outCh; c++) pix[o++] = (char)px[c];
        }
    }
    mime = "image/png";
    int type = gray ? (alpha.empty() ? 0 : 4) : (alpha.empty() ? 2 : 6);
    out = encodePng(w, h, type, 8, pix, std::string());
    return !out.empty();
}

/* =============================== таблицы ============================== */

struct TableOut {
    double top, bottom, x0, x1;
    std::string html;
    TableOut() : top(0), bottom(0), x0(0), x1(0) {}
};

void mergeRules(std::vector<Rule>& r) {
    for (size_t a = 1; a < r.size(); a++)
        for (size_t b = a; b > 0 && (r[b].pos < r[b - 1].pos - 0.01 ||
                                     (fabs(r[b].pos - r[b - 1].pos) < 0.01 && r[b].a < r[b - 1].a)); b--)
            std::swap(r[b], r[b - 1]);
    std::vector<Rule> out;
    for (size_t i = 0; i < r.size(); i++) {
        if (!out.empty() && fabs(r[i].pos - out.back().pos) < 1.5 && r[i].a <= out.back().b + 2) {
            out.back().b = std::max(out.back().b, r[i].b);
            continue;
        }
        out.push_back(r[i]);
    }
    r.clear();
    for (size_t i = 0; i < out.size(); i++) if (out[i].b - out[i].a >= 4) r.push_back(out[i]);
}

std::vector<double> clusterPositions(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    std::vector<double> out;
    for (size_t i = 0; i < v.size(); i++)
        if (out.empty() || v[i] - out.back() > 2.0) out.push_back(v[i]);
    return out;
}

/* Таблица с рамками: связные группы пересекающихся линий -> сетка; ячейка без внутренней
   линии справа/снизу объединяется с соседней (colspan/rowspan). Текст внутри сетки
   раскладывается по ячейкам и из обычного текста страницы убирается. */
void detectTables(std::vector<Rule> H, std::vector<Rule> V, std::vector<TextItem>& items, int page,
                  std::vector<TableOut>& out) {
    mergeRules(H);
    mergeRules(V);
    if (H.size() < 2 || V.size() < 2 || H.size() + V.size() > 4000) return;
    size_t n = H.size() + V.size();
    std::vector<size_t> parent(n);
    for (size_t i = 0; i < n; i++) parent[i] = i;
    struct UF {
        static size_t find(std::vector<size_t>& p, size_t x) {
            while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
            return x;
        }
    };
    for (size_t i = 0; i < H.size(); i++)
        for (size_t j = 0; j < V.size(); j++) {
            const Rule& h = H[i];
            const Rule& v = V[j];
            if (v.pos >= h.a - 2 && v.pos <= h.b + 2 && h.pos >= v.a - 2 && h.pos <= v.b + 2)
                parent[UF::find(parent, i)] = UF::find(parent, H.size() + j);
        }
    std::map<size_t, std::vector<size_t> > groups;
    for (size_t i = 0; i < n; i++) groups[UF::find(parent, i)].push_back(i);

    std::vector<bool> taken(items.size(), false);
    for (std::map<size_t, std::vector<size_t> >::iterator g = groups.begin(); g != groups.end(); ++g) {
        std::vector<Rule> gh, gv;
        for (size_t k = 0; k < g->second.size(); k++) {
            size_t id = g->second[k];
            if (id < H.size()) gh.push_back(H[id]);
            else gv.push_back(V[id - H.size()]);
        }
        if (gh.size() < 2 || gv.size() < 2) continue;
        std::vector<double> xs, ys;
        for (size_t k = 0; k < gv.size(); k++) xs.push_back(gv[k].pos);
        for (size_t k = 0; k < gh.size(); k++) ys.push_back(gh[k].pos);
        xs = clusterPositions(xs);
        ys = clusterPositions(ys);
        std::reverse(ys.begin(), ys.end()); /* сверху вниз */
        if (xs.size() < 2 || ys.size() < 2) continue;
        int rows = (int)ys.size() - 1, cols = (int)xs.size() - 1;
        if (rows * cols < 2 || rows > 500 || cols > 60) continue; /* рамка вокруг абзаца — не таблица */

        struct Has {
            static bool v(const std::vector<Rule>& r, double x, double ymid) {
                for (size_t i = 0; i < r.size(); i++)
                    if (fabs(r[i].pos - x) < 2 && r[i].a <= ymid + 1 && r[i].b >= ymid - 1) return true;
                return false;
            }
        };
        std::vector<std::vector<int> > owner((size_t)rows, std::vector<int>((size_t)cols, -1));
        struct Cell { int r, c, rs, cs; std::vector<TextItem> items; };
        std::vector<Cell> cells;
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++) {
                if (owner[(size_t)r][(size_t)c] >= 0) continue;
                double ymid = (ys[(size_t)r] + ys[(size_t)r + 1]) / 2;
                int cs = 1;
                while (c + cs < cols && owner[(size_t)r][(size_t)(c + cs)] < 0 && !Has::v(gv, xs[(size_t)(c + cs)], ymid)) cs++;
                double xmid = (xs[(size_t)c] + xs[(size_t)(c + cs)]) / 2;
                int rs = 1;
                while (r + rs < rows && !Has::v(gh, ys[(size_t)(r + rs)], xmid)) rs++;
                Cell cell = {r, c, rs, cs, std::vector<TextItem>()};
                for (int a = r; a < r + rs; a++)
                    for (int b = c; b < c + cs; b++) owner[(size_t)a][(size_t)b] = (int)cells.size();
                cells.push_back(cell);
            }

        bool any = false;
        std::vector<size_t> mine;
        for (size_t i = 0; i < items.size(); i++) {
            if (taken[i]) continue;
            const TextItem& it = items[i];
            double cx = (it.x + it.x1) / 2, cy = it.y + 0.3 * it.size;
            if (cx < xs.front() - 1 || cx > xs.back() + 1 || cy > ys.front() + 1 || cy < ys.back() - 1) continue;
            int c = 0, r = 0;
            while (c + 1 < cols && cx > xs[(size_t)c + 1]) c++;
            while (r + 1 < rows && cy < ys[(size_t)r + 1]) r++;
            cells[(size_t)owner[(size_t)r][(size_t)c]].items.push_back(it);
            mine.push_back(i);
            if (!collapse(it.text).empty()) any = true;
        }
        if (!any) continue; /* пустая сетка — украшение */
        for (size_t k = 0; k < mine.size(); k++) taken[mine[k]] = true;

        std::string html = "<table>";
        for (int r = 0; r < rows; r++) {
            std::string tr;
            for (size_t k = 0; k < cells.size(); k++) {
                Cell& cell = cells[k];
                if (cell.r != r) continue;
                std::vector<Line> cl;
                buildLines(cell.items, page, cl);
                std::string text;
                for (size_t m = 0; m < cl.size(); m++) {
                    if (!text.empty()) text += "<br/>";
                    text += segsHtml(cl[m].segs, NULL);
                }
                tr += "<td";
                if (cell.cs > 1) tr += " colspan=\"" + intToStr(cell.cs) + "\"";
                if (cell.rs > 1) tr += " rowspan=\"" + intToStr(cell.rs) + "\"";
                tr += ">" + text + "</td>";
            }
            if (!tr.empty()) html += "<tr>" + tr + "</tr>";
        }
        html += "</table>";
        TableOut t;
        t.top = ys.front();
        t.bottom = ys.back();
        t.x0 = xs.front();
        t.x1 = xs.back();
        t.html = html;
        out.push_back(t);
    }
    std::vector<TextItem> rest;
    for (size_t i = 0; i < items.size(); i++) if (!taken[i]) rest.push_back(items[i]);
    items.swap(rest);
}

/* ===================== колонки и таблицы без рамок ===================== */

/* «Кусок» строки: слова, между которыми нет большого промежутка */
struct Chunk {
    double x0, x1;
    std::vector<size_t> idx;
    std::string text;
};

struct RawLine {
    double top, bottom, size;
    std::vector<size_t> idx; /* все предметы строки, слева направо */
    std::vector<Chunk> chunks;
};

std::vector<Chunk> splitChunks(const std::vector<TextItem>& items, const std::vector<size_t>& idx, double gap) {
    std::vector<Chunk> out;
    for (size_t k = 0; k < idx.size(); k++) {
        const TextItem& it = items[idx[k]];
        if (!out.empty() && it.x - out.back().x1 <= gap) {
            out.back().x1 = std::max(out.back().x1, it.x1);
            out.back().idx.push_back(idx[k]);
            out.back().text += it.text;
            continue;
        }
        Chunk c;
        c.x0 = it.x;
        c.x1 = it.x1;
        c.idx.push_back(idx[k]);
        c.text = it.text;
        out.push_back(c);
    }
    return out;
}

/* строки страницы по вертикальному перекрытию — для анализа вёрстки, до сборки текста */
std::vector<RawLine> rawLines(const std::vector<TextItem>& items, const std::vector<size_t>& use, double chunkGap) {
    std::vector<size_t> order;
    for (size_t k = 0; k < use.size(); k++)
        if (!collapse(items[use[k]].text).empty()) order.push_back(use[k]); /* пробелы между колонками — не мост */
    for (size_t a = 1; a < order.size(); a++)
        for (size_t b = a; b > 0; b--) {
            const TextItem& p = items[order[b - 1]];
            const TextItem& q = items[order[b]];
            if (q.y + 0.8 * q.size > p.y + 0.8 * p.size + 0.01) std::swap(order[b], order[b - 1]);
            else break;
        }
    std::vector<RawLine> lines;
    for (size_t k = 0; k < order.size(); k++) {
        const TextItem& it = items[order[k]];
        double top = it.y + 0.8 * it.size, bottom = it.y - 0.2 * it.size;
        int best = -1;
        double bestOv = 0;
        for (size_t m = lines.size(); m-- > 0 && lines.size() - m <= 8;) {
            double ov = std::min(top, lines[m].top) - std::max(bottom, lines[m].bottom);
            double h = std::min(top - bottom, lines[m].top - lines[m].bottom);
            if (h > 0 && ov / h > bestOv) { bestOv = ov / h; best = (int)m; }
        }
        if (best >= 0 && bestOv >= 0.5) {
            RawLine& l = lines[(size_t)best];
            l.top = std::max(l.top, top);
            l.bottom = std::min(l.bottom, bottom);
            l.size = std::max(l.size, it.size);
            l.idx.push_back(order[k]);
        } else {
            RawLine l;
            l.top = top;
            l.bottom = bottom;
            l.size = it.size;
            l.idx.push_back(order[k]);
            lines.push_back(l);
        }
    }
    for (size_t m = 0; m < lines.size(); m++) {
        std::vector<size_t>& v = lines[m].idx;
        for (size_t a = 1; a < v.size(); a++)
            for (size_t b = a; b > 0 && items[v[b]].x < items[v[b - 1]].x; b--) std::swap(v[b], v[b - 1]);
        lines[m].chunks = splitChunks(items, v, chunkGap * lines[m].size);
    }
    return lines;
}

struct Gutter {
    double a, b;
};

FILE* layoutLog = NULL; /* SHARKONV_PDFDEBUG: разбор вёрстки по страницам */

/* строка пересекает межколонник, только если перекрывает его целиком (заголовок на всю ширину);
   строка с неровным правым краем может заходить в полосу, но до другой колонки не доходит */
bool crosses(const RawLine& l, const Gutter& g) {
    for (size_t k = 0; k < l.chunks.size(); k++)
        if (l.chunks[k].x0 < g.a + 1 && l.chunks[k].x1 > g.b - 1) return true;
    return false;
}

struct ColumnBlock {
    size_t from, to;            /* строки [from, to) */
    std::vector<double> edges;  /* границы колонок: x0, (a1+b1)/2, ..., x1 */
};

/* Межколонник — вертикальная полоса, которую не пересекает ни одна строка блока, а у многих
   строк текст есть по обе стороны. Колонки прозы отличаются от таблицы «наполненностью»:
   строки прозы занимают колонку почти целиком, ячейки таблицы — короткие. */
std::vector<ColumnBlock> findColumns(const std::vector<RawLine>& L, double body) {
    /* Гистограмма по горизонтали (шаг 1 pt): сколько строк покрывают текстом каждую точку.
       Межколонник — полоса, где текста намного меньше, чем в соседних полосах слева и справа
       (строки на всю ширину над колонками её пересекают, но их мало), а у её краёв много строк
       кончается слева и начинается справа. Выравнивание строк колонок по высоте не нужно:
       заголовок в одной колонке сдвигает её строки относительно соседней. */
    double minX = 1e18, maxX = -1e18;
    for (size_t i = 0; i < L.size(); i++)
        for (size_t k = 0; k < L[i].chunks.size(); k++) {
            minX = std::min(minX, L[i].chunks[k].x0);
            maxX = std::max(maxX, L[i].chunks[k].x1);
        }
    std::vector<Gutter> groups;
    std::vector<int> counts;
    if (maxX > minX && maxX - minX < 5000) {
        size_t nb = (size_t)(maxX - minX) + 2;
        std::vector<int> textCnt(nb, 0);
        for (size_t i = 0; i < L.size(); i++)
            for (size_t k = 0; k < L[i].chunks.size(); k++) {
                const Chunk& c = L[i].chunks[k];
                for (size_t x = (size_t)(c.x0 - minX); x <= (size_t)(c.x1 - minX) && x < nb; x++) textCnt[x]++;
            }
        size_t win = (size_t)(8 * body);
        if (layoutLog) {
            fprintf(layoutLog, "  coverage from x=%.0f, every 10 pt:", minX);
            for (size_t x = 0; x < nb; x += 10) fprintf(layoutLog, " %d", textCnt[x]);
            fprintf(layoutLog, "\n");
        }
        std::vector<bool> low(nb, false);
        for (size_t x = 0; x < nb; x++) {
            int l = 0, r = 0;
            for (size_t k = x > win ? x - win : 0; k < x; k++) l = std::max(l, textCnt[k]);
            for (size_t k = x + 1; k < nb && k <= x + win; k++) r = std::max(r, textCnt[k]);
            low[x] = l >= 4 && r >= 4 && textCnt[x] * 4 <= std::min(l, r);
        }
        size_t x = 0;
        while (x < nb) {
            if (!low[x]) { x++; continue; }
            size_t s = x;
            while (x < nb && low[x]) x++;
            if ((double)(x - s) < 0.6 * body || s == 0 || x >= nb) continue;
            Gutter g = {minX + s, minX + x};
            /* у краёв полосы строки должны кончаться (слева) и начинаться (справа) */
            int ends = 0, starts = 0;
            for (size_t i = 0; i < L.size(); i++)
                for (size_t k = 0; k < L[i].chunks.size(); k++) {
                    const Chunk& c = L[i].chunks[k];
                    if (c.x1 <= g.a + 0.5 && c.x1 >= g.a - 3 * body) ends++;
                    if (c.x0 >= g.b - 0.5 && c.x0 <= g.b + 3 * body) starts++;
                }
            if (layoutLog) fprintf(layoutLog, "  empty strip %.1f-%.1f: %d lines end left, %d start right\n", g.a, g.b, ends, starts);
            if (ends >= 4 && starts >= 4) {
                groups.push_back(g);
                counts.push_back(std::min(ends, starts));
            }
        }
    }

    struct Cand { size_t from, to; Gutter g; };
    std::vector<Cand> cands;
    for (size_t k = 0; k < groups.size(); k++) {
        if (layoutLog && counts[k] >= 2)
            fprintf(layoutLog, "  gutter %.1f-%.1f seen in %d lines\n", groups[k].a, groups[k].b, counts[k]);
        if (counts[k] < 4) continue;
        const Gutter& g = groups[k];
        size_t i = 0;
        while (i < L.size()) {
            if (crosses(L[i], g)) { i++; continue; }
            size_t j = i;
            int lefts = 0, rights = 0;
            while (j < L.size() && !crosses(L[j], g)) {
                for (size_t c = 0; c < L[j].chunks.size(); c++) {
                    if (L[j].chunks[c].x1 <= g.a + 0.5) { lefts++; break; }
                }
                for (size_t c = 0; c < L[j].chunks.size(); c++) {
                    if (L[j].chunks[c].x0 >= g.b - 0.5) { rights++; break; }
                }
                j++;
            }
            if (lefts >= 3 && rights >= 3) {
                Cand c = {i, j, g};
                cands.push_back(c);
            }
            i = j;
        }
    }

    std::vector<ColumnBlock> out;
    std::vector<bool> used(cands.size(), false);
    for (size_t a = 0; a < cands.size(); a++) {
        if (used[a]) continue;
        size_t from = cands[a].from, to = cands[a].to;
        std::vector<Gutter> gs(1, cands[a].g);
        used[a] = true;
        for (size_t b = a + 1; b < cands.size(); b++) {
            if (used[b] || cands[b].to <= from || cands[b].from >= to) continue;
            from = std::max(from, cands[b].from);
            to = std::min(to, cands[b].to);
            gs.push_back(cands[b].g);
            used[b] = true;
        }
        if (to <= from || to - from < 3) continue;
        for (size_t x = 1; x < gs.size(); x++)
            for (size_t y = x; y > 0 && gs[y].a < gs[y - 1].a; y--) std::swap(gs[y], gs[y - 1]);
        double x0 = 1e18, x1 = -1e18;
        for (size_t i = from; i < to; i++)
            for (size_t c = 0; c < L[i].chunks.size(); c++) {
                x0 = std::min(x0, L[i].chunks[c].x0);
                x1 = std::max(x1, L[i].chunks[c].x1);
            }
        ColumnBlock blk;
        blk.from = from;
        blk.to = to;
        blk.edges.push_back(x0);
        for (size_t k = 0; k < gs.size(); k++) blk.edges.push_back((gs[k].a + gs[k].b) / 2);
        blk.edges.push_back(x1);
        /* узкие колонки или короткие строки — это таблица, а не колонки текста */
        bool wide = true;
        for (size_t k = 0; k + 1 < blk.edges.size(); k++)
            if (blk.edges[k + 1] - blk.edges[k] < 6 * body) wide = false;
        size_t filled = 0, full = 0;
        for (size_t i = from; i < to; i++)
            for (size_t k = 0; k + 1 < blk.edges.size(); k++) {
                double c0 = k == 0 ? blk.edges[0] : gs[k - 1].b, c1 = k + 1 == blk.edges.size() - 1 ? blk.edges.back() : gs[k].a;
                double w = 0;
                for (size_t c = 0; c < L[i].chunks.size(); c++) {
                    double m = (L[i].chunks[c].x0 + L[i].chunks[c].x1) / 2;
                    if (m >= c0 && m <= c1) w += L[i].chunks[c].x1 - L[i].chunks[c].x0;
                }
                if (w > 0) { filled++; if (w >= 0.7 * (c1 - c0)) full++; }
            }
        if (layoutLog) {
            fprintf(layoutLog, "  block lines %u-%u, %u columns, wide=%d, full %u of %u\n", (unsigned)from, (unsigned)to,
                    (unsigned)(blk.edges.size() - 1), (int)wide, (unsigned)full, (unsigned)filled);
        }
        if (!wide || filled == 0 || full * 10 < filled * 6) continue;
        out.push_back(blk);
    }
    for (size_t x = 1; x < out.size(); x++)
        for (size_t y = x; y > 0 && out[y].from < out[y - 1].from; y--) std::swap(out[y], out[y - 1]);
    return out;
}

bool isListMarker(const std::string& t) {
    std::string s = collapse(t);
    if (s.empty() || utf8Len(s) > 4) return false;
    if (isBulletText(s) || s == "-" || s == "*" || s == "\xE2\x80\x93" || s == "\xE2\x80\x94") return true;
    char last = s[s.size() - 1];
    if (last != '.' && last != ')') return false;
    std::string core = s.substr(0, s.size() - 1);
    return allDigits(core) || (utf8Len(core) == 1);
}

/* Таблица без рамок: подряд идущие строки из нескольких «кусков», выровненных в столбцы.
   Строка, у которой пуст первый столбец, — продолжение ячеек предыдущей строки. */
void detectGridTables(std::vector<TextItem>& items, std::vector<RawLine>& L, int page, double body,
                      std::vector<TableOut>& out, std::vector<bool>& taken) {
    std::vector<std::vector<Chunk> > ch(L.size());
    for (size_t i = 0; i < L.size(); i++) ch[i] = splitChunks(items, L[i].idx, 1.5 * L[i].size);
    size_t i = 0;
    while (i < L.size()) {
        bool row = ch[i].size() >= 2 && !isListMarker(ch[i][0].text);
        if (!row) { i++; continue; }
        /* серия: строки-ряды и строки-продолжения между ними */
        size_t j = i + 1, lastRow = i;
        int rows = 1;
        while (j < L.size()) {
            double gap = L[j - 1].bottom - L[j].top;
            if (gap > 1.5 * body) break;
            bool r = ch[j].size() >= 2 && !isListMarker(ch[j][0].text);
            bool cont = !r && ch[j].size() >= 1 && ch[j][0].x0 > ch[i][0].x1 + 0.5 * body;
            if (!r && !cont) break;
            if (r) { rows++; lastRow = j; }
            j++;
        }
        j = lastRow + 1;
        /* хвост-продолжения после последнего ряда тоже относятся к таблице */
        while (j < L.size() && ch[j].size() == 1 && ch[j][0].x0 > ch[i][0].x1 + 0.5 * body &&
               L[j - 1].bottom - L[j].top <= 1.5 * body)
            j++;
        if (rows < 2) { i++; continue; }

        /* столбцы: объединение промежутков кусков по всем рядам */
        std::vector<std::pair<double, double> > iv;
        for (size_t k = i; k < j; k++)
            for (size_t c = 0; c < ch[k].size(); c++) iv.push_back(std::make_pair(ch[k][c].x0, ch[k][c].x1));
        std::sort(iv.begin(), iv.end());
        std::vector<std::pair<double, double> > cols;
        for (size_t k = 0; k < iv.size(); k++) {
            if (!cols.empty() && iv[k].first <= cols.back().second + 0.3 * body)
                cols.back().second = std::max(cols.back().second, iv[k].second);
            else
                cols.push_back(iv[k]);
        }
        if (cols.size() < 2 || cols.size() > 20) { i = j; continue; }

        std::vector<std::vector<std::string> > cells;
        for (size_t k = i; k < j; k++) {
            std::vector<std::string> rowCells(cols.size());
            bool firstEmpty = true;
            for (size_t c = 0; c < ch[k].size(); c++) {
                double m = (ch[k][c].x0 + ch[k][c].x1) / 2;
                size_t col = 0;
                while (col + 1 < cols.size() && m > cols[col].second) col++;
                if (col == 0) firstEmpty = false;
                std::vector<TextItem> part;
                for (size_t t = 0; t < ch[k][c].idx.size(); t++) part.push_back(items[ch[k][c].idx[t]]);
                std::vector<Line> pl;
                buildLines(part, page, pl);
                std::string h;
                for (size_t t = 0; t < pl.size(); t++) h += (h.empty() ? "" : " ") + segsHtml(pl[t].segs, NULL);
                rowCells[col] += (rowCells[col].empty() ? "" : " ") + h;
            }
            if (firstEmpty && !cells.empty()) {
                for (size_t c = 0; c < cols.size(); c++)
                    if (!rowCells[c].empty()) cells.back()[c] += (cells.back()[c].empty() ? "" : "<br/>") + rowCells[c];
            } else {
                cells.push_back(rowCells);
            }
            for (size_t t = 0; t < L[k].idx.size(); t++) taken[L[k].idx[t]] = true;
        }
        std::string html = "<table>";
        for (size_t r = 0; r < cells.size(); r++) {
            html += "<tr>";
            for (size_t c = 0; c < cells[r].size(); c++) html += "<td>" + cells[r][c] + "</td>";
            html += "</tr>";
        }
        TableOut t;
        t.top = L[i].top;
        t.bottom = L[j - 1].bottom;
        t.x0 = cols.front().first;
        t.x1 = cols.back().second;
        t.html = html + "</table>";
        out.push_back(t);
        i = j;
    }
}

/* Страница -> строки в порядке чтения: область над колонками, колонки слева направо, область
   под ними и т. д. Каждая область получает свой номер (Line::region): по нему абзац знает,
   что перешёл в следующую колонку, а отступы считаются для каждой колонки отдельно. */
void layoutPage(std::vector<TextItem>& items, int page, int& regionSeq, std::vector<Line>& out) {
    std::map<int, size_t> bySize;
    for (size_t i = 0; i < items.size(); i++) bySize[(int)floor(items[i].size * 2 + 0.5)] += items[i].text.size();
    int key = 0;
    size_t best = 0;
    for (std::map<int, size_t>::iterator it = bySize.begin(); it != bySize.end(); ++it)
        if (it->second > best) { best = it->second; key = it->first; }
    double body = key > 0 ? key / 2.0 : 10;

    std::vector<size_t> all;
    for (size_t i = 0; i < items.size(); i++) all.push_back(i);
    std::vector<RawLine> L = rawLines(items, all, 0.8);
    if (layoutLog) fprintf(layoutLog, "page %d: %u lines, body %.1f\n", page + 1, (unsigned)L.size(), body);
    std::vector<ColumnBlock> blocks = findColumns(L, body);

    struct Region {
        std::vector<size_t> lines; /* строки страницы (одноколоночная область) */
        std::vector<size_t> items; /* предметы (колонка) */
        bool column;
    };
    std::vector<Region> regions;
    size_t b = 0;
    for (size_t i = 0; i < L.size();) {
        if (b < blocks.size() && i == blocks[b].from) {
            const ColumnBlock& blk = blocks[b];
            size_t first = regions.size();
            for (size_t k = 0; k + 1 < blk.edges.size(); k++) {
                Region r;
                r.column = true;
                regions.push_back(r);
            }
            for (size_t m = blk.from; m < blk.to; m++)
                for (size_t t = 0; t < L[m].idx.size(); t++) {
                    const TextItem& it = items[L[m].idx[t]];
                    double c = (it.x + it.x1) / 2;
                    size_t col = 0;
                    while (col + 2 < blk.edges.size() && c > blk.edges[col + 1]) col++;
                    regions[first + col].items.push_back(L[m].idx[t]);
                }
            i = blk.to;
            b++;
            continue;
        }
        if (regions.empty() || regions.back().column) {
            Region r;
            r.column = false;
            regions.push_back(r);
        }
        regions.back().lines.push_back(i);
        i++;
    }

    std::vector<bool> taken(items.size(), false);
    for (size_t r = 0; r < regions.size(); r++) {
        Region& reg = regions[r];
        int id = regionSeq++;
        std::vector<TableOut> grid;
        if (!reg.column) {
            std::vector<RawLine> part;
            for (size_t k = 0; k < reg.lines.size(); k++) part.push_back(L[reg.lines[k]]);
            detectGridTables(items, part, page, body, grid, taken);
            for (size_t k = 0; k < reg.lines.size(); k++)
                for (size_t t = 0; t < L[reg.lines[k]].idx.size(); t++) reg.items.push_back(L[reg.lines[k]].idx[t]);
        }
        std::vector<TextItem> mine;
        for (size_t k = 0; k < reg.items.size(); k++)
            if (!taken[reg.items[k]]) mine.push_back(items[reg.items[k]]);
        std::vector<Line> lines;
        buildLines(mine, page, lines);
        for (size_t t = 0; t < grid.size(); t++) {
            Line l;
            l.page = page;
            l.kind = 2;
            l.html = grid[t].html;
            l.top = l.y = grid[t].top;
            l.bottom = grid[t].bottom;
            l.x0 = grid[t].x0;
            l.x1 = grid[t].x1;
            lines.push_back(l);
        }
        for (size_t a = 1; a < lines.size(); a++)
            for (size_t c = a; c > 0 && lines[c].top > lines[c - 1].top + 0.01; c--) std::swap(lines[c], lines[c - 1]);
        for (size_t t = 0; t < lines.size(); t++) {
            lines[t].region = id;
            out.push_back(lines[t]);
        }
    }
}

struct OutlineItem {
    std::string title;
    int level;
    int page;   /* индекс страницы, -1 — неизвестно */
    double y;   /* верх места на странице, NAN — неизвестно */
};

bool endsSentence(const std::string& t) {
    if (t.empty()) return false;
    std::string e = t.substr(t.size() > 4 ? t.size() - 4 : 0);
    char c = t[t.size() - 1];
    return c == '.' || c == '!' || c == '?' || c == ':' || c == ';' ||
           e.find("\xE2\x80\xA6") != std::string::npos || e.find("\xC2\xBB") != std::string::npos ||
           c == '"' || c == ')';
}

bool startsLower(const std::string& t) {
    unsigned cp = utf8CharAt(t, 0);
    return (cp >= 'a' && cp <= 'z') || (cp >= 0x430 && cp <= 0x44F) || cp == 0x451;
}

class PdfBook {
public:
    PdfBook(PdfDoc& d, Book& b) : doc(d), book(b), regionSeq(0) {}
    bool read(std::string* err);

private:
    PdfDoc& doc;
    Book& book;
    std::vector<const PObj*> pages;
    std::vector<double> pageH, pageW;
    std::map<int, int> pageIndexByObj;
    std::vector<OutlineItem> outline;
    std::map<const PObj*, Font> fontCache;
    int regionSeq;

    void collectPages(const PObj& node, const PObj* inheritedRes, int depth);
    std::vector<const PObj*> pageRes;
    int pageOf(const PObj& dest);
    void readOutline(const PObj& first, int level, int depth);
    bool destPos(const PObj& dest, int* page, double* y);
};

void PdfBook::collectPages(const PObj& nodeRef, const PObj* inheritedRes, int depth) {
    if (depth > 64 || pages.size() > 20000) return;
    const PObj& node = doc.resolve(nodeRef);
    const PObj* res = node.get("Resources");
    if (!res) res = inheritedRes;
    const PObj* kids = node.get("Kids");
    const PObj* type = node.get("Type");
    if (kids && (!type || doc.resolve(*type).str != "Page")) {
        const PObj& k = doc.resolve(*kids);
        for (size_t i = 0; k.t == P_ARR && i < k.arr.size(); i++) {
            if (k.arr[i].t == P_REF) pageIndexByObj[k.arr[i].ref] = -1;
            collectPages(k.arr[i], res, depth + 1);
        }
        return;
    }
    if (nodeRef.t == P_REF) pageIndexByObj[nodeRef.ref] = (int)pages.size();
    pages.push_back(&node);
    pageRes.push_back(res);
    const PObj* mb = node.get("MediaBox");
    const PObj& m = mb ? doc.resolve(*mb) : kNull;
    pageH.push_back(m.t == P_ARR && m.arr.size() == 4 ? doc.resolve(m.arr[3]).num - doc.resolve(m.arr[1]).num : 842);
    pageW.push_back(m.t == P_ARR && m.arr.size() == 4 ? doc.resolve(m.arr[2]).num - doc.resolve(m.arr[0]).num : 595);
}

bool PdfBook::destPos(const PObj& destIn, int* page, double* y) {
    const PObj* dp = &doc.resolve(destIn);
    /* именованное назначение: /Dests в каталоге или дерево /Names /Dests */
    if (dp->t == P_STR || dp->t == P_NAME) {
        const PObj& root = doc.resolve(*doc.trailer().get("Root"));
        std::string key = dp->str;
        const PObj* dests = root.get("Dests");
        const PObj* found = NULL;
        if (dests) {
            const PObj& dd = doc.resolve(*dests);
            found = dd.get(key.c_str());
        }
        const PObj* names = root.get("Names");
        if (!found && names) {
            const PObj* nd = doc.resolve(*names).get("Dests");
            std::vector<const PObj*> todo;
            if (nd) todo.push_back(&doc.resolve(*nd));
            for (size_t guard = 0; !todo.empty() && !found && guard < 100000; guard++) {
                const PObj* t = todo.back();
                todo.pop_back();
                const PObj* nm = t->get("Names");
                if (nm) {
                    const PObj& na = doc.resolve(*nm);
                    for (size_t i = 0; na.t == P_ARR && i + 1 < na.arr.size(); i += 2)
                        if (doc.resolve(na.arr[i]).str == key) { found = &na.arr[i + 1]; break; }
                }
                const PObj* kids = t->get("Kids");
                if (kids) {
                    const PObj& ka = doc.resolve(*kids);
                    for (size_t i = 0; ka.t == P_ARR && i < ka.arr.size(); i++) todo.push_back(&doc.resolve(ka.arr[i]));
                }
            }
        }
        if (!found) return false;
        dp = &doc.resolve(*found);
    }
    if (dp->t == P_DICT && dp->get("D")) dp = &doc.resolve(*dp->get("D"));
    if (dp->t != P_ARR || dp->arr.empty()) return false;
    const PObj& pg = dp->arr[0];
    if (pg.t == P_REF && pageIndexByObj.count(pg.ref)) *page = pageIndexByObj[pg.ref];
    else if (pg.t == P_NUM) *page = pg.intVal();
    else return false;
    *y = NAN;
    if (dp->arr.size() >= 4 && doc.resolve(dp->arr[1]).str == "XYZ" && doc.resolve(dp->arr[3]).t == P_NUM)
        *y = doc.resolve(dp->arr[3]).num;
    else if (dp->arr.size() >= 3 && (doc.resolve(dp->arr[1]).str == "FitH" || doc.resolve(dp->arr[1]).str == "FitBH") &&
             doc.resolve(dp->arr[2]).t == P_NUM)
        *y = doc.resolve(dp->arr[2]).num;
    return *page >= 0;
}

void PdfBook::readOutline(const PObj& firstRef, int level, int depth) {
    const PObj* cur = &doc.resolve(firstRef);
    std::set<const PObj*> seen;
    while (cur && cur->t == P_DICT && depth < 32 && !seen.count(cur) && outline.size() < 20000) {
        seen.insert(cur);
        OutlineItem it;
        const PObj* t = cur->get("Title");
        it.title = t ? collapse(pdfText(doc.resolve(*t).str)) : std::string();
        it.level = level;
        it.page = -1;
        it.y = NAN;
        const PObj* d = cur->get("Dest");
        const PObj* a = cur->get("A");
        if (d) destPos(*d, &it.page, &it.y);
        else if (a) {
            const PObj& ad = doc.resolve(*a);
            const PObj* ddd = ad.get("D");
            if (ddd) destPos(*ddd, &it.page, &it.y);
        }
        if (!it.title.empty()) outline.push_back(it);
        const PObj* fk = cur->get("First");
        if (fk) readOutline(*fk, level + 1, depth + 1);
        const PObj* nx = cur->get("Next");
        cur = nx ? &doc.resolve(*nx) : NULL;
    }
}

bool PdfBook::read(std::string* err) {
    const PObj* rootRef = doc.trailer().get("Root");
    const PObj& root = rootRef ? doc.resolve(*rootRef) : kNull;
    const PObj* pagesRef = root.get("Pages");
    if (!pagesRef) {
        if (err) *err = "В PDF не найден список страниц";
        return false;
    }
    collectPages(*pagesRef, NULL, 0);
    if (pages.empty()) {
        if (err) *err = "В PDF нет страниц";
        return false;
    }

    /* метаданные */
    const PObj* infoRef = doc.trailer().get("Info");
    if (infoRef) {
        const PObj& info = doc.resolve(*infoRef);
        const PObj* t = info.get("Title");
        if (t) book.meta.title = collapse(pdfText(doc.resolve(*t).str));
        const PObj* a = info.get("Author");
        std::string au = a ? collapse(pdfText(doc.resolve(*a).str)) : std::string();
        if (!au.empty()) book.meta.authors.push_back(splitAuthor(au, false));
        const PObj* s = info.get("Subject");
        std::string sub = s ? collapse(pdfText(doc.resolve(*s).str)) : std::string();
        if (!sub.empty()) book.meta.annotation = "<p>" + escHtml(sub) + "</p>";
    }
    const PObj* ol = root.get("Outlines");
    if (ol) {
        const PObj& o = doc.resolve(*ol);
        const PObj* f = o.get("First");
        if (f) readOutline(*f, 0, 0);
    }

    /* текст всех страниц */
    const char* dbgEnv = getenv("SHARKONV_PDFDEBUG");
    if (dbgEnv && *dbgEnv) layoutLog = fopen((std::string(dbgEnv) + ".layout").c_str(), "wb");
    struct LogCloser {
        ~LogCloser() { if (layoutLog) { fclose(layoutLog); layoutLog = NULL; } }
    } logCloser;
    std::vector<Line> lines;
    size_t chars = 0, mapped = 0;
    for (size_t p = 0; p < pages.size(); p++) {
        const PObj& pg = *pages[p];
        const PObj* c = pg.get("Contents");
        if (!c) continue;
        std::string content;
        const PObj& cr = doc.resolve(*c);
        if (cr.t == P_ARR) {
            for (size_t i = 0; i < cr.arr.size(); i++) {
                std::string part;
                if (doc.streamBytes(doc.resolve(cr.arr[i]), part)) content += part + "\n";
            }
        } else {
            doc.streamBytes(cr, content);
        }
        PageReader pr(doc, fontCache);
        PObj noRes;
        pr.run(content, pageRes[p] ? *pageRes[p] : noRes, Mat(), 0);
        for (size_t i = 0; i < pr.items.size(); i++) {
            chars += pr.items[i].text.size() + 1;
            mapped += collapse(pr.items[i].text).size();
        }
        /* таблицы забирают свой текст, остальное — обычные строки; картинки и таблицы встают
           в поток по своему верхнему краю */
        std::vector<TableOut> tables;
        detectTables(pr.hRules, pr.vRules, pr.items, (int)p, tables);
        std::vector<Line> pageLines;
        int firstRegion = regionSeq;
        layoutPage(pr.items, (int)p, regionSeq, pageLines);
        std::vector<Line> specials;
        for (size_t t = 0; t < tables.size(); t++) {
            Line l;
            l.page = (int)p;
            l.kind = 2;
            l.html = tables[t].html;
            l.top = l.y = tables[t].top;
            l.bottom = tables[t].bottom;
            l.x0 = tables[t].x0;
            l.x1 = tables[t].x1;
            specials.push_back(l);
        }
        double pageArea = pageW[p] * pageH[p];
        for (size_t i = 0; i < pr.images.size(); i++) {
            const ImagePlace& ip = pr.images[i];
            double w = ip.x1 - ip.x0, h = ip.y1 - ip.y0;
            if (w < 16 || h < 16) continue;                                     /* значки, линейки */
            if (w * h > pageArea * 0.85 && !pr.items.empty()) continue;         /* фон-скан под текстом */
            Line l;
            l.page = (int)p;
            l.kind = 1;
            l.img = ip.obj;
            l.top = ip.y1;
            l.y = l.bottom = ip.y0;
            l.x0 = ip.x0;
            l.x1 = ip.x1;
            specials.push_back(l);
        }
        /* таблица с рамками или картинка — в ту область, где ближайшая строка над ней по той же
           ширине; если такой нет — в первую область страницы */
        for (size_t s = 0; s < specials.size(); s++) {
            Line& sp = specials[s];
            sp.region = firstRegion;
            double bestDist = 1e18;
            for (size_t k = 0; k < pageLines.size(); k++) {
                const Line& l = pageLines[k];
                bool overlap = std::min(l.x1, sp.x1) - std::max(l.x0, sp.x0) > 0;
                double dist = l.bottom - sp.top;
                if (overlap && dist >= -2 && dist < bestDist) { bestDist = dist; sp.region = l.region; }
            }
            if (bestDist == 1e18)
                for (size_t k = 0; k < pageLines.size(); k++) {
                    const Line& l = pageLines[k];
                    if (std::min(l.x1, sp.x1) - std::max(l.x0, sp.x0) > 0) { sp.region = l.region; break; }
                }
            pageLines.push_back(sp);
        }
        /* порядок чтения: область за областью, внутри области сверху вниз */
        for (size_t a = 1; a < pageLines.size(); a++)
            for (size_t b = a; b > 0 && (pageLines[b].region < pageLines[b - 1].region ||
                                         (pageLines[b].region == pageLines[b - 1].region &&
                                          pageLines[b].top > pageLines[b - 1].top + 0.01)); b--)
                std::swap(pageLines[b], pageLines[b - 1]);
        lines.insert(lines.end(), pageLines.begin(), pageLines.end());
    }
    /* одна и та же картинка на многих страницах — логотип или фон, не иллюстрация */
    {
        std::map<const PObj*, std::set<int> > imgPages;
        for (size_t i = 0; i < lines.size(); i++)
            if (lines[i].kind == 1) imgPages[lines[i].img].insert(lines[i].page);
        for (size_t i = 0; i < lines.size(); i++)
            if (lines[i].kind == 1 && imgPages[lines[i].img].size() >= 3 && imgPages[lines[i].img].size() * 2 >= pages.size())
                lines[i].drop = true;
    }
    /* SHARKONV_PDFDEBUG=файл — строки страниц, как их увидел разбор (для разбора жалоб) */
    const char* dbg = getenv("SHARKONV_PDFDEBUG");
    if (dbg && *dbg) {
        FILE* f = fopen(dbg, "wb");
        if (f) {
            for (size_t i = 0; i < lines.size(); i++)
                fprintf(f, "p%d r%d y=%.1f x=%.1f-%.1f size=%.1f | %s\n", lines[i].page + 1, lines[i].region, lines[i].y,
                        lines[i].x0, lines[i].x1, lines[i].size,
                        lines[i].kind == 1 ? "[картинка]" : lines[i].kind == 2 ? lines[i].html.c_str() : lines[i].text.c_str());
            fclose(f);
        }
    }
    /* PDF из одних картинок (комикс, скан без текстового слоя) — книга из картинок */
    bool anyText = false, anyImage = false;
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].kind == 0) anyText = true;
        else if (!lines[i].drop) anyImage = true;
    }
    if (!anyText && !anyImage) {
        if (err) *err = mapped == 0 && chars > 0
                            ? "В PDF шрифты без таблицы символов — текст не извлекается"
                            : "В PDF нет текстового слоя (похоже на скан — нужно распознавание текста)";
        return false;
    }

    /* размер основного шрифта и левое поле */
    std::map<int, size_t> sizeChars, leftCount, rightCount;
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].kind == 0) sizeChars[(int)floor(lines[i].size * 2 + 0.5)] += lines[i].text.size();
    }
    int bodyKey = 0;
    size_t best = 0;
    for (std::map<int, size_t>::iterator it = sizeChars.begin(); it != sizeChars.end(); ++it)
        if (it->second > best) { best = it->second; bodyKey = it->first; }
    double body = bodyKey / 2.0;
    for (size_t i = 0; i < lines.size(); i++)
        if (fabs(lines[i].size - body) < 0.6) {
            leftCount[(int)floor(lines[i].x0 + 0.5)]++;
            rightCount[(int)floor(lines[i].x1 + 0.5)]++;
        }
    int left = 0;
    best = 0;
    for (std::map<int, size_t>::iterator it = leftCount.begin(); it != leftCount.end(); ++it)
        if (it->second > best) { best = it->second; left = it->first; }
    /* правое поле: 90-й процентиль правых краёв строк основного текста */
    std::vector<double> rights;
    for (size_t i = 0; i < lines.size(); i++) if (fabs(lines[i].size - body) < 0.6) rights.push_back(lines[i].x1);
    std::sort(rights.begin(), rights.end());
    double right = rights.empty() ? 500 : rights[(size_t)(rights.size() * 0.9) >= rights.size() ? rights.size() - 1 : (size_t)(rights.size() * 0.9)];
    /* в коротком тексте все строки могут быть короткими — тогда край набора ближе к краю страницы */
    std::vector<double> widths(pageW);
    std::sort(widths.begin(), widths.end());
    if (!widths.empty()) right = std::max(right, widths[widths.size() / 2] - left);

    /* колонтитулы и номера страниц: у самого края, только цифры — или одна и та же строка на многих страницах */
    std::map<std::string, std::set<int> > repeats;
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].kind) continue;
        double h = pageH[(size_t)lines[i].page];
        bool edge = lines[i].y > h * 0.9 || lines[i].y < h * 0.08;
        std::string key = lines[i].text;
        for (size_t k = 0; k < key.size(); k++) if (isdigit((unsigned char)key[k])) key[k] = '#';
        if (edge) repeats[key].insert(lines[i].page);
    }
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].kind) continue;
        double h = pageH[(size_t)lines[i].page];
        bool edge = lines[i].y > h * 0.9 || lines[i].y < h * 0.08;
        if (!edge) continue;
        std::string t = lines[i].text;
        std::string key = t;
        for (size_t k = 0; k < key.size(); k++) if (isdigit((unsigned char)key[k])) key[k] = '#';
        std::string digits = t;
        digits.erase(std::remove(digits.begin(), digits.end(), '-'), digits.end());
        digits = collapse(replaceAll(digits, " ", ""));
        /* колонтитул отделён от текста увеличенным промежутком; строка текста у края страницы
           (пусть и повторяющаяся) идёт через обычный межстрочный интервал */
        bool isolated = true;
        for (size_t k = 0; k < lines.size(); k++) {
            if (k == i || lines[k].page != lines[i].page || lines[k].kind) continue;
            double d = fabs(lines[k].y - lines[i].y);
            bool horizOverlap = std::min(lines[k].x1, lines[i].x1) - std::max(lines[k].x0, lines[i].x0) > 0;
            if (horizOverlap && d > 0.5 && d < 1.6 * std::max(lines[i].size, lines[k].size)) { isolated = false; break; }
        }
        if (allDigits(digits) || (isolated && repeats[key].size() >= 3 && repeats[key].size() * 2 >= pages.size() / 2))
            lines[i].drop = true;
    }

    /* сноски: внизу страницы мелкий текст, начинающийся с номера, на который есть надстрочная
       ссылка выше на той же странице */
    std::map<std::string, std::string> refIds;  /* "страница:номер" -> id сноски */
    std::map<int, std::map<std::string, std::string> > pageRefs;
    for (size_t i = 0; i < lines.size(); i++) {
        Line& l = lines[i];
        if (l.drop || l.size < body * 0.95) continue;
        for (size_t k = 0; k < l.segs.size(); k++)
            if (l.segs[k].sup && allDigits(collapse(l.segs[k].text)) && collapse(l.segs[k].text).size() <= 3)
                pageRefs[l.page][collapse(l.segs[k].text)] = std::string();
    }
    std::vector<Note> notes;
    for (size_t i = 0; i < lines.size(); i++) {
        Line& l = lines[i];
        if (l.kind || l.drop || l.size >= body * 0.92 || !pageRefs.count(l.page)) continue;
        /* только строки ниже последней строки основного текста на этой странице */
        bool bodyBelow = false;
        for (size_t k = i + 1; k < lines.size() && lines[k].page == l.page; k++)
            if (!lines[k].drop && fabs(lines[k].size - body) < 0.6 && lines[k].y < l.y) bodyBelow = true;
        if (bodyBelow) continue;
        std::string num;
        if (!l.segs.empty() && l.segs[0].sup) num = collapse(l.segs[0].text);
        else {
            size_t sp = l.text.find(' ');
            num = l.text.substr(0, sp);
            while (!num.empty() && strchr(".)", num[num.size() - 1])) num.erase(num.size() - 1);
        }
        std::map<std::string, std::string>& refs = pageRefs[l.page];
        if (allDigits(num) && refs.count(num) && refs[num].empty()) {
            Note n;
            n.id = "pdfnote" + intToStr((long long)notes.size() + 1);
            n.title = num;
            std::vector<Seg> rest = l.segs;
            if (!rest.empty() && rest[0].sup) rest.erase(rest.begin());
            else if (!rest.empty()) {
                std::string& t = rest[0].text;
                size_t at = t.find(num);
                if (at != std::string::npos) t = trim(t.substr(at + num.size()));
                while (!t.empty() && strchr(".)", t[0])) t = trim(t.substr(1));
            }
            n.blocks.push_back(Block::p(segsHtml(rest, NULL)));
            notes.push_back(n);
            refs[num] = n.id;
            l.drop = true;
            /* продолжение сноски на следующих строках — до следующего номера */
            for (size_t k = i + 1; k < lines.size() && lines[k].page == l.page; k++) {
                Line& c = lines[k];
                if (c.kind || c.drop || c.size >= body * 0.92) break;
                std::string cn = !c.segs.empty() && c.segs[0].sup ? collapse(c.segs[0].text) : std::string();
                if (!cn.empty() && refs.count(cn)) break;
                std::string& prev = notes.back().blocks.back().html;
                prev += " " + segsHtml(c.segs, NULL);
                c.drop = true;
                i = k;
            }
        }
    }
    for (size_t i = 0; i < notes.size(); i++) book.notes.push_back(notes[i]);

    /* главы по закладкам: строка страницы, с которой начинается текст закладки */
    std::vector<bool> used(lines.size(), false);
    bool haveOutline = false;
    for (size_t o = 0; o < outline.size(); o++) {
        OutlineItem& it = outline[o];
        if (it.page < 0) continue;
        std::string want = normKey(it.title);
        if (want.empty()) continue;
        for (size_t i = 0; i < lines.size(); i++) {
            Line& l = lines[i];
            if (l.kind || l.page != it.page || l.drop || used[i] || l.heading >= 0) continue;
            if (it.y == it.y && l.top > it.y + l.size * 1.5) continue; /* it.y == it.y: не NAN */
            /* заголовок может занимать до трёх строк */
            std::string acc;
            size_t j = i;
            for (; j < lines.size() && j < i + 3 && lines[j].page == l.page && !lines[j].kind; j++) {
                acc += normKey(lines[j].text);
                if (acc.size() >= want.size()) break;
            }
            if (acc.compare(0, want.size(), want) == 0 || (want.size() > 6 && acc.size() >= 6 && want.compare(0, acc.size(), acc) == 0 && acc.size() * 10 >= want.size() * 9)) {
                l.heading = it.level;
                l.text = it.title;
                for (size_t k = i + 1; k <= j && k < lines.size(); k++) lines[k].drop = true;
                used[i] = true;
                haveOutline = true;
                break;
            }
        }
    }
    /* без закладок — заголовки по размеру шрифта: короткие строки крупнее основного текста */
    if (!haveOutline) {
        std::set<int> bigSizes;
        for (size_t i = 0; i < lines.size(); i++)
            if (!lines[i].drop && lines[i].size >= body * 1.15 && utf8Len(lines[i].text) < 120)
                bigSizes.insert(-(int)floor(lines[i].size * 2 + 0.5));
        std::map<int, int> levelOf;
        int lv = 0;
        for (std::set<int>::iterator it = bigSizes.begin(); it != bigSizes.end() && lv < 4; ++it) levelOf[*it] = lv++;
        for (size_t i = 0; i < lines.size(); i++) {
            int key = -(int)floor(lines[i].size * 2 + 0.5);
            if (!lines[i].drop && levelOf.count(key) && utf8Len(lines[i].text) < 120) lines[i].heading = levelOf[key];
        }
        /* заголовок, перенесённый на две строки того же размера */
        for (size_t i = 0; i + 1 < lines.size(); i++) {
            if (lines[i].heading < 0 || lines[i + 1].heading != lines[i].heading || lines[i + 1].page != lines[i].page) continue;
            if (lines[i].y - lines[i + 1].y > lines[i].size * 1.6) continue;
            lines[i].text += " " + lines[i + 1].text;
            lines[i + 1].drop = true;
        }
    }
    /* PDF без названия в свойствах: крупная первая строка первой страницы (не закладка) — название */
    if (book.meta.title.empty()) {
        for (size_t i = 0; i < lines.size(); i++) {
            if (lines[i].drop || lines[i].kind) continue;
            if (lines[i].page == 0 && lines[i].heading < 0 && lines[i].size >= body * 1.4 && utf8Len(lines[i].text) < 150) {
                book.meta.title = lines[i].text;
                /* название, перенесённое на следующие строки того же кегля (узкая колонка) */
                for (size_t k = i + 1; k < lines.size() && k < i + 4; k++) {
                    const Line& n = lines[k];
                    if (n.drop || n.kind || n.region != lines[i].region || fabs(n.size - lines[i].size) > 0.3 ||
                        lines[k - 1].y - n.y > n.size * 1.6)
                        break;
                    book.meta.title += " " + n.text;
                    lines[k].drop = true;
                }
                lines[i].text = book.meta.title; /* строка ниже узнается как название и уберётся */
            }
            break;
        }
    }
    /* название книги крупным шрифтом в начале первой страницы — не глава */
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].drop || lines[i].kind) continue;
        if (lines[i].page == 0 && normKey(lines[i].text) == normKey(book.meta.title) && !book.meta.title.empty() &&
            lines[i].size > body * 1.15)
            lines[i].drop = true;
        break;
    }

    /* абзацы */
    std::vector<Chapter> chapters(1);
    std::string para;
    bool paraList = false;
    double paraX0 = 0;
    const Line* prev = NULL;
    double gapNorm = body * 1.2;
    {
        std::vector<double> gaps;
        const Line* pl = NULL;
        for (size_t i = 0; i < lines.size(); i++) {
            const Line& l = lines[i];
            if (l.drop || l.heading >= 0 || fabs(l.size - body) > 0.6) { pl = NULL; continue; }
            if (pl && pl->region == l.region && pl->y > l.y) gaps.push_back(pl->y - l.y);
            pl = &l;
        }
        std::sort(gaps.begin(), gaps.end());
        if (!gaps.empty()) gapNorm = gaps[gaps.size() / 2];
    }
    /* поля каждой области (колонки): левое — самое частое начало строки, правое — 90-й процентиль
       концов; у области из пары строк — поля всей книги */
    std::map<int, std::vector<double> > regX0, regX1;
    for (size_t i = 0; i < lines.size(); i++)
        if (!lines[i].kind && !lines[i].drop && fabs(lines[i].size - body) < 0.6) {
            regX0[lines[i].region].push_back(lines[i].x0);
            regX1[lines[i].region].push_back(lines[i].x1);
        }
    std::map<int, double> regLeft, regRight;
    bool indentStyle = false; /* абзацы начинаются с красной строки — вычисляется ниже */
    for (std::map<int, std::vector<double> >::iterator it = regX0.begin(); it != regX0.end(); ++it) {
        if (it->second.size() < 3) continue;
        std::map<int, size_t> cnt;
        for (size_t k = 0; k < it->second.size(); k++) cnt[(int)floor(it->second[k] + 0.5)]++;
        int bl = 0;
        size_t bc = 0;
        for (std::map<int, size_t>::iterator c = cnt.begin(); c != cnt.end(); ++c)
            if (c->second > bc) { bc = c->second; bl = c->first; }
        std::vector<double>& r = regX1[it->first];
        std::sort(r.begin(), r.end());
        regLeft[it->first] = bl;
        regRight[it->first] = r[std::min(r.size() - 1, (size_t)(r.size() * 0.9))];
    }
    {
        size_t bodyLines = 0, indented = 0;
        for (size_t i = 0; i < lines.size(); i++) {
            const Line& l = lines[i];
            if (l.kind || l.drop || l.heading >= 0 || fabs(l.size - body) > 0.6) continue;
            double lf = regLeft.count(l.region) ? regLeft[l.region] : left;
            bodyLines++;
            if (l.x0 > lf + body * 0.8 && l.x0 < lf + body * 6) indented++;
        }
        indentStyle = indented >= 3 && indented * 50 >= bodyLines;
    }
    struct Out {
        static void flush(std::vector<Chapter>& ch, std::string& para, bool& list) {
            std::string h = trim(para);
            para.clear();
            if (h.empty()) return;
            Block b(list ? Block::LI : Block::P);
            b.html = h;
            ch.back().blocks.push_back(b);
            list = false;
        }
    };
    std::map<const PObj*, std::string> imgNames; /* картинка PDF -> имя в книге ("" — не годится) */
    for (size_t i = 0; i < lines.size(); i++) {
        Line& l = lines[i];
        if (l.drop) continue;
        if (l.kind == 1) {
            std::map<const PObj*, std::string>::iterator it = imgNames.find(l.img);
            if (it == imgNames.end()) {
                std::string mime, data;
                std::string name;
                if (pdfImageFile(doc, *l.img, mime, data)) name = addImage(book, mime, Bytes(data.begin(), data.end()));
                it = imgNames.insert(std::make_pair(l.img, name)).first;
            }
            if (!it->second.empty()) {
                Out::flush(chapters, para, paraList);
                Block b(Block::IMG);
                b.src = "images/" + it->second;
                chapters.back().blocks.push_back(b);
                prev = NULL;
            }
            continue;
        }
        if (l.kind == 2) {
            Out::flush(chapters, para, paraList);
            Block b(Block::RAW);
            b.html = l.html;
            chapters.back().blocks.push_back(b);
            prev = NULL;
            continue;
        }
        if (l.heading >= 0) {
            Out::flush(chapters, para, paraList);
            Chapter c;
            c.level = l.heading;
            c.title = escHtml(l.text);
            chapters.push_back(c);
            prev = NULL;
            continue;
        }
        std::map<std::string, std::string>& refs = pageRefs[l.page];
        std::string html = segsHtml(l.segs, &refs);
        bool bullet = isBulletText(l.text);
        if (bullet) {
            /* маркер списка — первый символ первого сегмента */
            std::vector<Seg> s = l.segs;
            if (!s.empty()) s[0].text = stripBullet(s[0].text);
            if (!s.empty() && s[0].text.empty()) s.erase(s.begin());
            html = segsHtml(s, &refs);
        }
        bool newPara = prev == NULL || bullet;
        if (prev && !newPara) {
            /* следующая колонка — как следующая страница: абзац может продолжиться */
            bool samePage = prev->region == l.region;
            double lLeft = regLeft.count(l.region) ? regLeft[l.region] : left;
            double pRight = regRight.count(prev->region) ? regRight[prev->region] : right;
            double gap = samePage ? prev->y - l.y : gapNorm;
            const char* why = NULL;
            if (samePage && (gap > gapNorm * 1.45 || gap < 0)) why = "gap";
            else if (l.x0 > lLeft + body * 0.8 && l.x0 < lLeft + body * 6) why = "indent";          /* красная строка */
            /* короткая строка с точкой — конец абзаца; но если в книге абзацы начинаются с красной
               строки, то на переходе в новую колонку/страницу решает только отступ: последнюю
               строку колонки Word иногда не растягивает и посреди абзаца */
            else if ((samePage || !indentStyle) && prev->x1 < pRight - body * 2.5 && endsSentence(prev->text)) why = "short";
            else if (startsWith(l.text, "\xE2\x80\x94") || startsWith(l.text, "\xE2\x80\x93 ")) why = "dialog"; /* диалог */
            if (why) newPara = true;
            if (why && layoutLog)
                fprintf(layoutLog, "new paragraph (%s) p%d r%d: gap=%.1f norm=%.1f x0=%.1f left=%.1f prev.x1=%.1f right=%.1f | %.40s\n",
                        why, l.page + 1, l.region, gap, gapNorm, l.x0, lLeft, prev->x1, pRight, l.text.c_str());
            /* на границе колонки или страницы абзац кончается только короткой строкой или красной
               строкой выше; точка в конце полной строки — это конец предложения, не абзаца */
            /* продолжение пункта списка стоит правее маркера; строка вровень с маркером — уже не пункт */
            else if (paraList && l.x0 < paraX0 + body * 0.8) newPara = true;
            if (fabs(l.size - prev->size) > 0.6) newPara = true;
        }
        if (newPara) {
            Out::flush(chapters, para, paraList);
            para = html;
            paraList = bullet;
            paraX0 = l.x0;
        } else {
            /* перенос «сло- / во»: дефис в конце строки и строчная буква дальше */
            if (!para.empty() && para[para.size() - 1] == '-' && startsLower(l.text) &&
                (para.size() < 2 || para[para.size() - 2] != ' '))
                para = para.substr(0, para.size() - 1) + html;
            else if (para.size() >= 2 && para.compare(para.size() - 2, 2, "\xC2\xAD") == 0)
                para = para.substr(0, para.size() - 2) + html;
            else
                para += " " + html;
        }
        prev = &l;
    }
    Out::flush(chapters, para, paraList);

    for (size_t i = 0; i < chapters.size(); i++) {
        bool has = !chapters[i].title.empty();
        for (size_t k = 0; k < chapters[i].blocks.size() && !has; k++) has = true;
        if (has) book.chapters.push_back(chapters[i]);
    }
    normalizeChapterLevels(book.chapters);
    if (book.chapters.empty()) {
        if (err) *err = "В PDF не нашлось текста";
        return false;
    }
    return true;
}

} /* namespace */

bool readPDF(const Bytes& data, const std::string& fileName, Book& book, std::string* err) {
    std::string s(data.begin(), data.end());
    PdfDoc doc(s);
    if (!doc.open()) {
        if (err) *err = "Повреждённый PDF: не найден каталог документа";
        return false;
    }
    if (doc.encrypted) {
        if (err) *err = "PDF зашифрован (защита от копирования) — сначала снимите защиту";
        return false;
    }
    book.fileName = fileName;
    PdfBook pb(doc, book);
    return pb.read(err);
}
