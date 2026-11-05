/* CCITT G3/G4 (T.4, T.6) и JBIG2 (ISO 14492) для картинок из PDF.
   1.9: страницы сканов чаще всего лежат именно в этих форматах, и раньше такие картинки пропускались.
   Чего нет в JBIG2: Huffman вместе с уточнением символов и пропуск ячеек полутона — в них
   декодер честно отказывается, и PDF читается дальше без этой картинки. */
#include "imgcodec.h"
#include <algorithm>
#include <map>
#include <string.h>

MqDecoder::MqDecoder(const unsigned char* data, size_t start, size_t end)
    : d_(data), bp_(start), end_(end), clow_(0), a_(0x8000), ct_(0) {
    chigh_ = bp_ < end_ ? d_[bp_] : 0xFF;
    byteIn();
    chigh_ = ((chigh_ << 7) & 0xFFFF) | ((clow_ >> 9) & 0x7F);
    clow_ = (clow_ << 7) & 0xFFFF;
    ct_ -= 7;
}

void MqDecoder::byteIn() {
    unsigned cur = bp_ < end_ ? d_[bp_] : 0xFF;
    unsigned nxt = bp_ + 1 < end_ ? d_[bp_ + 1] : 0xFF;
    if (cur == 0xFF) {
        if (nxt > 0x8F) {
            clow_ += 0xFF00;
            ct_ = 8;
        } else {
            bp_++;
            clow_ += nxt << 9;
            ct_ = 7;
        }
    } else {
        bp_++;
        clow_ += bp_ < end_ ? (unsigned)d_[bp_] << 8 : 0xFF00u;
        ct_ = 8;
    }
    if (clow_ > 0xFFFF) {
        chigh_ += clow_ >> 16;
        clow_ &= 0xFFFF;
    }
}

namespace {

/* ================================ CCITT ================================= */

struct RunCode { const char* bits; int run; };

const RunCode kWhite[] = {
    {"00110101",0},{"000111",1},{"0111",2},{"1000",3},{"1011",4},{"1100",5},{"1110",6},{"1111",7},{"10011",8},{"10100",9},
    {"00111",10},{"01000",11},{"001000",12},{"000011",13},{"110100",14},{"110101",15},{"101010",16},{"101011",17},
    {"0100111",18},{"0001100",19},{"0001000",20},{"0010111",21},{"0000011",22},{"0000100",23},{"0101000",24},{"0101011",25},
    {"0010011",26},{"0100100",27},{"0011000",28},{"00000010",29},{"00000011",30},{"00011010",31},{"00011011",32},
    {"00010010",33},{"00010011",34},{"00010100",35},{"00010101",36},{"00010110",37},{"00010111",38},{"00101000",39},
    {"00101001",40},{"00101010",41},{"00101011",42},{"00101100",43},{"00101101",44},{"00000100",45},{"00000101",46},
    {"00001010",47},{"00001011",48},{"01010010",49},{"01010011",50},{"01010100",51},{"01010101",52},{"00100100",53},
    {"00100101",54},{"01011000",55},{"01011001",56},{"01011010",57},{"01011011",58},{"01001010",59},{"01001011",60},
    {"00110010",61},{"00110011",62},{"00110100",63},
    {"11011",64},{"10010",128},{"010111",192},{"0110111",256},{"00110110",320},{"00110111",384},{"01100100",448},
    {"01100101",512},{"01101000",576},{"01100111",640},{"011001100",704},{"011001101",768},{"011010010",832},
    {"011010011",896},{"011010100",960},{"011010101",1024},{"011010110",1088},{"011010111",1152},{"011011000",1216},
    {"011011001",1280},{"011011010",1344},{"011011011",1408},{"010011000",1472},{"010011001",1536},{"010011010",1600},
    {"011000",1664},{"010011011",1728},{NULL,0}};

const RunCode kBlack[] = {
    {"0000110111",0},{"010",1},{"11",2},{"10",3},{"011",4},{"0011",5},{"0010",6},{"00011",7},{"000101",8},{"000100",9},
    {"0000100",10},{"0000101",11},{"0000111",12},{"00000100",13},{"00000111",14},{"000011000",15},{"0000010111",16},
    {"0000011000",17},{"0000001000",18},{"00001100111",19},{"00001101000",20},{"00001101100",21},{"00000110111",22},
    {"00000101000",23},{"00000010111",24},{"00000011000",25},{"000011001010",26},{"000011001011",27},{"000011001100",28},
    {"000011001101",29},{"000001101000",30},{"000001101001",31},{"000001101010",32},{"000001101011",33},{"000011010010",34},
    {"000011010011",35},{"000011010100",36},{"000011010101",37},{"000011010110",38},{"000011010111",39},{"000001101100",40},
    {"000001101101",41},{"000011011010",42},{"000011011011",43},{"000001010100",44},{"000001010101",45},{"000001010110",46},
    {"000001010111",47},{"000001100100",48},{"000001100101",49},{"000001010010",50},{"000001010011",51},{"000000100100",52},
    {"000000110111",53},{"000000111000",54},{"000000100111",55},{"000000101000",56},{"000001011000",57},{"000001011001",58},
    {"000000101011",59},{"000000101100",60},{"000001011010",61},{"000001100110",62},{"000001100111",63},
    {"0000001111",64},{"000011001000",128},{"000011001001",192},{"000001011011",256},{"000000110011",320},{"000000110100",384},
    {"000000110101",448},{"0000001101100",512},{"0000001101101",576},{"0000001001010",640},{"0000001001011",704},
    {"0000001001100",768},{"0000001001101",832},{"0000001110010",896},{"0000001110011",960},{"0000001110100",1024},
    {"0000001110101",1088},{"0000001110110",1152},{"0000001110111",1216},{"0000001010010",1280},{"0000001010011",1344},
    {"0000001010100",1408},{"0000001010101",1472},{"0000001011010",1536},{"0000001011011",1600},{"0000001100100",1664},
    {"0000001100101",1728},{NULL,0}};

const RunCode kExt[] = {
    {"00000001000",1792},{"00000001100",1856},{"00000001101",1920},{"000000010010",1984},{"000000010011",2048},
    {"000000010100",2112},{"000000010101",2176},{"000000010110",2240},{"000000010111",2304},{"000000011100",2368},
    {"000000011101",2432},{"000000011110",2496},{"000000011111",2560},{NULL,0}};

/* таблица на 13 бит: индекс — следующие 13 бит потока, значение — длина кода и серия */
struct RunTable {
    short len[8192], run[8192];
    void add(const RunCode* c) {
        for (; c->bits; c++) {
            int n = (int)strlen(c->bits), v = 0;
            for (int i = 0; i < n; i++) v = (v << 1) | (c->bits[i] - '0');
            int shift = 13 - n;
            for (int i = 0; i < (1 << shift); i++) {
                len[(v << shift) | i] = (short)n;
                run[(v << shift) | i] = (short)c->run;
            }
        }
    }
    RunTable(const RunCode* a) {
        memset(len, 0, sizeof(len));
        memset(run, 0, sizeof(run));
        add(a);
        add(kExt);
    }
};

struct FaxBits {
    const unsigned char* d;
    size_t n, bit;
    FaxBits(const unsigned char* data, size_t size) : d(data), n(size * 8), bit(0) {}
    unsigned peek(int count) const {
        unsigned v = 0;
        for (int i = 0; i < count; i++) {
            size_t b = bit + i;
            v = (v << 1) | (b < n ? (d[b >> 3] >> (7 - (b & 7))) & 1u : 0u);
        }
        return v;
    }
    unsigned peek13() const {
        size_t by = bit >> 3;
        unsigned v = 0;
        for (int i = 0; i < 3; i++) v = (v << 8) | (by + i < n / 8 ? d[by + i] : 0u);
        return (v >> (11 - (bit & 7))) & 0x1FFF;
    }
    bool eof() const { return bit >= n; }
    void align() { bit = (bit + 7) & ~(size_t)7; }
};

/* длина серии: коды дополнения + завершающий; -1 — ошибка */
int readRun(FaxBits& b, const RunTable& t) {
    int total = 0;
    for (;;) {
        unsigned v = b.peek13();
        int l = t.len[v];
        if (!l) return -1;
        b.bit += (size_t)l;
        int r = t.run[v];
        total += r;
        if (r < 64) return total;
        if (b.eof()) return -1;
    }
}

const RunTable& whiteTable() { static const RunTable* t = new RunTable(kWhite); return *t; }
const RunTable& blackTable() { static const RunTable* t = new RunTable(kBlack); return *t; }

/* Смены цвета текущей и опорной строк. Возвращает строки как байты-пиксели (1 — чёрный). */
struct FaxDecoder {
    FaxBits b;
    int cols;
    std::vector<int> ref, cur;
    FaxDecoder(const unsigned char* d, size_t n, int columns) : b(d, n), cols(columns) {
        ref.assign(1, columns);
    }
    bool line1d() {
        cur.clear();
        int a0 = 0;
        bool black = false;
        while (a0 < cols) {
            int r = readRun(b, black ? blackTable() : whiteTable());
            if (r < 0) return false;
            a0 += r;
            if (a0 > cols) a0 = cols;
            cur.push_back(a0);
            black = !black;
        }
        return true;
    }
    /* первая смена опорной строки правее a0 и нужного цвета (смена к чёрному — чётные индексы) */
    int findB1(size_t& i, int a0, bool black) const {
        while (i > 0 && ref[i - 1] > a0) i--;
        while (i < ref.size() && (ref[i] <= a0 || ((i & 1) != (black ? 1u : 0u)))) {
            if (a0 < 0 && ref[i] == 0 && (i & 1) == (black ? 1u : 0u)) break;
            i++;
        }
        return i < ref.size() ? ref[i] : cols;
    }
    bool line2d() {
        cur.clear();
        int a0 = -1;
        bool black = false;
        size_t ri = 0;
        while (a0 < cols) {
            int b1 = findB1(ri, a0, black);
            int b2 = ri + 1 < ref.size() ? ref[ri + 1] : cols;
            unsigned v = b.peek(7);
            if (v >> 6) { /* V0: 1 */
                b.bit += 1;
                a0 = b1;
                cur.push_back(a0);
                black = !black;
            } else if ((v >> 4) == 3) { /* VR1: 011 */
                b.bit += 3;
                a0 = std::min(b1 + 1, cols);
                cur.push_back(a0);
                black = !black;
            } else if ((v >> 4) == 2) { /* VL1: 010 */
                b.bit += 3;
                a0 = std::max(b1 - 1, 0);
                cur.push_back(a0);
                black = !black;
            } else if ((v >> 4) == 1) { /* горизонтальный: 001 */
                b.bit += 3;
                int r1 = readRun(b, black ? blackTable() : whiteTable());
                int r2 = readRun(b, black ? whiteTable() : blackTable());
                if (r1 < 0 || r2 < 0) return false;
                int s = a0 < 0 ? 0 : a0;
                cur.push_back(std::min(s + r1, cols));
                a0 = std::min(s + r1 + r2, cols);
                cur.push_back(a0);
            } else if ((v >> 3) == 1) { /* проход: 0001 */
                b.bit += 4;
                a0 = b2;
            } else if ((v >> 1) == 3) { /* VR2: 000011 */
                b.bit += 6;
                a0 = std::min(b1 + 2, cols);
                cur.push_back(a0);
                black = !black;
            } else if ((v >> 1) == 2) { /* VL2: 000010 */
                b.bit += 6;
                a0 = std::max(b1 - 2, 0);
                cur.push_back(a0);
                black = !black;
            } else if (v == 3) { /* VR3: 0000011 */
                b.bit += 7;
                a0 = std::min(b1 + 3, cols);
                cur.push_back(a0);
                black = !black;
            } else if (v == 2) { /* VL3: 0000010 */
                b.bit += 7;
                a0 = std::max(b1 - 3, 0);
                cur.push_back(a0);
                black = !black;
            } else {
                return false; /* EOFB или мусор */
            }
        }
        return true;
    }
    bool skipEol() {
        /* EOL: не меньше 11 нулей и единица (заполнители-нули допустимы) */
        size_t save = b.bit;
        int zeros = 0;
        while (!b.eof() && b.peek(1) == 0) { b.bit++; zeros++; }
        if (zeros >= 11 && !b.eof()) { b.bit++; return true; }
        b.bit = save;
        return false;
    }
    void commit() {
        if (cur.empty() || cur.back() < cols) cur.push_back(cols);
        ref.swap(cur);
    }
    /* пиксели текущей (уже зафиксированной в ref) строки в упакованном виде: бит 1 — чёрный */
    void paint(unsigned char* row) const {
        memset(row, 0, (size_t)(cols + 7) / 8);
        int x = 0;
        bool black = false;
        for (size_t i = 0; i < ref.size(); i++) {
            int e = std::min(ref[i], cols);
            if (black)
                for (int p = x; p < e; p++) row[p >> 3] |= (unsigned char)(0x80 >> (p & 7));
            if (e > x) x = e;
            black = !black;
        }
    }
};

/* Общая часть: декодирует height строк (или до конца), 1 — чёрный */
bool faxToPacked(const unsigned char* d, size_t n, int k, int cols, int rows, bool byteAlign,
                 std::string& out, size_t* consumed) {
    if (cols <= 0 || cols > 100000) return false;
    size_t rb = (size_t)(cols + 7) / 8;
    int limit = rows > 0 ? rows : 100000;
    if (rb * (size_t)limit > 400u * 1024 * 1024) return false;
    FaxDecoder f(d, n, cols);
    out.clear();
    out.reserve(rb * (size_t)(rows > 0 ? rows : 1024));
    std::vector<unsigned char> row(rb);
    for (int y = 0; y < limit && !f.b.eof(); y++) {
        if (byteAlign) f.b.align();
        bool ok;
        if (k < 0) {
            ok = f.line2d();
        } else {
            bool eol = f.skipEol();
            /* RTC: шесть EOL подряд — конец */
            if (eol && f.skipEol()) break;
            if (k > 0 && eol) {
                bool oneD = f.b.peek(1) != 0;
                f.b.bit++;
                ok = oneD ? f.line1d() : f.line2d();
            } else {
                ok = f.line1d();
            }
        }
        if (!ok) break;
        f.commit();
        f.paint(&row[0]);
        out.append((const char*)&row[0], rb);
    }
    if (consumed) {
        /* EOFB у MMR в JBIG2 */
        if (k < 0 && f.b.peek(24) == 0x001001) f.b.bit += 24;
        *consumed = std::min((f.b.bit + 7) / 8, n);
    }
    return !out.empty();
}

} /* namespace */

