#include "common.h"
#include <objbase.h>

/* ============================== строки ================================= */

static bool isSpaceByte(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isSpaceByte((unsigned char)s[a])) a++;
    while (b > a && isSpaceByte((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string collapse(const std::string& in) {
    std::string s = replaceAll(in, "\xC2\xA0", " ");
    std::string out;
    out.reserve(s.size());
    bool sp = true;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (isSpaceByte(c)) {
            if (!sp) { out += ' '; sp = true; }
        } else {
            out += (char)c;
            sp = false;
        }
    }
    if (!out.empty() && out[out.size() - 1] == ' ') out.erase(out.size() - 1);
    return out;
}

bool startsWith(const std::string& s, const char* p) {
    size_t n = strlen(p);
    return s.size() >= n && memcmp(s.data(), p, n) == 0;
}

std::string lowerAscii(const std::string& s) {
    std::string r = s;
    for (size_t i = 0; i < r.size(); i++)
        if (r[i] >= 'A' && r[i] <= 'Z') r[i] = (char)(r[i] - 'A' + 'a');
    return r;
}

bool endsWithNoCase(const std::string& s, const char* p) {
    size_t n = strlen(p);
    if (s.size() < n) return false;
    std::string tail = lowerAscii(s.substr(s.size() - n));
    return tail == lowerAscii(std::string(p));
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    out.reserve(s.size());
    size_t pos = 0;
    for (;;) {
        size_t p = s.find(from, pos);
        if (p == std::string::npos) { out.append(s, pos, std::string::npos); break; }
        out.append(s, pos, p - pos);
        out += to;
        pos = p + from.size();
    }
    return out;
}

std::string intToStr(long long n) {
    char buf[32];
    sprintf(buf, "%lld", n);
    return std::string(buf);
}

std::string padNum(int n, int width) {
    std::string s = intToStr(n);
    while ((int)s.size() < width) s = "0" + s;
    return s;
}

/* =============================== UTF-8 ================================== */

size_t utf8Len(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

std::string utf8Substr(const std::string& s, size_t start, size_t count) {
    size_t i = 0, chars = 0, begin = 0, end = s.size();
    bool started = false;
    for (; i < s.size(); i++) {
        if (((unsigned char)s[i] & 0xC0) == 0x80) continue;
        if (chars == start) { begin = i; started = true; break; }
        chars++;
    }
    if (!started) return std::string();
    if (count == (size_t)-1) return s.substr(begin);
    chars = 0;
    for (i = begin; i < s.size(); i++) {
        if (((unsigned char)s[i] & 0xC0) == 0x80) continue;
        if (chars == count) { end = i; break; }
        chars++;
    }
    return s.substr(begin, end - begin);
}

unsigned utf8CharAt(const std::string& s, size_t index) {
    size_t n = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp; int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { cp = c; extra = 0; }
        size_t j = i + 1;
        for (int k = 0; k < extra && j < s.size(); k++, j++)
            cp = (cp << 6) | ((unsigned char)s[j] & 0x3F);
        if (n == index) return cp;
        n++;
        i = j;
    }
    return 0;
}

bool isValidUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        int extra;
        unsigned cp;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) { extra = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { extra = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { extra = 3; cp = c & 0x07; }
        else return false;
        if (i + (size_t)extra >= s.size()) return false;
        for (int k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += extra + 1;
    }
    return true;
}

/* ============================= кодировки ================================ */

std::wstring utf8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring w;
    w.resize((size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string wToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string s;
    s.resize((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

std::string cpToUtf8(const std::string& s, UINT codepage) {
    if (s.empty()) return std::string();
    int n = MultiByteToWideChar(codepage, 0, s.data(), (int)s.size(), NULL, 0);
    if (n <= 0) return s;
    std::wstring w;
    w.resize((size_t)n);
    MultiByteToWideChar(codepage, 0, s.data(), (int)s.size(), &w[0], n);
    return wToUtf8(w);
}

std::string utf8ToCp(const std::string& s, UINT codepage) {
    std::wstring w = utf8ToW(s);
    if (w.empty()) return std::string();
    BOOL used = FALSE;
    int n = WideCharToMultiByte(codepage, 0, w.data(), (int)w.size(), NULL, 0, "?", &used);
    if (n <= 0) return s;
    std::string r;
    r.resize((size_t)n);
    WideCharToMultiByte(codepage, 0, w.data(), (int)w.size(), &r[0], n, "?", &used);
    return r;
}

std::string toLowerRu(const std::string& s) {
    std::wstring w = utf8ToW(s);
    for (size_t i = 0; i < w.size(); i++) {
        wchar_t c = w[i];
        if (c >= L'A' && c <= L'Z') w[i] = (wchar_t)(c - L'A' + L'a');
        else if (c >= 0x410 && c <= 0x42F) w[i] = (wchar_t)(c + 32);
        else if (c == 0x401) w[i] = 0x451;
    }
    return wToUtf8(w);
}

static std::string findEncodingLabel(const std::string& head) {
    /* ищем encoding="..." или charset=... */
    const char* keys[3] = { "encoding", "charset", nullptr };
    std::string low = lowerAscii(head);
    for (int k = 0; keys[k]; k++) {
        std::string key = keys[k];
        size_t p = 0;
        while ((p = low.find(key, p)) != std::string::npos) {
            size_t q = p + key.size();
            while (q < low.size() && isSpaceByte((unsigned char)low[q])) q++;
            if (q < low.size() && low[q] == '=') {
                q++;
                while (q < low.size() && isSpaceByte((unsigned char)low[q])) q++;
                char quote = 0;
                if (q < low.size() && (low[q] == '"' || low[q] == '\'')) { quote = low[q]; q++; }
                std::string val;
                while (q < low.size()) {
                    char c = low[q];
                    if (quote) { if (c == quote) break; }
                    else if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) break;
                    val += c;
                    q++;
                }
                if (!val.empty()) return val;
            }
            p += key.size();
        }
    }
    return std::string();
}

std::string decodeText(const Bytes& data, std::string* encOut) {
    std::string enc = "utf-8";
    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        enc = "utf-8";
        std::string s((const char*)&data[3], data.size() - 3);
        if (encOut) *encOut = enc;
        return s;
    }
    if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        std::wstring w;
        size_t n = (data.size() - 2) / 2;
        w.resize(n);
        memcpy(&w[0], &data[2], n * 2);
        if (encOut) *encOut = "utf-16le";
        return wToUtf8(w);
    }
    if (data.size() >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        std::wstring w;
        size_t n = (data.size() - 2) / 2;
        w.resize(n);
        for (size_t i = 0; i < n; i++)
            w[i] = (wchar_t)((data[2 + i * 2] << 8) | data[2 + i * 2 + 1]);
        if (encOut) *encOut = "utf-16be";
        return wToUtf8(w);
    }

    std::string raw((const char*)data.data(), data.size());
    if (isValidUtf8(raw)) {
        if (encOut) *encOut = "utf-8";
        return raw;
    }

    std::string head = cpToUtf8(raw.substr(0, std::min<size_t>(raw.size(), 900)), 1251);
    std::string label = findEncodingLabel(head);
    UINT cp = 0;
    if (!label.empty()) {
        std::string l = lowerAscii(label);
        l = replaceAll(l, "_", "-");
        if (l == "utf-8" || l == "utf8") cp = CP_UTF8;
        else if (l == "windows-1251" || l == "cp1251" || l == "win-1251" || l == "1251") cp = 1251;
        else if (l == "windows-1252" || l == "cp1252" || l == "iso-8859-1" || l == "latin1" || l == "iso8859-1") cp = 1252;
        else if (l == "koi8-r" || l == "koi8r") cp = 20866;
        else if (l == "koi8-u") cp = 21866;
        else if (l == "ibm866" || l == "cp866" || l == "866" || l == "dos-866") cp = 866;
        else if (l == "us-ascii" || l == "ascii") cp = 20127;
    }
    if (cp == CP_UTF8) {
        if (encOut) *encOut = "utf-8";
        return raw;
    }
    if (cp == 0) {
        std::string low = lowerAscii(head);
        if (low.find("koi8") != std::string::npos) cp = 20866;
        else if (low.find("1252") != std::string::npos) cp = 1252;
        else if (low.find("866") != std::string::npos) cp = 866;
        else cp = 1251;
    }
    if (encOut) {
        switch (cp) {
            case 1251: *encOut = "windows-1251"; break;
            case 1252: *encOut = "windows-1252"; break;
            case 20866: *encOut = "koi8-r"; break;
            case 866: *encOut = "ibm866"; break;
            default: *encOut = "windows-1251"; break;
        }
    }
    return cpToUtf8(raw, cp);
}

/* ============================ экранирование ============================= */

std::string escXml(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); i++) {
        switch (s[i]) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += s[i];
        }
    }
    return out;
}

