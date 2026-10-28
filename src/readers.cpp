/* Читатели: FB2, TXT, HTML, DOCX, EPUB, MOBI. Всё приводится к Book (HTML-фрагменты). */
#include "convert.h"
#include "markup.h"
#include "zip.h"
#include <functional>

/* ============================== помощники =============================== */

static std::string elText(const MNode* n) { return n ? collapse(n->textContent()) : std::string(); }

static void collectNamed(const MNode* n, const std::string& name, std::vector<const MNode*>& out) {
    for (size_t i = 0; i < n->kids.size(); i++) {
        const MNode* k = n->kids[i].get();
        if (k->text) continue;
        if (k->name == name) out.push_back(k);
        collectNamed(k, name, out);
    }
}

static std::vector<const MNode*> allNamed(const MNode* n, const char* name) {
    std::vector<const MNode*> out;
    collectNamed(n, name, out);
    return out;
}

static const MNode* findFirst(const MNode* n, const char* name) {
    if (!n) return NULL;
    for (size_t i = 0; i < n->kids.size(); i++) {
        const MNode* k = n->kids[i].get();
        if (k->text) continue;
        if (k->name == name) return k;
        const MNode* deep = findFirst(k, name);
        if (deep) return deep;
    }
    return NULL;
}

static int parseIntSafe(const std::string& s, int def) {
    if (s.empty()) return def;
    char* end = NULL;
    long v = strtol(s.c_str(), &end, 10);
    return end == s.c_str() ? def : (int)v;
}

/* Маленькая структура для передачи контекста в HTML-разбор. */
struct HtmlCtx {
    Book* book;
    std::map<std::string, Bytes>* files; /* ключи в нижнем регистре */
    std::string baseDir;
    std::map<std::string, std::string>* mobiImages; /* "1" -> имя картинки */
    const std::set<const MNode*>* skip;             /* узлы, ставшие сносками: в текст не идут */
    std::string diskDir;                            /* папка HTML-файла на диске (UTF-8) */
    HtmlCtx() : book(NULL), files(NULL), mobiImages(NULL), skip(NULL) {}
};

static std::string normalizeRel(const std::string& base, const std::string& rel) {
    std::string b = replaceAll(base, "\\", "/");
    std::string r = rel;
    r = replaceAll(r, "\\", "/");
    size_t h = r.find('#');
    if (h != std::string::npos) r = r.substr(0, h);
    size_t q = r.find('?');
    if (q != std::string::npos) r = r.substr(0, q);
    if (r.empty()) return std::string();
    if (r[0] == '/') {
        r = r.substr(1);
        if (startsWith(r, "./")) r = r.substr(2);
        return r;
    }
    std::vector<std::string> parts;
    if (!b.empty()) {
        size_t pos = 0;
        while (pos <= b.size()) {
            size_t sl = b.find('/', pos);
            if (sl == std::string::npos) sl = b.size();
            std::string seg = b.substr(pos, sl - pos);
            if (!seg.empty() && seg != ".") parts.push_back(seg);
            if (sl == b.size()) break;
            pos = sl + 1;
        }
    }
    size_t pos = 0;
    while (pos <= r.size()) {
        size_t sl = r.find('/', pos);
        if (sl == std::string::npos) sl = r.size();
        std::string seg = r.substr(pos, sl - pos);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!seg.empty() && seg != ".") parts.push_back(seg);
        if (sl == r.size()) break;
        pos = sl + 1;
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += '/';
        out += parts[i];
    }
    return out;
}

static Bytes* zipGet(HtmlCtx& ctx, const std::string& path) {
    if (!ctx.files) return NULL;
    std::map<std::string, Bytes>::iterator it = ctx.files->find(lowerAscii(path));
    if (it == ctx.files->end()) return NULL;
    return &it->second;
}

static std::string imageFromEl(const MNode* el, HtmlCtx& ctx) {
    std::string src = trim(el->attr("src"));
    if (src.empty()) src = trim(el->attr("data-src"));
    if (src.empty()) return std::string();
    std::string low = lowerAscii(src);
    if (startsWith(low, "data:")) return addImageDataUrl(*ctx.book, src);
    if (ctx.mobiImages) {
        if (startsWith(low, "mobi:")) {
            std::string k = src.substr(5);
            std::map<std::string, std::string>::iterator it = ctx.mobiImages->find(k);
            if (it != ctx.mobiImages->end()) return it->second;
        }
        return std::string();
    }
    if (startsWith(low, "images/")) {
        /* уже зарегистрировано читателем (FB2/MOBI) */
        std::string name = src.substr(7);
        std::map<std::string, Image>::iterator it = ctx.book->images.find(name);
        if (it != ctx.book->images.end()) return name;
        return std::string();
    }
    if (ctx.files) {
        std::string p = normalizeRel(ctx.baseDir, src);
        Bytes* data = zipGet(ctx, p);
        if (data && !data->empty()) {
            std::string mime = mimeFromExt(pathExt(p));
            return addImage(*ctx.book, mime, *data);
        }
    }
    if (!ctx.diskDir.empty()) {
        /* HTML с диска: картинки лежат рядом (Word кладёт их в «имя.files/») */
        std::string rel = src;
        if (startsWith(low, "file:///")) rel = rel.substr(8);
        if (low.find("://") != std::string::npos && !startsWith(low, "file:")) return std::string();
        std::string dec;
        for (size_t i = 0; i < rel.size(); i++) {
            if (rel[i] == '%' && i + 2 < rel.size() && isxdigit((unsigned char)rel[i + 1]) && isxdigit((unsigned char)rel[i + 2])) {
                dec += (char)strtol(rel.substr(i + 1, 2).c_str(), NULL, 16);
                i += 2;
            } else {
                dec += rel[i];
            }
        }
        size_t q = dec.find_first_of("?#");
        if (q != std::string::npos) dec = dec.substr(0, q);
        std::string ext = lowerAscii(pathExt(dec));
        if (ext != "png" && ext != "jpg" && ext != "jpeg" && ext != "gif" && ext != "bmp" && ext != "webp") return std::string();
        std::string full = (dec.size() > 1 && dec[1] == ':') ? dec : ctx.diskDir + "/" + dec;
        Bytes data;
        std::string ferr;
        if (readFileBytes(utf8ToW(replaceAll(full, "/", "\\")), data, &ferr) && !data.empty() && data.size() < 30u * 1024 * 1024)
            return addImage(*ctx.book, mimeFromExt(ext), data);
    }
    return std::string();
}

/* ============================ инлайн-HTML ============================== */

static bool skipInlineTag(const std::string& t) {
    return t == "script" || t == "style" || t == "noscript" || t == "head" || t == "title" ||
           t == "meta" || t == "link";
}

static std::string inlineHtmlOf(const MNode* node, HtmlCtx& ctx) {
    std::string s;
    for (size_t i = 0; i < node->kids.size(); i++) {
        const MNode* ch = node->kids[i].get();
        if (ch->text) {
            std::string v = ch->value;
            for (size_t k = 0; k < v.size(); k++) {
                unsigned char c = (unsigned char)v[k];
                if (c == '\r' || c == '\n' || c == '\t') v[k] = ' ';
            }
            std::string vv;
            bool sp = false;
            for (size_t k = 0; k < v.size(); k++) {
                if (v[k] == ' ') { if (!sp) vv += ' '; sp = true; }
                else { vv += v[k]; sp = false; }
            }
            s += escHtml(vv);
            continue;
        }
        const std::string& t = ch->name;
        if (skipInlineTag(t)) continue;
        std::string inner = inlineHtmlOf(ch, ctx);
        if (t == "b" || t == "strong") s += "<strong>" + inner + "</strong>";
        else if (t == "i" || t == "em" || t == "cite" || t == "dfn") s += "<em>" + inner + "</em>";
        else if (t == "s" || t == "strike" || t == "del") s += "<s>" + inner + "</s>";
        else if (t == "u" || t == "ins") s += "<u>" + inner + "</u>";
        else if (t == "sub") s += "<sub>" + inner + "</sub>";
        else if (t == "sup") s += "<sup>" + inner + "</sup>";
        else if (t == "code" || t == "tt" || t == "kbd") s += "<code>" + inner + "</code>";
        else if (t == "br") s += "<br/>";
        else if (t == "a") {
            std::string href = trim(ch->attr("href"));
            if (!href.empty() && lowerAscii(href).compare(0, 11, "javascript:") != 0)
                s += "<a href=\"" + escHtml(href) + "\">" + inner + "</a>";
            else
                s += inner;
        } else if (t == "img") {
            std::string name = imageFromEl(ch, ctx);
            if (!name.empty()) s += "<img src=\"images/" + escHtml(name) + "\" alt=\"\"/>";
        } else {
            s += inner;
        }
    }
    return s;
}

/* ============================ блоки HTML =============================== */

static bool hasBlockChildren(const MNode* el) {
    for (size_t i = 0; i < el->kids.size(); i++) {
        const MNode* ch = el->kids[i].get();
        if (ch->text) continue;
        const std::string& t = ch->name;
        if (t == "p" || t == "div" || t == "section" || t == "article" || t == "h1" ||
            t == "h2" || t == "h3" || t == "h4" || t == "h5" || t == "h6" || t == "ul" ||
            t == "ol" || t == "table" || t == "blockquote" || t == "pre" || t == "hr" ||
            t == "figure" || t == "main" || t == "dl" || t == "body")
            return true;
    }
    return false;
}

static std::string cleanTableHtml(const MNode* table, HtmlCtx& ctx) {
    std::string html = "<table>";
    std::vector<const MNode*> rows;
    collectNamed(table, "tr", rows);
    for (size_t r = 0; r < rows.size(); r++) {
        html += "<tr>";
        for (size_t c = 0; c < rows[r]->kids.size(); c++) {
            const MNode* cell = rows[r]->kids[c].get();
            if (cell->text) continue;
            if (cell->name != "td" && cell->name != "th") continue;
            std::string tag = cell->name == "th" ? "th" : "td";
            html += "<" + tag;
            std::string cs = cell->attr("colspan");
            std::string rs = cell->attr("rowspan");
            if (!cs.empty()) html += " colspan=\"" + escHtml(cs) + "\"";
            if (!rs.empty()) html += " rowspan=\"" + escHtml(rs) + "\"";
            html += ">";
            html += inlineHtmlOf(cell, ctx);
            html += "</" + tag + ">";
        }
        html += "</tr>";
    }
    return html + "</table>";
}

static void blockFromEl(const MNode* el, HtmlCtx& ctx, std::vector<Block>& out, int depth);

/* Картинки внутри контейнера без текста: <div align="center"><img/></div>,
   <p><img/></p>. Раньше такие блоки отбрасывались вместе с картинкой. */
static bool pushImagesFrom(const MNode* el, HtmlCtx& ctx, std::vector<Block>& out) {
    std::vector<const MNode*> imgs;
    collectNamed(el, "img", imgs);
    bool any = false;
    for (size_t i = 0; i < imgs.size(); i++) {
        std::string name = imageFromEl(imgs[i], ctx);
        if (name.empty()) continue;
        Block b(Block::IMG);
        b.src = "images/" + name;
        out.push_back(b);
        any = true;
    }
    return any;
}

static std::vector<Block> blocksOf(const MNode* el, HtmlCtx& ctx) {
    std::vector<Block> out;
    for (size_t i = 0; i < el->kids.size(); i++) {
        const MNode* ch = el->kids[i].get();
        if (ch->text) continue;
        blockFromEl(ch, ctx, out, 0);
    }
    return out;
}

static bool hasVisibleText(const std::string& html) {
    return !collapse(htmlToText(html)).empty();
}

static bool hasClassWord(const std::string& cls, const char* w) {
    if (cls.empty()) return false;
    std::string want = w;
    size_t pos = 0;
    while (pos <= cls.size()) {
        size_t sp = cls.find(' ', pos);
        if (sp == std::string::npos) sp = cls.size();
        if (cls.substr(pos, sp - pos) == want) return true;
        if (sp >= cls.size()) break;
        pos = sp + 1;
    }
    return false;
}

/* div/секция с особым смыслом — не «прозрачный» контейнер */
static bool specialContainer(const MNode* el) {
    std::string cls = lowerAscii(el->attr("class"));
    if (cls.empty()) return false;
    return hasClassWord(cls, "poem") || hasClassWord(cls, "stanza") ||
           hasClassWord(cls, "picture") || hasClassWord(cls, "cover") ||
           hasClassWord(cls, "titlepage") || hasClassWord(cls, "notes") ||
           hasClassWord(cls, "note");
}

static void blockFromEl(const MNode* el, HtmlCtx& ctx, std::vector<Block>& out, int depth) {
    if (depth > 60) return;
    if (ctx.skip && ctx.skip->count(el)) return;
    const std::string& t = el->name;
    std::string cls = lowerAscii(el->attr("class"));

    if (t == "p") {
        std::string h = inlineHtmlOf(el, ctx);
        if (hasClassWord(cls, "empty")) { out.push_back(Block::empty()); return; }
        if (!hasVisibleText(h)) { pushImagesFrom(el, ctx, out); return; }
        int kind = Block::P;
        if (hasClassWord(cls, "subtitle") || hasClassWord(cls, "sub")) kind = Block::SUB;
        else if (hasClassWord(cls, "author")) kind = Block::AUTHOR;
        else if (hasClassWord(cls, "v") || hasClassWord(cls, "verse")) kind = Block::V;
        else if (hasClassWord(cls, "li")) kind = Block::LI;
        Block b(kind);
        b.html = h;
        std::string id = el->attr("id");
        if (!id.empty()) b.id = id;
        out.push_back(b);
        return;
    }
    if (t == "br" || t == "hr") { out.push_back(Block::empty()); return; }
    if (t == "blockquote") {
        std::string kind = (hasClassWord(cls, "epigraph") || hasClassWord(cls, "epigraphs"))
                               ? "epigraph" : "cite";
        out.push_back(Block::quote(kind, blocksOf(el, ctx)));
        return;
    }
    if (t == "div" && hasClassWord(cls, "poem")) {
        out.push_back(Block::wrap(Block::POEM, blocksOf(el, ctx)));
        return;
    }
    if ((t == "div" || t == "section") && hasClassWord(cls, "stanza")) {
        out.push_back(Block::wrap(Block::STANZA, blocksOf(el, ctx)));
        return;
    }
    if ((t == "div" || t == "figure" || t == "p") &&
        (hasClassWord(cls, "picture") || hasClassWord(cls, "cover"))) {
        std::vector<const MNode*> imgs;
        collectNamed(el, "img", imgs);
        for (size_t i = 0; i < imgs.size(); i++) {
            std::string name = imageFromEl(imgs[i], ctx);
            if (name.empty()) continue;
            Block b(Block::IMG);
            b.src = "images/" + name;
            out.push_back(b);
        }
        return;
    }
    if (t == "pre") {
        std::string txt = el->textContent();
        while (!txt.empty() && (txt[txt.size() - 1] == '\n' || txt[txt.size() - 1] == '\r' ||
                                txt[txt.size() - 1] == ' ' || txt[txt.size() - 1] == '\t'))
            txt.erase(txt.size() - 1);
        Block b(Block::PRE);
        b.html = escHtml(txt);
        out.push_back(b);
        return;
    }
    if (t == "img") {
        std::string name = imageFromEl(el, ctx);
        if (!name.empty()) {
            Block b(Block::IMG);
            b.src = "images/" + name;
            out.push_back(b);
        }
        return;
    }
    if (t == "ul" || t == "ol" || t == "dl") {
        for (size_t i = 0; i < el->kids.size(); i++) {
            const MNode* li = el->kids[i].get();
            if (li->text) continue;
            if (li->name != "li" && li->name != "dt" && li->name != "dd") continue;
            if (ctx.skip && ctx.skip->count(li)) continue; /* пункт стал сноской */
            std::string h = inlineHtmlOf(li, ctx);
            if (hasVisibleText(h)) {
                Block b(Block::LI);
                b.html = h;
                out.push_back(b);
            }
        }
        return;
    }
    if (t == "table") {
        Block b(Block::RAW);
        b.html = cleanTableHtml(el, ctx);
        out.push_back(b);
        return;
    }
    if (t == "li") {
        std::string h = inlineHtmlOf(el, ctx);
        if (hasVisibleText(h)) {
            Block b(Block::LI);
            b.html = h;
            out.push_back(b);
        }
        return;
    }
    if (t == "div" || t == "section" || t == "article" || t == "main" || t == "body" ||
        t == "center" || t == "figure" || t == "figcaption" || t == "header" ||
        t == "footer" || t == "aside" || t == "td" || t == "th") {
        if (t == "li" || hasBlockChildren(el)) {
            for (size_t i = 0; i < el->kids.size(); i++) {
                const MNode* ch = el->kids[i].get();
                if (ch->text) continue;
                blockFromEl(ch, ctx, out, depth + 1);
            }
        } else {
            std::string h = inlineHtmlOf(el, ctx);
            if (hasVisibleText(h)) out.push_back(Block::p(h));
            else pushImagesFrom(el, ctx, out);
        }
        return;
    }
    if (t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6') {
        Block b(Block::SUB);
        b.html = inlineHtmlOf(el, ctx);
        out.push_back(b);
        return;
    }
    std::string h = inlineHtmlOf(el, ctx);
    if (hasVisibleText(h)) out.push_back(Block::p(h));
}

/* Заголовок без <h1>: <div class="title1">, <p class="title">, «chapter-title» — так пишут
   EPUB/HTML, сделанные из FB2. Возвращает уровень (title2 -> 1) или -1. */
static int titleClassLevel(const MNode* el) {
    if (el->name != "div" && el->name != "p") return -1;
    std::string cls = lowerAscii(el->attr("class"));
    if (cls.empty()) return -1;
    size_t pos = 0;
    int level = -1;
    while (pos <= cls.size() && level < 0) {
        size_t sp = cls.find(' ', pos);
        if (sp == std::string::npos) sp = cls.size();
        std::string w = cls.substr(pos, sp - pos);
        if (w == "title" || w == "chapter-title" || w == "chaptertitle" || w == "part-title" || w == "parttitle")
            level = 0;
        else if (w.size() == 6 && w.compare(0, 5, "title") == 0 && w[5] >= '0' && w[5] <= '9')
            level = std::max(0, w[5] - '1');
        pos = sp + 1;
    }
    if (level < 0) return -1;
    if (findNode(el, "h1") || findNode(el, "h2") || findNode(el, "h3")) return -1;
    if (collapse(el->textContent()).size() > 400) return -1; /* это уже не заголовок */
    return level;
}