bool ccittDecode(const std::string& data, const CcittParams& p, std::string& out) {
    if (data.empty()) return false;
    if (!faxToPacked((const unsigned char*)data.data(), data.size(), p.k, p.columns, p.rows, p.byteAlign, out, NULL))
        return false;
    /* внутри 1 — чёрный; в PDF по умолчанию 0 — чёрный (BlackIs1 = false) */
    if (!p.blackIs1)
        for (size_t i = 0; i < out.size(); i++) out[i] = (char)~out[i];
    return true;
}

/* ================================ JBIG2 ================================= */

namespace {

const size_t kMaxPixels = 200u * 1024 * 1024; /* предел области: столько байт на 32-битной системе ещё есть */

struct Bitmap {
    int w, h;
    std::vector<unsigned char> px; /* байт на пиксель, 1 — чёрный */
    Bitmap() : w(0), h(0) {}
    bool init(int width, int height, unsigned char v = 0) {
        if (width < 0 || height < 0 || (height && (size_t)width > kMaxPixels / (size_t)height)) return false;
        w = width;
        h = height;
        px.assign((size_t)width * (size_t)height, v);
        return true;
    }
    int get(int x, int y) const {
        return (x < 0 || y < 0 || x >= w || y >= h) ? 0 : px[(size_t)y * (size_t)w + (size_t)x];
    }
};

/* op: 0 OR, 1 AND, 2 XOR, 3 XNOR, 4 REPLACE */
void combine(Bitmap& dst, const Bitmap& src, int x0, int y0, int op) {
    for (int y = 0; y < src.h; y++) {
        int dy = y0 + y;
        if (dy < 0 || dy >= dst.h) continue;
        const unsigned char* s = &src.px[(size_t)y * (size_t)src.w];
        unsigned char* d = &dst.px[(size_t)dy * (size_t)dst.w];
        for (int x = 0; x < src.w; x++) {
            int dx = x0 + x;
            if (dx < 0 || dx >= dst.w) continue;
            switch (op) {
                case 0: d[dx] |= s[x]; break;
                case 1: d[dx] &= s[x]; break;
                case 2: d[dx] ^= s[x]; break;
                case 3: d[dx] = (unsigned char)(1 ^ d[dx] ^ s[x]); break;
                default: d[dx] = s[x]; break;
            }
        }
    }
}

struct Reader {
    const unsigned char* d;
    size_t n, p;
    bool bad;
    Reader(const unsigned char* data, size_t size) : d(data), n(size), p(0), bad(false) {}
    unsigned u8() { if (p >= n) { bad = true; return 0; } return d[p++]; }
    unsigned u16() { unsigned a = u8(); return (a << 8) | u8(); }
    unsigned u32() { unsigned a = u16(); return (a << 16) | u16(); }
    int i8() { return (signed char)u8(); }
    int i32() { return (int)u32(); }
};

/* ------------------------------ Huffman --------------------------------- */

struct HuffLine { int low, pref, rlen; int kind; }; /* kind: 0 обычная, 1 lower, 2 upper, 3 OOB */

struct HuffTable {
    /* код: длина префикса + значение; поиск линейный по длине — таблицы маленькие */
    struct Entry { int low, rlen, kind, plen; unsigned code; };
    std::vector<Entry> e;
    void build(const std::vector<HuffLine>& lines) {
        /* B.3: коды назначаются по возрастанию длины, внутри длины — по порядку строк */
        int maxLen = 0;
        for (size_t i = 0; i < lines.size(); i++) maxLen = std::max(maxLen, lines[i].pref);
        std::vector<int> count((size_t)maxLen + 2, 0);
        for (size_t i = 0; i < lines.size(); i++) if (lines[i].pref > 0) count[(size_t)lines[i].pref]++;
        unsigned first = 0;
        e.clear();
        for (int len = 1; len <= maxLen; len++) {
            first = (first + (unsigned)count[(size_t)len - 1]) << 1;
            if (len == 1) first = 0;
            unsigned code = first;
            for (size_t i = 0; i < lines.size(); i++) {
                if (lines[i].pref != len) continue;
                Entry x = {lines[i].low, lines[i].rlen, lines[i].kind, len, code++};
                e.push_back(x);
            }
        }
    }
};

const int kOob = 0x7FFFFFFF;

struct HuffBits {
    const unsigned char* d;
    size_t n, bit;
    bool bad;
    HuffBits(const unsigned char* data, size_t size) : d(data), n(size * 8), bit(0), bad(false) {}
    unsigned read(int c) {
        unsigned v = 0;
        for (int i = 0; i < c; i++) {
            if (bit >= n) { bad = true; v <<= 1; continue; }
            v = (v << 1) | ((d[bit >> 3] >> (7 - (bit & 7))) & 1u);
            bit++;
        }
        return v;
    }
    void align() { bit = (bit + 7) & ~(size_t)7; }
    int decode(const HuffTable& t) {
        unsigned code = 0;
        int len = 0;
        size_t i = 0;
        while (i < t.e.size()) {
            const HuffTable::Entry& x = t.e[i];
            while (len < x.plen) { code = (code << 1) | read(1); len++; if (bad) return kOob; }
            if (x.plen == len && x.code == code) {
                if (x.kind == 3) return kOob;
                if (x.kind == 1) return x.low - (int)read(32);
                if (x.kind == 2) return x.low + (int)read(32);
                return x.low + (x.rlen ? (int)read(x.rlen) : 0);
            }
            i++;
        }
        bad = true;
        return kOob;
    }
};

/* Стандартные таблицы B.1–B.15: {low, prefix, range} + метки */
#define L(lo, p, r) {lo, p, r, 0}
#define LOW(lo, p) {lo, p, 32, 1}
#define UP(lo, p) {lo, p, 32, 2}
#define OOB(p) {0, p, 0, 3}
const HuffLine kB1[] = {L(0,1,4),L(16,2,8),L(272,3,16),UP(65808,3)};
const HuffLine kB2[] = {L(0,1,0),L(1,2,0),L(2,3,0),L(3,4,3),L(11,5,6),UP(75,6),OOB(6)};
const HuffLine kB3[] = {L(-256,8,8),L(0,1,0),L(1,2,0),L(2,3,0),L(3,4,3),L(11,5,6),LOW(-257,8),UP(75,7),OOB(6)};
const HuffLine kB4[] = {L(1,1,0),L(2,2,0),L(3,3,0),L(4,4,3),L(12,5,6),UP(76,5)};
const HuffLine kB5[] = {L(-255,7,8),L(1,1,0),L(2,2,0),L(3,3,0),L(4,4,3),L(12,5,6),LOW(-256,7),UP(76,6)};
const HuffLine kB6[] = {L(-2048,5,10),L(-1024,4,9),L(-512,4,8),L(-256,4,7),L(-128,5,6),L(-64,5,5),L(-32,4,5),L(0,2,7),
                        L(128,3,7),L(256,3,8),L(512,4,9),L(1024,4,10),LOW(-2049,6),UP(2048,6)};
const HuffLine kB7[] = {L(-1024,4,9),L(-512,3,8),L(-256,4,7),L(-128,5,6),L(-64,5,5),L(-32,4,5),L(0,4,5),L(32,5,5),L(64,5,6),
                        L(128,4,7),L(256,3,8),L(512,3,9),L(1024,3,10),LOW(-1025,5),UP(2048,5)};
const HuffLine kB8[] = {L(-15,8,3),L(-7,9,1),L(-5,8,1),L(-3,9,0),L(-2,7,0),L(-1,4,0),L(0,2,1),L(2,5,0),L(3,6,0),L(4,3,4),
                        L(20,6,1),L(22,4,4),L(38,4,5),L(70,5,6),L(134,5,7),L(262,6,7),L(390,7,8),L(646,6,10),
                        LOW(-16,9),UP(1670,9),OOB(2)};
const HuffLine kB9[] = {L(-31,8,4),L(-15,9,2),L(-11,8,2),L(-7,9,1),L(-5,7,1),L(-3,4,1),L(-1,3,1),L(1,3,1),L(3,5,1),L(5,6,1),
                        L(7,3,5),L(39,6,2),L(43,4,5),L(75,4,6),L(139,5,7),L(267,5,8),L(523,6,8),L(779,7,9),L(1291,6,11),
                        LOW(-32,9),UP(3339,9),OOB(2)};
const HuffLine kB10[] = {L(-21,7,4),L(-5,8,0),L(-4,7,0),L(-3,5,0),L(-2,2,2),L(2,5,0),L(3,6,0),L(4,7,0),L(5,8,0),L(6,2,6),
                         L(70,5,5),L(102,6,5),L(134,6,6),L(198,6,7),L(326,6,8),L(582,6,9),L(1094,6,10),L(2118,7,11),
                         LOW(-22,8),UP(4166,8),OOB(2)};
const HuffLine kB11[] = {L(1,1,0),L(2,2,1),L(4,4,0),L(5,4,1),L(7,5,1),L(9,5,2),L(13,6,2),L(17,7,2),L(21,7,3),L(29,7,4),
                         L(45,7,5),L(77,7,6),UP(141,7)};
const HuffLine kB12[] = {L(1,1,0),L(2,2,0),L(3,3,1),L(5,5,0),L(6,5,1),L(8,6,1),L(10,7,0),L(11,7,1),L(13,7,2),L(17,7,3),
                         L(25,7,4),L(41,8,5),UP(73,8)};
const HuffLine kB13[] = {L(1,1,0),L(2,3,0),L(3,4,0),L(4,5,0),L(5,4,1),L(7,3,3),L(15,6,1),L(17,6,2),L(21,6,3),L(29,6,4),
                         L(45,6,5),L(77,7,6),UP(141,7)};
const HuffLine kB14[] = {L(-2,3,0),L(-1,3,0),L(0,1,0),L(1,3,0),L(2,3,0)};
const HuffLine kB15[] = {L(-24,7,4),L(-8,6,2),L(-4,5,1),L(-2,4,0),L(-1,3,0),L(0,1,0),L(1,3,0),L(2,4,0),L(3,5,1),L(5,6,2),
                         L(9,7,4),LOW(-25,7),UP(25,7)};
#undef L
#undef LOW
#undef UP
#undef OOB

const HuffTable& stdTable(int n) {
    static std::map<int, HuffTable>* cache = new std::map<int, HuffTable>();
    std::map<int, HuffTable>::iterator it = cache->find(n);
    if (it != cache->end()) return it->second;
    std::vector<HuffLine> v;
    #define T(k, a) case k: v.assign(a, a + sizeof(a) / sizeof(a[0])); break
    switch (n) {
        T(1,kB1); T(2,kB2); T(3,kB3); T(4,kB4); T(5,kB5); T(6,kB6); T(7,kB7); T(8,kB8);
        T(9,kB9); T(10,kB10); T(11,kB11); T(12,kB12); T(13,kB13); T(14,kB14); default: T(15,kB15);
    }
    #undef T
    HuffTable& t = (*cache)[n];
    t.build(v);
    return t;
}

/* --------------------------- арифметические целые ----------------------- */

struct IntCtx { unsigned char cx[512]; IntCtx() { memset(cx, 0, sizeof(cx)); } };

/* Annex A.2; kOob — «вне диапазона» */
int decodeInt(MqDecoder& mq, IntCtx& c) {
    unsigned prev = 1;
    struct R {
        static unsigned bits(MqDecoder& m, IntCtx& ic, unsigned& pv, int n) {
            unsigned v = 0;
            for (int i = 0; i < n; i++) {
                unsigned b = (unsigned)m.bit(ic.cx[pv]);
                pv = pv < 256 ? (pv << 1) | b : (((pv << 1) | b) & 511) | 256;
                v = (v << 1) | b;
            }
            return v;
        }
    };
    unsigned s = R::bits(mq, c, prev, 1);
    unsigned v;
    if (!R::bits(mq, c, prev, 1)) v = R::bits(mq, c, prev, 2);
    else if (!R::bits(mq, c, prev, 1)) v = R::bits(mq, c, prev, 4) + 4;
    else if (!R::bits(mq, c, prev, 1)) v = R::bits(mq, c, prev, 6) + 20;
    else if (!R::bits(mq, c, prev, 1)) v = R::bits(mq, c, prev, 8) + 84;
    else if (!R::bits(mq, c, prev, 1)) v = R::bits(mq, c, prev, 12) + 340;
    else v = R::bits(mq, c, prev, 32) + 4436;
    if (!s) return v > 0x7FFFFFF0u ? kOob : (int)v;
    if (v > 0) return v > 0x7FFFFFF0u ? kOob : -(int)v;
    return kOob;
}

unsigned decodeIaid(MqDecoder& mq, std::vector<unsigned char>& cx, int len) {
    unsigned prev = 1;
    for (int i = 0; i < len; i++) {
        unsigned b = (unsigned)mq.bit(cx[prev]);
        prev = (prev << 1) | b;
    }
    return len >= 32 ? prev : prev & ((1u << len) - 1);
}

/* ---------------------------- общие контексты --------------------------- */

struct Contexts {
    std::vector<unsigned char> gb, gr, iaid;
    IntCtx iadt, iafs, iads, iait, iari, iardw, iardh, iardx, iardy, iadh, iadw, iaex, iaai;
    Contexts() : gb(1 << 16, 0), gr(1 << 13, 0) {}
};

/* ---------------------------- универсальная область --------------------- */

struct At { int x, y; };

/* Шаблоны — порядок битов контекста из 6.2.5.3 (важен: от него зависит контекст SLTP) */
bool decodeGeneric(MqDecoder& mq, std::vector<unsigned char>& cx, int w, int h, int tmpl, bool tpgd,
                   const At* at, Bitmap& out) {
    if (!out.init(w, h)) return false;
    if (!w || !h) return true;
    struct Off { int x, y; };
    Off o[16];
    int n = 0;
    #define P(a, b) o[n].x = a; o[n].y = b; n++
    switch (tmpl) {
        case 0:
            P(-1,0);P(-2,0);P(-3,0);P(-4,0);P(at[0].x,at[0].y);P(2,-1);P(1,-1);P(0,-1);P(-1,-1);P(-2,-1);
            P(at[1].x,at[1].y);P(at[2].x,at[2].y);P(1,-2);P(0,-2);P(-1,-2);P(at[3].x,at[3].y);
            break;
        case 1:
            P(-1,0);P(-2,0);P(-3,0);P(at[0].x,at[0].y);P(2,-1);P(1,-1);P(0,-1);P(-1,-1);P(-2,-1);
            P(2,-2);P(1,-2);P(0,-2);P(-1,-2);
            break;
        case 2:
            P(-1,0);P(-2,0);P(at[0].x,at[0].y);P(1,-1);P(0,-1);P(-1,-1);P(-2,-1);P(1,-2);P(0,-2);P(-1,-2);
            break;
        default:
            P(-1,0);P(-2,0);P(-3,0);P(-4,0);P(at[0].x,at[0].y);P(1,-1);P(0,-1);P(-1,-1);P(-2,-1);P(-3,-1);
            break;
    }
    #undef P
    static const unsigned kSltp[4] = {0x9B25, 0x0795, 0x00E5, 0x0195};
    /* поле с полями из нулей: можно читать соседей без проверок границ */
    int minX = 0, maxX = 0, minY = 0;
    for (int i = 0; i < n; i++) {
        if (o[i].y > 0 || (o[i].y == 0 && o[i].x >= 0)) return false; /* будущие пиксели недопустимы */
        minX = std::min(minX, o[i].x);
        maxX = std::max(maxX, o[i].x);
        minY = std::min(minY, o[i].y);
    }
    int padL = -minX, padR = maxX, padT = -minY;
    size_t stride = (size_t)w + (size_t)padL + (size_t)padR;
    if ((size_t)h + (size_t)padT > kMaxPixels / stride) return false;
    std::vector<unsigned char> buf(stride * ((size_t)h + (size_t)padT), 0);
    ptrdiff_t off[16];
    for (int i = 0; i < n; i++) off[i] = (ptrdiff_t)o[i].y * (ptrdiff_t)stride + o[i].x;
    bool ltp = false;
    for (int y = 0; y < h; y++) {
        unsigned char* row = &buf[((size_t)y + (size_t)padT) * stride + (size_t)padL];
        if (tpgd) {
            ltp ^= mq.bit(cx[kSltp[tmpl]]) != 0;
            if (ltp) {
                if (y > 0) memcpy(row, row - stride, (size_t)w);
                continue;
            }
        }
        for (int x = 0; x < w; x++) {
            unsigned c = 0;
            const unsigned char* p = row + x;
            for (int i = 0; i < n; i++) c |= (unsigned)p[off[i]] << i;
            row[x] = (unsigned char)mq.bit(cx[c]);
        }
    }
    for (int y = 0; y < h; y++)
        memcpy(&out.px[(size_t)y * (size_t)w], &buf[((size_t)y + (size_t)padT) * stride + (size_t)padL], (size_t)w);
    return true;
}

/* область MMR: возвращает сколько байт занято */
bool decodeMmr(const unsigned char* d, size_t n, int w, int h, Bitmap& out, size_t* used) {
    if (!out.init(w, h)) return false;
    if (!w || !h) { if (used) *used = 0; return true; }
    std::string packed;
    if (!faxToPacked(d, n, -1, w, h, false, packed, used)) return false;
    size_t rb = (size_t)(w + 7) / 8;
    size_t rows = packed.size() / rb;
    for (size_t y = 0; y < rows && y < (size_t)h; y++)
        for (int x = 0; x < w; x++)
            out.px[y * (size_t)w + (size_t)x] = ((unsigned char)packed[y * rb + (size_t)(x >> 3)] >> (7 - (x & 7))) & 1;
    return true;
}

/* ------------------------------- уточнение ------------------------------ */

bool decodeRefine(MqDecoder& mq, std::vector<unsigned char>& cx, int w, int h, int tmpl, const Bitmap& ref,
                  int dx, int dy, bool tpgr, const At* at, Bitmap& out) {
    if (!out.init(w, h)) return false;
    bool ltp = false;
    for (int y = 0; y < h; y++) {
        if (tpgr) ltp ^= mq.bit(cx[tmpl ? 0x0080 : 0x0100]) != 0;
        for (int x = 0; x < w; x++) {
            int rx = x - dx, ry = y - dy;
            if (ltp) {
                /* TPGRPIX: все 9 пикселей опоры одинаковы — значение известно */
                int v = ref.get(rx, ry);
                bool same = true;
                for (int j = -1; j <= 1 && same; j++)
                    for (int i = -1; i <= 1; i++)
                        if (ref.get(rx + i, ry + j) != v) { same = false; break; }
                if (same) { out.px[(size_t)y * (size_t)w + (size_t)x] = (unsigned char)v; continue; }
            }
            unsigned c;
            if (tmpl == 0) {
                c = (unsigned)out.get(x - 1, y) | ((unsigned)out.get(x + 1, y - 1) << 1) | ((unsigned)out.get(x, y - 1) << 2) |
                    ((unsigned)out.get(x + at[0].x, y + at[0].y) << 3) | ((unsigned)ref.get(rx + 1, ry + 1) << 4) |
                    ((unsigned)ref.get(rx, ry + 1) << 5) | ((unsigned)ref.get(rx - 1, ry + 1) << 6) |
                    ((unsigned)ref.get(rx + 1, ry) << 7) | ((unsigned)ref.get(rx, ry) << 8) |
                    ((unsigned)ref.get(rx - 1, ry) << 9) | ((unsigned)ref.get(rx + 1, ry - 1) << 10) |
                    ((unsigned)ref.get(rx, ry - 1) << 11) | ((unsigned)ref.get(rx + at[1].x, ry + at[1].y) << 12);
            } else {
                c = (unsigned)out.get(x - 1, y) | ((unsigned)out.get(x + 1, y - 1) << 1) | ((unsigned)out.get(x, y - 1) << 2) |
                    ((unsigned)out.get(x - 1, y - 1) << 3) | ((unsigned)ref.get(rx + 1, ry + 1) << 4) |
                    ((unsigned)ref.get(rx, ry + 1) << 5) | ((unsigned)ref.get(rx + 1, ry) << 6) |
                    ((unsigned)ref.get(rx, ry) << 7) | ((unsigned)ref.get(rx - 1, ry) << 8) |
                    ((unsigned)ref.get(rx, ry - 1) << 9);
            }
            out.px[(size_t)y * (size_t)w + (size_t)x] = (unsigned char)mq.bit(cx[c]);
        }
    }
    return true;
}

/* -------------------------------- текст --------------------------------- */

struct TextParams {
    bool huff, refine, transposed;
    int logStrips, corner, op, defPixel, dsOffset, rtemplate;
    At rat[2];
    unsigned instances;
    const HuffTable* fs; const HuffTable* ds; const HuffTable* dt;
    const HuffTable* rdw; const HuffTable* rdh; const HuffTable* rdx; const HuffTable* rdy; const HuffTable* rsize;
    const HuffTable* symId;
    int codeLen;
};

/* corner: 0 снизу-слева, 1 сверху-слева, 2 снизу-справа, 3 сверху-справа */
bool decodeText(const TextParams& tp, const std::vector<const Bitmap*>& syms, int w, int h, MqDecoder* mq,
                HuffBits* hb, Contexts& cx, Bitmap& out) {
    if (!out.init(w, h, (unsigned char)tp.defPixel)) return false;
    int strips = 1 << tp.logStrips;
    int first = tp.huff ? hb->decode(*tp.dt) : decodeInt(*mq, cx.iadt);
    if (first == kOob) return false;
    long long stripT = -(long long)first * strips, firstS = 0;
    unsigned n = 0;
    while (n < tp.instances) {
        int dt = tp.huff ? hb->decode(*tp.dt) : decodeInt(*mq, cx.iadt);
        if (dt == kOob) return false;
        stripT += (long long)dt * strips;
        int dfs = tp.huff ? hb->decode(*tp.fs) : decodeInt(*mq, cx.iafs);
        if (dfs == kOob) return false;
        firstS += dfs;
        long long curS = firstS;
        for (bool start = true;; start = false) {
            if (!start) {
                int ds = tp.huff ? hb->decode(*tp.ds) : decodeInt(*mq, cx.iads);
                if (ds == kOob) break;
                curS += (long long)ds + tp.dsOffset;
            }
            if (n >= tp.instances) break;
            int curT = 0;
            if (strips > 1) {
                curT = tp.huff ? (int)hb->read(tp.logStrips) : decodeInt(*mq, cx.iait);
                if (curT == kOob) return false;
            }
            long long T = stripT + curT;
            unsigned id = tp.huff ? (unsigned)hb->decode(*tp.symId) : decodeIaid(*mq, cx.iaid, tp.codeLen);
            if (tp.huff && hb->bad) return false;
            if (id >= syms.size() || !syms[id]) return false;
            const Bitmap* sb = syms[id];
            Bitmap refined;
            int ri = 0;
            if (tp.refine) ri = tp.huff ? (int)hb->read(1) : decodeInt(*mq, cx.iari);
            if (ri) {
                if (tp.huff) return false; /* Huffman + уточнение символа на практике не встречается */
                int rdw = decodeInt(*mq, cx.iardw), rdh = decodeInt(*mq, cx.iardh);
                int rdx = decodeInt(*mq, cx.iardx), rdy = decodeInt(*mq, cx.iardy);
                if (rdw == kOob || rdh == kOob || rdx == kOob || rdy == kOob) return false;
                if (rdw < -65536 || rdw > 65536 || rdh < -65536 || rdh > 65536) return false;
                int nw = sb->w + rdw, nh = sb->h + rdh;
                if (nw < 0 || nh < 0) return false;
                if (!decodeRefine(*mq, cx.gr, nw, nh, tp.rtemplate, *sb, (rdw >> 1) + rdx, (rdh >> 1) + rdy, false, tp.rat, refined))
                    return false;
                sb = &refined;
            }
            int W = sb->w, H = sb->h;
            if (!tp.transposed && tp.corner >= 2) curS += W - 1;
            else if (tp.transposed && (tp.corner & 1) == 0) curS += H - 1;
            long long x, y;
            if (!tp.transposed) {
                x = tp.corner >= 2 ? curS - W + 1 : curS;
                y = (tp.corner & 1) ? T : T - H + 1;
            } else {
                x = tp.corner >= 2 ? T - W + 1 : T;
                y = (tp.corner & 1) ? curS : curS - H + 1;
            }
            if (x > -100000 && x < 2000000 && y > -100000 && y < 2000000) combine(out, *sb, (int)x, (int)y, tp.op);
            if (!tp.transposed && tp.corner < 2) curS += W - 1;
            else if (tp.transposed && (tp.corner & 1)) curS += H - 1;
            n++;
        }
    }
    return true;
}

/* ---------------------------- декодер потока ---------------------------- */

struct Segment {
    unsigned num;
    int type;
    std::vector<unsigned> refs;
    const unsigned char* data;
    size_t len;
};

struct RegionInfo { int w, h, x, y, op; };

class Jbig2 {
public:
    Jbig2(int width, int height) : hasPage(false), hintW(width), hintH(height), budget(1200u * 1024 * 1024) {}
    bool run(const std::string& s);
    Bitmap page;
    bool hasPage;

private:
    int hintW, hintH;
    size_t budget; /* сумма размеров символов: защита от испорченных файлов на 32-битной системе */
    std::map<unsigned, std::vector<Bitmap> > dicts;    /* символы и шаблоны */
    std::map<unsigned, HuffTable> tables;
    std::map<unsigned, Bitmap> intermediate;
    std::map<unsigned, std::vector<unsigned char> > retainedGb, retainedGr;

