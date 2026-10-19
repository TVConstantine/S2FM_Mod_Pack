#include "rs_log.h"

#include <windows.h>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <mutex>

namespace {
std::mutex g_logMutex;
FILE* g_log = nullptr;
}

void RsToolLog(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    // _fsopen, not fopen_s: fopen_s shares the file with no one, so it could not be read while the
    // tools run.
    if (!g_log) {
        // Beside this DLL (the game's bin\\win64\\tools).
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&RsToolLog, &self);
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(self, path, MAX_PATH);
        if (wchar_t* slash = wcsrchr(path, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"s2fm_reshade_tool.log");
        g_log = _wfsopen(path, L"w", _SH_DENYWR);
    }
    if (!g_log) return;
    fprintf(g_log, "[%10llu ms, thread %5lu] ", (unsigned long long)GetTickCount64(), GetCurrentThreadId());
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}