/* normalize=false: уровни остаются «сырыми» (h1 = 0) — EPUB выравнивает их по оглавлению сам. */
static std::vector<Chapter> htmlToChapters(const MNode* root, HtmlCtx& ctx, bool normalize = true) {
    std::vector<Chapter> chapters;
    Chapter cur;
    cur.level = 0;
    chapters.push_back(cur);

    struct Walker {
        HtmlCtx& ctx;
        std::vector<Chapter>& chapters;
        Chapter* cur;
        int depth;
        void walk(const MNode* node) {
            if (depth > 60) return;
            depth++;
            for (size_t i = 0; i < node->kids.size(); i++) {
                const MNode* ch = node->kids[i].get();
                if (ch->text) continue;
                if (ctx.skip && ctx.skip->count(ch)) continue;
                const std::string& t = ch->name;
                if (t == "script" || t == "style" || t == "noscript" || t == "head" ||
                    t == "svg" || t == "nav")
                    continue;
                int titleLevel = titleClassLevel(ch);
                if (titleLevel >= 0) {
                    /* <div class="title1"><p>Глава</p><p>Название</p></div> из конвертеров FB2 */
                    std::string h;
                    std::vector<const MNode*> ps = ch->childrenNamed("p");
                    for (size_t k = 0; k < ps.size(); k++) {
                        std::string part = trim(inlineHtmlOf(ps[k], ctx));
                        if (part.empty()) continue;
                        if (!h.empty()) h += " ";
                        h += part;
                    }
                    if (h.empty()) h = trim(inlineHtmlOf(ch, ctx));
                    if (hasVisibleText(h)) {
                        Chapter c;
                        c.level = titleLevel;
                        c.title = h;
                        chapters.push_back(c);
                        cur = &chapters[chapters.size() - 1];
                    }
                    continue;
                }
                if (t.size() == 2 && t[0] == 'h' && t[1] >= '1' && t[1] <= '6') {
                    int level = std::max(0, (t[1] - '1'));
                    std::string h = inlineHtmlOf(ch, ctx);
                    if (hasVisibleText(h)) {
                        Chapter c;
                        c.level = level;
                        c.title = h;
                        chapters.push_back(c);
                        cur = &chapters[chapters.size() - 1];
                    }
                    continue;
                }
                bool container = (t == "div" || t == "section" || t == "article" ||
                                  t == "main" || t == "body" || t == "center" ||
                                  t == "figure" || t == "header" || t == "footer" ||
                                  t == "aside" || t == "html");
                if (container && !specialContainer(ch) && hasBlockChildren(ch)) {
                    walk(ch);
                    continue;
                }
                blockFromEl(ch, ctx, cur->blocks, 0);
            }
            depth--;
        }
    };
    Walker w = {ctx, chapters, &chapters[0], 0};
    w.walk(root);

    std::vector<Chapter> out;
    for (size_t i = 0; i < chapters.size(); i++) {
        bool has = !chapters[i].title.empty();
        for (size_t k = 0; k < chapters[i].blocks.size() && !has; k++)
            if (chapters[i].blocks[k].type != Block::EMPTY) has = true;
        if (has) out.push_back(chapters[i]);
    }
    if (!normalize) return out;

    /* самая верхняя глава всегда нулевого уровня */
    int minLvl = -1;
    for (size_t i = 0; i < out.size(); i++)
        if (!out[i].title.empty() && (minLvl < 0 || out[i].level < minLvl)) minLvl = out[i].level;
    if (minLvl > 0) {
        for (size_t i = 0; i < out.size(); i++)
            out[i].level = std::max(0, out[i].level - minLvl);
    }
    return out;
}

/* ===================== сноски в HTML, EPUB и MOBI ====================== */
/* Ссылка-сноска узнаётся по разметке (epub:type="noteref", role="doc-noteref", класс
   footnote/noteref) или по виду — короткий номер «1», «[1]», «*». Цель ссылки — абзац,
   пункт списка или блок с этим id (или <a name> внутри него) — становится сноской книги
   и из текста убирается. Так устроены сноски Word, pandoc, Calibre, Standard Ebooks. */

static void splitNoteNumber(std::string& html, std::string& title);
static bool isNotesTitle(const std::string& titleHtml);

struct NoteRefs {
    std::map<std::string, std::string> label; /* id цели -> номер, как он виден в тексте */
    std::set<std::string> refAnchors;         /* id/name самих ссылок: на них ведут обратные ссылки */
};

static std::string hrefFragment(const std::string& href) {
    size_t h = href.find('#');
    return h == std::string::npos ? std::string() : href.substr(h + 1);
}

/* «[12]» -> «12», «(*)» -> «*» */
static std::string noteLabelCore(const std::string& text) {
    std::string s = collapse(text);
    while (!s.empty() && strchr("[({", s[0])) s.erase(0, 1);
    while (!s.empty() && strchr("])}.", s[s.size() - 1])) s.erase(s.size() - 1);
    return trim(s);
}

static bool isNoteLabel(const std::string& text) {
    std::string s = noteLabelCore(text);
    if (s.empty() || s.size() > 4) return false;
    bool digits = true, stars = true;
    for (size_t i = 0; i < s.size(); i++) {
        if (!isdigit((unsigned char)s[i])) digits = false;
        if (s[i] != '*') stars = false;
    }
    return digits || stars;
}

/* Ссылки в порядке документа. Пара «ссылка <-> обратная ссылка» различается порядком:
   первой в тексте идёт ссылка, обратная ведёт на уже встреченную ссылку. */
static void collectNoteRefs(const MNode* n, NoteRefs& refs, int depth) {
    if (depth > 80) return;
    for (size_t i = 0; i < n->kids.size(); i++) {
        const MNode* k = n->kids[i].get();
        if (k->text) continue;
        if (k->name == "a") {
            std::string frag = hrefFragment(k->attr("href"));
            std::string own = k->attr("id");
            if (own.empty()) own = k->attr("name");
            std::string kind = lowerAscii(k->attr("epub:type") + " " + k->attr("role") + " " + k->attr("class"));
            bool back = kind.find("backlink") != std::string::npos || kind.find("footnote-back") != std::string::npos;
            bool marked = kind.find("noteref") != std::string::npos || kind.find("footnote") != std::string::npos ||
                          kind.find("endnote") != std::string::npos;
            if (!frag.empty() && !back && (marked || isNoteLabel(k->textContent())) &&
                !refs.refAnchors.count(frag) && !(own.size() && refs.label.count(own))) {
                if (!refs.label.count(frag)) refs.label[frag] = noteLabelCore(k->textContent());
                if (!own.empty()) refs.refAnchors.insert(own);
            }
        }
        collectNoteRefs(k, refs, depth + 1);
    }
}

static bool isNoteBoxTag(const std::string& t) {
    return t == "p" || t == "li" || t == "div" || t == "aside" || t == "section" || t == "dd" ||
           t == "blockquote" || t == "td";
}

/* в сноску не попадает то, что сноской быть не может: целая глава с заголовками */
static bool noteBoxOk(const MNode* box) {
    if (box->name == "body" || box->name == "html") return false;
    if (findNode(box, "h1") || findNode(box, "h2") || findNode(box, "h3") || findNode(box, "h4")) return false;
    return collapse(box->textContent()).size() < 6000;
}

/* убрать из текста сноски обратные ссылки (↩, «наверх», ссылки на сами номера в тексте) */
static std::string dropBackLinks(const std::string& html, const NoteRefs& refs) {
    std::string out;
    size_t pos = 0;
    for (;;) {
        size_t a = html.find("<a href=\"", pos);
        if (a == std::string::npos) { out.append(html, pos, std::string::npos); break; }
        size_t q = html.find('"', a + 9);
        size_t gt = q == std::string::npos ? q : html.find('>', q);
        size_t end = gt == std::string::npos ? gt : html.find("</a>", gt);
        if (end == std::string::npos) { out.append(html, pos, std::string::npos); break; }
        std::string frag = hrefFragment(html.substr(a + 9, q - a - 9));
        std::string inner = collapse(htmlToText(html.substr(gt + 1, end - gt - 1)));
        /* 1.8: «←1» — обратная ссылка в сносках MOBI от Calibre; раньше она оставалась и
           сноска состояла из одной стрелки */
        bool back = refs.refAnchors.count(frag) || inner == "\xE2\x86\xA9" || inner == "\xE2\x86\xA9\xEF\xB8\x8E" ||
                    inner == "^" || inner == "\xE2\x86\x91" || toLowerRu(inner) == "назад" || toLowerRu(inner) == "back" ||
                    startsWith(inner, "\xE2\x86\x90");
        out.append(html, pos, a - pos);
        if (!back) out.append(html, a, end + 4 - a);
        pos = end + 4;
    }
    out = replaceAll(out, "<sup></sup>", "");
    /* скобки, в которых стояла обратная ссылка: «[←1]» -> «[]» */
    std::string t = trim(out);
    if (t == "[]" || t == "()" || t == "[ ]") return std::string();
    return t;
}

/* номер в начале сноски: «<a …>1</a> Текст», «[1] Текст», «1. Текст» — в заголовок сноски */
static void takeLeadingLabel(std::string& html, std::string& title, const std::string& label) {
    splitNoteNumber(html, title);
    if (!title.empty()) return;
    if (startsWith(html, "<a ")) {
        size_t gt = html.find('>');
        size_t end = gt == std::string::npos ? gt : html.find("</a>", gt);
        if (end != std::string::npos && isNoteLabel(htmlToText(html.substr(gt + 1, end - gt - 1)))) {
            title = noteLabelCore(htmlToText(html.substr(gt + 1, end - gt - 1)));
            html = trim(html.substr(end + 4));
            return;
        }
    }
    if (label.empty()) return;
    const char* forms[] = {"[%s]", "%s.", "%s)", "%s"};
    for (int i = 0; i < 4; i++) {
        std::string f = forms[i];
        f.replace(f.find("%s"), 2, label);
        if (startsWith(html, f.c_str()) && html.size() > f.size() && (html[f.size()] == ' ' || html[f.size()] == '<')) {
            title = label;
            html = trim(html.substr(f.size()));
            return;
        }
    }
}

static void extractNotesRec(const MNode* n, std::vector<const MNode*>& path, const NoteRefs& refs,
                            HtmlCtx& ctx, std::set<const MNode*>& skip, std::set<std::string>& taken,
                            int depth) {
    if (depth > 80) return;
    for (size_t i = 0; i < n->kids.size(); i++) {
        const MNode* k = n->kids[i].get();
        if (k->text || skip.count(k)) continue;
        std::string id = k->attr("id");
        if (id.empty() && k->name == "a") id = k->attr("name");
        std::map<std::string, std::string>::const_iterator lab = refs.label.find(id);
        if (!id.empty() && lab != refs.label.end() && !taken.count(id)) {
            const MNode* box = isNoteBoxTag(k->name) ? k : NULL;
            for (size_t j = path.size(); !box && j-- > 0;)
                if (isNoteBoxTag(path[j]->name)) box = path[j];
            if (box && !skip.count(box) && noteBoxOk(box)) {
                Note note;
                note.id = id;
                std::vector<Block> bl;
                if (box->name == "p") bl.push_back(Block::p(inlineHtmlOf(box, ctx)));
                else bl = blocksOf(box, ctx);
                bool first = true;
                for (size_t b = 0; b < bl.size(); b++) {
                    bl[b].html = dropBackLinks(bl[b].html, refs);
                    if (first && !bl[b].html.empty()) {
                        takeLeadingLabel(bl[b].html, note.title, lab->second);
                        first = false;
                    }
                    if (bl[b].type == Block::IMG || hasVisibleText(bl[b].html) || !bl[b].blocks.empty())
                        note.blocks.push_back(bl[b]);
                }
                /* 1.8: якорь сноски стоит на строке с одной обратной ссылкой, а текст идёт
                   следующими блоками (так пишет Calibre: «[←1]», затем <blockquote> с текстом).
                   Раньше в сноску попадала только стрелка, а текст оставался в основном тексте.
                   Берём следующие блоки до разрыва страницы, заголовка или следующей сноски. */
                if (note.blocks.empty() && box == k) {
                    size_t taken2 = 0;
                    for (size_t s = i + 1; s < n->kids.size() && taken2 < 20; s++) {
                        const MNode* sib = n->kids[s].get();
                        if (sib->text) {
                            if (collapse(sib->value).empty()) continue;
                            break;
                        }
                        const std::string& sn = sib->name;
                        if (sn == "mbp:pagebreak" || (sn.size() == 2 && sn[0] == 'h' && isdigit((unsigned char)sn[1])))
                            break;
                        std::string sid = sib->attr("id");
                        if (!sid.empty() && refs.label.count(sid)) break;
                        if (!noteBoxOk(sib)) break;
                        std::vector<Block> more;
                        if (sib->name == "p") more.push_back(Block::p(inlineHtmlOf(sib, ctx)));
                        else more = blocksOf(sib, ctx);
                        /* отступ <blockquote> у Calibre — оформление, а не цитата: раскрываем */
                        for (size_t b = 0; b < more.size(); b++) {
                            if (more[b].type == Block::QUOTE && !more[b].blocks.empty()) {
                                std::vector<Block> inner = more[b].blocks;
                                more.erase(more.begin() + b);
                                more.insert(more.begin() + b, inner.begin(), inner.end());
                                b--;
                            }
                        }
                        for (size_t b = 0; b < more.size(); b++) {
                            more[b].html = dropBackLinks(more[b].html, refs);
                            if (more[b].type == Block::IMG || hasVisibleText(more[b].html) || !more[b].blocks.empty())
                                note.blocks.push_back(more[b]);
                        }
                        skip.insert(sib);
                        taken2++;
                    }
                }
                if (note.title.empty()) note.title = lab->second;
                taken.insert(id);
                skip.insert(box);
                if (!note.blocks.empty() && ctx.book) ctx.book->notes.push_back(note);
                if (box != k) return; /* ячейка-родитель занята — дальше в ней искать нечего */
                continue;
            }
        }
        path.push_back(k);
        extractNotesRec(k, path, refs, ctx, skip, taken, depth + 1);
        path.pop_back();
    }
}

static void extractNotes(const MNode* root, const NoteRefs& refs, HtmlCtx& ctx,
                         std::set<const MNode*>& skip, std::set<std::string>& taken) {
    if (refs.label.empty()) return;
    std::vector<const MNode*> path;
    path.push_back(root);
    extractNotesRec(root, path, refs, ctx, skip, taken, 0);
}

/* «chapter-1.xhtml#note-1» -> «#note-1», если note-1 — сноска книги */
static void pointLinksToNotes(std::vector<Block>& blocks, const std::set<std::string>& ids) {
    for (size_t i = 0; i < blocks.size(); i++) {
        std::string& h = blocks[i].html;
        size_t pos = 0;
        while ((pos = h.find("href=\"", pos)) != std::string::npos) {
            size_t q = h.find('"', pos + 6);
            if (q == std::string::npos) break;
            std::string href = h.substr(pos + 6, q - pos - 6);
            std::string frag = hrefFragment(href);
            if (!frag.empty() && href[0] != '#' && ids.count(frag)) {
                h.replace(pos + 6, q - pos - 6, "#" + frag);
                q = pos + 7 + frag.size();
            }
            pos = q;
        }
        pointLinksToNotes(blocks[i].blocks, ids);
    }
}

/* общая доводка после чтения HTML-подобного формата */
static void finishHtmlBook(Book& book) {
    if (!book.notes.empty()) {
        std::set<std::string> ids;
        for (size_t i = 0; i < book.notes.size(); i++) ids.insert(book.notes[i].id);
        for (size_t i = 0; i < book.chapters.size(); i++) pointLinksToNotes(book.chapters[i].blocks, ids);
        /* глава «Примечания», из которой всё ушло в сноски */
        for (size_t i = book.chapters.size(); i-- > 0;) {
            Chapter& c = book.chapters[i];
            if (!isNotesTitle(c.title)) continue;
            bool has = false;
            for (size_t k = 0; k < c.blocks.size() && !has; k++)
                if (c.blocks[k].type == Block::IMG || hasVisibleText(c.blocks[k].html) || !c.blocks[k].blocks.empty())
                    has = true;
            if (!has) book.chapters.erase(book.chapters.begin() + i);
        }
        /* 1.8: заголовок раздела сносок абзацем в конце главы («Заметки» в MOBI от Calibre) —
           сами сноски ушли в сноски книги, от раздела остался одинокий заголовок */
        for (size_t i = 0; i < book.chapters.size(); i++) {
            std::vector<Block>& bl = book.chapters[i].blocks;
            while (!bl.empty() && (bl.back().type == Block::EMPTY ||
                                   (bl.back().type == Block::P && !hasVisibleText(bl.back().html))))
                bl.pop_back();
            if (!bl.empty() && bl.back().type == Block::P && isNotesTitle(bl.back().html)) bl.pop_back();
        }
    }
    /* единственный заголовок верхнего уровня, совпадающий с названием книги, — это название,
       а не раздел: «<h1>Книга</h1><h2>Глава 1</h2>…» */
    std::string want = toLowerRu(collapse(htmlToText(escHtml(book.meta.title))));
    int top = -1, topCount = 0, titled = 0;
    for (size_t i = 0; i < book.chapters.size(); i++) {
        if (book.chapters[i].title.empty()) continue;
        titled++;
        if (top < 0 || book.chapters[i].level < book.chapters[top].level) { top = (int)i; topCount = 0; }
        if (book.chapters[i].level == book.chapters[top].level) topCount++;
    }
    if (top >= 0 && topCount == 1 && titled > 1 && !want.empty() &&
        toLowerRu(collapse(htmlToText(book.chapters[top].title))) == want) {
        book.chapters[top].title.clear();
        if (book.chapters[top].blocks.empty()) book.chapters.erase(book.chapters.begin() + top);
    }
    /* 1.8: первый абзац книги, повторяющий её название (титул, который Calibre пишет в начало
       MOBI крупным шрифтом), — это не текст: название уже есть в описании книги */
    for (size_t i = 0; i < book.chapters.size() && !want.empty(); i++) {
        std::vector<Block>& bl = book.chapters[i].blocks;
        if (!book.chapters[i].title.empty() && i > 0) break;
        size_t k = 0;
        while (k < bl.size() && bl[k].type == Block::EMPTY) k++;
        if (k < bl.size() && bl[k].type == Block::P &&
            toLowerRu(collapse(htmlToText(bl[k].html))) == want) {
            bl.erase(bl.begin() + (long)k);
            if (bl.empty() && book.chapters[i].title.empty()) book.chapters.erase(book.chapters.begin() + (long)i);
        }
        break;
    }
    normalizeChapterLevels(book.chapters);
}

/* ================================= FB2 ================================= */

