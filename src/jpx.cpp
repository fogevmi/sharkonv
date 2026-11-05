/* JPEG 2000 Part 1 (ISO 15444-1) для картинок из PDF (1.9): раньше такие картинки пропускались.
   Контейнер JP2 или голый кодовый поток; все пять порядков пакетов, вейвлеты 5-3 и 9-7, RCT/ICT,
   палитра JP2, подвыборка компонентов. Не поддержаны режимы кодового блока «обход
   арифметического кодера» и «завершение каждого прохода» и PPM/PPT: в них декодер отказывается,
   и PDF читается дальше без этой картинки. */
#include "imgcodec.h"
#include <algorithm>
#include <map>
#include <math.h>
#include <string.h>

namespace {

const size_t kMaxSamples = 150u * 1024 * 1024; /* float на отсчёт: столько помещается в 32-битный процесс */

inline int ceilDiv(int a, int b) { return (int)(((long long)a + b - 1) / b); }
inline int ceilPow2(int a, int n) { return (int)(((long long)a + (1LL << n) - 1) >> n); }

struct CompSiz { int prec; bool sgn; int xr, yr; };

struct Cod {
    int style, prog, layers, mct, levels, xcb, ycb, cbStyle, wavelet;
    bool sop, eph;
    std::vector<int> ppx, ppy;
    Cod() : style(0), prog(0), layers(1), mct(0), levels(0), xcb(6), ycb(6), cbStyle(0), wavelet(0), sop(false), eph(false) {}
};

struct Qcd {
    int style, guard;
    std::vector<int> eps, mu;
    Qcd() : style(0), guard(0) {}
};

/* дерево тегов (B.10.2) */
struct TagTree {
    struct Node { int value, low, parent; };
    std::vector<Node> n;
    int w, h;
    TagTree() : w(0), h(0) {}
    void init(int width, int height) {
        w = width;
        h = height;
        n.clear();
        std::vector<int> ws, hs;
        int a = width, b = height;
        for (;;) {
            ws.push_back(a);
            hs.push_back(b);
            if (a <= 1 && b <= 1) break;
            a = (a + 1) / 2;
            b = (b + 1) / 2;
        }
        size_t total = 0;
        for (size_t i = 0; i < ws.size(); i++) total += (size_t)ws[i] * hs[i];
        n.resize(total);
        size_t base = 0;
        for (size_t l = 0; l < ws.size(); l++) {
            size_t next = base + (size_t)ws[l] * hs[l];
            for (int y = 0; y < hs[l]; y++)
                for (int x = 0; x < ws[l]; x++) {
                    Node& nd = n[base + (size_t)y * ws[l] + x];
                    nd.value = 0x7FFFFFFF;
                    nd.low = 0;
                    nd.parent = l + 1 < ws.size() ? (int)(next + (size_t)(y / 2) * ws[l + 1] + x / 2) : -1;
                }
            base = next;
        }
    }
};

struct BitIn {
    const unsigned char* d;
    size_t pos, end;
    unsigned buf;
    int cnt;
    bool skip, bad;
    BitIn(const unsigned char* data, size_t p, size_t e) : d(data), pos(p), end(e), buf(0), cnt(0), skip(false), bad(false) {}
    int bit() {
        if (cnt == 0) {
            if (pos >= end) { bad = true; return 0; }
            unsigned b = d[pos++];
            if (skip) { cnt = 7; buf = b & 0x7F; }
            else { cnt = 8; buf = b; }
            skip = b == 0xFF;
        }
        cnt--;
        return (int)((buf >> cnt) & 1);
    }
    unsigned bits(int n) {
        unsigned v = 0;
        for (int i = 0; i < n; i++) v = (v << 1) | (unsigned)bit();
        return v;
    }
    void align() {
        cnt = 0;
        if (skip) { pos++; skip = false; }
    }
};

/* решение дерева: значение узла меньше порога? (B.10.2) */
bool tagDecode(TagTree& t, BitIn& b, int x, int y, int threshold) {
    int stack[32] = {0}, sp = 0;
    int node = y * t.w + x;
    while (node >= 0 && sp < 32) {
        stack[sp++] = node;
        node = t.n[(size_t)node].parent;
    }
    int low = 0;
    int cur = stack[0];
    while (sp > 0) {
        cur = stack[--sp];
        TagTree::Node& nd = t.n[(size_t)cur];
        if (low > nd.low) nd.low = low;
        else low = nd.low;
        while (low < threshold && low < nd.value) {
            if (b.bit()) nd.value = low;
            else low++;
        }
        nd.low = low;
    }
    return t.n[(size_t)cur].value < threshold;
}

struct Seg { size_t pos, len; };

struct CodeBlock {
    int x0, y0, x1, y1;   /* в координатах полосы */
    int gx, gy;           /* номер в сетке области */
    bool included;
    int zeroPlanes, lblock, passes;
    std::vector<Seg> segs;
    CodeBlock() : x0(0), y0(0), x1(0), y1(0), gx(0), gy(0), included(false), zeroPlanes(0), lblock(3), passes(0) {}
};

struct PrecinctBand {
    int gw, gh;
    std::vector<CodeBlock> blocks;
    TagTree incl, imsb;
    PrecinctBand() : gw(0), gh(0) {}
};

struct Band {
    int type;    /* 0 LL, 1 HL, 2 LH, 3 HH */
    int x0, y0, x1, y1;
    int mb;
    float delta;
    std::vector<PrecinctBand> precincts;
    Band() : type(0), x0(0), y0(0), x1(0), y1(0), mb(0), delta(1) {}
};

struct Resolution {
    int x0, y0, x1, y1, ppx, ppy, npw, nph;
    std::vector<Band> bands;
    Resolution() : x0(0), y0(0), x1(0), y1(0), ppx(15), ppy(15), npw(0), nph(0) {}
};

struct TileComp {
    int x0, y0, x1, y1;
    Cod cod;
    Qcd qcd;
    std::vector<Resolution> res;
    std::vector<float> samples;
    TileComp() : x0(0), y0(0), x1(0), y1(0) {}
};

struct TilePart { size_t pos, end; };

struct Tile {
    int x0, y0, x1, y1;
    bool hasCod, hasQcd;
    Cod cod;
    Qcd qcd;
    std::map<int, Cod> coc;
    std::map<int, Qcd> qcc;
    std::vector<TilePart> parts;
    Tile() : x0(0), y0(0), x1(0), y1(0), hasCod(false), hasQcd(false) {}
};

/* ------------------------------ tier-1 ----------------------------------- */

struct Tier1 {
    int w, h, bandType;
    bool causal;
    std::vector<unsigned char> sig;  /* с рамкой в 1 пиксель */
    std::vector<signed char> sgn;
    std::vector<unsigned char> visited, refined;
    std::vector<unsigned> mag;
    unsigned char cx[19];
    int stride;
    MqDecoder* mq;

    void resetCx() {
        memset(cx, 0, sizeof(cx));
        cx[0] = 4 << 1;
        cx[17] = 3 << 1;  /* серии */
        cx[18] = 46 << 1; /* равновероятный */
    }
    Tier1(int width, int height, int type, bool cau) : w(width), h(height), bandType(type), causal(cau), mq(NULL) {
        stride = w + 2;
        size_t n = (size_t)stride * (h + 2);
        sig.assign(n, 0);
        sgn.assign(n, 0);
        visited.assign((size_t)w * h, 0);
        refined.assign((size_t)w * h, 0);
        mag.assign((size_t)w * h, 0);
        resetCx();
    }
    inline size_t at(int x, int y) const { return (size_t)(y + 1) * stride + (x + 1); }

