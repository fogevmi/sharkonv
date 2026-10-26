/* Оконный интерфейс: два шага («какие книги» -> «во что») и одна большая кнопка.
   Только Win32 API и comctl32, совместим с Windows XP. */
#include "convert.h"
#include "notrace.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#define IDC_LIST      1001
#define IDC_ADD       1002
#define IDC_ADDDIR    1003
#define IDC_REMOVE    1004
#define IDC_CLEAR     1005
#define IDC_CONVERT   1006
#define IDC_PROGRESS  1007
#define IDC_STATUS    1008
#define IDC_HEAD1     1009
#define IDC_HEAD2     1010
#define IDC_HINT      1011
#define IDC_FMTDESC   1012
#define IDC_OPEN      1013
#define IDC_DROP      1014
#define IDC_FMT0      1100 /* IDC_FMT0 + 0..5 — плашки форматов */

#define WM_APP_ITEM   (WM_APP + 1)
#define WM_APP_DONE   (WM_APP + 2)

bool convertOne(const std::wstring& inPath, const std::string& target, const std::wstring& outDir,
                bool overwrite, std::wstring* outPath, std::wstring* errText);

#ifndef LVS_EX_DOUBLEBUFFER
#define LVS_EX_DOUBLEBUFFER 0x00010000
#endif
#ifndef OFN_DONTADDTORECENT
#define OFN_DONTADDTORECENT 0x02000000
#endif
#ifndef BIF_NEWDIALOGSTYLE
#define BIF_NEWDIALOGSTYLE 0x0040
#endif
#ifndef ODS_NOFOCUSRECT
#define ODS_NOFOCUSRECT 0x0200
#endif

namespace {

/* ------------------------------- палитра -------------------------------- */

const COLORREF kAccent      = RGB(0, 103, 192);
const COLORREF kAccentDark  = RGB(0, 80, 155);
const COLORREF kText        = RGB(32, 32, 32);
const COLORREF kMuted       = RGB(110, 110, 110);
const COLORREF kBorder      = RGB(190, 196, 206);
const COLORREF kChipPressed = RGB(232, 238, 246);
const COLORREF kDropFill    = RGB(246, 249, 253);
const COLORREF kDropBorder  = RGB(150, 170, 200);
const COLORREF kOk          = RGB(16, 124, 16);
const COLORREF kErr         = RGB(196, 43, 28);
const COLORREF kDisabledBg  = RGB(228, 230, 234);
const COLORREF kDisabledFg  = RGB(140, 140, 140);

/* ----------------------------- состояние -------------------------------- */

enum RowState { ROW_PENDING = 0, ROW_QUEUED, ROW_OK, ROW_ERR };

struct AppState {
    HWND hwnd;
    HWND list, drop;
    HWND head1, head2, hint, fmtDesc, status, progress;
    HWND btnAdd, btnAddDir, btnRemove, btnClear, btnConvert, btnOpen;
    HWND chips[6];
    HFONT font, fontBold, fontBig, fontDrop;
    HICON icon;
    HANDLE thread;
    int lastListW;
    int format;            /* 0 = авто, далее kFormats */
    int doneCount;         /* сколько книг обработано в текущем запуске */
    int runTotal;
    bool statusError;
    bool busy;
    std::vector<std::wstring> files;
    std::vector<std::wstring> outputs;
    std::vector<int> state;
    AppState() : hwnd(NULL), list(NULL), drop(NULL), head1(NULL), head2(NULL), hint(NULL),
                 fmtDesc(NULL), status(NULL), progress(NULL), btnAdd(NULL), btnAddDir(NULL),
                 btnRemove(NULL), btnClear(NULL), btnConvert(NULL), btnOpen(NULL), font(NULL),
                 fontBold(NULL), fontBig(NULL), fontDrop(NULL), icon(NULL), thread(NULL),
                 lastListW(0), format(0), doneCount(0), runTotal(0), statusError(false), busy(false) {
        for (int i = 0; i < 6; i++) chips[i] = NULL;
    }
};

AppState g_app;

const char* kFormats[] = {"auto", "fb2", "epub", "mobi", "html", "txt"};
const wchar_t* kFormatNames[] = {L"Авто", L"FB2", L"EPUB", L"MOBI", L"HTML", L"TXT"};
const wchar_t* kFormatDesc[] = {
    L"FB2-книги станут EPUB, все остальные — FB2. Подходит в большинстве случаев.",
    L"FB2 — для FBReader, AlReader, CoolReader, PocketBook. Лучше всего хранит сноски и оформление.",
    L"EPUB — для большинства читалок, телефонов и планшетов (Apple Books, Google Play Книги).",
    L"MOBI — для старых Kindle.",
    L"HTML — одна веб-страница, откроется в любом браузере.",
    L"TXT — только текст, без картинок и оформления.",
};

/* Системный DPI (манифест dpiAware=true): все размеры заданы для 96 dpi и масштабируются. */
int g_dpi = 96;

int S(int v) { return MulDiv(v, g_dpi, 96); }

void initDpi() {
    HDC dc = GetDC(NULL);
    if (dc) {
        int d = GetDeviceCaps(dc, LOGPIXELSY);
        if (d >= 72 && d <= 480) g_dpi = d;
        ReleaseDC(NULL, dc);
    }
}

/* ------------------------- настройки (рядом с exe) ------------------------ */

/* Портативно: fb2conv.ini лежит рядом с программой, реестр не используется.
   Если папка только для чтения — настройка просто не сохранится. */
std::wstring iniPath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2) return std::wstring();
    std::wstring p(buf, n);
    size_t sl = p.find_last_of(L"\\/");
    return (sl == std::wstring::npos ? std::wstring() : p.substr(0, sl + 1)) + L"fb2conv.ini";
}