struct Fb2Ctx {
    std::map<std::string, std::string> imgMap;
};

static std::string fb2Inline(const MNode* node, Fb2Ctx& ctx);

static std::string fb2ImageName(const MNode* el, Fb2Ctx& ctx) {
    std::string href = el->attr("l:href");
    if (href.empty()) href = el->attr("href");
    if (href.empty() || href[0] != '#') return std::string();
    std::string id = href.substr(1);
    std::map<std::string, std::string>::iterator it = ctx.imgMap.find(id);
    return it == ctx.imgMap.end() ? std::string() : it->second;
}

static std::string fb2Inline(const MNode* node, Fb2Ctx& ctx) {
    std::string s;
    for (size_t i = 0; i < node->kids.size(); i++) {
        const MNode* ch = node->kids[i].get();
        if (ch->text) { s += escXml(ch->value); continue; }
        const std::string& t = ch->name;
        std::string inner = fb2Inline(ch, ctx);
        if (t == "strong") s += "<strong>" + inner + "</strong>";
        else if (t == "emphasis") s += "<em>" + inner + "</em>";
        else if (t == "strikethrough") s += "<s>" + inner + "</s>";
        else if (t == "sub") s += "<sub>" + inner + "</sub>";
        else if (t == "sup") s += "<sup>" + inner + "</sup>";
        else if (t == "code") s += "<code>" + inner + "</code>";
        else if (t == "style") s += inner;
        else if (t == "a") {
            std::string href = ch->attr("l:href");
            if (href.empty()) href = ch->attr("href");
            if (!href.empty() && lowerAscii(href).compare(0, 11, "javascript:") != 0)
                s += "<a href=\"" + escXml(href) + "\">" + inner + "</a>";
            else
                s += inner;
        } else if (t == "image") {
            std::string name = fb2ImageName(ch, ctx);
            if (!name.empty()) s += "<img src=\"images/" + escHtml(name) + "\" alt=\"\"/>";
        } else {
            s += inner;
        }
    }
    return s;
}

static std::string fb2TitleHtml(const MNode* el, Fb2Ctx& ctx) {
    std::vector<std::string> parts;
    for (size_t i = 0; i < el->kids.size(); i++) {
        const MNode* ch = el->kids[i].get();
        if (ch->text) continue;
        if (ch->name == "p" || ch->name == "subtitle") {
            std::string h = trim(fb2Inline(ch, ctx));
            if (!h.empty()) parts.push_back(h);
        }
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += " — ";
        out += parts[i];
    }
    return out;
}

static std::string fb2Table(const MNode* el, Fb2Ctx& ctx) {
    std::string html = "<table>";
    for (size_t i = 0; i < el->kids.size(); i++) {
        const MNode* row = el->kids[i].get();
        if (row->text || row->name != "tr") continue;
        html += "<tr>";
        for (size_t k = 0; k < row->kids.size(); k++) {
            const MNode* cell = row->kids[k].get();
            if (cell->text) continue;
            std::string tag = cell->name == "th" ? "th" : "td";
            html += "<" + tag;
            std::string cs = cell->attr("colspan");
            std::string rs = cell->attr("rowspan");
            if (!cs.empty()) html += " colspan=\"" + escHtml(cs) + "\"";
            if (!rs.empty()) html += " rowspan=\"" + escHtml(rs) + "\"";
            html += ">" + fb2Inline(cell, ctx) + "</" + tag + ">";
        }
        html += "</tr>";
    }
    return html + "</table>";
}

static void fb2BlocksOf(const MNode* node, Fb2Ctx& ctx, std::vector<Block>& out);

static std::vector<Block> fb2BlocksVec(const MNode* node, Fb2Ctx& ctx) {
    std::vector<Block> tmp;
    fb2BlocksOf(node, ctx, tmp);
    return tmp;
}

static void fb2Block(const MNode* node, Fb2Ctx& ctx, std::vector<Block>& out) {
    const std::string& t = node->name;
    if (t == "p") {
        std::string h = fb2Inline(node, ctx);
        if (!trim(h).empty()) out.push_back(Block::p(h));
    } else if (t == "subtitle") {
        std::string h = fb2Inline(node, ctx);
        if (!trim(h).empty()) out.push_back(Block::sub(h));
    } else if (t == "empty-line") {
        out.push_back(Block::empty());
    } else if (t == "image") {
        std::string name = fb2ImageName(node, ctx);
        if (!name.empty()) {
            Block b(Block::IMG);
            b.src = "images/" + name;
            out.push_back(b);
        }
    } else if (t == "epigraph") {
        out.push_back(Block::quote("epigraph", fb2BlocksVec(node, ctx)));
    } else if (t == "cite") {
        out.push_back(Block::quote("cite", fb2BlocksVec(node, ctx)));
    } else if (t == "poem") {
        out.push_back(Block::wrap(Block::POEM, fb2BlocksVec(node, ctx)));
    } else if (t == "stanza") {
        out.push_back(Block::wrap(Block::STANZA, fb2BlocksVec(node, ctx)));
    } else if (t == "v") {
        Block b(Block::V);
        b.html = fb2Inline(node, ctx);
        out.push_back(b);
    } else if (t == "text-author") {
        Block b(Block::AUTHOR);
        b.html = fb2Inline(node, ctx);
        out.push_back(b);
    } else if (t == "annotation") {
        fb2BlocksOf(node, ctx, out);
    } else if (t == "table") {
        Block b(Block::RAW);
        b.html = fb2Table(node, ctx);
        out.push_back(b);
    } else if (t == "title") {
        /* заголовок внутри poem/cite: разделы и тело фильтруют <title> сами */
        std::string h = fb2TitleHtml(node, ctx);
        if (!h.empty()) {
            Block b(Block::SUB);
            b.html = h;
            out.push_back(b);
        }
    } else if (t == "section") {
        /* пропускаем: разделы обрабатываются в fb2Sections */
    } else {
        std::string h = fb2Inline(node, ctx);
        if (!trim(h).empty()) out.push_back(Block::p(h));
    }
}

static void fb2BlocksOf(const MNode* node, Fb2Ctx& ctx, std::vector<Block>& out) {
    for (size_t i = 0; i < node->kids.size(); i++) {
        const MNode* ch = node->kids[i].get();
        if (ch->text) continue;
        fb2Block(ch, ctx, out);
    }
}

static void fb2Sections(const MNode* parent, int depth, Fb2Ctx& ctx, Book& book) {
    for (size_t i = 0; i < parent->kids.size(); i++) {
        const MNode* sec = parent->kids[i].get();
        if (sec->text || sec->name != "section") continue;
        const MNode* titleEl = NULL;
        for (size_t k = 0; k < sec->kids.size(); k++) {
            const MNode* ch = sec->kids[k].get();
            if (!ch->text && ch->name == "title") { titleEl = ch; break; }
        }
        Chapter c;
        c.level = depth;
        c.title = titleEl ? fb2TitleHtml(titleEl, ctx) : std::string();
        for (size_t k = 0; k < sec->kids.size(); k++) {
            const MNode* ch = sec->kids[k].get();
            if (ch->text) continue;
            if (ch->name == "section" || ch->name == "title") continue;
            fb2Block(ch, ctx, c.blocks);
        }
        book.chapters.push_back(c);
        fb2Sections(sec, c.title.empty() ? depth : depth + 1, ctx, book);
    }
}

static bool readFB2(const Bytes& data, const std::string& fileName, Book& book, std::string* err) {
    std::string text = decodeText(data, NULL);
    MNode doc = parseMarkup(text, false);
    const MNode* root = NULL;
    for (size_t i = 0; i < doc.kids.size(); i++) {
        if (!doc.kids[i]->text) { root = doc.kids[i].get(); break; }
    }
    if (!root) {
        if (err) *err = "Пустой файл FB2";
        return false;
    }
    if (root->name != "fictionbook") {
        if (err) *err = "Это не FB2: корневой элемент <" + root->name + ">";
        return false;
    }
    book.fileName = fileName;
    Fb2Ctx ctx;

    std::vector<const MNode*> binaries = allNamed(root, "binary");
    int imgN = 0;
    for (size_t i = 0; i < binaries.size(); i++) {
        std::string id = binaries[i]->attr("id");
        if (id.empty()) continue;
        std::string mime = lowerAscii(binaries[i]->attr("content-type"));
        if (mime.empty()) mime = "image/jpeg";
        Bytes data2 = b64decode(binaries[i]->textContent());
        if (data2.empty()) continue;
        imgN++;
        std::string ext = extFromMime(mime);
        std::string name = "img-" + padNum(imgN, 3) + "." + ext;
        Image img;
        img.name = name;
        img.mime = mime;
        img.bytes = data2;
        book.images[name] = img;
        ctx.imgMap[id] = name;
    }

    const MNode* desc = findFirst(root, "description");
    const MNode* ti = NULL;
    if (desc) {
        for (size_t i = 0; i < desc->kids.size(); i++) {
            if (!desc->kids[i]->text && desc->kids[i]->name == "title-info") { ti = desc->kids[i].get(); break; }
        }
    }
    if (ti) {
        for (size_t i = 0; i < ti->kids.size(); i++) {
            const MNode* a = ti->kids[i].get();
            if (a->text) continue;
            const std::string& t = a->name;
            if (t == "book-title") {
                book.meta.title = elText(a);
            } else if (t == "author") {
                Author au;
                au.first = elText(findFirst(a, "first-name"));
                au.middle = elText(findFirst(a, "middle-name"));
                au.last = elText(findFirst(a, "last-name"));
                au.nick = elText(findFirst(a, "nickname"));
                if (!fullName(au).empty()) book.meta.authors.push_back(au);
            } else if (t == "genre") {
                std::string g = elText(a);
                if (!g.empty()) book.meta.genres.push_back(g);
            } else if (t == "annotation") {
                std::vector<Block> bl;
                fb2BlocksOf(a, ctx, bl);
                std::string ann;
                for (size_t k = 0; k < bl.size(); k++) {
                    if (bl[k].type != Block::P) continue;
                    if (!trim(bl[k].html).empty()) ann += "<p>" + bl[k].html + "</p>";
                }
                book.meta.annotation = ann;
            } else if (t == "keywords") {
                book.meta.keywords = elText(a);
            } else if (t == "date") {
                std::string v = a->attr("value");
                if (v.empty()) v = elText(a);
                if (v.size() > 10) v = v.substr(0, 10);
                book.meta.year = v;
            } else if (t == "lang") {
                std::string v = elText(a);
                book.meta.lang = v.empty() ? "ru" : v;
            } else if (t == "src-lang") {
                book.meta.srcLang = elText(a);
            } else if (t == "translator") {
                Author au;
                au.first = elText(findFirst(a, "first-name"));
                au.middle = elText(findFirst(a, "middle-name"));
                au.last = elText(findFirst(a, "last-name"));
                au.nick = elText(findFirst(a, "nickname"));
                book.meta.translators.push_back(au);
            } else if (t == "sequence") {
                book.meta.seriesName = collapse(a->attr("name"));
                book.meta.seriesNum = collapse(a->attr("number"));
            } else if (t == "coverpage") {
                std::vector<const MNode*> imgs;
                collectNamed(a, "image", imgs);
                if (!imgs.empty()) {
                    std::string href = imgs[0]->attr("l:href");
                    if (href.empty()) href = imgs[0]->attr("href");
                    if (!href.empty() && href[0] == '#') {
                        std::map<std::string, std::string>::iterator it = ctx.imgMap.find(href.substr(1));
                        if (it != ctx.imgMap.end()) book.coverName = it->second;
                    }
                }
            }
        }
    }
    if (desc) {
        const MNode* pi = NULL;
        const MNode* di = NULL;
        for (size_t i = 0; i < desc->kids.size(); i++) {
            if (desc->kids[i]->text) continue;
            if (desc->kids[i]->name == "publish-info") pi = desc->kids[i].get();
            if (desc->kids[i]->name == "document-info") di = desc->kids[i].get();
        }
        if (pi) {
            book.meta.publisher = elText(findFirst(pi, "publisher"));
            book.meta.city = elText(findFirst(pi, "city"));
            book.meta.isbn = elText(findFirst(pi, "isbn"));
            if (book.meta.year.empty()) book.meta.year = elText(findFirst(pi, "year"));
        }
        if (di) {
            std::string id = elText(findFirst(di, "id"));
            (void)id;
        }
    }

    for (size_t i = 0; i < root->kids.size(); i++) {
        const MNode* body = root->kids[i].get();
        if (body->text || body->name != "body") continue;
        std::string bname = body->attr("name");
        if (bname == "notes") {
            for (size_t k = 0; k < body->kids.size(); k++) {
                const MNode* sec = body->kids[k].get();
                if (sec->text || sec->name != "section") continue;
                Note note;
                note.id = sec->attr("id");
                const MNode* titleEl = NULL;
                for (size_t j = 0; j < sec->kids.size(); j++) {
                    if (!sec->kids[j]->text && sec->kids[j]->name == "title") {
                        titleEl = sec->kids[j].get();
                        break;
                    }
                }
                note.title = titleEl ? fb2TitleHtml(titleEl, ctx) : std::string();
                for (size_t j = 0; j < sec->kids.size(); j++) {
                    const MNode* ch = sec->kids[j].get();
                    if (ch->text || ch->name == "title") continue;
                    fb2Block(ch, ctx, note.blocks);
                }
                book.notes.push_back(note);
            }
        } else {
            std::vector<Block> lead;
            for (size_t k = 0; k < body->kids.size(); k++) {
                const MNode* ch = body->kids[k].get();
                if (ch->text || ch->name == "section" || ch->name == "title") continue;
                fb2Block(ch, ctx, lead);
            }
            if (!lead.empty()) {
                Chapter c;
                c.level = 0;
                c.blocks = lead;
                book.chapters.push_back(c);
            }
            fb2Sections(body, 0, ctx, book);
        }
    }

    if (book.chapters.empty() && book.notes.empty()) {
        if (err) *err = "В файле нет текста";
        return false;
    }
    return true;
}

/* ================================= TXT ================================= */

static bool isTxtBreak(const std::string& s) {
    /* "***", "---", "- - -", «— — —» и похожие разделители */
    std::string t = trim(s);
    if (t.empty()) return false;
    std::string u;
    for (size_t i = 0; i < t.size(); i++)
        if (t[i] != ' ' && t[i] != '\t') u += t[i];
    if (u.empty()) return false;

    bool onlyStars = true;
    int stars = 0;
    for (size_t i = 0; i < u.size(); i++) {
        if (u[i] == '*') stars++;
        else { onlyStars = false; break; }
    }
    if (onlyStars && stars >= 3) return true;

    /* три и более тире/подчёркивания/юникодных тире */
    std::string v = replaceAll(replaceAll(replaceAll(replaceAll(u,
        "\xE2\x80\x94", "-"), "\xE2\x80\x93", "-"), "\xE2\x80\x92", "-"), "\xE2\x80\x95", "-");
    int dashes = 0;
    bool onlyDashes = true;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == '-' || v[i] == '_') dashes++;
        else { onlyDashes = false; break; }
    }
    return onlyDashes && dashes >= 3;
}

/* «1», «IV», «первая», «третий», «one», «first» — то, что идёт после «Часть/Книга/Том»;
   за номером может следовать название: «Часть 2. Возвращение». */
static bool txtIsOrdinal(const std::string& rest) {
    if (rest.empty()) return false;
    size_t e = 0;
    while (e < rest.size() && rest[e] != ' ' && rest[e] != '.' && rest[e] != ',' && rest[e] != ':') e++;
    std::string w = rest.substr(0, e);
    if (w.empty()) return false;
    bool digits = true, roman = true;
    for (size_t i = 0; i < w.size(); i++) {
        if (!isdigit((unsigned char)w[i])) digits = false;
        if (strchr("ivxlc", w[i]) == NULL) roman = false;
    }
    if ((digits && w.size() <= 3) || (roman && w.size() <= 6)) return true;
    const char* stems[] = {"перв", "втор", "трет", "четв", "пят", "шест", "седьм", "восьм", "девят",
                           "десят", "одиннадцат", "двенадцат", "последн", "заключит",
                           "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                           "first", "second", "third", "fourth", "fifth", "last", NULL};
    for (int i = 0; stems[i]; i++)
        if (w.compare(0, strlen(stems[i]), stems[i]) == 0) return true;
    return false;
}

static bool txtHeadingLevel(const std::string& line, int* level, std::string* text) {
    std::string s = trim(line);
    if (s.empty() || utf8Len(s) > 110) return false;

    /* # Заголовок */
    if (s[0] == '#') {
        size_t hashes = 0;
        while (hashes < s.size() && s[hashes] == '#') hashes++;
        if (hashes >= 1 && hashes <= 6 && hashes < s.size() && s[hashes] == ' ') {
            std::string t = trim(s.substr(hashes));
            if (!t.empty()) {
                *level = (int)hashes - 1;
                *text = t;
                return true;
            }
        }
    }

    std::string low = toLowerRu(s);
    /* «Часть/Книга/Том» — только с номером: «Книга первая», «Часть II», «Том 3».
       Иначе «Книга из Word» или «Часть людей ушла» тоже стали бы разделами. */
    const char* partWords[] = {"часть", "книга", "том", "part", "book", "volume", NULL};
    for (int i = 0; partWords[i]; i++) {
        std::string w = partWords[i];
        if (low.compare(0, w.size(), w) == 0 && low.size() > w.size() + 1 && low[w.size()] == ' ' &&
            txtIsOrdinal(trim(low.substr(w.size() + 1)))) {
            *level = 0;
            *text = s;
            return true;
        }
    }
    const char* chapWords[] = {"глава", "пролог", "эпилог", "введение", "предисловие",
                               "послесловие", "приложение", "интермедия", "chapter",
                               "prologue", "epilogue", "introduction", "appendix", NULL};
    for (int i = 0; chapWords[i]; i++) {
        std::string w = chapWords[i];
        if (low.compare(0, w.size(), w) == 0 &&
            (low.size() == w.size() || low[w.size()] == ' ' || low[w.size()] == '.')) {
            *level = 1;
            *text = s;
            return true;
        }
    }
    /* СТРОКА ЦЕЛИКОМ КАПСОМ */
    if (utf8Len(s) <= 70) {
        bool hasUpper = false, hasLower = false, hasDigitsRun = false;
        /* проход по кодпойнтам */
        size_t idx = 0;
        int digitRun = 0;
        for (;;) {
            unsigned cp = utf8CharAt(s, idx);
            if (!cp) break;
            idx++;
            if (cp >= 'A' && cp <= 'Z') hasUpper = true;
            else if (cp >= 'a' && cp <= 'z') hasLower = true;
            else if ((cp >= 0x410 && cp <= 0x42F) || (cp >= 0x400 && cp <= 0x40F)) hasUpper = true; /* А–Я, Ё, Ђ… */
            else if ((cp >= 0x430 && cp <= 0x44F) || (cp >= 0x450 && cp <= 0x45F)) hasLower = true; /* а–я, ё… */
            if (cp >= '0' && cp <= '9') { digitRun++; if (digitRun >= 3) hasDigitsRun = true; }
            else digitRun = 0;
        }
        if (hasUpper && !hasLower && !hasDigitsRun) {
            char last = s[s.size() - 1];
            if (last != '.' && last != ',' && last != ';' && last != ':' && last != '!' &&
                last != '?' && last != ')') {
                *level = 1;
                *text = s;
                return true;
            }
        }
    }
    return false;
}

static bool txtEndsSentence(const std::string& s) {
    if (utf8Len(s) < 20) return false;
    std::string t = trim(s);
    if (t.empty()) return false;
    const char* ends[] = {".", "!", "?", "…", ":", ";", "»", "\"", ")", "]", NULL};
    for (int i = 0; ends[i]; i++) {
        std::string e = ends[i];
        if (t.size() >= e.size() && t.compare(t.size() - e.size(), e.size(), e) == 0) return true;
    }
    return false;
}

static std::string txtJoinLines(const std::vector<std::string>& lines) {
    std::vector<std::string> parts;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string L = trim(lines[i]);
        if (L.empty()) continue;
        if (!parts.empty()) {
            std::string& prev = parts[parts.size() - 1];
            if (!prev.empty() && (prev[prev.size() - 1] == '-' ||
                                  (unsigned char)prev[prev.size() - 1] == 0xAD)) {
                unsigned first = utf8CharAt(L, 0);
                bool lower = (first >= 'a' && first <= 'z') || (first >= 0x430 && first <= 0x44F) ||
                             first == 0x451;
                if (lower) {
                    prev.erase(prev.size() - 1);
                    prev += L;
                    continue;
                }
            }
        }
        parts.push_back(L);
    }
    std::string joined;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) joined += ' ';
        joined += parts[i];
    }
    return collapse(joined);
}

