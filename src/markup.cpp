#include "markup.h"

static bool isNameChar(unsigned char c) {
    return isalnum(c) || c >= 0x80 || c == '-' || c == '_' || c == ':' || c == '.';
}

const MNode* MNode::child(const char* n) const {
    for (size_t i = 0; i < kids.size(); i++)
        if (!kids[i]->text && kids[i]->name == n) return kids[i].get();
    return NULL;
}

std::vector<const MNode*> MNode::childrenNamed(const char* n) const {
    std::vector<const MNode*> out;
    for (size_t i = 0; i < kids.size(); i++)
        if (!kids[i]->text && kids[i]->name == n) out.push_back(kids[i].get());
    return out;
}

static std::string localPart(const std::string& n) {
    size_t p = n.rfind(':');
    return p == std::string::npos ? n : n.substr(p + 1);
}

std::string MNode::attr(const char* n) const {
    std::string want = lowerAscii(n);
    for (size_t i = 0; i < attrs.size(); i++)
        if (attrs[i].first == want) return attrs[i].second;
    std::string lw = localPart(want);
    for (size_t i = 0; i < attrs.size(); i++)
        if (localPart(attrs[i].first) == lw) return attrs[i].second;
    return std::string();
}

bool MNode::hasAttr(const char* n) const {
    std::string want = lowerAscii(n);
    for (size_t i = 0; i < attrs.size(); i++)
        if (attrs[i].first == want) return true;
    std::string lw = localPart(want);
    for (size_t i = 0; i < attrs.size(); i++)
        if (localPart(attrs[i].first) == lw) return true;
    return false;
}

static void collectText(const MNode* n, std::string& out) {
    if (n->text) { out += n->value; return; }
    for (size_t i = 0; i < n->kids.size(); i++) collectText(n->kids[i].get(), out);
}

std::string MNode::textContent() const {
    std::string s;
    collectText(this, s);
    return s;
}

static void ser(const MNode* n, std::string& out) {
    if (n->text) { out += escHtml(n->value); return; }
    out += '<';
    out += n->name;
    for (size_t i = 0; i < n->attrs.size(); i++) {
        out += ' ';
        out += n->attrs[i].first;
        out += "=\"";
        out += escHtml(n->attrs[i].second);
        out += '"';
    }
    out += '>';
    for (size_t i = 0; i < n->kids.size(); i++) ser(n->kids[i].get(), out);
    out += "</";
    out += n->name;
    out += '>';
}

std::string MNode::innerHtml() const {
    std::string out;
    for (size_t i = 0; i < kids.size(); i++) ser(kids[i].get(), out);
    return out;
}

/* ============================== парсер ================================== */

static bool isVoidHtml(const std::string& n) {
    static const char* v[] = {
        "area","base","br","col","embed","hr","img","input","keygen","link",
        "meta","param","source","track","wbr","!doctype", NULL
    };
    for (int i = 0; v[i]; i++) if (n == v[i]) return true;
    return false;
}

static bool isBlockHtml(const std::string& n) {
    static const char* v[] = {
        "address","article","aside","blockquote","div","dl","dd","dt","fieldset","figcaption",
        "figure","footer","form","h1","h2","h3","h4","h5","h6","header","hr","li","main",
        "nav","ol","p","pre","section","table","tbody","td","tfoot","th","thead","tr","ul",
        "body","html", NULL
    };
    for (int i = 0; v[i]; i++) if (n == v[i]) return true;
    return false;
}

namespace {

class Parser {
public:
    Parser(const std::string& s, bool html) : src(s), htmlMode(html) {}

