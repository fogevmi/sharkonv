/* Читатели FB3 и CHM (1.9).
   FB3 — это ZIP с разметкой, почти как у FB2: переводится в FB2 и читается читателем FB2.
   CHM — контейнер ITSF со сжатием LZX: файлы распаковываются, оглавление .hhc становится NCX,
   и книга читается читателем EPUB. Так сноски, картинки и главы разбирает уже проверенный код. */
#include "convert.h"
#include "markup.h"
#include "zip.h"
#include <algorithm>

/* ================================= FB3 ================================== */

namespace {

struct Fb3 {
    std::map<std::string, Bytes>* files;  /* имена в нижнем регистре */
    std::map<std::string, std::string> imgByRel;  /* rId -> имя binary */
    std::string binaries;
    std::set<std::string> done;

    static std::string normPath(const std::string& base, std::string t) {
        if (!t.empty() && t[0] == '/') t = t.substr(1);
        else if (!base.empty()) t = base + "/" + t;
        std::vector<std::string> parts;
        size_t p = 0;
        while (p <= t.size()) {
            size_t e = t.find('/', p);
            if (e == std::string::npos) e = t.size();
            std::string seg = t.substr(p, e - p);
            if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
            else if (!seg.empty() && seg != ".") parts.push_back(seg);
            p = e + 1;
        }
        std::string out;
        for (size_t i = 0; i < parts.size(); i++) out += (i ? "/" : "") + parts[i];
        return lowerAscii(out);
    }

    /* связи части: rId -> цель */
    std::map<std::string, std::string> rels(const std::string& part) {
        std::map<std::string, std::string> out;
        std::string dir = pathDirOf(part), base = pathBase(part);
        std::string rp = (dir.empty() ? std::string() : dir + "/") + "_rels/" + base + ".rels";
        std::map<std::string, Bytes>::iterator it = files->find(lowerAscii(rp));
        if (it == files->end()) return out;
        MNode d = parseMarkup(decodeText(it->second, NULL), false);
        std::vector<const MNode*> r = findNodes(&d, "relationship");
        for (size_t i = 0; i < r.size(); i++) out[r[i]->attr("id")] = normPath(dir, r[i]->attr("target"));
        return out;
    }

    /* картинка как <binary> FB2; возвращает её id */
    std::string addImage(const std::string& path) {
        std::string name = pathBase(path);
        if (done.count(name)) return name;
        std::map<std::string, Bytes>::iterator it = files->find(path);
        if (it == files->end()) return std::string();
        done.insert(name);
        binaries += "<binary id=\"" + escXml(name) + "\" content-type=\"" + escXml(mimeFromExt(pathExt(name))) + "\">" +
                    b64encode(it->second) + "</binary>\n";
        return name;
    }

    std::map<std::string, std::string> bodyRels;

    std::string imageTag(const MNode* n) {
        std::string src = n->attr("src");
        std::map<std::string, std::string>::iterator it = bodyRels.find(src);
        std::string id = it != bodyRels.end() ? addImage(it->second) : std::string();
        return id.empty() ? std::string() : "<image l:href=\"#" + escXml(id) + "\"/>";
    }

    std::string kids(const MNode* n, int depth) {
        std::string s;
        for (size_t i = 0; i < n->kids.size(); i++) s += node(n->kids[i].get(), depth + 1);
        return s;
    }

    static bool hasBlockChild(const MNode* n) {
        for (size_t i = 0; i < n->kids.size(); i++)
            if (!n->kids[i]->text && (n->kids[i]->name == "p" || n->kids[i]->name == "ul" || n->kids[i]->name == "ol" || n->kids[i]->name == "subtitle"))
                return true;
        return false;
    }