std::string escHtml(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); i++) {
        switch (s[i]) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += s[i];
        }
    }
    return out;
}

static void putUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

struct NamedEntity { const char* name; unsigned cp; };
static const NamedEntity kEntities[] = {
    {"nbsp",0x00A0},{"mdash",0x2014},{"ndash",0x2013},{"laquo",0x00AB},{"raquo",0x00BB},
    {"hellip",0x2026},{"bull",0x2022},{"copy",0x00A9},{"reg",0x00AE},{"trade",0x2122},
    {"deg",0x00B0},{"plusmn",0x00B1},{"times",0x00D7},{"divide",0x00F7},{"frac12",0x00BD},
    {"frac14",0x00BC},{"frac34",0x00BE},{"sect",0x00A7},{"para",0x00B6},{"dagger",0x2020},
    {"Dagger",0x2021},{"permil",0x2030},{"lsquo",0x2018},{"rsquo",0x2019},{"ldquo",0x201C},
    {"rdquo",0x201D},{"sbquo",0x201A},{"bdquo",0x201E},{"lsaquo",0x2039},{"rsaquo",0x203A},
    {"prime",0x2032},{"Prime",0x2033},{"minus",0x2212},{"shy",0x00AD},{"middot",0x00B7},
    {"euro",0x20AC},{"pound",0x00A3},{"yen",0x00A5},{"cent",0x00A2},{"alpha",0x03B1},
    {"beta",0x03B2},{"gamma",0x03B3},{"delta",0x03B4},{"pi",0x03C0},{"mu",0x03BC},
    {"sigma",0x03C3},{"omega",0x03C9},{"infin",0x221E},{"ne",0x2260},{"le",0x2264},
    {"ge",0x2265},{"asymp",0x2248},{"rarr",0x2192},{"larr",0x2190},{"uarr",0x2191},
    {"darr",0x2193},{"harr",0x2194},{"loz",0x25CA},{"spades",0x2660},{"clubs",0x2663},
    {"hearts",0x2665},{"diams",0x2666},{"apos",0x0027},{"AMP",0x0026},{"LT",0x003C},
    {"GT",0x003E},{"quot",0x0022},{"amp",0x0026},{"lt",0x003C},{"gt",0x003E},
    {"emsp",0x2003},{"ensp",0x2002},{"thinsp",0x2009},{"zwnj",0x200C},{"zwj",0x200D},
    {"lrm",0x200E},{"rlm",0x200F},{"oelig",0x0153},{"OElig",0x0152},{"scaron",0x0161},
    {"Scaron",0x0160},{"Yuml",0x0178},{"fnof",0x0192},{"circ",0x02C6},{"tilde",0x02DC},
    {"sup1",0x00B9},{"sup2",0x00B2},{"sup3",0x00B3},{"micro",0x00B5},{"acute",0x00B4},
    {"cedil",0x00B8},{"uml",0x00A8},{"ordf",0x00AA},{"not",0x00AC},{"macr",0x00AF},
    {"brvbar",0x00A6},{"curren",0x00A4},{"iquest",0x00BF},{"iexcl",0x00A1},{"szlig",0x00DF},
    {"agrave",0x00E0},{"aacute",0x00E1},{"acirc",0x00E2},{"atilde",0x00E3},{"auml",0x00E4},
    {"aring",0x00E5},{"aelig",0x00E6},{"ccedil",0x00E7},{"egrave",0x00E8},{"eacute",0x00E9},
    {"ecirc",0x00EA},{"euml",0x00EB},{"igrave",0x00EC},{"iacute",0x00ED},{"icirc",0x00EE},
    {"iuml",0x00EF},{"ntilde",0x00F1},{"ograve",0x00F2},{"oacute",0x00F3},{"ocirc",0x00F4},
    {"otilde",0x00F5},{"ouml",0x00F6},{"oslash",0x00F8},{"ugrave",0x00F9},{"uacute",0x00FA},
    {"ucirc",0x00FB},{"uuml",0x00FC},{"yacute",0x00FD},{"yuml",0x00FF},{"Agrave",0x00C0},
    {"Aacute",0x00C1},{"Acirc",0x00C2},{"Atilde",0x00C3},{"Auml",0x00C4},{"Aring",0x00C5},
    {"AElig",0x00C6},{"Ccedil",0x00C7},{"Egrave",0x00C8},{"Eacute",0x00C9},{"Ecirc",0x00CA},
    {"Euml",0x00CB},{"Igrave",0x00CC},{"Iacute",0x00CD},{"Icirc",0x00CE},{"Iuml",0x00CF},
    {"Ntilde",0x00D1},{"Ograve",0x00D2},{"Oacute",0x00D3},{"Ocirc",0x00D4},{"Otilde",0x00D5},
    {"Ouml",0x00D6},{"Oslash",0x00D8},{"Ugrave",0x00D9},{"Uacute",0x00DA},{"Ucirc",0x00DB},
    {"Uuml",0x00DC},{"Yacute",0x00DD},
    {NULL,0}
};

