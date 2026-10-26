/* Терпимый разбор XML/HTML в дерево. Без внешних зависимостей. */
#ifndef FB2C_MARKUP_H
#define FB2C_MARKUP_H

#include "common.h"
#include <memory>

struct MNode {
    bool text;
    std::string name;        /* имя элемента в нижнем регистре */
    std::string value;       /* текст для текстового узла */
    std::vector<std::pair<std::string, std::string> > attrs;
    std::vector<std::unique_ptr<MNode> > kids;

    MNode() : text(false) {}
    explicit MNode(bool isText) : text(isText) {}

    const MNode* child(const char* n) const;
    std::vector<const MNode*> childrenNamed(const char* n) const;
    std::string attr(const char* n) const;
    bool hasAttr(const char* n) const;
    std::string textContent() const;
    std::string innerHtml() const;
};

/* htmlMode: терпимость к незакрытым тегам, void-элементы, <script>/<style> */
MNode parseMarkup(const std::string& src, bool htmlMode);

/* Поиск элементов по имени во всём поддереве. */
std::vector<const MNode*> findNodes(const MNode* n, const char* name);
const MNode* findNode(const MNode* n, const char* name);

/* Декодировать текст из байтов с автоопределением кодировки. */
std::string decodeToUtf8(const Bytes& data);

#endif