    std::string node(const MNode* n, int depth) {
        if (depth > 80) return std::string();
        if (n->text) return escXml(n->value);
        const std::string& t = n->name;
        if (t == "em" || t == "emphasis") return "<emphasis>" + kids(n, depth) + "</emphasis>";
        if (t == "strong" || t == "strikethrough" || t == "sub" || t == "sup" || t == "code")
            return "<" + t + ">" + kids(n, depth) + "</" + t + ">";
        if (t == "underline" || t == "span" || t == "marker" || t == "paper-page-break" || t == "autotext") return kids(n, depth);
        if (t == "a") return "<a l:href=\"" + escXml(n->attr("href")) + "\">" + kids(n, depth) + "</a>";
        if (t == "note") {
            std::string href = n->attr("href");
            if (!href.empty() && href[0] != '#') href = "#" + href;
            std::string label = collapse(n->textContent());
            if (label.empty()) label = "*";
            return "<a l:href=\"" + escXml(href) + "\" type=\"note\">" + escXml(label) + "</a>";
        }
        if (t == "img") return imageTag(n);
        if (t == "br") return " ";
        if (t == "p" || t == "subtitle") {
            /* картинка отдельным абзацем — блок картинки FB2 */
            if (t == "p" && n->kids.size() == 1 && !n->kids[0]->text && n->kids[0]->name == "img") return imageTag(n->kids[0].get());
            return "<" + t + ">" + kids(n, depth) + "</" + t + ">";
        }
        if (t == "pre") return "<p><code>" + kids(n, depth) + "</code></p>";
        if (t == "ul" || t == "ol") {
            std::string s;
            int num = 1;
            for (size_t i = 0; i < n->kids.size(); i++) {
                const MNode* li = n->kids[i].get();
                if (li->text || li->name != "li") continue;
                std::string mark = t == "ol" ? intToStr(num++) + ". " : "\xE2\x80\xA2 ";
                s += hasBlockChild(li) ? kids(li, depth) : "<p>" + mark + kids(li, depth) + "</p>";
            }
            return s;
        }
        if (t == "spacing") return "<empty-line/>";
        if (t == "blockquote") return "<cite>" + kids(n, depth) + "</cite>";
        if (t == "poem" || t == "stanza" || t == "epigraph" || t == "table" || t == "tr" || t == "text-author")
            return "<" + t + ">" + kids(n, depth) + "</" + t + ">";
        if (t == "td" || t == "th") {
            std::string attrs;
            if (n->hasAttr("colspan")) attrs += " colspan=\"" + escXml(n->attr("colspan")) + "\"";
            if (n->hasAttr("rowspan")) attrs += " rowspan=\"" + escXml(n->attr("rowspan")) + "\"";
            return "<" + t + attrs + ">" + kids(n, depth) + "</" + t + ">";
        }
        if (t == "title") {
            std::string s = "<title>";
            for (size_t i = 0; i < n->kids.size(); i++) {
                const MNode* k = n->kids[i].get();
                if (k->text) continue;
                s += "<p>" + kids(k, depth) + "</p>";
            }
            return s + "</title>";
        }
        if (t == "section") {
            std::string s = "<section" + (n->hasAttr("id") ? " id=\"" + escXml(n->attr("id")) + "\"" : std::string()) + ">";
            return s + kids(n, depth) + "</section>";
        }
        return kids(n, depth);
    }
};

bool readFb3Impl(const Bytes& data, const std::string& fileName, Book& out, std::string* err) {
    std::map<std::string, Bytes> raw, files;
    if (!zipRead(data, raw, err)) return false;
    for (std::map<std::string, Bytes>::iterator it = raw.begin(); it != raw.end(); ++it)
        files[lowerAscii(it->first)] = it->second;
    Fb3 f;
    f.files = &files;
    /* путь к описанию и телу — по связям, с запасным вариантом стандартных имён */
    std::string descPath = "fb3/description.xml", coverPath;
    std::map<std::string, std::string> rootRels;
    std::map<std::string, Bytes>::iterator rr = files.find("_rels/.rels");
    if (rr != files.end()) {
        MNode d = parseMarkup(decodeText(rr->second, NULL), false);
        std::vector<const MNode*> r = findNodes(&d, "relationship");
        for (size_t i = 0; i < r.size(); i++) {
            std::string type = lowerAscii(r[i]->attr("type")), target = Fb3::normPath(std::string(), r[i]->attr("target"));
            if (type.find("fictionbook3/relationships/book") != std::string::npos) descPath = target;
            else if (type.find("thumbnail") != std::string::npos) coverPath = target;
        }
    }
    std::string bodyPath = "fb3/body.xml";
    std::map<std::string, std::string> descRels = f.rels(descPath);
    for (std::map<std::string, std::string>::iterator it = descRels.begin(); it != descRels.end(); ++it)
        if (endsWithNoCase(it->second, ".xml") && it->second.find("body") != std::string::npos) bodyPath = it->second;
    if (!files.count(bodyPath) || !files.count(descPath)) {
        if (err) *err = "Это не FB3: в архиве нет текста книги (fb3/body.xml)";
        return false;
    }
    f.bodyRels = f.rels(bodyPath);

    MNode desc = parseMarkup(decodeText(files[descPath], NULL), false);
    MNode body = parseMarkup(decodeText(files[bodyPath], NULL), false);
    const MNode* root = findNode(&body, "fb3-body");
    if (!root) {
        if (err) *err = "Это не FB3: в тексте книги нет элемента <fb3-body>";
        return false;
    }

    /* описание -> title-info FB2 */
    std::string title;
    const MNode* dr = findNode(&desc, "fb3-description");
    if (dr) {
        const MNode* tt = dr->child("title");
        const MNode* tm = tt ? tt->child("main") : NULL;
        if (tm) title = collapse(tm->textContent());
    }
    std::string info = "<genre>prose</genre>";
    std::vector<const MNode*> subjects = findNodes(&desc, "subject");
    for (size_t i = 0; i < subjects.size(); i++) {
        if (lowerAscii(subjects[i]->attr("link")) != "author") continue;
        std::string a;
        const char* parts[] = {"first-name", "middle-name", "last-name"};
        for (int k = 0; k < 3; k++) {
            const MNode* p = subjects[i]->child(parts[k]);
            if (p && !collapse(p->textContent()).empty()) a += std::string("<") + parts[k] + ">" + escXml(collapse(p->textContent())) + "</" + parts[k] + ">";
        }
        if (a.empty()) {
            const MNode* tt = subjects[i]->child("title");
            const MNode* tm = tt ? tt->child("main") : NULL;
            if (tm) a = "<last-name>" + escXml(collapse(tm->textContent())) + "</last-name>";
        }
        if (!a.empty()) info += "<author>" + a + "</author>";
    }
    info += "<book-title>" + escXml(title) + "</book-title>";
    if (dr) {
        const MNode* ann = dr->child("annotation");
        if (ann) info += "<annotation>" + f.kids(ann, 0) + "</annotation>";
        const MNode* lang = dr->child("lang");
        if (lang) info += "<lang>" + escXml(collapse(lang->textContent())) + "</lang>";
        const MNode* seq = dr->child("sequence");
        if (seq && !seq->attr("name").empty())
            info += "<sequence name=\"" + escXml(seq->attr("name")) + "\"" + (seq->hasAttr("number") ? " number=\"" + escXml(seq->attr("number")) + "\"" : std::string()) + "/>";
    }
    if (!coverPath.empty()) {
        std::string id = f.addImage(coverPath);
        if (!id.empty()) info += "<coverpage><image l:href=\"#" + escXml(id) + "\"/></coverpage>";
    }

    /* тело: разделы — в основное, <notes> — в тело сносок */
    std::string mainBody, notes;
    for (size_t i = 0; i < root->kids.size(); i++) {
        const MNode* k = root->kids[i].get();
        if (k->text) continue;
        if (k->name == "notes") {
            for (size_t j = 0; j < k->kids.size(); j++) {
                const MNode* nb = k->kids[j].get();
                if (nb->text || nb->name != "notebody") continue;
                std::string label = intToStr((long long)j + 1);
                const MNode* nt = nb->child("title");
                notes += "<section id=\"" + escXml(nb->attr("id")) + "\">";
                if (!nt) notes += "<title><p>" + label + "</p></title>";
                notes += f.kids(nb, 0) + "</section>";
            }
        } else if (k->name == "title" || k->name == "epigraph" || k->name == "section") {
            mainBody += f.node(k, 0);
        } else {
            mainBody += "<section>" + f.node(k, 0) + "</section>";
        }
    }
    std::string fb2 =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\" "
        "xmlns:l=\"http://www.w3.org/1999/xlink\"><description><title-info>" + info + "</title-info></description><body>" + mainBody + "</body>" +
        (notes.empty() ? std::string() : "<body name=\"notes\">" + notes + "</body>") + f.binaries + "</FictionBook>";
    Bytes fb(fb2.begin(), fb2.end());
    return readBook(pathStripExt(fileName) + ".fb2", fb, out, err);
}

/* ================================= LZX ================================== */

struct HuffDec {
    int maxLen;
    int count[17], first[17];
    std::vector<unsigned short> sym, offs;
    void build(const unsigned char* lens, int n) {
        memset(count, 0, sizeof(count));
        for (int i = 0; i < n; i++) if (lens[i] <= 16) count[lens[i]]++;
        count[0] = 0;
        int code = 0, idx = 0;
        offs.assign(17, 0);
        for (int l = 1; l <= 16; l++) {
            code <<= 1;
            first[l] = code;
            offs[(size_t)l] = (unsigned short)idx;
            code += count[l];
            idx += count[l];
        }
        sym.assign((size_t)idx + 1, 0);
        std::vector<unsigned short> next(offs);
        for (int i = 0; i < n; i++) if (lens[i] && lens[i] <= 16) sym[next[lens[i]]++] = (unsigned short)i;
    }
};

class Lzx {
public:
    explicit Lzx(int wbits) : wbits_(wbits), wsize_(1u << wbits), window_(wsize_, 0) {
        slots_ = wbits == 21 ? 50 : (wbits == 20 ? 42 : wbits << 1);
        mainElems_ = 256 + (slots_ << 3);
        int j = 0;
        for (int i = 0; i <= 50; i += 2) {
            extra_[i] = extra_[i + 1] = j;
            if (i != 0 && j < 17) j++;
        }
        unsigned b = 0;
        for (int i = 0; i <= 50; i++) { base_[i] = b; b += 1u << extra_[i]; }
        reset();
    }
    void reset() {
        r0_ = r1_ = r2_ = 1;
        headerRead_ = false;
        blockRemaining_ = 0;
        blockLength_ = 0;
        blockType_ = 0;
        pos_ = 0;
        intelStarted_ = false;
        intelFilesize_ = 0;
        intelCur_ = 0;
        frames_ = 0;
        memset(mainLens_, 0, sizeof(mainLens_));
        memset(lenLens_, 0, sizeof(lenLens_));
    }
    /* один кадр: на входе сжатые байты, на выходе ровно outLen байт */
    bool decode(const unsigned char* in, size_t inLen, unsigned char* out, unsigned outLen);

private:
    int wbits_;
    unsigned wsize_;
    std::vector<unsigned char> window_;
    int slots_, mainElems_;
    int extra_[52];
    unsigned base_[52];
    unsigned r0_, r1_, r2_;
    bool headerRead_, intelStarted_;
    unsigned blockRemaining_, blockLength_;
    int blockType_;
    unsigned pos_;
    int intelFilesize_;
    int intelCur_;
    unsigned frames_;
    unsigned char mainLens_[656 + 8], lenLens_[250 + 8], alignedLens_[8];
    HuffDec mainT_, lenT_, alignedT_, preT_;
    /* биты: 16-битные слова little-endian, старший бит первым */
    const unsigned char* in_;
    size_t inPos_, inLen_;
    unsigned bitBuf_;
    int bitCount_;
    bool bad_;

