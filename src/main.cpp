/* Точка входа. Один exe: без параметров-ключей — окно, с ключами (-f, -o, -h …) — консольный режим. */
#include "convert.h"
#include <shellapi.h>

int runGui(HINSTANCE hInst, const std::vector<std::wstring>& files);

#define APP_VERSION L"1.7"

/* --------------------------- вывод в консоль ---------------------------- */

static HANDLE g_out = INVALID_HANDLE_VALUE;
static bool g_ownConsole = false; /* консоль создана нами — в конце ждём Enter */

static void conWrite(const std::wstring& w) {
    if (g_out == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(g_out, &mode)) {
        DWORD wrote = 0;
        WriteConsoleW(g_out, w.c_str(), (DWORD)w.size(), &wrote, NULL);
    } else {
        std::string u = wToUtf8(w);
        DWORD wrote = 0;
        WriteFile(g_out, u.data(), (DWORD)u.size(), &wrote, NULL);
    }
}

static void conLine(const std::wstring& w) { conWrite(w + L"\r\n"); }

static bool validHandle(HANDLE h) { return h != NULL && h != INVALID_HANDLE_VALUE; }

/* Куда писать в консольном режиме (exe собран как оконный):
   1) вывод перенаправлен (> log.txt, конвейер) — в него;
   2) запуск из cmd/PowerShell — в консоль родителя;
   3) ярлык или «Выполнить» с ключами — своё окно консоли, в конце ждём Enter. */
static bool openConsoleOutput() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (validHandle(h) && GetFileType(h) != FILE_TYPE_UNKNOWN) {
        g_out = h;
        return true;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (!AllocConsole()) return false;
        SetConsoleTitleW(L"Sharkonv");
        g_ownConsole = true;
    }
    g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    if (!validHandle(g_out)) return false;
    if (!g_ownConsole) conWrite(L"\r\n"); /* cmd уже напечатал приглашение — начинаем с новой строки */
    return true;
}

static void closeConsoleOutput() {
    if (!g_ownConsole) return;
    conWrite(L"\r\nНажмите Enter, чтобы закрыть окно.");
    HANDLE in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
    if (!validHandle(in)) return;
    wchar_t buf[16];
    DWORD got = 0;
    ReadConsoleW(in, buf, 16, &got, NULL);
    CloseHandle(in);
}

/* ------------------------------ утилиты --------------------------------- */