    MNode run() {
        MNode root;
        std::vector<MNode*> stack;
        stack.push_back(&root);
        std::string lowSrc; /* src в нижнем регистре — строится один раз, при первом <script>/<style> */
        size_t i = 0;
        const size_t n = src.size();
        while (i < n) {
            if (src[i] != '<') {
                size_t lt = src.find('<', i);
                if (lt == std::string::npos) lt = n;
                std::string txt = src.substr(i, lt - i);
                MNode* t = new MNode(true);
                t->value = unescapeEntities(txt);
                stack.back()->kids.push_back(std::unique_ptr<MNode>(t));
                i = lt;
                continue;
            }
            if (i + 3 < n && src.compare(i, 4, "<!--") == 0) {
                size_t e = src.find("-->", i + 4);
                i = (e == std::string::npos) ? n : e + 3;
                continue;
            }
            if (i + 8 < n && src.compare(i, 9, "<![CDATA[") == 0) {
                size_t e = src.find("]]>", i + 9);
                std::string raw = src.substr(i + 9, (e == std::string::npos ? n : e) - i - 9);
                MNode* t = new MNode(true);
                t->value = raw;
                stack.back()->kids.push_back(std::unique_ptr<MNode>(t));
                i = (e == std::string::npos) ? n : e + 3;
                continue;
            }
            if (i + 1 < n && (src[i + 1] == '!' || src[i + 1] == '?')) {
                size_t e = src.find('>', i + 2);
                i = (e == std::string::npos) ? n : e + 1;
                continue;
            }
            if (i + 1 < n && src[i + 1] == '/') {
                size_t e = src.find('>', i + 2);
                if (e == std::string::npos) { i = n; break; }
                std::string nm = lowerAscii(trim(src.substr(i + 2, e - i - 2)));
                /* имя элемента без пространства имён: </w:body> -> body */
                {
                    size_t cl = nm.find(':');
                    if (cl != std::string::npos && cl + 1 < nm.size()) nm = nm.substr(cl + 1);
                }
                /* отрезаем мусор после имени */
                size_t sp = nm.find_first_of(" \t\r\n");
                if (sp != std::string::npos) nm = nm.substr(0, sp);
                closeTag(stack, nm);
                i = e + 1;
                continue;
            }
            /* открывающий тег */
            size_t p = i + 1;
            size_t nameStart = p;
            while (p < n && isNameChar((unsigned char)src[p])) p++;
            if (p == nameStart) { /* не тег, просто символ */
                MNode* t = new MNode(true);
                t->value = "<";
                stack.back()->kids.push_back(std::unique_ptr<MNode>(t));
                i++;
                continue;
            }
            std::string name = lowerAscii(src.substr(nameStart, p - nameStart));
            std::string nameRaw = name;
            {
                size_t cl = name.find(':');
                if (cl != std::string::npos && cl + 1 < name.size()) name = name.substr(cl + 1);
            }
            std::vector<std::pair<std::string, std::string> > attrs;
            bool selfClose = false;
            bool bad = false;
            for (;;) {
                while (p < n && (unsigned char)src[p] <= ' ') p++;
                if (p >= n) { bad = true; break; }
                if (src[p] == '>') { p++; break; }
                if (src[p] == '/' && p + 1 < n && src[p + 1] == '>') { selfClose = true; p += 2; break; }
                size_t as = p;
                while (p < n && isNameChar((unsigned char)src[p])) p++;
                if (p == as) { p++; continue; }
                std::string an = lowerAscii(src.substr(as, p - as));
                while (p < n && (unsigned char)src[p] <= ' ') p++;
                std::string av;
                if (p < n && src[p] == '=') {
                    p++;
                    while (p < n && (unsigned char)src[p] <= ' ') p++;
                    if (p < n && (src[p] == '"' || src[p] == '\'')) {
                        char q = src[p++];
                        size_t vs = p;
                        while (p < n && src[p] != q) p++;
                        av = src.substr(vs, p - vs);
                        if (p < n) p++;
                    } else {
                        size_t vs = p;
                        while (p < n && (unsigned char)src[p] > ' ' && src[p] != '>') p++;
                        av = src.substr(vs, p - vs);
                    }
                    av = unescapeEntities(av);
                }
                attrs.push_back(std::make_pair(an, av));
            }
            if (bad) { i = n; break; }
            i = p;

            /* сырой текст — только у HTML-тегов <script>/<style>; <w:style> из DOCX — обычный элемент */
            if (htmlMode && !selfClose && (nameRaw == "script" || nameRaw == "style")) {
                if (lowSrc.empty()) lowSrc = lowerAscii(src);
                std::string closeTagStr = "</" + nameRaw;
                size_t e = lowSrc.find(closeTagStr, i);
                if (e == std::string::npos) { i = n; }
                else {
                    size_t g = src.find('>', e);
                    i = (g == std::string::npos) ? n : g + 1;
                }
                continue;
            }

            if (htmlMode) {
                implicitClose(stack, name);
                if (!stack.empty() && stack.size() > 1) {
                    /* <p> внутри <p> и подобное */
                }
            }

            MNode* el = new MNode();
            el->name = name;
            el->attrs = attrs;

            /* перенос атрибутов в нижний регистр уже сделан */
            stack.back()->kids.push_back(std::unique_ptr<MNode>(el));

            bool isVoid = htmlMode ? isVoidHtml(name) : false;
            /* XML: <tag/> самозакрывающийся; html: тоже уважаем */
            if (!selfClose && !isVoid) {
                stack.push_back(el);
            }
        }
        return root;
    }

private:
    void closeTag(std::vector<MNode*>& stack, const std::string& nm) {
        if (nm.empty()) return;
        for (size_t k = stack.size(); k-- > 1;) {
            if (stack[k]->name == nm) {
                stack.resize(k);
                return;
            }
        }
    }