    void initBits() { bitBuf_ = 0; bitCount_ = 0; }
    void need(int n) {
        while (bitCount_ < n) {
            unsigned w = 0;
            if (inPos_ + 1 < inLen_) w = in_[inPos_] | ((unsigned)in_[inPos_ + 1] << 8);
            else if (inPos_ >= inLen_ + 8) bad_ = true;
            inPos_ += 2;
            bitBuf_ |= w << (16 - bitCount_);
            bitCount_ += 16;
        }
    }
    unsigned bits(int n) {
        if (!n) return 0;
        unsigned v = 0;
        /* больше 16 за раз — по частям, чтобы влезло в 32-битный буфер */
        while (n > 16) { v = (v << 16) | bits(16); n -= 16; }
        need(n);
        v = (v << n) | (bitBuf_ >> (32 - n));
        bitBuf_ <<= n;
        bitCount_ -= n;
        return v;
    }
    int huff(const HuffDec& t) {
        need(16);
        unsigned v = bitBuf_ >> 16;
        int code = 0;
        for (int l = 1; l <= 16; l++) {
            code = (int)(v >> (16 - l));
            int d = code - t.first[l];
            if (d >= 0 && d < t.count[l]) {
                bitBuf_ <<= l;
                bitCount_ -= l;
                return t.sym[(size_t)t.offs[(size_t)l] + (size_t)d];
            }
        }
        bad_ = true;
        return 0;
    }
    bool readLens(unsigned char* lens, int first, int last) {
        unsigned char pl[20];
        for (int i = 0; i < 20; i++) pl[i] = (unsigned char)bits(4);
        preT_.build(pl, 20);
        for (int x = first; x < last;) {
            int z = huff(preT_);
            if (bad_) return false;
            if (z == 17) {
                int y = (int)bits(4) + 4;
                while (y-- && x < last) lens[x++] = 0;
            } else if (z == 18) {
                int y = (int)bits(5) + 20;
                while (y-- && x < last) lens[x++] = 0;
            } else if (z == 19) {
                int y = (int)bits(1) + 4;
                z = huff(preT_);
                if (bad_ || x >= last) return false;
                z = lens[x] - z;
                if (z < 0) z += 17;
                while (y-- && x < last) lens[x++] = (unsigned char)z;
            } else {
                z = lens[x] - z;
                if (z < 0) z += 17;
                lens[x++] = (unsigned char)z;
            }
        }
        return true;
    }
};

bool Lzx::decode(const unsigned char* in, size_t inLen, unsigned char* out, unsigned outLen) {
    in_ = in;
    inLen_ = inLen;
    inPos_ = 0;
    bad_ = false;
    initBits();
    if (!headerRead_) {
        if (bits(1)) {
            unsigned hi = bits(16), lo = bits(16);
            intelFilesize_ = (int)((hi << 16) | lo);
        }
        headerRead_ = true;
    }
    unsigned togo = outLen;
    unsigned mask = wsize_ - 1;
    while (togo > 0) {
        if (blockRemaining_ == 0) {
            if (blockType_ == 3) {
                if (blockLength_ & 1) inPos_++;
                initBits();
            }
            blockType_ = (int)bits(3);
            unsigned i = bits(16), j = bits(8);
            blockRemaining_ = blockLength_ = (i << 8) | j;
            if (blockType_ == 2) {
                for (int k = 0; k < 8; k++) alignedLens_[k] = (unsigned char)bits(3);
                alignedT_.build(alignedLens_, 8);
            }
            if (blockType_ == 1 || blockType_ == 2) {
                if (!readLens(mainLens_, 0, 256) || !readLens(mainLens_, 256, mainElems_)) return false;
                mainT_.build(mainLens_, mainElems_);
                if (mainLens_[0xE8]) intelStarted_ = true;
                if (!readLens(lenLens_, 0, 249)) return false;
                lenT_.build(lenLens_, 249);
            } else if (blockType_ == 3) {
                intelStarted_ = true;
                /* R0..R2 лежат с границы 16 бит */
                need(16);
                if (bitCount_ > 16) inPos_ -= 2;
                initBits();
                if (inPos_ + 12 > inLen_) return false;
                r0_ = in_[inPos_] | (in_[inPos_ + 1] << 8) | (in_[inPos_ + 2] << 16) | ((unsigned)in_[inPos_ + 3] << 24);
                r1_ = in_[inPos_ + 4] | (in_[inPos_ + 5] << 8) | (in_[inPos_ + 6] << 16) | ((unsigned)in_[inPos_ + 7] << 24);
                r2_ = in_[inPos_ + 8] | (in_[inPos_ + 9] << 8) | (in_[inPos_ + 10] << 16) | ((unsigned)in_[inPos_ + 11] << 24);
                inPos_ += 12;
            } else {
                return false;
            }
        }
        unsigned run = blockRemaining_ > togo ? togo : blockRemaining_;
        togo -= run;
        blockRemaining_ -= run;
        pos_ &= mask;
        if (pos_ + run > wsize_) return false;
        int left = (int)run;
        if (blockType_ == 3) {
            if (inPos_ + run > inLen_) return false;
            memcpy(&window_[pos_], in_ + inPos_, run);
            inPos_ += run;
            pos_ += run;
            left = 0;
        }
        while (left > 0) {
            int m = huff(mainT_);
            if (bad_) return false;
            if (m < 256) {
                window_[pos_++] = (unsigned char)m;
                left--;
                continue;
            }
            m -= 256;
            int ml = m & 7;
            if (ml == 7) ml += huff(lenT_);
            ml += 2;
            int mo = m >> 3;
            unsigned off;
            if (mo > 2) {
                int ex = extra_[mo];
                if (blockType_ == 1) {
                    off = base_[mo] - 2 + bits(ex);
                } else {
                    off = base_[mo] - 2;
                    if (ex > 3) { off += bits(ex - 3) << 3; off += (unsigned)huff(alignedT_); }
                    else if (ex == 3) off += (unsigned)huff(alignedT_);
                    else if (ex > 0) off += bits(ex);
                    else off = 1;
                }
                r2_ = r1_; r1_ = r0_; r0_ = off;
            } else if (mo == 0) {
                off = r0_;
            } else if (mo == 1) {
                off = r1_; r1_ = r0_; r0_ = off;
            } else {
                off = r2_; r2_ = r0_; r0_ = off;
            }
            if (bad_ || off == 0 || off > wsize_ || pos_ + (unsigned)ml > wsize_) return false;
            unsigned src = pos_ >= off ? pos_ - off : pos_ + (wsize_ - off);
            left -= ml;
            for (int i = 0; i < ml; i++) {
                window_[pos_++] = window_[src];
                src = (src + 1) & mask;
            }
        }
        if (left < 0) {
            /* совпадение перешло границу блока */
            if ((unsigned)(-left) > blockRemaining_) return false;
            blockRemaining_ -= (unsigned)(-left);
        }
    }
    if (bad_) return false;
    unsigned start = pos_ == 0 ? wsize_ - outLen : pos_ - outLen;
    if (pos_ != 0 && pos_ < outLen) return false;
    memcpy(out, &window_[start], outLen);
    /* обратное преобразование переходов x86 (E8), как в LZXdecompress */
    if (outLen > 10 && frames_++ < 32768 && intelFilesize_ != 0 && intelStarted_) {
        int cur = intelCur_;
        unsigned char* p = out;
        unsigned char* end = out + outLen - 10;
        while (p < end) {
            if (*p++ != 0xE8) { cur++; continue; }
            int abs = p[0] | (p[1] << 8) | (p[2] << 16) | ((int)p[3] << 24);
            if (abs >= -cur && abs < intelFilesize_) {
                int rel = abs >= 0 ? abs - cur : abs + intelFilesize_;
                p[0] = (unsigned char)rel; p[1] = (unsigned char)(rel >> 8);
                p[2] = (unsigned char)(rel >> 16); p[3] = (unsigned char)(rel >> 24);
            }
            p += 4;
            cur += 5;
        }
        intelCur_ += (int)outLen;
    } else if (frames_ < 32768) {
        intelCur_ += (int)outLen;
    }
    return true;
}

/* ================================= CHM ================================== */

struct ChmFile { std::string name; int section; unsigned long long start, len; };

unsigned rd32(const unsigned char* d, size_t n, size_t p) {
    return p + 4 > n ? 0 : d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((unsigned)d[p + 3] << 24);
}
unsigned long long rd64(const unsigned char* d, size_t n, size_t p) {
    return p + 8 > n ? 0 : (unsigned long long)rd32(d, n, p) | ((unsigned long long)rd32(d, n, p + 4) << 32);
}

bool encInt(const unsigned char* d, size_t end, size_t& p, unsigned long long& v) {
    v = 0;
    for (int i = 0; i < 10; i++) {
        if (p >= end) return false;
        unsigned char c = d[p++];
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) return true;
    }
    return false;
}

