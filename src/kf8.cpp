/* Сжатие HUFF/CDIC, индексы MOBI и книги KF8 (AZW3) (1.9).
   Раскладка индексов и сборка файлов из скелетов и фрагментов — по описанию формата в Calibre
   (ebooks/mobi/reader/index.py и mobi8.py). KF8 собирается в EPUB в памяти и читается читателем EPUB. */
#include "kf8.h"
#include "convert.h"
#include "zip.h"
#include <algorithm>

/* ---------------------------------- индекс -------------------------------- */

static size_t decint(const unsigned char* p, size_t n, unsigned& val) {
    val = 0;
    for (size_t i = 0; i < n && i < 5; i++) {
        val = (val << 7) | (p[i] & 0x7F);
        if (p[i] & 0x80) return i + 1;
    }
    return 0;
}

static unsigned beAt(const Bytes& r, size_t p, int n) {
    if (p > r.size() || (size_t)n > r.size() - p) return 0;
    unsigned v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | r[p + i];
    return v;
}

static Bytes recordOf(const Bytes& b, const std::vector<unsigned>& o, size_t i) {
    if (i + 1 >= o.size() || o[i] >= o[i + 1] || o[i + 1] > b.size()) return Bytes();
    return Bytes(b.begin() + o[i], b.begin() + o[i + 1]);
}

bool mobiReadIndex(const Bytes& b, const std::vector<unsigned>& offsets, unsigned indx,
                   std::vector<MobiIndexEntry>& out, std::map<unsigned, std::string>& cncx) {
    if (indx == 0 || indx + 1 >= offsets.size()) return false;
    Bytes prim = recordOf(b, offsets, indx);
    if (prim.size() < 56 || memcmp(&prim[0], "INDX", 4) != 0) return false;
    unsigned indexRecs = beAt(prim, 24, 4), ncncx = beAt(prim, 52, 4);
    if (indexRecs > 1000 || ncncx > 1000) return false;

    /* TAGX: сразу за заголовком; на всякий случай ищем по сигнатуре */
    size_t tx = beAt(prim, 4, 4);
    if (tx + 12 > prim.size() || memcmp(&prim[tx], "TAGX", 4) != 0) {
        tx = std::string::npos;
        for (size_t i = 0; i + 12 <= prim.size(); i++)
            if (memcmp(&prim[i], "TAGX", 4) == 0) { tx = i; break; }
        if (tx == std::string::npos) return false;
    }
    unsigned tagxLen = beAt(prim, tx + 4, 4), cbc = beAt(prim, tx + 8, 4);
    if (tagxLen < 12 || tx + tagxLen > prim.size() || cbc == 0 || cbc > 8) return false;
    struct Tag { unsigned tag, perEntry, mask, eof; };
    std::vector<Tag> tags;
    for (size_t p = tx + 12; p + 4 <= tx + tagxLen; p += 4) {
        Tag t = {prim[p], prim[p + 1], prim[p + 2], prim[p + 3]};
        tags.push_back(t);
    }

    /* строки CNCX: длина (число переменной длины) + UTF-8; смещение = позиция + 0x10000 * номер записи */
    for (unsigned k = 0; k < ncncx; k++) {
        Bytes c = recordOf(b, offsets, (size_t)indx + indexRecs + 1 + k);
        size_t p = 0;
        while (p < c.size()) {
            unsigned len = 0;
            size_t used = decint(&c[p], c.size() - p, len);
            if (!used || len > c.size() - p - used) break;
            if (len) cncx[(unsigned)p + 0x10000u * k] = std::string((const char*)&c[p + used], len);
            p += used + len;
        }
    }

    for (unsigned r = 1; r <= indexRecs; r++) {
        Bytes d = recordOf(b, offsets, (size_t)indx + r);
        if (d.size() < 28 || memcmp(&d[0], "INDX", 4) != 0) continue;
        unsigned idxt = beAt(d, 20, 4), count = beAt(d, 24, 4);
        if (idxt + 4 > d.size() || count > 65535 || (size_t)idxt + 4 + 2 * (size_t)count > d.size()) continue;
        for (unsigned j = 0; j < count; j++) {
            size_t s = beAt(d, idxt + 4 + 2 * j, 2);
            size_t e = j + 1 < count ? beAt(d, idxt + 4 + 2 * (j + 1), 2) : idxt;
            if (s >= e || e > d.size()) continue;
            size_t p = s;
            size_t keyLen = d[p];
            if (p + 1 + keyLen > e) continue;
            MobiIndexEntry entry;
            entry.key.assign((const char*)&d[p + 1], keyLen);
            p += 1 + keyLen;
            if (p + cbc > e) continue;
            std::vector<unsigned> cb(d.begin() + p, d.begin() + p + cbc);
            p += cbc;
            /* какие теги есть в записи и сколько у них значений */
            struct PTag { unsigned tag, count, bytes, perEntry; };
            std::vector<PTag> ptags;
            size_t ci = 0;
            for (size_t t = 0; t < tags.size(); t++) {
                if (tags[t].eof) { ci++; continue; }
                if (ci >= cb.size() || !tags[t].mask) continue;
                unsigned value = cb[ci] & tags[t].mask;
                if (!value) continue;
                PTag pt = {tags[t].tag, 0, 0, tags[t].perEntry};
                unsigned bits = 0;
                for (unsigned m = tags[t].mask; m; m >>= 1) bits += m & 1;
                if (value == tags[t].mask) {
                    if (bits > 1) {
                        unsigned v = 0;
                        size_t used = p < e ? decint(&d[p], e - p, v) : 0;
                        if (!used) break;
                        p += used;
                        pt.bytes = v;
                    } else {
                        pt.count = 1;
                    }
                } else {
                    unsigned m = tags[t].mask;
                    while (!(m & 1)) { m >>= 1; value >>= 1; }
                    pt.count = value;
                }
                ptags.push_back(pt);
            }
            bool broken = false;
            for (size_t t = 0; t < ptags.size() && !broken; t++) {
                std::vector<unsigned>& vals = entry.tags[ptags[t].tag];
                if (ptags[t].count) {
                    for (unsigned k = 0; k < ptags[t].count * ptags[t].perEntry && k < 64; k++) {
                        unsigned v = 0;
                        size_t used = p < e ? decint(&d[p], e - p, v) : 0;
                        if (!used) { broken = true; break; }
                        p += used;
                        vals.push_back(v);
                    }
                } else {
                    size_t consumed = 0;
                    while (consumed < ptags[t].bytes) {
                        unsigned v = 0;
                        size_t used = p < e ? decint(&d[p], e - p, v) : 0;
                        if (!used) { broken = true; break; }
                        p += used;
                        consumed += used;
                        vals.push_back(v);
                    }
                }
            }
            if (!broken) out.push_back(entry);
        }
    }
    return true;
}