static void collectDir(const std::wstring& dir, std::vector<std::wstring>& out, int depth) {
    if (depth > 8) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            collectDir(full, out, depth + 1);
        } else if (isSupportedInput(wPathToUtf8(name))) {
            out.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static std::wstring uniquePath(const std::wstring& dir, const std::wstring& base,
                               const std::wstring& ext) {
    std::wstring path = dir + L"\\" + base + L"." + ext;
    if (!fileExistsW(path)) return path;
    for (int i = 2; i < 1000; i++) {
        std::wstring p = dir + L"\\" + base + L" (" + utf8ToW(intToStr(i)) + L")." + ext;
        if (!fileExistsW(p)) return p;
    }
    return dir + L"\\" + base + L" (" + utf8ToW(intToStr((long long)GetTickCount())) + L")." + ext;
}

/* ------------------------------ конверсия -------------------------------- */

bool convertOne(const std::wstring& inPath, const std::string& target, const std::wstring& outDir,
                bool overwrite, std::wstring* outPath, std::wstring* errText) {
    Bytes data;
    std::string err;
    if (!readFileBytes(inPath, data, &err)) {
        if (errText) *errText = utf8ToW(err);
        return false;
    }
    std::string name = wPathToUtf8(inPath);
    Book book;
    if (!readBook(name, data, book, &err)) {
        if (errText) *errText = utf8ToW(err);
        return false;
    }
    Bytes out;
    if (!writeBook(book, target, out, &err)) {
        if (errText) *errText = utf8ToW(err);
        return false;
    }

    std::wstring dir = outDir;
    if (dir.empty()) {
        size_t sl = inPath.find_last_of(L"\\/");
        dir = sl == std::wstring::npos ? std::wstring(L".") : inPath.substr(0, sl);
    }
    std::wstring base = utf8ToW(pathStripExt(pathBase(name)));
    std::wstring ext = utf8ToW(target);
    std::wstring path = overwrite ? (dir + L"\\" + base + L"." + ext) : uniquePath(dir, base, ext);
    err.clear();
    if (!writeFileBytes(path, out, &err)) {
        if (errText) *errText = utf8ToW(err);
        return false;
    }
    if (outPath) *outPath = path;
    return true;
}

/* -------------------------------- CLI ----------------------------------- */

static void printHelp() {
    conLine(L"Sharkonv " APP_VERSION L" — конвертация книг без браузера и установки.");
    conLine(L"");
    conLine(L"Без ключей открывается окно. С ключами — консольный режим:");
    conLine(L"  sharkonv.exe -f epub книга.fb2         -> EPUB рядом с исходным файлом");
    conLine(L"  sharkonv.exe -f fb2 D:\\Книги           вся папка (вложенные тоже) -> FB2");
    conLine(L"  sharkonv.exe -f auto -o D:\\Out D:\\In   авто: FB2 -> EPUB, остальное -> FB2");
    conLine(L"");
    conLine(L"Ключи:");
    conLine(L"  -f, --format  auto | fb2 | epub | html | txt | mobi");
    conLine(L"  -o, --out     папка для результатов (по умолчанию — рядом с исходником)");
    conLine(L"  -h, --help    эта справка      -v, --version  версия");
    conLine(L"");
    conLine(L"Вход: FB2 (.fb2, .fb2.zip), EPUB, MOBI/PRC/AZW, PDF, DjVu, DOC, DOCX, ODT, RTF, TXT, HTML.");
    conLine(L"В консольном режиме существующий файл результата заменяется.");
    conLine(L"Код возврата: 0 — без ошибок, 1 — были ошибки, 2 — неверные параметры.");
    conLine(L"В .bat-файле программа дожидается конца сама; в окне cmd используйте");
    conLine(L"  start /wait sharkonv.exe -f epub книга.fb2");
}

static int runCliBody(const std::vector<std::wstring>& args) {

    std::wstring fmt, outDir;
    std::vector<std::wstring> files;

    for (size_t i = 0; i < args.size(); i++) {
        const std::wstring& a = args[i];
        if (_wcsicmp(a.c_str(), L"-f") == 0 || _wcsicmp(a.c_str(), L"--format") == 0) {
            if (i + 1 < args.size()) fmt = args[++i];
            continue;
        }
        if (_wcsicmp(a.c_str(), L"-o") == 0 || _wcsicmp(a.c_str(), L"--out") == 0) {
            if (i + 1 < args.size()) outDir = args[++i];
            continue;
        }
        if (_wcsicmp(a.c_str(), L"-h") == 0 || _wcsicmp(a.c_str(), L"--help") == 0 ||
            _wcsicmp(a.c_str(), L"/?") == 0) {
            printHelp();
            return 0;
        }
        if (_wcsicmp(a.c_str(), L"-v") == 0 || _wcsicmp(a.c_str(), L"--version") == 0) {
            conLine(L"Sharkonv " APP_VERSION);
            return 0;
        }
        if (!a.empty() && (a[0] == L'-' || a[0] == L'/')) {
            conLine(L"Неизвестный параметр: " + a);
            continue;
        }
        std::wstring p = a;
        if (dirExistsW(p)) collectDir(p, files, 0);
        else if (fileExistsW(p)) files.push_back(p);
        else conLine(L"Пропущено (не найдено): " + a);
    }

    if (_wcsicmp(fmt.c_str(), L"auto") == 0) fmt.clear();
    if (!fmt.empty() && !isSupportedTarget(wPathToUtf8(fmt))) {
        conLine(L"Неизвестный формат: " + fmt + L". Допустимо: auto, fb2, epub, html, txt, mobi.");
        return 2;
    }

    if (!outDir.empty() && !dirExistsW(outDir)) {
        if (!CreateDirectoryW(outDir.c_str(), NULL)) {
            conLine(L"Не удалось создать папку: " + outDir);
            return 2;
        }
    }

    if (files.empty()) {
        printHelp();
        return 2;
    }

    int ok = 0, fail = 0;
    for (size_t i = 0; i < files.size(); i++) {
        std::string inName = wPathToUtf8(files[i]);
        std::string target = resolveTarget(inName, wPathToUtf8(fmt));
        std::wstring outPath, err;
        if (convertOne(files[i], target, outDir, true, &outPath, &err)) {
            ok++;
            conLine(L"OK   " + files[i] + L"  ->  " + outPath);
        } else {
            fail++;
            conLine(L"ОШИБКА  " + files[i] + L": " + err);
        }
    }
    conLine(L"");
    conLine(L"Готово: " + utf8ToW(intToStr(ok)) + L", ошибок: " + utf8ToW(intToStr(fail)));
    return fail == 0 ? 0 : 1;
}

static int runCli(const std::vector<std::wstring>& args) {
    if (!openConsoleOutput()) return 1;
    int rc = runCliBody(args);
    closeConsoleOutput();
    return rc;
}

/* ------------------------------- запуск --------------------------------- */

static std::vector<std::wstring> commandLineArgs() {
    std::vector<std::wstring> args;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return args;
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);
    LocalFree(argv);
    return args;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    std::vector<std::wstring> args = commandLineArgs();
    bool consoleMode = false;
    for (size_t i = 0; i < args.size(); i++) {
        if (!args[i].empty() && (args[i][0] == L'-' || args[i][0] == L'/')) {
            consoleMode = true;
            break;
        }
    }
    if (consoleMode) return runCli(args);

    std::vector<std::wstring> files;
    for (size_t i = 0; i < args.size(); i++)
        if (fileExistsW(args[i]) || dirExistsW(args[i])) files.push_back(args[i]);

    return runGui(hInst, files);
}
