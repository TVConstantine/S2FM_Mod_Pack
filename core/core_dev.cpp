// For development only: starts a Python layer for S2FM (s2fm_python\s2fm_python.dll beside this DLL, not shipped),
// which the live tests run their scripts in, when the file s2fm_mods_dev.txt beside the DLL says "python". Without
// that file, or without the layer installed, nothing here does anything; the packages ship neither.
//
// The layer has to start on Qt's thread. A thread waits for
// S2FM's main window, takes its window procedure for one message posted to it, and starts the layer from there,
// which is Qt's thread, then gives the window its procedure back.
#include "core.h"

#include <atomic>
#include <thread>

namespace {
const UINT kStartMessage = WM_APP + 0x5F2;
std::atomic<WNDPROC> g_oldProc{nullptr};
HWND g_window = nullptr;

void StartLayer() {
    const std::wstring dll = CorePath(L"s2fm_python\\s2fm_python.dll");
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("[DEV] %ls is not installed: no Python layer\n", dll.c_str());
        return;
    }
    HMODULE layer = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!layer) {
        Log("[DEV] could not load %ls (error %lu)\n", dll.c_str(), GetLastError());
        return;
    }
    typedef int (*StartFn)();
    StartFn start = (StartFn)GetProcAddress(layer, "S2fmPython_Start");
    const int ok = start ? start() : -1;
    Log("[DEV] the Python layer %s\n", ok == 1 ? "started" : ok == 0 ? "did not start" : "has no S2fmPython_Start");
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const WNDPROC old = g_oldProc.load();
    if (msg == kStartMessage) {
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)old);
        StartLayer();
        return 0;
    }
    return CallWindowProcW(old, hwnd, msg, wp, lp);
}

BOOL CALLBACK FindMain(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
    wchar_t title[512] = {};
    InternalGetWindowText(hwnd, title, 512);
    if (wcsstr(title, L"Source Filmmaker")) {
        g_window = hwnd;
        return FALSE;
    }
    return TRUE;
}

void Wait() {
    for (int i = 0; i < 600 && !g_window; ++i) {
        EnumWindows(FindMain, 0);
        if (!g_window) Sleep(500);
    }
    if (!g_window) {
        Log("[DEV] S2FM's main window did not come: no Python layer\n");
        return;
    }
    // A moment for S2FM to finish making its menus.
    Sleep(3000);
    g_oldProc.store((WNDPROC)GetWindowLongPtrW(g_window, GWLP_WNDPROC));
    SetWindowLongPtrW(g_window, GWLP_WNDPROC, (LONG_PTR)&Proc);
    PostMessageW(g_window, kStartMessage, 0, 0);
}
}  // namespace

void CoreDevStart() {
    static std::atomic<bool> once{false};
    if (once.exchange(true)) return;
    FILE* f = nullptr;
    char words[256] = {};
    if (_wfopen_s(&f, CorePath(L"s2fm_mods_dev.txt").c_str(), L"r") != 0 || !f) return;
    fread(words, 1, sizeof(words) - 1, f);
    fclose(f);
    if (!strstr(words, "python")) return;
    Log("[DEV] s2fm_mods_dev.txt asks for the Python layer: it starts once S2FM's main window is up\n");
    std::thread(Wait).detach();
}