std::wstring iniGet(const wchar_t* key) {
    std::wstring ini = iniPath();
    if (ini.empty()) return std::wstring();
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetPrivateProfileStringW(L"main", key, L"", buf, MAX_PATH * 2, ini.c_str());
    return std::wstring(buf, n);
}

int iniGetInt(const wchar_t* key, int def) {
    std::wstring v = iniGet(key);
    if (v.empty()) return def;
    return _wtoi(v.c_str());
}

/* пишем только изменившееся значение — файл не трогается без нужды */
void iniSet(const wchar_t* key, const std::wstring& value) {
    std::wstring ini = iniPath();
    if (ini.empty() || iniGet(key) == value) return;
    WritePrivateProfileStringW(L"main", key, value.c_str(), ini.c_str());
}

void iniSetInt(const wchar_t* key, int v) {
    wchar_t buf[16];
    wsprintfW(buf, L"%d", v);
    iniSet(key, buf);
}

/* формат в ini хранится именем (fb2, epub …), чтобы порядок плашек можно было менять */
int loadFormatIndex() {
    std::wstring v = iniGet(L"format");
    for (int i = 0; i < 6; i++)
        if (_wcsicmp(v.c_str(), utf8ToW(kFormats[i]).c_str()) == 0) return i;
    return 0;
}

void saveFormatIndex(int v) { iniSet(L"format", utf8ToW(kFormats[v])); }

void saveWindowPlacement(HWND hwnd) {
    WINDOWPLACEMENT wp;
    ZeroMemory(&wp, sizeof(wp));
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) return;
    const RECT& r = wp.rcNormalPosition;
    iniSetInt(L"x", r.left);
    iniSetInt(L"y", r.top);
    iniSetInt(L"w", r.right - r.left);
    iniSetInt(L"h", r.bottom - r.top);
    iniSetInt(L"maximized", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
}

/* ------------------------------- утилиты -------------------------------- */

std::wstring fileNameOf(const std::wstring& p) {
    size_t sl = p.find_last_of(L"\\/");
    return sl == std::wstring::npos ? p : p.substr(sl + 1);
}

std::wstring folderOf(const std::wstring& p) {
    size_t sl = p.find_last_of(L"\\/");
    return sl == std::wstring::npos ? std::wstring() : p.substr(0, sl);
}

bool isSupportedName(const std::wstring& name) {
    return isSupportedInput(wPathToUtf8(name));
}

std::wstring upperW(const std::string& s) {
    std::wstring w = utf8ToW(s);
    for (size_t i = 0; i < w.size(); i++)
        if (w[i] >= L'a' && w[i] <= L'z') w[i] = (wchar_t)(w[i] - 32);
    return w;
}

std::wstring inputFormatOf(const std::wstring& path) {
    std::string n = lowerAscii(wPathToUtf8(fileNameOf(path)));
    if (n.size() > 8 && n.compare(n.size() - 8, 8, ".fb2.zip") == 0) return L"FB2.ZIP";
    return upperW(pathExt(n));
}

std::wstring targetOf(const std::wstring& path) {
    return upperW(resolveTarget(wPathToUtf8(path), kFormats[g_app.format]));
}

/* 1 книга, 2 книги, 5 книг */
std::wstring books(int n) {
    int a = n % 100, b = n % 10;
    const wchar_t* w = (a >= 11 && a <= 14) ? L"книг" : (b == 1 ? L"книга" : (b >= 2 && b <= 4 ? L"книги" : L"книг"));
    wchar_t buf[32];
    wsprintfW(buf, L"%d ", n);
    return std::wstring(buf) + w;
}