    bool segment(const Segment& s);
    bool readRegion(Reader& r, RegionInfo& ri);
    void place(const Segment& s, const RegionInfo& ri, Bitmap& b);
    bool symbolDict(const Segment& s);
    bool textRegion(const Segment& s);
    bool patternDict(const Segment& s);
    bool halftone(const Segment& s);
    bool generic(const Segment& s);
    bool refinement(const Segment& s);
    bool pageInfo(const Segment& s);
    bool customTable(const Segment& s);
    const HuffTable* userTable(const Segment& s, size_t& idx);
    void ensurePage();
};

bool Jbig2::readRegion(Reader& r, RegionInfo& ri) {
    unsigned w = r.u32(), h = r.u32(), x = r.u32(), y = r.u32();
    unsigned f = r.u8();
    if (r.bad || w > 200000 || h > 200000) return false;
    ri.w = (int)w;
    ri.h = (int)h;
    ri.x = (int)x;
    ri.y = (int)y;
    ri.op = (int)(f & 7);
    if (ri.op > 4) ri.op = 0;
    return true;
}

void Jbig2::ensurePage() {
    if (hasPage) return;
    page.init(hintW, hintH);
    hasPage = true;
}

void Jbig2::place(const Segment& s, const RegionInfo& ri, Bitmap& b) {
    /* промежуточные области (типы 4, 20, 36, 40) на страницу не идут — их уточняют позже */
    if (s.type == 4 || s.type == 20 || s.type == 36 || s.type == 40) {
        intermediate[s.num].w = b.w;
        intermediate[s.num].h = b.h;
        intermediate[s.num].px.swap(b.px);
        return;
    }
    ensurePage();
    combine(page, b, ri.x, ri.y, ri.op);
}

bool Jbig2::pageInfo(const Segment& s) {
    Reader r(s.data, s.len);
    unsigned w = r.u32(), h = r.u32();
    r.u32(); r.u32();
    unsigned f = r.u8();
    if (r.bad) return false;
    if (hasPage) return true; /* в потоке PDF ровно одна страница */
    if (w == 0 || w > 200000) w = (unsigned)hintW;
    if (h == 0xFFFFFFFFu || h == 0 || h > 200000) h = (unsigned)hintH; /* высота неизвестна: как в словаре PDF */
    if (!page.init((int)w, (int)h, (unsigned char)((f >> 2) & 1))) return false;
    hasPage = true;
    return true;
}

bool Jbig2::generic(const Segment& s) {
    Reader r(s.data, s.len);
    RegionInfo ri;
    if (!readRegion(r, ri)) return false;
    unsigned f = r.u8();
    bool mmr = f & 1;
    int tmpl = (int)(f >> 1) & 3;
    bool tpgd = (f >> 3) & 1;
    At at[4];
    memset(at, 0, sizeof(at));
    if (!mmr) {
        int cnt = tmpl == 0 ? 4 : 1;
        for (int i = 0; i < cnt; i++) { at[i].x = r.i8(); at[i].y = r.i8(); }
    }
    if (r.bad) return false;
    Bitmap b;
    if (mmr) {
        if (!decodeMmr(s.data + r.p, s.len - r.p, ri.w, ri.h, b, NULL)) return false;
    } else {
        MqDecoder mq(s.data, r.p, s.len);
        std::vector<unsigned char> gb(1 << 16, 0);
        if (!decodeGeneric(mq, gb, ri.w, ri.h, tmpl, tpgd, at, b)) return false;
    }
    place(s, ri, b);
    return true;
}

bool Jbig2::refinement(const Segment& s) {
    Reader r(s.data, s.len);
    RegionInfo ri;
    if (!readRegion(r, ri)) return false;
    unsigned f = r.u8();
    int tmpl = (int)(f & 1);
    bool tpgr = (f >> 1) & 1;
    At at[2];
    memset(at, 0, sizeof(at));
    if (!tmpl) for (int i = 0; i < 2; i++) { at[i].x = r.i8(); at[i].y = r.i8(); }
    if (r.bad) return false;
    /* опора: промежуточная область из ссылок, иначе кусок страницы */
    Bitmap ref;
    bool found = false;
    for (size_t i = 0; i < s.refs.size(); i++) {
        std::map<unsigned, Bitmap>::iterator it = intermediate.find(s.refs[i]);
        if (it != intermediate.end()) { ref = it->second; found = true; }
    }
    if (!found) {
        ensurePage();
        if (!ref.init(ri.w, ri.h)) return false;
        for (int y = 0; y < ri.h; y++)
            for (int x = 0; x < ri.w; x++)
                ref.px[(size_t)y * (size_t)ri.w + (size_t)x] = (unsigned char)page.get(ri.x + x, ri.y + y);
    }
    MqDecoder mq(s.data, r.p, s.len);
    std::vector<unsigned char> gr(1 << 13, 0);
    Bitmap b;
    if (!decodeRefine(mq, gr, ri.w, ri.h, tmpl, ref, 0, 0, tpgr, at, b)) return false;
    if (!found) ri.op = 4; /* уточняется сама страница: результат заменяет исходное */
    place(s, ri, b);
    return true;
}

const HuffTable* Jbig2::userTable(const Segment& s, size_t& idx) {
    /* 7.4: пользовательские таблицы берутся из ссылок по порядку */
    size_t seen = 0;
    for (size_t i = 0; i < s.refs.size(); i++) {
        std::map<unsigned, HuffTable>::iterator it = tables.find(s.refs[i]);
        if (it == tables.end()) continue;
        if (seen == idx) { idx++; return &it->second; }
        seen++;
    }
    return NULL;
}

bool Jbig2::customTable(const Segment& s) {
    Reader r(s.data, s.len);
    unsigned f = r.u8();
    int ps = (int)((f >> 1) & 7) + 1, rs = (int)((f >> 4) & 7) + 1;
    int low = r.i32(), high = r.i32();
    if (r.bad || high < low) return false;
    HuffBits b(s.data + r.p, s.len - r.p);
    std::vector<HuffLine> lines;
    long long cur = low;
    while (cur < high && lines.size() < 100000) {
        HuffLine l;
        l.low = (int)cur;
        l.pref = (int)b.read(ps);
        l.rlen = (int)b.read(rs);
        l.kind = 0;
        if (b.bad || l.rlen > 31) return false;
        lines.push_back(l);
        cur += 1LL << l.rlen;
    }
    HuffLine lo = {low - 1, (int)b.read(ps), 32, 1};
    HuffLine up = {high, (int)b.read(ps), 32, 2};
    lines.push_back(lo);
    lines.push_back(up);
    if (f & 1) { HuffLine ob = {0, (int)b.read(ps), 0, 3}; lines.push_back(ob); }
    if (b.bad) return false;
    tables[s.num].build(lines);
    return true;
}

static int ceilLog2(unsigned v) {
    int n = 0;
    while ((1ull << n) < (unsigned long long)v) n++;
    return n;
}

bool Jbig2::symbolDict(const Segment& s) {
    Reader r(s.data, s.len);
    unsigned f = r.u16();
    bool huff = f & 1, refagg = (f >> 1) & 1;
    int selDH = (int)(f >> 2) & 3, selDW = (int)(f >> 4) & 3, selBM = (int)(f >> 6) & 1, selAgg = (int)(f >> 7) & 1;
    bool ctxUsed = (f >> 8) & 1, ctxRetained = (f >> 9) & 1;
    int tmpl = (int)(f >> 10) & 3, rtmpl = (int)(f >> 12) & 1;
    At at[4], rat[2];
    memset(at, 0, sizeof(at));
    memset(rat, 0, sizeof(rat));
    if (!huff) {
        int cnt = tmpl == 0 ? 4 : 1;
        for (int i = 0; i < cnt; i++) { at[i].x = r.i8(); at[i].y = r.i8(); }
    }
    if (refagg && !rtmpl) for (int i = 0; i < 2; i++) { rat[i].x = r.i8(); rat[i].y = r.i8(); }
    unsigned numEx = r.u32(), numNew = r.u32();
    if (r.bad || numNew > 2000000 || numEx > 4000000) return false;
    if (huff && refagg) return false; /* на практике не встречается */

    std::vector<const Bitmap*> in;
    for (size_t i = 0; i < s.refs.size(); i++) {
        std::map<unsigned, std::vector<Bitmap> >::iterator it = dicts.find(s.refs[i]);
        if (it == dicts.end()) continue;
        for (size_t k = 0; k < it->second.size(); k++) in.push_back(&it->second[k]);
    }
    if (in.size() + numNew > 8000000) return false;

    size_t ut = 0;
    const HuffTable* tDH = NULL; const HuffTable* tDW = NULL; const HuffTable* tBM = NULL; const HuffTable* tAgg = NULL;
    if (huff) {
        tDH = selDH == 0 ? &stdTable(4) : selDH == 1 ? &stdTable(5) : userTable(s, ut);
        tDW = selDW == 0 ? &stdTable(2) : selDW == 1 ? &stdTable(3) : userTable(s, ut);
        tBM = selBM == 0 ? &stdTable(1) : userTable(s, ut);
        tAgg = selAgg == 0 ? &stdTable(1) : userTable(s, ut);
        if (!tDH || !tDW || !tBM || !tAgg) return false;
    }

    Contexts cx;
    if (ctxUsed) {
        /* контексты последнего словаря, который их сохранил */
        for (size_t i = s.refs.size(); i-- > 0;) {
            std::map<unsigned, std::vector<unsigned char> >::iterator g = retainedGb.find(s.refs[i]);
            if (g != retainedGb.end()) {
                cx.gb = g->second;
                cx.gr = retainedGr[s.refs[i]];
                break;
            }
        }
    }
    int codeLen = huff ? std::max(1, ceilLog2((unsigned)in.size() + numNew)) : ceilLog2((unsigned)in.size() + numNew);
    if (codeLen > 24) return false;
    cx.iaid.assign((size_t)1 << (codeLen + 1), 0);

    std::vector<Bitmap> news;
    news.reserve(numNew);
    std::vector<int> widths;
    MqDecoder* mq = NULL;
    MqDecoder mqStore(s.data, r.p, s.len);
    if (!huff) mq = &mqStore;
    HuffBits hb(s.data + r.p, s.len - r.p);
    long long height = 0;
    while (news.size() < numNew) {
        int dh = huff ? hb.decode(*tDH) : decodeInt(*mq, cx.iadh);
        if (dh == kOob) return false;
        height += dh;
        if (height < 0 || height > 65536) return false;
        long long width = 0, total = 0;
        size_t firstInClass = huff ? widths.size() : 0;
        for (;;) {
            int dw = huff ? hb.decode(*tDW) : decodeInt(*mq, cx.iadw);
            if (dw == kOob) break;
            if (huff && hb.bad) return false;
            if (news.size() + (huff ? widths.size() - firstInClass : 0) >= numNew) return false;
            width += dw;
            if (width < 0 || width > 65536) return false;
            total += width;
            if (huff) {
                widths.push_back((int)width);
                continue;
            }
            size_t cost = (size_t)width * (size_t)height;
            if (cost > budget) return false;
            budget -= cost;
            news.push_back(Bitmap());
            Bitmap& b = news.back();
            if (!refagg) {
                if (!decodeGeneric(*mq, cx.gb, (int)width, (int)height, tmpl, false, at, b)) return false;
            } else {
                int ninst = decodeInt(*mq, cx.iaai);
                if (ninst == kOob || ninst < 1) return false;
                std::vector<const Bitmap*> all(in);
                for (size_t k = 0; k + 1 < news.size(); k++) all.push_back(&news[k]);
                if (ninst > 1) {
                    TextParams tp;
                    memset(&tp, 0, sizeof(tp));
                    tp.refine = true;
                    tp.corner = 1;
                    tp.rtemplate = rtmpl;
                    tp.rat[0] = rat[0];
                    tp.rat[1] = rat[1];
                    tp.instances = (unsigned)ninst;
                    tp.codeLen = codeLen;
                    all.resize(in.size() + numNew, NULL);
                    if (!decodeText(tp, all, (int)width, (int)height, mq, NULL, cx, b)) return false;
                } else {
                    unsigned id = decodeIaid(*mq, cx.iaid, codeLen);
                    int rdx = decodeInt(*mq, cx.iardx), rdy = decodeInt(*mq, cx.iardy);
                    if (rdx == kOob || rdy == kOob || id >= all.size()) return false;
                    if (!decodeRefine(*mq, cx.gr, (int)width, (int)height, rtmpl, *all[id], rdx, rdy, false, rat, b)) return false;
                }
            }
        }
        if (huff) {
            /* символы класса высоты лежат одной картинкой, ширины уже известны */
            int bmSize = hb.decode(*tBM);
            if (bmSize == kOob || bmSize < 0) return false;
            hb.align();
            size_t at0 = hb.bit / 8;
            if (at0 > hb.n / 8) return false;
            Bitmap coll;
            if (total > 200000) return false;
            size_t avail = hb.n / 8 - at0;
            if (bmSize == 0) {
                size_t rb = (size_t)(total + 7) / 8, need = rb * (size_t)height;
                if (need > avail || !coll.init((int)total, (int)height)) return false;
                const unsigned char* p = hb.d + at0;
                for (long long y = 0; y < height; y++)
                    for (long long x = 0; x < total; x++)
                        coll.px[(size_t)y * (size_t)total + (size_t)x] = (p[(size_t)y * rb + (size_t)(x >> 3)] >> (7 - (x & 7))) & 1;
                hb.bit += need * 8;
            } else {
                if ((size_t)bmSize > avail) return false;
                if (!decodeMmr(hb.d + at0, (size_t)bmSize, (int)total, (int)height, coll, NULL)) return false;
                hb.bit += (size_t)bmSize * 8;
            }
            long long x0 = 0;
            for (size_t k = firstInClass; k < widths.size(); k++) {
                news.push_back(Bitmap());
                Bitmap& b = news.back();
                if (!b.init(widths[k], (int)height)) return false;
                for (long long y = 0; y < height; y++)
                    for (int x = 0; x < widths[k]; x++)
                        b.px[(size_t)y * (size_t)widths[k] + (size_t)x] = (unsigned char)coll.get((int)x0 + x, (int)y);
                x0 += widths[k];
            }
        }
        if (news.empty() && numNew && height > 65535) return false;
    }

    /* какие символы экспортируются: чередующиеся серии «нет/да» */
    std::vector<const Bitmap*> all(in);
    for (size_t k = 0; k < news.size(); k++) all.push_back(&news[k]);
    std::vector<Bitmap>& ex = dicts[s.num];
    size_t i = 0;
    bool flag = false;
    while (i < all.size()) {
        int run = huff ? hb.decode(stdTable(1)) : decodeInt(*mq, cx.iaex);
        if (run == kOob || run < 0 || (size_t)run > all.size() - i) return false;
        if (flag) for (int k = 0; k < run; k++) ex.push_back(*all[i + (size_t)k]);
        i += (size_t)run;
        flag = !flag;
        if (run == 0 && i == 0 && flag == false) return false;
    }
    if (ctxRetained) {
        retainedGb[s.num] = cx.gb;
        retainedGr[s.num] = cx.gr;
    }
    return true;
}

bool Jbig2::textRegion(const Segment& s) {
    Reader r(s.data, s.len);
    RegionInfo ri;
    if (!readRegion(r, ri)) return false;
    unsigned f = r.u16();
    TextParams tp;
    memset(&tp, 0, sizeof(tp));
    tp.huff = f & 1;
    tp.refine = (f >> 1) & 1;
    tp.logStrips = (int)(f >> 2) & 3;
    tp.corner = (int)(f >> 4) & 3;
    tp.transposed = (f >> 6) & 1;
    tp.op = (int)(f >> 7) & 3;
    tp.defPixel = (int)(f >> 9) & 1;
    tp.dsOffset = (int)(f >> 10) & 31;
    if (tp.dsOffset > 15) tp.dsOffset -= 32;
    tp.rtemplate = (int)(f >> 15) & 1;
    unsigned hf = 0;
    if (tp.huff) hf = r.u16();
    if (tp.refine && !tp.rtemplate)
        for (int i = 0; i < 2; i++) { tp.rat[i].x = r.i8(); tp.rat[i].y = r.i8(); }
    tp.instances = r.u32();
    if (r.bad) return false;

    std::vector<const Bitmap*> syms;
    for (size_t i = 0; i < s.refs.size(); i++) {
        std::map<unsigned, std::vector<Bitmap> >::iterator it = dicts.find(s.refs[i]);
        if (it == dicts.end()) continue;
        for (size_t k = 0; k < it->second.size(); k++) syms.push_back(&it->second[k]);
    }
    if (syms.empty() && tp.instances) return false;
    tp.codeLen = ceilLog2((unsigned)syms.size());
    if (tp.codeLen > 24) return false;

    Contexts cx;
    cx.iaid.assign((size_t)1 << (tp.codeLen + 1), 0);
    Bitmap b;
    HuffTable symTable;
    if (tp.huff) {
        size_t ut = 0;
        int v;
        #define SEL(field, shift, mask, a, b2, c) \
            v = (int)(hf >> shift) & mask; \
            tp.field = v == 0 ? &stdTable(a) : (v == 1 && b2 ? &stdTable(b2) : (v == 2 && c ? &stdTable(c) : userTable(s, ut)));
        SEL(fs, 0, 3, 6, 7, 0)
        SEL(ds, 2, 3, 8, 9, 10)
        SEL(dt, 4, 3, 11, 12, 13)
        SEL(rdw, 6, 3, 14, 15, 0)
        SEL(rdh, 8, 3, 14, 15, 0)
        SEL(rdx, 10, 3, 14, 15, 0)
        SEL(rdy, 12, 3, 14, 15, 0)
        SEL(rsize, 14, 1, 1, 0, 0)
        #undef SEL
        if (!tp.fs || !tp.ds || !tp.dt || !tp.rdw || !tp.rdh || !tp.rdx || !tp.rdy || !tp.rsize) return false;
        HuffBits hb(s.data + r.p, s.len - r.p);
        /* 7.4.3.1.7: таблица кодов символов записана длинами с кодированием серий */
        std::vector<HuffLine> runLines(35);
        for (int i = 0; i < 35; i++) {
            runLines[(size_t)i].low = i;
            runLines[(size_t)i].pref = (int)hb.read(4);
            runLines[(size_t)i].rlen = 0;
            runLines[(size_t)i].kind = 0;
        }
        HuffTable runTable;
        runTable.build(runLines);
        std::vector<HuffLine> symLines;
        int prev = 0;
        while (symLines.size() < syms.size()) {
            int code = hb.decode(runTable);
            if (hb.bad || code == kOob) return false;
            int rep = 1, val = code;
            if (code == 32) { rep = (int)hb.read(2) + 3; val = prev; }
            else if (code == 33) { rep = (int)hb.read(3) + 3; val = 0; }
            else if (code == 34) { rep = (int)hb.read(7) + 11; val = 0; }
            if (code < 32) prev = code;
            for (int i = 0; i < rep && symLines.size() < syms.size(); i++) {
                HuffLine l = {(int)symLines.size(), val, 0, 0};
                symLines.push_back(l);
            }
        }
        hb.align();
        symTable.build(symLines);
        tp.symId = &symTable;
        if (!decodeText(tp, syms, ri.w, ri.h, NULL, &hb, cx, b)) return false;
    } else {
        MqDecoder mq(s.data, r.p, s.len);
        if (!decodeText(tp, syms, ri.w, ri.h, &mq, NULL, cx, b)) return false;
    }
    place(s, ri, b);
    return true;
}

bool Jbig2::patternDict(const Segment& s) {
    Reader r(s.data, s.len);
    unsigned f = r.u8();
    bool mmr = f & 1;
    int tmpl = (int)(f >> 1) & 3;
    int pw = (int)r.u8(), ph = (int)r.u8();
    unsigned grayMax = r.u32();
    if (r.bad || !pw || !ph || grayMax > 65535) return false;
    int cols = (int)(grayMax + 1) * pw;
    At at[4];
    at[0].x = -pw; at[0].y = 0; at[1].x = -3; at[1].y = -1; at[2].x = 2; at[2].y = -2; at[3].x = -2; at[3].y = -2;
    Bitmap coll;
    if (mmr) {
        if (!decodeMmr(s.data + r.p, s.len - r.p, cols, ph, coll, NULL)) return false;
    } else {
        MqDecoder mq(s.data, r.p, s.len);
        std::vector<unsigned char> gb(1 << 16, 0);
        if (!decodeGeneric(mq, gb, cols, ph, tmpl, false, at, coll)) return false;
    }
    std::vector<Bitmap>& d = dicts[s.num];
    d.resize(grayMax + 1);
    for (unsigned g = 0; g <= grayMax; g++) {
        d[g].init(pw, ph);
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++)
                d[g].px[(size_t)y * (size_t)pw + (size_t)x] = (unsigned char)coll.get((int)g * pw + x, y);
    }
    return true;
}

