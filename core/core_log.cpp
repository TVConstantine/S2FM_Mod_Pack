// The log and the paths beside the DLL.
#include "core.h"

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <mutex>

namespace {
std::mutex g_logMutex;
FILE* g_log = nullptr;
bool g_logTried = false;
}  // namespace

const std::wstring& CoreDir() {
    static std::wstring dir;
    if (dir.empty()) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&CoreDir, &self);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(self, path, MAX_PATH);
        if (wchar_t* slash = wcsrchr(path, L'\\')) *slash = 0;
        dir = path;
    }
    return dir;
}

std::wstring CorePath(const wchar_t* name) { return CoreDir() + L"\\" + name; }

std::wstring CoreGameDir() {
    // game\bin\win64 -> game
    std::wstring d = CoreDir();
    for (int up = 0; up < 2; ++up) {
        const size_t slash = d.find_last_of(L'\\');
        if (slash == std::wstring::npos) break;
        d.resize(slash);
    }
    return d;
}

// One line a call, with a time stamp, flushed: what a crash leaves behind has to be on disk.
void Log(const char* fmt, ...) {
    char line[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof(line), fmt, a);
    va_end(a);
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (!g_log && !g_logTried) {
        g_logTried = true;
        // The last session's log is kept beside it: after a crash or a reset of the card, the game is usually started
        // again before anyone reads it. Shared for reading: the live tests read it from inside the game while it is
        // written.
        MoveFileExW(CorePath(L"s2fm_mods.log").c_str(), CorePath(L"s2fm_mods.previous.log").c_str(),
                    MOVEFILE_REPLACE_EXISTING);
        g_log = _wfsopen(CorePath(L"s2fm_mods.log").c_str(), L"w", _SH_DENYWR);
    }
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    const size_t n = strlen(line);
    fprintf(g_log, "%02u:%02u:%02u.%03u %s%s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line,
            (n && line[n - 1] == '\n') ? "" : "\n");
    fflush(g_log);
}