std::string unescapeEntities(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] != '&') { out += in[i++]; continue; }
        size_t semi = in.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 32) { out += in[i++]; continue; }
        std::string name = in.substr(i + 1, semi - i - 1);
        if (!name.empty() && name[0] == '#') {
            unsigned cp = 0;
            bool ok = false;
            if (name.size() > 1 && (name[1] == 'x' || name[1] == 'X')) {
                cp = (unsigned)strtoul(name.c_str() + 2, NULL, 16);
                ok = name.size() > 2;
            } else if (name.size() > 1) {
                cp = (unsigned)strtoul(name.c_str() + 1, NULL, 10);
                ok = true;
            }
            if (ok && cp) { putUtf8(out, cp); i = semi + 1; continue; }
            out += in[i++];
            continue;
        }
        bool found = false;
        for (int k = 0; kEntities[k].name; k++) {
            if (name == kEntities[k].name) {
                putUtf8(out, kEntities[k].cp);
                found = true;
                break;
            }
        }
        if (found) { i = semi + 1; continue; }
        out += in[i++];
    }
    return out;
}

std::string collapseSpacesPreserveBr(const std::string& s) {
    std::string out;
    bool sp = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '\n') { out += '\n'; sp = true; }
        else if (c == ' ' || c == '\t' || c == '\r') { if (!sp) { out += ' '; sp = true; } }
        else { out += c; sp = false; }
    }
    return out;
}