struct TocItem { int level; std::string name, local; };

std::string attrValue(const std::string& tag, const char* name) {
    std::string low = lowerAscii(tag);
    size_t p = low.find(std::string(name) + "=");
    if (p == std::string::npos) return std::string();
    p += strlen(name) + 1;
    if (p >= tag.size()) return std::string();
    char q = tag[p];
    size_t e;
    if (q == '"' || q == '\'') { p++; e = tag.find(q, p); }
    else e = tag.find_first_of(" \t\r\n>", p);
    if (e == std::string::npos) e = tag.size();
    return tag.substr(p, e - p);
}

/* .hhc: вложенные <UL>, в них <OBJECT type="text/sitemap"> с <param name="Name"|"Local"> */
void parseHhc(const std::string& s, std::vector<TocItem>& out) {
    int depth = 0;
    std::string name, local;
    bool inObj = false;
    size_t p = 0;
    while ((p = s.find('<', p)) != std::string::npos) {
        size_t e = s.find('>', p);
        if (e == std::string::npos) break;
        std::string tag = s.substr(p + 1, e - p - 1);
        std::string low = lowerAscii(tag);
        p = e + 1;
        if (startsWith(low, "ul")) depth++;
        else if (startsWith(low, "/ul")) { if (depth > 0) depth--; }
        else if (startsWith(low, "object")) { inObj = low.find("text/sitemap") != std::string::npos; name.clear(); local.clear(); }
        else if (startsWith(low, "/object")) {
            if (inObj && !name.empty()) {
                TocItem t;
                t.level = std::max(0, depth - 1);
                t.name = collapse(unescapeEntities(name));
                t.local = unescapeEntities(local);
                out.push_back(t);
            }
            inObj = false;
        } else if (inObj && startsWith(low, "param")) {
            std::string n = lowerAscii(attrValue(tag, "name")), v = attrValue(tag, "value");
            if (n == "name" && name.empty()) name = v;
            else if (n == "local" && local.empty()) local = v;
        }
    }
}