static bool readTXT(const Bytes& data, const std::string& fileName, Book& book, std::string* err) {
    std::string text = decodeText(data, NULL);
    text = replaceAll(text, "\r\n", "\n");
    text = replaceAll(text, "\r", "\n");
    text = replaceAll(text, "\xC2\xA0", " ");
    book.fileName = fileName;

    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        lines.push_back(text.substr(pos, nl - pos));
        if (nl == text.size()) break;
        pos = nl + 1;
    }

    Chapter cur;
    cur.level = 0;
    book.chapters.push_back(cur);
    std::vector<std::string> group;

    struct Flusher {
        std::vector<std::string>& group;
        Chapter* cur;
        void flush() {
            if (group.empty()) return;
            std::string joined = txtJoinLines(group);
            if (!joined.empty()) cur->blocks.push_back(Block::p(escHtml(joined)));
            group.clear();
        }
    };
    Flusher fl = {group, &book.chapters[0]};

    for (size_t i = 0; i < lines.size(); i++) {
        std::string s = trim(lines[i]);
        if (s.empty()) { fl.flush(); continue; }
        if (isTxtBreak(s)) {
            fl.flush();
            fl.cur->blocks.push_back(Block::empty());
            continue;
        }
        int level = 0;
        std::string htext;
        if (txtHeadingLevel(s, &level, &htext)) {
            fl.flush();
            Chapter c;
            c.level = level;
            c.title = escHtml(collapse(htext));
            book.chapters.push_back(c);
            fl.cur = &book.chapters[book.chapters.size() - 1];
            continue;
        }
        if (!group.empty()) {
            std::string prev = trim(group[group.size() - 1]);
            if (txtEndsSentence(prev)) {
                unsigned first = utf8CharAt(s, 0);
                bool lower = (first >= 'a' && first <= 'z') || (first >= 0x430 && first <= 0x44F) ||
                             first == 0x451;
                if (!lower) fl.flush();
            }
        }
        group.push_back(lines[i]);
    }
    fl.flush();

    /* «Пролог», «Эпилог», «Послесловие» — вровень с частями, если книга делится на части */
    bool hasParts = false;
    for (size_t i = 0; i < book.chapters.size(); i++)
        if (book.chapters[i].level == 0 && !book.chapters[i].title.empty()) hasParts = true;
    if (hasParts) {
        const char* outer[] = {"пролог", "эпилог", "предисловие", "послесловие", "введение", "заключение",
                               "от автора", "приложение", "prologue", "epilogue", "foreword", "afterword",
                               "introduction", "appendix", NULL};
        for (size_t i = 0; i < book.chapters.size(); i++) {
            std::string low = toLowerRu(htmlToText(book.chapters[i].title));
            for (int k = 0; outer[k]; k++)
                if (low.compare(0, strlen(outer[k]), outer[k]) == 0) book.chapters[i].level = 0;
        }
    }

    if (book.chapters.empty()) {
        if (err) *err = "В файле нет текста";
        return false;
    }
    return true;
}

/* ================================ HTML ================================= */

static bool readHTML(const Bytes& data, const std::string& fileName, Book& book, std::string* err) {
    std::string text = decodeText(data, NULL);
    MNode doc = parseMarkup(text, true);
    book.fileName = fileName;

    const MNode* titleEl = findFirst(&doc, "title");
    if (titleEl) book.meta.title = elText(titleEl);

    std::vector<const MNode*> metas = allNamed(&doc, "meta");
    for (size_t i = 0; i < metas.size(); i++) {
        std::string nm = lowerAscii(metas[i]->attr("name"));
        std::string he = lowerAscii(metas[i]->attr("http-equiv"));
        std::string content = collapse(metas[i]->attr("content"));
        if (nm == "author" && !content.empty() && book.meta.authors.empty()) {
            book.meta.authors.push_back(splitAuthor(content, false));
        } else if ((nm == "language" || he == "content-language") && !content.empty()) {
            std::string l = lowerAscii(content.substr(0, 5));
            if (l.size() >= 2 && isalpha((unsigned char)l[0]) && isalpha((unsigned char)l[1]))
                book.meta.lang = l.substr(0, 2);
        }
    }
    if (book.meta.title.empty()) {
        const MNode* h1 = findFirst(&doc, "h1");
        if (h1) book.meta.title = elText(h1);
    }

    const MNode* body = NULL;
    std::vector<const MNode*> bodies = allNamed(&doc, "body");
    if (!bodies.empty()) body = bodies[0];
    if (!body) {
        for (size_t i = 0; i < doc.kids.size(); i++)
            if (!doc.kids[i]->text) { body = doc.kids[i].get(); break; }
    }
    if (!body) {
        if (err) *err = "В HTML не нашлось текста";
        return false;
    }

    HtmlCtx ctx;
    ctx.book = &book;
    ctx.diskDir = pathDirOf(replaceAll(fileName, "\\", "/"));
    NoteRefs refs;
    std::set<const MNode*> skip;
    std::set<std::string> taken;
    collectNoteRefs(body, refs, 0);
    extractNotes(body, refs, ctx, skip, taken);
    ctx.skip = &skip;
    book.chapters = htmlToChapters(body, ctx);
    finishHtmlBook(book);
    if (book.chapters.empty()) {
        if (err) *err = "В HTML не нашлось текста";
        return false;
    }
    return true;
}

/* ================================ DOCX ================================= */

struct DocxStyle {
    std::string name;    /* w:name в нижнем регистре: «heading 1», «title», «заголовок 1» */
    std::string basedOn;
    int outline;         /* w:outlineLvl из стиля, -1 — нет */
    DocxStyle() : outline(-1) {}
};

enum { DOCX_BODY = -1, DOCX_TITLE = -2, DOCX_SUBTITLE = -3 };

/* Уровень заголовка по стилю: имя «heading N» (так Word хранит встроенные стили на любом
   языке интерфейса, хотя id у русского Word — «1», «2»…), затем outlineLvl, затем
   родительский стиль. */
static int docxStyleLevel(const std::map<std::string, DocxStyle>& styles, const std::string& id, int depth) {
    std::map<std::string, DocxStyle>::const_iterator it = styles.find(id);
    if (it == styles.end() || depth > 10) return DOCX_BODY;
    const std::string& nm = it->second.name;
    if (nm == "title" || nm == "название" || nm == "заголовок") return DOCX_TITLE;
    if (nm == "subtitle" || nm == "подзаголовок") return DOCX_SUBTITLE;
    const char* pfx[] = {"heading ", "заголовок "};
    for (int k = 0; k < 2; k++) {
        size_t pl = strlen(pfx[k]);
        if (nm.compare(0, pl, pfx[k]) == 0 && nm.size() > pl && isdigit((unsigned char)nm[pl])) {
            int n = atoi(nm.c_str() + pl);
            if (n >= 1 && n <= 9) return n - 1;
        }
    }
    if (it->second.outline >= 0 && it->second.outline <= 8) return it->second.outline;
    if (!it->second.basedOn.empty()) return docxStyleLevel(styles, it->second.basedOn, depth + 1);
    return DOCX_BODY;
}

static bool docxListStyle(const std::map<std::string, DocxStyle>& styles, const std::string& id) {
    std::map<std::string, DocxStyle>::const_iterator it = styles.find(id);
    if (it == styles.end()) return false;
    return it->second.name.find("list") != std::string::npos || it->second.name.find("список") != std::string::npos;
}

static const MNode* docxChild(const MNode* n, const char* name) {
    if (!n) return NULL;
    for (size_t i = 0; i < n->kids.size(); i++)
        if (!n->kids[i]->text && n->kids[i]->name == name) return n->kids[i].get();
    return NULL;
}

/* <w:b/>, <w:b w:val="1"/> — включено; <w:b w:val="0"/> или "false" — выключено */
static bool docxOn(const MNode* n) {
    if (!n) return false;
    std::string v = lowerAscii(n->attr("val"));
    return !(v == "0" || v == "false" || v == "off" || v == "none");
}

struct DocxReader {
    Book& book;
    std::map<std::string, Bytes>& low;
    std::map<std::string, std::string> rels;
    std::map<std::string, DocxStyle> styles;
    std::vector<Chapter> chapters;
    size_t cur;
    std::map<std::string, int> noteNum; /* "fn3"/"en1" -> номер, под которым сноска видна в тексте */
    std::map<int, Note> notes;

    DocxReader(Book& b, std::map<std::string, Bytes>& l) : book(b), low(l), cur(0) {
        chapters.push_back(Chapter());
    }

    std::string imageHtml(const MNode* c) {
        std::string rid;
        std::vector<const MNode*> blips = allNamed(c, "blip");
        if (!blips.empty()) rid = blips[0]->attr("r:embed");
        if (rid.empty()) {
            std::vector<const MNode*> imd = allNamed(c, "imagedata");
            if (!imd.empty()) rid = imd[0]->attr("r:id");
        }
        std::map<std::string, std::string>::iterator it = rels.find(rid);
        if (rid.empty() || it == rels.end()) return std::string();
        std::string clean = replaceAll(it->second, "\\", "/");
        if (startsWith(clean, "./")) clean = clean.substr(2);
        if (!clean.empty() && clean[0] == '/') clean = clean.substr(1);
        std::map<std::string, Bytes>::iterator fit = low.find(lowerAscii("word/" + clean));
        if (fit == low.end()) fit = low.find(lowerAscii(clean));
        if (fit == low.end()) fit = low.find(lowerAscii("word/media/" + pathBase(clean)));
        if (fit == low.end() || fit->second.empty()) return std::string();
        std::string name = addImage(book, mimeFromExt(pathExt(clean)), fit->second);
        return name.empty() ? std::string() : "<img src=\"images/" + name + "\" alt=\"\"/>";
    }

    std::string noteRef(const std::string& key) {
        int n;
        std::map<std::string, int>::iterator it = noteNum.find(key);
        if (it == noteNum.end()) {
            n = (int)noteNum.size() + 1;
            noteNum[key] = n;
        } else {
            n = it->second;
        }
        return "<a href=\"#" + key + "\">[" + intToStr(n) + "]</a>";
    }

    static std::string styled(std::string inner, const MNode* rPr) {
        if (inner.empty() || !rPr) return inner;
        const MNode* va = docxChild(rPr, "vertalign");
        std::string v = va ? va->attr("val") : std::string();
        if (v == "superscript") inner = "<sup>" + inner + "</sup>";
        else if (v == "subscript") inner = "<sub>" + inner + "</sub>";
        if (docxOn(docxChild(rPr, "strike")) || docxOn(docxChild(rPr, "dstrike"))) inner = "<s>" + inner + "</s>";
        if (docxOn(docxChild(rPr, "b"))) inner = "<strong>" + inner + "</strong>";
        if (docxOn(docxChild(rPr, "i"))) inner = "<em>" + inner + "</em>";
        return inner;
    }

    /* содержимое абзаца: прогоны, гиперссылки, вставки правки; удалённый текст пропускается */
    std::string runsHtml(const MNode* node) {
        std::string html;
        for (size_t i = 0; i < node->kids.size(); i++) {
            const MNode* ch = node->kids[i].get();
            if (ch->text) continue;
            const std::string& t = ch->name;
            if (t == "ppr" || t == "del" || t == "movefrom" || t == "rpr") continue;
            if (t != "r") { html += runsHtml(ch); continue; }

            const MNode* rPr = docxChild(ch, "rpr");
            std::string inner;
            for (size_t k = 0; k < ch->kids.size(); k++) {
                const MNode* c = ch->kids[k].get();
                if (c->text) continue;
                const std::string& ct = c->name;
                if (ct == "t") inner += escHtml(c->textContent());
                else if (ct == "br" && c->attr("type") != "page") inner += "<br/>";
                else if (ct == "tab") inner += " ";
                else if (ct == "nobreakhyphen") inner += "-";
                else if (ct == "drawing" || ct == "pict" || ct == "object") inner += imageHtml(c);
                else if (ct == "footnotereference" || ct == "endnotereference") {
                    html += styled(inner, rPr);
                    inner.clear();
                    html += noteRef(std::string(ct[0] == 'f' ? "fn" : "en") + c->attr("id"));
                }
            }
            html += styled(inner, rPr);
        }
        return html;
    }

    void handleParagraph(const MNode* p) {
        const MNode* pPr = docxChild(p, "ppr");
        const MNode* ps = docxChild(pPr, "pstyle");
        std::string styleId = ps ? ps->attr("val") : std::string();
        int level = styleId.empty() ? DOCX_BODY : docxStyleLevel(styles, styleId, 0);
        const MNode* ol = docxChild(pPr, "outlinelvl");
        if (ol && level == DOCX_BODY) {
            int v = parseIntSafe(ol->attr("val"), 9);
            if (v >= 0 && v <= 8) level = v;
        }
        bool isList = docxChild(pPr, "numpr") != NULL || docxListStyle(styles, styleId);
        {
            /* строки автоматического оглавления Word («TOC 1», «оглавление 1») — не текст книги */
            std::map<std::string, DocxStyle>::const_iterator st = styles.find(styleId);
            if (st != styles.end() && (startsWith(st->second.name, "toc ") || startsWith(st->second.name, "оглавление ")))
                return;
        }

        std::string html = trim(runsHtml(p));
        std::string plain = collapse(htmlToText(html));
        std::vector<Block>& blocks = chapters[cur].blocks;

        if (level == DOCX_TITLE && !plain.empty()) {
            if (book.meta.title.empty()) book.meta.title = plain;
            return; /* название книги — не глава */
        }
        if (level >= 0) {
            if (plain.empty()) return;
            Chapter n;
            n.level = level;
            n.title = html;
            chapters.push_back(n);
            cur = chapters.size() - 1;
            return;
        }
        if (html.empty()) {
            if (!blocks.empty() && blocks.back().type != Block::EMPTY) blocks.push_back(Block::empty());
            return;
        }
        if (plain.empty() && html.find("<img") != std::string::npos) {
            /* абзац из одних картинок */
            size_t pos = 0;
            while ((pos = html.find("src=\"images/", pos)) != std::string::npos) {
                size_t e = html.find('"', pos + 12);
                if (e == std::string::npos) break;
                Block b(Block::IMG);
                b.src = "images/" + html.substr(pos + 12, e - pos - 12);
                blocks.push_back(b);
                pos = e;
            }
            return;
        }
        if (level == DOCX_SUBTITLE) { blocks.push_back(Block::sub(html)); return; }
        Block b(isList ? Block::LI : Block::P);
        b.html = html;
        blocks.push_back(b);
    }

    std::string cellHtml(const MNode* tc) {
        std::string s;
        std::vector<const MNode*> ps = allNamed(tc, "p");
        for (size_t i = 0; i < ps.size(); i++) {
            std::string h = trim(runsHtml(ps[i]));
            if (h.empty()) continue;
            if (!s.empty()) s += "<br/>";
            s += h;
        }
        return s;
    }

    void handleTable(const MNode* tbl) {
        std::string html = "<table>";
        for (size_t r = 0; r < tbl->kids.size(); r++) {
            const MNode* tr = tbl->kids[r].get();
            if (tr->text || tr->name != "tr") continue;
            bool header = docxChild(docxChild(tr, "trpr"), "tblheader") != NULL;
            const char* tag = header ? "th" : "td";
            html += "<tr>";
            for (size_t c = 0; c < tr->kids.size(); c++) {
                const MNode* tc = tr->kids[c].get();
                if (tc->text || tc->name != "tc") continue;
                std::string span;
                const MNode* gs = docxChild(docxChild(tc, "tcpr"), "gridspan");
                if (gs && parseIntSafe(gs->attr("val"), 1) > 1) span = " colspan=\"" + escHtml(gs->attr("val")) + "\"";
                html += std::string("<") + tag + span + ">" + cellHtml(tc) + "</" + tag + ">";
            }
            html += "</tr>";
        }
        Block b(Block::RAW);
        b.html = html + "</table>";
        chapters[cur].blocks.push_back(b);
    }

    void walk(const MNode* node, int depth) {
        if (depth > 40) return;
        for (size_t i = 0; i < node->kids.size(); i++) {
            const MNode* ch = node->kids[i].get();
            if (ch->text) continue;
            const std::string& t = ch->name;
            if (t == "p") handleParagraph(ch);
            else if (t == "tbl") handleTable(ch);
            else if (t == "sdt" || t == "sdtcontent" || t == "body" || t == "customxml" ||
                     t == "smarttag" || t == "ins")
                walk(ch, depth + 1);
        }
    }