    void implicitClose(std::vector<MNode*>& stack, const std::string& name) {
        if (stack.size() < 2) return;
        std::string top = stack.back()->name;

        if (name == "li" && top == "li") { stack.pop_back(); return; }
        if ((name == "td" || name == "th") && (top == "td" || top == "th")) { stack.pop_back(); return; }
        if (name == "tr") {
            if (top == "td" || top == "th") stack.pop_back();
            if (stack.size() > 1 && stack.back()->name == "tr") stack.pop_back();
            return;
        }
        if (name == "option" && top == "option") { stack.pop_back(); return; }
        if (name == "dt" || name == "dd") {
            if (top == "dt" || top == "dd") stack.pop_back();
            return;
        }
        if (isBlockHtml(name)) {
            /* закрываем висящий <p> и незакрытые строчные обёртки */
            for (size_t k = stack.size(); k-- > 1;) {
                const std::string& t = stack[k]->name;
                if (t == "p") { stack.resize(k); break; }
                if (isBlockHtml(t) || t == "body" || t == "html" || t == "table" ||
                    t == "ul" || t == "ol" || t == "blockquote") break;
                if (t == "span" || t == "a" || t == "b" || t == "i" || t == "em" ||
                    t == "strong" || t == "font" || t == "small" || t == "big" ||
                    t == "u" || t == "s" || t == "strike" || t == "sup" || t == "sub") {
                    continue;
                }
                break;
            }
        }
    }

    const std::string& src;
    bool htmlMode;
};

} /* namespace */

MNode parseMarkup(const std::string& src, bool htmlMode) {    std::string s = src;
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        s = s.substr(3);
    Parser p(s, htmlMode);
    return p.run();
}

std::string decodeToUtf8(const Bytes& data) {
    return decodeText(data, NULL);
}

static void findNodesRec(const MNode* n, const std::string& name, std::vector<const MNode*>& out) {
    for (size_t i = 0; i < n->kids.size(); i++) {
        const MNode* k = n->kids[i].get();
        if (k->text) continue;
        if (k->name == name) out.push_back(k);
        findNodesRec(k, name, out);
    }
}

std::vector<const MNode*> findNodes(const MNode* n, const char* name) {
    std::vector<const MNode*> out;
    if (n) findNodesRec(n, lowerAscii(name), out);
    return out;
}

const MNode* findNode(const MNode* n, const char* name) {
    if (!n) return NULL;
    std::string want = lowerAscii(name);
    for (size_t i = 0; i < n->kids.size(); i++) {
        const MNode* k = n->kids[i].get();
        if (k->text) continue;
        if (k->name == want) return k;
        const MNode* deep = findNode(k, name);
        if (deep) return deep;
    }
    return NULL;
}
