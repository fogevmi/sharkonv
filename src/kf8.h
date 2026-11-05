/* Общие части MOBI и KF8: индексы, сжатие HUFF/CDIC, сборка книги KF8. */
#ifndef FB2C_KF8_H
#define FB2C_KF8_H

#include "book.h"

struct MobiIndexEntry {
    std::string key;
    std::map<unsigned, std::vector<unsigned> > tags;
};

/* Индекс INDX + TAGX + IDXT; cncx — строки индекса по смещению. */
bool mobiReadIndex(const Bytes& b, const std::vector<unsigned>& offsets, unsigned indx,
                   std::vector<MobiIndexEntry>& out, std::map<unsigned, std::string>& cncx);

class HuffCdic {
public:
    bool load(const Bytes& b, const std::vector<unsigned>& offsets, unsigned first, unsigned count);
    bool unpack(const unsigned char* d, size_t n, std::string& out, int depth = 0);

private:
    struct Phrase { std::string data; bool done, busy; };
    std::vector<Phrase> phrases_;
    unsigned char codeLen_[256];
    bool term_[256];
    unsigned long long maxCode_[256], min_[33], max_[33];
};

/* flow — текст первого потока KF8 (HTML). Заполняет главы, сноски, картинки; описание в book не трогает. */
bool readKF8Body(const Bytes& b, const std::vector<unsigned>& offsets, const std::string& flow, unsigned skelIdx,
                 unsigned fragIdx, unsigned ncxIdx, unsigned guideIdx, unsigned firstImage, long coverOffset, const std::string& fileName,
                 Book& book, std::string* err);

#endif