    /* footnotes.xml / endnotes.xml: только сноски, на которые есть ссылки в тексте */
    void readNotes(const char* part, const char* tag, const char* prefix) {
        if (low.find(part) == low.end()) return;
        MNode nd = parseMarkup(decodeText(low[part], NULL), false);
        std::vector<const MNode*> list = allNamed(&nd, tag);
        for (size_t i = 0; i < list.size(); i++) {
            std::string type = list[i]->attr("type");
            if (!type.empty() && type != "normal") continue; /* разделители */
            std::string key = prefix + list[i]->attr("id");
            std::map<std::string, int>::iterator num = noteNum.find(key);
            if (num == noteNum.end()) continue;
            Note n;
            n.id = key;
            n.title = intToStr(num->second);
            std::vector<const MNode*> ps = allNamed(list[i], "p");
            for (size_t k = 0; k < ps.size(); k++) {
                std::string h = trim(runsHtml(ps[k]));
                if (!collapse(htmlToText(h)).empty()) n.blocks.push_back(Block::p(h));
            }
            if (!n.blocks.empty()) notes[num->second] = n;
        }
    }
};

static bool readDOCX(const Bytes& data, const std::string& fileName, Book& book, std::string* err) {
    std::map<std::string, Bytes> files;
    std::string zerr;
    if (!zipRead(data, files, &zerr)) {
        if (err) *err = zerr;
        return false;
    }
    std::map<std::string, Bytes> low;
    for (std::map<std::string, Bytes>::iterator it = files.begin(); it != files.end(); ++it)
        low[lowerAscii(it->first)] = it->second;

    if (low.find("word/document.xml") == low.end()) {
        if (err) *err = "В DOCX нет word/document.xml";
        return false;
    }
    book.fileName = fileName;
    DocxReader r(book, low);

    if (low.find("word/styles.xml") != low.end()) {
        MNode st = parseMarkup(decodeText(low["word/styles.xml"], NULL), false);
        std::vector<const MNode*> list = allNamed(&st, "style");
        for (size_t i = 0; i < list.size(); i++) {
            std::string id = list[i]->attr("styleId");
            if (id.empty()) continue;
            DocxStyle s;
            const MNode* nm = docxChild(list[i], "name");
            if (nm) s.name = toLowerRu(collapse(nm->attr("val")));
            const MNode* bo = docxChild(list[i], "basedon");
            if (bo) s.basedOn = bo->attr("val");
            const MNode* ol = docxChild(docxChild(list[i], "ppr"), "outlinelvl");
            if (ol) s.outline = parseIntSafe(ol->attr("val"), -1);
            r.styles[id] = s;
        }
    }

    if (low.find("word/_rels/document.xml.rels") != low.end()) {
        MNode rd = parseMarkup(decodeText(low["word/_rels/document.xml.rels"], NULL), false);
        std::vector<const MNode*> list = allNamed(&rd, "relationship");
        for (size_t i = 0; i < list.size(); i++) {
            std::string id = list[i]->attr("id");
            std::string target = list[i]->attr("target");
            if (!id.empty() && !target.empty()) r.rels[id] = target;
        }
    }

    if (low.find("docprops/core.xml") != low.end()) {
        MNode core = parseMarkup(decodeText(low["docprops/core.xml"], NULL), false);
        std::vector<const MNode*> cn = allNamed(&core, "title");
        if (!cn.empty()) book.meta.title = elText(cn[0]);
        std::vector<const MNode*> cr = allNamed(&core, "creator");
        if (!cr.empty()) {
            std::string v = elText(cr[0]);
            if (!v.empty()) book.meta.authors.push_back(splitAuthor(v, false));
        }
        std::vector<const MNode*> ds = allNamed(&core, "description");
        if (!ds.empty() && !elText(ds[0]).empty()) book.meta.annotation = "<p>" + escHtml(elText(ds[0])) + "</p>";
    }

    MNode doc = parseMarkup(decodeText(low["word/document.xml"], NULL), false);
    const MNode* bodyEl = findFirst(&doc, "body");
    r.walk(bodyEl ? bodyEl : &doc, 0);
    r.readNotes("word/footnotes.xml", "footnote", "fn");
    r.readNotes("word/endnotes.xml", "endnote", "en");
    for (std::map<int, Note>::iterator it = r.notes.begin(); it != r.notes.end(); ++it)
        book.notes.push_back(it->second);

    book.chapters.clear();
    for (size_t i = 0; i < r.chapters.size(); i++) {
        bool has = !r.chapters[i].title.empty();
        for (size_t k = 0; k < r.chapters[i].blocks.size() && !has; k++)
            if (r.chapters[i].blocks[k].type != Block::EMPTY) has = true;
        if (has) book.chapters.push_back(r.chapters[i]);
    }
    normalizeChapterLevels(book.chapters);
    if (book.chapters.empty()) {
        if (err) *err = "В DOCX не нашлось текста";
        return false;
    }
    return true;
}

/* ================================= ODT ================================= */
/* OpenDocument (LibreOffice, Word «Сохранить как ODT»): content.xml со стилями,
   заголовки text:h с outline-level, сноски text:note прямо в абзаце. */

struct OdtStyle {
    std::string name, display, parent;
    int bold, italic, strike; /* -1 — как у родителя */
    int pos;                  /* 1 — надстрочный, -1 — подстрочный, 0 — обычный, -2 — как у родителя */
    int outline;
    OdtStyle() : bold(-1), italic(-1), strike(-1), pos(-2), outline(-1) {}
};

struct OdtReader {
    Book& book;
    std::map<std::string, Bytes>& low;
    std::map<std::string, OdtStyle> styles;
    std::vector<Chapter> chapters;
    size_t cur;
    int noteSeq;

    OdtReader(Book& b, std::map<std::string, Bytes>& l) : book(b), low(l), cur(0), noteSeq(0) {
        chapters.push_back(Chapter());
    }

    void readStyles(const MNode* root) {
        std::vector<const MNode*> list = allNamed(root, "style");
        for (size_t i = 0; i < list.size(); i++) {
            const MNode* el = list[i];
            std::string name = el->attr("style:name");
            if (name.empty()) continue;
            OdtStyle s;
            s.name = toLowerRu(name);
            s.display = toLowerRu(el->attr("style:display-name"));
            s.parent = el->attr("style:parent-style-name");
            s.outline = parseIntSafe(el->attr("style:default-outline-level"), -1);
            const MNode* tp = el->child("text-properties");
            if (tp) {
                std::string fw = tp->attr("fo:font-weight");
                if (!fw.empty()) s.bold = (fw == "bold" || atoi(fw.c_str()) >= 600) ? 1 : 0;
                std::string fs = tp->attr("fo:font-style");
                if (!fs.empty()) s.italic = (fs == "italic" || fs == "oblique") ? 1 : 0;
                std::string lt = tp->attr("style:text-line-through-style");
                if (!lt.empty()) s.strike = lt != "none" ? 1 : 0;
                std::string tpos = lowerAscii(trim(tp->attr("style:text-position")));
                if (!tpos.empty()) {
                    if (startsWith(tpos, "super")) s.pos = 1;
                    else if (startsWith(tpos, "sub") || tpos[0] == '-') s.pos = -1;
                    else s.pos = atoi(tpos.c_str()) > 0 ? 1 : 0;
                }
            }
            styles[name] = s;
        }
    }

    const OdtStyle* style(const std::string& name) const {
        std::map<std::string, OdtStyle>::const_iterator it = styles.find(name);
        return it == styles.end() ? NULL : &it->second;
    }

    int prop(const std::string& name, int OdtStyle::*field, int unset, int depth) const {
        const OdtStyle* s = style(name);
        if (!s || depth > 12) return unset;
        if (s->*field != unset) return s->*field;
        return s->parent.empty() ? unset : prop(s->parent, field, unset, depth + 1);
    }

    bool isTitleStyle(const std::string& name, int depth) const {
        const OdtStyle* s = style(name);
        if (!s || depth > 12) return false;
        const char* names[] = {"title", "заголовок", "название", NULL};
        for (int i = 0; names[i]; i++)
            if (s->name == names[i] || s->display == names[i]) return true;
        return !s->parent.empty() && isTitleStyle(s->parent, depth + 1);
    }

    bool isTocStyle(const std::string& name) const {
        const OdtStyle* s = style(name);
        if (!s) return false;
        return startsWith(s->display, "contents") || startsWith(s->display, "оглавление") ||
               startsWith(s->name, "contents") || startsWith(s->name, "toc");
    }

    std::string styled(std::string inner, const std::string& styleName) const {
        if (inner.empty() || styleName.empty()) return inner;
        int pos = prop(styleName, &OdtStyle::pos, -2, 0);
        if (pos == 1) inner = "<sup>" + inner + "</sup>";
        else if (pos == -1) inner = "<sub>" + inner + "</sub>";
        if (prop(styleName, &OdtStyle::strike, -1, 0) == 1) inner = "<s>" + inner + "</s>";
        if (prop(styleName, &OdtStyle::bold, -1, 0) == 1) inner = "<strong>" + inner + "</strong>";
        if (prop(styleName, &OdtStyle::italic, -1, 0) == 1) inner = "<em>" + inner + "</em>";
        return inner;
    }

    std::string imageHtml(const MNode* frame) {
        std::string out;
        std::vector<const MNode*> imgs = allNamed(frame, "image");
        for (size_t i = 0; i < imgs.size() && out.empty(); i++) {
            std::string href = replaceAll(imgs[i]->attr("xlink:href"), "\\", "/");
            if (startsWith(href, "./")) href = href.substr(2);
            std::map<std::string, Bytes>::iterator f = low.find(lowerAscii(href));
            if (f == low.end() || f->second.empty()) continue;
            std::string name = addImage(book, mimeFromExt(pathExt(href)), f->second);
            if (!name.empty()) out = "<img src=\"images/" + name + "\" alt=\"\"/>";
        }
        return out;
    }

    std::string noteHtml(const MNode* note) {
        Note n;
        n.id = note->attr("text:id");
        if (n.id.empty()) n.id = "note" + intToStr(++noteSeq);
        const MNode* cit = note->child("note-citation");
        std::string label = cit ? collapse(cit->textContent()) : std::string();
        if (label.empty()) label = intToStr((long long)book.notes.size() + 1);
        n.title = escHtml(label);
        const MNode* body = note->child("note-body");
        if (body) {
            for (size_t i = 0; i < body->kids.size(); i++) {
                const MNode* p = body->kids[i].get();
                if (p->text || (p->name != "p" && p->name != "h")) continue;
                std::string h = trim(inlineHtml(p));
                if (hasVisibleText(h)) n.blocks.push_back(Block::p(h));
            }
        }
        if (n.blocks.empty()) return std::string();
        book.notes.push_back(n);
        return "<a href=\"#" + escHtml(n.id) + "\">[" + escHtml(label) + "]</a>";
    }

    /* ODF схлопывает пробелы так же, как HTML; явные пробелы — <text:s text:c="N"/> */
    std::string inlineHtml(const MNode* n) {
        std::string s;
        for (size_t i = 0; i < n->kids.size(); i++) {
            const MNode* k = n->kids[i].get();
            if (k->text) {
                std::string v = k->value;
                for (size_t c = 0; c < v.size(); c++)
                    if (v[c] == '\n' || v[c] == '\r' || v[c] == '\t') v[c] = ' ';
                s += escHtml(v);
                continue;
            }
            const std::string& t = k->name;
            if (t == "s") {
                int c = parseIntSafe(k->attr("text:c"), 1);
                s.append((size_t)std::max(1, std::min(c, 50)), ' ');
            } else if (t == "tab") s += " ";
            else if (t == "line-break") s += "<br/>";
            else if (t == "span") s += styled(inlineHtml(k), k->attr("text:style-name"));
            else if (t == "a") {
                std::string href = k->attr("xlink:href");
                std::string inner = inlineHtml(k);
                s += href.empty() ? inner : "<a href=\"" + escHtml(href) + "\">" + inner + "</a>";
            } else if (t == "note") s += noteHtml(k);
            else if (t == "frame") s += imageHtml(k);
            else if (t == "annotation" || t == "bookmark" || t == "bookmark-start" || t == "bookmark-end" ||
                     t == "soft-page-break" || t == "change" || t == "change-start" || t == "change-end" ||
                     t == "reference-mark-start" || t == "reference-mark-end" || t == "tracked-changes")
                continue;
            else s += inlineHtml(k);
        }
        return s;
    }

    void paragraph(const MNode* p, bool inList) {
        std::string st = p->attr("text:style-name");
        std::string html = trim(collapseSpacesPreserveBr(inlineHtml(p)));
        std::string plain = collapse(htmlToText(html));
        std::vector<Block>& blocks = chapters[cur].blocks;
        if (isTocStyle(st)) return;
        if (isTitleStyle(st, 0) && !plain.empty()) {
            if (book.meta.title.empty()) book.meta.title = plain;
            return;
        }
        int outline = prop(st, &OdtStyle::outline, -1, 0);
        if (outline >= 1 && outline <= 9 && !plain.empty()) { heading(html, outline - 1); return; }
        if (html.empty()) {
            if (!blocks.empty() && blocks.back().type != Block::EMPTY) blocks.push_back(Block::empty());
            return;
        }
        if (plain.empty() && html.find("<img") != std::string::npos) {
            size_t pos = 0;
            while ((pos = html.find("src=\"images/", pos)) != std::string::npos) {
                size_t e = html.find('"', pos + 12);
                if (e == std::string::npos) break;
                Block b(Block::IMG);
                b.src = "images/" + html.substr(pos + 12, e - pos - 12);
                blocks.push_back(b);
                pos = e;
            }
            return;
        }
        Block b(inList ? Block::LI : Block::P);
        b.html = html;
        blocks.push_back(b);
    }

    void heading(const std::string& html, int level) {
        Chapter c;
        c.level = level;
        c.title = html;
        chapters.push_back(c);
        cur = chapters.size() - 1;
    }

    std::string cellHtml(const MNode* cell) {
        std::string s;
        for (size_t i = 0; i < cell->kids.size(); i++) {
            const MNode* p = cell->kids[i].get();
            if (p->text || (p->name != "p" && p->name != "h")) continue;
            std::string h = trim(collapseSpacesPreserveBr(inlineHtml(p)));
            if (h.empty()) continue;
            if (!s.empty()) s += "<br/>";
            s += h;
        }
        return s;
    }

    void tableRows(const MNode* n, std::string& html) {
        for (size_t i = 0; i < n->kids.size(); i++) {
            const MNode* r = n->kids[i].get();
            if (r->text) continue;
            if (r->name == "table-header-rows" || r->name == "table-rows" || r->name == "table-row-group") {
                tableRows(r, html);
                continue;
            }
            if (r->name != "table-row") continue;
            html += "<tr>";
            for (size_t c = 0; c < r->kids.size(); c++) {
                const MNode* cell = r->kids[c].get();
                if (cell->text || cell->name != "table-cell") continue;
                int span = parseIntSafe(cell->attr("table:number-columns-spanned"), 1);
                html += span > 1 ? "<td colspan=\"" + intToStr(span) + "\">" : std::string("<td>");
                html += cellHtml(cell) + "</td>";
            }
            html += "</tr>";
        }
    }

    void walk(const MNode* n, bool inList, int depth) {
        if (depth > 40) return;
        for (size_t i = 0; i < n->kids.size(); i++) {
            const MNode* k = n->kids[i].get();
            if (k->text) continue;
            const std::string& t = k->name;
            if (t == "h") {
                std::string html = trim(collapseSpacesPreserveBr(inlineHtml(k)));
                if (collapse(htmlToText(html)).empty()) continue;
                int lvl = parseIntSafe(k->attr("text:outline-level"), 1);
                heading(html, std::max(0, std::min(lvl, 9) - 1));
            } else if (t == "p") {
                paragraph(k, inList);
            } else if (t == "list" || t == "list-item" || t == "list-header") {
                walk(k, true, depth + 1);
            } else if (t == "table") {
                std::string html = "<table>";
                tableRows(k, html);
                Block b(Block::RAW);
                b.html = html + "</table>";
                chapters[cur].blocks.push_back(b);
            } else if (t == "table-of-content" || t == "alphabetical-index" || t == "illustration-index" ||
                       t == "tracked-changes" || t == "sequence-decls" || t == "forms") {
                continue; /* оглавление документа и служебное — не текст книги */
            } else {
                walk(k, inList, depth + 1); /* section, index-body и прочие обёртки */
            }
        }
    }
};

static bool readODT(const Bytes& data, const std::string& fileName, Book& book, std::string* err) {
    std::map<std::string, Bytes> files;
    std::string zerr;
    if (!zipRead(data, files, &zerr)) {
        if (err) *err = zerr;
        return false;
    }
    std::map<std::string, Bytes> low;
    for (std::map<std::string, Bytes>::iterator it = files.begin(); it != files.end(); ++it)
        low[lowerAscii(it->first)] = it->second;
    if (low.find("content.xml") == low.end()) {
        if (err) *err = "В ODT нет content.xml";
        return false;
    }
    book.fileName = fileName;
    OdtReader r(book, low);

    if (low.count("meta.xml")) {
        MNode meta = parseMarkup(decodeText(low["meta.xml"], NULL), false);
        const MNode* t = findFirst(&meta, "title");
        if (t && !elText(t).empty()) book.meta.title = elText(t);
        /* initial-creator — автор документа; creator — тот, кто сохранял последним */
        const MNode* a = findFirst(&meta, "initial-creator");
        if (!a || elText(a).empty()) a = findFirst(&meta, "creator");
        if (a && !elText(a).empty()) book.meta.authors.push_back(splitAuthor(elText(a), false));
        const MNode* d = findFirst(&meta, "description");
        if (d && !elText(d).empty()) book.meta.annotation = "<p>" + escHtml(elText(d)) + "</p>";
        const MNode* l = findFirst(&meta, "language");
        if (l && elText(l).size() >= 2) book.meta.lang = lowerAscii(elText(l).substr(0, 2));
    }
    if (low.count("styles.xml")) {
        MNode st = parseMarkup(decodeText(low["styles.xml"], NULL), false);
        r.readStyles(&st);
    }
    MNode doc = parseMarkup(decodeText(low["content.xml"], NULL), false);
    r.readStyles(&doc); /* автоматические стили документа */
    const MNode* text = findFirst(&doc, "text"); /* office:text */
    if (text) r.walk(text, false, 0);
    if (!text) {
        if (err) *err = "В ODT не нашлось текста";
        return false;
    }

    book.chapters.clear();
    for (size_t i = 0; i < r.chapters.size(); i++) {
        bool has = !r.chapters[i].title.empty();
        for (size_t k = 0; k < r.chapters[i].blocks.size() && !has; k++)
            if (r.chapters[i].blocks[k].type != Block::EMPTY) has = true;
        if (has) book.chapters.push_back(r.chapters[i]);
    }
    normalizeChapterLevels(book.chapters);
    if (book.chapters.empty()) {
        if (err) *err = "В ODT не нашлось текста";
        return false;
    }
    return true;
}

