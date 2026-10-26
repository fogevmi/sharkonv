#include "book.h"

static const char* kMimeByExt[][2] = {
    {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"png", "image/png"},
    {"gif", "image/gif"}, {"svg", "image/svg+xml"}, {"webp", "image/webp"},
    {"bmp", "image/bmp"}, {"tif", "image/tiff"}, {"tiff", "image/tiff"},
    {NULL, NULL}
};

std::string extFromMime(const std::string& mime) {
    std::string m = lowerAscii(mime);
    if (m == "image/jpeg" || m == "image/jpg") return "jpg";
    if (m == "image/png") return "png";
    if (m == "image/gif") return "gif";
    if (m == "image/svg+xml") return "svg";
    if (m == "image/webp") return "webp";
    if (m == "image/bmp") return "bmp";
    if (m == "image/tiff") return "tif";
    size_t p = m.find('/');
    if (p != std::string::npos) {
        std::string e = m.substr(p + 1);
        if (e == "jpeg") e = "jpg";
        if (e.size() >= 1 && e.size() <= 5) return e;
    }
    return "jpg";
}

std::string mimeFromExt(const std::string& ext) {
    std::string e = lowerAscii(ext);
    if (e == "jpeg") e = "jpg";
    if (e == "tif") e = "tiff";
    for (int i = 0; kMimeByExt[i][0]; i++)
        if (e == kMimeByExt[i][0]) return kMimeByExt[i][1];
    return "image/jpeg";
}

