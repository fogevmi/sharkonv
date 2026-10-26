/* Проверка NoTraceGuard на настоящем диалоге открытия файлов.
   1) диалог без защиты: ветки истории должны измениться (иначе проверка ничего не доказывает);
   2) диалог под NoTraceGuard: все ветки должны совпасть с исходными байт-в-байт.
   В конце реестр возвращается к исходному снимку. Файл для выбора — первый аргумент;
   его папка должна быть новой, чтобы Windows завела для неё shellbags.
   Сборка: см. tools\test.ps1. */
#include "../src/notrace.h"
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>

static std::wstring g_pick, g_dir;

static bool same(const RegShot& a, const RegShot& b) {
    if (a.exists != b.exists || a.vals.size() != b.vals.size() || a.subs.size() != b.subs.size()) return false;
    for (std::map<std::wstring, RegShot::Val>::const_iterator it = a.vals.begin(); it != a.vals.end(); ++it) {
        std::map<std::wstring, RegShot::Val>::const_iterator o = b.vals.find(it->first);
        if (o == b.vals.end() || o->second.type != it->second.type || o->second.data != it->second.data) return false;
    }
    for (std::map<std::wstring, RegShot>::const_iterator it = a.subs.begin(); it != a.subs.end(); ++it) {
        std::map<std::wstring, RegShot>::const_iterator o = b.subs.find(it->first);
        if (o == b.subs.end() || !same(it->second, o->second)) return false;
    }
    return true;
}

/* находит диалог своего процесса, вписывает имя файла и жмёт «Открыть» */
static DWORD WINAPI autoPick(LPVOID) {
    for (int i = 0; i < 100; i++) {
        Sleep(100);
        HWND dlg = FindWindowW(L"#32770", L"notrace-test");
        if (!dlg) continue;
        Sleep(1000); /* первый запуск диалога долго инициализирует оболочку */
        HWND name = GetDlgItem(dlg, 0x47C); /* cmb13: поле «Имя файла» */
        if (!name) name = GetDlgItem(dlg, 0x480); /* edt1 в старом виде диалога */
        if (name) SetWindowTextW(name, g_pick.c_str());
        PostMessageW(dlg, WM_COMMAND, IDOK, 0);
        return 0;
    }
    return 1;
}

static bool runDialog() {
    wchar_t buf[1024] = {0};
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Книги\0*.fb2;*.epub;*.txt\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = L"notrace-test";
    ofn.lpstrInitialDir = g_dir.c_str(); /* диалог показывает новую папку -> shellbags */
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | 0x02000000;
    HANDLE t = CreateThread(NULL, 0, autoPick, NULL, 0, NULL);
    BOOL ok = GetOpenFileNameW(&ofn);
    WaitForSingleObject(t, 15000);
    CloseHandle(t);
    if (!ok) printf("  dialog cancelled/failed, CommDlgExtendedError=0x%lx\n", CommDlgExtendedError());
    else if (_wcsicmp(buf, g_pick.c_str()) != 0) printf("  dialog returned another path (len %d)\n", (int)wcslen(buf));
    return ok && _wcsicmp(buf, g_pick.c_str()) == 0;
}

int main() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 2) { printf("usage: notrace_test <file>\n"); return 2; }
    g_pick = argv[1];
    g_dir = g_pick.substr(0, g_pick.find_last_of(L"\\/"));

    /* охраняемые ветки + RecentDocs, куда диалог писать не должен вовсе */
    std::vector<std::wstring> keys;
    for (int i = 0; i < historyKeyCount(); i++) keys.push_back(historyKey(i));
    keys.push_back(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\RecentDocs");
    const size_t n = keys.size();

    std::vector<RegShot> before;
    for (size_t i = 0; i < n; i++) before.push_back(regSnapshot(HKEY_CURRENT_USER, keys[i]));

    bool pickedRaw = runDialog();
    bool rawChanged = false;
    for (size_t i = 0; i < n; i++) {
        if (!same(before[i], regSnapshot(HKEY_CURRENT_USER, keys[i]))) {
            rawChanged = true;
            wprintf(L"  without guard the dialog changed HKCU\\%ls\n", keys[i].c_str());
        }
    }
    for (size_t i = 0; i < n; i++) regRestore(HKEY_CURRENT_USER, keys[i], before[i]);
    bool restored = true;
    for (size_t i = 0; i < n; i++) restored = restored && same(before[i], regSnapshot(HKEY_CURRENT_USER, keys[i]));

    bool pickedGuarded;
    DWORD t0 = GetTickCount(), tGuard;
    {
        NoTraceGuard guard;
        tGuard = GetTickCount() - t0;
        pickedGuarded = runDialog();
    }
    bool guardedSame = true;
    for (size_t i = 0; i < n; i++) {
        if (!same(before[i], regSnapshot(HKEY_CURRENT_USER, keys[i]))) {
            guardedSame = false;
            wprintf(L"  WITH guard still changed HKCU\\%ls\n", keys[i].c_str());
        }
    }

    printf("dialog picked file: raw=%d guarded=%d\n", pickedRaw, pickedGuarded);
    printf("without guard: registry changed = %d (expected 1)\n", rawChanged);
    printf("restore of raw changes worked   = %d (expected 1)\n", restored);
    printf("with NoTraceGuard: unchanged    = %d (expected 1)\n", guardedSame);
    printf("guard snapshot took %lu ms\n", (unsigned long)tGuard);
    bool ok = pickedRaw && pickedGuarded && rawChanged && restored && guardedSame;
    printf(ok ? "NOTRACE OK\n" : "NOTRACE FAIL\n");
    return ok ? 0 : 1;
}