/* 1.9: справка Windows и всё, что собрано Sandcastle, держит в каждой странице обвязку: скрытые
   подписи и иконки, меню языков, кнопку «Свернуть все», повтор заголовка. Браузер их не показывает
   (display:none и presentation.css), а CSS мы не применяем — в книгу попадал мусор на каждой странице.
   Убираем элементы с display:none в самом теге и известные блоки этой обвязки. */
std::string stripChmChrome(const std::string& h) {
    static const char* const kIds[] = {"devlangsMenu", "topTable", "bottomTable", "gradientTable"};
    struct Tag {
        static bool isVoid(const std::string& n) {
            return n == "img" || n == "input" || n == "br" || n == "hr" || n == "meta" || n == "link" || n == "param" || n == "area";
        }
    };
    std::string out;
    out.reserve(h.size());
    size_t p = 0;
    while (p < h.size()) {
        size_t lt = h.find('<', p);
        if (lt == std::string::npos) { out.append(h, p, std::string::npos); break; }
        out.append(h, p, lt - p);
        size_t gt = h.find('>', lt);
        if (gt == std::string::npos) { out.append(h, lt, std::string::npos); break; }
        std::string tag = h.substr(lt + 1, gt - lt - 1);
        size_t ne = tag.find_first_of(" \t\r\n/");
        std::string name = lowerAscii(tag.substr(0, ne));
        p = gt + 1;
        bool drop = false;
        if (!name.empty() && isalpha((unsigned char)name[0])) {
            std::string style = lowerAscii(attrValue(tag, " style"));
            style = replaceAll(style, " ", "");
            if (style.find("display:none") != std::string::npos) drop = true;
            if (name == "input" && lowerAscii(attrValue(tag, " type")) == "hidden") drop = true;
            std::string id = attrValue(tag, " id");
            for (size_t i = 0; !drop && i < sizeof(kIds) / sizeof(kIds[0]); i++) drop = id == kIds[i];
            if (attrValue(tag, " class") == "footerLine") drop = true; /* полоска-картинка над подвалом */
        }
        if (!drop) { out.append(h, lt, gt + 1 - lt); continue; }
        if (Tag::isVoid(name) || tag[tag.size() - 1] == '/') continue;
        /* пропуск до парного закрывающего тега; без пары удаляется только сам открывающий */
        int depth = 1;
        size_t q = p;
        const std::string open = "<" + name, close = "</" + name;
        while (depth > 0) {
            size_t t = h.find('<', q);
            if (t == std::string::npos) break;
            std::string head = lowerAscii(h.substr(t, name.size() + 2));
            char after = head.size() > open.size() ? head[open.size()] : ' ';
            if (head.compare(0, close.size(), close) == 0) depth--;
            else if (head.compare(0, open.size(), open) == 0 && !isalnum((unsigned char)after)) {
                size_t te = h.find('>', t);
                if (te != std::string::npos && h[te - 1] != '/') depth++;
            }
            q = t + 1;
        }
        if (depth == 0) {
            size_t te = h.find('>', q);
            p = te == std::string::npos ? h.size() : te + 1;
        }
    }
    return out;
}

