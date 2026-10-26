/* Снимок и откат ветки реестра (только Win32 API, есть в XP). */
#include "notrace.h"
#include <shlwapi.h>

/* Куда диалоги открытия файлов и выбора папки пишут историю:
   ComDlg32 — последние файлы и папки; Shell/ShellNoRoam (XP) и Local Settings\...\Shell
   (Vista+) — «shellbags», настройки вида каждой показанной папки (BagMRU/Bags). */
static const wchar_t* kHistoryKeys[] = {
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ComDlg32",
    L"Software\\Microsoft\\Windows\\Shell",
    L"Software\\Microsoft\\Windows\\ShellNoRoam",
    L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell",
};
static const int kHistoryKeyCount = sizeof(kHistoryKeys) / sizeof(kHistoryKeys[0]);

/* BagMRU вложен по глубине папок, поэтому предел глубины с запасом */
static const int kMaxDepth = 64;

static void snapKey(HKEY key, RegShot& shot, int depth) {
    shot.exists = true;
    if (depth > kMaxDepth) return;
    DWORD nSub = 0, maxSub = 0, nVal = 0, maxName = 0, maxData = 0;
    if (RegQueryInfoKeyW(key, NULL, NULL, NULL, &nSub, &maxSub, NULL, &nVal, &maxName, &maxData,
                         NULL, NULL) != ERROR_SUCCESS)
        return;

    std::vector<wchar_t> name(maxName + 2);
    Bytes data(maxData + 2);
    for (DWORD i = 0; i < nVal; i++) {
        DWORD nl = (DWORD)name.size(), dl = (DWORD)data.size(), type = 0;
        if (RegEnumValueW(key, i, &name[0], &nl, NULL, &type, &data[0], &dl) != ERROR_SUCCESS) continue;
        RegShot::Val v;
        v.type = type;
        v.data.assign(data.begin(), data.begin() + dl);
        shot.vals[std::wstring(&name[0], nl)] = v;
    }

    std::vector<wchar_t> sub(maxSub + 2);
    for (DWORD i = 0; i < nSub; i++) {
        DWORD sl = (DWORD)sub.size();
        if (RegEnumKeyExW(key, i, &sub[0], &sl, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) continue;
        std::wstring sn(&sub[0], sl);
        HKEY k;
        if (RegOpenKeyExW(key, sn.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        snapKey(k, shot.subs[sn], depth + 1);
        RegCloseKey(k);
    }
}

RegShot regSnapshot(HKEY root, const std::wstring& path) {
    RegShot shot;
    HKEY key;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return shot;
    snapKey(key, shot, 0);
    RegCloseKey(key);
    return shot;
}

/* Пишем только то, что изменилось: лишняя запись тоже была бы следом. */
static void restoreKey(HKEY key, const RegShot& shot, int depth) {
    if (depth > kMaxDepth) return;
    RegShot now;
    snapKey(key, now, depth);

    for (std::map<std::wstring, RegShot::Val>::const_iterator it = now.vals.begin(); it != now.vals.end(); ++it)
        if (shot.vals.find(it->first) == shot.vals.end()) RegDeleteValueW(key, it->first.c_str());
    for (std::map<std::wstring, RegShot::Val>::const_iterator it = shot.vals.begin(); it != shot.vals.end(); ++it) {
        std::map<std::wstring, RegShot::Val>::const_iterator cur = now.vals.find(it->first);
        if (cur != now.vals.end() && cur->second.type == it->second.type && cur->second.data == it->second.data)
            continue;
        RegSetValueExW(key, it->first.c_str(), 0, it->second.type,
                       it->second.data.empty() ? NULL : &it->second.data[0], (DWORD)it->second.data.size());
    }

    for (std::map<std::wstring, RegShot>::const_iterator it = now.subs.begin(); it != now.subs.end(); ++it) {
        std::map<std::wstring, RegShot>::const_iterator was = shot.subs.find(it->first);
        if (was == shot.subs.end()) {
            SHDeleteKeyW(key, it->first.c_str());
            continue;
        }
        HKEY k;
        if (RegOpenKeyExW(key, it->first.c_str(), 0, KEY_READ | KEY_WRITE, &k) != ERROR_SUCCESS) continue;
        restoreKey(k, was->second, depth + 1);
        RegCloseKey(k);
    }
}

void regRestore(HKEY root, const std::wstring& path, const RegShot& shot) {
    if (!shot.exists) {
        SHDeleteKeyW(root, path.c_str());
        return;
    }
    HKEY key;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) return;
    restoreKey(key, shot, 0);
    RegCloseKey(key);
}

int historyKeyCount() { return kHistoryKeyCount; }

const wchar_t* historyKey(int i) { return kHistoryKeys[i]; }

NoTraceGuard::NoTraceGuard() {
    for (int i = 0; i < kHistoryKeyCount; i++) shots_.push_back(regSnapshot(HKEY_CURRENT_USER, kHistoryKeys[i]));
}

NoTraceGuard::~NoTraceGuard() {
    for (int i = 0; i < kHistoryKeyCount; i++) regRestore(HKEY_CURRENT_USER, kHistoryKeys[i], shots_[(size_t)i]);
}