/* HTML-фрагмент -> простой текст */
std::string htmlToText(const std::string& html) {
    if (html.empty()) return std::string();
    std::string s;
    s.reserve(html.size() + 16);
    size_t i = 0;
    while (i < html.size()) {
        char c = html[i];
        if (c != '<') { s += c; i++; continue; }
        size_t gt = html.find('>', i);
        if (gt == std::string::npos) { s += html.substr(i); break; }
        std::string tag = lowerAscii(html.substr(i + 1, gt - i - 1));
        /* имя тега */
        size_t p = 0;
        while (p < tag.size() && (isalnum((unsigned char)tag[p]) || tag[p] == '/')) p++;
        std::string nm = tag.substr(0, p);
        bool closing = !nm.empty() && nm[0] == '/';
        std::string bare = closing ? nm.substr(1) : nm;

        if (bare == "br") s += '\n';
        else if (bare == "p" && closing) s += "\n\n";
        else if ((bare == "td" || bare == "th") && closing) s += " | ";
        else if (bare == "tr" && closing) s += '\n';
        else if (bare == "div" && closing) s += '\n';
        else if (bare == "li") s += "\n* ";
        else if (bare.size() == 2 && bare[0] == 'h' && bare[1] >= '1' && bare[1] <= '6') s += "\n\n";
        else if (bare == "img") {
            /* alt="..." */
            size_t a = tag.find("alt=\"");
            if (a != std::string::npos) {
                size_t e = tag.find('"', a + 5);
                if (e != std::string::npos) s += tag.substr(a + 5, e - a - 5);
            }
        }
        i = gt + 1;
    }
    std::string t = unescapeEntities(s);
    t = replaceAll(t, "\xC2\xA0", " ");

    /* нормализуем пустые строки и хвосты */
    std::string out;
    out.reserve(t.size());
    int nl = 0;
    for (size_t k = 0; k < t.size(); k++) {
        char c = t[k];
        if (c == '\r') continue;
        if (c == '\n') {
            nl++;
            if (nl <= 2) out += '\n';
        } else {
            if (c == ' ' || c == '\t') {
                if (nl == 0 && !out.empty() && out[out.size() - 1] == ' ') continue;
                if (nl > 0) { nl = 0; continue; }
            } else {
                nl = 0;
            }
            out += c;
        }
    }
    /* хвосты « | » в конце строк */
    out = replaceAll(out, " | \n", "\n");
    out = replaceAll(out, " | ", " ");
    return trim(out);
}

