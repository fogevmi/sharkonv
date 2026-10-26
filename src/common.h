/* Sharkonv — общие объявления.
   Нативный Win32, совместимость с Windows XP SP3 .. Windows 11. */
#ifndef FB2C_COMMON_H
#define FB2C_COMMON_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

typedef std::vector<unsigned char> Bytes;

/* ------------------------------- строки --------------------------------- */

std::string trim(const std::string& s);
std::string collapse(const std::string& s);
bool startsWith(const std::string& s, const char* p);
bool endsWithNoCase(const std::string& s, const char* p);
std::string lowerAscii(const std::string& s);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
std::string intToStr(long long n);
std::string padNum(int n, int width);

/* UTF-8 */
size_t utf8Len(const std::string& s);
std::string utf8Substr(const std::string& s, size_t start, size_t count);
unsigned utf8CharAt(const std::string& s, size_t index);
bool isValidUtf8(const std::string& s);

/* ------------------------------ кодировки -------------------------------- */

std::wstring utf8ToW(const std::string& s);
std::string wToUtf8(const std::wstring& s);
std::string cpToUtf8(const std::string& s, UINT codepage);
std::string utf8ToCp(const std::string& s, UINT codepage);
std::string toLowerRu(const std::string& s);

/* Определение кодировки и декодирование в UTF-8. enc — наружу. */
std::string decodeText(const Bytes& data, std::string* enc);

/* ------------------------------ экранирование ---------------------------- */

std::string escXml(const std::string& s);
std::string escHtml(const std::string& s);
std::string unescapeEntities(const std::string& s);
std::string htmlToText(const std::string& html);
std::string collapseSpacesPreserveBr(const std::string& s);

/* -------------------------------- base64 --------------------------------- */

std::string b64encode(const Bytes& data);
Bytes b64decode(const std::string& s);

/* -------------------------------- файлы ---------------------------------- */

bool readFileBytes(const std::wstring& path, Bytes& out, std::string* err);
bool writeFileBytes(const std::wstring& path, const Bytes& data, std::string* err);
bool fileExistsW(const std::wstring& path);
bool dirExistsW(const std::wstring& path);
std::wstring utf8PathToW(const std::string& p);
std::string wPathToUtf8(const std::wstring& p);

/* -------------------------------- пути ----------------------------------- */

std::string pathBase(const std::string& path);         /* «dir/имя.ext» -> «имя.ext» */
std::string pathDirOf(const std::string& path);
std::string pathStripExt(const std::string& name);
std::string pathExt(const std::string& name);          /* без точки, в нижнем регистре */
std::string pathJoin(const std::string& a, const std::string& b);
std::string safeFileName(const std::string& s, const char* fallback);

/* -------------------------------- прочее --------------------------------- */

std::string fmtSize(unsigned long long n);
std::string uuidStr();
std::string isoDateNow();
std::string isoDateTimeNow();
void sleepMs(int ms);

struct Author { std::string first, middle, last, nick; };
std::string fullName(const Author& a);
Author splitAuthor(const std::string& name, bool familyFirst);

#endif