/* кодовая страница текста: из <meta charset>, иначе из языка CHM */
std::string toUtf8Html(const Bytes& b, unsigned cp) {
    std::string s(b.begin(), b.end());
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) return s.substr(3);
    std::string low = lowerAscii(s.substr(0, 4096));
    size_t c = low.find("charset=");
    if (c != std::string::npos) {
        size_t q = c + 8;
        if (q < low.size() && (low[q] == '"' || low[q] == '\'')) q++;
        size_t e = low.find_first_of("\"'; >\t\r\n", q);
        std::string cs = low.substr(q, e == std::string::npos ? std::string::npos : e - q);
        if (cs == "utf-8" || cs == "utf8") return s;
        if (startsWith(cs, "windows-")) cp = (unsigned)atoi(cs.c_str() + 8);
        else if (startsWith(cs, "koi8-r")) cp = 20866;
        else if (startsWith(cs, "koi8-u")) cp = 21866;
        else if (startsWith(cs, "iso-8859-")) cp = 28590 + (unsigned)atoi(cs.c_str() + 9);
        else if (cs == "shift_jis") cp = 932;
        else if (cs == "gb2312") cp = 936;
        else if (cs == "big5") cp = 950;
        else if (cs == "us-ascii") cp = 1252;
    }
    return cpToUtf8(s, cp ? cp : 1252);
}

