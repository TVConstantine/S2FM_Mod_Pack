// Crash reports. When the game crashes, a short text report and a small minidump are written to
// game\bin\win64\s2fm_mods_crash\ before the game's own crash handling runs. The report says where the crash was and
// whether one of our modules (dxgi.dll, s2fm_reshade_core.dll, s2fm_reshade.dll) was on the crashing thread's stack,
// so a crash in the mods can be told apart from one in the game.
//
// The game sets its own unhandled exception filter after we are loaded. SetUnhandledExceptionFilter is hooked so that
// ours stays first: the game's filter is kept and called after ours. Exceptions caught inside our own __try blocks
// never get here.
#include "core.h"
#include "../package/version.h"

#include <dbghelp.h>
#include <atomic>
#include <cstdio>
#include <string>

namespace {

typedef LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI* SetFilter_Fn)(LPTOP_LEVEL_EXCEPTION_FILTER);
SetFilter_Fn g_origSetFilter = nullptr;
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_next{nullptr};
std::atomic<bool> g_written{false};

const wchar_t* const kOurs[] = { L"dxgi.dll", L"s2fm_reshade_core.dll", L"s2fm_reshade.dll" };

// The module an address is in, its file name and the offset into it.
bool ModuleOf(DWORD64 address, wchar_t* name, DWORD size, DWORD64* offset, bool* ours) {
    HMODULE m = nullptr;
    *ours = false;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)address, &m) || !m) {
        wcscpy_s(name, size, L"?");
        *offset = address;
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* file = wcsrchr(path, L'\\');
    wcscpy_s(name, size, file ? file + 1 : path);
    *offset = address - (DWORD64)m;
    // Our dxgi.dll is the one beside the game; another module of that name (the system's) is not ours.
    for (const wchar_t* o : kOurs) {
        if (_wcsicmp(name, o) != 0) continue;
        if (_wcsicmp(o, L"dxgi.dll") == 0) *ours = _wcsnicmp(path, CoreDir().c_str(), CoreDir().size()) == 0;
        else *ours = true;
    }
    return true;
}

void Write(HANDLE f, const char* text) {
    DWORD n = 0;
    WriteFile(f, text, (DWORD)strlen(text), &n, nullptr);
}

// The last part of the log, as it is on disk.
void WriteLogTail(HANDLE out) {
    HANDLE f = CreateFileW(CorePath(L"s2fm_mods.log").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    const LONGLONG want = 6000;
    LARGE_INTEGER at;
    at.QuadPart = size.QuadPart > want ? size.QuadPart - want : 0;
    SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
    static char buf[6001];
    DWORD n = 0;
    ReadFile(f, buf, 6000, &n, nullptr);
    CloseHandle(f);
    buf[n] = 0;
    Write(out, "\r\nLast lines of s2fm_mods.log:\r\n");
    Write(out, buf);
}

void Report(EXCEPTION_POINTERS* ep) {
    const std::wstring dir = CorePath(L"s2fm_mods_crash");
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[64];
    swprintf_s(stamp, L"crash_%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::wstring base = dir + L"\\" + stamp;

    HANDLE f = CreateFileW((base + L".txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char line[512];
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        wchar_t mod[MAX_PATH];
        DWORD64 off = 0;
        bool ours = false;
        ModuleOf((DWORD64)er->ExceptionAddress, mod, MAX_PATH, &off, &ours);
        sprintf_s(line, "S2FM Mod Pack %s crash report\r\n\r\nException 0x%08X at %ls+0x%llX\r\n", S2FM_MODS_VERSION,
                  (unsigned)er->ExceptionCode, mod, (unsigned long long)off);
        Write(f, line);
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
            sprintf_s(line, "%s address 0x%llX\r\n", er->ExceptionInformation[0] == 1 ? "Writing" : "Reading",
                      (unsigned long long)er->ExceptionInformation[1]);
            Write(f, line);
        }
        // The crashing thread's stack, from its context.
        CONTEXT c = *ep->ContextRecord;
        bool anyOurs = ours;
        std::string frames;
        for (int i = 0; i < 40 && c.Rip; ++i) {
            wchar_t m[MAX_PATH];
            DWORD64 o = 0;
            bool our = false;
            ModuleOf(c.Rip, m, MAX_PATH, &o, &our);
            anyOurs = anyOurs || our;
            sprintf_s(line, "  %2d  %ls+0x%llX%s\r\n", i, m, (unsigned long long)o, our ? "   <- S2FM Mod Pack" : "");
            frames += line;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!fn) {
                // A leaf function: the return address is at the top of the stack.
                if (!c.Rsp || IsBadReadPtr((void*)c.Rsp, 8)) break;
                c.Rip = *(DWORD64*)c.Rsp;
                c.Rsp += 8;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &frame, nullptr);
        }
        Write(f, anyOurs ? "An S2FM Mod Pack module is on the crashing thread's stack.\r\n"
                         : "No S2FM Mod Pack module is on the crashing thread's stack.\r\n");
        Write(f, "\r\nStack:\r\n");
        Write(f, frames.c_str());
        DISPLAY_DEVICEA dd = { sizeof(dd) };
        for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
            if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
                sprintf_s(line, "\r\nGraphics card: %s\r\n", dd.DeviceString);
                Write(f, line);
                break;
            }
        }
        OSVERSIONINFOW os = { sizeof(os) };
        typedef LONG(WINAPI* GetVersion_Fn)(OSVERSIONINFOW*);
        if (auto getVersion = (GetVersion_Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) {
            getVersion(&os);
            sprintf_s(line, "Windows %lu.%lu build %lu\r\n", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
            Write(f, line);
        }
        WriteLogTail(f);
        CloseHandle(f);
    }

    // A small minidump: the threads, their stacks and the module list, no heap.
    if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
        typedef BOOL(WINAPI* Dump_Fn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                      PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
        if (auto dump = (Dump_Fn)GetProcAddress(dbghelp, "MiniDumpWriteDump")) {
            HANDLE d = CreateFileW((base + L".dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (d != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
                dump(GetCurrentProcess(), GetCurrentProcessId(), d,
                     (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), &mei,
                     nullptr, nullptr);
                CloseHandle(d);
            }
        }
    }
}

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    if (ep && !g_written.exchange(true)) {
        __try {
            Report(ep);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    LPTOP_LEVEL_EXCEPTION_FILTER next = g_next.load();
    return next ? next(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// The game's own filter is kept as the next one; ours stays installed.
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI H_SetFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    if (filter == &Filter) return g_origSetFilter(filter);
    return g_next.exchange(filter);
}

}  // namespace

void CoreCrashInstall() {
    static bool done = false;
    if (done) return;
    done = true;
    g_next.store(SetUnhandledExceptionFilter(&Filter));
    void* target = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetUnhandledExceptionFilter");
    if (target && CoreHook(target, (void*)&H_SetFilter, (void**)&g_origSetFilter))
        Log("[CORE] crash reports go to s2fm_mods_crash\\");
    else
        Log("[CORE] crash reports: the game's own filter could not be kept behind ours");
}

// For the tests: a crash report written as a crash would write it, without crashing.
extern "C" __declspec(dllexport) int __cdecl S2fmMods_TestCrashReport() {
    __try {
        RaiseException(0xE0532F4D, 0, 0, nullptr);
    } __except (Report(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {
    }
    return 1;
}