/* ================================ EPUB ================================= */

static bool isTitlePageName(const std::string& base) {
    std::string b = lowerAscii(base);
    return b == "titlepage.xhtml" || b == "titlepage.html" || b == "title_page.xhtml" ||
           b == "title_page.html" || b == "title.xhtml" || b == "title.html" ||
           b == "cover.xhtml" || b == "cover.html" || b == "coverpage.xhtml" ||
           b == "coverpage.html";
}

static bool isNotesName(const std::string& base) {
    std::string b = lowerAscii(base);
    return b == "notes.xhtml" || b == "notes.html" || b == "notes.htm" ||
           b == "footnotes.xhtml" || b == "footnotes.html" || b == "footnotes.htm";
}

static void collectAnchors(const MNode* n, std::vector<const MNode*>& out) {
    for (size_t i = 0; i < n->kids.size(); i++) {
        const MNode* k = n->kids[i].get();
        if (k->text) continue;
        if (!k->attr("id").empty() &&
            (k->name == "p" || k->name == "div" || k->name == "aside" ||
             k->name == "li" || k->name == "blockquote" || k->name == "section")) {
            out.push_back(k);
            continue;
        }
        collectAnchors(k, out);
    }
}

/* ссылки «notes.xhtml#n1» -> «#n1» */
static std::string rewriteNoteHrefs(const std::string& html) {
    if (html.empty()) return html;
    std::string out;
    std::string low = lowerAscii(html);
    size_t pos = 0;
    for (;;) {
        size_t p = low.find("href=\"", pos);
        if (p == std::string::npos) { out.append(html, pos, std::string::npos); break; }
        out.append(html, pos, p - pos);
        size_t vs = p + 6;
        size_t q = html.find('"', vs);
        if (q == std::string::npos) { out.append(html, p, std::string::npos); break; }
        size_t hash = html.find('#', vs);
        if (hash != std::string::npos && hash < q && hash + 1 < q) {
            std::string file = lowerAscii(html.substr(vs, hash - vs));
            size_t slash = file.find_last_of('/');
            std::string base = slash == std::string::npos ? file : file.substr(slash + 1);
            if (isNotesName(base)) {
                out += "href=\"" + html.substr(hash, q - hash) + "\"";
                pos = q + 1;
                continue;
            }
        }
        out.append(html, p, q - p + 1);
        pos = q + 1;
    }
    return out;
}

static void rewriteBlocksHrefs(std::vector<Block>& blocks) {
    for (size_t i = 0; i < blocks.size(); i++) {
        blocks[i].html = rewriteNoteHrefs(blocks[i].html);
        rewriteBlocksHrefs(blocks[i].blocks);
    }
}

/* Глубина пунктов оглавления EPUB: файл (полный путь, нижний регистр) -> уровень.
   Уровни h1..h6 в разных файлах EPUB между собой не согласованы (часто каждая глава —
   свой h1), а оглавление nav/ncx хранит настоящую вложенность. */
struct TocEntry {
    int depth;
    std::string label; /* текст пункта — заголовок файла, если в самом файле его нет */
};
typedef std::map<std::string, TocEntry> TocDepth;

static void tocAdd(TocDepth& out, const std::string& dir, const std::string& href, int depth,
                   const std::string& label) {
    std::string p = lowerAscii(normalizeRel(dir, href));
    if (p.empty() || out.find(p) != out.end()) return;
    TocEntry e;
    e.depth = depth;
    e.label = collapse(label);
    out[p] = e;
}

static void tocWalkNcx(const MNode* n, int depth, const std::string& dir, TocDepth& out) {
    for (size_t i = 0; i < n->kids.size(); i++) {
        const MNode* k = n->kids[i].get();
        if (k->text) continue;
        if (k->name == "navpoint") {
            const MNode* c = k->child("content");
            const MNode* nl = k->child("navlabel");
            const MNode* tx = nl ? nl->child("text") : NULL;
            if (c) tocAdd(out, dir, c->attr("src"), depth, tx ? tx->textContent() : std::string());
            tocWalkNcx(k, depth + 1, dir, out);
        } else {
            tocWalkNcx(k, depth, dir, out);
        }
    }
}

static void tocWalkNavOl(const MNode* ol, int depth, const std::string& dir, TocDepth& out) {
    for (size_t i = 0; i < ol->kids.size(); i++) {
        const MNode* li = ol->kids[i].get();
        if (li->text || li->name != "li") continue;
        const MNode* a = li->child("a");
        if (!a) a = findNode(li, "a");
        if (a) tocAdd(out, dir, a->attr("href"), depth, a->textContent());
        std::vector<const MNode*> sub = li->childrenNamed("ol");
        for (size_t s = 0; s < sub.size(); s++) tocWalkNavOl(sub[s], depth + 1, dir, out);
    }
}

static TocDepth readEpubToc(std::map<std::string, Bytes>& low, const std::string& navPath,
                            const std::string& ncxPath) {
    TocDepth out;
    if (!navPath.empty() && low.count(lowerAscii(navPath))) {
        MNode doc = parseMarkup(decodeText(low[lowerAscii(navPath)], NULL), true);
        std::vector<const MNode*> navs = findNodes(&doc, "nav");
        for (size_t i = 0; i < navs.size() && out.empty(); i++) {
            std::string t = lowerAscii(navs[i]->attr("epub:type") + " " + navs[i]->attr("type"));
            if (t.find("toc") == std::string::npos && navs.size() > 1) continue;
            const MNode* ol = navs[i]->child("ol");
            if (!ol) ol = findNode(navs[i], "ol");
            if (ol) tocWalkNavOl(ol, 0, pathDirOf(navPath), out);
        }
    }
    if (out.empty() && !ncxPath.empty() && low.count(lowerAscii(ncxPath))) {
        MNode doc = parseMarkup(decodeText(low[lowerAscii(ncxPath)], NULL), false);
        const MNode* map = findNode(&doc, "navmap");
        if (map) tocWalkNcx(map, 0, pathDirOf(ncxPath), out);
    }
    return out;
}

/* Описание из OPF: абзацы через пустую строку или HTML внутри (так пишет calibre). */
static std::string epubAnnotation(const std::string& raw) {
    std::string ann;
    std::string low = lowerAscii(raw);
    if (low.find("<p") != std::string::npos || low.find("<div") != std::string::npos ||
        low.find("<br") != std::string::npos) {
        MNode d = parseMarkup(raw, true);
        std::vector<const MNode*> ps = findNodes(&d, "p");
        for (size_t i = 0; i < ps.size(); i++) {
            std::string t = collapse(ps[i]->textContent());
            if (!t.empty()) ann += "<p>" + escHtml(t) + "</p>";
        }
        if (ann.empty()) {
            std::string t = collapse(d.textContent());
            if (!t.empty()) ann = "<p>" + escHtml(t) + "</p>";
        }
        return ann;
    }
    std::string s = replaceAll(raw, "\r", "");
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find("\n\n", pos);
        if (e == std::string::npos) e = s.size();
        std::string t = collapse(s.substr(pos, e - pos));
        if (!t.empty()) ann += "<p>" + escHtml(t) + "</p>";
        if (e == s.size()) break;
        pos = e + 2;
    }
    return ann;
}

/* «<strong>12</strong> Текст» -> номер примечания отдельно (так пишут EPUB и MOBI этой программы). */
static void splitNoteNumber(std::string& html, std::string& title) {
    const char* open[] = {"<strong>", "<b>"};
    const char* close[] = {"</strong>", "</b>"};
    for (int i = 0; i < 2; i++) {
        if (!startsWith(html, open[i])) continue;
        size_t ol = strlen(open[i]);
        size_t e = html.find(close[i], ol);
        if (e == std::string::npos || e - ol > 24) return;
        std::string num = trim(html.substr(ol, e - ol));
        std::string rest = trim(html.substr(e + strlen(close[i])));
        if (num.empty() || rest.empty() || num.find('<') != std::string::npos) return;
        title = num;
        html = rest;
        return;
    }
}

static bool readEPUB(const Bytes& data, const std::string& fileName, Book& book, std::string* err) {
    std::map<std::string, Bytes> files;
    std::string zerr;
    if (!zipRead(data, files, &zerr)) {
        if (err) *err = zerr;
        return false;
    }
    std::map<std::string, Bytes> low;
    for (std::map<std::string, Bytes>::iterator it = files.begin(); it != files.end(); ++it)
        low[lowerAscii(it->first)] = it->second;

    book.fileName = fileName;

    std::string opfPath;
    if (low.count("meta-inf/container.xml")) {
        MNode c = parseMarkup(decodeText(low["meta-inf/container.xml"], NULL), false);
        std::vector<const MNode*> rf = allNamed(&c, "rootfile");
        for (size_t i = 0; i < rf.size(); i++) {
            std::string fp = rf[i]->attr("full-path");
            if (!fp.empty()) { opfPath = fp; break; }
        }
    }
    if (opfPath.empty()) {
        for (std::map<std::string, Bytes>::iterator it = low.begin(); it != low.end(); ++it)
            if (endsWithNoCase(it->first, ".opf")) { opfPath = it->first; break; }
    }
    if (opfPath.empty() || low.find(lowerAscii(opfPath)) == low.end()) {
        if (err) *err = "В EPUB нет OPF-файла";
        return false;
    }
    std::string opfDir = pathDirOf(opfPath);
    if (!opfDir.empty()) opfDir += "/";

    MNode opf = parseMarkup(decodeText(low[lowerAscii(opfPath)], NULL), false);

    std::vector<const MNode*> titles = allNamed(&opf, "title");
    for (size_t i = 0; i < titles.size(); i++) {
        std::string v = elText(titles[i]);
        if (!v.empty()) { book.meta.title = v; break; }
    }
    std::vector<const MNode*> creators = allNamed(&opf, "creator");
    for (size_t i = 0; i < creators.size(); i++) {
        std::string v = elText(creators[i]);
        if (!v.empty()) {
            Author a = splitAuthor(v, false);
            if (!fullName(a).empty()) book.meta.authors.push_back(a);
        }
    }
    std::vector<const MNode*> langs = allNamed(&opf, "language");
    for (size_t i = 0; i < langs.size(); i++) {
        std::string v = elText(langs[i]);
        if (!v.empty()) { book.meta.lang = v; break; }
    }
    std::vector<const MNode*> pubs = allNamed(&opf, "publisher");
    if (!pubs.empty()) book.meta.publisher = elText(pubs[0]);
    std::vector<const MNode*> dates = allNamed(&opf, "date");
    if (!dates.empty()) {
        std::string v = elText(dates[0]);
        if (v.size() > 10) v = v.substr(0, 10);
        book.meta.year = v;
    }
    std::vector<const MNode*> descs = allNamed(&opf, "description");
    if (!descs.empty()) book.meta.annotation = epubAnnotation(descs[0]->textContent());
    std::vector<const MNode*> subjects = allNamed(&opf, "subject");
    for (size_t i = 0; i < subjects.size(); i++) {
        std::string v = elText(subjects[i]);
        if (!v.empty()) book.meta.genres.push_back(v);
    }

    std::string coverId;
    std::vector<const MNode*> metas = allNamed(&opf, "meta");
    std::string colId;
    for (size_t i = 0; i < metas.size(); i++) {
        std::string name = lowerAscii(metas[i]->attr("name"));
        std::string prop = lowerAscii(metas[i]->attr("property"));
        if (name == "cover") coverId = metas[i]->attr("content");
        /* серия: EPUB 3 (belongs-to-collection) и calibre (EPUB 2) */
        if (prop == "belongs-to-collection" && book.meta.seriesName.empty()) {
            book.meta.seriesName = elText(metas[i]);
            colId = metas[i]->attr("id");
        }
        if (name == "calibre:series" && book.meta.seriesName.empty())
            book.meta.seriesName = collapse(metas[i]->attr("content"));
        if (name == "calibre:series_index" && book.meta.seriesNum.empty()) {
            std::string v = collapse(metas[i]->attr("content"));
            if (endsWithNoCase(v, ".0")) v = v.substr(0, v.size() - 2);
            book.meta.seriesNum = v;
        }
    }
    if (!colId.empty()) {
        for (size_t i = 0; i < metas.size(); i++) {
            if (metas[i]->attr("refines") == "#" + colId &&
                lowerAscii(metas[i]->attr("property")) == "group-position")
                book.meta.seriesNum = elText(metas[i]);
        }
    }

    struct Item { std::string href, type, props; };
    std::map<std::string, Item> manifest;
    std::vector<const MNode*> items = allNamed(&opf, "item");
    for (size_t i = 0; i < items.size(); i++) {
        std::string id = items[i]->attr("id");
        if (id.empty()) continue;
        Item it;
        it.href = items[i]->attr("href");
        it.type = lowerAscii(items[i]->attr("media-type"));
        it.props = lowerAscii(items[i]->attr("properties"));
        manifest[id] = it;
    }
    std::vector<std::string> spine;
    std::vector<const MNode*> refs = allNamed(&opf, "itemref");
    for (size_t i = 0; i < refs.size(); i++) {
        std::string id = refs[i]->attr("idref");
        if (!id.empty()) spine.push_back(id);
    }

    /* оглавление: nav.xhtml (EPUB 3) или toc.ncx (EPUB 2) */
    std::string navPath, ncxPath;
    for (std::map<std::string, Item>::iterator it = manifest.begin(); it != manifest.end(); ++it) {
        if (it->second.props.find("nav") != std::string::npos)
            navPath = normalizeRel(opfDir, it->second.href);
        else if (it->second.type == "application/x-dtbncx+xml")
            ncxPath = normalizeRel(opfDir, it->second.href);
    }
    TocDepth tocDepth = readEpubToc(low, navPath, ncxPath);

    /* обложка */
    for (std::map<std::string, Item>::iterator it = manifest.begin(); it != manifest.end(); ++it) {
        bool isCover = it->second.props.find("cover-image") != std::string::npos ||
                       it->first == coverId;
        if (!isCover) continue;
        std::string p = normalizeRel(opfDir, it->second.href);
        std::map<std::string, Bytes>::iterator f = low.find(lowerAscii(p));
        if (f != low.end() && !f->second.empty()) {
            std::string mime = it->second.type.empty() ? mimeFromExt(pathExt(p)) : it->second.type;
            book.coverName = addImage(book, mime, f->second);
        }
        break;
    }

    std::vector<Chapter> chapters;
    Chapter first;
    first.level = 0;
    chapters.push_back(first);
    Chapter* cur = &chapters[0];

    /* проход 1: все файлы текста; ссылка на сноску и сама сноска обычно в разных файлах */
    struct SpineDoc {
        std::string path;
        std::unique_ptr<MNode> doc;
        const MNode* body;
    };
    std::vector<SpineDoc> docs;
    for (size_t s = 0; s < spine.size(); s++) {
        std::map<std::string, Item>::iterator it = manifest.find(spine[s]);
        if (it == manifest.end()) continue;
        if (!it->second.type.empty() && it->second.type.find("html") == std::string::npos) continue;
        std::string path = normalizeRel(opfDir, it->second.href);
        std::map<std::string, Bytes>::iterator f = low.find(lowerAscii(path));
        if (f == low.end()) continue;
        if (isTitlePageName(lowerAscii(pathBase(path)))) continue;

        SpineDoc sd;
        sd.path = path;
        sd.doc.reset(new MNode(parseMarkup(decodeText(f->second, NULL), true)));
        std::vector<const MNode*> bodies = allNamed(sd.doc.get(), "body");
        sd.body = bodies.empty() ? NULL : bodies[0];
        for (size_t i = 0; !sd.body && i < sd.doc->kids.size(); i++)
            if (!sd.doc->kids[i]->text) sd.body = sd.doc->kids[i].get();
        if (sd.body) docs.push_back(std::move(sd));
    }

    NoteRefs noteRefs;
    std::set<const MNode*> skip;
    std::set<std::string> taken;
    for (size_t d = 0; d < docs.size(); d++) collectNoteRefs(docs[d].body, noteRefs, 0);
    for (size_t d = 0; d < docs.size(); d++) {
        HtmlCtx ctx;
        ctx.book = &book;
        ctx.files = &low;
        ctx.baseDir = pathDirOf(docs[d].path);
        extractNotes(docs[d].body, noteRefs, ctx, skip, taken);
    }
    bool genericNotes = !book.notes.empty();

    /* проход 2: главы */
    for (size_t d = 0; d < docs.size(); d++) {
        const std::string& path = docs[d].path;
        const MNode* body = docs[d].body;
        std::string baseName = lowerAscii(pathBase(path));

        HtmlCtx ctx;
        ctx.book = &book;
        ctx.files = &low;
        ctx.baseDir = pathDirOf(path);
        ctx.skip = &skip;

        /* «notes.xhtml» с якорями, но без найденных ссылок -> раздел примечаний */
        if (!genericNotes && isNotesName(baseName)) {
            std::vector<const MNode*> anchors;
            collectAnchors(body, anchors);
            if (!anchors.empty()) {
                for (size_t a = 0; a < anchors.size(); a++) {
                    Note note;
                    note.id = trim(anchors[a]->attr("id"));
                    if (note.id.empty()) continue;
                    if (anchors[a]->name == "p") {
                        std::string h = trim(inlineHtmlOf(anchors[a], ctx));
                        splitNoteNumber(h, note.title);
                        if (!h.empty()) note.blocks.push_back(Block::p(h));
                    } else {
                        note.blocks = blocksOf(anchors[a], ctx);
                    }
                    if (!note.blocks.empty()) book.notes.push_back(note);
                }
                if (!book.notes.empty()) continue;
            }
        }

        std::vector<Chapter> tmp = htmlToChapters(body, ctx, false);

        /* первый заголовок файла встаёт на уровень из оглавления, остальные — относительно него */
        TocDepth::iterator td = tocDepth.find(lowerAscii(path));
        bool titled = false;
        if (td != tocDepth.end()) {
            int delta = 0;
            for (size_t k = 0; k < tmp.size() && !titled; k++)
                if (!tmp[k].title.empty()) { delta = td->second.depth - tmp[k].level; titled = true; }
            for (size_t k = 0; k < tmp.size() && titled; k++)
                if (!tmp[k].title.empty()) tmp[k].level = std::max(0, tmp[k].level + delta);
        }

        /* в файле нет ни одного заголовка, но он есть в оглавлении — берём название оттуда */
        if (td != tocDepth.end() && !titled && !td->second.label.empty()) {
            Chapter c;
            c.level = td->second.depth;
            c.title = escHtml(td->second.label);
            std::string want = toLowerRu(td->second.label);
            for (size_t k = 0; k < tmp.size(); k++) {
                for (size_t j = 0; j < tmp[k].blocks.size(); j++) {
                    const Block& b = tmp[k].blocks[j];
                    /* заголовок, набранный обычным абзацем, повторять не нужно */
                    if (c.blocks.empty() && toLowerRu(collapse(htmlToText(b.html))) == want) continue;
                    c.blocks.push_back(b);
                }
            }
            chapters.push_back(c);
            cur = &chapters[chapters.size() - 1];
            continue;
        }

        if (tmp.size() == 1 && tmp[0].title.empty()) {
            if (!cur->blocks.empty()) {
                Chapter c;
                c.level = 0;
                chapters.push_back(c);
                cur = &chapters[chapters.size() - 1];
            }
            for (size_t k = 0; k < tmp[0].blocks.size(); k++) cur->blocks.push_back(tmp[0].blocks[k]);
        } else {
            for (size_t k = 0; k < tmp.size(); k++) {
                if (!tmp[k].title.empty()) {
                    chapters.push_back(tmp[k]);
                    cur = &chapters[chapters.size() - 1];
                } else {
                    for (size_t j = 0; j < tmp[k].blocks.size(); j++)
                        cur->blocks.push_back(tmp[k].blocks[j]);
                }
            }
        }
    }

    /* ссылки на примечания из отдельных файлов -> внутренние */
    for (size_t i = 0; i < chapters.size(); i++) {
        chapters[i].title = rewriteNoteHrefs(chapters[i].title);
        rewriteBlocksHrefs(chapters[i].blocks);
    }
    for (size_t i = 0; i < book.notes.size(); i++) {
        book.notes[i].title = rewriteNoteHrefs(book.notes[i].title);
        rewriteBlocksHrefs(book.notes[i].blocks);
    }

    /* выкидываем пустые главы */
    book.chapters.clear();
    for (size_t i = 0; i < chapters.size(); i++) {
        bool has = !chapters[i].title.empty();
        for (size_t k = 0; k < chapters[i].blocks.size() && !has; k++)
            if (chapters[i].blocks[k].type != Block::EMPTY) has = true;
        if (has) book.chapters.push_back(chapters[i]);
    }

    finishHtmlBook(book);

    if (book.chapters.empty()) {
        if (err) *err = "В EPUB не нашлось текста";
        return false;
    }
    return true;
}