/* ============================== base64 ================================== */

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64encode(const Bytes& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
        i += 3;
    }
    size_t rest = data.size() - i;
    if (rest == 1) {
        unsigned v = data[i] << 16;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += "==";
    } else if (rest == 2) {
        unsigned v = (data[i] << 16) | (data[i + 1] << 8);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

Bytes b64decode(const std::string& s) {
    Bytes out;
    out.reserve(s.size() * 3 / 4 + 3);
    unsigned buf = 0;
    int bits = 0;
    for (size_t i = 0; i < s.size(); i++) {
        int v = b64val(s[i]);
        if (v < 0) continue;
        buf = (buf << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char)((buf >> bits) & 0xFF));
        }
    }
    return out;
}

/* =============================== файлы ================================== */

std::wstring utf8PathToW(const std::string& p) { return utf8ToW(p); }
std::string wPathToUtf8(const std::wstring& p) { return wToUtf8(p); }

bool readFileBytes(const std::wstring& path, Bytes& out, std::string* err) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = "Не удалось открыть файл (код " + intToStr(GetLastError()) + ")";
        return false;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        if (err) *err = "Не удалось определить размер файла";
        return false;
    }
    if (sz.QuadPart > 512LL * 1024 * 1024) {
        CloseHandle(h);
        if (err) *err = "Файл слишком большой (больше 512 МБ)";
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    size_t done = 0;
    while (done < out.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(out.size() - done, 1 << 20);
        DWORD got = 0;
        if (!ReadFile(h, &out[done], chunk, &got, NULL) || got == 0) {
            CloseHandle(h);
            if (err) *err = "Ошибка чтения файла";
            return false;
        }
        done += got;
    }
    CloseHandle(h);
    return true;
}

bool writeFileBytes(const std::wstring& path, const Bytes& data, std::string* err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) {
            DWORD e = GetLastError();
            if (e == ERROR_ACCESS_DENIED) *err = "Нет прав на запись в папку";
            else if (e == ERROR_PATH_NOT_FOUND) *err = "Папка не существует";
            else *err = "Не удалось создать файл (код " + intToStr(e) + ")";
        }
        return false;
    }
    size_t done = 0;
    while (done < data.size()) {
        DWORD chunk = (DWORD)std::min<size_t>(data.size() - done, 1 << 20);
        DWORD put = 0;
        if (!WriteFile(h, &data[done], chunk, &put, NULL) || put == 0) {
            CloseHandle(h);
            if (err) *err = "Ошибка записи файла";
            return false;
        }
        done += put;
    }
    CloseHandle(h);
    return true;
}

