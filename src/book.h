/* Модель книги (единый внутренний формат для всех конвертаций). */
#ifndef FB2C_BOOK_H
#define FB2C_BOOK_H

#include "common.h"

struct Block {
    enum Type { P = 0, SUB, EMPTY, IMG, QUOTE, POEM, STANZA, V, AUTHOR, LI, PRE, RAW };
    int type;
    std::string html;
    std::string cls;
    std::string src;
    std::string id;
    std::vector<Block> blocks;

    Block() : type(P) {}
    explicit Block(int t) : type(t) {}

    static Block p(const std::string& h) { Block b(P); b.html = h; return b; }
    static Block sub(const std::string& h) { Block b(SUB); b.html = h; return b; }
    static Block empty() { return Block(EMPTY); }
    static Block quote(const std::string& c, const std::vector<Block>& b) {
        Block x(QUOTE); x.cls = c; x.blocks = b; return x;
    }
    static Block wrap(int t, const std::vector<Block>& b) {
        Block x(t); x.blocks = b; return x;
    }
};

struct Chapter {
    int level;
    std::string title;
    std::vector<Block> blocks;
    Chapter() : level(0) {}
};

struct Note {
    std::string id;
    std::string title;
    std::vector<Block> blocks;
};

struct Meta {
    std::string title;
    std::vector<Author> authors;
    std::vector<std::string> genres;
    std::string annotation;
    std::string keywords;
    std::string lang;
    std::string srcLang;
    std::string year;
    std::string publisher;
    std::string city;
    std::string isbn;
    std::string seriesName;
    std::string seriesNum;
    std::vector<Author> translators;
    std::string program;
    Meta() : lang("ru"), program("FB2 Конвертер") {}
};

struct Image {
    std::string name;
    std::string mime;
    Bytes bytes;
};

struct Book {
    std::string fileName;
    Meta meta;
    std::map<std::string, Image> images;
    std::map<std::string, std::string> imageHash;
    std::string coverName;
    std::vector<Chapter> chapters;
    std::vector<Note> notes;
};

/* Картинки */
std::string extFromMime(const std::string& mime);
std::string mimeFromExt(const std::string& ext);
std::string addImage(Book& book, const std::string& mime, const Bytes& data);
std::string addImageDataUrl(Book& book, const std::string& url);

/* Метаданные */
std::string authorsLine(const Book& book);
void normalizeBook(Book& book);
std::string outBaseName(const Book& book);

/* Плоский список глав -> дерево */
struct ChapterNode {
    int level;
    std::string title;
    const std::vector<Block>* blocks;
    std::vector<ChapterNode> children;
    ChapterNode() : level(0), blocks(NULL) {}
};
std::vector<ChapterNode> buildTree(const std::vector<Chapter>& chapters);

/* верхний уровень — нулевой, без скачков через уровень (h1 -> h3 становится 0 -> 1) */
void normalizeChapterLevels(std::vector<Chapter>& chapters);

#endif
