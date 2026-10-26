/* Портативность без следов: стандартные диалоги Windows сами пишут историю — последние
   файлы и папки (ComDlg32) и настройки вида показанных папок («shellbags»). NoTraceGuard
   снимает эти ветки HKCU перед диалогом и возвращает их в точности как были после него. */
#ifndef FB2C_NOTRACE_H
#define FB2C_NOTRACE_H

#include "common.h"

struct RegShot {
    struct Val {
        DWORD type;
        Bytes data;
    };
    bool exists;
    std::map<std::wstring, Val> vals;
    std::map<std::wstring, RegShot> subs;
    RegShot() : exists(false) {}
};

RegShot regSnapshot(HKEY root, const std::wstring& path);
void regRestore(HKEY root, const std::wstring& path, const RegShot& shot);

/* ветки HKCU, которые охраняет NoTraceGuard */
int historyKeyCount();
const wchar_t* historyKey(int i);

class NoTraceGuard {
public:
    NoTraceGuard();
    ~NoTraceGuard();
private:
    std::vector<RegShot> shots_;
    NoTraceGuard(const NoTraceGuard&);
    NoTraceGuard& operator=(const NoTraceGuard&);
};

#endif