/* ------------------------------- HUFF/CDIC -------------------------------- */

bool HuffCdic::load(const Bytes& b, const std::vector<unsigned>& offsets, unsigned first, unsigned count) {
    if (!count || first + 1 >= offsets.size()) return false;
    Bytes h = recordOf(b, offsets, first);
    if (h.size() < 24 || memcmp(&h[0], "HUFF", 4) != 0) return false;
    unsigned off1 = beAt(h, 8, 4), off2 = beAt(h, 12, 4);
    /* 1.9: вторая таблица — 32 пары по 4 байта, то есть 256 байт. Проверка на 512 отвергала
       файлы, где HUFF заканчивается сразу за таблицей: нашёл образец tools\make_huff_sample.js,
       который Calibre читал, а Sharkonv называл повреждённым */
    if ((size_t)off1 + 1024 > h.size() || (size_t)off2 + 256 > h.size()) return false;
    for (int i = 0; i < 256; i++) {
        unsigned v = beAt(h, off1 + (size_t)i * 4, 4);
        unsigned len = v & 0x1F;
        if (!len) return false;
        codeLen_[i] = (unsigned char)len;
        term_[i] = (v & 0x80) != 0;
        if (len <= 8 && !term_[i]) return false;
        maxCode_[i] = (((unsigned long long)(v >> 8) + 1) << (32 - len)) - 1;
    }
    min_[0] = 0;
    max_[0] = 0xFFFFFFFFull; /* длина 0 не встречается */
    for (int l = 1; l <= 32; l++) {
        unsigned lo = beAt(h, off2 + (size_t)(l - 1) * 8, 4), hi = beAt(h, off2 + (size_t)(l - 1) * 8 + 4, 4);
        min_[l] = (unsigned long long)lo << (32 - l);
        max_[l] = (((unsigned long long)hi + 1) << (32 - l)) - 1;
    }
    for (unsigned k = 1; k < count; k++) {
        Bytes c = recordOf(b, offsets, (size_t)first + k);
        if (c.size() < 16 || memcmp(&c[0], "CDIC", 4) != 0) return false;
        unsigned phrases = beAt(c, 8, 4), bits = beAt(c, 12, 4);
        if (bits > 16 || phrases > 4000000 || phrases < phrases_.size()) return false;
        size_t n = std::min<size_t>((size_t)1 << bits, phrases - phrases_.size());
        for (size_t i = 0; i < n; i++) {
            size_t po = beAt(c, 16 + i * 2, 2);
            if (16 + po + 2 > c.size()) return false;
            unsigned bl = beAt(c, 16 + po, 2);
            size_t ln = bl & 0x7FFF;
            if (16 + po + 2 + ln > c.size()) return false;
            Phrase ph;
            ph.data.assign((const char*)&c[16 + po + 2], ln);
            ph.done = (bl & 0x8000) != 0;
            ph.busy = false;
            phrases_.push_back(ph);
        }
    }
    return !phrases_.empty();
}