    /* при вертикально-причинной форме нижняя полоса ещё не считается известной */
    inline void counts(int x, int y, int& hc, int& vc, int& dc) const {
        const unsigned char* p = &sig[at(x, y)];
        bool lowOk = !(causal && (y & 3) == 3);
        hc = p[-1] + p[1];
        vc = p[-stride] + (lowOk ? p[stride] : 0);
        dc = p[-stride - 1] + p[-stride + 1] + (lowOk ? p[stride - 1] + p[stride + 1] : 0);
    }
    inline int zeroCtx(int hc, int vc, int dc) const {
        if (bandType == 3) {
            int hv = hc + vc;
            if (dc >= 3) return 8;
            if (dc == 2) return hv >= 1 ? 7 : 6;
            if (dc == 1) return hv >= 2 ? 5 : (hv == 1 ? 4 : 3);
            return hv >= 2 ? 2 : hv;
        }
        if (bandType == 1) std::swap(hc, vc);
        if (hc == 2) return 8;
        if (hc == 1) return vc >= 1 ? 7 : (dc >= 1 ? 6 : 5);
        if (vc == 2) return 4;
        if (vc == 1) return 3;
        return dc >= 2 ? 2 : dc;
    }
    inline void decodeSign(int x, int y, int bp) {
        size_t a = at(x, y);
        bool lowOk = !(causal && (y & 3) == 3);
        int hs = sgn[a - 1] + sgn[a + 1];
        int vs = sgn[a - stride] + (lowOk ? sgn[a + stride] : 0);
        hs = hs > 1 ? 1 : (hs < -1 ? -1 : hs);
        vs = vs > 1 ? 1 : (vs < -1 ? -1 : vs);
        int ctx, xr = 0;
        if (hs == 1) ctx = vs == 1 ? 13 : (vs == 0 ? 12 : 11);
        else if (hs == 0) { ctx = vs == 0 ? 9 : 10; xr = vs == -1; }
        else { ctx = vs == 1 ? 11 : (vs == 0 ? 12 : 13); xr = 1; }
        int b = mq->bit(cx[ctx]) ^ xr;
        sig[a] = 1;
        sgn[a] = b ? -1 : 1;
        mag[(size_t)y * w + x] = 1u << bp;
    }
    void sigPass(int bp) {
        for (int y0 = 0; y0 < h; y0 += 4)
            for (int x = 0; x < w; x++)
                for (int y = y0; y < y0 + 4 && y < h; y++) {
                    if (sig[at(x, y)]) continue;
                    int hc, vc, dc;
                    counts(x, y, hc, vc, dc);
                    if (hc + vc + dc == 0) continue;
                    visited[(size_t)y * w + x] = 1;
                    if (mq->bit(cx[zeroCtx(hc, vc, dc)])) decodeSign(x, y, bp);
                }
    }
    void refPass(int bp) {
        for (int y0 = 0; y0 < h; y0 += 4)
            for (int x = 0; x < w; x++)
                for (int y = y0; y < y0 + 4 && y < h; y++) {
                    size_t i = (size_t)y * w + x;
                    if (!sig[at(x, y)] || visited[i]) continue;
                    int ctx = 16;
                    if (!refined[i]) {
                        int hc, vc, dc;
                        counts(x, y, hc, vc, dc);
                        ctx = hc + vc + dc ? 15 : 14;
                        refined[i] = 1;
                    }
                    if (mq->bit(cx[ctx])) mag[i] |= 1u << bp;
                }
    }
    void cleanPass(int bp) {
        for (int y0 = 0; y0 < h; y0 += 4)
            for (int x = 0; x < w; x++) {
                int y = y0;
                bool partial = false;
                if (y0 + 3 < h) {
                    /* серия: все четыре незначимы, не посещены и без значимых соседей */
                    bool all = true;
                    for (int k = 0; k < 4 && all; k++) {
                        size_t i = (size_t)(y0 + k) * w + x;
                        int hc, vc, dc;
                        counts(x, y0 + k, hc, vc, dc);
                        if (sig[at(x, y0 + k)] || visited[i] || hc + vc + dc) all = false;
                    }
                    if (all) {
                        if (!mq->bit(cx[17])) continue;
                        int run = mq->bit(cx[18]) << 1;
                        run |= mq->bit(cx[18]);
                        y = y0 + run;
                        decodeSign(x, y, bp);
                        y++;
                        partial = true;
                    }
                }
                (void)partial;
                for (; y < y0 + 4 && y < h; y++) {
                    size_t i = (size_t)y * w + x;
                    if (sig[at(x, y)] || visited[i]) continue;
                    int hc, vc, dc;
                    counts(x, y, hc, vc, dc);
                    if (mq->bit(cx[zeroCtx(hc, vc, dc)])) decodeSign(x, y, bp);
                }
            }
        std::fill(visited.begin(), visited.end(), 0);
    }
};

/* ------------------------------ вейвлеты --------------------------------- */

/* 1D-синтез строки длины n с глобальным началом i0; buf с запасом 8 с каждой стороны */
void synth1d(float* line, int n, int i0, bool rev, std::vector<float>& tmp) {
    if (n == 1) {
        if (i0 & 1) line[0] /= 2;
        return;
    }
    const int ext = 8 + (i0 & 1); /* чётность индекса буфера совпадает с глобальной */
    tmp.resize((size_t)n + 2 * ext + 2);
    float* b = &tmp[0];
    memcpy(b + ext, line, (size_t)n * sizeof(float));
    /* симметричное продолжение */
    int period = 2 * (n - 1);
    for (int k = 1; k <= ext; k++) {
        int m = k % period;
        b[ext - k] = line[m < n ? m : period - m];
        m = (n - 1 + k) % period;
        b[ext + n - 1 + k] = line[m < n ? m : period - m];
    }
    int lo = ext, hi = ext + n; /* [lo, hi) */
    if (rev) {
        for (int j = lo - 2; j < hi + 2; j++) {
            if (j & 1) continue;
            b[j] -= floorf((b[j - 1] + b[j + 1] + 2) * 0.25f);
        }
        for (int j = lo - 1; j < hi + 1; j++) {
            if (!(j & 1)) continue;
            b[j] += floorf((b[j - 1] + b[j + 1]) * 0.5f);
        }
    } else {
        const float A = -1.586134342059924f, B = -0.052980118572961f, G = 0.882911075530934f, D = 0.443506852043971f,
                    K = 1.230174104914001f, IK = 1.0f / K;
        int s = 0, e = (int)n + 2 * ext;
        for (int j = s; j < e; j++) b[j] *= (j & 1) ? IK : K;
        for (int j = 2; j < e - 1; j += 1) if (!(j & 1)) b[j] -= D * (b[j - 1] + b[j + 1]);
        for (int j = 3; j < e - 2; j += 1) if (j & 1) b[j] -= G * (b[j - 1] + b[j + 1]);
        for (int j = 4; j < e - 3; j += 1) if (!(j & 1)) b[j] -= B * (b[j - 1] + b[j + 1]);
        for (int j = 5; j < e - 4; j += 1) if (j & 1) b[j] -= A * (b[j - 1] + b[j + 1]);
    }
    memcpy(line, b + ext, (size_t)n * sizeof(float));
}

/* ------------------------------ декодер ---------------------------------- */

class Decoder {
public:
    Decoder(const unsigned char* data, size_t n) : d(data), size(n), xsiz(0), ysiz(0), xosiz(0), yosiz(0),
        xtsiz(0), ytsiz(0), xtosiz(0), ytosiz(0), ntx(0), nty(0), hasCod(false), hasQcd(false) {}
    bool run();
    int imgW() const { return xsiz - xosiz; }
    int imgH() const { return ysiz - yosiz; }
    std::vector<CompSiz> comps;
    std::vector<std::vector<unsigned char> > planes;  /* по 8 бит, размер компонента */
    std::vector<int> planeW, planeH;

private:
    const unsigned char* d;
    size_t size;
    int xsiz, ysiz, xosiz, yosiz, xtsiz, ytsiz, xtosiz, ytosiz, ntx, nty;
    Cod cod;
    Qcd qcd;
    bool hasCod, hasQcd;
    std::map<int, Cod> coc;
    std::map<int, Qcd> qcc;
    std::map<int, Tile> tiles;