/* ================================ MOBI ================================= */

static Bytes palmdocDecompress(const Bytes& src) {
    Bytes out;
    out.reserve(src.size() * 3);
    size_t i = 0;
    while (i < src.size()) {
        unsigned char c = src[i++];
        if (c == 0) {
            out.push_back(0);
        } else if (c <= 8) {
            for (unsigned k = 0; k < c && i < src.size(); k++) out.push_back(src[i++]);
        } else if (c <= 0x7F) {
            out.push_back(c);
        } else if (c <= 0xBF) {
            if (i >= src.size()) break;
            unsigned c2 = src[i++];
            unsigned v = (((unsigned)c << 8) | c2) & 0x3FFF;
            unsigned dist = v >> 3;
            unsigned len = (v & 7) + 3;
            if (dist == 0 || dist > out.size()) break;
            for (unsigned k = 0; k < len; k++) out.push_back(out[out.size() - dist]);
        } else {
            /* 0xC0–0xFF: пробел и символ (байт XOR 0x80).
               1.8: раньше здесь выдавалось (c & 0x3F) + 1 пробелов — сжатый текст из kindlegen и
               Calibre терял буквы после пробелов */
            out.push_back(0x20);
            out.push_back((unsigned char)(c ^ 0x80));
        }
    }
    return out;
}

/* Сколько байт в конце текстовой записи занимают «хвосты» (extra record data): kindlegen и
   Calibre дописывают их по флагам из заголовка MOBI. Бит 0 — перекрывающиеся байты
   многобайтного символа, остальные биты — записи с длиной в обратном base-128 в самом конце.
   1.8: раньше хвосты не срезались и попадали в текст мусором. */
static size_t mobiTrailingSize(const Bytes& rec, unsigned flags) {
    size_t size = rec.size(), num = 0;
    for (unsigned f = flags >> 1; f; f >>= 1) {
        if (!(f & 1)) continue;
        if (num >= size) return size;
        /* размер записи: байты с конца, по 7 бит, последний (первый с конца) помечен 0x80 */
        size_t psize = size - num, entry = 0;
        unsigned bitpos = 0;
        for (;;) {
            unsigned char v = rec[psize - 1];
            entry |= (size_t)(v & 0x7F) << bitpos;
            bitpos += 7;
            psize--;
            if ((v & 0x80) || bitpos >= 28 || psize == 0) break;
        }
        num += entry;
    }
    if ((flags & 1) && num < size) num += (rec[size - num - 1] & 0x3) + 1;
    return num > size ? size : num;
}

/* Ссылки MOBI <a filepos="0000123"> указывают байтовое смещение в распакованном тексте.
   Ставим в эти места якоря id="fpN" (в сам тег, если смещение попадает на него) и
   переписываем ссылки в href="#fpN". Вставка идёт с конца, чтобы смещения не съезжали. */
/* Оглавление MOBI (индекс NCX): записи INDX + TAGX + IDXT + строки CNCX.
   1.8: раньше не читалось — у MOBI из Calibre и kindlegen заголовки глав в тексте не <h1>,
   а абзацы с <font size>, и оглавление терялось. Разбор — по описанию формата в Calibre
   (src/calibre/ebooks/mobi/reader/index.py). */
struct NcxEntry { size_t offset; std::string label; int depth; NcxEntry() : offset(0), depth(0) {} };