bool HuffCdic::unpack(const unsigned char* d, size_t n, std::string& out, int depth) {
    if (depth > 32 || out.size() > 1u << 30) return false;
    long long bitsLeft = (long long)n * 8;
    size_t pos = 0;
    int have = 32;
    struct Q {
        static unsigned long long at(const unsigned char* dd, size_t nn, size_t p) {
            unsigned long long x = 0;
            for (int i = 0; i < 8; i++) x = (x << 8) | (p + i < nn ? dd[p + i] : 0u);
            return x;
        }
    };
    unsigned long long x = Q::at(d, n, 0);
    for (;;) {
        if (have <= 0) {
            pos += 4;
            x = Q::at(d, n, pos);
            have += 32;
        }
        unsigned long long code = (x >> have) & 0xFFFFFFFFull;
        unsigned top = (unsigned)(code >> 24);
        int len = codeLen_[top];
        unsigned long long maxc = maxCode_[top];
        if (!term_[top]) {
            while (len < 33 && code < min_[len]) len++;
            if (len > 32) return false;
            maxc = max_[len];
        }
        have -= len;
        bitsLeft -= len;
        if (bitsLeft < 0) return true;
        unsigned long long r = (maxc - code) >> (32 - len);
        if (r >= phrases_.size()) return false;
        Phrase& ph = phrases_[(size_t)r];
        if (!ph.done) {
            /* фраза сама сжата: распаковка один раз, циклы в испорченном файле отсекаются */
            if (ph.busy) return false;
            ph.busy = true;
            std::string res;
            if (!unpack((const unsigned char*)ph.data.data(), ph.data.size(), res, depth + 1)) return false;
            ph.data.swap(res);
            ph.done = true;
            ph.busy = false;
        }
        out += ph.data;
    }
}

/* ---------------------------------- KF8 ----------------------------------- */

static int base32Value(const std::string& s) {
    long long v = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'V' ? c - 'A' + 10 : (c >= 'a' && c <= 'v' ? c - 'a' + 10 : -1));
        if (d < 0) return -1;
        v = v * 32 + d;
        if (v > 0x7FFFFFFF) return -1;
    }
    return (int)v;
}

static const char* sniffImage(const Bytes& r) {
    if (r.size() >= 4 && r[0] == 0x89 && r[1] == 'P') return "png";
    if (r.size() >= 3 && r[0] == 0xFF && r[1] == 0xD8) return "jpg";
    if (r.size() >= 4 && r[0] == 'G' && r[1] == 'I' && r[2] == 'F') return "gif";
    if (r.size() >= 2 && r[0] == 'B' && r[1] == 'M') return "bmp";
    return NULL;
}