    unsigned u16(size_t p) const { return p + 2 <= size ? ((unsigned)d[p] << 8) | d[p + 1] : 0; }
    unsigned u32(size_t p) const { return p + 4 <= size ? ((unsigned)d[p] << 24) | (d[p + 1] << 16) | (d[p + 2] << 8) | d[p + 3] : 0; }

    bool parseSiz(size_t p, size_t e);
    bool parseCod(size_t p, size_t e, Cod& c);
    bool parseCoc(size_t p, size_t e, const Cod& base, std::map<int, Cod>& out);
    bool parseQcdBody(size_t p, size_t e, Qcd& q);
    bool parseQcc(size_t p, size_t e, std::map<int, Qcd>& out);
    bool decodeTile(int idx, Tile& t);
    bool buildComp(Tile& t, int c, TileComp& tc);
    bool packets(Tile& t, std::vector<TileComp>& tcs);
    bool onePacket(BitIn& in, std::vector<TileComp>& tcs, int comp, int r, int k, int layer, const Cod& tcod, size_t& pos, size_t& partEnd);
    bool tier1(TileComp& tc, int comp, std::vector<std::vector<float> >& levels);
    bool idwt(TileComp& tc, std::vector<std::vector<float> >& levels);
};

bool Decoder::parseSiz(size_t p, size_t e) {
    if (e - p < 38) return false;
    xsiz = (int)u32(p + 4); ysiz = (int)u32(p + 8); xosiz = (int)u32(p + 12); yosiz = (int)u32(p + 16);
    xtsiz = (int)u32(p + 20); ytsiz = (int)u32(p + 24); xtosiz = (int)u32(p + 28); ytosiz = (int)u32(p + 32);
    int c = (int)u16(p + 36);
    if (xsiz <= 0 || ysiz <= 0 || xosiz < 0 || yosiz < 0 || xosiz >= xsiz || yosiz >= ysiz || xtsiz <= 0 || ytsiz <= 0 ||
        xtosiz < 0 || ytosiz < 0 || c < 1 || c > 16 || e - p < 38 + (size_t)c * 3) return false;
    comps.resize((size_t)c);
    size_t total = 0;
    for (int i = 0; i < c; i++) {
        unsigned s = d[p + 38 + i * 3];
        comps[i].sgn = (s & 0x80) != 0;
        comps[i].prec = (int)(s & 0x7F) + 1;
        comps[i].xr = d[p + 39 + i * 3];
        comps[i].yr = d[p + 40 + i * 3];
        if (!comps[i].xr || !comps[i].yr || comps[i].prec > 24) return false;
        size_t pw = (size_t)ceilDiv(xsiz, comps[i].xr) - ceilDiv(xosiz, comps[i].xr);
        size_t ph = (size_t)ceilDiv(ysiz, comps[i].yr) - ceilDiv(yosiz, comps[i].yr);
        if (ph && pw > kMaxSamples / ph) return false;
        total += pw * ph;
        if (total > kMaxSamples) return false;
    }
    ntx = ceilDiv(xsiz - xtosiz, xtsiz);
    nty = ceilDiv(ysiz - ytosiz, ytsiz);
    if (ntx <= 0 || nty <= 0 || (long long)ntx * nty > 65535) return false;
    return true;
}

bool Decoder::parseCod(size_t p, size_t e, Cod& c) {
    if (e - p < 12) return false;
    unsigned s = d[p + 2];
    c.sop = (s & 2) != 0;
    c.eph = (s & 4) != 0;
    c.prog = d[p + 3];
    c.layers = (int)u16(p + 4);
    c.mct = d[p + 6];
    c.levels = d[p + 7];
    c.xcb = d[p + 8] + 2;
    c.ycb = d[p + 9] + 2;
    c.cbStyle = d[p + 10];
    c.wavelet = d[p + 11];
    if (c.levels > 32 || c.xcb > 10 || c.ycb > 10 || c.xcb + c.ycb > 12 || c.layers < 1) return false;
    c.ppx.assign((size_t)c.levels + 1, 15);
    c.ppy.assign((size_t)c.levels + 1, 15);
    if (s & 1) {
        if (e - p < 12 + (size_t)c.levels + 1) return false;
        for (int i = 0; i <= c.levels; i++) {
            c.ppx[(size_t)i] = d[p + 12 + i] & 15;
            c.ppy[(size_t)i] = d[p + 12 + i] >> 4;
        }
    }
    return true;
}

bool Decoder::parseCoc(size_t p, size_t e, const Cod& base, std::map<int, Cod>& out) {
    size_t q = p + 2;
    int ci;
    if (comps.size() < 257) { if (q >= e) return false; ci = d[q++]; }
    else { if (q + 2 > e) return false; ci = (int)u16(q); q += 2; }
    if (e - q < 6) return false;
    Cod c = base;
    unsigned s = d[q];
    c.levels = d[q + 1];
    c.xcb = d[q + 2] + 2;
    c.ycb = d[q + 3] + 2;
    c.cbStyle = d[q + 4];
    c.wavelet = d[q + 5];
    if (c.levels > 32 || c.xcb > 10 || c.ycb > 10 || c.xcb + c.ycb > 12) return false;
    c.ppx.assign((size_t)c.levels + 1, 15);
    c.ppy.assign((size_t)c.levels + 1, 15);
    if (s & 1) {
        if (e - q < 6 + (size_t)c.levels + 1) return false;
        for (int i = 0; i <= c.levels; i++) {
            c.ppx[(size_t)i] = d[q + 6 + i] & 15;
            c.ppy[(size_t)i] = d[q + 6 + i] >> 4;
        }
    }
    out[ci] = c;
    return true;
}

bool Decoder::parseQcdBody(size_t p, size_t e, Qcd& q) {
    if (p >= e) return false;
    unsigned s = d[p++];
    q.style = (int)(s & 31);
    q.guard = (int)(s >> 5);
    q.eps.clear();
    q.mu.clear();
    if (q.style == 0) {
        for (; p < e; p++) { q.eps.push_back(d[p] >> 3); q.mu.push_back(0); }
    } else if (q.style == 1 || q.style == 2) {
        for (; p + 2 <= e; p += 2) {
            unsigned v = u16(p);
            q.eps.push_back((int)(v >> 11));
            q.mu.push_back((int)(v & 0x7FF));
        }
    } else {
        return false;
    }
    return !q.eps.empty();
}

bool Decoder::parseQcc(size_t p, size_t e, std::map<int, Qcd>& out) {
    size_t q = p + 2;
    int ci;
    if (comps.size() < 257) { if (q >= e) return false; ci = d[q++]; }
    else { if (q + 2 > e) return false; ci = (int)u16(q); q += 2; }
    Qcd v;
    if (!parseQcdBody(q, e, v)) return false;
    out[ci] = v;
    return true;
}

bool Decoder::buildComp(Tile& t, int c, TileComp& tc) {
    const CompSiz& cs = comps[(size_t)c];
    tc.cod = t.coc.count(c) ? t.coc[c] : (t.hasCod ? t.cod : (coc.count(c) ? coc[c] : cod));
    tc.qcd = t.qcc.count(c) ? t.qcc[c] : (t.hasQcd ? t.qcd : (qcc.count(c) ? qcc[c] : qcd));
    /* общие для плитки поля — порядок и слои — всегда из COD */
    const Cod& main = t.hasCod ? t.cod : cod;
    tc.cod.prog = main.prog;
    tc.cod.layers = main.layers;
    tc.cod.mct = main.mct;
    tc.cod.sop = main.sop;
    tc.cod.eph = main.eph;
    /* режимы, для которых нужна другая разметка потока */
    if (tc.cod.cbStyle & (1 | 4)) return false;
    tc.x0 = ceilDiv(t.x0, cs.xr);
    tc.y0 = ceilDiv(t.y0, cs.yr);
    tc.x1 = ceilDiv(t.x1, cs.xr);
    tc.y1 = ceilDiv(t.y1, cs.yr);
    int NL = tc.cod.levels;
    tc.res.resize((size_t)NL + 1);
    for (int r = 0; r <= NL; r++) {
        Resolution& R = tc.res[(size_t)r];
        int nd = NL - r;
        R.x0 = ceilPow2(tc.x0, nd);
        R.y0 = ceilPow2(tc.y0, nd);
        R.x1 = ceilPow2(tc.x1, nd);
        R.y1 = ceilPow2(tc.y1, nd);
        R.ppx = tc.cod.ppx[(size_t)r];
        R.ppy = tc.cod.ppy[(size_t)r];
        if (r > 0 && (R.ppx == 0 || R.ppy == 0)) return false;
        R.npw = R.x1 > R.x0 ? ceilPow2(R.x1, R.ppx) - (R.x0 >> R.ppx) : 0;
        R.nph = R.y1 > R.y0 ? ceilPow2(R.y1, R.ppy) - (R.y0 >> R.ppy) : 0;
        if (R.nph && R.npw > 1000000 / R.nph) return false;
        int nb = r == 0 ? 1 : 3;
        R.bands.resize((size_t)nb);
        for (int b = 0; b < nb; b++) {
            Band& B = R.bands[(size_t)b];
            B.type = r == 0 ? 0 : b + 1;
            int lvl = r == 0 ? NL : NL - r + 1;
            int xo = (B.type == 1 || B.type == 3) ? 1 : 0, yo = (B.type == 2 || B.type == 3) ? 1 : 0;
            if (r == 0) { xo = 0; yo = 0; }
            /* B-15: tbx = ceil((tcx - 2^(nb-1) * xo) / 2^nb) */
            long long half = lvl > 0 ? 1LL << (lvl - 1) : 0;
            B.x0 = (int)(((long long)tc.x0 - half * xo + (1LL << lvl) - 1) >> lvl);
            B.y0 = (int)(((long long)tc.y0 - half * yo + (1LL << lvl) - 1) >> lvl);
            B.x1 = (int)(((long long)tc.x1 - half * xo + (1LL << lvl) - 1) >> lvl);
            B.y1 = (int)(((long long)tc.y1 - half * yo + (1LL << lvl) - 1) >> lvl);
            /* показатели квантования */
            int qi = r == 0 ? 0 : 3 * (r - 1) + 1 + b;
            int eps, mu;
            if (tc.qcd.style == 1) {
                eps = tc.qcd.eps[0] - (r == 0 ? 0 : r - 1);
                mu = tc.qcd.mu[0];
            } else {
                if ((size_t)qi >= tc.qcd.eps.size()) return false;
                eps = tc.qcd.eps[(size_t)qi];
                mu = tc.qcd.mu[(size_t)qi];
            }
            B.mb = tc.qcd.guard + eps - 1;
            if (B.mb < 0 || B.mb > 31) return false;
            int gain = B.type == 0 ? 0 : (B.type == 3 ? 2 : 1);
            B.delta = tc.cod.wavelet == 1 ? 1.0f
                      : (float)ldexp(1.0 + mu / 2048.0, cs.prec + gain - eps);
            /* области и кодовые блоки */
            int pxb = r == 0 ? R.ppx : R.ppx - 1, pyb = r == 0 ? R.ppy : R.ppy - 1;
            int xcb = std::min(tc.cod.xcb, pxb), ycb = std::min(tc.cod.ycb, pyb);
            size_t np = (size_t)R.npw * R.nph;
            B.precincts.resize(np);
            int px0 = (R.x0 >> R.ppx), py0 = (R.y0 >> R.ppy);
            for (int pj = 0; pj < R.nph; pj++)
                for (int pi = 0; pi < R.npw; pi++) {
                    PrecinctBand& P = B.precincts[(size_t)pj * R.npw + pi];
                    /* граница области в координатах полосы */
                    long long ax0 = (long long)(px0 + pi) << pxb, ay0 = (long long)(py0 + pj) << pyb;
                    long long ax1 = ax0 + (1LL << pxb), ay1 = ay0 + (1LL << pyb);
                    int x0 = (int)std::max<long long>(ax0, B.x0), y0 = (int)std::max<long long>(ay0, B.y0);
                    int x1 = (int)std::min<long long>(ax1, B.x1), y1 = (int)std::min<long long>(ay1, B.y1);
                    if (x1 <= x0 || y1 <= y0) continue;
                    int cx0 = x0 >> xcb, cy0 = y0 >> ycb;
                    int cx1 = ceilPow2(x1, xcb), cy1 = ceilPow2(y1, ycb);
                    P.gw = cx1 - cx0;
                    P.gh = cy1 - cy0;
                    if (P.gh && P.gw > 1000000 / P.gh) return false;
                    P.blocks.resize((size_t)P.gw * P.gh);
                    for (int j = 0; j < P.gh; j++)
                        for (int i = 0; i < P.gw; i++) {
                            CodeBlock& cb = P.blocks[(size_t)j * P.gw + i];
                            cb.gx = i;
                            cb.gy = j;
                            cb.x0 = std::max((cx0 + i) << xcb, x0);
                            cb.y0 = std::max((cy0 + j) << ycb, y0);
                            cb.x1 = std::min((cx0 + i + 1) << xcb, x1);
                            cb.y1 = std::min((cy0 + j + 1) << ycb, y1);
                        }
                    P.incl.init(P.gw, P.gh);
                    P.imsb.init(P.gw, P.gh);
                }
        }
    }
    return true;
}

/* один пакет (B.9–B.10); pos — текущая позиция в данных плитки */
bool Decoder::onePacket(BitIn& in, std::vector<TileComp>& tcs, int comp, int r, int k, int layer, const Cod& tcod,
                        size_t& pos, size_t& partEnd) {
    (void)partEnd;
    TileComp& tc = tcs[(size_t)comp];
    Resolution& R = tc.res[(size_t)r];
    if (tcod.sop && pos + 6 <= in.end && d[pos] == 0xFF && d[pos + 1] == 0x91) pos += 6;
    in.pos = pos;
    in.cnt = 0;
    in.skip = false;
    struct Add { CodeBlock* cb; size_t len; };
    std::vector<Add> adds;
    if (in.bit()) {
        for (size_t b = 0; b < R.bands.size(); b++) {
            if ((size_t)k >= R.bands[b].precincts.size()) continue;
            PrecinctBand& P = R.bands[b].precincts[(size_t)k];
            for (size_t i = 0; i < P.blocks.size(); i++) {
                CodeBlock& cb = P.blocks[i];
                bool now;
                if (cb.included) {
                    now = in.bit() != 0;
                } else {
                    now = tagDecode(P.incl, in, cb.gx, cb.gy, layer + 1);
                    if (now) {
                        int z = 1;
                        while (!tagDecode(P.imsb, in, cb.gx, cb.gy, z)) {
                            if (in.bad || z > 64) return false;
                            z++;
                        }
                        cb.zeroPlanes = z - 1;
                        cb.included = true;
                    }
                }
                if (!now) continue;
                /* число проходов (B.10.6) */
                int passes;
                if (!in.bit()) passes = 1;
                else if (!in.bit()) passes = 2;
                else {
                    unsigned v = in.bits(2);
                    if (v < 3) passes = 3 + (int)v;
                    else {
                        v = in.bits(5);
                        if (v < 31) passes = 6 + (int)v;
                        else passes = 37 + (int)in.bits(7);
                    }
                }
                while (in.bit()) {
                    cb.lblock++;
                    if (in.bad || cb.lblock > 32) return false;
                }
                int lg = 0;
                while ((1 << (lg + 1)) <= passes) lg++;
                int nb = cb.lblock + lg;
                if (nb > 32) return false;
                unsigned len = in.bits(nb);
                if (in.bad) return false;
                cb.passes += passes;
                Add a = {&cb, len};
                adds.push_back(a);
            }
        }
    }
    if (in.bad) return false;
    in.align();
    pos = in.pos;
    if (tcod.eph && pos + 2 <= in.end && d[pos] == 0xFF && d[pos + 1] == 0x92) pos += 2;
    for (size_t i = 0; i < adds.size(); i++) {
        if (adds[i].len > in.end - std::min(pos, in.end)) {
            /* обрезанный файл: остаток блока потерян, пакеты дальше читать незачем */
            return false;
        }
        Seg s = {pos, adds[i].len};
        adds[i].cb->segs.push_back(s);
        pos += adds[i].len;
    }
    return true;
}

/* порядок пакетов: B.12 */
bool Decoder::packets(Tile& t, std::vector<TileComp>& tcs) {
    const Cod& c = tcs[0].cod;
    int maxRes = 0;
    for (size_t i = 0; i < tcs.size(); i++) maxRes = std::max(maxRes, tcs[i].cod.levels);
    /* части плитки идут подряд; пакеты могут переходить из одной в другую на границе пакета */
    size_t part = 0;
    if (t.parts.empty()) return true;
    size_t pos = t.parts[0].pos;
    BitIn in(d, pos, t.parts[0].end);
    /* вызов с переходом на следующую часть; false — данные кончились (не ошибка) */
    struct Step {
        static bool go(Decoder& self, Tile& tl, std::vector<TileComp>& tc, BitIn& b, size_t& pt, size_t& ps,
                       int comp, int r, int k, int layer, const Cod& cd) {
            while (ps >= tl.parts[pt].end) {
                if (pt + 1 >= tl.parts.size()) return false;
                pt++;
                ps = tl.parts[pt].pos;
                b.end = tl.parts[pt].end;
            }
            size_t e = tl.parts[pt].end;
            return self.onePacket(b, tc, comp, r, k, layer, cd, ps, e);
        }
    };
    int L = c.layers;
    int ncomp = (int)tcs.size();
    if (c.prog == 0) { /* LRCP */
        for (int l = 0; l < L; l++)
            for (int r = 0; r <= maxRes; r++)
                for (int i = 0; i < ncomp; i++) {
                    if (r > tcs[(size_t)i].cod.levels) continue;
                    Resolution& R = tcs[(size_t)i].res[(size_t)r];
                    int np = R.npw * R.nph;
                    for (int k = 0; k < np; k++)
                        if (!Step::go(*this, t, tcs, in, part, pos, i, r, k, l, c)) return true;
                }
        return true;
    }
    if (c.prog == 1) { /* RLCP */
        for (int r = 0; r <= maxRes; r++)
            for (int l = 0; l < L; l++)
                for (int i = 0; i < ncomp; i++) {
                    if (r > tcs[(size_t)i].cod.levels) continue;
                    Resolution& R = tcs[(size_t)i].res[(size_t)r];
                    int np = R.npw * R.nph;
                    for (int k = 0; k < np; k++)
                        if (!Step::go(*this, t, tcs, in, part, pos, i, r, k, l, c)) return true;
                }
        return true;
    }
    if (c.prog > 4) return false;
    /* порядки по положению: шаг — наименьший размер области в сетке изображения */
    long long dx = 0, dy = 0;
    for (int i = 0; i < ncomp; i++) {
        const CompSiz& cs = comps[(size_t)i];
        TileComp& tc = tcs[(size_t)i];
        for (int r = 0; r <= tc.cod.levels; r++) {
            int nd = tc.cod.levels - r;
            long long sx = (long long)cs.xr << (tc.res[(size_t)r].ppx + nd), sy = (long long)cs.yr << (tc.res[(size_t)r].ppy + nd);
            dx = dx ? std::min(dx, sx) : sx;
            dy = dy ? std::min(dy, sy) : sy;
        }
    }
    if (dx <= 0 || dy <= 0) return true;
    /* номер области для (x, y); -1 — в этой точке области нет */
    struct At {
        static int prec(const Decoder& dc, TileComp& tc, const CompSiz& cs, int r, long long x, long long y, const Tile& tl) {
            (void)dc;
            Resolution& R = tc.res[(size_t)r];
            if (!R.npw || !R.nph) return -1;
            int nd = tc.cod.levels - r;
            long long sy = (long long)cs.yr << (R.ppy + nd), sx = (long long)cs.xr << (R.ppx + nd);
            bool okY = y % sy == 0 || (y == tl.y0 && (((long long)R.y0 << nd) % (1LL << (R.ppy + nd))) != 0);
            bool okX = x % sx == 0 || (x == tl.x0 && (((long long)R.x0 << nd) % (1LL << (R.ppx + nd))) != 0);
            if (!okX || !okY) return -1;
            long long cx = (x + cs.xr - 1) / cs.xr, cy = (y + cs.yr - 1) / cs.yr;
            long long px = ((cx + (1LL << nd) - 1) >> nd) >> R.ppx, py = ((cy + (1LL << nd) - 1) >> nd) >> R.ppy;
            long long k = (px - (R.x0 >> R.ppx)) + (long long)R.npw * (py - (R.y0 >> R.ppy));
            if (k < 0 || k >= (long long)R.npw * R.nph) return -1;
            return (int)k;
        }
    };
    #define NEXT(v, step) ((v) - (v) % (step) + (step))
    if (c.prog == 2) { /* RPCL */
        for (int r = 0; r <= maxRes; r++)
            for (long long y = t.y0; y < t.y1; y = NEXT(y, dy))
                for (long long x = t.x0; x < t.x1; x = NEXT(x, dx))
                    for (int i = 0; i < ncomp; i++) {
                        if (r > tcs[(size_t)i].cod.levels) continue;
                        int k = At::prec(*this, tcs[(size_t)i], comps[(size_t)i], r, x, y, t);
                        if (k < 0) continue;
                        for (int l = 0; l < L; l++)
                            if (!Step::go(*this, t, tcs, in, part, pos, i, r, k, l, c)) return true;
                    }
    } else if (c.prog == 3) { /* PCRL */
        for (long long y = t.y0; y < t.y1; y = NEXT(y, dy))
            for (long long x = t.x0; x < t.x1; x = NEXT(x, dx))
                for (int i = 0; i < ncomp; i++)
                    for (int r = 0; r <= tcs[(size_t)i].cod.levels; r++) {
                        int k = At::prec(*this, tcs[(size_t)i], comps[(size_t)i], r, x, y, t);
                        if (k < 0) continue;
                        for (int l = 0; l < L; l++)
                            if (!Step::go(*this, t, tcs, in, part, pos, i, r, k, l, c)) return true;
                    }
    } else { /* CPRL */
        for (int i = 0; i < ncomp; i++)
            for (long long y = t.y0; y < t.y1; y = NEXT(y, dy))
                for (long long x = t.x0; x < t.x1; x = NEXT(x, dx))
                    for (int r = 0; r <= tcs[(size_t)i].cod.levels; r++) {
                        int k = At::prec(*this, tcs[(size_t)i], comps[(size_t)i], r, x, y, t);
                        if (k < 0) continue;
                        for (int l = 0; l < L; l++)
                            if (!Step::go(*this, t, tcs, in, part, pos, i, r, k, l, c)) return true;
                    }
    }
    #undef NEXT
    return true;
}

/* кодовые блоки -> коэффициенты разрешений; levels[r] — полосы вперемежку для idwt */
bool Decoder::tier1(TileComp& tc, int comp, std::vector<std::vector<float> >& levels) {
    (void)comp;
    bool rev = tc.cod.wavelet == 1;
    bool reset = (tc.cod.cbStyle & 2) != 0, causal = (tc.cod.cbStyle & 8) != 0, segSym = (tc.cod.cbStyle & 32) != 0;
    levels.resize(tc.res.size());
    std::string joined;
    for (size_t r = 0; r < tc.res.size(); r++) {
        Resolution& R = tc.res[r];
        int rw = R.x1 - R.x0, rh = R.y1 - R.y0;
        if (rw <= 0 || rh <= 0) continue;
        levels[r].assign((size_t)rw * rh, 0.0f);
        for (size_t b = 0; b < R.bands.size(); b++) {
            Band& B = R.bands[b];
            for (size_t p = 0; p < B.precincts.size(); p++) {
                PrecinctBand& P = B.precincts[p];
                for (size_t i = 0; i < P.blocks.size(); i++) {
                    CodeBlock& cb = P.blocks[i];
                    if (!cb.passes || cb.segs.empty()) continue;
                    int w = cb.x1 - cb.x0, h = cb.y1 - cb.y0;
                    if (w <= 0 || h <= 0) continue;
                    joined.clear();
                    for (size_t s = 0; s < cb.segs.size(); s++)
                        joined.append((const char*)d + cb.segs[s].pos, cb.segs[s].len);
                    if (joined.empty()) continue;
                    int bp = B.mb - 1 - cb.zeroPlanes;
                    if (bp < 0) continue;
                    if (bp > 30) return false;
                    Tier1 t(w, h, B.type, causal);
                    MqDecoder mq((const unsigned char*)joined.data(), 0, joined.size());
                    t.mq = &mq;
                    int type = 2, lastBp = bp;
                    for (int k = 0; k < cb.passes; k++) {
                        if (bp < 0) break;
                        lastBp = bp;
                        if (type == 0) t.sigPass(bp);
                        else if (type == 1) t.refPass(bp);
                        else {
                            t.cleanPass(bp);
                            if (segSym) for (int q = 0; q < 4; q++) mq.bit(t.cx[18]);
                        }
                        if (reset) t.resetCx();
                        type = (type + 1) % 3;
                        if (type == 0) bp--;
                    }
                    /* полоса на своём месте в чересстрочной сетке разрешения */
                    int xo = (B.type == 1 || B.type == 3) ? 1 : 0, yo = (B.type == 2 || B.type == 3) ? 1 : 0;
                    float half = (lastBp > 0 || !rev) ? 0.5f * (float)(1u << lastBp) : 0.0f;
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < w; x++) {
                            unsigned m = t.mag[(size_t)y * w + x];
                            if (!m) continue;
                            float v = ((float)m + half) * B.delta;
                            if (t.sgn[t.at(x, y)] < 0) v = -v;
                            long long gx, gy;
                            if (r == 0) { gx = (long long)cb.x0 + x - R.x0; gy = (long long)cb.y0 + y - R.y0; }
                            else {
                                gx = 2LL * (cb.x0 + x) + xo - R.x0;
                                gy = 2LL * (cb.y0 + y) + yo - R.y0;
                            }
                            if (gx >= 0 && gx < rw && gy >= 0 && gy < rh)
                                levels[r][(size_t)gy * rw + (size_t)gx] = v;
                        }
                }
            }
        }
    }
    return true;
}