/* число переменной длины: по 7 бит, старший бит отмечает последний байт */
static size_t mobiDecint(const unsigned char* p, size_t n, unsigned& val) {
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

static void mobiReadNcx(const Bytes& b, const std::vector<unsigned>& offsets, unsigned indx,
                        std::vector<NcxEntry>& out) {
    struct Rec {
        static Bytes get(const Bytes& b, const std::vector<unsigned>& o, size_t i) {
            if (i + 1 >= o.size() || o[i] >= o[i + 1] || o[i + 1] > b.size()) return Bytes();
            return Bytes(b.begin() + o[i], b.begin() + o[i + 1]);
        }
    };
    if (indx == 0 || indx + 1 >= offsets.size()) return;
    Bytes prim = Rec::get(b, offsets, indx);
    if (prim.size() < 56 || memcmp(&prim[0], "INDX", 4) != 0) return;
    unsigned indexRecs = beAt(prim, 24, 4), ncncx = beAt(prim, 52, 4);
    if (indexRecs > 1000 || ncncx > 1000) return;

    /* TAGX: сразу за заголовком; на всякий случай ищем по сигнатуре */
    size_t tx = beAt(prim, 4, 4);
    if (tx + 12 > prim.size() || memcmp(&prim[tx], "TAGX", 4) != 0) {
        tx = std::string::npos;
        for (size_t i = 0; i + 12 <= prim.size(); i++)
            if (memcmp(&prim[i], "TAGX", 4) == 0) { tx = i; break; }
        if (tx == std::string::npos) return;
    }
    unsigned tagxLen = beAt(prim, tx + 4, 4), cbc = beAt(prim, tx + 8, 4);
    if (tagxLen < 12 || tx + tagxLen > prim.size() || cbc == 0 || cbc > 8) return;
    struct Tag { unsigned tag, perEntry, mask, eof; };
    std::vector<Tag> tags;
    for (size_t p = tx + 12; p + 4 <= tx + tagxLen; p += 4) {
        Tag t = {prim[p], prim[p + 1], prim[p + 2], prim[p + 3]};
        tags.push_back(t);
    }

    /* строки CNCX: длина (число переменной длины) + UTF-8; смещение = позиция + 0x10000 * номер записи */
    std::map<unsigned, std::string> cncx;
    for (unsigned k = 0; k < ncncx; k++) {
        Bytes c = Rec::get(b, offsets, indx + indexRecs + 1 + k);
        size_t p = 0;
        while (p < c.size()) {
            unsigned len = 0;
            size_t used = mobiDecint(&c[p], c.size() - p, len);
            if (!used || len > c.size() - p - used) break;
            if (len) cncx[(unsigned)p + 0x10000u * k] = std::string((const char*)&c[p + used], len);
            p += used + len;
        }
    }

    for (unsigned r = 1; r <= indexRecs; r++) {
        Bytes d = Rec::get(b, offsets, indx + r);
        if (d.size() < 28 || memcmp(&d[0], "INDX", 4) != 0) continue;
        unsigned idxt = beAt(d, 20, 4), count = beAt(d, 24, 4);
        if (idxt + 4 > d.size() || count > 65535 || (size_t)idxt + 4 + 2 * (size_t)count > d.size()) continue;
        for (unsigned j = 0; j < count; j++) {
            size_t s = beAt(d, idxt + 4 + 2 * j, 2);
            size_t e = j + 1 < count ? beAt(d, idxt + 4 + 2 * (j + 1), 2) : idxt;
            if (s >= e || e > d.size()) continue;
            size_t p = s;
            p += 1 + d[p];                                  /* имя записи: длина + байты */
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
                        size_t used = mobiDecint(&d[p], e - p, v);
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
            std::map<unsigned, std::vector<unsigned> > tm;
            bool broken = false;
            for (size_t t = 0; t < ptags.size() && !broken; t++) {
                std::vector<unsigned>& vals = tm[ptags[t].tag];
                if (ptags[t].count) {
                    for (unsigned k = 0; k < ptags[t].count * ptags[t].perEntry && k < 64; k++) {
                        unsigned v = 0;
                        size_t used = p < e ? mobiDecint(&d[p], e - p, v) : 0;
                        if (!used) { broken = true; break; }
                        p += used;
                        vals.push_back(v);
                    }
                } else {
                    size_t consumed = 0;
                    while (consumed < ptags[t].bytes) {
                        unsigned v = 0;
                        size_t used = p < e ? mobiDecint(&d[p], e - p, v) : 0;
                        if (!used) { broken = true; break; }
                        p += used;
                        consumed += used;
                        vals.push_back(v);
                    }
                }
            }
            /* теги NCX: 1 — смещение в тексте, 3 — подпись в CNCX, 4 — глубина */
            if (broken || tm[1].empty() || tm[3].empty()) continue;
            std::map<unsigned, std::string>::iterator lab = cncx.find(tm[3][0]);
            if (lab == cncx.end()) continue;
            NcxEntry ne;
            ne.offset = tm[1][0];
            ne.label = collapse(lab->second);
            ne.depth = tm[4].empty() ? 0 : (int)std::min(tm[4][0], 8u);
            if (!ne.label.empty()) out.push_back(ne);
        }
    }
}

/* 1.8: страница «Table of Contents», которую Calibre и kindlegen вставляют в сам текст
   (<guide><reference type="toc" filepos=N>), раньше попадала в книгу списком ссылок — хотя
   оглавление и так строится из глав. Она забивается пробелами той же длины: байтовые
   смещения filepos и NCX после неё не сдвигаются. Только если там действительно ссылки. */
static void mobiBlankHtmlToc(std::string& raw) {
    std::string head = lowerAscii(raw.substr(0, std::min<size_t>(raw.size(), 4096)));
    size_t r = head.find("<reference");
    while (r != std::string::npos) {
        size_t e = head.find('>', r);
        if (e == std::string::npos) return;
        std::string tag = head.substr(r, e - r);
        if (tag.find("type=\"toc\"") != std::string::npos || tag.find("type=toc") != std::string::npos) {
            size_t f = tag.find("filepos=");
            if (f == std::string::npos) return;
            size_t p = f + 8;
            if (p < tag.size() && (tag[p] == '"' || tag[p] == '\'')) p++;
            size_t at = 0;
            while (p < tag.size() && isdigit((unsigned char)tag[p])) at = at * 10 + (size_t)(tag[p++] - '0');
            if (at == 0 || at >= raw.size()) return;
            std::string low = lowerAscii(raw.substr(at, 200000));
            size_t end = low.find("<mbp:pagebreak");
            size_t body = low.find("</body>");
            if (end == std::string::npos || (body != std::string::npos && body < end)) end = body;
            if (end == std::string::npos) end = low.size();
            std::string part = low.substr(0, end);
            size_t links = 0;
            for (size_t q = part.find("filepos="); q != std::string::npos; q = part.find("filepos=", q + 8)) links++;
            /* оглавление — это почти одни ссылки: не меньше двух и мало текста вне них */
            if (links < 2 || htmlToText(part).size() > links * 200) return;
            for (size_t i = 0; i < end; i++) raw[at + i] = ' ';
            return;
        }
        r = head.find("<reference", e);
    }
}

/* Номер размера шрифта первого <font size="N"> в куске HTML, 0 — нет */
static int mobiFontSize(const std::string& low) {
    size_t f = low.find("<font size=");
    if (f == std::string::npos) return 0;
    size_t p = f + 11;
    if (p < low.size() && (low[p] == '"' || low[p] == '\'')) p++;
    int v = 0;
    while (p < low.size() && isdigit((unsigned char)low[p])) v = v * 10 + (low[p++] - '0');
    return v;
}

static void mobiResolveFilepos(std::string& raw, const std::vector<NcxEntry>& ncx) {
    /* заголовки по оглавлению NCX: абзац на смещении записи становится <hN> (если его текст —
       это и есть подпись записи), иначе <hN> с подписью вставляется перед ним */
    std::map<size_t, std::pair<std::string, int> > heads;
    {
        bool anyDepth = false;
        for (size_t i = 0; i < ncx.size(); i++) if (ncx[i].depth > 0) anyDepth = true;
        /* у MOBI6 от Calibre глубина везде 0 — уровень по размеру шрифта заголовка */
        std::set<int> sizes;
        std::vector<int> sz(ncx.size(), 0);
        for (size_t i = 0; i < ncx.size(); i++) {
            if (ncx[i].offset >= raw.size()) continue;
            std::string head = lowerAscii(raw.substr(ncx[i].offset, 400));
            size_t e = head.find("</p>");
            sz[i] = mobiFontSize(e == std::string::npos ? head : head.substr(0, e));
            if (sz[i]) sizes.insert(sz[i]);
        }
        for (size_t i = 0; i < ncx.size(); i++) {
            if (ncx[i].offset >= raw.size()) continue;
            int level = 1;
            if (anyDepth) level = ncx[i].depth + 1;
            else if (sz[i]) {
                level = 1;
                for (std::set<int>::reverse_iterator it = sizes.rbegin(); it != sizes.rend() && *it > sz[i]; ++it) level++;
            } else if (!sizes.empty()) level = (int)sizes.size();
            heads[ncx[i].offset] = std::make_pair(ncx[i].label, std::min(level, 6));
        }
    }

    std::string low = lowerAscii(raw);
    std::set<size_t> targets;
    size_t pos = 0;
    while ((pos = low.find("filepos=", pos)) != std::string::npos) {
        size_t p = pos + 8;
        if (p < raw.size() && (raw[p] == '"' || raw[p] == '\'')) p++;
        size_t v = 0, digits = 0;
        while (p < raw.size() && isdigit((unsigned char)raw[p]) && digits < 10) {
            v = v * 10 + (size_t)(raw[p] - '0');
            p++;
            digits++;
        }
        if (digits && v < raw.size()) targets.insert(v);
        pos = p;
    }
    if (targets.empty() && heads.empty()) return;

    /* правки идут с конца текста, чтобы байтовые смещения перед ними не съезжали */
    std::set<size_t> all(targets);
    for (std::map<size_t, std::pair<std::string, int> >::iterator h = heads.begin(); h != heads.end(); ++h)
        all.insert(h->first);
    for (std::set<size_t>::reverse_iterator it = all.rbegin(); it != all.rend(); ++it) {
        size_t at = *it;
        std::string id = "fp" + intToStr((long long)at);
        bool isTarget = targets.count(at) != 0;
        std::map<size_t, std::pair<std::string, int> >::iterator h = heads.find(at);
        if (h != heads.end()) {
            std::string hn = "h" + intToStr(h->second.second);
            std::string idAttr = isTarget ? " id=\"" + id + "\"" : std::string();
            std::string tag = "<" + hn + idAttr + ">" + escHtml(h->second.first) + "</" + hn + ">";
            /* абзац-заголовок целиком заменяется, если в нём ровно подпись и нет других якорей */
            size_t end = std::string::npos;
            if (raw.compare(at, 2, "<p") == 0 || raw.compare(at, 2, "<P") == 0) {
                size_t close = lowerAscii(raw.substr(at, 2000)).find("</p>");
                if (close != std::string::npos) {
                    std::string inner = raw.substr(at, close + 4);
                    bool otherTarget = false;
                    std::set<size_t>::iterator nx = all.upper_bound(at);
                    if (nx != all.end() && *nx < at + close + 4) otherTarget = true;
                    if (!otherTarget && toLowerRu(collapse(htmlToText(inner))) == toLowerRu(h->second.first))
                        end = at + close + 4;
                }
            }
            if (end != std::string::npos) raw.replace(at, end - at, tag);
            else raw.insert(at, tag);
            continue;
        }
        if (raw[at] == '<' && at + 1 < raw.size() && isalpha((unsigned char)raw[at + 1])) {
            size_t e = at + 1;
            while (e < raw.size() && (isalnum((unsigned char)raw[e]) || raw[e] == ':')) e++;
            raw.insert(e, " id=\"" + id + "\"");
        } else {
            raw.insert(at, "<a id=\"" + id + "\"></a>");
        }
    }

    std::string out;
    low = lowerAscii(raw);
    pos = 0;
    for (;;) {
        size_t p = low.find("filepos=", pos);
        if (p == std::string::npos) { out.append(raw, pos, std::string::npos); break; }
        out.append(raw, pos, p - pos);
        size_t q = p + 8;
        bool quoted = q < raw.size() && (raw[q] == '"' || raw[q] == '\'');
        if (quoted) q++;
        size_t v = 0, digits = 0;
        while (q < raw.size() && isdigit((unsigned char)raw[q]) && digits < 10) {
            v = v * 10 + (size_t)(raw[q] - '0');
            q++;
            digits++;
        }
        if (quoted && q < raw.size() && (raw[q] == '"' || raw[q] == '\'')) q++;
        out += "href=\"#fp" + intToStr((long long)v) + "\"";
        pos = q;
    }
    raw.swap(out);
}

static bool isNotesTitle(const std::string& titleHtml) {
    std::string t = toLowerRu(collapse(htmlToText(titleHtml)));
    /* 1.8: «заметки» — так Calibre по-русски озаглавливает сноски в MOBI */
    return t == "примечания" || t == "сноски" || t == "комментарии" || t == "примечания и комментарии" ||
           t == "заметки" || t == "notes" || t == "footnotes" || t == "endnotes";
}

/* Раздел «Примечания», на абзацы которого ведут ссылки, -> настоящие сноски книги. */
static void extractLinkedNotes(Book& book) {
    std::set<std::string> linked;
    for (size_t i = 0; i < book.chapters.size(); i++) {
        for (size_t k = 0; k < book.chapters[i].blocks.size(); k++) {
            const std::string& h = book.chapters[i].blocks[k].html;
            size_t pos = 0;
            while ((pos = h.find("href=\"#", pos)) != std::string::npos) {
                size_t q = h.find('"', pos + 7);
                if (q == std::string::npos) break;
                linked.insert(h.substr(pos + 7, q - pos - 7));
                pos = q;
            }
        }
    }
    if (linked.empty()) return;

    for (size_t i = book.chapters.size(); i-- > 0;) {
        Chapter& ch = book.chapters[i];
        if (!isNotesTitle(ch.title)) continue;
        std::vector<Note> notes;
        bool ok = true;
        for (size_t k = 0; k < ch.blocks.size(); k++) {
            Block& bl = ch.blocks[k];
            if (bl.type == Block::EMPTY) continue;
            if (!bl.id.empty() && linked.count(bl.id)) {
                Note n;
                n.id = bl.id;
                std::string h = trim(bl.html);
                splitNoteNumber(h, n.title);
                bl.html = h;
                bl.id.clear();
                n.blocks.push_back(bl);
                notes.push_back(n);
            } else if (!notes.empty()) {
                notes.back().blocks.push_back(bl);
            } else {
                ok = false; /* текст до первой сноски — это обычная глава */
                break;
            }
        }
        if (!ok || notes.empty()) continue;
        book.notes.insert(book.notes.begin(), notes.begin(), notes.end());
        book.chapters.erase(book.chapters.begin() + i);
    }
}

static bool readMOBI(const Bytes& b, const std::string& fileName, Book& book, std::string* err) {
    if (b.size() < 100) {
        if (err) *err = "Файл слишком мал для MOBI";
        return false;
    }
    /* PalmDB и MOBI — big-endian. 1.8: раньше читалось как little-endian, поэтому настоящие
       MOBI из Calibre и kindlegen не открывались («Повреждён первый блок MOBI») */
    unsigned numRecords = ((unsigned)b[76] << 8) | (unsigned)b[77];
    if (numRecords < 2) {
        if (err) *err = "Повреждён заголовок PalmDB";
        return false;
    }
    std::vector<unsigned> offsets;
    for (unsigned i = 0; i < numRecords; i++) {
        size_t p = 78 + i * 8;
        if (p + 4 > b.size()) break;
        offsets.push_back(((unsigned)b[p] << 24) | ((unsigned)b[p + 1] << 16) |
                          ((unsigned)b[p + 2] << 8) | (unsigned)b[p + 3]);
    }
    offsets.push_back((unsigned)b.size());
    if (offsets.size() < 3) {
        if (err) *err = "Повреждён заголовок PalmDB";
        return false;
    }

    size_t r0start = offsets[0], r0end = offsets[1];
    if (r0start >= b.size() || r0end > b.size() || r0end <= r0start) {
        if (err) *err = "Повреждён первый блок MOBI";
        return false;
    }
    Bytes r0(b.begin() + r0start, b.begin() + r0end);
    #define R0U16(p) ((unsigned)(((p) + 2 <= r0.size()) ? (((unsigned)r0[p] << 8) | (unsigned)r0[p+1]) : 0))
    #define R0U32(p) ((unsigned)(((p) + 4 <= r0.size()) ? (((unsigned)r0[p] << 24) | ((unsigned)r0[p+1] << 16) | ((unsigned)r0[p+2] << 8) | (unsigned)r0[p+3]) : 0))

    unsigned compression = R0U16(0);
    unsigned encryption = R0U16(12);
    if (encryption) {
        if (err) *err = "Файл защищён DRM — конвертация невозможна";
        return false;
    }
    unsigned textRecords = R0U16(8);
    unsigned encoding = 1252;
    unsigned fullNameOffset = 0, fullNameLength = 0, exthFlags = 0, firstImageIndex = 0;
    unsigned mobiVersion = 6;
    unsigned extraFlags = 0;   /* какие «хвосты» дописаны в конец каждой текстовой записи */
    unsigned ncxIndex = 0xFFFFFFFFu;

    if (r0.size() >= 20 && r0[16] == 'M' && r0[17] == 'O' && r0[18] == 'B' && r0[19] == 'I') {
        unsigned headerLen = R0U32(16 + 4);
        encoding = R0U32(16 + 12);
        mobiVersion = R0U32(16 + 20);
        fullNameOffset = R0U32(16 + 0x44);
        fullNameLength = R0U32(16 + 0x48);
        firstImageIndex = R0U32(16 + 0x5C);
        exthFlags = R0U32(16 + 0x70);
        /* как в Calibre: флаги есть при длине заголовка от 0xE4, значимы младшие 16 бит */
        if (headerLen >= 0xE4) extraFlags = R0U16(16 + 0xE2);
        if (headerLen >= 0xE8) ncxIndex = R0U32(16 + 0xE4);   /* запись INDX оглавления */
    }
    if (mobiVersion >= 8) {
        if (err) *err = "Это AZW3/KF8 — такие файлы здесь не читаются. Сконвертируйте их в FB2 через Calibre.";
        return false;
    }

    book.fileName = fileName;

    std::string title;
    /* 1.8: проверки без сложения — в 32-битной сборке offset + length из битого файла
       переполнялись, проходили проверку и роняли программу (нашёл fuzz.ps1) */
    if (fullNameLength && fullNameOffset && fullNameOffset < r0.size() &&
        fullNameLength <= r0.size() - fullNameOffset) {
        std::string raw((const char*)&r0[fullNameOffset], fullNameLength);
        title = (encoding == 65001) ? raw : cpToUtf8(raw, 1252);
        while (!title.empty() && title[title.size() - 1] == '\0') title.erase(title.size() - 1);
    }

    long coverOffset = -1;
    if (exthFlags & 0x40) {
        unsigned mobiLen = R0U32(16 + 4);
        size_t base = mobiLen < r0.size() ? 16 + (size_t)mobiLen : r0.size();
        if (base + 12 <= r0.size() && r0[base] == 'E' && r0[base + 1] == 'X' &&
            r0[base + 2] == 'T' && r0[base + 3] == 'H') {
            unsigned count = R0U32(base + 8);
            size_t p = base + 12;
            for (unsigned i = 0; i < count && p + 8 <= r0.size(); i++) {
                unsigned type = R0U32(p);
                unsigned len = R0U32(p + 4);
                if (len < 8 || len > r0.size() - p) break;
                std::string raw((const char*)&r0[p + 8], len - 8);
                std::string val = raw;
                if (type == 100 && !raw.empty()) {
                    Author a = splitAuthor(val, false);
                    if (!fullName(a).empty()) book.meta.authors.push_back(a);
                } else if (type == 503 && !val.empty() && title.empty()) {
                    title = val;
                } else if (type == 524 && !val.empty()) {
                    book.meta.lang = val;
                } else if (type == 201 && raw.size() >= 4) {
                    coverOffset = (long)(((unsigned char)raw[0] << 24) | ((unsigned char)raw[1] << 16) |
                                         ((unsigned char)raw[2] << 8) | (unsigned char)raw[3]);
                }
                p += len;
            }
        }
    }

    if (title.empty()) {
        size_t n = std::min<size_t>(32, b.size());
        std::string raw((const char*)&b[0], n);
        size_t z = raw.find('\0');
        if (z != std::string::npos) raw = raw.substr(0, z);
        title = raw;
    }
    book.meta.title = collapse(title);

    if (compression == 17480) {
        if (err) *err = "Сжатие HUFF/CDIC не поддерживается";
        return false;
    }
    if (!textRecords || textRecords + 1 > offsets.size()) textRecords = (unsigned)offsets.size() - 2;

    Bytes textBytes;
    for (unsigned i = 1; i <= textRecords; i++) {
        if (i + 1 >= offsets.size()) break;
        size_t a = offsets[i], z = offsets[i + 1];
        if (a > b.size() || z > b.size() || z < a) break;
        Bytes rec(b.begin() + a, b.begin() + z);
        size_t tail = mobiTrailingSize(rec, extraFlags);
        rec.resize(tail < rec.size() ? rec.size() - tail : 0);
        if (compression == 1) {
            textBytes.insert(textBytes.end(), rec.begin(), rec.end());
        } else if (compression == 2) {
            Bytes dec = palmdocDecompress(rec);
            textBytes.insert(textBytes.end(), dec.begin(), dec.end());
        } else {
            if (err) *err = "Неизвестное сжатие MOBI: " + intToStr(compression);
            return false;
        }
    }
    if (textBytes.empty()) {
        if (err) *err = "Не удалось прочитать текст MOBI";
        return false;
    }
    std::string rawText;
    {
        std::string rawAll((const char*)&textBytes[0], textBytes.size());
        std::vector<NcxEntry> ncx;
        if (ncxIndex != 0xFFFFFFFFu) mobiReadNcx(b, offsets, ncxIndex, ncx);
        mobiBlankHtmlToc(rawAll);
        mobiResolveFilepos(rawAll, ncx);
        if (encoding == 65001) {
            rawText = rawAll;
        } else {
            /* В MOBI обычно cp1252, но русские книги часто в cp1251.
               Считаем кириллицу в варианте cp1251 и «акценты» в cp1252. */
            std::string a = cpToUtf8(rawAll, 1251);
            std::string b = cpToUtf8(rawAll, 1252);
            size_t limit = std::min<size_t>(a.size() / 1, 200000);
            int cyr = 0, acc = 0;
            for (size_t i = 0, idx = 0; i < limit; idx++) {
                unsigned cp = utf8CharAt(a, idx);
                if (!cp) break;
                i += (cp < 0x80) ? 1 : ((cp < 0x800) ? 2 : 3);
                if (cp >= 0x400 && cp <= 0x4FF) cyr++;
                unsigned cp2 = utf8CharAt(b, idx);
                if (cp2 >= 0xC0 && cp2 <= 0xFF) acc++;
            }
            rawText = (cyr > 30 && cyr > acc * 2) ? a : b;
        }
    }
    for (size_t i = 0; i < rawText.size(); i++)
        if (rawText[i] == '\0') rawText.erase(i--, 1);
    /* <mbp:pagebreak ...> -> <hr/> */
    {
        std::string lowText = lowerAscii(rawText);
        std::string out;
        size_t pos = 0;
        for (;;) {
            size_t p = lowText.find("<mbp:pagebreak", pos);
            if (p == std::string::npos) { out.append(rawText, pos, std::string::npos); break; }
            out.append(rawText, pos, p - pos);
            size_t gt = rawText.find('>', p);
            out += "<hr/>";
            if (gt == std::string::npos) { pos = rawText.size(); break; }
            pos = gt + 1;
        }
        rawText.swap(out);
    }

    /* картинки */
    std::vector<std::string> recImages;
    if (firstImageIndex > 0 && firstImageIndex + 1 < offsets.size()) {
        for (unsigned i = firstImageIndex; i + 1 < offsets.size(); i++) {
            size_t a = offsets[i], z = offsets[i + 1];
            if (a >= b.size() || z > b.size() || z <= a) continue;
            Bytes rec(b.begin() + a, b.begin() + z);
            std::string mime = "image/jpeg";
            if (rec.size() >= 4 && rec[0] == 0x89 && rec[1] == 0x50 && rec[2] == 0x4E && rec[3] == 0x47)
                mime = "image/png";
            else if (rec.size() >= 4 && rec[0] == 0x47 && rec[1] == 0x49 && rec[2] == 0x46 && rec[3] == 0x38)
                mime = "image/gif";
            else if (rec.size() >= 2 && rec[0] == 0xFF && rec[1] == 0xD8)
                mime = "image/jpeg";
            else if (rec.size() >= 2 && rec[0] == 0x42 && rec[1] == 0x4D)
                mime = "image/bmp";
            else {
                /* не картинка (например, маркер конца файла) — сохраняем позицию */
                recImages.push_back(std::string());
                continue;
            }
            std::string name = addImage(book, mime, rec);
            recImages.push_back(name);
        }
    }
    if (coverOffset >= 0 && coverOffset < (long)recImages.size() && !recImages[(size_t)coverOffset].empty())
        book.coverName = recImages[(size_t)coverOffset];

    /* подмена recindex -> mobi:N */
    MNode doc = parseMarkup(rawText, true);
    std::vector<const MNode*> imgs = allNamed(&doc, "img");
    for (size_t i = 0; i < imgs.size(); i++) {
        std::string recindex = imgs[i]->attr("recindex");
        int idx = parseIntSafe(recindex, 0);
        if (idx <= 0 || idx > (int)recImages.size()) continue;
        /* MNode не const по факту: allNamed вернул const-указатели */
        MNode* m = const_cast<MNode*>(imgs[i]);
        for (size_t k = 0; k < m->attrs.size(); k++)
            if (m->attrs[k].first == "src") m->attrs[k].second = "mobi:" + intToStr(idx);
        bool hasSrc = false;
        for (size_t k = 0; k < m->attrs.size(); k++)
            if (m->attrs[k].first == "src") hasSrc = true;
        if (!hasSrc) m->attrs.push_back(std::make_pair(std::string("src"), "mobi:" + intToStr(idx)));
    }

    const MNode* body = NULL;
    std::vector<const MNode*> bodies = allNamed(&doc, "body");
    if (!bodies.empty()) body = bodies[0];
    if (!body) {
        for (size_t i = 0; i < doc.kids.size(); i++)
            if (!doc.kids[i]->text) { body = doc.kids[i].get(); break; }
    }
    if (!body) body = &doc;

    HtmlCtx ctx;
    ctx.book = &book;
    std::map<std::string, std::string> mobiMap;
    for (size_t i = 0; i < recImages.size(); i++) mobiMap[intToStr((long long)i + 1)] = recImages[i];
    ctx.mobiImages = &mobiMap;

    NoteRefs refs;
    std::set<const MNode*> skip;
    std::set<std::string> taken;
    collectNoteRefs(body, refs, 0);
    extractNotes(body, refs, ctx, skip, taken);
    ctx.skip = &skip;
    book.chapters = htmlToChapters(body, ctx);
    if (book.notes.empty()) extractLinkedNotes(book);
    finishHtmlBook(book);
    if (book.chapters.empty()) {
        if (err) *err = "В MOBI не нашлось текста";
        return false;
    }
    /* Обложка уже описана в coverpage — не дублируем её первой страницей текста. */
    if (!book.coverName.empty() && !book.chapters.empty()) {
        Chapter& first = book.chapters[0];
        if (first.title.empty() && !first.blocks.empty() &&
            first.blocks[0].type == Block::IMG &&
            pathBase(first.blocks[0].src) == book.coverName) {
            first.blocks.erase(first.blocks.begin());
            if (first.blocks.empty()) book.chapters.erase(book.chapters.begin());
        }
    }
    return true;
    #undef R0U16
    #undef R0U32
}

/* ============================== диспетчер ============================== */

bool readRTF(const Bytes& data, const std::string& fileName, Book& book, std::string* err); /* rtf.cpp */
bool readPDF(const Bytes& data, const std::string& fileName, Book& book, std::string* err); /* pdf.cpp */
bool readDOC(const Bytes& data, const std::string& fileName, Book& book, std::string* err); /* doc.cpp */
bool readDJVU(const Bytes& data, const std::string& fileName, Book& book, std::string* err); /* djvu.cpp */

bool readBook(const std::string& fileName, const Bytes& data, Book& out, std::string* err) {
    out = Book();
    std::string ext = pathExt(fileName);

    if (ext == "zip" || ext == "fbz") {
        std::map<std::string, Bytes> files;
        std::string zerr;
        if (!zipRead(data, files, &zerr)) {
            if (err) *err = zerr;
            return false;
        }
        for (std::map<std::string, Bytes>::iterator it = files.begin(); it != files.end(); ++it) {
            if (endsWithNoCase(it->first, ".fb2")) {
                Book inner;
                if (!readFB2(it->second, fileName, inner, err)) return false;
                out = inner;
                return true;
            }
        }
        if (err) *err = "В архиве нет FB2-файла";
        return false;
    }

    if (ext == "fb2") return readFB2(data, fileName, out, err);
    if (ext == "txt") return readTXT(data, fileName, out, err);
    if (ext == "html" || ext == "htm" || ext == "xhtml") return readHTML(data, fileName, out, err);
    if (ext == "docx") return readDOCX(data, fileName, out, err);
    if (ext == "odt") return readODT(data, fileName, out, err);
    if (ext == "rtf") return readRTF(data, fileName, out, err);
    if (ext == "pdf") return readPDF(data, fileName, out, err);
    if (ext == "doc" || ext == "dot") return readDOC(data, fileName, out, err);
    if (ext == "djvu" || ext == "djv") return readDJVU(data, fileName, out, err);
    if (ext == "epub") return readEPUB(data, fileName, out, err);
    if (ext == "mobi" || ext == "azw" || ext == "azw3" || ext == "prc")
        return readMOBI(data, fileName, out, err);

    /* последняя попытка: определить по содержимому */
    if (data.size() > 5 && memcmp(&data[0], "{\\rtf", 5) == 0) return readRTF(data, fileName, out, err);
    if (data.size() > 5 && memcmp(&data[0], "%PDF-", 5) == 0) return readPDF(data, fileName, out, err);
    if (data.size() > 12 && memcmp(&data[0], "AT&TFORM", 8) == 0) return readDJVU(data, fileName, out, err);
    if (data.size() > 8 && memcmp(&data[0], "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0) return readDOC(data, fileName, out, err);
    if (data.size() > 4 && data[0] == 'P' && data[1] == 'K') {
        std::map<std::string, Bytes> files;
        std::string zerr;
        if (zipRead(data, files, &zerr)) {
            if (files.find("word/document.xml") != files.end()) return readDOCX(data, fileName, out, err);
            if (files.find("content.xml") != files.end() && files.find("META-INF/container.xml") == files.end())
                return readODT(data, fileName, out, err);
            return readEPUB(data, fileName, out, err);
        }
    }
    if (err) *err = "Формат не поддерживается";
    return false;
}
