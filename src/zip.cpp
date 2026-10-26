#include "zip.h"
#include <zlib.h>

static unsigned rd32(const Bytes& b, size_t p) {
    if (p + 4 > b.size()) return 0;
    return (unsigned)b[p] | ((unsigned)b[p + 1] << 8) | ((unsigned)b[p + 2] << 16) |
           ((unsigned)b[p + 3] << 24);
}
static unsigned rd16(const Bytes& b, size_t p) {
    if (p + 2 > b.size()) return 0;
    return (unsigned)b[p] | ((unsigned)b[p + 1] << 8);
}
static void wr16(Bytes& b, unsigned v) {
    b.push_back((unsigned char)(v & 0xFF));
    b.push_back((unsigned char)((v >> 8) & 0xFF));
}
static void wr32(Bytes& b, unsigned v) {
    b.push_back((unsigned char)(v & 0xFF));
    b.push_back((unsigned char)((v >> 8) & 0xFF));
    b.push_back((unsigned char)((v >> 16) & 0xFF));
    b.push_back((unsigned char)((v >> 24) & 0xFF));
}

bool rawInflate(const unsigned char* src, size_t srcLen, Bytes& out, std::string* err) {
    out.clear();
    if (srcLen == 0) return true;
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -15) != Z_OK) {
        if (err) *err = "zlib: не удалось начать распаковку";
        return false;
    }
    zs.next_in = (Bytef*)src;
    zs.avail_in = (uInt)srcLen;
    unsigned char buf[65536];
    int rc = Z_OK;
    do {
        zs.next_out = buf;
        zs.avail_out = sizeof(buf);
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) break;
        size_t got = sizeof(buf) - zs.avail_out;
        if (got) out.insert(out.end(), buf, buf + got);
        if (rc == Z_BUF_ERROR && got == 0) break;
    } while (rc != Z_STREAM_END);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && rc != Z_OK && rc != Z_BUF_ERROR) {
        if (err) *err = "Не удалось распаковать данные (повреждённый архив?)";
        return false;
    }
    return true;
}

bool rawDeflate(const unsigned char* src, size_t srcLen, Bytes& out) {
    out.clear();
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    zs.next_in = (Bytef*)src;
    zs.avail_in = (uInt)srcLen;
    unsigned char buf[65536];
    int rc;
    do {
        zs.next_out = buf;
        zs.avail_out = sizeof(buf);
        rc = deflate(&zs, Z_FINISH);
        size_t got = sizeof(buf) - zs.avail_out;
        if (got) out.insert(out.end(), buf, buf + got);
    } while (rc == Z_OK);
    deflateEnd(&zs);
    return rc == Z_STREAM_END;
}

bool zipRead(const Bytes& b, std::map<std::string, Bytes>& out, std::string* err) {
    out.clear();
    if (b.size() < 22) {
        if (err) *err = "Файл слишком мал для ZIP";
        return false;
    }
    size_t eocd = (size_t)-1;
    size_t minPos = b.size() > 66000 ? b.size() - 66000 : 0;
    for (size_t i = b.size() - 22 + 1; i-- > minPos;) {
        if (rd32(b, i) == 0x06054b50) { eocd = i; break; }
        if (i == 0) break;
    }
    if (eocd == (size_t)-1) {
        if (err) *err = "Это не ZIP-архив (или он повреждён)";
        return false;
    }
    unsigned count = rd16(b, eocd + 10);
    size_t off = rd32(b, eocd + 16);

    for (unsigned i = 0; i < count; i++) {
        if (off + 46 > b.size() || rd32(b, off) != 0x02014b50) break;
        unsigned flags = rd16(b, off + 8);
        unsigned method = rd16(b, off + 10);
        unsigned csize = rd32(b, off + 20);
        unsigned nameLen = rd16(b, off + 28);
        unsigned extraLen = rd16(b, off + 30);
        unsigned cmtLen = rd16(b, off + 32);
        size_t lho = rd32(b, off + 42);
        if (off + 46 + nameLen > b.size()) break;
        std::string rawName((const char*)&b[off + 46], nameLen);
        std::string name;
        if (flags & 0x800) name = rawName;
        else if (isValidUtf8(rawName)) name = rawName;
        else name = cpToUtf8(rawName, 1251);
        off += 46 + nameLen + extraLen + cmtLen;

        if (name.empty() || name[name.size() - 1] == '/') continue;
        if (lho + 30 > b.size() || rd32(b, lho) != 0x04034b50) continue;
        unsigned lNameLen = rd16(b, lho + 26);
        unsigned lExtraLen = rd16(b, lho + 28);
        size_t start = lho + 30 + lNameLen + lExtraLen;
        if (start > b.size()) continue;
        if (start + csize > b.size()) csize = (unsigned)(b.size() - start);

        Bytes data;
        if (method == 0) {
            data.assign(b.begin() + start, b.begin() + start + csize);
        } else if (method == 8) {
            std::string e;
            if (!rawInflate(&b[start], csize, data, &e)) {
                if (err) *err = "Не удалось распаковать «" + name + "»: " + e;
                return false;
            }
        } else {
            if (err) *err = "Неподдерживаемый метод сжатия ZIP: " + intToStr(method);
            return false;
        }
        out[name] = data;
    }
    if (out.empty()) {
        if (err) *err = "В архиве нет файлов";
        return false;
    }
    return true;
}