/* обратное вейвлет-преобразование снизу вверх */
bool Decoder::idwt(TileComp& tc, std::vector<std::vector<float> >& levels) {
    bool rev = tc.cod.wavelet == 1;
    std::vector<float> cur = levels[0];
    int cw = tc.res[0].x1 - tc.res[0].x0, ch = tc.res[0].y1 - tc.res[0].y0;
    std::vector<float> tmp, col;
    for (size_t r = 1; r < tc.res.size(); r++) {
        Resolution& R = tc.res[r];
        int w = R.x1 - R.x0, h = R.y1 - R.y0;
        if (w <= 0 || h <= 0) { cur.clear(); cw = ch = 0; continue; }
        std::vector<float>& a = levels[r];
        /* младшая полоса: глобальная координата 2*(x0 + x) */
        Resolution& P = tc.res[r - 1];
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++) {
                long long gx = 2LL * (P.x0 + x) - R.x0, gy = 2LL * (P.y0 + y) - R.y0;
                if (gx >= 0 && gx < w && gy >= 0 && gy < h) a[(size_t)gy * w + (size_t)gx] = cur[(size_t)y * cw + x];
            }
        for (int y = 0; y < h; y++) synth1d(&a[(size_t)y * w], w, R.x0, rev, tmp);
        col.resize((size_t)h);
        for (int x = 0; x < w; x++) {
            for (int y = 0; y < h; y++) col[(size_t)y] = a[(size_t)y * w + x];
            synth1d(&col[0], h, R.y0, rev, tmp);
            for (int y = 0; y < h; y++) a[(size_t)y * w + x] = col[(size_t)y];
        }
        cur.swap(a);
        std::vector<float>().swap(a);
        cw = w;
        ch = h;
    }
    tc.samples.swap(cur);
    return true;
}