void scanFolder(const std::wstring& dir, std::vector<std::wstring>& out, int depth) {
    if (depth > 8) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (name.size() && name[0] == L'.') continue;
            scanFolder(full, out, depth + 1);
        } else if (isSupportedName(name)) {
            out.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* винительный падеж для кнопки: «конвертировать 1 книгу» */
std::wstring booksAcc(int n) {
    int a = n % 100, b = n % 10;
    const wchar_t* w = (a >= 11 && a <= 14) ? L"книг" : (b == 1 ? L"книгу" : (b >= 2 && b <= 4 ? L"книги" : L"книг"));
    wchar_t buf[32];
    wsprintfW(buf, L"%d ", n);
    return std::wstring(buf) + w;
}

int errorCount() {
    int n = 0;
    for (size_t i = 0; i < g_app.state.size(); i++)
        if (g_app.state[i] == ROW_ERR) n++;
    return n;
}

int pendingCount() {
    int n = 0;
    for (size_t i = 0; i < g_app.state.size(); i++)
        if (g_app.state[i] != ROW_OK) n++;
    return n;
}

bool hasResults() {
    for (size_t i = 0; i < g_app.outputs.size(); i++)
        if (!g_app.outputs[i].empty()) return true;
    return false;
}

/* ---------------------------- обновление вида --------------------------- */

void layoutControls(HWND hwnd);

void setStatus(const std::wstring& text, bool error) {
    g_app.statusError = error;
    if (g_app.status) {
        SetWindowTextW(g_app.status, text.c_str());
        InvalidateRect(g_app.status, NULL, TRUE);
    }
}

void setCell(int i, int sub, const std::wstring& text) {
    LVITEMW it;
    ZeroMemory(&it, sizeof(it));
    it.iSubItem = sub;
    it.mask = LVIF_TEXT;
    it.iItem = i;
    it.pszText = (LPWSTR)text.c_str();
    SendMessageW(g_app.list, LVM_SETITEMW, 0, (LPARAM)&it);
}

/* колонка «Результат» для книги, которую ещё не конвертировали: что из неё получится */
void showPlanned(int i) {
    setCell(i, 2, L"→ " + targetOf(g_app.files[i]));
}

void refreshTitle() {
    std::wstring t = L"FB2 Конвертер";
    if (!g_app.files.empty()) t += L" — " + books((int)g_app.files.size());
    SetWindowTextW(g_app.hwnd, t.c_str());
}

void refreshConvertButton() {
    std::wstring t;
    int n = pendingCount();
    if (g_app.busy) {
        wchar_t buf[64];
        wsprintfW(buf, L"Конвертация… %d из %d", g_app.doneCount, g_app.runTotal);
        t = buf;
    } else if (g_app.files.empty()) {
        t = L"Добавьте книги, чтобы начать";
    } else if (n == 0) {
        t = L"Готово. Добавьте ещё книги или выберите другой формат";
    } else if (n == errorCount()) {
        t = L"Попробовать ещё раз";
    } else {
        t = L"Конвертировать " + booksAcc(n);
        if (g_app.format != 0) t += std::wstring(L" в ") + kFormatNames[g_app.format];
    }
    SetWindowTextW(g_app.btnConvert, t.c_str());
    EnableWindow(g_app.btnConvert, !g_app.busy && n > 0);
    InvalidateRect(g_app.btnConvert, NULL, FALSE);
}

void refreshAll() {
    bool has = !g_app.files.empty();
    bool idle = !g_app.busy;
    ShowWindow(g_app.list, has ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.drop, has ? SW_HIDE : SW_SHOW);
    ShowWindow(g_app.hint, has ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.btnRemove, has ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.btnClear, has ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.progress, g_app.busy ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.btnOpen, hasResults() ? SW_SHOW : SW_HIDE);

    EnableWindow(g_app.btnAdd, idle);
    EnableWindow(g_app.btnAddDir, idle);
    EnableWindow(g_app.btnRemove, idle);
    EnableWindow(g_app.btnClear, idle);
    EnableWindow(g_app.btnOpen, idle);
    for (int i = 0; i < 6; i++) {
        EnableWindow(g_app.chips[i], idle);
        InvalidateRect(g_app.chips[i], NULL, FALSE);
    }
    SetWindowTextW(g_app.fmtDesc, kFormatDesc[g_app.format]);

    if (has) {
        std::wstring h = books((int)g_app.files.size()) + L" в списке.  Delete — убрать,  двойной щелчок — показать файл";
        SetWindowTextW(g_app.hint, h.c_str());
    }
    refreshTitle();
    refreshConvertButton();
    layoutControls(g_app.hwnd);
}

void selectFormat(int f) {
    if (f < 0 || f > 5 || g_app.busy) return;
    if (f != g_app.format) {
        g_app.format = f;
        saveFormatIndex(f);
        /* другой формат — значит, всё надо конвертировать заново */
        for (size_t i = 0; i < g_app.files.size(); i++) {
            g_app.state[i] = ROW_PENDING;
            showPlanned((int)i);
        }
    }
    refreshAll();
}

/* ------------------------------- список --------------------------------- */

void addOne(const std::wstring& path) {
    for (size_t i = 0; i < g_app.files.size(); i++)
        if (_wcsicmp(g_app.files[i].c_str(), path.c_str()) == 0) return;

    int idx = (int)g_app.files.size();
    g_app.files.push_back(path);
    g_app.outputs.push_back(std::wstring());
    g_app.state.push_back(ROW_PENDING);

    LVITEMW it;
    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = idx;
    std::wstring shown = fileNameOf(path);
    it.pszText = (LPWSTR)shown.c_str();
    SendMessageW(g_app.list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    setCell(idx, 1, inputFormatOf(path));
    showPlanned(idx);
}

void addPaths(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> list;
    for (size_t i = 0; i < paths.size(); i++) {
        const std::wstring& p = paths[i];
        if (dirExistsW(p)) scanFolder(p, list, 0);
        else if (fileExistsW(p) && isSupportedName(p)) list.push_back(p);
    }
    int before = (int)g_app.files.size();
    for (size_t i = 0; i < list.size(); i++) addOne(list[i]);
    int added = (int)g_app.files.size() - before;
    refreshAll();
    if (added > 0) setStatus(L"Добавлено: " + books(added) + L".", false);
    else if (!list.empty()) setStatus(L"Эти книги уже в списке.", false);
    else setStatus(L"Здесь нет книг подходящих форматов (FB2, EPUB, MOBI, PDF, DjVu, DOC, DOCX, ODT, RTF, TXT, HTML).", true);
}

void removeSelected() {
    if (g_app.busy) return;
    int sel = (int)SendMessageW(g_app.list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (sel < 0) {
        setStatus(L"Выделите книгу в списке, чтобы убрать её.", false);
        return;
    }
    SendMessageW(g_app.list, LVM_DELETEITEM, (WPARAM)sel, 0);
    g_app.files.erase(g_app.files.begin() + sel);
    g_app.outputs.erase(g_app.outputs.begin() + sel);
    g_app.state.erase(g_app.state.begin() + sel);
    if (!g_app.files.empty()) {
        int next = sel < (int)g_app.files.size() ? sel : (int)g_app.files.size() - 1;
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_STATE;
        it.state = LVIS_SELECTED | LVIS_FOCUSED;
        it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(g_app.list, LVM_SETITEMSTATE, (WPARAM)next, (LPARAM)&it);
        SendMessageW(g_app.list, LVM_ENSUREVISIBLE, (WPARAM)next, FALSE);
    }
    setStatus(L"", false);
    refreshAll();
}

void clearAll() {
    if (g_app.busy) return;
    SendMessageW(g_app.list, LVM_DELETEALLITEMS, 0, 0);
    g_app.files.clear();
    g_app.outputs.clear();
    g_app.state.clear();
    setStatus(L"", false);
    refreshAll();
}

/* ------------------------------ конвертация ----------------------------- */

struct WorkerArg {
    std::string format;
    std::vector<int> rows;
    std::vector<std::wstring> files;
};

struct ItemResult {
    bool ok;
    std::wstring text;
    std::wstring outPath;
};

DWORD WINAPI workerProc(LPVOID param) {
    WorkerArg* arg = (WorkerArg*)param;
    std::string fmt = arg->format;
    std::vector<int> rows = arg->rows;
    std::vector<std::wstring> files = arg->files;
    delete arg;

    int ok = 0, fail = 0;
    for (size_t i = 0; i < files.size(); i++) {
        std::string target = resolveTarget(wPathToUtf8(files[i]), fmt);
        std::wstring outPath, err;
        ItemResult* res = new ItemResult();
        res->ok = convertOne(files[i], target, std::wstring(), false, &outPath, &err);
        if (res->ok) {
            ok++;
            res->outPath = outPath;
            res->text = L"Готово: " + fileNameOf(outPath);
        } else {
            fail++;
            res->text = L"Ошибка: " + err;
        }
        PostMessageW(g_app.hwnd, WM_APP_ITEM, (WPARAM)rows[i], (LPARAM)res);
    }
    PostMessageW(g_app.hwnd, WM_APP_DONE, (WPARAM)ok, (LPARAM)fail);
    return 0;
}

void startConvert() {
    if (g_app.busy || pendingCount() == 0) return;

    WorkerArg* arg = new WorkerArg();
    arg->format = kFormats[g_app.format];
    for (size_t i = 0; i < g_app.files.size(); i++) {
        if (g_app.state[i] == ROW_OK) continue;
        arg->rows.push_back((int)i);
        arg->files.push_back(g_app.files[i]);
        g_app.state[i] = ROW_QUEUED;
        setCell((int)i, 2, L"ждёт очереди…");
    }

    g_app.busy = true;
    g_app.doneCount = 0;
    g_app.runTotal = (int)arg->rows.size();
    SendMessageW(g_app.progress, PBM_SETRANGE32, 0, (LPARAM)g_app.runTotal);
    SendMessageW(g_app.progress, PBM_SETPOS, 0, 0);
    setStatus(L"Идёт конвертация. Готовые книги появляются рядом с исходными.", false);
    refreshAll();

    DWORD tid = 0;
    if (g_app.thread) { CloseHandle(g_app.thread); g_app.thread = NULL; }
    g_app.thread = CreateThread(NULL, 0, workerProc, arg, 0, &tid);
    if (!g_app.thread) {
        delete arg;
        g_app.busy = false;
        for (size_t i = 0; i < g_app.files.size(); i++)
            if (g_app.state[i] == ROW_QUEUED) { g_app.state[i] = ROW_PENDING; showPlanned((int)i); }
        setStatus(L"Не удалось запустить конвертацию.", true);
        refreshAll();
    }
}

/* -------------------------------- диалоги -------------------------------- */

void chooseFiles() {
    if (g_app.busy) return;
    wchar_t buf[32768];
    buf[0] = 0;
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_app.hwnd;
    ofn.lpstrFilter = L"Книги\0*.fb2;*.epub;*.mobi;*.prc;*.azw;*.azw3;*.pdf;*.djvu;*.djv;*.docx;*.doc;*.odt;*.rtf;*.txt;*.html;*.htm;*.fb2.zip;*.fbz;*.zip\0"
                      L"FB2 (*.fb2)\0*.fb2;*.fb2.zip;*.fbz\0"
                      L"EPUB (*.epub)\0*.epub\0"
                      L"MOBI (*.mobi;*.prc;*.azw)\0*.mobi;*.prc;*.azw;*.azw3\0"
                      L"PDF, DjVu (*.pdf;*.djvu)\0*.pdf;*.djvu;*.djv\0"
                      L"Документы (*.docx;*.doc;*.odt;*.rtf)\0*.docx;*.doc;*.odt;*.rtf\0"
                      L"Тексты (*.txt)\0*.txt\0"
                      L"HTML (*.html;*.htm)\0*.html;*.htm\0"
                      L"Все файлы (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 32768;
    ofn.lpstrTitle = L"Выберите книги (можно несколько)";
    /* последняя папка — из fb2conv.ini, а не из истории Windows */
    std::wstring startDir = iniGet(L"folder");
    if (!startDir.empty() && dirExistsW(startDir)) ofn.lpstrInitialDir = startDir.c_str();
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY |
                OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;

    BOOL picked;
    {
        NoTraceGuard guard; /* история диалога в реестре откатывается при выходе из блока */
        picked = GetOpenFileNameW(&ofn);
    }
    if (!picked) return;

    std::vector<std::wstring> paths;
    const wchar_t* p = buf;
    std::wstring first = p;
    p += first.size() + 1;
    if (*p == 0) {
        paths.push_back(first);
    } else {
        while (*p) {
            std::wstring name = p;
            p += name.size() + 1;
            paths.push_back(first + L"\\" + name);
        }
    }
    iniSet(L"folder", paths.size() == 1 ? folderOf(paths[0]) : first);
    addPaths(paths);
}

int CALLBACK browseCallback(HWND hwnd, UINT msg, LPARAM, LPARAM data) {
    if (msg == BFFM_INITIALIZED && data) SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

void chooseFolder() {
    if (g_app.busy) return;
    std::wstring startDir = iniGet(L"folder");
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = g_app.hwnd;
    bi.lpszTitle = L"Выберите папку с книгами. Вложенные папки тоже будут просмотрены.";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browseCallback;
    bi.lParam = startDir.empty() ? 0 : (LPARAM)startDir.c_str();

    LPITEMIDLIST pidl;
    {
        NoTraceGuard guard;
        pidl = SHBrowseForFolderW(&bi);
    }
    if (!pidl) return;
    wchar_t path[MAX_PATH];
    BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok) return;
    std::vector<std::wstring> paths(1, std::wstring(path));
    iniSet(L"folder", path);
    addPaths(paths);
}

void revealInExplorer(const std::wstring& path) {
    if (path.empty()) return;
    if (!fileExistsW(path)) {
        std::wstring dir = folderOf(path);
        if (dir.empty() || !dirExistsW(dir)) return;
        ShellExecuteW(NULL, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(NULL, L"open", L"explorer.exe", args.c_str(), NULL, SW_SHOWNORMAL);
}

void openResult() {
    /* открываем папку с последним успешным результатом */
    for (size_t i = g_app.outputs.size(); i-- > 0;) {
        if (!g_app.outputs[i].empty() && fileExistsW(g_app.outputs[i])) {
            revealInExplorer(g_app.outputs[i]);
            return;
        }
    }
}

/* ------------------------------ рисование ------------------------------- */

void roundBox(HDC dc, const RECT& r, COLORREF fill, COLORREF border, int radius) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

void drawButtonText(HDC dc, HWND btn, RECT r, HFONT font, COLORREF color) {
    wchar_t text[256];
    GetWindowTextW(btn, text, 256);
    HGDIOBJ of = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of);
}

/* плашка формата и большая кнопка — свои, чтобы выглядели одинаково от XP до 11 */
void drawOwnerButton(const DRAWITEMSTRUCT* d) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    bool pressed = (d->itemState & ODS_SELECTED) != 0;
    bool disabled = (d->itemState & ODS_DISABLED) != 0;
    bool focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);

    FillRect(dc, &r, GetSysColorBrush(COLOR_WINDOW));
    RECT box = r;
    InflateRect(&box, -1, -1);

    if (d->CtlID == IDC_CONVERT) {
        COLORREF fill = disabled ? kDisabledBg : (pressed ? kAccentDark : kAccent);
        roundBox(dc, box, fill, fill, S(10));
        drawButtonText(dc, d->hwndItem, box, g_app.fontBig, disabled ? kDisabledFg : RGB(255, 255, 255));
    } else {
        int f = (int)d->CtlID - IDC_FMT0;
        bool sel = f == g_app.format;
        COLORREF fill = sel ? kAccent : (pressed ? kChipPressed : RGB(255, 255, 255));
        COLORREF border = sel ? kAccent : kBorder;
        COLORREF text = sel ? RGB(255, 255, 255) : (disabled ? kDisabledFg : kText);
        if (disabled && sel) fill = border = RGB(120, 150, 185);
        roundBox(dc, box, fill, border, S(8));
        drawButtonText(dc, d->hwndItem, box, sel ? g_app.fontBold : g_app.font, text);
    }
    if (focus) {
        RECT fr = box;
        InflateRect(&fr, -S(3), -S(3));
        SetTextColor(dc, RGB(0, 0, 0));
        SetBkColor(dc, RGB(255, 255, 255));
        DrawFocusRect(dc, &fr);
    }
}

/* пустой список: большая зона «перетащите сюда», по щелчку — выбор файлов */
LRESULT CALLBACK dropProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT r;
            GetClientRect(hwnd, &r);
            FillRect(dc, &r, GetSysColorBrush(COLOR_WINDOW));

            HBRUSH br = CreateSolidBrush(kDropFill);
            HPEN pen = CreatePen(PS_DASH, 1, kDropBorder);
            HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
            RoundRect(dc, r.left, r.top, r.right - 1, r.bottom - 1, S(14), S(14));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(br);
            DeleteObject(pen);

            int iconSz = S(48);
            int lineBig = S(30), lineSmall = S(22);
            int blockH = iconSz + S(14) + lineBig + lineSmall + S(6) + lineSmall;
            int y = (r.bottom - blockH) / 2;
            if (y < S(8)) y = S(8);
            if (g_app.icon)
                DrawIconEx(dc, (r.right - iconSz) / 2, y, g_app.icon, iconSz, iconSz, 0, NULL, DI_NORMAL);
            y += iconSz + S(14);

            SetBkMode(dc, TRANSPARENT);
            RECT t = {S(10), y, r.right - S(10), y + lineBig};
            HGDIOBJ of = SelectObject(dc, g_app.fontDrop);
            SetTextColor(dc, kText);
            DrawTextW(dc, L"Перетащите сюда книги или папки", -1, &t,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += lineBig;
            t.top = y; t.bottom = y + lineSmall;
            SelectObject(dc, g_app.font);
            SetTextColor(dc, kAccent);
            DrawTextW(dc, L"или щёлкните здесь, чтобы выбрать файлы", -1, &t,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += lineSmall + S(6);
            t.top = y; t.bottom = y + lineSmall;
            SetTextColor(dc, kMuted);
            DrawTextW(dc, L"FB2 · EPUB · MOBI · PDF · DjVu · DOC(X) · ODT · RTF · TXT · HTML", -1, &t,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, of);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        case WM_LBUTTONUP:
            chooseFiles();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* -------------------------------- разметка ------------------------------- */

void layoutControls(HWND hwnd) {
    if (!hwnd || !g_app.list) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    const int pad = S(16);
    const int btnH = S(28);
    int inner = W - pad * 2;

    /* шаг 1: заголовок и кнопки добавления */
    int y = pad;
    int addW = S(150), dirW = S(150);
    MoveWindow(g_app.btnAddDir, W - pad - dirW, y, dirW, btnH, TRUE);
    MoveWindow(g_app.btnAdd, W - pad - dirW - S(8) - addW, y, addW, btnH, TRUE);
    MoveWindow(g_app.head1, pad, y + (btnH - S(20)) / 2, inner - addW - dirW - S(16), S(20), TRUE);
    int listTop = y + btnH + S(10);

    /* снизу вверх: кнопка, строка состояния, описание формата, плашки, заголовок шага 2 */
    int convertH = S(44);
    int convertY = H - pad - convertH;
    int statusH = S(20);
    int statusY = convertY - S(10) - statusH;
    int descH = S(20);
    int descY = statusY - S(8) - descH;
    int chipH = S(32);
    int chipY = descY - S(6) - chipH;
    int head2Y = chipY - S(8) - S(20);
    int hintH = S(26);
    int hintY = head2Y - S(16) - hintH;
    /* без книг строки-подсказки под списком нет — зона перетаскивания занимает и её место */
    int listBottom = g_app.files.empty() ? hintY + hintH : hintY - S(6);
    if (listBottom < listTop + S(60)) listBottom = listTop + S(60);

    MoveWindow(g_app.list, pad, listTop, inner, listBottom - listTop, TRUE);
    MoveWindow(g_app.drop, pad, listTop, inner, listBottom - listTop, TRUE);
    if (inner != g_app.lastListW && inner > S(200)) {
        g_app.lastListW = inner;
        int total = inner - GetSystemMetrics(SM_CXVSCROLL) - S(4);
        int fmtW = S(70);
        int nameW = (total - fmtW) * 55 / 100;
        SendMessageW(g_app.list, LVM_SETCOLUMNWIDTH, 0, MAKELPARAM(nameW, 0));
        SendMessageW(g_app.list, LVM_SETCOLUMNWIDTH, 1, MAKELPARAM(fmtW, 0));
        SendMessageW(g_app.list, LVM_SETCOLUMNWIDTH, 2, MAKELPARAM(total - fmtW - nameW, 0));
    }

    int smallW = S(96);
    MoveWindow(g_app.btnClear, W - pad - smallW, hintY, smallW, hintH, TRUE);
    MoveWindow(g_app.btnRemove, W - pad - smallW * 2 - S(8), hintY, smallW, hintH, TRUE);
    MoveWindow(g_app.hint, pad, hintY + (hintH - S(18)) / 2, inner - smallW * 2 - S(16), S(18), TRUE);

    /* шаг 2: формат */
    MoveWindow(g_app.head2, pad, head2Y, inner, S(20), TRUE);
    int gap = S(8);
    int chipW = (inner - gap * 5) / 6;
    if (chipW > S(100)) chipW = S(100);
    for (int i = 0; i < 6; i++)
        MoveWindow(g_app.chips[i], pad + i * (chipW + gap), chipY, chipW, chipH, TRUE);
    MoveWindow(g_app.fmtDesc, pad, descY, inner, descH, TRUE);

    /* состояние: во время работы слева полоса прогресса */
    int textX = pad;
    if (g_app.busy) {
        int progW = inner * 2 / 5;
        MoveWindow(g_app.progress, pad, statusY + (statusH - S(14)) / 2, progW, S(14), TRUE);
        textX = pad + progW + S(12);
    }
    MoveWindow(g_app.status, textX, statusY, W - pad - textX, statusH, TRUE);

    /* большая кнопка; «Показать готовые» появляется слева, когда есть результаты */
    int x = pad;
    if (GetWindowLongW(g_app.btnOpen, GWL_STYLE) & WS_VISIBLE) {
        int openW = S(190);
        MoveWindow(g_app.btnOpen, pad, convertY, openW, convertH, TRUE);
        x = pad + openW + S(10);
    }
    MoveWindow(g_app.btnConvert, x, convertY, W - pad - x, convertH, TRUE);
}

HFONT scaledFont(const LOGFONTW& base, int num, int den, int weight) {
    LOGFONTW lf = base;
    lf.lfHeight = MulDiv(lf.lfHeight, num, den);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW(&lf);
}

HWND makeChild(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0) {
    return CreateWindowExW(exStyle, cls, text, WS_CHILD | style, 0, 0, 10, 10, g_app.hwnd,
                           (HMENU)(INT_PTR)id, (HINSTANCE)GetWindowLongPtrW(g_app.hwnd, GWLP_HINSTANCE), NULL);
}

void createControls(HWND hwnd) {
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        /* Windows XP: структура без iPaddedBorderWidth */
        ZeroMemory(&ncm, sizeof(ncm));
        ncm.cbSize = sizeof(ncm) - sizeof(int);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm) - sizeof(int), &ncm, 0);
    }
    LOGFONTW base = ncm.lfMessageFont;
    if (!base.lfHeight) GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(base), &base);
    g_app.font = scaledFont(base, 1, 1, FW_NORMAL);
    g_app.fontBold = scaledFont(base, 1, 1, FW_BOLD);
    g_app.fontBig = scaledFont(base, 5, 4, FW_BOLD);
    g_app.fontDrop = scaledFont(base, 3, 2, FW_BOLD);
    g_app.icon = (HICON)LoadImageW((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE), L"APPICON",
                                   IMAGE_ICON, S(48), S(48), 0);

    g_app.head1 = makeChild(L"STATIC", L"1.  Какие книги конвертировать", WS_VISIBLE | SS_LEFT, IDC_HEAD1);
    g_app.btnAdd = makeChild(L"BUTTON", L"Добавить книги…", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, IDC_ADD);
    g_app.btnAddDir = makeChild(L"BUTTON", L"Добавить папку…", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, IDC_ADDDIR);

    g_app.drop = makeChild(L"FB2DropZone", NULL, WS_VISIBLE, IDC_DROP);
    g_app.list = makeChild(WC_LISTVIEWW, NULL,
                           WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                           IDC_LIST, WS_EX_CLIENTEDGE);
    SendMessageW(g_app.list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[] = {L"Книга", L"Формат", L"Результат"};
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW col;
        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = (LPWSTR)cols[i];
        col.cx = S(150);
        col.iSubItem = i;
        SendMessageW(g_app.list, LVM_INSERTCOLUMNW, i, (LPARAM)&col);
    }

    g_app.hint = makeChild(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, IDC_HINT);
    g_app.btnRemove = makeChild(L"BUTTON", L"Убрать", WS_TABSTOP | BS_PUSHBUTTON, IDC_REMOVE);
    g_app.btnClear = makeChild(L"BUTTON", L"Очистить", WS_TABSTOP | BS_PUSHBUTTON, IDC_CLEAR);

    g_app.head2 = makeChild(L"STATIC", L"2.  Во что конвертировать", WS_VISIBLE | SS_LEFT, IDC_HEAD2);
    for (int i = 0; i < 6; i++)
        g_app.chips[i] = makeChild(L"BUTTON", kFormatNames[i], WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, IDC_FMT0 + i);
    g_app.fmtDesc = makeChild(L"STATIC", L"", WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, IDC_FMTDESC);

    g_app.progress = makeChild(PROGRESS_CLASSW, NULL, PBS_SMOOTH, IDC_PROGRESS);
    g_app.status = makeChild(L"STATIC", L"", WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, IDC_STATUS);
    g_app.btnOpen = makeChild(L"BUTTON", L"Показать готовые книги", WS_TABSTOP | BS_PUSHBUTTON, IDC_OPEN);
    g_app.btnConvert = makeChild(L"BUTTON", L"", WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, IDC_CONVERT);

    HWND normal[] = {g_app.btnAdd, g_app.btnAddDir, g_app.list, g_app.hint, g_app.btnRemove,
                     g_app.btnClear, g_app.fmtDesc, g_app.status, g_app.btnOpen, g_app.progress};
    for (int i = 0; i < (int)(sizeof(normal) / sizeof(normal[0])); i++)
        SendMessageW(normal[i], WM_SETFONT, (WPARAM)g_app.font, TRUE);
    SendMessageW(g_app.head1, WM_SETFONT, (WPARAM)g_app.fontBold, TRUE);
    SendMessageW(g_app.head2, WM_SETFONT, (WPARAM)g_app.fontBold, TRUE);

    g_app.format = loadFormatIndex();
}