std::string addImage(Book& book, const std::string& mime, const Bytes& data) {
    if (data.empty()) return std::string();
    /* FNV-1a по содержимому: одинаковые картинки не дублируем */
    unsigned h = 2166136261u;
    for (size_t i = 0; i < data.size(); i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    std::string key = intToStr((long long)data.size()) + ":" + intToStr(h);
    std::map<std::string, std::string>::iterator it = book.imageHash.find(key);
    if (it != book.imageHash.end()) return it->second;

    std::string md = mime.empty() ? "image/jpeg" : mime;
    std::string ext = extFromMime(md);
    int n = (int)book.images.size() + 1;
    std::string name = "img-" + padNum(n, 3) + "." + ext;
    while (book.images.find(name) != book.images.end()) {
        n++;
        name = "img-" + padNum(n, 3) + "." + ext;
    }
    Image img;
    img.name = name;
    img.mime = md;
    img.bytes = data;
    book.images[name] = img;
    book.imageHash[key] = name;
    return name;
}

std::string addImageDataUrl(Book& book, const std::string& url) {
    if (!startsWith(lowerAscii(url), "data:")) return std::string();
    size_t comma = url.find(',');
    if (comma == std::string::npos) return std::string();
    std::string head = url.substr(5, comma - 5);
    std::string body = url.substr(comma + 1);
    std::string mime = "image/jpeg";
    bool isB64 = false;
    size_t semi = head.find(';');
    if (semi == std::string::npos) {
        if (!head.empty()) mime = head;
    } else {
        if (semi > 0) mime = head.substr(0, semi);
        isB64 = lowerAscii(head).find("base64") != std::string::npos;
    }
    Bytes data;
    if (isB64) data = b64decode(body);
    else data.assign(body.begin(), body.end());
    if (data.empty()) return std::string();
    return addImage(book, mime, data);
}

std::string authorsLine(const Book& book) {
    std::string s;
    for (size_t i = 0; i < book.meta.authors.size(); i++) {
        std::string f = fullName(book.meta.authors[i]);
        if (f.empty()) continue;
        if (!s.empty()) s += ", ";
        s += f;
    }
    return s;
}

/* -------------------------- догадка по имени файла ----------------------- */

struct Guess { std::string title, author; bool authorFamilyFirst; Guess() : authorFamilyFirst(false) {} };

static Guess guessMetaFromName(const std::string& fileName) {
    Guess g;
    std::string base = pathBase(fileName);
    for (int i = 0; i < 3; i++) {
        std::string e = pathExt(base);
        if (e == "fb2" || e == "txt" || e == "html" || e == "htm" || e == "docx" ||
            e == "epub" || e == "mobi" || e == "azw" || e == "azw3" || e == "prc" ||
            e == "zip" || e == "fbz" || e == "pdf" || e == "doc" || e == "dot" || e == "odt" ||
            e == "rtf" || e == "djvu" || e == "djv")
            base = pathStripExt(base);
        else
            break;
    }
    base = collapse(replaceAll(base, "_", " "));
    if (base.empty()) return g;

    size_t ob = base.find_first_of("([");
    size_t cb = base.find_last_of(")]");
    if (ob != std::string::npos && cb != std::string::npos && cb > ob && cb == base.size() - 1) {
        std::string before = collapse(base.substr(0, ob));
        std::string inside = collapse(base.substr(ob + 1, cb - ob - 1));
        if (!before.empty() && !inside.empty()) {
            g.title = before;
            g.author = inside;
            g.authorFamilyFirst = true;
            return g;
        }
    }
    /* «Автор - Название» (тире с пробелами) */
    const char* dashes[] = {" \xE2\x80\x94 ", " \xE2\x80\x93 ", " - ", NULL};
    for (int i = 0; dashes[i]; i++) {
        size_t p = base.find(dashes[i]);
        if (p != std::string::npos && p > 0) {
            g.author = collapse(base.substr(0, p));
            g.title = collapse(base.substr(p + strlen(dashes[i])));
            g.authorFamilyFirst = true;
            if (!g.author.empty() && !g.title.empty()) return g;
        }
    }
    g.title = base;
    return g;
}

/* ------------------------------ нормализация ---------------------------- */

static bool isFb2Genre(const std::string& g) {
    static const char* list =
        "adv_animal adv_geo adv_history adv_indian adv_maritime adv_other adv_space adv_story adv_western "
        "antique antique_ant antique_eur antique_myths antique_russian aphorisms architecture architecture_book art art_criticism "
        "art_history astronomy bank biology biogr_autobiography biogr_memoirs biz child_adv child_det child_education child_prose "
        "child_sf child_tale child_verse comp_db comp_hard comp_net comp_osnet comp_programming comp_soft comp_www computers cultural "
        "culture design detective detective_ironic detective_rus detective_world dramaturgy drawing economics epic essay fairytale family "
        "fantasy fantasy_alt_hist fantasy_epic fantasy_fairy fantasy_hero fantasy_historical fantasy_hum fantasy_other folklore "
        "foreign_antique foreign_classic foreign_contemporary foreign_children foreign_edu foreign_history foreign_hum foreign_legal "
        "foreign_prose foreign_publicism foreign_sf geo_guide geo_history geography_book global_economy gothic historical history home "
        "home_health home_pets home_plants housekeeping humor humor_anecdote humor_prose humor_verse industry jewelry journalism "
        "job_hunting languages law_detective linguistics literary_criticism love love_contemporary love_erotica love_history love_short "
        "love_sf manga medicine military military_africa military_history military_weapon music nature nonf_biography nonf_criticism "
        "nonf_history nonf_publicism notes nsf pedagogy politics poetry prose prose_absurd prose_classic prose_counter prose_history "
        "prose_military prose_other prose_rus_classic professions psy psychiatry psychology radio reference religion religion_esoterics "
        "religion_history religion_rel romance_sf sci_biology sci_chem sci_cosmos sci_ecology sci_juris sci_math sci_medicine sci_phys "
        "sci_psychology sci_tech sci_text science sf sf_action sf_alternative sf_cyberpunk sf_detective sf_epic sf_fantasy sf_heroic "
        "sf_history sf_horror sf_humor sf_space sf_stars sf_social sf_etc sf_technological stock story thriller thriller_medical "
        "thriller_psychology thriller_spy upbringing vaudeville visual_arts zoology short_story";
    std::string padded = std::string(" ") + list + " ";
    return padded.find(" " + g + " ") != std::string::npos;
}

void normalizeBook(Book& book) {
    Guess g = guessMetaFromName(book.fileName);
    Meta& m = book.meta;
    if (m.title.empty() && !g.title.empty()) m.title = g.title;
    if (m.authors.empty() && !g.author.empty()) {
        Author a = splitAuthor(g.author, g.authorFamilyFirst);
        if (!fullName(a).empty()) m.authors.push_back(a);
    }
    m.title = collapse(m.title);
    /* убираем управляющие символы */
    for (size_t i = 0; i < m.title.size(); i++)
        if ((unsigned char)m.title[i] < 0x20) m.title.erase(i--, 1);
    if (m.title.empty()) m.title = "Без названия";
    if (m.lang.empty()) m.lang = "ru";

    std::vector<Author> ok;
    for (size_t i = 0; i < m.authors.size(); i++) {
        Author a = m.authors[i];
        a.first = collapse(a.first);
        a.middle = collapse(a.middle);
        a.last = collapse(a.last);
        a.nick = collapse(a.nick);
        if (!fullName(a).empty()) ok.push_back(a);
    }
    m.authors = ok;

    std::vector<std::string> genres;
    for (size_t i = 0; i < m.genres.size() && genres.size() < 4; i++) {
        std::string gg = trim(m.genres[i]);
        if (isFb2Genre(gg)) genres.push_back(gg);
    }
    m.genres = genres;
}

std::string outBaseName(const Book& book) {
    std::string a = authorsLine(book);
    std::string t = book.meta.title.empty() ? "book" : book.meta.title;
    std::string raw = a.empty() ? t : (t + " — " + a);
    return safeFileName(raw, "book");
}

namespace {

struct TreeBuilder {
    const std::vector<Chapter>& chapters;
    const std::vector<std::vector<int> >& childrenOf;

    TreeBuilder(const std::vector<Chapter>& c, const std::vector<std::vector<int> >& ch)
        : chapters(c), childrenOf(ch) {}

    ChapterNode make(int i) const {
        ChapterNode n;
        n.level = chapters[(size_t)i].level;
        n.title = chapters[(size_t)i].title;
        n.blocks = &chapters[(size_t)i].blocks;
        const std::vector<int>& ch = childrenOf[(size_t)i];
        for (size_t k = 0; k < ch.size(); k++) n.children.push_back(make(ch[k]));
        return n;
    }
};

} /* namespace */

std::vector<ChapterNode> buildTree(const std::vector<Chapter>& chapters) {
    struct Flat { int level; int parent; size_t index; };
    std::vector<Flat> flat;
    for (size_t i = 0; i < chapters.size(); i++) {
        int level = chapters[i].level;
        int parent = -1;
        for (int k = (int)flat.size() - 1; k >= 0; k--) {
            if (flat[k].level < level) { parent = k; break; }
        }
        Flat f;
        f.level = level;
        f.parent = parent;
        f.index = i;
        flat.push_back(f);
    }
    std::vector<std::vector<int> > childrenOf(flat.size());
    std::vector<int> top;
    for (size_t i = 0; i < flat.size(); i++) {
        if (flat[i].parent < 0) top.push_back((int)i);
        else childrenOf[(size_t)flat[i].parent].push_back((int)i);
    }

    TreeBuilder b(chapters, childrenOf);
    std::vector<ChapterNode> out;
    for (size_t i = 0; i < top.size(); i++) out.push_back(b.make(top[i]));
    return out;
}

/* Уровни глав: верхний уровень книги — нулевой, скачки через уровень сглаживаются
   (h1 -> h3 без h2 становится 0 -> 1). Главы без заголовка наследуют уровень предыдущей. */
void normalizeChapterLevels(std::vector<Chapter>& chapters) {
    int minLvl = -1;
    for (size_t i = 0; i < chapters.size(); i++)
        if (!chapters[i].title.empty() && (minLvl < 0 || chapters[i].level < minLvl)) minLvl = chapters[i].level;
    int prev = -1;
    for (size_t i = 0; i < chapters.size(); i++) {
        Chapter& c = chapters[i];
        if (c.title.empty()) { c.level = prev < 0 ? 0 : prev; continue; }
        c.level = std::max(0, c.level - std::max(0, minLvl));
        if (c.level > prev + 1) c.level = prev + 1;
        prev = c.level;
    }
}