bool Decoder::decodeTile(int idx, Tile& t) {
    int p = idx % ntx, q = idx / ntx;
    t.x0 = std::max(xtosiz + p * xtsiz, xosiz);
    t.y0 = std::max(ytosiz + q * ytsiz, yosiz);
    t.x1 = (int)std::min<long long>((long long)xtosiz + (long long)(p + 1) * xtsiz, xsiz);
    t.y1 = (int)std::min<long long>((long long)ytosiz + (long long)(q + 1) * ytsiz, ysiz);
    if (t.x1 <= t.x0 || t.y1 <= t.y0) return true;
    std::vector<TileComp> tcs(comps.size());
    for (size_t c = 0; c < comps.size(); c++)
        if (!buildComp(t, (int)c, tcs[c])) return false;
    if (!packets(t, tcs)) return false;
    for (size_t c = 0; c < comps.size(); c++) {
        std::vector<std::vector<float> > lv;
        if (!tier1(tcs[c], (int)c, lv)) return false;
        /* блоки больше не нужны: память освобождается до вейвлетов */
        for (size_t r = 0; r < tcs[c].res.size(); r++)
            for (size_t b = 0; b < tcs[c].res[r].bands.size(); b++)
                std::vector<PrecinctBand>().swap(tcs[c].res[r].bands[b].precincts);
        if (!idwt(tcs[c], lv)) return false;
    }
    /* обратное преобразование цвета */
    if (tcs[0].cod.mct && comps.size() >= 3 && tcs[0].samples.size() == tcs[1].samples.size() &&
        tcs[0].samples.size() == tcs[2].samples.size()) {
        float* a = tcs[0].samples.empty() ? NULL : &tcs[0].samples[0];
        float* b = tcs[1].samples.empty() ? NULL : &tcs[1].samples[0];
        float* c = tcs[2].samples.empty() ? NULL : &tcs[2].samples[0];
        size_t n = tcs[0].samples.size();
        if (tcs[0].cod.wavelet == 1) {
            for (size_t i = 0; i < n; i++) {
                float g = a[i] - floorf((b[i] + c[i]) * 0.25f);
                float r = c[i] + g, bl = b[i] + g;
                a[i] = r; b[i] = g; c[i] = bl;
            }
        } else {
            for (size_t i = 0; i < n; i++) {
                float y = a[i], u = b[i], v = c[i];
                a[i] = y + 1.402f * v;
                b[i] = y - 0.34413f * u - 0.71414f * v;
                c[i] = y + 1.772f * u;
            }
        }
    }
    /* отсчёты -> 8 бит в плоскость компонента */
    for (size_t c = 0; c < comps.size(); c++) {
        TileComp& tc = tcs[c];
        const CompSiz& cs = comps[c];
        int tw = tc.x1 - tc.x0, th = tc.y1 - tc.y0;
        if (tw <= 0 || th <= 0 || tc.samples.size() < (size_t)tw * th) continue;
        int ox = tc.x0 - ceilDiv(xosiz, cs.xr), oy = tc.y0 - ceilDiv(yosiz, cs.yr);
        float shift = (float)(1u << (cs.prec - 1));
        float maxV = (float)((1u << cs.prec) - 1);
        float scale = 255.0f / maxV;
        for (int y = 0; y < th; y++) {
            int py = oy + y;
            if (py < 0 || py >= planeH[c]) continue;
            for (int x = 0; x < tw; x++) {
                int px = ox + x;
                if (px < 0 || px >= planeW[c]) continue;
                float v = tc.samples[(size_t)y * tw + x] + shift;
                v = v * scale + 0.5f;
                planes[c][(size_t)py * planeW[c] + px] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
        std::vector<float>().swap(tc.samples);
    }
    return true;
}

bool Decoder::run() {
    if (size < 4 || u16(0) != 0xFF4F) return false;
    size_t p = 2;
    Tile* cur = NULL;
    while (p + 2 <= size) {
        unsigned m = u16(p);
        if (m == 0xFFD9) break;
        if (m < 0xFF00) return !tiles.empty();
        size_t seg = p + 2;
        size_t len = u16(seg);
        if (len < 2 || seg + len > size) break;
        size_t e = seg + len;
        switch (m) {
            case 0xFF51:
                if (!parseSiz(seg, e)) return false;
                break;
            case 0xFF52:
                if (comps.empty() || !parseCod(seg, e, cod)) return false;
                hasCod = true;
                break;
            case 0xFF53:
                if (comps.empty() || !hasCod || !parseCoc(seg, e, cod, coc)) return false;
                break;
            case 0xFF5C:
                if (!parseQcdBody(seg + 2, e, qcd)) return false;
                hasQcd = true;
                break;
            case 0xFF5D:
                if (comps.empty() || !parseQcc(seg, e, qcc)) return false;
                break;
            case 0xFF5F: case 0xFF60: case 0xFF61: /* POC, PPM, PPT: другая разметка пакетов */
                return false;
            case 0xFF90: { /* SOT */
                if (comps.empty() || !hasCod || !hasQcd || e - seg < 10) return false;
                int idx = (int)u16(seg + 2);
                unsigned psot = u32(seg + 4);
                if ((long long)idx >= (long long)ntx * nty) return false;
                cur = &tiles[idx];
                /* заголовок части плитки: до SOD могут быть свои COD/QCD */
                size_t q = e;
                bool sod = false;
                while (q + 2 <= size) {
                    unsigned mm = u16(q);
                    if (mm == 0xFF93) { q += 2; sod = true; break; }
                    size_t sg = q + 2, ln = u16(sg);
                    if (ln < 2 || sg + ln > size) return false;
                    size_t en = sg + ln;
                    if (mm == 0xFF52) { if (!parseCod(sg, en, cur->cod)) return false; cur->hasCod = true; }
                    else if (mm == 0xFF53) { if (!parseCoc(sg, en, cur->hasCod ? cur->cod : cod, cur->coc)) return false; }
                    else if (mm == 0xFF5C) { if (!parseQcdBody(sg + 2, en, cur->qcd)) return false; cur->hasQcd = true; }
                    else if (mm == 0xFF5D) { if (!parseQcc(sg, en, cur->qcc)) return false; }
                    else if (mm == 0xFF5F || mm == 0xFF61) return false;
                    q = en;
                }
                if (!sod) return !tiles.empty();
                size_t partEnd = psot ? p + psot : size;
                if (partEnd > size || partEnd < q) partEnd = size;
                if (!psot && partEnd >= 2 && u16(partEnd - 2) == 0xFFD9) partEnd -= 2;
                TilePart tp = {q, partEnd};
                cur->parts.push_back(tp);
                p = partEnd;
                continue;
            }
            default:
                break;
        }
        p = e;
    }
    if (comps.empty()) return false;
    planes.resize(comps.size());
    planeW.resize(comps.size());
    planeH.resize(comps.size());
    for (size_t c = 0; c < comps.size(); c++) {
        planeW[c] = ceilDiv(xsiz, comps[c].xr) - ceilDiv(xosiz, comps[c].xr);
        planeH[c] = ceilDiv(ysiz, comps[c].yr) - ceilDiv(yosiz, comps[c].yr);
        planes[c].assign((size_t)planeW[c] * planeH[c], 0);
    }
    bool any = false;
    for (std::map<int, Tile>::iterator it = tiles.begin(); it != tiles.end(); ++it) {
        if (!decodeTile(it->first, it->second)) return false;
        any = true;
    }
    return any;
}

} /* namespace */

bool jpxDecode(const std::string& data, JpxImage& out) {
    const unsigned char* d = (const unsigned char*)data.data();
    size_t n = data.size();
    size_t csPos = 0, csLen = n;
    int enumCs = -1;
    /* палитра и карта каналов из контейнера JP2 */
    std::vector<std::vector<unsigned char> > palette; /* [столбец][вход], уже в 8 битах */
    struct CMap { int comp, type, col; };
    std::vector<CMap> cmap;
    std::vector<int> alphaChannels;
    struct Be {
        static unsigned u32(const unsigned char* p) { return ((unsigned)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
        static unsigned u16(const unsigned char* p) { return ((unsigned)p[0] << 8) | p[1]; }
    };
    if (n >= 12 && Be::u32(d + 4) == 0x6A502020) { /* «jP  » */
        size_t p = 0;
        bool found = false;
        while (p + 8 <= n) {
            unsigned long long len = Be::u32(d + p);
            unsigned type = Be::u32(d + p + 4);
            size_t hdr = 8;
            if (len == 1) {
                if (p + 16 > n) break;
                len = ((unsigned long long)Be::u32(d + p + 8) << 32) | Be::u32(d + p + 12);
                hdr = 16;
            } else if (len == 0) {
                len = n - p;
            }
            if (len < hdr || len > n - p) len = n - p;
            size_t end = p + (size_t)len;
            if (type == 0x6A703263) { /* jp2c */
                csPos = p + hdr;
                csLen = end - csPos;
                found = true;
                break;
            }
            if (type == 0x6A703268) { /* jp2h */
                size_t q = p + hdr;
                while (q + 8 <= end) {
                    size_t l2 = Be::u32(d + q);
                    unsigned t2 = Be::u32(d + q + 4);
                    if (l2 < 8 || q + l2 > end) break;
                    const unsigned char* b = d + q + 8;
                    size_t bl = l2 - 8;
                    if (t2 == 0x636F6C72 && bl >= 7 && b[0] == 1 && enumCs < 0) { /* colr */
                        enumCs = (int)Be::u32(b + 3);
                    } else if (t2 == 0x70636C72 && bl >= 3) { /* pclr */
                        unsigned ne = Be::u16(b), npc = b[2];
                        size_t at = 3 + (size_t)npc;
                        if (npc && ne && ne <= 1024 && at <= bl) {
                            std::vector<int> bits(npc);
                            for (unsigned i = 0; i < npc; i++) bits[i] = (b[3 + i] & 0x7F) + 1;
                            palette.assign(npc, std::vector<unsigned char>(ne, 0));
                            for (unsigned e = 0; e < ne; e++)
                                for (unsigned c = 0; c < npc; c++) {
                                    int nb = (bits[c] + 7) / 8;
                                    unsigned v = 0;
                                    for (int k = 0; k < nb && at < bl; k++) v = (v << 8) | b[at++];
                                    palette[c][e] = bits[c] > 8 ? (unsigned char)(v >> (bits[c] - 8))
                                                                : (unsigned char)(v * 255 / ((1u << bits[c]) - 1));
                                }
                        }
                    } else if (t2 == 0x636D6170) { /* cmap */
                        for (size_t k = 0; k + 4 <= bl; k += 4) {
                            CMap m = {(int)Be::u16(b + k), b[k + 2], b[k + 3]};
                            cmap.push_back(m);
                        }
                    } else if (t2 == 0x63646566 && bl >= 2) { /* cdef: какие каналы — прозрачность */
                        unsigned cnt = Be::u16(b);
                        for (unsigned k = 0; k < cnt && 2 + (size_t)(k + 1) * 6 <= bl; k++)
                            if (Be::u16(b + 2 + (size_t)k * 6 + 2) != 0) alphaChannels.push_back((int)Be::u16(b + 2 + (size_t)k * 6));
                    }
                    q += l2;
                }
            }
            p = end;
        }
        if (!found) return false;
    }
    Decoder dec(d + csPos, csLen);
    if (!dec.run()) return false;
    int W = dec.imgW(), H = dec.imgH();
    if (W <= 0 || H <= 0) return false;
    /* каналы: компоненты или столбцы палитры */
    struct Chan { int comp, col; };
    std::vector<Chan> ch;
    if (!palette.empty() && !cmap.empty()) {
        for (size_t i = 0; i < cmap.size(); i++) {
            if (cmap[i].comp < 0 || (size_t)cmap[i].comp >= dec.planes.size()) return false;
            Chan c = {cmap[i].comp, cmap[i].type == 1 ? cmap[i].col : -1};
            if (c.col >= (int)palette.size()) return false;
            ch.push_back(c);
        }
    } else {
        for (size_t i = 0; i < dec.planes.size(); i++) { Chan c = {(int)i, -1}; ch.push_back(c); }
    }
    std::vector<Chan> color;
    for (size_t i = 0; i < ch.size(); i++)
        if (std::find(alphaChannels.begin(), alphaChannels.end(), (int)i) == alphaChannels.end()) color.push_back(ch[i]);
    if (color.empty()) color = ch;
    int cs = JpxImage::CS_UNKNOWN;
    size_t want = color.size();
    if (enumCs == 17) { cs = JpxImage::CS_GRAY; want = 1; }
    else if (enumCs == 16 || enumCs == 18) { cs = JpxImage::CS_RGB; want = 3; }
    else if (enumCs == 12) { cs = JpxImage::CS_CMYK; want = 4; }
    else if (want == 2) { want = 1; cs = JpxImage::CS_GRAY; }
    else if (want == 1) cs = JpxImage::CS_GRAY;
    else if (want == 3) cs = JpxImage::CS_RGB;
    else if (want == 4) cs = JpxImage::CS_CMYK;
    else return false;
    if (color.size() < want) return false;
    color.resize(want);
    out.w = W;
    out.h = H;
    out.comps = (int)want;
    out.colorSpace = cs;
    out.pixels.resize((size_t)W * H * want);
    for (size_t c = 0; c < want; c++) {
        int ci = color[c].comp, col = color[c].col;
        int pw = dec.planeW[(size_t)ci], ph = dec.planeH[(size_t)ci];
        int xr = dec.comps[(size_t)ci].xr, yr = dec.comps[(size_t)ci].yr;
        const unsigned char* pl = dec.planes[(size_t)ci].empty() ? NULL : &dec.planes[(size_t)ci][0];
        if (!pl) continue;
        for (int y = 0; y < H; y++) {
            int py = std::min(y / yr, ph - 1);
            char* o = &out.pixels[((size_t)y * W) * want + c];
            for (int x = 0; x < W; x++) {
                unsigned char v = pl[(size_t)py * pw + std::min(x / xr, pw - 1)];
                if (col >= 0) {
                    /* индекс палитры: плоскость уже растянута на 0–255, возвращаем исходное значение */
                    int prec = dec.comps[(size_t)ci].prec;
                    unsigned idx = prec >= 8 ? v : (unsigned)(v * ((1u << prec) - 1) + 127) / 255;
                    const std::vector<unsigned char>& pal = palette[(size_t)col];
                    v = idx < pal.size() ? pal[idx] : 0;
                }
                o[(size_t)x * want] = (char)v;
            }
        }
    }
    /* sYCC -> RGB */
    if (enumCs == 18 && want == 3) {
        for (size_t i = 0; i + 3 <= out.pixels.size(); i += 3) {
            float y = (unsigned char)out.pixels[i], u = (float)(unsigned char)out.pixels[i + 1] - 128.0f, v = (float)(unsigned char)out.pixels[i + 2] - 128.0f;
            float r = y + 1.402f * v, g = y - 0.34413f * u - 0.71414f * v, b = y + 1.772f * u;
            out.pixels[i] = (char)(unsigned char)(r < 0 ? 0 : (r > 255 ? 255 : r + 0.5f));
            out.pixels[i + 1] = (char)(unsigned char)(g < 0 ? 0 : (g > 255 ? 255 : g + 0.5f));
            out.pixels[i + 2] = (char)(unsigned char)(b < 0 ? 0 : (b > 255 ? 255 : b + 0.5f));
        }
    }
    return true;
}