/* -------------------------------- окно ----------------------------------- */

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_app.hwnd = hwnd;
            createControls(hwnd);
            DragAcceptFiles(hwnd, TRUE);
            refreshAll();
            return 0;

        case WM_SIZE:
            layoutControls(hwnd);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = (MINMAXINFO*)lParam;
            mm->ptMinTrackSize.x = S(600);
            mm->ptMinTrackSize.y = S(480);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wParam;
            HWND c = (HWND)lParam;
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            COLORREF col = kText;
            if (c == g_app.hint || c == g_app.fmtDesc) col = kMuted;
            if (c == g_app.status) col = g_app.statusError ? kErr : kText;
            SetTextColor(dc, col);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }

        case WM_DRAWITEM:
            drawOwnerButton((const DRAWITEMSTRUCT*)lParam);
            return TRUE;

        case WM_DROPFILES: {
            HDROP drop = (HDROP)wParam;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
            std::vector<std::wstring> paths;
            for (UINT i = 0; i < n; i++) {
                UINT len = DragQueryFileW(drop, i, NULL, 0);
                if (!len) continue;
                std::wstring p;
                p.resize((size_t)len + 1);
                UINT got = DragQueryFileW(drop, i, &p[0], len + 1);
                p.resize(got);
                paths.push_back(p);
            }
            DragFinish(drop);
            if (g_app.busy) {
                setStatus(L"Дождитесь окончания конвертации, потом добавляйте новые книги.", true);
                return 0;
            }
            if (!paths.empty()) iniSet(L"folder", dirExistsW(paths[0]) ? paths[0] : folderOf(paths[0]));
            addPaths(paths);
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (code != BN_CLICKED) return 0;
            if (id == IDC_ADD) chooseFiles();
            else if (id == IDC_ADDDIR) chooseFolder();
            else if (id == IDC_REMOVE) removeSelected();
            else if (id == IDC_CLEAR) clearAll();
            else if (id == IDC_CONVERT) startConvert();
            else if (id == IDC_OPEN) openResult();
            else if (id >= IDC_FMT0 && id < IDC_FMT0 + 6) selectFormat(id - IDC_FMT0);
            return 0;
        }

        case WM_APP_ITEM: {
            int idx = (int)wParam;
            ItemResult* res = (ItemResult*)lParam;
            if (res) {
                if (idx >= 0 && idx < (int)g_app.files.size()) {
                    g_app.state[idx] = res->ok ? ROW_OK : ROW_ERR;
                    if (res->ok) g_app.outputs[idx] = res->outPath;
                    setCell(idx, 2, res->text);
                }
                delete res;
            }
            g_app.doneCount++;
            SendMessageW(g_app.progress, PBM_SETPOS, (WPARAM)g_app.doneCount, 0);
            refreshConvertButton();
            return 0;
        }

        case WM_APP_DONE: {
            int ok = (int)wParam, fail = (int)lParam;
            g_app.busy = false;
            if (g_app.thread) { CloseHandle(g_app.thread); g_app.thread = NULL; }
            if (fail == 0) {
                setStatus(L"Готово: " + books(ok) + L". Новые файлы лежат рядом с исходными.", false);
            } else {
                wchar_t buf[96];
                wsprintfW(buf, L"Готово: %d из %d. Не получилось: %d — причина в колонке «Результат».",
                          ok, ok + fail, fail);
                setStatus(buf, true);
            }
            refreshAll();
            return 0;
        }

        case WM_NOTIFY: {
            NMHDR* nh = (NMHDR*)lParam;
            if (!nh || nh->idFrom != IDC_LIST) return 0;
            if (nh->code == LVN_KEYDOWN) {
                NMLVKEYDOWN* k = (NMLVKEYDOWN*)lParam;
                if (k->wVKey == VK_DELETE && !g_app.busy) removeSelected();
                return 0;
            }
            if (nh->code == NM_DBLCLK) {
                int sel = (int)SendMessageW(g_app.list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
                if (sel >= 0 && sel < (int)g_app.files.size()) {
                    bool done = !g_app.outputs[sel].empty() && fileExistsW(g_app.outputs[sel]);
                    revealInExplorer(done ? g_app.outputs[sel] : g_app.files[sel]);
                }
                return 0;
            }
            if (nh->code == NM_CUSTOMDRAW) {
                /* цвет колонки «Результат»: серый — план, зелёный — готово, красный — ошибка */
                NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
                if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                    int row = (int)cd->nmcd.dwItemSpec;
                    cd->clrText = GetSysColor(COLOR_WINDOWTEXT);
                    if (cd->iSubItem == 1) cd->clrText = kMuted;
                    if (cd->iSubItem == 2 && row >= 0 && row < (int)g_app.state.size()) {
                        int st = g_app.state[row];
                        cd->clrText = st == ROW_OK ? kOk : (st == ROW_ERR ? kErr : kMuted);
                    }
                    return CDRF_NEWFONT;
                }
                return CDRF_DODEFAULT;
            }
            return 0;
        }

        case WM_CLOSE:
            if (g_app.busy) {
                if (MessageBoxW(hwnd, L"Идёт конвертация. Дождаться окончания и закрыть?",
                                L"FB2 Конвертер", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) != IDYES)
                    return 0;
                /* даём текущему файлу дописаться, не замораживая окно */
                DWORD start = GetTickCount();
                while (g_app.busy && GetTickCount() - start < 20000) {
                    MSG m;
                    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&m);
                        DispatchMessageW(&m);
                    }
                    MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
                }
            }
            saveWindowPlacement(hwnd);
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} /* namespace */