bool Jbig2::halftone(const Segment& s) {
    Reader r(s.data, s.len);
    RegionInfo ri;
    if (!readRegion(r, ri)) return false;
    unsigned f = r.u8();
    bool mmr = f & 1, skip = (f >> 3) & 1;
    int tmpl = (int)(f >> 1) & 3, op = (int)(f >> 4) & 7, def = (int)(f >> 7) & 1;
    unsigned gw = r.u32(), gh = r.u32();
    int gx = r.i32(), gy = r.i32();
    int rx = (int)r.u16(), ry = (int)r.u16();
    if (r.bad || skip || gw > 100000 || gh > 100000 || (gh && gw > kMaxPixels / 8 / gh)) return false;
    const std::vector<Bitmap>* pats = NULL;
    for (size_t i = 0; i < s.refs.size(); i++) {
        std::map<unsigned, std::vector<Bitmap> >::iterator it = dicts.find(s.refs[i]);
        if (it != dicts.end()) pats = &it->second;
    }
    if (!pats || pats->empty()) return false;
    int bpp = ceilLog2((unsigned)pats->size());
    std::vector<unsigned> gray((size_t)gw * gh, 0);
    At at[4];
    at[0].x = tmpl <= 1 ? 3 : 2; at[0].y = -1; at[1].x = -3; at[1].y = -1; at[2].x = 2; at[2].y = -2; at[3].x = -2; at[3].y = -2;
    MqDecoder mq(s.data, r.p, s.len);
    std::vector<unsigned char> gb(1 << 16, 0);
    size_t mmrPos = r.p;
    Bitmap prev;
    for (int j = bpp - 1; j >= 0; j--) {
        Bitmap pl;
        if (mmr) {
            size_t used = 0;
            if (mmrPos > s.len || !decodeMmr(s.data + mmrPos, s.len - mmrPos, (int)gw, (int)gh, pl, &used)) return false;
            mmrPos += used;
        } else if (!decodeGeneric(mq, gb, (int)gw, (int)gh, tmpl, false, at, pl)) {
            return false;
        }
        /* код Грея: каждая плоскость XOR с предыдущей */
        if (j < bpp - 1) for (size_t k = 0; k < pl.px.size(); k++) pl.px[k] ^= prev.px[k];
        for (size_t k = 0; k < pl.px.size(); k++) gray[k] |= (unsigned)pl.px[k] << j;
        prev.px.swap(pl.px);
        prev.w = pl.w;
        prev.h = pl.h;
    }
    Bitmap b;
    if (!b.init(ri.w, ri.h, (unsigned char)def)) return false;
    for (unsigned m = 0; m < gh; m++)
        for (unsigned n = 0; n < gw; n++) {
            long long x = ((long long)gx + (long long)m * ry + (long long)n * rx) >> 8;
            long long y = ((long long)gy + (long long)m * rx - (long long)n * ry) >> 8;
            unsigned g = gray[(size_t)m * gw + n];
            if (g >= pats->size()) g = (unsigned)pats->size() - 1;
            if (x > -70000 && x < 300000 && y > -70000 && y < 300000) combine(b, (*pats)[g], (int)x, (int)y, op > 4 ? 0 : op);
        }
    place(s, ri, b);
    return true;
}