Bytes zipWrite(const std::vector<ZipEntry>& entries) {
    Bytes out;
    Bytes central;
    unsigned offset = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    unsigned dosTime = ((unsigned)(st.wHour << 11) | (unsigned)(st.wMinute << 5) |
                        (unsigned)(st.wSecond >> 1)) & 0xFFFF;
    unsigned dosDate = (((unsigned)(st.wYear - 1980) << 9) | ((unsigned)st.wMonth << 5) |
                        (unsigned)st.wDay) & 0xFFFF;

    for (size_t i = 0; i < entries.size(); i++) {
        const ZipEntry& e = entries[i];
        Bytes body;
        unsigned method = 0;
        if (!e.store && e.data.size() > 64) {
            Bytes def;
            if (rawDeflate(e.data.empty() ? (const unsigned char*)"" : &e.data[0], e.data.size(), def) &&
                def.size() < e.data.size()) {
                body.swap(def);
                method = 8;
            }
        }
        if (method == 0) body = e.data;
        unsigned crc = (unsigned)crc32(0L, e.data.empty() ? (const Bytef*)"" : (const Bytef*)&e.data[0],
                                       (uInt)e.data.size());

        Bytes lh;
        wr32(lh, 0x04034b50);
        wr16(lh, 20);
        wr16(lh, 0x0800);
        wr16(lh, method);
        wr16(lh, dosTime);
        wr16(lh, dosDate);
        wr32(lh, crc);
        wr32(lh, (unsigned)body.size());
        wr32(lh, (unsigned)e.data.size());
        wr16(lh, (unsigned)e.name.size());
        wr16(lh, 0);
        lh.insert(lh.end(), e.name.begin(), e.name.end());

        out.insert(out.end(), lh.begin(), lh.end());
        out.insert(out.end(), body.begin(), body.end());

        wr32(central, 0x02014b50);
        wr16(central, 20);
        wr16(central, 20);
        wr16(central, 0x0800);
        wr16(central, method);
        wr16(central, dosTime);
        wr16(central, dosDate);
        wr32(central, crc);
        wr32(central, (unsigned)body.size());
        wr32(central, (unsigned)e.data.size());
        wr16(central, (unsigned)e.name.size());
        wr16(central, 0);
        wr16(central, 0);
        wr16(central, 0);
        wr16(central, 0);
        wr32(central, 0);
        wr32(central, offset);
        central.insert(central.end(), e.name.begin(), e.name.end());

        offset += (unsigned)(lh.size() + body.size());
    }

    unsigned centralStart = offset;
    out.insert(out.end(), central.begin(), central.end());

    wr32(out, 0x06054b50);
    wr16(out, 0);
    wr16(out, 0);
    wr16(out, (unsigned)entries.size());
    wr16(out, (unsigned)entries.size());
    wr32(out, (unsigned)central.size());
    wr32(out, centralStart);
    wr16(out, 0);
    return out;
}