bool readKF8Body(const Bytes& b, const std::vector<unsigned>& offsets, const std::string& flow, unsigned skelIdx,
                 unsigned fragIdx, unsigned ncxIdx, unsigned guideIdx, unsigned firstImage, long coverOffset, const std::string& fileName,
                 Book& book, std::string* err) {
    std::vector<MobiIndexEntry> skel, frag, ncxE;
    std::map<unsigned, std::string> fragCncx, ncxCncx, skelCncx;
    if (!mobiReadIndex(b, offsets, skelIdx, skel, skelCncx) || !mobiReadIndex(b, offsets, fragIdx, frag, fragCncx) || skel.empty()) {
        if (err) *err = "AZW3 повреждён: нет таблицы частей книги";
        return false;
    }
    if (ncxIdx != 0xFFFFFFFFu) mobiReadIndex(b, offsets, ncxIdx, ncxE, ncxCncx);

    struct Frag { size_t insertPos, rawStart, len; };
    std::vector<Frag> frags(frag.size());
    std::map<int, std::set<unsigned> > targets; /* фрагмент -> смещения, на которые есть ссылки */
    /* ссылки kindle:pos:fid:XXXX:off:YYYYYYYYYY и закладки оглавления */
    {
        size_t p = 0;
        while ((p = flow.find("kindle:pos:fid:", p)) != std::string::npos) {
            int fid = base32Value(flow.substr(p + 15, 4));
            int off = flow.compare(p + 19, 5, ":off:") == 0 ? base32Value(flow.substr(p + 24, 10)) : -1;
            if (fid >= 0 && off >= 0) targets[fid].insert((unsigned)off);
            p += 15;
        }
        for (size_t i = 0; i < ncxE.size(); i++) {
            std::map<unsigned, std::vector<unsigned> >::iterator t = ncxE[i].tags.find(6);
            if (t != ncxE[i].tags.end() && t->second.size() >= 2) targets[(int)t->second[0]].insert(t->second[1]);
        }
    }
    struct Mark {
        static std::string id(int fid, unsigned off) { return "kf" + intToStr(fid) + "_" + intToStr((long long)off); }
    };

    /* сборка файлов: скелет + вставленные в него фрагменты */
    std::vector<std::string> names;
    std::vector<ZipEntry> z;
    std::map<int, std::string> fileOfFrag;
    std::map<std::string, std::string> alias; /* наш якорь -> id, который уже был на теге */
    size_t fi = 0;
    for (size_t s = 0; s < skel.size(); s++) {
        std::map<unsigned, std::vector<unsigned> >& t = skel[s].tags;
        if (!t.count(1) || !t.count(6) || t[1].empty() || t[6].size() < 2) continue;
        unsigned cnt = t[1][0];
        size_t skelPos = t[6][0], skelLen = t[6][1];
        if (skelPos > flow.size() || skelLen > flow.size() - skelPos) continue;
        std::string name = "part" + intToStr((long long)s) + ".xhtml";
        std::string text = flow.substr(skelPos, skelLen);
        size_t base = skelPos + skelLen;
        for (unsigned k = 0; k < cnt && fi < frag.size(); k++, fi++) {
            std::map<unsigned, std::vector<unsigned> >& ft = frag[fi].tags;
            if (!ft.count(6) || ft[6].size() < 2) continue;
            size_t ins = (size_t)strtoul(frag[fi].key.c_str(), NULL, 10);
            size_t fl = ft[6][1];
            if (base > flow.size() || fl > flow.size() - base) break;
            std::string slice = flow.substr(base, fl);
            /* якоря там, куда ведут ссылки; с конца, чтобы смещения не съезжали */
            std::map<int, std::set<unsigned> >::iterator tg = targets.find((int)fi);
            if (tg != targets.end()) {
                for (std::set<unsigned>::reverse_iterator o = tg->second.rbegin(); o != tg->second.rend(); ++o) {
                    size_t at = std::min<size_t>(*o, slice.size());
                    /* попали внутрь тега: якорь перед ним */
                    size_t lt = at ? slice.rfind('<', at - 1) : std::string::npos;
                    size_t gt = at ? slice.rfind('>', at - 1) : std::string::npos;
                    if (lt != std::string::npos && (gt == std::string::npos || gt < lt)) at = lt;
                    /* якорь — атрибут самого тега (абзаца, пункта списка): по нему читатель
                       находит блок сноски; отдельный <a> между блоками сноской не считается */
                    std::string id = Mark::id((int)fi, *o);
                    if (at < slice.size() && slice[at] == '<' && at + 1 < slice.size() && isalpha((unsigned char)slice[at + 1])) {
                        size_t e = slice.find('>', at);
                        size_t ne = at + 1;
                        while (ne < slice.size() && (isalnum((unsigned char)slice[ne]) || slice[ne] == ':')) ne++;
                        size_t has = e == std::string::npos ? std::string::npos : lowerAscii(slice.substr(at, e - at)).find(" id=");
                        if (e != std::string::npos && has == std::string::npos) {
                            slice.insert(ne, " id=\"" + id + "\"");
                            continue;
                        }
                        if (has != std::string::npos && at + has + 5 < e) {
                            /* у тега уже есть id: ссылаемся на него, а не вкладываем второй якорь */
                            size_t vs = at + has + 4;
                            char q = slice[vs];
                            size_t ve = (q == '"' || q == '\'') ? slice.find(q, vs + 1) : std::string::npos;
                            if (ve != std::string::npos && ve < e) {
                                alias[id] = slice.substr(vs + 1, ve - vs - 1);
                                continue;
                            }
                        }
                    }
                    slice.insert(at, "<a id=\"" + id + "\"></a>");
                }
            }
            size_t rel = ins >= skelPos ? ins - skelPos : 0;
            if (rel > text.size()) rel = text.size();
            text.insert(rel, slice);
            base += fl;
            fileOfFrag[(int)fi] = name;
        }
        names.push_back(name);
        z.push_back(ZipEntry(name, Bytes(text.begin(), text.end()), false));
    }
    if (names.empty()) {
        if (err) *err = "AZW3 повреждён: части книги не собираются";
        return false;
    }
    /* картинки: kindle:embed:XXXX — номер от первой картинки, с единицы */
    std::map<int, std::string> imgFile;
    std::string imgMan;
    std::string coverId;
    for (size_t i = 0; i < z.size(); i++) {
        std::string t(z[i].data.begin(), z[i].data.end());
        size_t p = 0;
        std::string res;
        for (;;) {
            size_t q = t.find("kindle:", p);
            if (q == std::string::npos) { res.append(t, p, std::string::npos); break; }
            res.append(t, p, q - p);
            if (t.compare(q, 13, "kindle:embed:") == 0) {
                int idx = base32Value(t.substr(q + 13, 4));
                size_t e = q + 17;
                /* «?mime=image/jpeg» не нужен */
                while (e < t.size() && t[e] != '"' && t[e] != '\'' && t[e] != ')' && t[e] != '>' && t[e] != ' ') e++;
                std::string target;
                if (idx > 0) {
                    std::map<int, std::string>::iterator f = imgFile.find(idx);
                    if (f != imgFile.end()) target = f->second;
                    else {
                        size_t recNo = (size_t)firstImage + (size_t)idx - 1;
                        Bytes rec = recNo + 1 < offsets.size() ? recordOf(b, offsets, recNo) : Bytes();
                        const char* ext = sniffImage(rec);
                        if (ext) {
                            target = "img" + intToStr(idx) + "." + ext;
                            imgFile[idx] = target;
                            z.push_back(ZipEntry(target, rec, true));
                            imgMan += "<item id=\"i" + intToStr(idx) + "\" href=\"" + target + "\" media-type=\"" + mimeFromExt(ext) + "\"/>";
                        }
                    }
                }
                res += target;
                p = e;
            } else if (t.compare(q, 15, "kindle:pos:fid:") == 0 && t.compare(q + 19, 5, ":off:") == 0) {
                int fid = base32Value(t.substr(q + 15, 4));
                int off = base32Value(t.substr(q + 24, 10));
                std::map<int, std::string>::iterator f = fileOfFrag.find(fid);
                if (fid >= 0 && off >= 0 && f != fileOfFrag.end()) {
                    std::string id = Mark::id(fid, (unsigned)off);
                    std::map<std::string, std::string>::iterator al = alias.find(id);
                    res += f->second + "#" + (al != alias.end() ? al->second : id);
                }
                p = q + 34;
            } else {
                res += "kindle:";
                p = q + 7;
            }
        }
        if (i < names.size()) z[i].data.assign(res.begin(), res.end());
    }
    if (coverOffset >= 0) {
        int idx = (int)coverOffset + 1;
        std::map<int, std::string>::iterator f = imgFile.find(idx);
        size_t recNo = (size_t)firstImage + (size_t)coverOffset;
        if (f == imgFile.end() && recNo + 1 < offsets.size()) {
            Bytes rec = recordOf(b, offsets, recNo);
            const char* ext = sniffImage(rec);
            if (ext) {
                std::string target = "img" + intToStr(idx) + "." + ext;
                imgFile[idx] = target;
                z.push_back(ZipEntry(target, rec, true));
                imgMan += "<item id=\"i" + intToStr(idx) + "\" href=\"" + target + "\" media-type=\"" + mimeFromExt(ext) + "\"/>";
            }
        }
        if (imgFile.count(idx)) coverId = "i" + intToStr(idx);
    }

    /* страница оглавления из таблицы guide — служебная, в тексте книги ей не место */
    std::string tocFile;
    if (guideIdx != 0xFFFFFFFFu) {
        std::vector<MobiIndexEntry> guide;
        std::map<unsigned, std::string> gc;
        if (mobiReadIndex(b, offsets, guideIdx, guide, gc)) {
            for (size_t i = 0; i < guide.size(); i++) {
                if (lowerAscii(guide[i].key) != "toc") continue;
                std::map<unsigned, std::vector<unsigned> >::iterator t = guide[i].tags.find(6);
                if (t == guide[i].tags.end() || t->second.empty()) continue;
                std::map<int, std::string>::iterator f = fileOfFrag.find((int)t->second[0]);
                if (f != fileOfFrag.end()) tocFile = f->second;
            }
        }
    }
    /* OPF и NCX */
    std::string man, sp;
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i] == tocFile && names.size() > 1) continue;
        man += "<item id=\"p" + intToStr((long long)i) + "\" href=\"" + names[i] + "\" media-type=\"application/xhtml+xml\"/>";
        sp += "<itemref idref=\"p" + intToStr((long long)i) + "\"/>";
    }
    man += imgMan + "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>";
    std::string metaC = coverId.empty() ? std::string() : "<meta name=\"cover\" content=\"" + coverId + "\"/>";
    std::string opf = "<?xml version=\"1.0\" encoding=\"utf-8\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>" +
                      escXml(book.meta.title) + "</dc:title>" + metaC + "</metadata><manifest>" + man + "</manifest><spine toc=\"ncx\">" + sp + "</spine></package>";
    std::string ncx = "<?xml version=\"1.0\" encoding=\"utf-8\"?><ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><navMap>";
    int cur = -1, np = 1;
    for (size_t i = 0; i < ncxE.size(); i++) {
        std::map<unsigned, std::vector<unsigned> >& t = ncxE[i].tags;
        if (!t.count(3) || t[3].empty() || !t.count(6) || t[6].size() < 2) continue;
        std::map<unsigned, std::string>::iterator lab = ncxCncx.find(t[3][0]);
        if (lab == ncxCncx.end()) continue;
        std::map<int, std::string>::iterator f = fileOfFrag.find((int)t[6][0]);
        if (f == fileOfFrag.end() || f->second == tocFile) continue;
        int lv = t.count(4) && !t[4].empty() ? (int)t[4][0] : 0;
        if (lv > cur + 1) lv = cur + 1;
        while (cur >= lv) { ncx += "</navPoint>"; cur--; }
        ncx += "<navPoint id=\"n" + intToStr(np) + "\" playOrder=\"" + intToStr(np) + "\"><navLabel><text>" + escXml(lab->second) +
               "</text></navLabel><content src=\"" + f->second + "#" + (alias.count(Mark::id((int)t[6][0], t[6][1])) ? alias[Mark::id((int)t[6][0], t[6][1])] : Mark::id((int)t[6][0], t[6][1])) + "\"/>";
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
    /* SHARKONV_KF8DEBUG=файл — собранный из KF8 EPUB, для разбора жалоб */
    const char* dbg = getenv("SHARKONV_KF8DEBUG");
    if (dbg && *dbg) {
        FILE* df = fopen(dbg, "wb");
        if (df) { fwrite(&zipData[0], 1, zipData.size(), df); fclose(df); }
    }
    Book inner;
    if (!readBook(pathStripExt(fileName) + ".epub", zipData, inner, err)) return false;
    /* описание уже прочитано из заголовка MOBI, отсюда только содержание */
    book.chapters.swap(inner.chapters);
    book.notes.swap(inner.notes);
    book.images.swap(inner.images);
    book.imageHash.swap(inner.imageHash);
    book.coverName = inner.coverName;
    return true;
}