bool Jbig2::segment(const Segment& s) {
    switch (s.type) {
        case 0: return symbolDict(s);
        case 4: case 6: case 7: return textRegion(s);
        case 16: return patternDict(s);
        case 20: case 22: case 23: return halftone(s);
        case 36: case 38: case 39: return generic(s);
        case 40: case 42: case 43: return refinement(s);
        case 48: return pageInfo(s);
        case 53: return customTable(s);
        default: return true; /* конец страницы, полосы, файла, профили, расширения */
    }
}

bool Jbig2::run(const std::string& str) {
    const unsigned char* d = (const unsigned char*)str.data();
    size_t n = str.size(), p = 0;
    static const char kMagic[] = "\x97JB2\r\n\x1A\n";
    if (n >= 9 && memcmp(d, kMagic, 8) == 0) {
        unsigned fl = d[8];
        p = 9 + ((fl & 2) ? 0 : 4);
    }
    int guard = 0;
    while (p + 11 <= n && guard++ < 2000000) {
        Reader r(d + p, n - p);
        Segment s;
        s.num = r.u32();
        unsigned fl = r.u8();
        s.type = (int)(fl & 63);
        unsigned b = r.u8();
        unsigned cnt = b >> 5;
        if (cnt == 7) {
            r.p--;
            cnt = r.u32() & 0x1FFFFFFF;
            if (cnt > 100000) return false;
            r.p += (cnt + 8) / 8;
        }
        int sz = s.num <= 256 ? 1 : (s.num <= 65536 ? 2 : 4);
        for (unsigned i = 0; i < cnt && !r.bad; i++) s.refs.push_back(sz == 1 ? r.u8() : (sz == 2 ? r.u16() : r.u32()));
        if (fl & 64) r.u32(); else r.u8();
        unsigned len = r.u32();
        if (r.bad) return false;
        size_t start = p + r.p;
        if (start > n) return false;
        if (len == 0xFFFFFFFFu) {
            /* длина неизвестна (только у универсальной области): конец — маркер 00 00 или FF AC и счётчик */
            if (s.type != 38 || n - start < 18) return false;
            bool mmr = d[start + 17] & 1;
            size_t q = start + 18;
            for (;; q++) {
                if (q + 6 > n) return false;
                if (mmr ? (d[q] == 0 && d[q + 1] == 0) : (d[q] == 0xFF && d[q + 1] == 0xAC)) break;
            }
            len = (unsigned)(q + 2 + 4 - start);
        }
        if (len > n - start) {
            /* обрезанный файл: декодируем что есть */
            len = (unsigned)(n - start);
        }
        s.data = d + start;
        s.len = len;
        if (s.type == 51) break;
        if (!segment(s)) return false;
        p = start + len;
    }
    return hasPage;
}

} /* namespace */

bool jbig2Decode(const std::string& data, const std::string& globals, int width, int height, std::string& out) {
    if (width <= 0 || height <= 0 || (size_t)width > kMaxPixels / (size_t)height) return false;
    Jbig2 j(width, height);
    if (!globals.empty() && !j.run(globals) && j.hasPage) return false;
    if (!j.run(data)) return false;
    /* как в PDF: 0 — чёрный */
    size_t rb = (size_t)(width + 7) / 8;
    out.assign(rb * (size_t)height, (char)0xFF);
    for (int y = 0; y < height && y < j.page.h; y++) {
        const unsigned char* s = &j.page.px[(size_t)y * (size_t)j.page.w];
        char* row = &out[(size_t)y * rb];
        for (int x = 0; x < width && x < j.page.w; x++)
            if (s[x]) row[x >> 3] &= (char)~(0x80 >> (x & 7));
    }
    return true;
}