int runGui(HINSTANCE hInst, const std::vector<std::wstring>& files) {
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    OleInitialize(NULL); /* нужен диалогу выбора папки нового вида */

    WNDCLASSEXW dc;
    ZeroMemory(&dc, sizeof(dc));
    dc.cbSize = sizeof(dc);
    dc.lpfnWndProc = dropProc;
    dc.hInstance = hInst;
    dc.hCursor = LoadCursorW(NULL, IDC_HAND);
    dc.lpszClassName = L"FB2DropZone";
    RegisterClassExW(&dc);

    const wchar_t* kClass = L"FB2ConverterWindow";
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIconW(hInst, L"APPICON");
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    initDpi();
    int dw = S(700), dh = S(560);
    RECT rc = {0, 0, dw, dh};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w > sw - 60) w = sw - 60;
    if (h > sh - 80) h = sh - 80;
    int x = (sw - w) / 2, y = (sh - h) / 2;

    /* прошлое положение окна из fb2conv.ini — если оно целиком на рабочем столе */
    int ix = iniGetInt(L"x", -32000), iy = iniGetInt(L"y", -32000);
    int iw = iniGetInt(L"w", 0), ih = iniGetInt(L"h", 0);
    RECT desk = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    desk.right = desk.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    desk.bottom = desk.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (iw >= S(600) && ih >= S(480) && ix >= desk.left && iy >= desk.top &&
        ix + iw <= desk.right && iy + ih <= desk.bottom) {
        x = ix; y = iy; w = iw; h = ih;
    }

    HWND hwnd = CreateWindowExW(0, kClass, L"FB2 Конвертер",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, w, h, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, iniGetInt(L"maximized", 0) ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd);

    if (!files.empty()) addPaths(files);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* Ctrl+O — добавить книги */
        if (msg.message == WM_KEYDOWN && msg.wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            chooseFiles();
            continue;
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    HFONT fonts[] = {g_app.font, g_app.fontBold, g_app.fontBig, g_app.fontDrop};
    for (int i = 0; i < 4; i++)
        if (fonts[i]) DeleteObject(fonts[i]);
    OleUninitialize();
    return 0;
}
