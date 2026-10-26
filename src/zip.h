/* ZIP: чтение и запись. Сжатие — zlib (raw deflate). */
#ifndef FB2C_ZIP_H
#define FB2C_ZIP_H

#include "common.h"

struct ZipEntry {
    std::string name;
    Bytes data;
    bool store;
    ZipEntry() : store(false) {}
    ZipEntry(const std::string& n, const Bytes& d, bool s) : name(n), data(d), store(s) {}
};

bool zipRead(const Bytes& zip, std::map<std::string, Bytes>& out, std::string* err);

/* Порядок записей сохраняется; mimetype для EPUB нужно ставить первым с store=true. */
Bytes zipWrite(const std::vector<ZipEntry>& entries);

/* Раздуть/сжать raw deflate. */
bool rawInflate(const unsigned char* src, size_t srcLen, Bytes& out, std::string* err);
bool rawDeflate(const unsigned char* src, size_t srcLen, Bytes& out);

#endif