bool fileExistsW(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExistsW(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ================================ пути ================================== */

static std::string normalizeSlashes(const std::string& p) {
    std::string s = p;
    for (size_t i = 0; i < s.size(); i++) if (s[i] == '\\') s[i] = '/';
    return s;
}

std::string pathBase(const std::string& path) {
    std::string s = normalizeSlashes(path);
    size_t p = s.rfind('/');
    return p == std::string::npos ? s : s.substr(p + 1);
}

std::string pathDirOf(const std::string& path) {
    std::string s = path;
    size_t p = s.find_last_of("\\/");
    if (p == std::string::npos) return std::string();
    return s.substr(0, p);
}

std::string pathStripExt(const std::string& name) {
    size_t dot = name.rfind('.');
    size_t slash = name.find_last_of("\\/");
    if (dot == std::string::npos) return name;
    if (slash != std::string::npos && dot < slash) return name;
    return name.substr(0, dot);
}

std::string pathExt(const std::string& name) {
    size_t dot = name.rfind('.');
    size_t slash = name.find_last_of("\\/");
    if (dot == std::string::npos) return std::string();
    if (slash != std::string::npos && dot < slash) return std::string();
    std::string e = name.substr(dot + 1);
    if (e.empty() || e.size() > 8) return std::string();
    return lowerAscii(e);
}

std::string pathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a[a.size() - 1];
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

std::string safeFileName(const std::string& in, const char* fallback) {
    std::string t = collapse(in);
    std::string out;
    for (size_t i = 0; i < t.size(); i++) {
        char c = t[i];
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || (unsigned char)c < 0x20)
            out += ' ';
        else
            out += c;
    }
    out = collapse(out);
    while (!out.empty() && (out[0] == '.' || out[0] == ' ')) out.erase(0, 1);
    while (!out.empty() && (out[out.size() - 1] == '.' || out[out.size() - 1] == ' '))
        out.erase(out.size() - 1);
    if (out.empty()) out = fallback ? fallback : "book";
    if (utf8Len(out) > 90) out = utf8Substr(out, 0, 90);
    out = trim(out);
    if (out.empty()) out = fallback ? fallback : "book";
    return out;
}

/* ================================ прочее ================================ */

std::string fmtSize(unsigned long long n) {
    char buf[64];
    if (n < 1024) sprintf(buf, "%llu Б", n);
    else if (n < 1024ULL * 1024) sprintf(buf, "%.1f КБ", n / 1024.0);
    else sprintf(buf, "%.1f МБ", n / 1048576.0);
    std::string s(buf);
    s = replaceAll(s, ".", ",");
    return s;
}

std::string uuidStr() {
    GUID g;
    if (CoCreateGuid(&g) == S_OK) {
        char buf[64];
        sprintf(buf, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                (unsigned long)g.Data1, g.Data2, g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        std::string s(buf);
        for (size_t i = 0; i < s.size(); i++) s[i] = (char)tolower((int)s[i]);
        return s;
    }
    return "00000000-0000-4000-8000-000000000000";
}

std::string isoDateNow() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    sprintf(buf, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return buf;
}

std::string isoDateTimeNow() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[40];
    sprintf(buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void sleepMs(int ms) { Sleep((DWORD)ms); }

std::string fullName(const Author& a) {
    std::vector<std::string> parts;
    if (!a.first.empty()) parts.push_back(a.first);
    if (!a.middle.empty()) parts.push_back(a.middle);
    if (!a.last.empty()) parts.push_back(a.last);
    if (!parts.empty()) {
        std::string s;
        for (size_t i = 0; i < parts.size(); i++) {
            if (i) s += ' ';
            s += parts[i];
        }
        return collapse(s);
    }
    return collapse(a.nick);
}

Author splitAuthor(const std::string& name, bool familyFirst) {
    Author a;
    std::string n = collapse(name);
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < n.size()) {
        size_t sp = n.find(' ', pos);
        if (sp == std::string::npos) { parts.push_back(n.substr(pos)); break; }
        parts.push_back(n.substr(pos, sp - pos));
        pos = sp + 1;
        while (pos < n.size() && n[pos] == ' ') pos++;
    }
    if (parts.empty()) return a;
    if (parts.size() == 1) { a.last = parts[0]; return a; }
    if (parts.size() == 2) {
        if (familyFirst) { a.last = parts[0]; a.first = parts[1]; }
        else { a.first = parts[0]; a.last = parts[1]; }
        return a;
    }
    if (familyFirst) {
        a.last = parts[0];
        std::string rest;
        for (size_t i = 1; i < parts.size(); i++) { if (i > 1) rest += ' '; rest += parts[i]; }
        a.first = rest;
    } else {
        a.first = parts[0];
        a.middle = parts[1];
        std::string rest;
        for (size_t i = 2; i < parts.size(); i++) { if (i > 2) rest += ' '; rest += parts[i]; }
        a.last = rest;
    }
    return a;
}