bool readChmImpl(const Bytes& data, const std::string& fileName, Book& out, std::string* err) {
    const unsigned char* d = data.empty() ? NULL : &data[0];
    size_t n = data.size();
    if (n < 0x60 || memcmp(d, "ITSF", 4) != 0) {
        if (err) *err = "Это не CHM: неверная подпись файла";
        return false;
    }
    unsigned ver = rd32(d, n, 4);
    unsigned long long dirOff = rd64(d, n, 0x48), dirLen = rd64(d, n, 0x50);
    unsigned long long dataOff = ver >= 3 ? rd64(d, n, 0x58) : dirOff + dirLen;
    if (dirOff + 0x54 > n || memcmp(d + dirOff, "ITSP", 4) != 0) {
        if (err) *err = "CHM повреждён: нет каталога файлов";
        return false;
    }
    unsigned itspHdr = rd32(d, n, (size_t)dirOff + 8), blockLen = rd32(d, n, (size_t)dirOff + 0x10);
    if (blockLen < 32 || blockLen > 1u << 24 || itspHdr > 4096) {
        if (err) *err = "CHM повреждён: неверный каталог файлов";
        return false;
    }
    std::map<std::string, ChmFile> files;
    for (unsigned long long off = dirOff + itspHdr; off + blockLen <= n && off + blockLen <= dirOff + dirLen; off += blockLen) {
        size_t c = (size_t)off;
        if (memcmp(d + c, "PMGL", 4) != 0) continue;
        unsigned freeSpace = rd32(d, n, c + 4);
        if (freeSpace > blockLen) continue;
        size_t end = c + blockLen - freeSpace, p = c + 20;
        while (p < end) {
            unsigned long long nl, sec, st, ln;
            if (!encInt(d, end, p, nl) || nl > 4096 || p + nl > end) break;
            std::string name((const char*)d + p, (size_t)nl);
            p += (size_t)nl;
            if (!encInt(d, end, p, sec) || !encInt(d, end, p, st) || !encInt(d, end, p, ln)) break;
            ChmFile f;
            f.name = name;
            f.section = (int)sec;
            f.start = st;
            f.len = ln;
            files[name] = f;
        }
    }
    if (files.empty()) {
        if (err) *err = "CHM повреждён: каталог файлов пуст";
        return false;
    }
    /* несжатые файлы (раздел 0) */
    struct Sec0 {
        static bool get(const std::map<std::string, ChmFile>& fs, const unsigned char* dd, size_t nn, unsigned long long base,
                        const char* name, Bytes& o) {
            std::map<std::string, ChmFile>::const_iterator it = fs.find(name);
            if (it == fs.end() || it->second.section != 0 || base + it->second.start + it->second.len > nn) return false;
            o.assign(dd + base + it->second.start, dd + base + it->second.start + it->second.len);
            return true;
        }
    };
    Bytes ctrl, reset, comp;
    const char* kCtrl = "::DataSpace/Storage/MSCompressed/ControlData";
    const char* kContent = "::DataSpace/Storage/MSCompressed/Content";
    const char* kReset = "::DataSpace/Storage/MSCompressed/Transform/{7FC28940-9D31-11D0-9B27-00A0C91E9C7C}/InstanceData/ResetTable";
    Bytes unpacked;
    if (Sec0::get(files, d, n, dataOff, kCtrl, ctrl) && Sec0::get(files, d, n, dataOff, kReset, reset) &&
        Sec0::get(files, d, n, dataOff, kContent, comp) && ctrl.size() >= 24 && reset.size() >= 40) {
        const unsigned char* c = &ctrl[0];
        const unsigned char* r = &reset[0];
        if (memcmp(c + 4, "LZXC", 4) != 0) {
            if (err) *err = "CHM сжат неизвестным способом";
            return false;
        }
        unsigned cver = rd32(c, ctrl.size(), 8), resetInt = rd32(c, ctrl.size(), 12), winSize = rd32(c, ctrl.size(), 16), perReset = rd32(c, ctrl.size(), 20);
        if (cver == 2) {
            if (resetInt > 0x10000 || winSize > 0x10000) { if (err) *err = "CHM повреждён: неверные параметры сжатия"; return false; }
            resetInt *= 0x8000;
            winSize *= 0x8000;
        }
        int wbits = 0;
        while (wbits < 26 && (1u << wbits) < winSize) wbits++;
        if (wbits < 15 || wbits > 21 || (1u << wbits) != winSize || !perReset) {
            if (err) *err = "CHM повреждён: неверный размер окна сжатия";
            return false;
        }
        unsigned resetBlocks = resetInt / (winSize / 2) * perReset;
        if (!resetBlocks) resetBlocks = 1;
        unsigned blocks = rd32(r, reset.size(), 4), tableOff = rd32(r, reset.size(), 12);
        unsigned long long uLen = rd64(r, reset.size(), 16), bLen = rd64(r, reset.size(), 32);
        if (bLen != 0x8000 || uLen > 512u * 1024 * 1024 || tableOff > reset.size() || (unsigned long long)blocks * 8 > reset.size() - tableOff) {
            if (err) *err = "CHM повреждён: неверная таблица сжатия";
            return false;
        }
        unpacked.resize((size_t)uLen);
        Lzx lzx(wbits);
        for (unsigned i = 0; i < blocks; i++) {
            unsigned long long a = rd64(r, reset.size(), (size_t)tableOff + (size_t)i * 8);
            unsigned long long z = i + 1 < blocks ? rd64(r, reset.size(), (size_t)tableOff + ((size_t)i + 1) * 8) : comp.size();
            unsigned long long at = (unsigned long long)i * 0x8000;
            if (at >= uLen) break;
            unsigned ol = (unsigned)std::min<unsigned long long>(0x8000, uLen - at);
            if (a > z || z > comp.size()) break;
            if (i % resetBlocks == 0) lzx.reset();
            if (!lzx.decode(comp.empty() ? NULL : &comp[(size_t)a], (size_t)(z - a), &unpacked[(size_t)at], ol)) {
                /* повреждение: всё до этого кадра ещё можно прочитать */
                unpacked.resize((size_t)at);
                break;
            }
        }
    }
    struct Get {
        static bool file(const ChmFile& f, const unsigned char* dd, size_t nn, unsigned long long base, const Bytes& up, Bytes& o) {
            if (f.section == 0) {
                if (base + f.start + f.len > nn) return false;
                o.assign(dd + base + f.start, dd + base + f.start + f.len);
                return true;
            }
            if (f.start + f.len > up.size()) return false;
            o.assign(up.begin() + (long)f.start, up.begin() + (long)(f.start + f.len));
            return true;
        }
    };
    /* #SYSTEM: файл оглавления, стартовая страница, заголовок, язык */
    std::string hhcName, title, defaultTopic;
    unsigned lcid = 0;
    std::map<std::string, ChmFile>::iterator sys = files.find("/#SYSTEM");
    Bytes sb;
    if (sys != files.end() && Get::file(sys->second, d, n, dataOff, unpacked, sb) && sb.size() > 4) {
        size_t p = 4;
        while (p + 4 <= sb.size()) {
            unsigned code = sb[p] | (sb[p + 1] << 8), ln = sb[p + 2] | (sb[p + 3] << 8);
            p += 4;
            if (p + ln > sb.size()) break;
            std::string v((const char*)(sb.empty() ? NULL : &sb[0]) + p, ln);
            size_t z = v.find('\0');
            std::string sv = z == std::string::npos ? v : v.substr(0, z);
            if (code == 0) hhcName = sv;
            else if (code == 2) defaultTopic = sv;
            else if (code == 3) title = sv;
            else if (code == 4 && ln >= 4) lcid = (unsigned)(unsigned char)v[0] | ((unsigned)(unsigned char)v[1] << 8);
            p += ln;
        }
    }
    unsigned cp = 0;
    if (lcid) {
        char buf[16];
        if (GetLocaleInfoA(lcid, LOCALE_IDEFAULTANSICODEPAGE, buf, sizeof(buf))) cp = (unsigned)atoi(buf);
    }
    if (!cp) cp = 1252;

    /* все файлы книги: HTML в UTF-8, картинки как есть; служебные (# и $ в начале) не нужны */
    std::vector<ZipEntry> z;
    std::vector<std::string> htmlNames;
    std::map<std::string, std::string> htmlByLow;
    std::vector<std::pair<std::string, std::string> > images;
    std::string hhcText;
    if (!hhcName.empty() && hhcName[0] != '/') hhcName = "/" + hhcName;
    for (std::map<std::string, ChmFile>::iterator it = files.begin(); it != files.end(); ++it) {
        const std::string& nm = it->first;
        if (nm.size() < 2 || nm[0] != '/' || nm[1] == '#' || nm[1] == '$' || nm[nm.size() - 1] == '/') continue;
        std::string ext = lowerAscii(pathExt(nm));
        bool isHtml = ext == "htm" || ext == "html" || ext == "xhtml";
        bool isImg = ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "bmp" || ext == "svg";
        bool isHhc = ext == "hhc";
        if (!isHtml && !isImg && !isHhc) continue;
        Bytes fb;
        if (!Get::file(it->second, d, n, dataOff, unpacked, fb)) continue;
        std::string rel = nm.substr(1);
        if (isHhc) {
            if (hhcText.empty() || lowerAscii(nm) == lowerAscii(hhcName)) hhcText = toUtf8Html(fb, cp);
            continue;
        }
        if (isHtml) {
            std::string h = toUtf8Html(fb, cp);
            /* кодировка в разметке больше не та: текст уже в UTF-8 */
            std::string low = lowerAscii(h.substr(0, 4096));
            size_t ci = low.find("charset=");
            if (ci != std::string::npos) {
                size_t e = low.find_first_of("\"'; >\t\r\n", ci + 9);
                if (e != std::string::npos) h.replace(ci, e - ci, "charset=utf-8");
            }
            h = stripChmChrome(h);
            z.push_back(ZipEntry(rel, Bytes(h.begin(), h.end()), false));
            htmlNames.push_back(rel);
            htmlByLow[lowerAscii(rel)] = rel;
        } else {
            z.push_back(ZipEntry(rel, fb, false));
            images.push_back(std::make_pair(rel, mimeFromExt(ext)));
        }
    }
    if (htmlNames.empty()) {
        if (err) *err = "В CHM нет страниц с текстом";
        return false;
    }
    /* порядок страниц и оглавление — из .hhc; страницы вне оглавления идут в конец */
    std::vector<TocItem> toc;
    parseHhc(hhcText, toc);
    std::vector<std::string> spine;
    std::set<std::string> inSpine;
    struct Clean {
        static std::string key(std::string s) {
            size_t h = s.find('#');
            if (h != std::string::npos) s = s.substr(0, h);
            s = replaceAll(s, "\\", "/");
            size_t col = s.find("::/");
            if (col != std::string::npos) s = s.substr(col + 3);
            while (!s.empty() && s[0] == '/') s = s.substr(1);
            return lowerAscii(s);
        }
    };
    for (size_t i = 0; i < toc.size(); i++) {
        std::string k = Clean::key(toc[i].local);
        if (htmlByLow.count(k) && !inSpine.count(k)) { inSpine.insert(k); spine.push_back(htmlByLow[k]); }
    }
    if (spine.empty() && !defaultTopic.empty()) {
        std::string k = Clean::key(defaultTopic);
        if (htmlByLow.count(k)) { inSpine.insert(k); spine.push_back(htmlByLow[k]); }
    }
    for (size_t i = 0; i < htmlNames.size(); i++) {
        std::string k = lowerAscii(htmlNames[i]);
        if (!inSpine.count(k)) { inSpine.insert(k); spine.push_back(htmlNames[i]); }
    }
    if (title.empty()) title = pathStripExt(pathBase(fileName));
    else title = cpToUtf8(title, cp);

    /* синтетический OPF и NCX */
    std::string man, sp;
    for (size_t i = 0; i < spine.size(); i++) {
        std::string id = "p" + intToStr((long long)i);
        man += "<item id=\"" + id + "\" href=\"" + escXml(spine[i]) + "\" media-type=\"application/xhtml+xml\"/>";
        sp += "<itemref idref=\"" + id + "\"/>";
    }
    for (size_t i = 0; i < images.size(); i++)
        man += "<item id=\"i" + intToStr((long long)i) + "\" href=\"" + escXml(images[i].first) + "\" media-type=\"" + escXml(images[i].second) + "\"/>";
    man += "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>";
    std::string opf = "<?xml version=\"1.0\" encoding=\"utf-8\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" +
                      escXml(title) + "</dc:title></metadata><manifest>" + man + "</manifest><spine toc=\"ncx\">" + sp + "</spine></package>";
    std::string ncx = "<?xml version=\"1.0\" encoding=\"utf-8\"?><ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><navMap>";
    int cur = -1, np = 1;
    for (size_t i = 0; i < toc.size(); i++) {
        std::string k = Clean::key(toc[i].local);
        if (!htmlByLow.count(k)) continue; /* узел без своей страницы: заголовок ветки */
        std::string href = htmlByLow[k];
        size_t h = toc[i].local.find('#');
        if (h != std::string::npos) href += toc[i].local.substr(h);
        int lv = toc[i].level;
        if (lv > cur + 1) lv = cur + 1;
        while (cur >= lv) { ncx += "</navPoint>"; cur--; }
        ncx += "<navPoint id=\"n" + intToStr(np) + "\" playOrder=\"" + intToStr(np) + "\"><navLabel><text>" + escXml(toc[i].name) +
               "</text></navLabel><content src=\"" + escXml(href) + "\"/>";
        np++;
        cur = lv;
    }
    while (cur >= 0) { ncx += "</navPoint>"; cur--; }
    ncx += "</navMap></ncx>";
    std::string cont = "<?xml version=\"1.0\"?><container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    z.push_back(ZipEntry("META-INF/container.xml", Bytes(cont.begin(), cont.end()), false));
    z.push_back(ZipEntry("content.opf", Bytes(opf.begin(), opf.end()), false));
    z.push_back(ZipEntry("toc.ncx", Bytes(ncx.begin(), ncx.end()), false));
    Bytes zipData = zipWrite(z);
    if (!readBook(pathStripExt(fileName) + ".epub", zipData, out, err)) return false;
    out.fileName = fileName;
    return true;
}

} /* namespace */

bool readFB3(const Bytes& data, const std::string& fileName, Book& out, std::string* err) {
    return readFb3Impl(data, fileName, out, err);
}

bool readCHM(const Bytes& data, const std::string& fileName, Book& out, std::string* err) {
    return readChmImpl(data, fileName, out, err);
}
